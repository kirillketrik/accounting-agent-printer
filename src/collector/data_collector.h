#pragma once
#include <string>
#include <windows.h>
#include "printer_config.h"
#include "common/logger.h"
#include "collector/liveness_tracker.h"
#include "discovery/network_discovery.h"

// Runs one full collection cycle and returns the JSON report: enumerate
// printers (WMI), gather page counts (SNMP for TCP/IP + WSD printers,
// PJL/WMI-spooler/registry fallback tiers for USB/local ones), optionally
// sweep the network for printers this PC has no queue for (`discovery`, when
// [discovery] enabled), evaluate reachability against `tracker` to derive each
// printer's online/offline status and detect printers that disappeared from
// Windows entirely, gather this host's own IP addresses and assemble the
// payload. Delivery to the server is the core's job, not this function's.
// Every sub-step is isolated so that one bad printer logs a warning and the
// rest of the cycle still completes. Returns an empty string only when
// nothing can be reported at all (WMI unavailable); never throws.
//
// `tracker` and `discovery` persist across calls (owned by the caller, i.e.
// the PrinterAgent) so consecutive-failure counts, "last known printers" and
// previously discovered printers survive across cycles for the lifetime of
// the process - a single cycle has no memory of its own.
//
// `stopEvent` (optional) lets a service stop/shutdown request interrupt a
// cycle between printers and between SNMP retry waits, instead of forcing
// the SCM to wait out a potentially multi-minute in-flight cycle. A cycle
// interrupted this way only marks the printers it actually got to as "seen"
// for removal-detection purposes, so a shutdown never produces false-positive
// "removed" reports for printers it simply didn't reach yet.
std::string CollectPrinterReport(const PrinterConfig& config, Logger& logger, LivenessTracker& tracker,
                                 NetworkDiscovery& discovery, HANDLE stopEvent = nullptr);
