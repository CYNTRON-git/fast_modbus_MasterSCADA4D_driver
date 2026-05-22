#pragma once
#include <mplc/api.h>
#include "fmb_transport.h"
#include "fmb_frames.h"
#include "fast_modbus_module.h"
#include <vector>
#include <chrono>

// Main protocol class: owns the RS-485 bus, manages scan + event polling.
// One instance per serial port (protocol node in MS4).
class FastModbusProtocol : public mplc::api::ScadaProtocol {
public:
    MPLC_OBJECT(FastModbusProtocol);

    // ---- Configurable properties (set from MS4 project editor) ----
    STRING PortName{"/dev/ttyUSB0"};
    INT    BaudRate{9600};
    INT    Parity{0};                    // 0=None, 1=Even, 2=Odd
    INT    StopBits{1};
    INT    DataBits{8};
    INT    ResponseTimeoutMs{200};
    INT    InterFrameDelayMs{5};         // Extra delay between frames (ms), added to t3.5
    INT    EventPollIntervalMs{50};      // How often to request events (ms)
    INT    FallbackPollPeriodMs{1000};   // Cyclic RTU poll period for non-FMB devices (ms)
    BOOL   EnableFastModbus{true};       // false = cyclic Modbus RTU only for all devices
    BOOL   AutoScan{false};              // Scan bus on Init to discover serial numbers

    // ---- ScadaProtocol interface ----
    void Init() override;
    void Execute() override;
    mplc::api::ScadaModule* Create(const mplc::vm::IOModule* module) override;

private:
    FmbTransport m_transport;
    std::vector<FastModbusDeviceModule*> m_modules;

    // ---- Event polling confirm state ----
    uint8_t m_confirm_slave{0};  // slave_id from last 0x11 to confirm
    uint8_t m_confirm_flag{0};   // flag from last 0x11 to confirm

    using clock = std::chrono::steady_clock;
    clock::time_point m_last_event_poll{};
    clock::time_point m_last_fallback{};
    clock::time_point m_last_scan{};

    bool m_initialized{false};

    // ---- Internal methods ----

    // Scan bus (0x01/0x02) to discover device serial numbers.
    void scan_bus();

    // Send 0x18 event-config commands for all channels of one device that need syncing.
    void sync_device_priorities(FastModbusDeviceModule* mod);

    // Loop event-request/response until bus quiet (0x12) or timeout.
    void poll_events();

    // Send one Modbus RTU request and receive response.
    // Returns byte count received, <=0 on error.
    int modbus_request(const std::vector<uint8_t>& req, uint8_t* resp_buf, size_t resp_max);

    // Send one 0x18 priority config command for a single channel.
    // Returns true on valid ACK.
    bool send_event_config(FastModbusDeviceModule* mod, FastModbusRegChannel* ch);

    // Send a write command (FC05/FC06) to a device.
    void send_write(FastModbusDeviceModule* mod, const FmbWriteCmd& cmd);

    // Cyclic RTU read of all channels (used for non-FMB devices, or when FMB is disabled).
    void fallback_poll(FastModbusDeviceModule* mod);

    // Cyclic RTU read of only prio=DISABLED channels on a confirmed FMB device.
    // Called every FallbackPollPeriodMs for FMB devices that have some channels
    // explicitly excluded from event reporting.
    void fallback_poll_disabled_channels(FastModbusDeviceModule* mod);

    // Wait t3.5 + InterFrameDelayMs between bus transactions.
    void inter_frame_delay();
};
