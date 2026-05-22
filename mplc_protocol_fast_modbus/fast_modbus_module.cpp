#include "fast_modbus_module.h"
#include "fmb_frames.h"
#include <mplc/vm/node_typese.h>
#include <cmath>
#include <algorithm>

// ---- Init: read device-level settings from MS4 project tree ----

void FastModbusDeviceModule::Init(const mplc::vm::IOModule* modl)
{
    {
        int v = 1;
        modl->get("ModbusAddress").GetInt(v);
        if (v < 1 || v > 247) v = 1;
        modbus_addr = static_cast<uint8_t>(v);
    }
    {
        int v = 0;
        modl->get("SerialNumber").GetInt(v);
        serial_number = static_cast<uint32_t>(v);
    }
    {
        bool b = false;
        modl->get("UseSerial").GetBool(b);
        use_serial = b;
    }
    display_name = std::string(modl->name.utf8());
}

// ---- Channel creation ----

mplc::api::ScadaChannel* FastModbusDeviceModule::Create(const mplc::vm::Channel* channel,
                                                          LuaDataProvider* provider)
{
    auto* ch = new FastModbusRegChannel();
    ch->BaseInit(channel, provider);

    uint32_t key = fmb_reg_key(ch->reg_type, ch->reg_addr);
    channel_by_reg[key] = ch;
    channels_all.push_back(ch);
    return ch;
}

// ---- Event dispatch ----

void FastModbusDeviceModule::dispatch_event(const FmbEvent& ev, LuaDataProvider* provider,
                                             std::chrono::steady_clock::time_point now)
{
    uint32_t key = fmb_reg_key(ev.type, ev.reg_addr);
    auto it = channel_by_reg.find(key);
    if (it == channel_by_reg.end()) return;

    double raw = fmb::fmb_event_to_double(ev);
    it->second->on_event(raw, now, provider);
}

// ---- Flush pending (anti-spam timer) ----

void FastModbusDeviceModule::flush_pending_values(LuaDataProvider* provider,
                                                   std::chrono::steady_clock::time_point now)
{
    for (auto* ch : channels_all)
        ch->flush_pending(now, provider);
}

// ---- Collect write commands (MS4 тЖТ device) ----

std::vector<FmbWriteCmd> FastModbusDeviceModule::collect_writes(LuaDataProvider* provider)
{
    std::vector<FmbWriteCmd> writes;
    if (!isWrite()) return writes;

    for (auto* ch : channels_all) {
        if (!ch->OutVar) continue;
        if (ch->reg_type != FMB_TYPE_COIL && ch->reg_type != FMB_TYPE_HOLDING)
            continue; // INPUT / DISCRETE are read-only

        OpcUa_VariantHlp val;
        auto status = ch->ReadVariant(provider, val);
        if (!OpcUa_IsGood(status)) continue;
        if (!IsNeedWrite(*ch, val)) continue;

        // Apply inverse scale: raw = (physical - offset) / scale
        float fv  = val.Get<float>(0.0f);
        float scl = (std::fabs(ch->scale) > 1e-9f) ? ch->scale : 1.0f;
        float raw_f = (fv - ch->offset) / scl;

        // Clamp to uint16 range
        raw_f = std::max(0.0f, std::min(65535.0f, std::roundf(raw_f)));

        FmbWriteCmd cmd;
        cmd.reg_type = ch->reg_type;
        cmd.reg_addr = ch->reg_addr;
        cmd.value    = static_cast<uint16_t>(raw_f);
        writes.push_back(cmd);
    }
    return writes;
}

// ---- Priority sync helpers ----

bool FastModbusDeviceModule::needs_priority_sync() const
{
    // If probe finished and device is not FMB-capable, stop sending 0x18
    if (fmb_probe_done && !fmb_capable) return false;

    for (const auto* ch : channels_all)
        if (!ch->prio_synced) return true;
    return false;
}

void FastModbusDeviceModule::mark_priority_synced()
{
    for (auto* ch : channels_all)
        ch->prio_synced = true;
}

void FastModbusDeviceModule::reset_prio_sync()
{
    // Re-sync needed (e.g. after REBOOT event or port reconnect)
    fmb_probe_done  = false;   // re-run probe in case device changed
    fmb_capable     = false;
    prio_fail_count = 0;
    for (auto* ch : channels_all)
        ch->prio_synced = false;
}

bool FastModbusDeviceModule::has_disabled_channels() const
{
    for (const auto* ch : channels_all)
        if (!ch->is_event_enabled()) return true;
    return false;
}
