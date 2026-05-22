#pragma once
#include "fmb_defs.h"
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <termios.h>
#endif

// OS-abstracted RS-485 serial transport.
// NOT thread-safe тАФ use from a single Execute() loop.
class FmbTransport {
public:
    FmbTransport() = default;
    ~FmbTransport() { close(); }
    FmbTransport(const FmbTransport&) = delete;
    FmbTransport& operator=(const FmbTransport&) = delete;

    // Open serial port. Returns true on success.
    bool open(const std::string& port, int baud, int parity /*0=N,1=E,2=O*/,
              int stop_bits, int data_bits, int response_timeout_ms);
    void close();
    bool is_open() const;

    // Send frame (raw bytes, CRC must already be appended by caller)
    bool send(const uint8_t* data, size_t len);
    bool send(const std::vector<uint8_t>& frame) { return send(frame.data(), frame.size()); }

    // Receive up to max_bytes within timeout_ms. Returns bytes read, 0 on timeout, <0 on error.
    int  recv(uint8_t* buf, size_t max_bytes, uint32_t timeout_ms);

    // Flush RX buffer (discard stale data before sending a new request)
    void flush_rx();

    // Inter-frame silence тЙе 3.5 char times at current baud rate
    void wait_t35();

    // Platform-portable sleep (used for InterFrameDelayMs)
    static void sleep_ms(uint32_t ms);

    // CRC-16/MODBUS (poly 0xA001, init 0xFFFF)
    static uint16_t crc16(const uint8_t* data, size_t len);
    static uint16_t crc16(const std::vector<uint8_t>& v) { return crc16(v.data(), v.size()); }

    // Append CRC16 to vector
    static void append_crc(std::vector<uint8_t>& frame) {
        uint16_t crc = crc16(frame.data(), frame.size());
        frame.push_back(static_cast<uint8_t>(crc & 0xFF));
        frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    }

    // Validate CRC at end of buffer (last 2 bytes are CRC LSB,MSB)
    static bool check_crc(const uint8_t* data, size_t len) {
        if (len < 3) return false;
        uint16_t calc = crc16(data, len - 2);
        uint16_t got  = static_cast<uint16_t>(data[len-2]) | (static_cast<uint16_t>(data[len-1]) << 8);
        return calc == got;
    }

private:
    int  m_baud{9600};
    int  m_response_timeout_ms{200};
    // t3.5 in microseconds (derived from baud)
    uint32_t m_t35_us{2000};

#ifdef _WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    int m_fd{-1};
#endif
};
