#include "fast_modbus_channel.h"
#include <mplc/vm/node_typese.h>
#include <cmath>

void FastModbusRegChannel::Init(const mplc::vm::Channel* channel)
{
    // Read per-channel settings from the MS4 project tree.
    // Settings keys match names configured in the MS4 protocol editor.

    {
        int v = static_cast<int>(FMB_TYPE_INPUT);
        channel->get("RegType").GetInt(v);
        reg_type = static_cast<uint8_t>(v);
    }
    {
        int v = 0;
        channel->get("RegAddr").GetInt(v);
        reg_addr = static_cast<uint16_t>(v);
    }
    {
        int v = 1;
        channel->get("Scale").GetInt(v); // stored as int * 1000 to avoid float settings
        // If Scale is 0 (default for GetInt fallback), use 1.0
        // Prefer float if the setting is stored as REAL
        float fv = 1.0f;
        // Try REAL path first; if GetInt succeeded with non-zero, it might be 1000x
        // The simplest approach: treat Scale as float directly if stored as REAL.
        // For integer fallback, leave scale at 1.0f.
        (void)v;
        channel->get("Scale").Get<float>(fv);
        scale = (fv == 0.0f) ? 1.0f : fv;
    }
    {
        float fv = 0.0f;
        channel->get("Offset").Get<float>(fv);
        offset = fv;
    }
    {
        int v = static_cast<int>(FMB_PRIO_HIGH);
        channel->get("Priority").GetInt(v);
        if (v < 0) v = 0;
        if (v > 2) v = 2;
        prio = static_cast<uint8_t>(v);
    }
    {
        // Anti-spam / rate-limit settings
        float fv = 0.0f;
        channel->get("Deadband").Get<float>(fv);
        filter.deadband = static_cast<double>(fv);
    }
    {
        int v = 0;
        channel->get("MinIntervalMs").GetInt(v);
        filter.min_interval_ms = static_cast<uint32_t>(v > 0 ? v : 0);
    }
    {
        int v = 0;
        channel->get("BurstWindowMs").GetInt(v);
        filter.burst_window_ms = static_cast<uint32_t>(v > 0 ? v : 0);
    }
    {
        int v = 0;
        channel->get("BurstMaxEvents").GetInt(v);
        filter.burst_max_events = static_cast<uint16_t>(v > 0 ? v : 0);
    }
}

void FastModbusRegChannel::push_value(double value, LuaDataProvider* provider)
{
    double physical = value * static_cast<double>(scale) + static_cast<double>(offset);
    Write<float>(provider, static_cast<float>(physical));
}

void FastModbusRegChannel::on_event(double raw_value,
                                     std::chrono::steady_clock::time_point ts,
                                     LuaDataProvider* provider)
{
    if (filter.should_commit(raw_value, ts)) {
        push_value(raw_value, provider);
        filter.mark_committed(raw_value, ts);
    }
    // else: filter stored it as pending
}

void FastModbusRegChannel::flush_pending(std::chrono::steady_clock::time_point now,
                                          LuaDataProvider* provider)
{
    double val = 0.0;
    if (filter.try_flush_pending(now, val)) {
        push_value(val, provider);
        filter.mark_committed(val, now);
    }
}
