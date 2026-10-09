#pragma once
#include <vector>
#include "model/printer_info.h"
#include "common/logger.h"
#include "wmi/wmi_client.h"

// Enumerates real (non-virtual) printers installed on this machine via WMI,
// with port classification and, where possible, the printer's network
// endpoint already resolved (TCP/IP HostAddress, or host/IP parsed out of a
// WSD port's Location field). SNMP/registry counter collection happens later
// (data_collector), this module only does discovery + classification.
std::vector<PrinterInfo> EnumeratePrinters(WmiSession& wmi, Logger& logger);

// Exposed for unit-style manual testing / reuse.
bool IsVirtualPrinter(const PrinterInfo& p);
PortType ClassifyPortName(const std::string& portName);

struct ParsedWsdLocation {
    bool ok = false;
    std::string host;      // literal IP (v4 or v6, scope stripped) or hostname
    bool isIPv6 = false;
    uint32_t scopeId = 0;  // IPv6 zone id, 0 if none/not applicable
};
ParsedWsdLocation ParseWsdLocation(const std::string& location);
