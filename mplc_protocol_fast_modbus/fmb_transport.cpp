#include "fmb_transport.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#endif
using Clock=std::chrono::steady_clock;
uint16_t FmbTransport::crc16(const uint8_t*p,size_t n){
    uint16_t c=65535;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=uint16_t((c>>1)^((c&1)?0xA001:0));}return c;
}
void FmbTransport::append_crc(fmb::Frame&f){auto c=crc16(f.data(),f.size());f.push_back(uint8_t(c));f.push_back(uint8_t(c>>8));}
bool FmbTransport::check_crc(const uint8_t*p,size_t n){return n>=3&&crc16(p,n-2)==uint16_t(p[n-2]|(uint16_t(p[n-1])<<8));}
void FmbTransport::sleep_ms(uint32_t n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
void FmbTransport::wait_t35(){std::this_thread::sleep_for(std::chrono::microseconds(t35_us_)+std::chrono::milliseconds(extra_delay_ms));}
bool FmbTransport::is_open()const{
#ifdef _WIN32
    return tcp_?socket_!=INVALID_SOCKET:handle_!=INVALID_HANDLE_VALUE;
#else
    return fd_>=0;
#endif
}
void FmbTransport::close(){
#ifdef _WIN32
    if(socket_!=INVALID_SOCKET){closesocket(socket_);socket_=INVALID_SOCKET;}
    if(handle_!=INVALID_HANDLE_VALUE){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}
#else
    if(fd_>=0){if(!tcp_){
#ifdef TIOCNXCL
        if(exclusive_owned_)ioctl(fd_,TIOCNXCL);
#endif
    }::close(fd_);fd_=-1;exclusive_owned_=false;}
#endif
}
bool FmbTransport::open_tcp(const std::string& text,uint32_t ms){
    auto colon=text.rfind(':');if(colon==std::string::npos)return false;
    std::string ip=text.substr(0,colon),port=text.substr(colon+1);
    if(port.empty()||port.size()>5||port.find_first_not_of("0123456789")!=std::string::npos)return false;
    unsigned pn=unsigned(std::stoul(port));if(!pn||pn>65535)return false;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(uint16_t(pn));
#ifdef _WIN32
    static const bool initialized=[](){WSADATA w{};return WSAStartup(MAKEWORD(2,2),&w)==0;}();
    if(!initialized||InetPtonA(AF_INET,ip.c_str(),&address.sin_addr)!=1)return false;
    socket_=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socket_==INVALID_SOCKET)return false;
    u_long nb=1;ioctlsocket(socket_,FIONBIO,&nb);
    int r=::connect(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(r==SOCKET_ERROR&&WSAGetLastError()!=WSAEWOULDBLOCK){close();return false;}
    auto fd=socket_;
#else
    if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1)return false;
    fd_=::socket(AF_INET,SOCK_STREAM,0);if(fd_<0)return false;
    if(fd_>=FD_SETSIZE){close();return false;}
    fcntl(fd_,F_SETFL,fcntl(fd_,F_GETFL,0)|O_NONBLOCK);
    int r=::connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(r<0&&errno!=EINPROGRESS){close();return false;}auto fd=fd_;
#endif
    fd_set w;FD_ZERO(&w);FD_SET(fd,&w);timeval tv{long(ms/1000),long(ms%1000*1000)};
    int selected=select(int(fd)+1,nullptr,&w,nullptr,&tv);
    int err=0;
#ifdef _WIN32
    int len=sizeof(err);getsockopt(fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&err),&len);
#else
    socklen_t len=sizeof(err);getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);
#endif
    if(selected<=0||err){close();return false;}return true;
}
bool FmbTransport::open(const std::string& name,int baud,int parity,int stops,int bits,int timeout){
    close();
    if(timeout<10||timeout>5000||baud<1200||baud>230400||parity<0||parity>2||
        (stops!=1&&stops!=2)||(bits!=7&&bits!=8))return false;
    timeout_ms_=uint32_t(timeout);t35_us_=std::max(1750u,38500000u/uint32_t(baud));
    tcp_=name.rfind("tcp://",0)==0;
    if(tcp_)return open_tcp(name.substr(6),timeout_ms_);
#ifdef _WIN32
    std::string port=name.rfind("\\\\.\\",0)==0?name:"\\\\.\\"+name;
    handle_=CreateFileA(port.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle_==INVALID_HANDLE_VALUE)return false;
    DCB d{};d.DCBlength=sizeof(d);if(!GetCommState(handle_,&d)){close();return false;}
    d.BaudRate=DWORD(baud);d.ByteSize=BYTE(bits);d.StopBits=stops==2?TWOSTOPBITS:ONESTOPBIT;
    d.Parity=parity==1?EVENPARITY:parity==2?ODDPARITY:NOPARITY;d.fParity=parity!=0;
    d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;
    d.fOutX=FALSE;d.fInX=FALSE;d.fBinary=TRUE;d.fNull=FALSE;d.fAbortOnError=FALSE;
    if(!SetCommState(handle_,&d)){close();return false;}
    COMMTIMEOUTS t{};t.ReadIntervalTimeout=MAXDWORD;t.ReadTotalTimeoutConstant=0;
    t.WriteTotalTimeoutConstant=timeout_ms_;if(!SetCommTimeouts(handle_,&t)){close();return false;}
    PurgeComm(handle_,PURGE_RXCLEAR|PURGE_TXCLEAR);return true;
#else
    fd_=::open(name.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK);if(fd_<0)return false;
    if(fd_>=FD_SETSIZE){close();return false;}
    if(flock(fd_,LOCK_EX|LOCK_NB)<0){close();return false;}
#ifdef TIOCEXCL
    if(ioctl(fd_,TIOCEXCL)<0){close();return false;}
    exclusive_owned_=true;
#endif
    speed_t speed;
    switch(baud){
    case 1200:speed=B1200;break;case 2400:speed=B2400;break;case 4800:speed=B4800;break;
    case 9600:speed=B9600;break;case 19200:speed=B19200;break;case 38400:speed=B38400;break;
    case 57600:speed=B57600;break;case 115200:speed=B115200;break;
#ifdef B230400
    case 230400:speed=B230400;break;
#endif
    default:close();return false;
    }
    termios t{};if(tcgetattr(fd_,&t)<0){close();return false;}cfmakeraw(&t);
    cfsetispeed(&t,speed);cfsetospeed(&t,speed);t.c_cflag|=CLOCAL|CREAD;t.c_cflag&=~(CSIZE|PARENB|PARODD|CSTOPB);
#ifdef CRTSCTS
    t.c_cflag&=~CRTSCTS;
#endif
    t.c_cflag|=bits==7?CS7:CS8;
    if(stops==2)t.c_cflag|=CSTOPB;
    if(parity){t.c_cflag|=PARENB;if(parity==2)t.c_cflag|=PARODD;t.c_iflag|=INPCK;}
    t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
    if(tcsetattr(fd_,TCSANOW,&t)<0){close();return false;}tcflush(fd_,TCIOFLUSH);return true;
#endif
}
int FmbTransport::read_some(uint8_t* p,size_t n,uint32_t ms){
    if(!is_open())return -1;
#ifdef _WIN32
    if(!tcp_){DWORD count=0;if(!ReadFile(handle_,p,DWORD(n),&count,nullptr)){close();return -1;}if(!count)sleep_ms(std::min(ms,1u));return int(count);}
    auto fd=socket_;
#else
    auto fd=fd_;
#endif
    fd_set r;FD_ZERO(&r);FD_SET(fd,&r);timeval tv{long(ms/1000),long(ms%1000*1000)};
    int ready=select(int(fd)+1,&r,nullptr,nullptr,&tv);if(ready==0)return 0;
    if(ready<0){close();return -1;}
#ifdef _WIN32
    int got=::recv(fd,reinterpret_cast<char*>(p),int(n),0);
    if(got==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return 0;
#else
    int got=int(tcp_ ? ::recv(fd,p,n,0) : ::read(fd,p,n));
    if(got<0&&(errno==EAGAIN||errno==EINTR))return 0;
#endif
    if(got<=0){close();return -1;}return got;
}
bool FmbTransport::send(const uint8_t*p,size_t n){
    if(!is_open()||!n||n>256)return false;
    auto end=Clock::now()+std::chrono::milliseconds(timeout_ms_);size_t sent=0;
    while(sent<n&&Clock::now()<end){
#ifdef _WIN32
        if(!tcp_){DWORD count=0;if(!WriteFile(handle_,p+sent,DWORD(n-sent),&count,nullptr)){close();return false;}sent+=count;continue;}
        int count=::send(socket_,reinterpret_cast<const char*>(p+sent),int(n-sent),0);
        if(count==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK){sleep_ms(1);continue;}
#else
        int count=int(tcp_ ? ::send(fd_,p+sent,n-sent,MSG_NOSIGNAL) : ::write(fd_,p+sent,n-sent));
        if(count<0&&(errno==EAGAIN||errno==EINTR)){sleep_ms(1);continue;}
#endif
        if(count<=0){close();return false;}sent+=size_t(count);
    }
    if(sent!=n){close();return false;}return true;
}
int FmbTransport::recv(uint8_t*p,size_t max,uint32_t timeout){
    size_t total=0;auto end=Clock::now()+std::chrono::milliseconds(timeout);
    while(Clock::now()<end){
        uint8_t b[256];int got=read_some(b,sizeof(b),1);if(got<0)return -1;
        for(int i=0;i<got;++i){
            if(!total&&b[i]==255)continue; // Arbitration pulses precede, never belong to, an RTU ADU.
            if(total>=max||total>=256)return -1;
            p[total++]=b[i];int needed=fmb::response_length(p,total);
            if(needed<0||needed>256)return -1;
            if(needed&&total==size_t(needed)){
                // Additional non-arbitration data in the same receive is unexpected contamination.
                if(i+1<got)return -1;return int(total);
            }
        }
    }
    // Never return a partial frame as successful.
    return 0;
}
void FmbTransport::flush_rx(){
    if(!is_open())return;
#ifdef _WIN32
    if(!tcp_){PurgeComm(handle_,PURGE_RXCLEAR);return;}
#else
    if(!tcp_){tcflush(fd_,TCIFLUSH);return;}
#endif
    uint8_t buf[256];for(unsigned i=0;i<8;++i){int r=read_some(buf,sizeof(buf),0);if(r<=0)break;}
}
fmb::Frame FmbTransport::exchange(const fmb::Frame&q,uint32_t timeout){
    wait_t35();flush_rx();if(!send(q))return {};
    uint8_t bytes[256]{};int n=recv(bytes,sizeof(bytes),timeout);
    return n>0?fmb::Frame(bytes,bytes+n):fmb::Frame{};
}
