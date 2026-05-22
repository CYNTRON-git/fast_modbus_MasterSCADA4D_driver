#include "fmb_transport.h"
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#endif

// ---- CRC-16/MODBUS ----
static const uint16_t s_crc16_table[256] = {
    0x0000,0xC0C1,0xC181,0x0140,0xC301,0x03C0,0x0280,0xC241,
    0xC601,0x06C0,0x0780,0xC741,0x0500,0xC5C1,0xC481,0x0440,
    0xCC01,0x0CC0,0x0D80,0xCD41,0x0F00,0xCFC1,0xCE81,0x0E40,
    0x0A00,0xCAC1,0xCB81,0x0B40,0xC901,0x09C0,0x0880,0xC841,
    0xD801,0x18C0,0x1980,0xD941,0x1B00,0xDBC1,0xDA81,0x1A40,
    0x1E00,0xDEC1,0xDF81,0x1F40,0xDD01,0x1DC0,0x1C80,0xDC41,
    0x1400,0xD4C1,0xD581,0x1540,0xD701,0x17C0,0x1680,0xD641,
    0xD201,0x12C0,0x1380,0xD341,0x1100,0xD1C1,0xD081,0x1040,
    0xF001,0x30C0,0x3180,0xF141,0x3300,0xF3C1,0xF281,0x3240,
    0x3600,0xF6C1,0xF781,0x3740,0xF501,0x35C0,0x3480,0xF441,
    0x3C00,0xFCC1,0xFD81,0x3D40,0xFF01,0x3FC0,0x3E80,0xFE41,
    0xFA01,0x3AC0,0x3B80,0xFB41,0x3900,0xF9C1,0xF881,0x3840,
    0x2800,0xE8C1,0xE981,0x2940,0xEB01,0x2BC0,0x2A80,0xEA41,
    0xEE01,0x2EC0,0x2F80,0xEF41,0x2D00,0xEDC1,0xEC81,0x2C40,
    0xE401,0x24C0,0x2580,0xE541,0x2700,0xE7C1,0xE681,0x2640,
    0x2200,0xE2C1,0xE381,0x2340,0xE101,0x21C0,0x2080,0xE041,
    0xA001,0x60C0,0x6180,0xA141,0x6300,0xA3C1,0xA281,0x6240,
    0x6600,0xA6C1,0xA781,0x6740,0xA501,0x65C0,0x6480,0xA441,
    0x6C00,0xACC1,0xAD81,0x6D40,0xAF01,0x6FC0,0x6E80,0xAE41,
    0xAA01,0x6AC0,0x6B80,0xAB41,0x6900,0xA9C1,0xA881,0x6840,
    0x7800,0xB8C1,0xB981,0x7940,0xBB01,0x7BC0,0x7A80,0xBA41,
    0xBE01,0x7EC0,0x7F80,0xBF41,0x7D00,0xBDC1,0xBC81,0x7C40,
    0xB401,0x74C0,0x7580,0xB541,0x7700,0xB7C1,0xB681,0x7640,
    0x7200,0xB2C1,0xB381,0x7340,0xB101,0x71C0,0x7080,0xB041,
    0x5000,0x90C1,0x9181,0x5140,0x9301,0x53C0,0x5280,0x9241,
    0x9601,0x56C0,0x5780,0x9741,0x5500,0x95C1,0x9481,0x5440,
    0x9C01,0x5CC0,0x5D80,0x9D41,0x5F00,0x9FC1,0x9E81,0x5E40,
    0x5A00,0x9AC1,0x9B81,0x5B40,0x9901,0x59C0,0x5880,0x9841,
    0x8801,0x48C0,0x4980,0x8941,0x4B00,0x8BC1,0x8A81,0x4A40,
    0x4E00,0x8EC1,0x8F81,0x4F40,0x8D01,0x4DC0,0x4C80,0x8C41,
    0x4400,0x84C1,0x8581,0x4540,0x8701,0x47C0,0x4680,0x8641,
    0x8201,0x42C0,0x4380,0x8341,0x4100,0x81C1,0x8081,0x4040,
};

uint16_t FmbTransport::crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ s_crc16_table[(crc ^ data[i]) & 0xFF];
    return crc;
}

// ---- t3.5 helper ----
static uint32_t calc_t35_us(int baud)
{
    // Modbus RTU: 3.5 char times, 1 char = 11 bits
    // t3.5 [us] = (11 * 3.5 * 1e6) / baud = 38500000 / baud
    // Minimum 1750 us (at 115200: ~3.8 char times rounded to 2 ms min)
    uint32_t t = static_cast<uint32_t>(38500000u / static_cast<uint32_t>(baud > 0 ? baud : 9600));
    return (t < 1750u) ? 1750u : t;
}

#ifdef _WIN32
// ============================================================
//  Windows implementation
// ============================================================

bool FmbTransport::open(const std::string& port, int baud, int parity,
                        int stop_bits, int data_bits, int response_timeout_ms)
{
    close();
    m_baud = baud;
    m_response_timeout_ms = response_timeout_ms;
    m_t35_us = calc_t35_us(baud);

    std::string p = (port.rfind("\\\\.\\", 0) == 0) ? port : "\\\\.\\" + port;
    m_handle = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) return false;

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb)) { close(); return false; }

    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = static_cast<BYTE>(data_bits > 0 ? data_bits : 8);
    dcb.StopBits = (stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    switch (parity) {
        case 1: dcb.Parity = EVENPARITY; dcb.fParity = TRUE;  break;
        case 2: dcb.Parity = ODDPARITY;  dcb.fParity = TRUE;  break;
        default:dcb.Parity = NOPARITY;   dcb.fParity = FALSE; break;
    }
    dcb.fDtrControl    = DTR_CONTROL_DISABLE;
    dcb.fRtsControl    = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow   = FALSE;
    dcb.fOutxDsrFlow   = FALSE;
    dcb.fBinary        = TRUE;
    dcb.fNull          = FALSE;
    if (!SetCommState(m_handle, &dcb)) { close(); return false; }

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = 5;    // ms between chars
    to.ReadTotalTimeoutMultiplier  = 2;
    to.ReadTotalTimeoutConstant    = static_cast<DWORD>(response_timeout_ms);
    to.WriteTotalTimeoutConstant   = 200;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(m_handle, &to);

    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

void FmbTransport::close()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

bool FmbTransport::is_open() const { return m_handle != INVALID_HANDLE_VALUE; }

bool FmbTransport::send(const uint8_t* data, size_t len)
{
    if (!is_open()) return false;
    DWORD written = 0;
    return WriteFile(m_handle, data, static_cast<DWORD>(len), &written, nullptr)
           && written == static_cast<DWORD>(len);
}

int FmbTransport::recv(uint8_t* buf, size_t max_bytes, uint32_t timeout_ms)
{
    if (!is_open()) return -1;
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = 5;
    to.ReadTotalTimeoutMultiplier  = 2;
    to.ReadTotalTimeoutConstant    = static_cast<DWORD>(timeout_ms);
    SetCommTimeouts(m_handle, &to);

    DWORD got = 0;
    if (!ReadFile(m_handle, buf, static_cast<DWORD>(max_bytes), &got, nullptr))
        return -1;
    return static_cast<int>(got);
}

void FmbTransport::flush_rx()
{
    if (is_open()) PurgeComm(m_handle, PURGE_RXCLEAR);
}

void FmbTransport::wait_t35()
{
    // Windows: Sleep in ms (round up)
    DWORD ms = (m_t35_us + 999u) / 1000u;
    Sleep(ms ? ms : 1);
}

#else
// ============================================================
//  Linux / POSIX implementation
// ============================================================
#include <time.h>

static speed_t to_speed(int baud)
{
    switch (baud) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default:     return B9600;
    }
}

bool FmbTransport::open(const std::string& port, int baud, int parity,
                        int stop_bits, int data_bits, int response_timeout_ms)
{
    close();
    m_baud = baud;
    m_response_timeout_ms = response_timeout_ms;
    m_t35_us = calc_t35_us(baud);

    m_fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) return false;

    // Set blocking mode
    int flags = fcntl(m_fd, F_GETFL, 0);
    fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio{};
    tcgetattr(m_fd, &tio);
    cfmakeraw(&tio);

    speed_t spd = to_speed(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSIZE;
    switch (data_bits) {
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }
    if (stop_bits == 2) tio.c_cflag |= CSTOPB;
    else tio.c_cflag &= ~CSTOPB;

    tio.c_cflag &= ~PARENB;
    tio.c_iflag &= ~(INPCK | ISTRIP);
    if (parity == 1) { // even
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        tio.c_iflag |= INPCK;
    } else if (parity == 2) { // odd
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
    }

    // VMIN=0, VTIME in deciseconds (select-based timeout done manually)
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(m_fd, TCSANOW, &tio) != 0) { close(); return false; }
    tcflush(m_fd, TCIOFLUSH);
    return true;
}

void FmbTransport::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool FmbTransport::is_open() const { return m_fd >= 0; }

bool FmbTransport::send(const uint8_t* data, size_t len)
{
    if (!is_open()) return false;
    ssize_t w = ::write(m_fd, data, len);
    return w == static_cast<ssize_t>(len);
}

int FmbTransport::recv(uint8_t* buf, size_t max_bytes, uint32_t timeout_ms)
{
    if (!is_open()) return -1;

    size_t total = 0;
    uint32_t deadline_ms = timeout_ms;

    while (total < max_bytes && deadline_ms > 0) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(m_fd, &rset);
        struct timeval tv;
        tv.tv_sec  = deadline_ms / 1000u;
        tv.tv_usec = (deadline_ms % 1000u) * 1000u;

        int rc = select(m_fd + 1, &rset, nullptr, nullptr, &tv);
        if (rc <= 0) break; // timeout or error

        ssize_t r = ::read(m_fd, buf + total, max_bytes - total);
        if (r <= 0) break;
        total += static_cast<size_t>(r);

        // After first byte: inter-character timeout (5 ms)
        deadline_ms = 5;
    }
    return static_cast<int>(total);
}

void FmbTransport::flush_rx()
{
    if (is_open()) tcflush(m_fd, TCIFLUSH);
}

void FmbTransport::wait_t35()
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = static_cast<long>(m_t35_us) * 1000L;
    nanosleep(&ts, nullptr);
}

#endif // _WIN32

// ---- Platform-portable sleep ----
void FmbTransport::sleep_ms(uint32_t ms)
{
    if (ms == 0) return;
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = static_cast<long>(ms % 1000u) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}
