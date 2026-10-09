#include "data_collector.h"
#include "common/json_writer.h"
#include "wmi/wmi_client.h"
#include "collector/printer_enumerator.h"
#include "snmp/snmp_client.h"
#include "registry/registry_reader.h"
#include "network/network_info.h"
#include "pjl/pjl_client.h"
#include <windows.h>
#include <cstdio>
#include <set>

namespace {
// PRINTER_STATUS_OFFLINE from wingdi/winspool.h - not pulled in directly here
// since printer_enumerator.cpp already captured this as a plain uint32_t.
constexpr uint32_t kWmiPrinterStatusOffline = 7;

// Identity used both for cross-cycle liveness tracking here and for the
// accounting backend's own Printer dedup key (PrinterRepository.
// find_by_dedup_key) - keeping these in agreement means the agent's
// "removed" reports and the server's stored rows refer to the same printer.
std::string DedupKey(const PrinterInfo& p) {
    return !p.resolvedHost.empty() ? p.resolvedHost : p.name;
}
} // namespace

namespace {

std::string CurrentIso8601Utc() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// The print spooler exposes its own per-queue "Total Pages Printed" counter
// via WMI, populated by Windows itself (not the vendor driver) for every
// queue regardless of connection type - the one page-count source that
// actually works uniformly for USB/local printers, unlike the vendor-optional
// registry data below. Returns false if the perf-counter class isn't
// available (seen on some localized Windows builds) or has no row for this
// queue name; callers should fall back to the registry heuristic in that
// case, not treat it as fatal.
//
// Caveat (not this function's concern - a snapshot-diffing concern for
// whoever aggregates history later): this counter resets to 0 whenever the
// Print Spooler service restarts, so it's "pages since spooler last
// started," not a lifetime counter like the SNMP/registry sources.
bool TryGetSpoolerPageCount(WmiSession& wmi, const std::string& printerName, uint32_t& outPages, Logger& logger) {
    std::wstring wql = L"SELECT TotalPagesPrinted FROM Win32_PerfRawData_Spooler_PrintQueue WHERE Name='" +
        EscapeWqlLiteral(strutil::Utf8ToWide(printerName)) + L"'";

    bool found = false;
    uint32_t pages = 0;
    bool queried = wmi.Query(wql, [&](IWbemClassObject* obj) -> bool {
        pages = WmiGetUInt32(obj, L"TotalPagesPrinted", 0);
        found = true;
        return false; // one matching queue is all we need
    });

    if (!queried) {
        logger.Debug("WMI: Win32_PerfRawData_Spooler_PrintQueue query failed for '" + printerName +
            "' (class unavailable on this system?); falling back to registry.");
        return false;
    }
    if (!found) {
        logger.Debug("WMI: no Print Queue perf counter instance for '" + printerName + "'.");
        return false;
    }

    outPages = pages;
    return true;
}

void CollectPageCountForPrinter(WmiSession& wmi, PrinterInfo& p, const PrinterConfig& config, Logger& logger, HANDLE stopEvent) {
    if (p.portType == PortType::TcpIp || p.portType == PortType::Wsd) {
        if (!p.hostResolved || p.resolvedHost.empty()) {
            p.note = p.note.empty() ? "could not determine IP" : p.note;
            return;
        }

        SnmpTarget target;
        target.host = p.resolvedHost;
        target.isIPv6 = p.isIPv6;
        target.scopeId = p.ipv6ScopeId;
        target.port = config.snmpPort;

        SnmpOptions options;
        options.community = config.snmpCommunity;
        options.timeoutMs = config.snmpTimeoutMs;
        options.retries = config.snmpRetries;
        options.retryDelayMs = config.snmpRetryDelayMs;
        options.stopEvent = stopEvent;

        p.snmpAttempted = true;
        SnmpGetResult snmpResult = SnmpGetSysDescrAndPageCount(target, options, logger, p.name);

        if (!snmpResult.hostResolved) {
            p.note = snmpResult.error;
            return;
        }

        p.snmpReachable = snmpResult.success;
        p.snmpSysDescr = snmpResult.sysDescr;

        if (snmpResult.hasPageCount) {
            p.pageCountSource = PageCountSource::Snmp;
            p.pageCount = snmpResult.pageCount;
        } else if (!snmpResult.error.empty()) {
            p.note = snmpResult.error;
        }
        return;
    }

    if (p.portType == PortType::LocalUsb) {
        if (config.pjlEnabled) {
            int64_t pjlPages = 0;
            if (TryGetPjlPageCount(p.name, config.pjlTimeoutMs, pjlPages, logger)) {
                p.pageCountSource = PageCountSource::Pjl;
                p.pageCount = pjlPages;
                return;
            }
        }

        uint32_t spoolerPages = 0;
        if (TryGetSpoolerPageCount(wmi, p.name, spoolerPages, logger)) {
            p.pageCountSource = PageCountSource::SpoolerCounter;
            p.pageCount = (int64_t)spoolerPages;
            return;
        }

        RegistryCounterResult reg = ReadPrinterDriverDataCounters(p.name, logger);
        if (!reg.counters.empty()) {
            // Vendor-specific and unstructured (see brief) - take the first
            // matching value; all matches were already logged at DEBUG for
            // manual review.
            p.pageCountSource = PageCountSource::RegistryDriverData;
            p.pageCount = reg.counters.front().value;
            p.note = "registry value used: " + reg.counters.front().name;
        } else if (reg.keyFound) {
            p.note = "PrinterDriverData present but no counter-like value found";
        } else {
            p.note = "PrinterDriverData registry key not found (local/USB printer with no vendor counter data)";
        }
        return;
    }

    // PortType::Other - nothing we know how to query. (PortType::Network
    // never gets here: discovery already read its counter during the sweep.)
    if (p.note.empty()) p.note = "unclassified port type; no counter source attempted";
}

// This cycle's raw reachability signal, before the LivenessTracker's
// consecutive-failure threshold is applied. Only meaningful for port types
// that actually have a probe (TcpIp/Wsd via SNMP, Network via the discovery
// sweep's SNMP, LocalUsb via WMI's own PrinterStatus/WorkOffline flags,
// already collected in printer_enumerator.cpp but previously only ever passed
// through to JSON, never acted on).
// PortType::Other has no probe at all - callers must use LivenessTracker::MarkSeen
// for those instead of this function, so they stay Unknown rather than
// silently drifting to Offline with no real signal behind it.
bool EvaluateReachableThisCycle(const PrinterInfo& p) {
    if (p.portType == PortType::TcpIp || p.portType == PortType::Wsd || p.portType == PortType::Network) {
        return p.snmpReachable;
    }
    if (p.portType == PortType::LocalUsb) return p.printerStatus != kWmiPrinterStatusOffline && !p.workOffline;
    return false;
}

void WritePrinterJson(JsonWriter& w, const PrinterInfo& p, const std::string& collectedAtUtc) {
    w.BeginObjectElement();
    w.Field("name", p.name);
    // Repeated per record: a profile may read the measurement time relative to the record.
    w.Field("collectedAtUtc", collectedAtUtc);
    w.Field("driverName", p.driverName);
    w.Field("portName", p.portName);
    w.Field("portType", PortTypeName(p.portType));
    w.Field("location", p.location);
    w.Field("shared", p.shared);
    w.Field("shareName", p.shareName);
    w.Field("network", p.network);
    w.Field("workOffline", p.workOffline);
    w.Field("printerStatus", p.printerStatus);

    if (p.hostResolved) {
        w.Field("resolvedHost", p.resolvedHost);
        w.Field("isIPv6", p.isIPv6);
    } else {
        w.FieldNull("resolvedHost");
    }

    if (p.pageCountSource != PageCountSource::None) {
        w.Field("pageCount", p.pageCount);
    } else {
        w.FieldNull("pageCount");
    }
    w.Field("pageCountSource", PageCountSourceName(p.pageCountSource));

    w.Field("snmpAttempted", p.snmpAttempted);
    w.Field("snmpReachable", p.snmpReachable);
    if (!p.snmpSysDescr.empty()) w.Field("snmpSysDescr", p.snmpSysDescr);

    w.Field("status", PrinterStatusName(p.status));
    w.Field("consecutiveFailures", p.consecutiveFailures);

    if (!p.note.empty()) w.Field("note", p.note);

    w.EndObject();
}

} // namespace

std::string CollectPrinterReport(const PrinterConfig& config, Logger& logger, LivenessTracker& tracker,
                                 NetworkDiscovery& discovery, HANDLE stopEvent) {

    try {
        WmiSession wmi(logger);
        if (!wmi.Connect()) {
            logger.Error("Collection cycle aborted: could not connect to WMI (ROOT\\CIMV2).");
            return std::string();
        }

        std::vector<PrinterInfo> printers = EnumeratePrinters(wmi, logger);
        std::vector<HostAddress> hostIps = GetHostIpAddresses(logger);
        std::string hostName = GetLocalComputerName();

        bool stopped = false;
        for (PrinterInfo& p : printers) {
            if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                logger.Info("Collection cycle interrupted by shutdown request; sending partial report " +
                    std::string("(remaining printers keep their pre-collection defaults)."));
                stopped = true;
                break;
            }
            try {
                CollectPageCountForPrinter(wmi, p, config, logger, stopEvent);
            } catch (const std::exception& ex) {
                logger.Warn("Error collecting data for printer '" + p.name + "': " + ex.what());
                p.note = std::string("collection error: ") + ex.what();
            } catch (...) {
                logger.Warn("Unknown error collecting data for printer '" + p.name + "'");
                p.note = "unknown collection error";
            }

            std::string dedupKey = DedupKey(p);
            if (p.portType == PortType::Other) {
                tracker.MarkSeen(dedupKey, p.name);
            } else {
                bool reachable = EvaluateReachableThisCycle(p);
                tracker.RecordThisCycle(dedupKey, p.name, reachable, config.offlineAfterConsecutiveFailures,
                                         p.status, p.consecutiveFailures);
            }

            logger.Info("Page count for '" + p.name + "': source=" + PageCountSourceName(p.pageCountSource) +
                (p.pageCountSource != PageCountSource::None ? (" value=" + std::to_string(p.pageCount)) : std::string(" (no source succeeded)")) +
                " status=" + PrinterStatusName(p.status));
        }

        if (config.discoveryEnabled) {
            if (stopped) {
                for (const auto& known : discovery.Known()) tracker.MarkSeen(known.first, known.second);
            } else {
                try {
                    std::set<std::string> installedHosts;
                    for (const auto& p : printers) {
                        if (p.hostResolved && !p.resolvedHost.empty()) installedHosts.insert(p.resolvedHost);
                    }

                    DiscoveryResult found = discovery.Run(config, hostIps, installedHosts, logger, stopEvent);
                    for (PrinterInfo& p : found.printers) {
                        tracker.RecordThisCycle(DedupKey(p), p.name, EvaluateReachableThisCycle(p),
                                                 config.offlineAfterConsecutiveFailures, p.status, p.consecutiveFailures);
                        logger.Debug("Discovered printer '" + p.name + "' at " + p.resolvedHost + ": source=" +
                            PageCountSourceName(p.pageCountSource) +
                            (p.pageCountSource != PageCountSource::None ? (" value=" + std::to_string(p.pageCount)) : std::string()) +
                            " status=" + PrinterStatusName(p.status));
                        printers.push_back(std::move(p));
                    }
                    for (const auto& skipped : found.notProbed) tracker.MarkSeen(skipped.first, skipped.second);
                } catch (const std::exception& ex) {
                    logger.Warn(std::string("Network discovery failed: ") + ex.what());
                    for (const auto& known : discovery.Known()) tracker.MarkSeen(known.first, known.second);
                } catch (...) {
                    logger.Warn("Network discovery failed with an unknown error.");
                    for (const auto& known : discovery.Known()) tracker.MarkSeen(known.first, known.second);
                }
            }
        }

        std::vector<RemovedPrinter> removedPrinters = tracker.DrainRemoved();
        for (const auto& rp : removedPrinters) {
            logger.Info("Printer removed: '" + rp.name + "' (dedupKey=" + rp.dedupKey +
                ") no longer enumerated by Windows.");
        }

        const std::string collectedAt = CurrentIso8601Utc();
        JsonWriter w;
        w.BeginObject();
        WriteHostIdentity(w, hostName, hostIps);
        w.Field("collectedAtUtc", collectedAt);

        w.BeginArray("printers");
        for (const auto& p : printers) {
            WritePrinterJson(w, p, collectedAt);
        }
        w.EndArray();

        w.BeginArray("removedPrinters");
        for (const auto& rp : removedPrinters) {
            w.BeginObjectElement();
            w.Field("dedupKey", rp.dedupKey);
            w.Field("name", rp.name);
            w.EndObject();
        }
        w.EndArray();

        w.EndObject();

        std::string payload = w.Str();
        logger.Debug("Payload size: " + std::to_string(payload.size()) + " bytes, " +
            std::to_string(printers.size()) + " printer(s).");

        logger.Info("Collected " + std::to_string(printers.size()) + " printer(s).");
        return payload;
    } catch (const std::exception& ex) {
        logger.Error(std::string("Collection cycle failed with an unexpected exception: ") + ex.what());
    } catch (...) {
        logger.Error("Collection cycle failed with an unknown unexpected error.");
    }
    return std::string();
}
