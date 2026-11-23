#pragma once
#include <mplc/api.h>
#include "fmb_core.h"
#include "fmb_output_gate.h"
class FastModbusRegChannel final : public mplc::api::ScadaChannel {
public:
    fmb::Channel state;
    fmb::OutputGate output_gate;
protected:
    void Init(const mplc::vm::Channel*)override;
};
