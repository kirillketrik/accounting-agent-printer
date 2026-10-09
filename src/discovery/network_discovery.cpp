// winsock2.h must precede any transitive <windows.h> include - see
// network_info.cpp for the full note.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "network_discovery.h"
#include "common/string_utils.h"
#include "snmp/snmp_client.h"

namespace {

// Win32_Printer.PrinterStatus "Unknown" - a discovered printer has no
// spooler queue to report a real value for.
constexpr uint32_t kWmiPrinterStatusUnknown = 2;

// Explicit [discovery] subnets narrower than this are refused outright,
// before max_hosts even comes into it - a /8 typo shouldn't turn into a
// 4096-address partial sweep of some arbitrary corner of it.
constexpr int kMinExplicitPrefixLength = 16;

bool ParseIPv4(const std::string& s, uint32_t& outHostOrder) {
    in_addr a{};
    if (InetPtonA(AF_INET, s.c_str(), &a) != 1) return false;
    outHostOrder = ntohl(a.s_addr);
    return true;
}

uint32_t PrefixMask(int prefix) {
    if (prefix <= 0) return 0;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return ~((1u << (32 - prefix)) - 1);
}

// Adds the usable host addresses of ip/prefix to `out` (network and
// broadcast addresses excluded, except for /31 and /32 which have none),
// stopping once `out` holds `maxHosts`.
void AddRange(uint32_t ip, int prefix, size_t maxHosts, std::set<uint32_t>& out, bool& truncated) {
    uint32_t mask = PrefixMask(prefix);
    uint32_t net = ip & mask;
    uint32_t last = net | ~mask;
    uint64_t first = net, end = last;
    if (prefix <= 30) {
        first = (uint64_t)net + 1;
        end = (uint64_t)last - 1;
    }
    for (uint64_t h = first; h <= end; h++) {
        if (out.size() >= maxHosts) {
            truncated = true;
            return;
        }
        out.insert((uint32_t)h);
    }
}

// "a.b.c.d/n", or a bare "a.b.c.d" for a single address.
bool ParseCidr(const std::string& s, uint32_t& outIp, int& outPrefix) {
    size_t slash = s.find('/');
    if (!ParseIPv4(strutil::Trim(s.substr(0, slash)), outIp)) return false;
    if (slash == std::string::npos) {
        outPrefix = 32;
        return true;
    }
    try {
        size_t used = 0;
        std::string prefixStr = strutil::Trim(s.substr(slash + 1));
        outPrefix = std::stoi(prefixStr, &used);
        return used == prefixStr.size() && outPrefix >= 0 && outPrefix <= 32;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find_first_of(",;", start);
        if (end == std::string::npos) end = s.size();
        std::string part = strutil::Trim(s.substr(start, end - start));
        if (!part.empty()) parts.push_back(part);
        start = end + 1;
    }
    return parts;
}

// SNMP strings are raw octets in whatever encoding the device was set up
// with (a CP1251 sysLocation is typical here). One invalid UTF-8 sequence in
// the JSON would get the whole report - installed printers included -
// rejected by the server, so anything that isn't valid UTF-8 is reduced to
// ASCII, and control characters are dropped either way.
std::string SanitizeSnmpText(const std::string& raw) {
    bool validUtf8 = true;
    for (size_t i = 0; i < raw.size() && validUtf8;) {
        unsigned char c = (unsigned char)raw[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > raw.size()) {
            validUtf8 = false;
            break;
        }
        for (size_t k = 1; k < len; k++) {
            if (((unsigned char)raw[i + k] >> 6) != 0x2) validUtf8 = false;
        }
        i += len;
    }

    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        unsigned char c = (unsigned char)ch;
        if (c == '\r' || c == '\n' || c == '\t') {
            out += ' ';
        } else if (c < 0x20 || c == 0x7F) {
            continue;
        } else if (c >= 0x80 && !validUtf8) {
            out += '?';
        } else {
            out += ch;
        }
    }
    return strutil::Trim(out);
}

std::string Truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    // Back off to a UTF-8 lead byte so a multi-byte character isn't split.
    size_t cut = maxLen;
    while (cut > 0 && ((unsigned char)s[cut] >> 6) == 0x2) cut--;
    return s.substr(0, cut);
}

// Same shape Windows gives a queue it creates itself, e.g.
// "NPI939951 (HP LaserJet CP1525nw)" - the model carries the words the
// server's asset suggestions match on, sysName tells two of them apart.
std::string DiscoveredName(const std::string& model, const std::string& sysName, const std::string& ip) {
    if (!model.empty() && !sysName.empty() && !strutil::IEquals(model, sysName)) {
        return Truncate(sysName + " (" + model + ")", 255);
    }
    if (!model.empty()) return Truncate(model, 255);
    if (!sysName.empty()) return Truncate(sysName, 255);
    return "Printer " + ip;
}

PrinterInfo BuildPrinterInfo(const SnmpDiscoveredPrinter& d, const PrinterInfo* previous) {
    PrinterInfo p;
    p.portType = PortType::Network;
    p.portName = d.ip;
    p.network = true;
    p.printerStatus = kWmiPrinterStatusUnknown;
    p.hostResolved = true;
    p.resolvedHost = d.ip;
    p.isIPv6 = false;
    p.snmpAttempted = true;
    p.snmpReachable = true;

    if (d.detailsAnswered || previous == nullptr) {
        std::string sysDescr = SanitizeSnmpText(d.sysDescr);
        // Fall back to sysDescr for the model when hrDeviceDescr.1 isn't
        // there; it is at least the device's own description of itself.
        std::string model = SanitizeSnmpText(d.model);
        if (model.empty()) model = Truncate(sysDescr, 128);
        p.name = DiscoveredName(model, SanitizeSnmpText(d.sysName), d.ip);
        p.location = Truncate(SanitizeSnmpText(d.sysLocation), 255);
        p.snmpSysDescr = sysDescr;
    } else {
        // The printer answered the sweep but its detail GETs were lost -
        // keep last cycle's identity rather than renaming it to its IP.
        p.name = previous->name;
        p.location = previous->location;
        p.snmpSysDescr = previous->snmpSysDescr;
    }

    if (d.hasPageCount) {
        p.pageCountSource = PageCountSource::Snmp;
        p.pageCount = d.pageCount;
    } else {
        p.note = "found by network discovery; Printer-MIB present but no prtMarkerLifeCount value";
    }
    return p;
}

} // namespace

DiscoveryResult NetworkDiscovery::Run(const PrinterConfig& config, const std::vector<HostAddress>& hostIps,
                                      const std::set<std::string>& skipHosts, Logger& logger, HANDLE stopEvent) {
    DiscoveryResult result;
    size_t maxHosts = (size_t)config.discoveryMaxHosts;
    std::set<uint32_t> targets;
    std::set<uint32_t> ownIps;
    bool truncated = false;

    if (config.discoveryScanLocalSubnets) {
        for (const auto& ha : hostIps) {
            uint32_t ip = 0;
            if (ha.isIPv6 || !ParseIPv4(ha.ip, ip)) continue; // an IPv6 subnet is far too big to sweep
            ownIps.insert(ip);
            if ((ip >> 16) == 0xA9FE) continue; // 169.254/16 link-local: no DHCP, nothing to find
            int prefix = ha.prefixLength;
            if (prefix < 1 || prefix > 32) continue;
            if (prefix < config.discoveryMinPrefixLength) {
                logger.Info("Network discovery: adapter '" + ha.adapterName + "' is on a /" + std::to_string(prefix) +
                    " - sweeping only the /" + std::to_string(config.discoveryMinPrefixLength) + " around " + ha.ip +
                    " (discovery.min_prefix_length). List other ranges in discovery.subnets.");
                prefix = config.discoveryMinPrefixLength;
            }
            AddRange(ip, prefix, maxHosts, targets, truncated);
        }
    }

    for (const auto& cidr : SplitList(config.discoverySubnets)) {
        uint32_t ip = 0;
        int prefix = 0;
        if (!ParseCidr(cidr, ip, prefix)) {
            logger.Warn("Network discovery: ignoring invalid discovery.subnets entry '" + cidr +
                "' (expected e.g. 192.168.1.0/24).");
            continue;
        }
        if (prefix < kMinExplicitPrefixLength) {
            logger.Warn("Network discovery: ignoring discovery.subnets entry '" + cidr + "' - wider than /" +
                std::to_string(kMinExplicitPrefixLength) + "; split it into smaller ranges.");
            continue;
        }
        AddRange(ip, prefix, maxHosts, targets, truncated);
    }

    // Printers found before are asked again even if the ranges no longer
    // cover them (adapter moved, config changed), so they go offline in the
    // usual way rather than silently freezing on their last reading.
    for (const auto& entry : known_) {
        uint32_t ip = 0;
        if (ParseIPv4(entry.first, ip)) targets.insert(ip);
    }
    for (uint32_t ip : ownIps) targets.erase(ip);

    if (truncated) {
        logger.Warn("Network discovery: address list capped at discovery.max_hosts=" + std::to_string(maxHosts) +
            "; the rest of the configured ranges is not swept.");
    }
    if (targets.empty()) {
        logger.Debug("Network discovery: no IPv4 ranges to sweep.");
        return result;
    }

    std::vector<uint32_t> targetsNetOrder;
    targetsNetOrder.reserve(targets.size());
    for (uint32_t ip : targets) targetsNetOrder.push_back(htonl(ip));

    SnmpOptions options;
    options.community = config.snmpCommunity;
    options.timeoutMs = config.discoveryTimeoutMs;
    options.retries = config.discoveryRetries;
    options.stopEvent = stopEvent;

    logger.Info("Network discovery: sweeping " + std::to_string(targetsNetOrder.size()) + " IPv4 address(es) via SNMP.");
    SnmpSweepResult sweep = SnmpSweepForPrinters(targetsNetOrder, config.snmpPort, options, logger);

    if (!sweep.error.empty()) {
        // Nothing was asked, so nothing is known to be down - keep the
        // remembered printers alive without a fresh (false) reading.
        result.notProbed = Known();
        return result;
    }

    std::set<std::string> answered;
    for (const auto& d : sweep.printers) {
        if (skipHosts.count(d.ip)) {
            known_.erase(d.ip); // installed on this PC now; that queue reports it
            continue;
        }
        auto prev = known_.find(d.ip);
        PrinterInfo p = BuildPrinterInfo(d, prev != known_.end() ? &prev->second : nullptr);
        known_[d.ip] = p;
        answered.insert(d.ip);
        result.printers.push_back(p);
    }

    for (auto it = known_.begin(); it != known_.end();) {
        if (answered.count(it->first)) {
            ++it;
            continue;
        }
        if (skipHosts.count(it->first)) {
            it = known_.erase(it);
            continue;
        }
        if (sweep.interrupted) {
            result.notProbed.emplace_back(it->first, it->second.name);
            ++it;
            continue;
        }
        PrinterInfo p = it->second;
        p.snmpReachable = false;
        p.pageCountSource = PageCountSource::None;
        p.pageCount = -1;
        p.note = "did not answer the network discovery sweep this cycle";
        result.printers.push_back(p);
        ++it;
    }

    result.interrupted = sweep.interrupted;
    logger.Info("Network discovery: " + std::to_string(answered.size()) + " printer(s) not installed on this PC " +
        "answered; " + std::to_string(result.printers.size() - answered.size()) + " previously found did not" +
        (sweep.interrupted ? " (sweep interrupted by shutdown)." : "."));
    return result;
}

std::vector<std::pair<std::string, std::string>> NetworkDiscovery::Known() const {
    std::vector<std::pair<std::string, std::string>> known;
    for (const auto& entry : known_) known.emplace_back(entry.first, entry.second.name);
    return known;
}
