#include "fmb_frames.h"
#include "fmb_transport.h"
#include <cmath>
#include <cstring>
#include <limits>
namespace fmb {
static void u16(Frame& f,uint16_t v){f.push_back(uint8_t(v>>8));f.push_back(uint8_t(v));}
static void u32(Frame& f,uint32_t v){u16(f,uint16_t(v>>16));u16(f,uint16_t(v));}
static uint16_t r16(const uint8_t*p){return uint16_t((uint16_t(p[0])<<8)|p[1]);}
static uint32_t r32(const uint8_t*p){return (uint32_t(r16(p))<<16)|r16(p+2);}
static Frame seal(Frame f){FmbTransport::append_crc(f);return f;}
Frame build_read_regs(uint8_t a,uint8_t fc,uint16_t r,uint16_t n){
    if(a<1||a>247||fc<1||fc>4||!n||n>((fc<3)?2000:125)||uint32_t(r)+n>65536)return {};
    Frame f{a,fc};u16(f,r);u16(f,n);return seal(f);
}
Frame build_write_reg(uint8_t a,uint16_t r,uint16_t v){Frame f{a,6};u16(f,r);u16(f,v);return seal(f);}
Frame build_write_coil(uint8_t a,uint16_t r,bool v){Frame f{a,5};u16(f,r);u16(f,v?0xFF00:0);return seal(f);}
Frame build_write_regs(uint8_t a,uint16_t r,const uint16_t*v,uint8_t n){
    if(a<1||a>247||!n||n>123||uint32_t(r)+n>65536||!v)return {};
    Frame f{a,16};u16(f,r);u16(f,n);f.push_back(uint8_t(n*2));for(unsigned i=0;i<n;++i)u16(f,v[i]);return seal(f);
}
Frame build_scan_start(){return seal({253,70,1});}
Frame build_scan_next(){return seal({253,70,2});}
Frame build_event_request(uint8_t m,uint8_t n,uint8_t a,uint8_t f){return seal({253,70,16,m,uint8_t(n>248?248:n),a,f});}
Frame build_event_config(uint8_t a,uint8_t t,uint16_t r,uint8_t n,uint8_t p){
    if(a<1||a>247||(t>3&&t!=15)||!n||n>246||p>2||uint32_t(r)+n>65536)return {};
    Frame f{a,70,24,uint8_t(4+n),uint8_t(t==15?15:t+1)};u16(f,r);f.push_back(n);
    // One channel = one value; subsequent words have no separate subscription.
    for(unsigned i=0;i<n;++i)f.push_back(i?0:p);
    return seal(f);
}
Frame build_by_serial_request(uint32_t s,const Frame& p){
    if(!s||p.empty()||p.size()>247)return {};
    Frame f{253,70,8};u32(f,s);f.insert(f.end(),p.begin(),p.end());return seal(f);
}
bool unwrap_serial(const Frame& f,uint32_t s,uint8_t a,Frame& inner){
    inner.clear();if(f.size()<10||f.size()>256||f[0]!=253||f[1]!=70||f[2]!=9||r32(f.data()+3)!=s||!FmbTransport::check_crc(f.data(),f.size()))return false;
    inner.push_back(a);inner.insert(inner.end(),f.begin()+7,f.end()-2);FmbTransport::append_crc(inner);return true;
}
bool parse_scan_response(const uint8_t*f,size_t n,FmbScanItem& out){
    if(n!=10||f[0]!=253||(f[1]!=70&&f[1]!=96)||f[2]!=3||f[7]<1||f[7]>247||!FmbTransport::check_crc(f,n))return false;
    out.serial=r32(f+3);out.modbus_addr=f[7];return true;
}
bool scan_finished(const uint8_t*f,size_t n){return n==5&&f[0]==253&&(f[1]==70||f[1]==96)&&f[2]==4&&FmbTransport::check_crc(f,n);}
int parse_event_response(const uint8_t*f,size_t n,std::vector<FmbEvent>& out,uint8_t& as,uint8_t& af){
    out.clear();as=af=0;
    if(n<5||n>256||f[1]!=70||!FmbTransport::check_crc(f,n))return -1;
    if(f[2]==18)return n==5&&f[0]==253?0:-1;
    if(f[2]!=17||n<8||f[0]<1||f[0]>247||size_t(f[5])+8!=n)return -1;
    std::vector<FmbEvent> parsed;
    size_t p=6,end=n-2;
    while(p<end){
        if(end-p<4)return -1;
        FmbEvent e{};e.slave_id=f[0];e.payload_len=f[p];const auto wire=f[p+1];e.reg_addr=r16(f+p+2);p+=4;
        if(e.payload_len>end-p)return -1;
        e.type=(wire>=1&&wire<=4)?uint8_t(wire-1):(wire==15?15:255);
        if((e.type<=1&&(e.payload_len!=1||f[p]>1))||
           ((e.type==2||e.type==3)&&e.payload_len!=2&&e.payload_len!=4)||
           (e.type==15&&(e.payload_len!=0||e.reg_addr!=0)))return -1;
        std::memcpy(e.payload,f+p,e.payload_len);p+=e.payload_len;parsed.push_back(e);
    }
    // N is all pending events, not the number of entries in this payload.
    if(parsed.size()>f[4])return -1;
    out.swap(parsed);as=f[0];af=f[3];return 1;
}
bool exception(const Frame&f,uint8_t a,uint8_t fc,uint8_t& code){
    if(f.size()!=5||f[0]!=a||f[1]!=(fc|128)||!FmbTransport::check_crc(f.data(),f.size()))return false;
    code=f[2];return true;
}
std::vector<uint16_t> parse_read_response(const uint8_t*f,size_t n){
    std::vector<uint16_t> out;
    if(n<5||n>256||f[0]<1||f[0]>247||f[1]<1||f[1]>4||size_t(f[2])+5!=n||!FmbTransport::check_crc(f,n))return out;
    if(f[1]<3){for(size_t i=0;i<f[2];++i)for(unsigned b=0;b<8;++b)out.push_back((f[3+i]>>b)&1);}
    else {if(f[2]%2)return out;for(size_t i=0;i<f[2];i+=2)out.push_back(r16(f+3+i));}
    return out;
}
bool validate_read(const Frame&f,uint8_t a,uint8_t fc,uint16_t n,std::vector<uint16_t>& out){
    out.clear();if(f.size()<5||f[0]!=a||f[1]!=fc||!n)return false;
    size_t bytes=fc<3?(n+7)/8:n*2;if(f[2]!=bytes)return false;
    auto v=parse_read_response(f.data(),f.size());if(v.size()<n)return false;v.resize(n);out.swap(v);return true;
}
bool validate_write(const Frame&q,const Frame&r){
    if(q.size()<8||r.size()!=8||r[0]!=q[0]||r[1]!=q[1]||!FmbTransport::check_crc(r.data(),r.size()))return false;
    return (q[1]==5||q[1]==6||q[1]==16||q[1]==15)&&std::memcmp(q.data(),r.data(),6)==0;
}
int parse_config_response(const Frame&r,uint8_t a,uint8_t n){
    size_t masks=(n+7)/8;
    if(!n||r.size()!=masks+6||r[0]!=a||r[1]!=70||r[2]!=24||r[3]!=masks||!FmbTransport::check_crc(r.data(),r.size()))return -1;
    return (r[4]&1)?1:0;
}
size_t value_words(ValueType t){return unsigned(t)>=2?2:1;}
bool decode_value(const uint8_t*b,size_t n,bool le,ValueType t,bool low,double& out){
    if(unsigned(t)>4||n!=2*value_words(t))return false;
    uint32_t u=0;
    if(le){for(size_t i=0;i<n;++i)u|=uint32_t(b[i])<<(8*i);}
    else {for(size_t i=0;i<n;++i)u=(u<<8)|b[i];if(n==4&&low)u=(u<<16)|(u>>16);}
    switch(t){
    case ValueType::U16:out=uint16_t(u);break;
    case ValueType::I16:out=(u&0x8000)?double(u)-65536.0:double(u);break;
    case ValueType::U32:out=u;break;
    case ValueType::I32:out=(u&0x80000000)?double(u)-4294967296.0:double(u);break;
    case ValueType::F32:{float v;std::memcpy(&v,&u,4);out=v;break;}
    }
    return std::isfinite(out);
}
bool encode_value(double v,ValueType t,bool low,std::vector<uint16_t>& out){
    out.clear();if(!std::isfinite(v)||unsigned(t)>4)return false;
    uint32_t u=0;
    if(t==ValueType::F32){if(std::fabs(v)>std::numeric_limits<float>::max())return false;float f=float(v);std::memcpy(&u,&f,4);}
    else {
        v=std::round(v);double lo=0,hi=65535;
        if(t==ValueType::I16){lo=-32768;hi=32767;}
        if(t==ValueType::U32)hi=4294967295.0;
        if(t==ValueType::I32){lo=-2147483648.0;hi=2147483647;}
        if(v<lo||v>hi)return false;
        u=v<0?uint32_t(int64_t(v)):uint32_t(v);
    }
    if(value_words(t)==1){out.push_back(uint16_t(u));}
    else if(low){out.push_back(uint16_t(u));out.push_back(uint16_t(u>>16));}
    else {out.push_back(uint16_t(u>>16));out.push_back(uint16_t(u));}
    return true;
}
int response_length(const uint8_t*b,size_t n){
    if(n<2)return 0;
    if(b[1]&128)return 5;
    if(b[1]>=1&&b[1]<=4)return n<3?0:int(b[2])+5;
    if(b[1]==5||b[1]==6||b[1]==15||b[1]==16)return 8;
    if(b[1]!=70&&b[1]!=96)return -1;
    if(n<3)return 0;
    switch(b[2]){
    case 3:return 10;
    case 4:case 18:return 5;
    case 17:return n<6?0:int(b[5])+8;
    case 24:return n<4?0:int(b[3])+6;
    case 9:{
        if(n<8)return 0;uint8_t fc=b[7];
        if(fc&128)return 11;
        if(fc>=1&&fc<=4)return n<9?0:int(b[8])+11;
        if(fc==5||fc==6||fc==15||fc==16)return 14;
        return -1;
    }
    default:return -1;
    }
}
}
