#include "fast_modbus_protocol.h"
#include <mplc/vm/node_typese.h>
#include <cstring>
#include <string>

// ============================================================
//  Init тАФ called once after all modules/channels are created
// ============================================================

void FastModbusProtocol::Init()
{
    bool ok = m_transport.open(PortName,
                               static_cast<int>(BaudRate),
                               static_cast<int>(Parity),
                               static_cast<int>(StopBits),
                               static_cast<int>(DataBits),
                               static_cast<int>(ResponseTimeoutMs));
    SetFaultState(!ok, ok ? "" : "Cannot open serial port: " + PortName);
    if (!ok) return;

    m_initialized       = true;
    m_confirm_slave     = 0;
    m_confirm_flag      = 0;
    m_last_event_poll   = clock::now();
    m_last_fallback     = clock::now();

    if (AutoScan)
        scan_bus();

    // Initial priority config for all devices
    if (EnableFastModbus) {
        for (auto* mod : m_modules)
            sync_device_priorities(mod);
    }
}

// ============================================================
//  Execute тАФ called every TaskPeriod() ms by the runtime
// ============================================================

void FastModbusProtocol::Execute()
{
    // Re-open port if lost (e.g. USB disconnect)
    if (!m_initialized || !m_transport.is_open()) {
        bool ok = m_transport.open(PortName,
                                   static_cast<int>(BaudRate),
                                   static_cast<int>(Parity),
                                   static_cast<int>(StopBits),
                                   static_cast<int>(DataBits),
                                   static_cast<int>(ResponseTimeoutMs));
        if (!ok) {
            SetFaultState(true, "Serial port not available: " + PortName);
            for (auto* mod : m_modules)
                mod->SetFaultState(true, "No serial port");
            return;
        }
        m_initialized   = true;
        m_confirm_slave = 0;
        m_confirm_flag  = 0;
        // All priorities need re-sync after reconnect
        for (auto* mod : m_modules)
            mod->reset_prio_sync();
    }

    SetFaultState(false, "");
    auto* provider = LuaProvider();
    auto  now      = clock::now();

    // ---- 1. Flush pending values (anti-spam min_interval timer) ----
    for (auto* mod : m_modules)
        mod->flush_pending_values(provider, now);

    // ---- 2. Send writes from MS4 тЖТ device (only changed values) ----
    for (auto* mod : m_modules) {
        if (!mod->isConnect() || !mod->isExecute()) continue;

        auto writes = mod->collect_writes(provider);
        for (const auto& cmd : writes) {
            send_write(mod, cmd);
            inter_frame_delay();
        }
    }

    // ---- 3. Priority sync: immediate, per-device, as needed ----
    if (EnableFastModbus) {
        for (auto* mod : m_modules) {
            if (!mod->isConnect()) continue;
            if (mod->needs_priority_sync())
                sync_device_priorities(mod);
        }
    }

    // ---- 4. Fast Modbus event poll ----
    if (EnableFastModbus) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_last_event_poll).count();
        if (static_cast<uint32_t>(ms) >= static_cast<uint32_t>(EventPollIntervalMs)) {
            poll_events();
            m_last_event_poll = clock::now();
        }
    }

    // ---- 5. Fallback cyclic RTU poll ----
    // Three cases:
    //   A. FMB globally disabled тЖТ full RTU poll for all devices.
    //   B. FMB enabled, device probe not done yet тЖТ full RTU poll until FMB confirmed.
    //   C. FMB enabled, device confirmed non-FMB тЖТ full RTU poll.
    //   D. FMB enabled, device confirmed FMB-capable тЖТ RTU poll only for prio=DISABLED channels.
    auto since_fb = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_last_fallback).count();
    if (static_cast<uint32_t>(since_fb) >= static_cast<uint32_t>(FallbackPollPeriodMs)) {
        for (auto* mod : m_modules) {
            if (!mod->isConnect() || !mod->isExecute()) continue;

            if (!EnableFastModbus) {
                // Case A: FMB globally off тАФ pure RTU mode
                fallback_poll(mod);
            } else if (!mod->fmb_probe_done || !mod->fmb_capable) {
                // Cases B/C: unknown or confirmed non-FMB тЖТ full RTU poll
                fallback_poll(mod);
            } else {
                // Case D: confirmed FMB device тАФ only poll prio=DISABLED channels
                if (mod->has_disabled_channels())
                    fallback_poll_disabled_channels(mod);
            }
        }
        m_last_fallback = clock::now();
    }
}

// ============================================================
//  Create тАФ called for each IOModule node in MS4 project
// ============================================================

mplc::api::ScadaModule* FastModbusProtocol::Create(const mplc::vm::IOModule* module)
{
    auto* mod = new FastModbusDeviceModule();
    mod->BaseInit(module, LuaProvider());
    m_modules.push_back(mod);
    return mod;
}

// ============================================================
//  scan_bus тАФ 0x01/0x02 to discover device serial numbers
// ============================================================

void FastModbusProtocol::scan_bus()
{
    if (!m_transport.is_open()) return;

    m_transport.flush_rx();

    // Scan cycle: 0x04 (reset) тЖТ 0x01 (start) тЖТ [0x03 + 0x02] ├Ч N тЖТ 0x04 (end)
    // Reference: MR-02m-flasher modbus_rtu.py + MR-02m fast_mb.c
    // All scan frames are exactly 5 bytes: FD 46 [sub] CRC_L CRC_H.
    // Slave arbitration is internal; master sends simple 5-byte 0x02 each time.

    // Reset any stale scan state on all slaves
    m_transport.send(fmb::build_scan_end());
    inter_frame_delay();

    if (!m_transport.send(fmb::build_scan_start())) return;

    for (int attempt = 0; attempt < 247; ++attempt) {
        uint8_t resp[FMB_SCAN_FRAME_LEN + 4]{};
        int got = m_transport.recv(resp, sizeof(resp),
                                   static_cast<uint32_t>(ResponseTimeoutMs));
        if (got < static_cast<int>(FMB_SCAN_FRAME_LEN)) break; // timeout = no more devices

        FmbScanItem item;
        if (!fmb::parse_scan_response(resp, static_cast<size_t>(got), item)) break;

        // Associate discovered device with a configured module.
        // Responding to 0x03 proves Fast Modbus capability тЖТ skip 0x18 probe failures.
        for (auto* mod : m_modules) {
            if (mod->serial_number == item.serial
                || (mod->modbus_addr == item.modbus_addr && mod->serial_number == 0)) {
                mod->serial_number        = item.serial;
                mod->supports_fast_modbus = true;
                mod->fmb_capable          = true;   // scan response is capability proof
                mod->fmb_probe_done       = true;
                break;
            }
        }

        // Prompt next unscanned slave (5-byte frame, no serial field)
        inter_frame_delay();
        if (!m_transport.send(fmb::build_scan_next())) break;
    }

    // Close scan cycle тАФ important: resets i_am_not_scaned on all slaves
    m_transport.send(fmb::build_scan_end());
    inter_frame_delay();
    m_last_scan = clock::now();
}

// ============================================================
//  sync_device_priorities тАФ send 0x18 for channels needing sync
// ============================================================

void FastModbusProtocol::sync_device_priorities(FastModbusDeviceModule* mod)
{
    // Guard: if probe done and device is not FMB-capable, nothing to do
    if (mod->fmb_probe_done && !mod->fmb_capable) return;

    for (auto* ch : mod->channels_all) {
        if (ch->prio_synced) continue;

        if (!ch->is_event_enabled()) {
            // prio=DISABLED: no 0x18 needed; channel will be read via fallback RTU poll
            ch->prio_synced = true;
            continue;
        }

        if (send_event_config(mod, ch)) {
            ch->prio_synced  = true;
            mod->fmb_capable     = true;   // device responded to FC=0x46 тЖТ it's FMB-capable
            mod->fmb_probe_done  = true;
            mod->prio_fail_count = 0;      // reset on success
        } else {
            ++mod->prio_fail_count;
            if (!mod->fmb_capable && mod->prio_fail_count >= 3) {
                // 3 consecutive failures and no successful ACK yet:
                // treat as non-FMB (generic Modbus RTU) device.
                mod->fmb_probe_done = true;
                mod->fmb_capable    = false;
                // Stop sending 0x18 by marking all channels synced
                for (auto* c : mod->channels_all) c->prio_synced = true;
                return;
            }
        }
        inter_frame_delay();
    }

    // All channels attempted without a definitive failure тАФ if we haven't confirmed
    // FMB capability yet and have no event-enabled channels, mark probe done.
    if (!mod->fmb_probe_done) {
        // All non-DISABLED channels either succeeded or failed, but not yet 3 in a row.
        // Probe stays incomplete тЖТ will retry next Execute() cycle.
    }
}

bool FastModbusProtocol::send_event_config(FastModbusDeviceModule* mod,
                                             FastModbusRegChannel* ch)
{
    auto req = fmb::build_event_config(mod->modbus_addr,
                                        ch->reg_type,
                                        ch->reg_addr,
                                        1,        // count = 1 register (uint8_t)
                                        ch->prio);
    uint8_t resp[16]{};
    int got = modbus_request(req, resp, sizeof(resp));

    // ACK: [slave] 46 18 01 00 CRC_L CRC_H тАФ 7 bytes
    // data_len=0x01, status=0x00 (OK)
    return (got >= 7)
           && (resp[0] == mod->modbus_addr)
           && (resp[1] == FMB_FUNC)
           && (resp[2] == FMB_SUB_EVT_CONFIG)
           && (resp[3] == 0x01)   // data_len
           && (resp[4] == 0x00)   // status OK
           && FmbTransport::check_crc(resp, static_cast<size_t>(got));
}

// ============================================================
//  poll_events тАФ loop until bus quiet (0x12) or timeout
// ============================================================

void FastModbusProtocol::poll_events()
{
    auto* provider = LuaProvider();

    // Safety limit: avoid hogging the bus if device keeps sending events
    const int max_polls = 64;

    for (int i = 0; i < max_polls; ++i) {
        // Build request with pending confirmation from previous iteration
        auto req = fmb::build_event_request(0x01,           // min_slave_id
                                             0xFF,           // max_data_len: accept any
                                             m_confirm_slave,
                                             m_confirm_flag);
        m_transport.flush_rx();
        if (!m_transport.send(req)) {
            m_confirm_slave = 0;
            m_confirm_flag  = 0;
            break;
        }

        uint8_t resp[256]{};
        int got = m_transport.recv(resp, sizeof(resp),
                                   static_cast<uint32_t>(ResponseTimeoutMs));
        if (got < 5) {
            // Timeout тАФ bus is quiet, confirm state reset
            m_confirm_slave = 0;
            m_confirm_flag  = 0;
            break;
        }

        std::vector<FmbEvent> events;
        uint8_t cs = 0, cf = 0;
        int rc = fmb::parse_event_response(resp, static_cast<size_t>(got), events, cs, cf);

        if (rc < 0) {
            // Bad frame (CRC error, wrong subcommand)
            m_confirm_slave = 0;
            m_confirm_flag  = 0;
            break;
        }

        if (rc == 0) {
            // FMB_SUB_EVT_NONE тАФ bus quiet, done
            m_confirm_slave = 0;
            m_confirm_flag  = 0;
            break;
        }

        // rc == 1: received 0x11 event packet
        m_confirm_slave = cs;
        m_confirm_flag  = cf;

        auto now = clock::now();
        for (const auto& ev : events) {
            if (ev.type == FMB_TYPE_REBOOT) {
                // Device rebooted: must resend 0x18 for all channels.
                // We already know it's FMB-capable (it just sent an event), so
                // reset probe state but immediately re-confirm capability.
                for (auto* mod : m_modules) {
                    if (mod->modbus_addr == ev.slave_id) {
                        mod->reset_prio_sync();  // clears prio_synced, fmb_capable, fmb_probe_done
                        mod->fmb_capable    = true;  // re-confirm: we just got a REBOOT event
                        mod->fmb_probe_done = true;  // no need to re-probe
                        break;
                    }
                }
                continue;
            }

            // Dispatch to matching module
            for (auto* mod : m_modules) {
                if (mod->modbus_addr == ev.slave_id) {
                    mod->dispatch_event(ev, provider, now);
                    break;
                }
            }
        }

        inter_frame_delay();
    }
}

// ============================================================
//  send_write тАФ FC05 (coil) or FC06 (register) write
// ============================================================

void FastModbusProtocol::send_write(FastModbusDeviceModule* mod, const FmbWriteCmd& cmd)
{
    std::vector<uint8_t> req;

    if (cmd.reg_type == FMB_TYPE_COIL) {
        req = fmb::build_write_coil(mod->modbus_addr, cmd.reg_addr, cmd.value != 0);
    } else if (cmd.reg_type == FMB_TYPE_HOLDING) {
        req = fmb::build_write_reg(mod->modbus_addr, cmd.reg_addr, cmd.value);
    } else {
        return; // INPUT / DISCRETE are read-only
    }

    uint8_t resp[16]{};
    int got = modbus_request(req, resp, sizeof(resp));

    // Echo response for FC05/FC06: [addr][fc][reg H][reg L][val H][val L][crc] = 8 bytes
    bool ok = (got >= 8)
              && (resp[0] == mod->modbus_addr)
              && FmbTransport::check_crc(resp, static_cast<size_t>(got));
    mod->SetFaultState(!ok, ok ? "" : "Write failed addr=" + std::to_string(mod->modbus_addr));
}

// ============================================================
//  fallback_poll тАФ cyclic read for non-FMB devices
// ============================================================

void FastModbusProtocol::fallback_poll(FastModbusDeviceModule* mod)
{
    auto* provider = LuaProvider();
    auto  now      = clock::now();

    for (auto* ch : mod->channels_all) {
        uint8_t fc = 0;
        switch (ch->reg_type) {
            case FMB_TYPE_COIL:     fc = MB_FC_READ_COILS;   break;
            case FMB_TYPE_DISCRETE: fc = MB_FC_READ_DISCRETE; break;
            case FMB_TYPE_HOLDING:  fc = MB_FC_READ_HOLDING;  break;
            case FMB_TYPE_INPUT:    fc = MB_FC_READ_INPUT;    break;
            default: continue;
        }

        auto req = fmb::build_read_regs(mod->modbus_addr, fc, ch->reg_addr, 1);
        uint8_t resp[32]{};
        int got = modbus_request(req, resp, sizeof(resp));

        if (got < 5) {
            mod->SetFaultState(true, "No response addr=" + std::to_string(mod->modbus_addr));
            inter_frame_delay();
            continue;
        }

        auto vals = fmb::parse_read_response(resp, static_cast<size_t>(got));
        if (vals.empty()) {
            inter_frame_delay();
            continue;
        }

        // For holding/input registers: interpret raw uint16 as signed int16 (same as events)
        double raw = 0.0;
        if (ch->reg_type == FMB_TYPE_HOLDING || ch->reg_type == FMB_TYPE_INPUT) {
            raw = static_cast<double>(static_cast<int16_t>(vals[0]));
        } else {
            raw = static_cast<double>(vals[0]); // coil/discrete: 0 or 1
        }

        ch->on_event(raw, now, provider);
        mod->SetFaultState(false, "");
        inter_frame_delay();
    }
}

// ============================================================
//  fallback_poll_disabled_channels
//  RTU read only for prio=DISABLED channels on a FMB device.
//  FMB events cover all other channels; these need a periodic RTU read.
// ============================================================

void FastModbusProtocol::fallback_poll_disabled_channels(FastModbusDeviceModule* mod)
{
    auto* provider = LuaProvider();
    auto  now      = clock::now();

    for (auto* ch : mod->channels_all) {
        if (ch->is_event_enabled()) continue; // handled by Fast Modbus events

        uint8_t fc = 0;
        switch (ch->reg_type) {
            case FMB_TYPE_COIL:     fc = MB_FC_READ_COILS;   break;
            case FMB_TYPE_DISCRETE: fc = MB_FC_READ_DISCRETE; break;
            case FMB_TYPE_HOLDING:  fc = MB_FC_READ_HOLDING;  break;
            case FMB_TYPE_INPUT:    fc = MB_FC_READ_INPUT;    break;
            default: continue;
        }

        auto req = fmb::build_read_regs(mod->modbus_addr, fc, ch->reg_addr, 1);
        uint8_t resp[32]{};
        int got = modbus_request(req, resp, sizeof(resp));

        if (got < 5) { inter_frame_delay(); continue; }

        auto vals = fmb::parse_read_response(resp, static_cast<size_t>(got));
        if (vals.empty()) { inter_frame_delay(); continue; }

        double raw = (ch->reg_type == FMB_TYPE_HOLDING || ch->reg_type == FMB_TYPE_INPUT)
                   ? static_cast<double>(static_cast<int16_t>(vals[0]))
                   : static_cast<double>(vals[0]);

        ch->on_event(raw, now, provider);
        inter_frame_delay();
    }
}

// ============================================================
//  modbus_request тАФ flush + send + receive
// ============================================================

int FastModbusProtocol::modbus_request(const std::vector<uint8_t>& req,
                                        uint8_t* resp_buf, size_t resp_max)
{
    m_transport.flush_rx();
    if (!m_transport.send(req)) return -1;
    return m_transport.recv(resp_buf, resp_max,
                            static_cast<uint32_t>(ResponseTimeoutMs));
}

// ============================================================
//  inter_frame_delay тАФ t3.5 + optional extra delay
// ============================================================

void FastModbusProtocol::inter_frame_delay()
{
    m_transport.wait_t35();
    if (InterFrameDelayMs > 0)
        FmbTransport::sleep_ms(static_cast<uint32_t>(InterFrameDelayMs));
}
