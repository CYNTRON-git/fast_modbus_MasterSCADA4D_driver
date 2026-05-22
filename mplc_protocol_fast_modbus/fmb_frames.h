#pragma once
#include "fmb_defs.h"
#include <vector>
#include <cstdint>

// Frame builder/parser functions.
// Builders return complete frames INCLUDING CRC (appended internally).
// Parsers validate CRC internally; return false / -1 on error.

namespace fmb {

// ---- Standard Modbus RTU builders ----

std::vector<uint8_t> build_read_regs(uint8_t addr, uint8_t fc, uint16_t reg, uint16_t count);
std::vector<uint8_t> build_write_reg(uint8_t addr, uint16_t reg, uint16_t value);
std::vector<uint8_t> build_write_regs(uint8_t addr, uint16_t reg,
                                       const uint16_t* values, uint8_t count);
std::vector<uint8_t> build_write_coil(uint8_t addr, uint16_t reg, bool value);

// ---- Standard Modbus RTU parsers ----

// Parse FC01/02/03/04 read response.
// FC01/02: each element is 0 or 1 (coil/discrete).
// FC03/04: each element is uint16 (holding/input); caller casts to int16 if signed.
// Returns empty vector on error or invalid CRC.
std::vector<uint16_t> parse_read_response(const uint8_t* frame, size_t len);

// ---- Fast Modbus WB-extension builders ----
// Reference: MR-02m firmware fast_mb.c + MR-02m-flasher modbus_rtu.py
// All broadcast frames use address 0xFD; all are exactly 5 bytes.

// Scan start тАФ FD 46 01 CRC_L CRC_H (5 bytes)
std::vector<uint8_t> build_scan_start();

// Scan next  тАФ FD 46 02 CRC_L CRC_H (5 bytes)
// No serial or priority fields; slave handles arbitration internally.
std::vector<uint8_t> build_scan_next();

// Scan end   тАФ FD 46 04 CRC_L CRC_H (5 bytes)
// Sent before and after each complete scan cycle.
std::vector<uint8_t> build_scan_end();

// Event request тАФ FD 46 10 [min_slave] [max_data_len] [ack_slave] [ack_flag] CRC (9 bytes)
// ack_slave=0, ack_flag=0 means no confirmation pending.
std::vector<uint8_t> build_event_request(uint8_t min_slave_id = 0x01,
                                          uint8_t max_data_len  = 0xFF,
                                          uint8_t ack_slave     = 0x00,
                                          uint8_t ack_flag      = 0x00);

// Event priority config тАФ unicast [addr] 46 18 05 [type] [reg_h] [reg_l] [count] [prio] CRC (11 bytes)
// count is 1 byte (1..255); prio: FMB_PRIO_DISABLED/LOW/HIGH.
// ACK from device: [addr] 46 18 01 00 CRC_L CRC_H (7 bytes).
std::vector<uint8_t> build_event_config(uint8_t slave_addr, uint8_t type,
                                         uint16_t reg_addr, uint8_t count,
                                         uint8_t prio);

// Serial-addressed inner-PDU wrapper тАФ FD 46 08 [serial BE 4B] [inner_pdu] CRC
std::vector<uint8_t> build_by_serial_request(uint32_t serial,
                                               const std::vector<uint8_t>& inner_pdu);

// ---- Fast Modbus parsers ----

// Parse scan response (FMB_SUB_SCAN_RSP = 0x03).
// Expected: FD 46 03 [serial BE 4B] [mb_addr] CRC_L CRC_H (10 bytes).
// Returns false on CRC error or wrong subcommand.
bool parse_scan_response(const uint8_t* frame, size_t len, FmbScanItem& out);

// Parse event response.
//   Returns  1 тАФ events received (0x11 FMB_SUB_EVT_TRANSMIT); events vector populated.
//   Returns  0 тАФ no events (0x12 FMB_SUB_EVT_NONE); bus quiet.
//   Returns -1 тАФ frame error (CRC, unexpected subcommand, length).
// out_ack_slave / out_ack_flag are set from the 0x11 response for use in next 0x10 ack fields.
// Format B (MR-02m >= 1.0.8.8): event records have no PLEN prefix; length is TYPE-determined.
int parse_event_response(const uint8_t* frame, size_t len,
                          std::vector<FmbEvent>& events,
                          uint8_t& out_ack_slave,
                          uint8_t& out_ack_flag);

// Convert raw event payload to double.
// COIL/DISCRETE: unsigned byte (0 or 1).
// HOLDING/INPUT: signed int16 big-endian (most sensor registers are signed).
// REBOOT: 0.0.
double fmb_event_to_double(const FmbEvent& ev);

} // namespace fmb
