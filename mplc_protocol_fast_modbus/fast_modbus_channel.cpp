#include "fast_modbus_channel.h"
#include "fmb_settings.h"
#include <mplc/vm/node_typese.h>
void FastModbusRegChannel::Init(const mplc::vm::Channel* channel){
    // Native Channel settings omit scalar zero values. Missing numeric entries
    // therefore mean zero (Coils/U16/disabled priority), not a nonzero default.
    int type=fmb_setting(channel,"RegType",0);
    int addr=fmb_setting(channel,"RegAddr",0);
    int priority=fmb_setting(channel,"Priority",0);
    int valueType=fmb_setting(channel,"ValueType",0);
    int order=fmb_setting(channel,"WordOrder",0);
    int interval=fmb_setting(channel,"MinIntervalMs",0);
    int window=fmb_setting(channel,"BurstWindowMs",0);
    int count=fmb_setting(channel,"BurstMaxEvents",0);
    state.valid=type>=0&&type<=3&&addr>=0&&addr<=65535&&priority>=0&&priority<=2&&
        valueType>=0&&valueType<=4&&order>=0&&order<=1&&interval>=0&&window>=0&&count>=0;
    state.type=uint8_t(type);state.reg=uint16_t(addr);state.priority=uint8_t(priority);
    state.value_type=fmb::ValueType(valueType);state.low_word_first=order!=0;
    // A scale must be explicitly nonzero; an omitted/zero scale is invalid.
    state.scale=fmb_setting(channel,"Scale",0.0);
    state.offset=fmb_setting(channel,"Offset",0.0);
    state.filter.deadband=fmb_setting(channel,"Deadband",0.0);
    state.filter.min_interval_ms=uint32_t(interval);state.filter.burst_window_ms=uint32_t(window);
    state.filter.burst_max_events=uint32_t(count);
    state.sink=[this](double value,bool good,fmb::Source){
        if(!InVar)return;
        if(good)InVar.Set(value,{},OpcUa_Good);
        else InVar.SetQuality(OpcUa_BadCommunicationError);
    };
}
