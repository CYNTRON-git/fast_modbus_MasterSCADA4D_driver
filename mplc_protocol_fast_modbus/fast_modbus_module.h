#pragma once
#include <mplc/api.h>
#include "fast_modbus_channel.h"
class FastModbusDeviceModule final : public mplc::api::ScadaModule {
public:
    fmb::Device state;
    std::vector<FastModbusRegChannel*> channels;
    ~FastModbusDeviceModule()override;
    mplc::api::ScadaChannel* Create(const mplc::vm::Channel*,LuaDataProvider*)override;
    void collect_writes(fmb::Engine&,bool protocol_permit);
protected:
    void Init(const mplc::vm::IOModule*)override;
};
