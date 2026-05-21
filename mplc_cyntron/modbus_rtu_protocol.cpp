#include "modbus_rtu_protocol.h"
#include "core/includes.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <chrono>
#include <thread>
#else
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <chrono>
#include <thread>
#endif

namespace mplc { namespace modbus_rtu {

    // Соответствие flasher_windows/modbus_rtu.py:
    // CRC16: poly 0xA001, init 0xFFFF, в кадре LSB first (как struct.pack("<H", crc)).
    // Сканирование: 0xFD 0x46/0x60 0x01+CRC (start), 0x02+CRC (next), ответ 0x03 [serial 4B BE][addr 1B] CRC (10 байт).
    // По серийному: запрос 0xFD 0x46 0x08 [serial 4B BE] inner_pdu CRC; ответ 0xFD 0x46 0x09 [serial 4B BE] inner CRC.

    // Константы Modbus RTU
    constexpr uint8_t MODBUS_BROADCAST_ADDRESS = 0x00;  // Стандартный broadcast адрес
    constexpr uint8_t MODBUS_EXTENDED_ADDRESS = 0xFD;  // Адрес для расширенных команд Wiren Board
    constexpr uint8_t MODBUS_MIN_ADDRESS = 0x01;
    constexpr uint8_t MODBUS_MAX_ADDRESS = 0xF7;
    
    // Коды функций Modbus
    constexpr uint8_t FUNC_READ_COILS = 0x01;
    constexpr uint8_t FUNC_READ_DISCRETE_INPUTS = 0x02;
    constexpr uint8_t FUNC_READ_HOLDING_REGISTERS = 0x03;
    constexpr uint8_t FUNC_READ_INPUT_REGISTERS = 0x04;
    constexpr uint8_t FUNC_WRITE_SINGLE_COIL = 0x05;
    constexpr uint8_t FUNC_WRITE_SINGLE_REGISTER = 0x06;
    constexpr uint8_t FUNC_WRITE_MULTIPLE_COILS = 0x0F;
    constexpr uint8_t FUNC_WRITE_MULTIPLE_REGISTERS = 0x10;
    
    // Код функции для расширенных команд Wiren Board
    constexpr uint8_t FUNC_EXTENDED = 0x46;       // Основная команда для расширенных функций
    constexpr uint8_t FUNC_EXTENDED_LEGACY = 0x60;  // Legacy (wb-modbus-ext-scanner)
    
    // Субкоманды сканирования шины
    constexpr uint8_t SUB_CMD_SCAN_START = 0x01;      // Начало сканирования
    constexpr uint8_t SUB_CMD_SCAN_CONTINUE = 0x02;  // Продолжение сканирования
    constexpr uint8_t SUB_CMD_SCAN_RESPONSE = 0x03;  // Ответ на сканирование
    constexpr uint8_t SUB_CMD_SCAN_END = 0x04;       // Конец сканирования
    
    // Субкоманды работы с устройствами по серийному номеру
    constexpr uint8_t SUB_CMD_SEND_STD_CMD = 0x08;   // Отправка стандартной команды
    constexpr uint8_t SUB_CMD_STD_CMD_RESPONSE = 0x09; // Ответ на стандартную команду
    
    // Субкоманды событий
    constexpr uint8_t SUB_CMD_EVENT_REQUEST = 0x10;  // Запрос событий
    constexpr uint8_t SUB_CMD_EVENT_TRANSMIT = 0x11; // Передача событий
    constexpr uint8_t SUB_CMD_EVENT_NONE = 0x12;     // Ответ если события отсутствуют
    constexpr uint8_t SUB_CMD_EVENT_CONFIG = 0x18;   // Настройка отправки событий
    
    // Смена адреса устройства (разрешение коллизий)
    constexpr uint8_t FUNC_FAST_SET_ADDRESS = 0x41;
    
    // Фиксированная длина кадра ответа на сканирование WB: 0xFD 0x46/0x60 0x03 [serial 4B BE] [addr 1B] CRC
    constexpr size_t WB_EXT_SCAN_FRAME_LEN = 10;
    
    // Коды ошибок Modbus
    constexpr uint8_t ERROR_ILLEGAL_FUNCTION = 0x01;
    constexpr uint8_t ERROR_ILLEGAL_DATA_ADDRESS = 0x02;
    constexpr uint8_t ERROR_ILLEGAL_DATA_VALUE = 0x03;
    constexpr uint8_t ERROR_SLAVE_DEVICE_FAILURE = 0x04;
    
    // Таблица CRC16 для Modbus
    static const uint16_t crc16_table[256] = {
        0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
        0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
        0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
        0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
        0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
        0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
        0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
        0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
        0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
        0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
        0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
        0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
        0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
        0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
        0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
        0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
        0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
        0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
        0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
        0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
        0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
        0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
        0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
        0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
        0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
        0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
        0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
        0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
        0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x4A81, 0x4A40,
        0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
        0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
        0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
    };

    // Вычисление CRC16 для Modbus
    uint16_t ModbusRtuChannel::CalculateCRC(const std::vector<uint8_t>& data) {
        uint16_t crc = 0xFFFF;
        for (uint8_t byte : data) {
            uint8_t index = (crc ^ byte) & 0xFF;
            crc = (crc >> 8) ^ crc16_table[index];
        }
        return crc;
    }

    // Проверка CRC в кадре
    bool ModbusRtuChannel::ValidateCRC(const std::vector<uint8_t>& frame) {
        if (frame.size() < 4) return false;  // Минимум: адрес + функция + CRC (2 байта)
        
        std::vector<uint8_t> data(frame.begin(), frame.end() - 2);
        uint16_t calculated_crc = CalculateCRC(data);
        uint16_t received_crc = (static_cast<uint16_t>(frame[frame.size() - 1]) << 8) | frame[frame.size() - 2];
        
        return calculated_crc == received_crc;
    }

    // Конструктор канала
    ModbusRtuChannel::ModbusRtuChannel() 
        : serial_fd(-1)
        , port_opened(false)
        , port_name("/dev/ttyUSB0")
        , baud_rate(9600)
        , data_bits(8)
        , stop_bits(1)
        , parity(0)
        , response_timeout_ms(1000)
        , inter_frame_delay_ms(5)
        , enable_fast_modbus(true)
        , event_poll_interval_ms(50)
        , auto_scan_enabled(true)
    {
    }

    void ModbusRtuChannel::Init() {
        if (!OpenSerialPort()) {
            SetFaultState(true, "Не удалось открыть последовательный порт");
            return;
        }
        
        // Если включен быстрый Modbus и автоматическое сканирование
        if (enable_fast_modbus && auto_scan_enabled) {
            std::vector<ModbusDevice> devices;
            if (ScanBus(devices)) {
                std::lock_guard<std::mutex> lock(cache_mutex);
                for (const auto& dev : devices) {
                    device_cache[dev.address] = dev;
                }
            } else {
                // Если broadcast сканирование не дало результатов, 
                // возможно устройства не поддерживают быстрый Modbus
                // Это нормально - будем работать в обычном режиме
            }
        }
        
        SetFaultState(false, "");
    }

    void ModbusRtuChannel::Execute() {
        if (!port_opened) {
            if (!OpenSerialPort()) {
                SetFaultState(true, "Последовательный порт не открыт");
                return;
            }
        }
        
        // Опрос событий быстрого Modbus
        if (enable_fast_modbus) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_event_poll).count();
            
            if (elapsed >= event_poll_interval_ms) {
                std::vector<ModbusEvent> events;
                if (RequestEvents(events)) {
                    // Обработка событий - обновление переменных канала
                    for (const auto& event : events) {
                        // Обновляем переменные канала на основе событий
                        // События приходят от устройств, поддерживающих быстрый Modbus
                        // Здесь можно обновить соответствующие переменные канала
                        // в зависимости от адреса регистра события
                        
                        // Пример: если событие связано с регистром, обновляем переменную
                        // ch->InVar.Update(event.value);
                    }
                }
                last_event_poll = now;
            }
        }
        
        SetFaultState(false, "");
    }

    // Открытие последовательного порта
    bool ModbusRtuChannel::OpenSerialPort() {
        std::lock_guard<std::mutex> lock(port_mutex);
        
        if (port_opened && serial_fd >= 0) {
            return true;
        }
        
#ifdef _WIN32
        std::wstring wport_name(port_name.begin(), port_name.end());
        serial_fd = (int)CreateFileW(
            wport_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        
        if (serial_fd == INVALID_HANDLE_VALUE) {
            serial_fd = -1;
            return false;
        }
        
        DCB dcb = {0};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState((HANDLE)serial_fd, &dcb)) {
            CloseHandle((HANDLE)serial_fd);
            serial_fd = -1;
            return false;
        }
        
        dcb.BaudRate = baud_rate;
        dcb.ByteSize = data_bits;
        dcb.StopBits = (stop_bits == 1) ? ONESTOPBIT : TWOSTOPBITS;
        dcb.Parity = (parity == 0) ? NOPARITY : ((parity == 1) ? ODDPARITY : EVENPARITY);
        dcb.fBinary = TRUE;
        dcb.fParity = (parity != 0);
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fTXContinueOnXoff = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fAbortOnError = FALSE;
        
        if (!SetCommState((HANDLE)serial_fd, &dcb)) {
            CloseHandle((HANDLE)serial_fd);
            serial_fd = -1;
            return false;
        }
        
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = response_timeout_ms;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 1000;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        SetCommTimeouts((HANDLE)serial_fd, &timeouts);
        
#else
        serial_fd = open(port_name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd < 0) {
            return false;
        }
        
        termios tty;
        if (tcgetattr(serial_fd, &tty) != 0) {
            close(serial_fd);
            serial_fd = -1;
            return false;
        }
        
        // Настройка скорости
        speed_t speed;
        switch (baud_rate) {
            case 9600: speed = B9600; break;
            case 19200: speed = B19200; break;
            case 38400: speed = B38400; break;
            case 57600: speed = B57600; break;
            case 115200: speed = B115200; break;
            default: speed = B9600; break;
        }
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);
        
        // Настройка битов данных, стоп-битов и четности
        tty.c_cflag &= ~PARENB;  // Отключить четность по умолчанию
        tty.c_cflag &= ~CSTOPB;  // 1 стоп-бит
        tty.c_cflag &= ~CSIZE;   // Очистить биты размера
        tty.c_cflag |= CS8;      // 8 бит данных
        
        if (parity == 1) {
            tty.c_cflag |= PARENB | PARODD;  // Нечетная четность
        } else if (parity == 2) {
            tty.c_cflag |= PARENB;  // Четная четность
        }
        
        if (stop_bits == 2) {
            tty.c_cflag |= CSTOPB;
        }
        
        tty.c_cflag |= CREAD | CLOCAL;  // Включить прием и локальный режим
        tty.c_cflag &= ~CRTSCTS;  // Отключить аппаратное управление потоком
        
        tty.c_lflag &= ~ICANON;
        tty.c_lflag &= ~ECHO;
        tty.c_lflag &= ~ECHOE;
        tty.c_lflag &= ~ISIG;
        
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        
        tty.c_oflag &= ~OPOST;
        
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = response_timeout_ms / 100;  // Таймаут в десятых долях секунды
        
        if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
            close(serial_fd);
            serial_fd = -1;
            return false;
        }
        
        // Переключение в блокирующий режим
        int flags = fcntl(serial_fd, F_GETFL, 0);
        fcntl(serial_fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
        
        port_opened = true;
        return true;
    }

    void ModbusRtuChannel::CloseSerialPort() {
        std::lock_guard<std::mutex> lock(port_mutex);
        
        if (serial_fd >= 0) {
#ifdef _WIN32
            CloseHandle((HANDLE)serial_fd);
#else
            close(serial_fd);
#endif
            serial_fd = -1;
        }
        port_opened = false;
    }

    // Отправка кадра
    bool ModbusRtuChannel::SendFrame(const std::vector<uint8_t>& frame) {
        if (!port_opened || serial_fd < 0) {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(port_mutex);
        
#ifdef _WIN32
        DWORD written;
        if (!WriteFile((HANDLE)serial_fd, frame.data(), (DWORD)frame.size(), &written, NULL)) {
            return false;
        }
        return written == frame.size();
#else
        ssize_t written = write(serial_fd, frame.data(), frame.size());
        return written == (ssize_t)frame.size();
#endif
    }

    // Прием кадра
    bool ModbusRtuChannel::ReceiveFrame(std::vector<uint8_t>& frame, uint32_t timeout_ms) {
        if (!port_opened || serial_fd < 0) {
            return false;
        }
        
        frame.clear();
        auto start_time = std::chrono::steady_clock::now();
        
        // Читаем минимум 4 байта (адрес + функция + CRC)
        while (frame.size() < 4) {
            uint8_t byte;
            
#ifdef _WIN32
            DWORD read;
            if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_ms) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
#else
            ssize_t bytes_read = ::read(serial_fd, &byte, 1);
            if (bytes_read <= 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_ms) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
#endif
            
            frame.push_back(byte);
        }
        
        // Расширенные кадры 0xFD 0x46/0x60 (ответ на сканирование WB): ровно 10 байт
        if (frame.size() >= 3 && frame[0] == MODBUS_EXTENDED_ADDRESS
            && (frame[1] == FUNC_EXTENDED || frame[1] == FUNC_EXTENDED_LEGACY)
            && frame[2] == SUB_CMD_SCAN_RESPONSE) {
            while (frame.size() < WB_EXT_SCAN_FRAME_LEN) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t bytes_read = ::read(serial_fd, &byte, 1);
                if (bytes_read <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            return ValidateCRC(frame);
        }
        
        // Ответ по серийному 0xFD 0x46 0x09 [serial 4B BE] [inner...] CRC (переменная длина)
        if (frame.size() >= 3 && frame[0] == MODBUS_EXTENDED_ADDRESS && frame[1] == FUNC_EXTENDED
            && frame[2] == SUB_CMD_STD_CMD_RESPONSE) {
            size_t target_len = 7;
            while (frame.size() < target_len) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t bytes_read = ::read(serial_fd, &byte, 1);
                if (bytes_read <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            target_len = 8;
            while (frame.size() < target_len) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t br = ::read(serial_fd, &byte, 1);
                if (br <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            if (frame.size() >= 8) {
                uint8_t inner_func = frame[7];
                if (inner_func == FUNC_READ_HOLDING_REGISTERS) {
                    target_len = 9;
                    while (frame.size() < target_len) {
                        uint8_t byte;
#ifdef _WIN32
                        DWORD read;
                        if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time).count();
                            if (elapsed >= timeout_ms) return false;
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }
#else
                        ssize_t br = ::read(serial_fd, &byte, 1);
                        if (br <= 0) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time).count();
                            if (elapsed >= timeout_ms) return false;
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }
#endif
                        frame.push_back(byte);
                    }
                    target_len = 11 + frame[8];
                } else if (inner_func == FUNC_WRITE_SINGLE_REGISTER || inner_func == FUNC_WRITE_MULTIPLE_REGISTERS) {
                    target_len = 15;
                } else if (inner_func & 0x80) {
                    target_len = 12;
                } else {
                    target_len = 15;
                }
                while (frame.size() < target_len) {
                    uint8_t byte;
#ifdef _WIN32
                    DWORD read;
                    if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#else
                    ssize_t br = ::read(serial_fd, &byte, 1);
                    if (br <= 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#endif
                    frame.push_back(byte);
                }
            }
            return ValidateCRC(frame);
        }
        
        // Ответ «событий нет» WB: 0xFD 0x46 0x12 + CRC (5 байт)
        if (frame.size() >= 3 && frame[0] == MODBUS_EXTENDED_ADDRESS && frame[1] == FUNC_EXTENDED
            && frame[2] == SUB_CMD_EVENT_NONE) {
            while (frame.size() < 5) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t br = ::read(serial_fd, &byte, 1);
                if (br <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            return ValidateCRC(frame);
        }
        
        // Ответ с событиями WB: slave_id 0x46 0x11 flag event_count data_len [события] CRC (переменная длина)
        if (frame.size() >= 3 && frame[0] >= MODBUS_MIN_ADDRESS && frame[0] <= MODBUS_MAX_ADDRESS
            && frame[1] == FUNC_EXTENDED && frame[2] == SUB_CMD_EVENT_TRANSMIT) {
            while (frame.size() < 6) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t br = ::read(serial_fd, &byte, 1);
                if (br <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            size_t data_len = frame[5];
            size_t target_len = 6 + data_len + 2;
            while (frame.size() < target_len) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#else
                ssize_t br = ::read(serial_fd, &byte, 1);
                if (br <= 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                    if (elapsed >= timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
#endif
                frame.push_back(byte);
            }
            return ValidateCRC(frame);
        }
        
        // Определяем длину ответа на основе функции
        uint8_t func_code = frame[1];
        if (func_code & 0x80) {
            // Ответ с ошибкой - 5 байт всего
            if (frame.size() < 5) {
                uint8_t byte;
#ifdef _WIN32
                DWORD read;
                ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL);
#else
                ::read(serial_fd, &byte, 1);
#endif
                frame.push_back(byte);
            }
        } else {
            // Нормальный ответ - читаем до конца на основе функции
            if (func_code == FUNC_READ_COILS || func_code == FUNC_READ_DISCRETE_INPUTS ||
                func_code == FUNC_READ_HOLDING_REGISTERS || func_code == FUNC_READ_INPUT_REGISTERS) {
                // Читаем байт количества данных
                if (frame.size() < 3) {
                    uint8_t byte;
#ifdef _WIN32
                    DWORD read;
                    ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL);
#else
                    ::read(serial_fd, &byte, 1);
#endif
                    frame.push_back(byte);
                }
                uint8_t byte_count = frame[2];
                // Читаем данные + CRC (2 байта)
                while (frame.size() < 3 + byte_count + 2) {
                    uint8_t byte;
#ifdef _WIN32
                    DWORD read;
                    if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) {
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#else
                    ssize_t bytes_read = ::read(serial_fd, &byte, 1);
                    if (bytes_read <= 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) {
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#endif
                    frame.push_back(byte);
                }
            } else {
                // Для других функций читаем фиксированное количество байт
                // Обычно 8 байт (адрес + функция + данные + CRC)
                while (frame.size() < 8) {
                    uint8_t byte;
#ifdef _WIN32
                    DWORD read;
                    if (!ReadFile((HANDLE)serial_fd, &byte, 1, &read, NULL) || read == 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) {
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#else
                    ssize_t bytes_read = ::read(serial_fd, &byte, 1);
                    if (bytes_read <= 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed >= timeout_ms) {
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
#endif
                    frame.push_back(byte);
                }
            }
        }
        
        return ValidateCRC(frame);
    }

    // Построение запросов Modbus RTU
    std::vector<uint8_t> ModbusRtuChannel::BuildReadHoldingRegistersRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_READ_HOLDING_REGISTERS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((quantity >> 8) & 0xFF);
        frame.push_back(quantity & 0xFF);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildReadInputRegistersRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_READ_INPUT_REGISTERS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((quantity >> 8) & 0xFF);
        frame.push_back(quantity & 0xFF);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildReadCoilsRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_READ_COILS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((quantity >> 8) & 0xFF);
        frame.push_back(quantity & 0xFF);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildReadDiscreteInputsRequest(uint8_t slave_id, uint16_t start_addr, uint16_t quantity) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_READ_DISCRETE_INPUTS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((quantity >> 8) & 0xFF);
        frame.push_back(quantity & 0xFF);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteSingleRegisterRequest(uint8_t slave_id, uint16_t address, uint16_t value) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_WRITE_SINGLE_REGISTER);
        frame.push_back((address >> 8) & 0xFF);
        frame.push_back(address & 0xFF);
        frame.push_back((value >> 8) & 0xFF);
        frame.push_back(value & 0xFF);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteMultipleRegistersRequest(uint8_t slave_id, uint16_t start_addr, const std::vector<uint16_t>& values) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_WRITE_MULTIPLE_REGISTERS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((values.size() >> 8) & 0xFF);
        frame.push_back(values.size() & 0xFF);
        frame.push_back(values.size() * 2);  // Количество байт данных
        for (uint16_t val : values) {
            frame.push_back((val >> 8) & 0xFF);
            frame.push_back(val & 0xFF);
        }
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteSingleCoilRequest(uint8_t slave_id, uint16_t address, bool value) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_WRITE_SINGLE_COIL);
        frame.push_back((address >> 8) & 0xFF);
        frame.push_back(address & 0xFF);
        frame.push_back(value ? 0xFF : 0x00);
        frame.push_back(0x00);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteMultipleCoilsRequest(uint8_t slave_id, uint16_t start_addr, const std::vector<bool>& values) {
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_WRITE_MULTIPLE_COILS);
        frame.push_back((start_addr >> 8) & 0xFF);
        frame.push_back(start_addr & 0xFF);
        frame.push_back((values.size() >> 8) & 0xFF);
        frame.push_back(values.size() & 0xFF);
        
        uint8_t byte_count = (values.size() + 7) / 8;
        frame.push_back(byte_count);
        
        uint8_t byte = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i]) {
                byte |= (1 << (i % 8));
            }
            if ((i + 1) % 8 == 0 || i == values.size() - 1) {
                frame.push_back(byte);
                byte = 0;
            }
        }
        
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    // Построение запросов быстрого Modbus
    std::vector<uint8_t> ModbusRtuChannel::BuildBroadcastScanRequest() {
        // Команда начала сканирования согласно документации Wiren Board
        // Формат: 0xFD (адрес) + 0x46 (команда) + 0x01 (субкоманда начала сканирования) + CRC
        std::vector<uint8_t> frame;
        frame.push_back(MODBUS_EXTENDED_ADDRESS);  // 0xFD для расширенных команд
        frame.push_back(FUNC_EXTENDED);            // 0x46 - команда расширенных функций
        frame.push_back(SUB_CMD_SCAN_START);      // 0x01 - начало сканирования
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }
    
    std::vector<uint8_t> ModbusRtuChannel::BuildScanContinueRequest() {
        // Команда продолжения сканирования
        // Формат: 0xFD + 0x46 + 0x02 + CRC
        std::vector<uint8_t> frame;
        frame.push_back(MODBUS_EXTENDED_ADDRESS);
        frame.push_back(FUNC_EXTENDED);
        frame.push_back(SUB_CMD_SCAN_CONTINUE);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildFastModbusRequest(uint32_t serial, const std::vector<uint8_t>& inner_pdu) {
        // 0xFD 0x46 0x08 [serial 4B BE] inner_pdu CRC (как build_fast_modbus_request в modbus_rtu.py)
        std::vector<uint8_t> frame;
        frame.push_back(MODBUS_EXTENDED_ADDRESS);
        frame.push_back(FUNC_EXTENDED);
        frame.push_back(SUB_CMD_SEND_STD_CMD);
        frame.push_back((serial >> 24) & 0xFF);
        frame.push_back((serial >> 16) & 0xFF);
        frame.push_back((serial >> 8) & 0xFF);
        frame.push_back(serial & 0xFF);
        frame.insert(frame.end(), inner_pdu.begin(), inner_pdu.end());
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildReadHoldingRegistersBody(uint16_t start_addr, uint16_t quantity) {
        std::vector<uint8_t> body;
        body.push_back(FUNC_READ_HOLDING_REGISTERS);
        body.push_back((start_addr >> 8) & 0xFF);
        body.push_back(start_addr & 0xFF);
        body.push_back((quantity >> 8) & 0xFF);
        body.push_back(quantity & 0xFF);
        return body;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteSingleRegisterBody(uint16_t address, uint16_t value) {
        std::vector<uint8_t> body;
        body.push_back(FUNC_WRITE_SINGLE_REGISTER);
        body.push_back((address >> 8) & 0xFF);
        body.push_back(address & 0xFF);
        body.push_back((value >> 8) & 0xFF);
        body.push_back(value & 0xFF);
        return body;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildWriteMultipleRegistersBody(uint16_t start_addr, const std::vector<uint16_t>& values) {
        std::vector<uint8_t> body;
        body.push_back(FUNC_WRITE_MULTIPLE_REGISTERS);
        body.push_back((start_addr >> 8) & 0xFF);
        body.push_back(start_addr & 0xFF);
        body.push_back((values.size() >> 8) & 0xFF);
        body.push_back(values.size() & 0xFF);
        body.push_back(static_cast<uint8_t>(values.size() * 2));
        for (uint16_t val : values) {
            body.push_back((val >> 8) & 0xFF);
            body.push_back(val & 0xFF);
        }
        return body;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildEventRequestRequest(uint8_t min_slave_id, uint8_t max_data_len, uint8_t confirm_slave_id, uint8_t confirm_flag) {
        // Запрос событий WB (protocol.ru.md): FD 46 10 min_slave_id max_data_len confirm_slave_id confirm_flag + CRC (10 байт)
        std::vector<uint8_t> frame;
        frame.push_back(MODBUS_EXTENDED_ADDRESS);
        frame.push_back(FUNC_EXTENDED);
        frame.push_back(SUB_CMD_EVENT_REQUEST);
        frame.push_back(min_slave_id);
        frame.push_back(max_data_len);
        frame.push_back(confirm_slave_id);
        frame.push_back(confirm_flag);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildEventConfigRequest(uint8_t slave_id, uint16_t register_addr, bool enable) {
        // Упрощенная версия настройки событий (полная версия требует диапазонов регистров)
        // Формат: slave_id + 0x46 + 0x18 + длина + настройки + CRC
        // Здесь реализована упрощенная версия для одного регистра
        std::vector<uint8_t> frame;
        frame.push_back(slave_id);
        frame.push_back(FUNC_EXTENDED);
        frame.push_back(SUB_CMD_EVENT_CONFIG);
        frame.push_back(0x01);  // Длина списка настроек (1 диапазон)
        // Тип регистра (0x01=coils, 0x02=discrete, 0x03=holding, 0x04=input)
        frame.push_back(0x03);  // Предполагаем holding registers
        frame.push_back((register_addr >> 8) & 0xFF);
        frame.push_back(register_addr & 0xFF);
        frame.push_back(0x01);  // Количество регистров (1)
        frame.push_back(enable ? 0x01 : 0x00);  // 0=выкл, 1=низкий приоритет, 2=высокий
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    std::vector<uint8_t> ModbusRtuChannel::BuildSetAddressRequest(uint8_t old_address, uint8_t new_address) {
        std::vector<uint8_t> frame;
        frame.push_back(old_address);
        frame.push_back(FUNC_FAST_SET_ADDRESS);
        frame.push_back(new_address);
        uint16_t crc = CalculateCRC(frame);
        frame.push_back(crc & 0xFF);
        frame.push_back((crc >> 8) & 0xFF);
        return frame;
    }

    // Парсинг ответов
    bool ModbusRtuChannel::ParseReadRegistersResponse(const std::vector<uint8_t>& response, std::vector<uint16_t>& values) {
        if (response.size() < 5) return false;
        if (response[1] & 0x80) return false;  // Ошибка
        
        uint8_t byte_count = response[2];
        if (response.size() < 3 + byte_count + 2) return false;
        
        values.clear();
        for (size_t i = 0; i < byte_count; i += 2) {
            uint16_t val = (static_cast<uint16_t>(response[3 + i]) << 8) | response[3 + i + 1];
            values.push_back(val);
        }
        return true;
    }

    bool ModbusRtuChannel::ParseReadCoilsResponse(const std::vector<uint8_t>& response, std::vector<bool>& values) {
        if (response.size() < 5) return false;
        if (response[1] & 0x80) return false;  // Ошибка
        
        uint8_t byte_count = response[2];
        if (response.size() < 3 + byte_count + 2) return false;
        
        values.clear();
        for (size_t i = 0; i < byte_count; ++i) {
            uint8_t byte = response[3 + i];
            for (int bit = 0; bit < 8; ++bit) {
                values.push_back((byte >> bit) & 0x01);
            }
        }
        return true;
    }

    bool ModbusRtuChannel::ParseScanResponse(const std::vector<uint8_t>& response, std::vector<ModbusDevice>& devices) {
        // Формат ответа на сканирование WB (как в flasher_windows/modbus_rtu.py):
        // 0xFD 0x46/0x60 0x03 [serial 4B BE] [modbus_addr 1B] CRC — ровно 10 байт
        if (response.size() < WB_EXT_SCAN_FRAME_LEN) return false;
        
        if (response[0] != MODBUS_EXTENDED_ADDRESS) return false;  // 0xFD
        if (response[1] != FUNC_EXTENDED && response[1] != FUNC_EXTENDED_LEGACY) return false;  // 0x46 или 0x60
        if (response[2] != SUB_CMD_SCAN_RESPONSE) return false;  // 0x03
        
        // Проверка CRC по первым 8 байтам
        std::vector<uint8_t> data_for_crc(response.begin(), response.begin() + (WB_EXT_SCAN_FRAME_LEN - 2));
        uint16_t calculated_crc = CalculateCRC(data_for_crc);
        uint16_t received_crc = static_cast<uint16_t>(response[WB_EXT_SCAN_FRAME_LEN - 2])
            | (static_cast<uint16_t>(response[WB_EXT_SCAN_FRAME_LEN - 1]) << 8);
        if (calculated_crc != received_crc) return false;
        
        uint8_t modbus_addr = response[7];
        if (modbus_addr < MODBUS_MIN_ADDRESS || modbus_addr > MODBUS_MAX_ADDRESS) return false;
        
        uint32_t serial_be = (static_cast<uint32_t>(response[3]) << 24)
            | (static_cast<uint32_t>(response[4]) << 16)
            | (static_cast<uint32_t>(response[5]) << 8)
            | response[6];
        
        ModbusDevice device;
        device.address = modbus_addr;
        char serial_hex[16];
        (void)snprintf(serial_hex, sizeof(serial_hex), "0x%08X", static_cast<unsigned>(serial_be));
        device.serial_number = serial_hex;
        device.supports_fast_modbus = true;
        device.last_seen = std::chrono::steady_clock::now();
        
        devices.push_back(device);
        return true;
    }

    bool ModbusRtuChannel::ParseFastModbusResponse(const std::vector<uint8_t>& response, uint32_t* out_serial, std::vector<uint8_t>* out_inner_payload) {
        // Ответ 0xFD 0x46 0x09 [serial 4B BE] inner_response CRC (как parse_fast_modbus_response в modbus_rtu.py)
        if (response.size() < 9) return false;
        if (response[0] != MODBUS_EXTENDED_ADDRESS || response[1] != FUNC_EXTENDED || response[2] != SUB_CMD_STD_CMD_RESPONSE)
            return false;
        std::vector<uint8_t> data_for_crc(response.begin(), response.end() - 2);
        uint16_t calculated_crc = CalculateCRC(data_for_crc);
        uint16_t received_crc = static_cast<uint16_t>(response[response.size() - 2])
            | (static_cast<uint16_t>(response[response.size() - 1]) << 8);
        if (calculated_crc != received_crc) return false;
        if (out_serial) {
            *out_serial = (static_cast<uint32_t>(response[3]) << 24)
                | (static_cast<uint32_t>(response[4]) << 16)
                | (static_cast<uint32_t>(response[5]) << 8)
                | response[6];
        }
        const size_t inner_len = response.size() - 7 - 2;
        if (inner_len >= 1 && (response[7] & 0x80))
            return false;  // Исключение Modbus
        if (out_inner_payload) {
            out_inner_payload->assign(response.begin() + 7, response.end() - 2);
        }
        return true;
    }

    bool ModbusRtuChannel::ExchangeBySerial(uint32_t serial, const std::vector<uint8_t>& inner_pdu, std::vector<uint8_t>& inner_response) {
        std::vector<uint8_t> request = BuildFastModbusRequest(serial, inner_pdu);
        if (!SendFrame(request))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        std::vector<uint8_t> frame;
        if (!ReceiveFrame(frame, response_timeout_ms))
            return false;
        uint32_t resp_serial = 0;
        inner_response.clear();
        if (!ParseFastModbusResponse(frame, &resp_serial, &inner_response))
            return false;
        if ((resp_serial & 0xFFFFFFFFu) != (serial & 0xFFFFFFFFu))
            return false;
        return true;
    }

    bool ModbusRtuChannel::ParseEventResponse(const std::vector<uint8_t>& response, std::vector<ModbusEvent>& events) {
        // Формат WB (protocol.ru.md): 0x11 = slave_id, 46, 11, flag, event_count, data_len, [события], CRC.
        // Событие: (1) len_payload, (1) type, (2) id BE, (len_payload) payload LE.
        if (response.size() < 5) return false;
        if (response[1] != FUNC_EXTENDED) return false;
        uint8_t sub_cmd = response[2];
        if (sub_cmd == SUB_CMD_EVENT_NONE) {
            events.clear();
            return true;
        }
        if (sub_cmd != SUB_CMD_EVENT_TRANSMIT) return false;
        if (response.size() < 6) return false;
        uint8_t event_count = response[4];
        uint8_t data_len = response[5];
        if (response.size() < 6 + data_len + 2u) return false;
        events.clear();
        events.reserve(event_count);
        size_t offset = 6;
        size_t data_end = 6 + data_len;
        for (uint8_t i = 0; i < event_count && offset + 4 <= data_end; ++i) {
            uint8_t len_payload = response[offset];
            if (offset + 4u + len_payload > data_end) break;
            ModbusEvent event;
            event.device_address = response[0];
            event.event_type = response[offset + 1];
            event.register_address = (static_cast<uint16_t>(response[offset + 2]) << 8) | response[offset + 3];
            event.value = 0;
            if (len_payload >= 2)
                event.value = static_cast<uint16_t>(response[offset + 4]) | (static_cast<uint16_t>(response[offset + 5]) << 8);
            else if (len_payload >= 1)
                event.value = response[offset + 4];
            event.timestamp = std::chrono::steady_clock::now();
            events.push_back(event);
            offset += 4 + len_payload;
        }
        return true;
    }

    // Публичные методы для работы с регистрами
    bool ModbusRtuChannel::ReadHoldingRegisters(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values) {
        if (!port_opened || serial_fd < 0) {
            SetFaultState(true, "Порт не открыт для чтения регистров");
            return false;
        }
        
        // Проверяем кэш - если устройство не поддерживает быстрый Modbus, работаем в обычном режиме
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = device_cache.find(slave_id);
            if (it == device_cache.end() && enable_fast_modbus) {
                // Проверяем поддержку быстрого Modbus при первом обращении
                CheckFastModbusSupport(slave_id);
            }
        }
        
        auto request = BuildReadHoldingRegistersRequest(slave_id, start_addr, quantity);
        
        if (!SendFrame(request)) {
            SetFaultState(true, "Ошибка отправки запроса чтения регистров");
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            SetFaultState(true, "Таймаут при чтении регистров");
            return false;
        }
        
        // Проверяем на ошибку Modbus
        if (response.size() >= 2 && (response[1] & 0x80)) {
            uint8_t error_code = response.size() >= 3 ? response[2] : 0;
            std::string error_msg = "Ошибка Modbus: ";
            switch (error_code) {
                case ERROR_ILLEGAL_FUNCTION: error_msg += "Неверная функция"; break;
                case ERROR_ILLEGAL_DATA_ADDRESS: error_msg += "Неверный адрес данных"; break;
                case ERROR_ILLEGAL_DATA_VALUE: error_msg += "Неверное значение данных"; break;
                case ERROR_SLAVE_DEVICE_FAILURE: error_msg += "Ошибка устройства"; break;
                default: error_msg += "Неизвестная ошибка"; break;
            }
            SetFaultState(true, error_msg.c_str());
            return false;
        }
        
        bool result = ParseReadRegistersResponse(response, values);
        if (!result) {
            SetFaultState(true, "Ошибка парсинга ответа");
        }
        return result;
    }

    bool ModbusRtuChannel::ReadInputRegisters(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values) {
        auto request = BuildReadInputRegistersRequest(slave_id, start_addr, quantity);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        return ParseReadRegistersResponse(response, values);
    }

    bool ModbusRtuChannel::ReadCoils(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<bool>& values) {
        auto request = BuildReadCoilsRequest(slave_id, start_addr, quantity);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        return ParseReadCoilsResponse(response, values);
    }

    bool ModbusRtuChannel::ReadDiscreteInputs(uint8_t slave_id, uint16_t start_addr, uint16_t quantity, std::vector<bool>& values) {
        auto request = BuildReadDiscreteInputsRequest(slave_id, start_addr, quantity);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        return ParseReadCoilsResponse(response, values);
    }

    bool ModbusRtuChannel::WriteSingleRegister(uint8_t slave_id, uint16_t address, uint16_t value) {
        auto request = BuildWriteSingleRegisterRequest(slave_id, address, value);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        // Ответ на запись должен быть эхом запроса
        if (response.size() < 8) return false;
        if (response[0] != slave_id) return false;
        if (response[1] != FUNC_WRITE_SINGLE_REGISTER) return false;
        
        return true;
    }

    bool ModbusRtuChannel::WriteMultipleRegisters(uint8_t slave_id, uint16_t start_addr, const std::vector<uint16_t>& values) {
        auto request = BuildWriteMultipleRegistersRequest(slave_id, start_addr, values);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        if (response.size() < 8) return false;
        if (response[0] != slave_id) return false;
        if (response[1] != FUNC_WRITE_MULTIPLE_REGISTERS) return false;
        
        return true;
    }

    bool ModbusRtuChannel::WriteSingleCoil(uint8_t slave_id, uint16_t address, bool value) {
        auto request = BuildWriteSingleCoilRequest(slave_id, address, value);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        if (response.size() < 8) return false;
        if (response[0] != slave_id) return false;
        if (response[1] != FUNC_WRITE_SINGLE_COIL) return false;
        
        return true;
    }

    bool ModbusRtuChannel::WriteMultipleCoils(uint8_t slave_id, uint16_t start_addr, const std::vector<bool>& values) {
        auto request = BuildWriteMultipleCoilsRequest(slave_id, start_addr, values);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        if (response.size() < 8) return false;
        if (response[0] != slave_id) return false;
        if (response[1] != FUNC_WRITE_MULTIPLE_COILS) return false;
        
        return true;
    }

    // Методы быстрого Modbus
    bool ModbusRtuChannel::ScanBus(std::vector<ModbusDevice>& devices) {
        if (!enable_fast_modbus) {
            return false;
        }
        
        devices.clear();
        
        // Шаг 1: Отправляем команду начала сканирования (0x01)
        auto request = BuildBroadcastScanRequest();
        if (!SendFrame(request)) {
            return false;
        }
        
        // Ждем 3.5 фрейма молчания для арбитража (согласно документации)
        // При 9600 бод: 3.5 * 11 бит / 9600 = ~4 мс
        // При 115200 бод: 3.5 * 11 бит / 115200 = ~0.33 мс
        // Используем задержку в зависимости от скорости
        uint32_t arbitration_delay = (baud_rate <= 9600) ? 10 : 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(arbitration_delay));
        
        // Читаем ответы от устройств
        auto start_time = std::chrono::steady_clock::now();
        bool got_response = false;
        
        while (true) {
            std::vector<uint8_t> response;
            if (ReceiveFrame(response, 50)) {  // Короткий таймаут для каждого ответа
                if (ParseScanResponse(response, devices)) {
                    got_response = true;
                }
            }
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= 100) {  // Таймаут ожидания ответа
                break;
            }
        }
        
        // Шаг 2: Продолжаем сканирование, пока есть неотсканированные устройства
        while (got_response) {
            got_response = false;
            
            // Отправляем команду продолжения сканирования (0x02)
            request = BuildScanContinueRequest();
            if (!SendFrame(request)) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(arbitration_delay));
            
            start_time = std::chrono::steady_clock::now();
            while (true) {
                std::vector<uint8_t> response;
                if (ReceiveFrame(response, 50)) {
                    // Проверяем, не конец ли сканирования (0x04)
                    if (response.size() >= 3 && response[1] == FUNC_EXTENDED && response[2] == 0x04) {
                        // Конец сканирования - все устройства отсканированы
                        return !devices.empty();
                    }
                    
                    if (ParseScanResponse(response, devices)) {
                        got_response = true;
                    }
                }
                
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= 100) {
                    break;
                }
            }
        }
        
        return !devices.empty();
    }

    bool ModbusRtuChannel::RequestEvents(std::vector<ModbusEvent>& events,
            uint8_t confirm_slave_id, uint8_t confirm_flag,
            uint8_t* out_confirm_slave_id, uint8_t* out_confirm_flag) {
        if (!enable_fast_modbus) return false;
        const uint8_t max_data_len = 200;  // по стандарту WB пакет до 256 байт
        auto request = BuildEventRequestRequest(0, max_data_len, confirm_slave_id, confirm_flag);
        if (!SendFrame(request)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, 50)) return false;
        if (response.size() >= 6 && response[1] == FUNC_EXTENDED && response[2] == SUB_CMD_EVENT_TRANSMIT) {
            if (out_confirm_slave_id) *out_confirm_slave_id = response[0];
            if (out_confirm_flag) *out_confirm_flag = response[3];
        }
        return ParseEventResponse(response, events);
    }

    bool ModbusRtuChannel::ConfigureEvent(uint8_t slave_id, uint16_t register_addr, bool enable) {
        if (!enable_fast_modbus) {
            return false;
        }
        
        auto request = BuildEventConfigRequest(slave_id, register_addr, enable);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        if (response.size() < 4) return false;
        if (response[0] != slave_id) return false;
        if (response[1] & 0x80) return false;  // Ошибка
        
        return true;
    }

    bool ModbusRtuChannel::ResolveAddressConflict(uint8_t old_address, uint8_t new_address) {
        if (!enable_fast_modbus) {
            return false;
        }
        
        auto request = BuildSetAddressRequest(old_address, new_address);
        
        if (!SendFrame(request)) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(inter_frame_delay_ms));
        
        std::vector<uint8_t> response;
        if (!ReceiveFrame(response, response_timeout_ms)) {
            return false;
        }
        
        if (response.size() < 4) return false;
        if (response[0] != old_address) return false;
        if (response[1] & 0x80) return false;  // Ошибка
        
        // Обновляем кэш
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = device_cache.find(old_address);
        if (it != device_cache.end()) {
            ModbusDevice device = it->second;
            device.address = new_address;
            device_cache.erase(it);
            device_cache[new_address] = device;
        }
        
        return true;
    }

    bool ModbusRtuChannel::ReadHoldingRegistersBySerial(uint32_t serial, uint16_t start_addr, uint16_t quantity, std::vector<uint16_t>& values) {
        if (!enable_fast_modbus || !port_opened || serial_fd < 0) return false;
        if (quantity == 0 || quantity > 125) return false;
        std::vector<uint8_t> body = BuildReadHoldingRegistersBody(start_addr, quantity);
        std::vector<uint8_t> inner;
        if (!ExchangeBySerial(serial, body, inner))
            return false;
        if (inner.size() < 3 || inner[0] != FUNC_READ_HOLDING_REGISTERS) return false;
        uint8_t byte_count = inner[1];
        if (inner.size() != 2 + byte_count) return false;
        values.clear();
        for (size_t i = 0; i < byte_count; i += 2) {
            uint16_t val = (static_cast<uint16_t>(inner[2 + i]) << 8) | inner[2 + i + 1];
            values.push_back(val);
        }
        return true;
    }

    bool ModbusRtuChannel::WriteSingleRegisterBySerial(uint32_t serial, uint16_t address, uint16_t value) {
        if (!enable_fast_modbus || !port_opened || serial_fd < 0) return false;
        std::vector<uint8_t> body = BuildWriteSingleRegisterBody(address, value);
        std::vector<uint8_t> inner;
        return ExchangeBySerial(serial, body, inner);
    }

    bool ModbusRtuChannel::WriteMultipleRegistersBySerial(uint32_t serial, uint16_t start_addr, const std::vector<uint16_t>& values) {
        if (!enable_fast_modbus || !port_opened || serial_fd < 0) return false;
        if (values.empty() || values.size() > 127) return false;
        std::vector<uint8_t> body = BuildWriteMultipleRegistersBody(start_addr, values);
        std::vector<uint8_t> inner;
        return ExchangeBySerial(serial, body, inner);
    }

    bool ModbusRtuChannel::CheckFastModbusSupport(uint8_t slave_id) {
        if (!enable_fast_modbus) {
            return false;
        }
        
        // Пытаемся отправить команду начала сканирования и проверить ответ
        auto request = BuildBroadcastScanRequest();
        
        if (!SendFrame(request)) {
            return false;
        }
        
        // Ждем арбитража
        uint32_t arbitration_delay = (baud_rate <= 9600) ? 10 : 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(arbitration_delay));
        
        std::vector<uint8_t> response;
        if (ReceiveFrame(response, 100)) {
            // Ответ на сканирование WB: 0xFD 0x46 0x03 [serial 4B][addr 1B] CRC; адрес устройства в байте 7
            std::vector<ModbusDevice> scanned;
            if (ParseScanResponse(response, scanned)) {
                for (const auto& dev : scanned) {
                    if (dev.address == slave_id) {
                        std::lock_guard<std::mutex> lock(cache_mutex);
                        auto it = device_cache.find(slave_id);
                        if (it != device_cache.end()) {
                            it->second.supports_fast_modbus = true;
                        } else {
                            ModbusDevice device;
                            device.address = slave_id;
                            device.serial_number = dev.serial_number;
                            device.supports_fast_modbus = true;
                            device.last_seen = std::chrono::steady_clock::now();
                            device_cache[slave_id] = device;
                        }
                        return true;
                    }
                }
            }
        }
        
        // Если нет ответа - устройство не поддерживает быстрый Modbus
        // Обновляем кэш
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = device_cache.find(slave_id);
        if (it != device_cache.end()) {
            it->second.supports_fast_modbus = false;
        } else {
            ModbusDevice device;
            device.address = slave_id;
            device.supports_fast_modbus = false;
            device.last_seen = std::chrono::steady_clock::now();
            device_cache[slave_id] = device;
        }
        
        return false;
    }

    // Реализация протокола
    ModbusRtuProtocol::ModbusRtuProtocol() {
    }

    void ModbusRtuProtocol::Init() {
    }

    void ModbusRtuProtocol::Execute() {
        std::lock_guard<std::mutex> lock(channels_mutex);
        for (auto& [name, channel] : channels) {
            if (channel) {
                channel->Execute();
            }
        }
    }

    api::ScadaChannel* ModbusRtuProtocol::Create(const vm::Channel* channel) {
        auto name = channel->getTranslitedName();
        ModbusRtuChannel* ch = new ModbusRtuChannel();
        ch->BaseInit(channel);
        Register(ch);
        
        std::lock_guard<std::mutex> lock(channels_mutex);
        channels[std::string(name)] = ch;
        
        return ch;
    }

}} // namespace mplc::modbus_rtu

// Регистрация типа протокола
MPLC_PROTOCOL_TYPE(ModbusRtuProtocol, mplc::modbus_rtu::ModbusRtuProtocol);
