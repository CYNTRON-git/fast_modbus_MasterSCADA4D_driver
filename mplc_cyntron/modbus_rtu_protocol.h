#pragma once
#include <mplc/api.h>
#include <share/scada_types.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include "core/main.h"

namespace mplc { namespace modbus_rtu {

    // Структура для хранения информации об устройстве
    struct ModbusDevice {
        uint8_t address;
        std::string serial_number;
        bool supports_fast_modbus;
        std::chrono::steady_clock::time_point last_seen;
        
        ModbusDevice() : address(0), supports_fast_modbus(false) {}
    };

    // Структура для события от устройства
    struct ModbusEvent {
        uint8_t device_address;
        uint16_t register_address;
        uint16_t value;
        uint8_t event_type;
        std::chrono::steady_clock::time_point timestamp;
    };

    // Класс для работы с Modbus RTU каналом
    class ModbusRtuChannel final : public api::ScadaChannel {
    public:
        MPLC_OBJECT(ModbusRtuChannel);
        
        ModbusRtuChannel();
        ~ModbusRtuChannel() override = default;
        
        void Init() override;
        void Execute() override;
        
        // Настройки канала
        std::string port_name;           // Имя COM порта (например, "/dev/ttyUSB0" или "COM1")
        uint32_t baud_rate;              // Скорость (9600, 19200, 38400, 57600, 115200)
        uint8_t data_bits;               // Биты данных (7 или 8)
        uint8_t stop_bits;               // Стоп-биты (1 или 2)
        uint8_t parity;                  // Четность (0=none, 1=odd, 2=even)
        uint32_t response_timeout_ms;    // Таймаут ответа в мс
        uint32_t inter_frame_delay_ms;   // Задержка между кадрами в мс
        
        // Настройки быстрого Modbus
        bool enable_fast_modbus;         // Включить поддержку быстрого Modbus
        uint32_t event_poll_interval_ms; // Интервал опроса событий (50 мс по умолчанию)
        bool auto_scan_enabled;          // Автоматическое сканирование устройств
        
        // Методы для работы с регистрами
        bool ReadHoldingRegisters(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values);
        bool ReadInputRegisters(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values);
        bool ReadCoils(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<bool>& values);
        bool ReadDiscreteInputs(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<bool>& values);
        bool WriteSingleRegister(uint8_t slave_id, uint16_t address, uint16_t value);
        bool WriteMultipleRegisters(uint8_t slave_id, uint16_t start_addr, const std::vector<uint16_t>& values);
        bool WriteSingleCoil(uint8_t slave_id, uint16_t address, bool value);
        bool WriteMultipleCoils(uint8_t slave_id, uint16_t start_addr, const std::vector<bool>& values);
        
        // Методы для быстрого Modbus
        bool ScanBus(std::vector<ModbusDevice>& devices);  // Быстрое сканирование шины
        // Запрос событий WB: 0x10 с полями min_slave_id, max_data_len, confirm_slave_id, confirm_flag (protocol.ru.md).
        // Для первого опроса передайте confirm_slave_id=0, confirm_flag=0; при успешном ответе 0x11 сохраните out_* для следующего вызова.
        bool RequestEvents(std::vector<ModbusEvent>& events,
            uint8_t confirm_slave_id = 0, uint8_t confirm_flag = 0,
            uint8_t* out_confirm_slave_id = nullptr, uint8_t* out_confirm_flag = nullptr);
        bool ConfigureEvent(uint8_t slave_id, uint16_t register_addr, bool enable);  // Настройка события
        bool ResolveAddressConflict(uint8_t old_address, uint8_t new_address);  // Разрешение коллизии адресов
        
        // Обращение по серийному номеру (0xFD 0x46 0x08/0x09, как в flasher_windows)
        bool ReadHoldingRegistersBySerial(uint32_t serial, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values);
        bool WriteSingleRegisterBySerial(uint32_t serial, uint16_t address, uint16_t value);
        bool WriteMultipleRegistersBySerial(uint32_t serial, uint16_t start_addr, const std::vector<uint16_t>& values);
        
        // Проверка поддержки быстрого Modbus устройством
        bool CheckFastModbusSupport(uint8_t slave_id);
        
    private:
        int serial_fd;  // Файловый дескриптор последовательного порта
        
        // Внутренние методы для работы с протоколом
        bool OpenSerialPort();
        void CloseSerialPort();
        bool SendFrame(const std::vector<uint8_t>& frame);
        bool ReceiveFrame(std::vector<uint8_t>& frame, uint32_t timeout_ms);
        uint16_t CalculateCRC(const std::vector<uint8_t>& data);
        bool ValidateCRC(const std::vector<uint8_t>& frame);
        
        // Формирование Modbus RTU запросов
        std::vector<uint8_t> BuildReadHoldingRegistersRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity);
        std::vector<uint8_t> BuildReadInputRegistersRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity);
        std::vector<uint8_t> BuildReadCoilsRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity);
        std::vector<uint8_t> BuildReadDiscreteInputsRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity);
        std::vector<uint8_t> BuildWriteSingleRegisterRequest(uint8_t slave_id, uint16_t address, uint16_t value);
        std::vector<uint8_t> BuildWriteMultipleRegistersRequest(uint8_t slave_id, uint16_t start_addr, const std::vector<uint16_t>& values);
        std::vector<uint8_t> BuildWriteSingleCoilRequest(uint8_t slave_id, uint16_t address, bool value);
        std::vector<uint8_t> BuildWriteMultipleCoilsRequest(uint8_t slave_id, uint16_t start_addr, const std::vector<bool>& values);
        
        // Методы для быстрого Modbus
        std::vector<uint8_t> BuildBroadcastScanRequest();  // Начало сканирования (0x01)
        std::vector<uint8_t> BuildScanContinueRequest();  // Продолжение сканирования (0x02)
        std::vector<uint8_t> BuildEventRequestRequest(uint8_t min_slave_id, uint8_t max_data_len, uint8_t confirm_slave_id, uint8_t confirm_flag);
        std::vector<uint8_t> BuildEventConfigRequest(uint8_t slave_id, uint16_t register_addr, bool enable);
        std::vector<uint8_t> BuildSetAddressRequest(uint8_t old_address, uint8_t new_address);
        
        // Обращение по серийному: 0xFD 0x46 0x08 [serial 4B BE] inner_pdu CRC
        std::vector<uint8_t> BuildFastModbusRequest(uint32_t serial, const std::vector<uint8_t>& inner_pdu);
        std::vector<uint8_t> BuildReadHoldingRegistersBody(uint16_t start_addr, uint16_t quantity);
        std::vector<uint8_t> BuildWriteSingleRegisterBody(uint16_t address, uint16_t value);
        std::vector<uint8_t> BuildWriteMultipleRegistersBody(uint16_t start_addr, const std::vector<uint16_t>& values);
        
        // Парсинг ответов
        bool ParseReadRegistersResponse(const std::vector<uint8_t>& response, std::vector<uint16_t>& values);
        bool ParseReadCoilsResponse(const std::vector<uint8_t>& response, std::vector<bool>& values);
        bool ParseScanResponse(const std::vector<uint8_t>& response, std::vector<ModbusDevice>& devices);
        bool ParseEventResponse(const std::vector<uint8_t>& response, std::vector<ModbusEvent>& events);
        // Ответ 0xFD 0x46 0x09: out_serial и out_inner_payload (тело без адреса; для 0x03 — byte_count + data)
        bool ParseFastModbusResponse(const std::vector<uint8_t>& response, uint32_t* out_serial, std::vector<uint8_t>* out_inner_payload);
        
        bool ExchangeBySerial(uint32_t serial, const std::vector<uint8_t>& inner_pdu, std::vector<uint8_t>& inner_response);
        
        // Управление состоянием
        std::mutex port_mutex;
        std::atomic<bool> port_opened;
        std::chrono::steady_clock::time_point last_event_poll;
        
        // Кэш устройств
        std::unordered_map<uint8_t, ModbusDevice> device_cache;
        std::mutex cache_mutex;
    };

    // Класс протокола Modbus RTU
    class ModbusRtuProtocol final : public api::ScadaProtocol {
    public:
        MPLC_OBJECT(ModbusRtuProtocol);
        
        ModbusRtuProtocol();
        ModbusRtuProtocol(const ModbusRtuProtocol&) = delete;
        ModbusRtuProtocol& operator=(const ModbusRtuProtocol&) = delete;
        ~ModbusRtuProtocol() override = default;
        
        void Init() override;
        void Execute() override;
        
        api::ScadaChannel* Create(const vm::Channel* channel) override;
        
    private:
        std::unordered_map<std::string, ModbusRtuChannel*> channels;
        std::mutex channels_mutex;
    };

}} // namespace mplc::modbus_rtu
