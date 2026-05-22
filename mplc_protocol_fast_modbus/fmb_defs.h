#pragma once
#include <cstdint>
#include <cstddef>

// ---- Fast Modbus WB extended protocol constants ----
// Reference: github.com/wirenboard/wb-modbus-ext-scanner/docs/protocol.ru.md

constexpr uint8_t FMB_BROADCAST_ADDR = 0xFD;
constexpr uint8_t FMB_FUNC           = 0x46;
constexpr uint8_t FMB_FUNC_LEGACY    = 0x60;

// Scan subcommands
constexpr uint8_t FMB_SUB_SCAN_START  = 0x01;
constexpr uint8_t FMB_SUB_SCAN_NEXT   = 0x02;
constexpr uint8_t FMB_SUB_SCAN_RSP    = 0x03;
constexpr uint8_t FMB_SUB_SCAN_END    = 0x04;

// Serial-addressed request/response
constexpr uint8_t FMB_SUB_BY_SER_REQ  = 0x08;
constexpr uint8_t FMB_SUB_BY_SER_RSP  = 0x09;

// Event commands
constexpr uint8_t FMB_SUB_EVT_REQUEST  = 0x10;
constexpr uint8_t FMB_SUB_EVT_TRANSMIT = 0x11;
constexpr uint8_t FMB_SUB_EVT_NONE     = 0x12;
constexpr uint8_t FMB_SUB_EVT_CONFIG   = 0x18;

// Event confirm flag values (toggle)
constexpr uint8_t FMB_FLAG_A = 0x55;
constexpr uint8_t FMB_FLAG_B = 0xAA;

// Event types (maps to Modbus register types)
constexpr uint8_t FMB_TYPE_COIL     = 0x00;
constexpr uint8_t FMB_TYPE_DISCRETE = 0x01;
constexpr uint8_t FMB_TYPE_HOLDING  = 0x02;
constexpr uint8_t FMB_TYPE_INPUT    = 0x03;
constexpr uint8_t FMB_TYPE_REBOOT   = 0x0F;

// Event priorities
constexpr uint8_t FMB_PRIO_DISABLED = 0;
constexpr uint8_t FMB_PRIO_LOW      = 1;
constexpr uint8_t FMB_PRIO_HIGH     = 2;

// Scan response fixed length (address + func + sub + serial(4) + mb_addr + unused + crc(2))
constexpr size_t FMB_SCAN_FRAME_LEN = 10;

// Standard Modbus function codes
constexpr uint8_t MB_FC_READ_COILS      = 0x01;
constexpr uint8_t MB_FC_READ_DISCRETE   = 0x02;
constexpr uint8_t MB_FC_READ_HOLDING    = 0x03;
constexpr uint8_t MB_FC_READ_INPUT      = 0x04;
constexpr uint8_t MB_FC_WRITE_COIL      = 0x05;
constexpr uint8_t MB_FC_WRITE_REGISTER  = 0x06;
constexpr uint8_t MB_FC_WRITE_COILS     = 0x0F;
constexpr uint8_t MB_FC_WRITE_REGS      = 0x10;

// ---- Data structures ----

struct FmbScanItem {
    uint32_t serial{0};
    uint8_t  modbus_addr{0};
};

// One event from a slave 0x11 response
struct FmbEvent {
    uint8_t  slave_id{0};
    uint8_t  type{0};         // FMB_TYPE_*
    uint16_t reg_addr{0};     // big-endian on wire, stored as host uint16
    uint8_t  payload_len{0};  // 0 (REBOOT), 1 (COIL/DISCRETE), 2 (HOLDING/INPUT)
    uint8_t  payload[2]{};    // wire order: payload[0]=MSB for 16-bit value
};

// Key for (type, reg_addr) тЖТ channel lookup
inline uint32_t fmb_reg_key(uint8_t type, uint16_t reg_addr) {
    return (static_cast<uint32_t>(type) << 16) | reg_addr;
}
