#pragma once
#include <mplc/api.h>
#include "fmb_defs.h"
#include "fmb_event_filter.h"
#include <chrono>

// One Modbus register (or coil) mapped to a MS4 channel.
// RegType values: 0=COIL, 1=DISCRETE, 2=HOLDING, 3=INPUT
// DataScale / DataOffset applied to raw uint16 value before pushing to MS4.
class FastModbusRegChannel : public mplc::api::ScadaChannel {
public:
    // ---- Configuration read from vm::Channel::settings ----
    uint8_t  reg_type{FMB_TYPE_INPUT};  // HOLDING=2, INPUT=3, COIL=0, DISCRETE=1
    uint16_t reg_addr{0};
    float    scale{1.0f};               // value = raw * scale + offset
    float    offset{0.0f};
    uint8_t  prio{FMB_PRIO_HIGH};       // event priority to configure on slave
    bool     prio_synced{false};        // whether 0x18 has been sent for this channel

    // True if this channel should produce Fast Modbus events (prio != DISABLED).
    // DISABLED channels are always polled via fallback RTU read.
    bool is_event_enabled() const { return prio != FMB_PRIO_DISABLED; }

    FmbEventFilter filter;

    // Called by FastModbusDeviceModule::dispatch_event() when a 0x11 event arrives.
    // Applies scale/offset + filter, then pushes to MS4 if filter passes.
    void on_event(double raw_value, std::chrono::steady_clock::time_point ts,
                  LuaDataProvider* provider);

    // Called every Execute() cycle: flush pending value if min_interval elapsed.
    void flush_pending(std::chrono::steady_clock::time_point now, LuaDataProvider* provider);

    // Push a concrete value to MS4 InVar.
    void push_value(double value, LuaDataProvider* provider);

protected:
    void Init(const mplc::vm::Channel* channel) override;
};
