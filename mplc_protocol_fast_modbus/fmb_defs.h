#pragma once
#include <cstdint>
#include <cstddef>
constexpr uint8_t FMB_BROADCAST_ADDR=0xFD, FMB_FUNC=0x46, FMB_FUNC_LEGACY=0x60;
constexpr uint8_t FMB_SUB_SCAN_START=1,FMB_SUB_SCAN_NEXT=2,FMB_SUB_SCAN_RSP=3,FMB_SUB_SCAN_END=4;
constexpr uint8_t FMB_SUB_BY_SER_REQ=8,FMB_SUB_BY_SER_RSP=9;
constexpr uint8_t FMB_SUB_EVT_REQUEST=0x10,FMB_SUB_EVT_TRANSMIT=0x11,FMB_SUB_EVT_NONE=0x12,FMB_SUB_EVT_CONFIG=0x18;
// Existing project setting numbers; convert to WB wire numbers at codec boundaries only.
constexpr uint8_t FMB_TYPE_COIL=0,FMB_TYPE_DISCRETE=1,FMB_TYPE_HOLDING=2,FMB_TYPE_INPUT=3,FMB_TYPE_REBOOT=15;
constexpr uint8_t FMB_PRIO_DISABLED=0,FMB_PRIO_LOW=1,FMB_PRIO_HIGH=2;
constexpr uint8_t MB_FC_READ_COILS=1,MB_FC_READ_DISCRETE=2,MB_FC_READ_HOLDING=3,MB_FC_READ_INPUT=4;
constexpr uint8_t MB_FC_WRITE_COIL=5,MB_FC_WRITE_REGISTER=6,MB_FC_WRITE_COILS=15,MB_FC_WRITE_REGS=16;
constexpr size_t FMB_SCAN_FRAME_LEN=10,FMB_MAX_ADU=256;
struct FmbScanItem { uint32_t serial{}; uint8_t modbus_addr{}; };
struct FmbEvent {
    uint8_t slave_id{},type{},payload_len{};
    uint16_t reg_addr{};
    uint8_t payload[248]{}; // WB little-endian VALUE; unknown event types are safely skipped.
};
inline uint32_t fmb_reg_key(uint8_t type,uint16_t reg){return (uint32_t(type)<<16)|reg;}
