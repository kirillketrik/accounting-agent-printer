#include "printer_enumerator.h"
#include "common/string_utils.h"
#include <cstdlib>

namespace {

// Virtual/pseudo printers that must never be queried (see brief). Matched
// case-insensitively as substrings against Name/DriverName, or as exact/prefix
// matches against PortName.
bool NameOrDriverIndicatesVirtual(const std::string& name, const std::string& driverName) {
    static const char* kNameSignatures[] = {
        "OneNote",
        "Microsoft XPS Document Writer",
        "Microsoft Print to PDF",
        "Microsoft Shared Fax Driver",
    };
    for (const char* sig : kNameSignatures) {
        if (strutil::IContains(name, sig) || strutil::IContains(driverName, sig)) return true;
    }
    return false;
}

bool PortIndicatesVirtual(const std::string& portName) {
    if (strutil::IStartsWith(portName, "Microsoft.Office.OneNote")) return true;
    if (strutil::IEquals(portName, "SHRFAX:")) return true;
    if (strutil::IEquals(portName, "nul:")) return true;
    if (strutil::IEquals(portName, "PORTPROMPT:")) return true;
    return false;
}

} // namespace

bool IsVirtualPrinter(const PrinterInfo& p) {
    return NameOrDriverIndicatesVirtual(p.name, p.driverName) || PortIndicatesVirtual(p.portName);
}

PortType ClassifyPortName(const std::string& portName) {
    if (strutil::IStartsWith(portName, "WSD-")) return PortType::Wsd;
    if (strutil::IStartsWith(portName, "USB")) return PortType::LocalUsb;
    if (strutil::IStartsWith(portName, "COM")) return PortType::LocalUsb;
    if (strutil::IStartsWith(portName, "LPT")) return PortType::LocalUsb;
    // "IPP_<device>_<n>" is Windows' IPP-over-USB class driver port (common on
    // Windows 10+ for USB printers plugged in without a vendor driver). Must
    // be classified here, before falling through to the Win32_TCPIPPrinterPort
    // fallback below - that table does have a row for these ports, but its
    // HostAddress is a synthetic device nickname (e.g. "Pantum-4A6A29"), not a
    // real network address, so treating it as TCP/IP sends SNMP nowhere and
    // silently drops the page count instead of using the (local) counter
    // sources that would actually work.
    if (strutil::IStartsWith(portName, "IPP_")) return PortType::LocalUsb;
    return PortType::Other; // may be upgraded to TcpIp by a Win32_TCPIPPrinterPort lookup
}

ParsedWsdLocation ParseWsdLocation(const std::string& location) {
    ParsedWsdLocation result;

    std::string schemeMarker = "://";
    size_t schemePos = location.find(schemeMarker);
    if (schemePos == std::string::npos) return result;
    size_t hostStart = schemePos + schemeMarker.size();
    if (hostStart >= location.size()) return result;

    if (location[hostStart] == '[') {
        size_t closeBracket = location.find(']', hostStart);
        if (closeBracket == std::string::npos) return result;
        std::string inside = location.substr(hostStart + 1, closeBracket - hostStart - 1);

        size_t pctPos = inside.find('%');
        std::string addr = (pctPos == std::string::npos) ? inside : inside.substr(0, pctPos);
        // Windows emits the zone id raw (e.g. "fe80::1%18", confirmed per the
        // brief), not RFC 6874 percent-encoded ("fe80::1%2518"). Deliberately
        // NOT stripping a "25" prefix here: doing so would corrupt a
        // legitimate zone id that happens to start with "25" (interface 25,
        // 251, ...), and Windows doesn't produce the escaped form in the
        // first place.
        std::string scope = (pctPos == std::string::npos) ? "" : inside.substr(pctPos + 1);

        result.ok = true;
        result.isIPv6 = true;
        result.host = addr;
        result.scopeId = scope.empty() ? 0 : (uint32_t)strtoul(scope.c_str(), nullptr, 10);
        return result;
    }

    // IPv4 literal or hostname: read until ':' (port) or '/' (path) or end.
    size_t hostEnd = location.find_first_of(":/", hostStart);
    std::string host = (hostEnd == std::string::npos)
        ? location.substr(hostStart)
        : location.substr(hostStart, hostEnd - hostStart);
    if (host.empty()) return result;

    result.ok = true;
    result.isIPv6 = false;
    result.host = host;
    return result;
}

std::vector<PrinterInfo> EnumeratePrinters(WmiSession& wmi, Logger& logger) {
    std::vector<PrinterInfo> printers;

    bool ok = wmi.Query(
        L"SELECT Name, DriverName, PortName, Shared, ShareName, Network, WorkOffline, "
        L"PrinterStatus, Location FROM Win32_Printer",
        [&](IWbemClassObject* obj) -> bool {
            PrinterInfo p;
            p.name = strutil::WideToUtf8(WmiGetString(obj, L"Name"));
            p.driverName = strutil::WideToUtf8(WmiGetString(obj, L"DriverName"));
            p.portName = strutil::WideToUtf8(WmiGetString(obj, L"PortName"));
            p.shared = WmiGetBool(obj, L"Shared");
            p.shareName = strutil::WideToUtf8(WmiGetString(obj, L"ShareName"));
            p.network = WmiGetBool(obj, L"Network");
            p.workOffline = WmiGetBool(obj, L"WorkOffline");
            p.printerStatus = WmiGetUInt32(obj, L"PrinterStatus");
            p.location = strutil::WideToUtf8(WmiGetString(obj, L"Location"));

            if (IsVirtualPrinter(p)) {
                logger.Debug("Skipping virtual printer: " + p.name + " (port=" + p.portName + ")");
                return true; // keep enumerating
            }

            p.portType = ClassifyPortName(p.portName);

            if (p.portType == PortType::Wsd) {
                ParsedWsdLocation parsed = ParseWsdLocation(p.location);
                if (parsed.ok) {
                    p.hostResolved = true;
                    p.resolvedHost = parsed.host;
                    p.isIPv6 = parsed.isIPv6;
                    p.ipv6ScopeId = parsed.scopeId;
                } else {
                    p.note = "could not determine IP: WSD port with unparsable Location '" + p.location + "'";
                    logger.Warn("Printer '" + p.name + "': " + p.note);
                }
            } else if (p.portType == PortType::Other) {
                // Might still be a TCP/IP port under a name that didn't match our
                // simple heuristics; the authoritative signal is whether
                // Win32_TCPIPPrinterPort has a row for this port name.
                std::wstring wPort = strutil::Utf8ToWide(p.portName);
                std::wstring wql = L"SELECT HostAddress FROM Win32_TCPIPPrinterPort WHERE Name='" +
                    EscapeWqlLiteral(wPort) + L"'";
                wmi.Query(wql, [&](IWbemClassObject* portObj) -> bool {
                    std::wstring hostAddr = WmiGetString(portObj, L"HostAddress");
                    if (!hostAddr.empty()) {
                        p.portType = PortType::TcpIp;
                        p.hostResolved = true;
                        p.resolvedHost = strutil::WideToUtf8(hostAddr);
                        p.isIPv6 = false; // Win32_TCPIPPrinterPort is IPv4-oriented in practice
                    }
                    return false; // one row expected
                });

                if (p.portType == PortType::Other) {
                    logger.Debug("Printer '" + p.name + "' port '" + p.portName +
                        "' did not match TCP/IP, WSD, or Local/USB patterns; reporting as Other.");
                }
            }
            // LocalUsb: nothing further to resolve here; registry_reader handles it.

            printers.push_back(std::move(p));
            return true;
        });

    if (!ok) {
        logger.Error("Failed to enumerate Win32_Printer via WMI; no printers collected this cycle.");
    } else {
        logger.Info("Enumerated " + std::to_string(printers.size()) + " printer(s) after filtering virtual printers.");
    }

    return printers;
}
