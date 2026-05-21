#pragma once
#include <mplc/api.h>
#include <share/scada_types.h>
#include "core/main.h"
#include "modbus_rtu_protocol.h"

namespace mplc { namespace fast_modbus {
    // Используем протокол Modbus RTU с поддержкой быстрого Modbus
    using FastModbusProtocol = modbus_rtu::ModbusRtuProtocol;
}} // namespace mplc::fast_modbus
