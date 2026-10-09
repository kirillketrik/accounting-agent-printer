#include "runtime/agent.h"
#include "printer_config.h"
#include "collector/data_collector.h"
#include "collector/liveness_tracker.h"
#include "discovery/network_discovery.h"
#include "snmp/snmp_client.h"

// Printer inventory agent: installed printers + their lifetime page counters.
//
// The report is {hostname, collectedAtUtc, hostIpAddresses[], printers[]}.
// The accounting profile for it reads the `printers` list, finds the asset by
// the printer's `name` and writes `pageCount` as a meter reading - see
// "Агент принтеров через профиль" in the accounting docs/agents.md and
// README.md here.

namespace {

class PrinterAgent : public Agent {
public:
    bool Init(const AgentContext& ctx) override {
        config_ = PrinterConfig::Load(ctx.config.ini);
        if (!SnmpGlobalInit(ctx.logger)) {
            ctx.logger.Error("SNMP subsystem failed to initialize; network printer page counts will be "
                "unavailable this run.");
            return false;
        }
        snmpReady_ = true;
        return true;
    }

    std::string Collect(const AgentContext& ctx) override {
        return CollectPrinterReport(config_, ctx.logger, liveness_, discovery_, ctx.stopEvent);
    }

    void Shutdown(const AgentContext& /*ctx*/) override {
        if (snmpReady_) SnmpGlobalCleanup();
        snmpReady_ = false;
    }

private:
    PrinterConfig config_;
    bool snmpReady_ = false;
    // Kept across cycles so consecutive-failure counts, "last known
    // printers" and printers found by discovery survive for the process run.
    LivenessTracker liveness_;
    NetworkDiscovery discovery_;
};

} // namespace

const AgentInfo& GetAgentInfo() {
    static const AgentInfo info = {
        "printer",
        L"PrinterInventoryAgent",
        L"Printer Inventory Agent",
        L"Collects installed-printer inventory and page counts and reports them to the accounting server.",
        "1.1",
    };
    return info;
}

std::unique_ptr<Agent> CreateAgent() {
    return std::unique_ptr<Agent>(new PrinterAgent());
}
