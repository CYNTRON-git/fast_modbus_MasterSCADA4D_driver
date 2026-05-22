#include "fmb_frames.h"
#include "fmb_transport.h"
#include <cstring>

namespace fmb {

// ---- Private helpers ----

static void push_u16be(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>(val >> 8));
    v.push_back(static_cast<uint8_t>(val & 0xFF));
}
static void push_u32be(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >>  8) & 0xFF));
    v.push_back(static_cast<uint8_t>( val        & 0xFF));
}
static uint16_t read_u16be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
static uint32_t read_u32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) <<  8)
         |  static_cast<uint32_t>(p[3]);
}

// ============================================================
//  Standard Modbus RTU builders
// ============================================================

std::vector<uint8_t> build_read_regs(uint8_t addr, uint8_t fc, uint16_t reg, uint16_t count)
{
    std::vector<uint8_t> f;
    f.reserve(8);
    f.push_back(addr);
    f.push_back(fc);
    push_u16be(f, reg);
    push_u16be(f, count);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_write_reg(uint8_t addr, uint16_t reg, uint16_t value)
{
    std::vector<uint8_t> f;
    f.push_back(addr);
    f.push_back(MB_FC_WRITE_REGISTER);
    push_u16be(f, reg);
    push_u16be(f, value);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_write_regs(uint8_t addr, uint16_t reg,
                                       const uint16_t* values, uint8_t count)
{
    std::vector<uint8_t> f;
    f.push_back(addr);
    f.push_back(MB_FC_WRITE_REGS);
    push_u16be(f, reg);
    push_u16be(f, static_cast<uint16_t>(count));
    f.push_back(static_cast<uint8_t>(count * 2u)); // byte count
    for (uint8_t i = 0; i < count; ++i) push_u16be(f, values[i]);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_write_coil(uint8_t addr, uint16_t reg, bool value)
{
    std::vector<uint8_t> f;
    f.push_back(addr);
    f.push_back(MB_FC_WRITE_COIL);
    push_u16be(f, reg);
    push_u16be(f, value ? 0xFF00u : 0x0000u);
    FmbTransport::append_crc(f);
    return f;
}

// ============================================================
//  Standard Modbus RTU parsers
// ============================================================

std::vector<uint16_t> parse_read_response(const uint8_t* frame, size_t len)
{
    std::vector<uint16_t> result;
    if (len < 5) return result;
    if (!FmbTransport::check_crc(frame, len)) return result;
    uint8_t fc = frame[1];
    if (fc & 0x80) return result; // exception response

    uint8_t byte_count = frame[2];
    if (static_cast<size_t>(byte_count) + 5u > len) return result;

    const uint8_t* data = frame + 3;
    if (fc == MB_FC_READ_COILS || fc == MB_FC_READ_DISCRETE) {
        for (uint8_t b = 0; b < byte_count; ++b)
            for (int bit = 0; bit < 8; ++bit)
                result.push_back((data[b] >> bit) & 1u);
    } else {
        // FC03/FC04: BE uint16 pairs; caller applies int16 cast if needed
        for (uint8_t i = 0; i + 1 < byte_count; i += 2)
            result.push_back(read_u16be(data + i));
    }
    return result;
}

// ============================================================
//  Fast Modbus WB-extension builders
//  Reference: MR-02m fast_mb.c + MR-02m-flasher modbus_rtu.py
// ============================================================

// All broadcast scan frames: FD 46 [sub] CRC_L CRC_H (5 bytes)
static std::vector<uint8_t> build_broadcast3(uint8_t sub)
{
    std::vector<uint8_t> f;
    f.push_back(FMB_BROADCAST_ADDR);
    f.push_back(FMB_FUNC);
    f.push_back(sub);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_scan_start() { return build_broadcast3(FMB_SUB_SCAN_START); }
std::vector<uint8_t> build_scan_next()  { return build_broadcast3(FMB_SUB_SCAN_NEXT);  }
std::vector<uint8_t> build_scan_end()   { return build_broadcast3(FMB_SUB_SCAN_END);   }

std::vector<uint8_t> build_event_request(uint8_t min_slave_id, uint8_t max_data_len,
                                          uint8_t ack_slave, uint8_t ack_flag)
{
    // FD 46 10 [min_slave] [max_data_len] [ack_slave] [ack_flag] CRC_L CRC_H тАФ 9 bytes
    std::vector<uint8_t> f;
    f.push_back(FMB_BROADCAST_ADDR);
    f.push_back(FMB_FUNC);
    f.push_back(FMB_SUB_EVT_REQUEST);
    f.push_back(min_slave_id);
    f.push_back(max_data_len);
    f.push_back(ack_slave);
    f.push_back(ack_flag);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_event_config(uint8_t slave_addr, uint8_t type,
                                         uint16_t reg_addr, uint8_t count,
                                         uint8_t prio)
{
    // [addr] 46 18 05 [type] [reg_h] [reg_l] [count] [prio] CRC_L CRC_H тАФ 11 bytes
    // data_len byte (0x05) = type(1) + reg(2) + count(1) + prio(1) = 5
    std::vector<uint8_t> f;
    f.push_back(slave_addr);
    f.push_back(FMB_FUNC);
    f.push_back(FMB_SUB_EVT_CONFIG);
    f.push_back(0x05);          // data length following
    f.push_back(type);
    push_u16be(f, reg_addr);
    f.push_back(count);         // 1 byte тАФ NOT 2!
    f.push_back(prio);
    FmbTransport::append_crc(f);
    return f;
}

std::vector<uint8_t> build_by_serial_request(uint32_t serial,
                                               const std::vector<uint8_t>& inner_pdu)
{
    // FD 46 08 [serial BE 4B] [inner_pdu] CRC
    std::vector<uint8_t> f;
    f.push_back(FMB_BROADCAST_ADDR);
    f.push_back(FMB_FUNC);
    f.push_back(FMB_SUB_BY_SER_REQ);
    push_u32be(f, serial);
    f.insert(f.end(), inner_pdu.begin(), inner_pdu.end());
    FmbTransport::append_crc(f);
    return f;
}

// ============================================================
//  Fast Modbus parsers
// ============================================================

bool parse_scan_response(const uint8_t* frame, size_t len, FmbScanItem& out)
{
    // FD 46 03 [serial BE 4B] [mb_addr] CRC_L CRC_H тАФ 10 bytes
    if (len < FMB_SCAN_FRAME_LEN) return false;
    if (!FmbTransport::check_crc(frame, len)) return false;
    if ((frame[1] != FMB_FUNC && frame[1] != FMB_FUNC_LEGACY)) return false;
    if (frame[2] != FMB_SUB_SCAN_RSP) return false;

    // frame[0] = 0xFD (broadcast); frame[3..6] = serial; frame[7] = modbus_addr
    out.serial      = read_u32be(frame + 3);
    out.modbus_addr = frame[7];
    return true;
}

int parse_event_response(const uint8_t* frame, size_t len,
                          std::vector<FmbEvent>& events,
                          uint8_t& out_ack_slave,
                          uint8_t& out_ack_flag)
{
    if (len < 5) return -1;
    if (!FmbTransport::check_crc(frame, len)) return -1;
    if (frame[1] != FMB_FUNC && frame[1] != FMB_FUNC_LEGACY) return -1;

    uint8_t sub = frame[2];

    if (sub == FMB_SUB_EVT_NONE) {
        // [slave_id] 46 12 [FLAG] CRC_L CRC_H тАФ 6 bytes
        // Bus quiet; no confirmation needed.
        return 0;
    }

    if (sub != FMB_SUB_EVT_TRANSMIT) return -1;

    // [slave_id] 46 11 [FLAG] [N] [DATA_LEN] [events ├Ч N] CRC_L CRC_H
    if (len < 8) return -1;

    uint8_t slave_id  = frame[0];
    uint8_t flag      = frame[3];
    uint8_t evt_count = frame[4];
    uint8_t data_len  = frame[5];

    out_ack_slave = slave_id;
    out_ack_flag  = flag;

    if (evt_count == 0) return 1; // empty batch (confirms previous reception)

    // Format B (MR-02m >= 1.0.8.8): no PLEN prefix per event.
    // Event layout: [TYPE:1] [REG_H:1] [REG_L:1] [VALUE:0/1/2 bytes]
    // Payload size is determined by TYPE.
    const uint8_t* p   = frame + 6;
    const uint8_t* end = frame + 6 + data_len;
    if (end > frame + len - 2) end = frame + len - 2; // guard: stay before CRC

    events.reserve(evt_count);

    for (uint8_t i = 0; i < evt_count && p + 3 <= end; ++i) {
        FmbEvent ev{};
        ev.slave_id = slave_id;
        ev.type     = p[0];
        ev.reg_addr = read_u16be(p + 1);
        p += 3;

        // Payload bytes determined by register type (no PLEN byte in Format B)
        uint8_t plen;
        switch (ev.type) {
            case FMB_TYPE_COIL:
            case FMB_TYPE_DISCRETE: plen = 1; break;
            case FMB_TYPE_HOLDING:
            case FMB_TYPE_INPUT:    plen = 2; break;
            case FMB_TYPE_REBOOT:   plen = 0; break;
            default:
                // Unknown type: cannot determine size, stop parsing
                goto end_parse;
        }

        if (p + plen > end) break;
        ev.payload_len = plen;
        if (plen > 0) ev.payload[0] = p[0];
        if (plen > 1) ev.payload[1] = p[1];
        p += plen;
        events.push_back(ev);
    }
end_parse:;

    return 1;
}

double fmb_event_to_double(const FmbEvent& ev)
{
    switch (ev.payload_len) {
        case 0: return 0.0; // REBOOT
        case 1: return static_cast<double>(ev.payload[0]); // COIL / DISCRETE (0 or 1)
        case 2: {
            // HOLDING / INPUT: big-endian; interpret as signed int16.
            // Most MR-02m sensor registers are signed (temperature ├Ч10, etc.).
            uint16_t u16 = (static_cast<uint16_t>(ev.payload[0]) << 8) | ev.payload[1];
            return static_cast<double>(static_cast<int16_t>(u16));
        }
        default: return 0.0;
    }
}

} // namespace fmb
