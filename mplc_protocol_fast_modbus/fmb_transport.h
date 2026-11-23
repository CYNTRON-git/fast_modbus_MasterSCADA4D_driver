#pragma once
#include "fmb_core.h"
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <termios.h>
#endif
class FmbTransport final : public fmb::Bus {
public:
    ~FmbTransport(){close();}
    bool open(const std::string& port,int baud,int parity,int stop_bits,int data_bits,int response_timeout_ms);
    void close();
    bool is_open()const;
    bool send(const uint8_t*,size_t);
    bool send(const fmb::Frame& f){return send(f.data(),f.size());}
    int recv(uint8_t*,size_t,uint32_t timeout_ms);
    void flush_rx();
    void wait_t35();
    fmb::Frame exchange(const fmb::Frame&,uint32_t timeout_ms)override;
    uint32_t extra_delay_ms{};
    static void sleep_ms(uint32_t ms);
    static uint16_t crc16(const uint8_t*,size_t);
    static void append_crc(fmb::Frame&);
    static bool check_crc(const uint8_t*,size_t);
private:
    uint32_t t35_us_{1750},timeout_ms_{200};
    bool tcp_{},exclusive_owned_{};
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    SOCKET socket_{INVALID_SOCKET};
#else
    int fd_{-1};
#endif
    int read_some(uint8_t*,size_t,uint32_t);
    bool open_tcp(const std::string&,uint32_t);
};
