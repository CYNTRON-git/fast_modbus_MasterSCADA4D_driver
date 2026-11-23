#include "fast_modbus_module.h"
#include "fmb_settings.h"
#include <mplc/vm/node_typese.h>
#include <cmath>
FastModbusDeviceModule::~FastModbusDeviceModule(){for(auto*c:channels){Unregister(c);delete c;}}
mplc::api::ScadaChannel* FastModbusDeviceModule::Create(const mplc::vm::Channel* node,LuaDataProvider*){
    auto*c=new FastModbusRegChannel();c->BaseInit(node);Register(c);channels.push_back(c);state.channels.push_back(&c->state);return c;
}
void FastModbusDeviceModule::Init(const mplc::vm::IOModule* node){
    // ModbusAddress is the implementation's original spelling; retain README alias for old projects.
    int addr=fmb_setting(node,"ModbusAddress",fmb_setting(node,"ModbusAddr",0));
    state.valid=addr>=1&&addr<=247;state.addr=uint8_t(addr);
    state.serial=uint32_t(fmb_setting(node,"SerialNumber",0));
    state.use_serial=fmb_setting(node,"UseSerial",false);
    state.group_event_ranges=fmb_setting(node,"GroupEventRanges",false);
    // Native exports omit false settings. A positive opt-out survives export
    // while an older module without this new flag keeps event support enabled.
    state.enable_events=!fmb_setting(node,"DisableEvents",false);
    state.status=[this](bool bad,const std::string& message){UpdateFaultState(bad,message);};
}
void FastModbusDeviceModule::collect_writes(fmb::Engine& engine,bool protocol_permit){
    bool module_permit=isWrite();
    bool permit=protocol_permit&&module_permit;
    for(auto*c:channels){
        if(!c->OutVar||!c->OutVar.HasValue())continue;
        if(OpcUa_IsBad(c->OutVar.Get().GetStatusCode()))continue;
        double v=c->OutVar.Get<double>();
        if(!std::isfinite(v))continue;
        if(c->output_gate.take(v,engine.options.allow_writes,permit)&&!engine.queue_write(state,c->state,v))
            c->state.write_failed=true;
    }
}
