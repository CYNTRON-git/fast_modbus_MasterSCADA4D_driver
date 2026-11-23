#pragma once
#include <mplc/api.h>
#include "fmb_transport.h"
#include "fast_modbus_module.h"
class FastModbusProtocol final : public mplc::api::ScadaProtocol {
public:
    MPLC_OBJECT(FastModbusProtocol);
    STRING PortName{"/dev/ttyUSB0"};
    DINT BaudRate{9600},Parity{0},StopBits{1},DataBits{8},ResponseTimeoutMs{200},InterFrameDelayMs{5};
    DINT EventPollIntervalMs{50},FallbackPollPeriodMs{1000},HealthPollPeriodMs{5000},DeviceTimeoutMs{15000};
    DINT MaxTransactionsPerCycle{8},MaxCycleMs{100};
    BOOL EnableFastModbus{true},AutoScan{false},AllowWrites{false};
    LINT Requests{},Responses{},Timeouts{},BadFrames{},EventPackets{},EventValues{},DuplicatePackets{};
    LINT ReadRequests{},WriteRequests{},WriteErrors{},RejectedSubscriptions{},ScannedDevices{};
    ~FastModbusProtocol()override;
    void Init()override;
    void Execute()override;
    mplc::api::ScadaModule* Create(const mplc::vm::IOModule*)override;
private:
    FmbTransport transport_;
    fmb::Engine engine_{transport_};
    std::vector<FastModbusDeviceModule*> modules_;
    bool configured_{},config_ok_{};
    uint64_t reconnect_at_{};
    void configure();
    void diagnostics();
};
