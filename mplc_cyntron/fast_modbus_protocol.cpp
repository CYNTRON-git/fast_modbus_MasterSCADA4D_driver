#include "fast_modbus_protocol.h"

// Регистрация типа протокола
// Используем алиас, поэтому регистрируем базовый тип
MPLC_PROTOCOL_TYPE(FastModbusProtocol, mplc::modbus_rtu::ModbusRtuProtocol);
