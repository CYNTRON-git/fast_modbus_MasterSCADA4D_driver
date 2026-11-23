#pragma once
#include "fmb_defs.h"
#include <vector>
namespace fmb {
using Frame=std::vector<uint8_t>;
Frame build_read_regs(uint8_t addr,uint8_t fc,uint16_t reg,uint16_t count);
Frame build_write_reg(uint8_t addr,uint16_t reg,uint16_t value);
Frame build_write_regs(uint8_t addr,uint16_t reg,const uint16_t* values,uint8_t count);
Frame build_write_coil(uint8_t addr,uint16_t reg,bool value);
Frame build_scan_start();
Frame build_scan_next();
// 0x04 is a response, not a command sent by the master.
Frame build_event_request(uint8_t min_slave=1,uint8_t max_data=248,uint8_t ack_slave=0,uint8_t ack_flag=0);
Frame build_event_config(uint8_t slave,uint8_t internal_type,uint16_t reg,uint8_t count,uint8_t priority);
Frame build_by_serial_request(uint32_t serial,const Frame& inner_pdu);
bool unwrap_serial(const Frame& response,uint32_t serial,uint8_t slave,Frame& inner);
bool parse_scan_response(const uint8_t*,size_t,FmbScanItem&);
bool scan_finished(const uint8_t*,size_t);
int parse_event_response(const uint8_t*,size_t,std::vector<FmbEvent>&,uint8_t& ack_slave,uint8_t& ack_flag);
enum class EventWire { WB, CyntronLegacy };
int parse_event_response(const uint8_t*,size_t,std::vector<FmbEvent>&,uint8_t& ack_slave,uint8_t& ack_flag,EventWire& format);
std::vector<uint16_t> parse_read_response(const uint8_t*,size_t);
bool validate_read(const Frame& response,uint8_t addr,uint8_t fc,uint16_t count,std::vector<uint16_t>& values);
bool validate_write(const Frame& request,const Frame& response);
// -1 invalid, 0 disabled/unsupported, 1 enabled; multi-word value uses the first bit.
int parse_config_response(const Frame& response,uint8_t addr,uint8_t count);
bool exception(const Frame&,uint8_t addr,uint8_t fc,uint8_t& code);
enum class ValueType : uint8_t { U16=0,I16=1,U32=2,I32=3,F32=4 };
size_t value_words(ValueType type);
bool decode_value(const uint8_t* bytes,size_t count,bool little,ValueType type,bool low_word_first,double& result);
bool encode_value(double value,ValueType type,bool low_word_first,std::vector<uint16_t>& words);
// Response length after leading arbitration FF has been discarded; -1 invalid, 0 incomplete header.
int response_length(const uint8_t* data,size_t length);
}
