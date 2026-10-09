#include "printer_config.h"

PrinterConfig PrinterConfig::Load(const IniFile& ini) {
    PrinterConfig cfg;

    cfg.offlineAfterConsecutiveFailures = ini.GetInt("polling", "offline_after_consecutive_failures",
        cfg.offlineAfterConsecutiveFailures);
    if (cfg.offlineAfterConsecutiveFailures < 1) cfg.offlineAfterConsecutiveFailures = 1;

    cfg.snmpCommunity = ini.GetStr("snmp", "community", cfg.snmpCommunity);
    cfg.snmpTimeoutMs = ini.GetInt("snmp", "timeout_ms", cfg.snmpTimeoutMs);
    cfg.snmpRetries = ini.GetInt("snmp", "retries", cfg.snmpRetries);
    cfg.snmpRetryDelayMs = ini.GetInt("snmp", "retry_delay_ms", cfg.snmpRetryDelayMs);
    cfg.snmpPort = (uint16_t)ini.GetInt("snmp", "port", cfg.snmpPort);

    cfg.discoveryEnabled = ini.GetBool("discovery", "enabled", cfg.discoveryEnabled);
    cfg.discoveryScanLocalSubnets = ini.GetBool("discovery", "scan_local_subnets", cfg.discoveryScanLocalSubnets);
    cfg.discoverySubnets = ini.GetStr("discovery", "subnets", cfg.discoverySubnets);
    cfg.discoveryMinPrefixLength = ini.GetInt("discovery", "min_prefix_length", cfg.discoveryMinPrefixLength);
    if (cfg.discoveryMinPrefixLength < 16) cfg.discoveryMinPrefixLength = 16; // a /16 is already 65k addresses
    if (cfg.discoveryMinPrefixLength > 30) cfg.discoveryMinPrefixLength = 30;
    cfg.discoveryMaxHosts = ini.GetInt("discovery", "max_hosts", cfg.discoveryMaxHosts);
    if (cfg.discoveryMaxHosts < 1) cfg.discoveryMaxHosts = 1;
    if (cfg.discoveryMaxHosts > 65536) cfg.discoveryMaxHosts = 65536;
    cfg.discoveryTimeoutMs = ini.GetInt("discovery", "timeout_ms", cfg.discoveryTimeoutMs);
    if (cfg.discoveryTimeoutMs < 200) cfg.discoveryTimeoutMs = 200;
    cfg.discoveryRetries = ini.GetInt("discovery", "retries", cfg.discoveryRetries);
    if (cfg.discoveryRetries < 1) cfg.discoveryRetries = 1;

    cfg.pjlEnabled = ini.GetBool("pjl", "enabled", cfg.pjlEnabled);
    cfg.pjlTimeoutMs = ini.GetInt("pjl", "timeout_ms", cfg.pjlTimeoutMs);
    if (cfg.pjlTimeoutMs < 500) cfg.pjlTimeoutMs = 500;

    return cfg;
}
