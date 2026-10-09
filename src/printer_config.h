#pragma once
#include <string>
#include <cstdint>
#include "common/config.h"

// Settings that only the printer agent has: its own sections of agent.ini
// ([snmp], [pjl], [discovery]) plus the offline threshold from [polling].
// Every field has a default, so a missing section just means defaults.
struct PrinterConfig {
    // [polling]
    // A printer only flips to "offline" after this many consecutive bad
    // cycles in a row (SNMP unreachable, or WMI PrinterStatus==Offline for
    // local/USB) - a single blip doesn't count.
    int offlineAfterConsecutiveFailures = 3;

    // [snmp]
    std::string snmpCommunity = "public";
    int snmpTimeoutMs = 2000;            // per-attempt timeout
    int snmpRetries = 3;                 // total attempts
    int snmpRetryDelayMs = 500;
    uint16_t snmpPort = 161;

    // [discovery]
    // Optional SNMP sweep for printers on the network that this PC has no
    // print queue for; they are reported with portType "Network" alongside
    // the installed ones. OFF by default: it sends an SNMP request to every
    // address in the ranges each cycle, which an IDS may flag as a scan.
    // Uses [snmp] community and port.
    bool discoveryEnabled = false;
    bool discoveryScanLocalSubnets = true;  // this PC's own IPv4 subnets
    std::string discoverySubnets;           // extra ranges, comma-separated CIDRs ("10.0.5.0/24, 10.0.7.20")
    // A local subnet wider than this (e.g. a /16) is narrowed to the
    // /min_prefix_length around this PC's own address.
    int discoveryMinPrefixLength = 22;
    int discoveryMaxHosts = 4096;           // hard cap on addresses swept per cycle
    int discoveryTimeoutMs = 1500;          // per sweep attempt, for the whole batch
    int discoveryRetries = 2;               // total sweep attempts

    // [pjl]
    // Optional highest-priority USB/local page-count source: sends a tiny raw
    // PJL status query print job to the device and reads its reply for the
    // printer's own true lifetime page count (unlike the WMI spooler counter,
    // which resets to 0 whenever the Print Spooler service restarts). OFF by
    // default: a printer that does not understand PJL could print a garbled
    // or blank page in response to this query. Only enable after confirming
    // (e.g. by watching the printer physically during one test cycle) that a
    // given fleet's printers handle it safely.
    bool pjlEnabled = false;
    int pjlTimeoutMs = 5000;

    static PrinterConfig Load(const IniFile& ini);
};
