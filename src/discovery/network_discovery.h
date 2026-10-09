#pragma once
#include <windows.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "printer_config.h"
#include "common/logger.h"
#include "model/printer_info.h"
#include "network/network_info.h"

struct DiscoveryResult {
    // Printers found on the network (or remembered from an earlier cycle),
    // ready to report alongside the installed ones: portType Network,
    // resolvedHost = their IPv4, snmpReachable = answered this cycle.
    std::vector<PrinterInfo> printers;
    // Remembered printers this cycle never got to because the sweep was
    // interrupted - not in `printers` (there is no fresh reading to send),
    // but the caller must still mark them seen so a service stop doesn't
    // report them as removed. Pairs of (dedupKey, name).
    std::vector<std::pair<std::string, std::string>> notProbed;
    bool interrupted = false;
};

// Finds printers on the local network that this PC has no print queue for:
// an SNMP sweep of this host's own IPv4 subnets plus any extra ranges from
// [discovery] subnets, keeping the devices that implement the Printer-MIB.
//
// Holds state across cycles (owned by the worker loop, like
// LivenessTracker): a printer found once keeps being reported, with
// snmpReachable=false, on cycles where it doesn't answer, so the tracker
// turns it offline after the usual consecutive failures instead of the
// agent reporting it removed every time it is switched off overnight. The
// memory resets on a service restart; the server keeps the last row.
class NetworkDiscovery {
public:
    // `skipHosts` are addresses already reported by an installed queue on
    // this PC - those rows carry more (driver, port, share) than a sweep
    // can, so the discovered duplicate is dropped. Never throws.
    DiscoveryResult Run(const PrinterConfig& config, const std::vector<HostAddress>& hostIps,
                        const std::set<std::string>& skipHosts, Logger& logger, HANDLE stopEvent);

    // Every remembered printer as (dedupKey, name), for a cycle that stops
    // before Run() - the caller marks them seen so they aren't reported removed.
    std::vector<std::pair<std::string, std::string>> Known() const;

private:
    std::map<std::string, PrinterInfo> known_; // by IPv4 string
};
