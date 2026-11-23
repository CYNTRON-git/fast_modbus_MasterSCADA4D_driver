#include "fmb_core.h"
#include "fmb_transport.h"
#include "fmb_output_gate.h"
#include <iostream>
#include <stdexcept>
#include <random>
#include <map>
#include <deque>
#include <cstring>
using namespace fmb;
static int checks=0,groups=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__func__)+":"+std::to_string(__LINE__)+" "+#x);}while(0)
static Frame seal(Frame f){FmbTransport::append_crc(f);return f;}
static Frame event(uint8_t addr,uint8_t flag,uint8_t type,uint16_t reg,Frame value,uint8_t pending=1){
    Frame f{addr,70,17,flag,pending,uint8_t(4+value.size()),uint8_t(value.size()),type,uint8_t(reg>>8),uint8_t(reg)};
    f.insert(f.end(),value.begin(),value.end());return seal(f);
}
struct FakeBus:Bus{
    std::vector<Frame> requests;
    std::deque<Frame> events;
    std::map<unsigned,uint16_t> values;
    bool offline{},wrong_write{},no_config{},unsupported{},no_fmb{},config_full{};
    unsigned writes{},configs{},reads{};
    Frame exchange(const Frame&q,uint32_t)override{
        requests.push_back(q);if(offline)return {};
        auto body=[this](const Frame&q)->Frame {
            if(q[1]==70&&q[2]==16){if(events.empty())return seal({253,70,18});auto r=events.front();events.pop_front();return r;}
            if(q[1]==70&&q[2]==24){++configs;if(no_config)return {};if(no_fmb)return seal({q[0],198,1});if(config_full)return seal({q[0],198,3});
                Frame r{q[0],70,24,uint8_t((q[7]+7)/8)};r.resize(4+r[3]);
                for(unsigned i=0;i<q[7];++i)if(!unsupported&&q[8+i])r[4+i/8]|=uint8_t(1u<<(i%8));return seal(r);}
            if(q[1]>=1&&q[1]<=4){
                ++reads;unsigned addr=unsigned(q[2])*256+q[3],count=unsigned(q[4])*256+q[5];
                Frame r{q[0],q[1],uint8_t(q[1]<3?(count+7)/8:count*2)};
                if(q[1]<3)r.push_back(uint8_t(values[addr]&1));
                else for(unsigned i=0;i<count;++i){auto v=values.count(addr+i)?values[addr+i]:uint16_t(42);r.push_back(uint8_t(v>>8));r.push_back(uint8_t(v));}
                return seal(r);
            }
            if(q[1]==5||q[1]==6||q[1]==16){
                ++writes;Frame r(q.begin(),q.begin()+6);if(wrong_write)r[5]^=1;
                else {unsigned a=unsigned(q[2])*256+q[3];if(q[1]==6)values[a]=uint16_t(q[4]*256+q[5]);else if(q[1]==5)values[a]=q[4]?1:0;
                    else for(unsigned i=0;i<q[5];++i)values[a+i]=uint16_t(q[7+2*i]*256+q[8+2*i]);}
                return seal(r);
            }
            if(q[1]==70&&q[2]==1)return seal({253,70,3,14,10,97,169,12});
            if(q[1]==70&&q[2]==2)return seal({253,70,4});
            return {};
        };
        if(q[1]==70&&q[2]==8){
            Frame inner{12};inner.insert(inner.end(),q.begin()+7,q.end()-2);FmbTransport::append_crc(inner);
            auto r=body(inner);Frame wrapped(q.begin(),q.begin()+7);wrapped[2]=9;wrapped.insert(wrapped.end(),r.begin()+1,r.end()-2);return seal(wrapped);
        }
        return body(q);
    }
};
struct Fixture{
    FakeBus bus;Engine engine{bus};Device dev;Channel channel;
    unsigned published{},event_values{},bad_values{};double last{};bool good{};
    Fixture(){
        dev.addr=12;channel.type=3;channel.reg=403;channel.value_type=ValueType::U16;
        channel.sink=[this](double v,bool g,Source s){last=v;good=g;++published;if(s==Source::Event)++event_values;if(!g)++bad_values;};
        dev.channels={&channel};engine.devices={&dev};CHECK(engine.validate());engine.reset();
    }
    void tick(uint64_t n){engine.tick(n);}
};
static void codecs(){
    auto q=build_event_config(12,3,403,1,2);
    CHECK(q[4]==4&&q[3]==5&&q[8]==2&&FmbTransport::check_crc(q.data(),q.size()));
    auto q32=build_event_config(12,3,105,2,2);
    CHECK(q32[3]==6&&q32[7]==2&&q32[8]==2&&q32[9]==0);
    CHECK(build_event_config(12,4,0,1,1).empty());
    CHECK(parse_config_response(seal({12,70,24,1,1}),12,1)==1);
    CHECK(parse_config_response(seal({12,70,24,1,0}),12,1)==0);
    CHECK(parse_config_response(seal({13,70,24,1,1}),12,1)==-1);
    auto f=event(12,85,4,403,{0x34,0x12},3);
    std::vector<FmbEvent> ev;uint8_t a=0,flag=0;
    CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==1&&ev.size()==1&&a==12&&flag==85);
    CHECK(ev[0].type==3&&ev[0].reg_addr==403&&ev[0].payload_len==2);
    double v=0;CHECK(decode_value(ev[0].payload,2,true,ValueType::U16,false,v)&&v==4660);
    auto end=seal({253,70,18});CHECK(parse_event_response(end.data(),end.size(),ev,a,flag)==0&&a==0);
    auto legacy=seal({12,70,18,85});CHECK(parse_event_response(legacy.data(),legacy.size(),ev,a,flag)==-1);
    auto unknown=event(12,1,0x99,0,{1,2,3});CHECK(parse_event_response(unknown.data(),unknown.size(),ev,a,flag)==1&&ev[0].type==255);
    auto reboot=event(12,2,15,0,{});CHECK(parse_event_response(reboot.data(),reboot.size(),ev,a,flag)==1&&ev[0].type==15);
    f=event(12,3,4,403,{1,2});f[5]=20;f.resize(f.size()-2);FmbTransport::append_crc(f);
    CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==-1&&ev.empty()&&a==0);
    f=event(12,3,4,403,{1,2},0);CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==-1);
    f=event(12,3,4,403,{1});CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==-1);
    f=event(12,3,2,18,{2});CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==-1);
    f=event(12,3,4,403,{1,2});f.back()^=1;CHECK(parse_event_response(f.data(),f.size(),ev,a,flag)==-1);
    FmbScanItem scan;auto s=seal({253,70,3,14,10,97,169,12});
    CHECK(parse_scan_response(s.data(),s.size(),scan)&&scan.serial==0x0E0A61A9&&scan.modbus_addr==12);
    auto se=seal({253,70,4});CHECK(scan_finished(se.data(),se.size()));
    CHECK(build_event_request(1,255)[4]==248);
}
static void values(){
    double v=0;uint8_t neg[]{0xF6,0xFF};CHECK(decode_value(neg,2,true,ValueType::I16,false,v)&&v==-10);
    CHECK(decode_value(neg,2,true,ValueType::U16,false,v)&&v==65526);
    uint8_t up[]{0x78,0x56,0x34,0x12};CHECK(decode_value(up,4,true,ValueType::U32,false,v)&&v==305419896);
    uint8_t minus[]{0xFE,0xFF,0xFF,0xFF};CHECK(decode_value(minus,4,true,ValueType::I32,false,v)&&v==-2);
    uint8_t real[]{0,0,0xC0,0x3F};CHECK(decode_value(real,4,true,ValueType::F32,false,v)&&v==1.5);
    uint8_t nan[]{0,0,0xC0,0x7F};CHECK(!decode_value(nan,4,true,ValueType::F32,false,v));
    uint8_t swapped[]{0x56,0x78,0x12,0x34};CHECK(decode_value(swapped,4,false,ValueType::U32,true,v)&&v==305419896);
    std::vector<uint16_t>w;CHECK(encode_value(-10,ValueType::I16,false,w)&&w[0]==65526);
    CHECK(!encode_value(-1,ValueType::U16,false,w)&&w.empty());
    CHECK(!encode_value(65536,ValueType::U16,false,w));CHECK(!encode_value(32768,ValueType::I16,false,w));
    CHECK(encode_value(4294967295.0,ValueType::U32,false,w)&&w[0]==65535&&w[1]==65535);
    CHECK(!encode_value(4294967296.0,ValueType::U32,false,w));
    CHECK(encode_value(1.5,ValueType::F32,false,w)&&w[0]==0x3FC0&&w[1]==0);
    CHECK(encode_value(305419896,ValueType::U32,true,w)&&w[0]==0x5678&&w[1]==0x1234);
}
static void response_validation(){
    std::vector<uint16_t>v;auto r=seal({12,4,2,0,42});
    CHECK(validate_read(r,12,4,1,v)&&v[0]==42);
    CHECK(!validate_read(r,13,4,1,v));CHECK(!validate_read(r,12,3,1,v));CHECK(!validate_read(r,12,4,2,v));
    CHECK(parse_read_response(seal({12,6,2,0,42}).data(),7).empty());
    auto q=build_write_reg(12,33,7);CHECK(validate_write(q,q));
    auto wrong=build_write_reg(12,33,8);CHECK(!validate_write(q,wrong));
    uint16_t nums[]{7,8};auto multi=build_write_regs(12,33,nums,2);
    auto echo=seal({12,16,0,33,0,2});CHECK(validate_write(multi,echo));
    auto wrapped=seal({253,70,9,14,10,97,169,4,2,0,42});Frame inner;
    CHECK(unwrap_serial(wrapped,0x0E0A61A9,12,inner)&&validate_read(inner,12,4,1,v));
    CHECK(!unwrap_serial(wrapped,1,12,inner));
    CHECK(response_length(r.data(),2)==0&&response_length(r.data(),3)==7);
    CHECK(response_length(wrapped.data(),wrapped.size())==int(wrapped.size()));
}
static void filters(){
    FmbEventFilter f;f.deadband=1;f.min_interval_ms=100;
    auto t=FmbEventFilter::tp(std::chrono::milliseconds(1000));
    CHECK(f.should_commit(0,t));f.mark_committed(0,t);
    CHECK(!f.should_commit(10,t+std::chrono::milliseconds(10))&&f.has_pending);
    CHECK(!f.should_commit(0.5,t+std::chrono::milliseconds(20))&&!f.has_pending);
    double v;CHECK(!f.try_flush_pending(t+std::chrono::milliseconds(120),v));
    CHECK(!f.should_commit(20,t+std::chrono::milliseconds(30)));
    CHECK(f.try_flush_pending(t+std::chrono::milliseconds(120),v)&&v==20);
    f.mark_committed(v,t+std::chrono::milliseconds(120));
    f.reset();f.burst_window_ms=1000;f.burst_max_events=1;
    CHECK(f.should_commit(1,t));f.mark_committed(1,t);
    CHECK(!f.should_commit(2,t+std::chrono::milliseconds(200)));
    CHECK(!f.try_flush_pending(t+std::chrono::milliseconds(900),v));
    CHECK(f.try_flush_pending(t+std::chrono::milliseconds(1000),v)&&v==2);
}
static void engine_initial_and_events(){
    Fixture f;f.tick(100);CHECK(f.channel.initialized&&f.channel.good&&f.last==42&&f.channel.event_accepted&&!f.engine.fault());
    unsigned reads=f.bus.reads;f.bus.events.push_back(event(12,1,4,403,{77,0}));f.tick(200);
    CHECK(f.last==77&&f.event_values==1&&f.bus.reads==reads);
    f.bus.events.push_back(event(12,1,4,403,{77,0}));f.tick(300);
    CHECK(f.event_values==1&&f.engine.counters.duplicates==1);
    f.bus.events.push_back(event(12,2,4,403,{78,0}));f.tick(400);CHECK(f.last==78&&f.event_values==2);
    CHECK(f.bus.requests.back()[2]==16); // idle cycles send only events.
    f.tick(500);CHECK(!f.engine.fault());
}
static void fallback_and_retries(){
    Fixture f;f.bus.unsupported=true;f.tick(100);CHECK(f.channel.config_done&&!f.channel.event_accepted&&f.engine.counters.rejected_subscriptions==1);
    f.bus.values[403]=99;f.tick(1200);CHECK(f.last==99);
    Fixture slow;slow.bus.no_config=true;slow.tick(100);CHECK(!slow.dev.no_fmb&&!slow.channel.config_done&&slow.channel.good);
    slow.bus.no_config=false;slow.tick(5200);CHECK(slow.channel.config_done&&slow.channel.event_accepted);
    Fixture generic;generic.bus.no_fmb=true;generic.tick(100);CHECK(generic.dev.no_fmb&&generic.channel.good);
    unsigned cfg=generic.bus.configs;generic.tick(2000);CHECK(generic.bus.configs==cfg);
    Fixture disabled;disabled.channel.priority=0;disabled.tick(100);CHECK(disabled.channel.config_done&&!disabled.channel.event_accepted&&disabled.engine.counters.rejected_subscriptions==0);
}
static void loss_and_recovery(){
    Fixture f;f.tick(100);f.bus.offline=true;f.tick(5200);CHECK(!f.good);
    f.tick(16000);CHECK(f.engine.fault()&&!f.dev.contact);
    f.bus.offline=false;f.bus.values[403]=88;f.tick(17000);f.tick(17100);
    CHECK(f.good&&f.last==88&&!f.engine.fault()&&f.channel.event_accepted);
    f.dev.active=false;unsigned n=unsigned(f.bus.requests.size());f.tick(18000);CHECK(f.bus.requests.size()==n&&!f.good);
    f.dev.active=true;f.tick(19000);CHECK(f.good);
}
static void write_commands(){
    Fixture f;f.channel.type=2;f.channel.value_type=ValueType::I16;f.channel.scale=0.1;f.tick(100);
    CHECK(!f.engine.queue_write(f.dev,f.channel,-1)&&f.bus.writes==0);
    f.engine.options.allow_writes=true;
    CHECK(f.engine.queue_write(f.dev,f.channel,-1));f.tick(200);
    CHECK(f.bus.writes==1&&f.bus.values[403]==65526&&f.last==-1&&!f.channel.write_pending);
    f.bus.wrong_write=true;CHECK(f.engine.queue_write(f.dev,f.channel,2));f.tick(300);
    CHECK(f.channel.write_pending&&f.channel.write_failed);
    f.tick(1400);f.tick(2500);CHECK(!f.channel.write_pending&&f.channel.write_failed&&f.bus.writes==4);
    f.bus.wrong_write=false;CHECK(f.engine.queue_write(f.dev,f.channel,3));f.tick(2600);CHECK(!f.channel.write_failed&&f.last==3);
    CHECK(!f.engine.queue_write(f.dev,f.channel,4000));
    f.bus.wrong_write=true;CHECK(f.engine.queue_write(f.dev,f.channel,4));f.tick(2700);
    unsigned before=f.bus.writes;CHECK(f.channel.write_pending);
    f.engine.options.allow_writes=false;f.tick(4000);
    CHECK(!f.channel.write_pending&&f.bus.writes==before);
    f.engine.options.allow_writes=true;f.tick(5000);CHECK(f.bus.writes==before);
}
static void multiword_and_serial(){
    Fixture f;f.channel.reg=105;f.channel.value_type=ValueType::U32;f.bus.values[105]=1;f.bus.values[106]=2;
    f.dev.use_serial=true;f.dev.serial=0x0E0A61A9;f.tick(100);
    CHECK(f.last==65538&&f.good);bool wrapper=false;for(auto&q:f.bus.requests)wrapper=wrapper||(q[1]==70&&q[2]==8);CHECK(wrapper);
    f.bus.events.push_back(event(12,1,4,105,{3,0,1,0}));f.tick(200);CHECK(f.last==65539&&f.event_values==1);
}
static void validation_and_budget(){
    Fixture f;f.channel.scale=0;CHECK(!f.engine.validate());
    Fixture d;Device other;Channel ch;other.addr=12;other.channels={&ch};d.engine.devices.push_back(&other);CHECK(!d.engine.validate()&&!d.dev.valid&&!other.valid);
    Fixture budget;budget.engine.options.max_transactions=1;CHECK(!budget.engine.validate());
    budget.engine.options.max_transactions=2;CHECK(budget.engine.validate());budget.tick(100);CHECK(budget.bus.requests.size()==2);
    budget.tick(101);CHECK(budget.bus.requests.size()<=4);
}
static void sparse_subscriptions(){
    Fixture f;f.dev.group_event_ranges=true;std::vector<Channel> extra(11);
    for(unsigned i=0;i<extra.size();++i){extra[i].type=3;extra[i].reg=uint16_t(410+7*i);extra[i].value_type=ValueType::U16;f.dev.channels.push_back(&extra[i]);}
    for(unsigned i=0;i<8;++i)f.tick(100+i*100);
    CHECK(f.bus.configs==1);for(auto*c:f.dev.channels)CHECK(c->initialized&&c->event_accepted&&c->good);
    auto it=std::find_if(f.bus.requests.begin(),f.bus.requests.end(),[](const Frame&q){return q[1]==70&&q[2]==24;});
    CHECK(it!=f.bus.requests.end()&&(*it)[7]==78&&(*it)[8]==2&&(*it)[9]==0&&(*it)[85]==2);
    Fixture full;full.bus.config_full=true;full.tick(100);CHECK(full.channel.config_done&&!full.channel.event_accepted&&full.channel.good);
    auto configs=full.bus.configs;full.tick(6000);CHECK(full.bus.configs==configs&&full.channel.good);
}
static void reboot_and_scan(){
    Fixture f;f.tick(100);unsigned configs=f.bus.configs;
    f.bus.events.push_back(event(12,1,4,403,{77,0}));f.tick(150);
    f.bus.events.push_back(event(12,1,15,0,{}));f.tick(200);
    CHECK(f.bus.configs>configs&&f.good);
    configs=f.bus.configs;f.bus.events.push_back(event(12,1,15,0,{}));f.tick(250);
    CHECK(f.bus.configs==configs&&f.engine.counters.duplicates==1);
    f.engine.start_scan();f.tick(300);f.tick(400);CHECK(f.engine.counters.scan_devices==1);
    for(auto&q:f.bus.requests)CHECK(!(q[0]==253&&q[1]==70&&q[2]==4));
    f.engine.set_fast(false);f.tick(500);CHECK(!f.channel.event_accepted);
    f.engine.set_fast(true);f.tick(600);CHECK(f.channel.event_accepted&&f.bus.configs>configs);
}
static void mixed_classic_and_events(){
    Fixture f;Device classic;Channel c;classic.addr=1;classic.enable_events=false;
    c.type=2;c.reg=16;c.value_type=ValueType::I16;c.priority=2;classic.channels={&c};
    f.bus.values[16]=65526;f.engine.devices.push_back(&classic);CHECK(f.engine.validate());f.engine.reset();
    for(unsigned i=0;i<10;++i)f.tick(100+i*100);
    CHECK(c.good&&c.value==-10&&!c.event_accepted&&!c.config_done);
    CHECK(f.channel.event_accepted&&f.good);
    for(auto&q:f.bus.requests)CHECK(!(q[0]==1&&q[1]==70));
    f.bus.values[16]=20;f.bus.events.push_back(event(12,1,4,403,{77,0}));f.tick(2100);
    CHECK(c.good&&c.value==20&&f.event_values==1&&f.last==77);
    auto seen=f.engine.counters.event_values;
    f.bus.events.push_back(event(1,1,3,16,{99,0}));f.tick(2200);
    CHECK(c.value==20&&f.engine.counters.event_values==seen);
    f.engine.set_fast(false);f.tick(2300);f.engine.set_fast(true);f.tick(2400);
    f.engine.reset();f.tick(2500);
    for(auto&q:f.bus.requests)CHECK(!(q[0]==1&&q[1]==70));
    FakeBus bus;Engine engine(bus);Device only;Channel input;only.enable_events=false;input.reg=1;
    only.channels={&input};engine.devices={&only};CHECK(engine.validate());engine.reset();engine.tick(100);
    CHECK(input.good&&bus.configs==0);
    for(auto&q:bus.requests)CHECK(q[1]>=1&&q[1]<=4);
}
static void output_gate(){
    OutputGate gate;
    CHECK(!gate.take(0,true,true)); // Startup default is not an operator command.
    CHECK(!gate.take(1,true,false));
    CHECK(!gate.take(2,true,false));
    CHECK(gate.take(2,true,true)); // Latest change survives a closed condition.
    CHECK(!gate.take(2,true,true));
    CHECK(!gate.take(3,false,false));
    CHECK(!gate.take(3,true,true)); // Global disable discards stale commands.
    CHECK(!gate.take(4,true,false));
    CHECK(!gate.take(3,true,true)); // Returning to submitted value cancels an excursion.
    CHECK(!gate.take(std::numeric_limits<double>::quiet_NaN(),true,true));
    CHECK(gate.take(4,true,true));
}
static void fuzz_parser(){
    std::mt19937 rng(20261122);std::vector<FmbEvent>ev;uint8_t a,b;
    for(unsigned trial=0;trial<100000;++trial){
        Frame f(rng()%257);for(auto&x:f)x=uint8_t(rng());
        if(f.size()>=3&&trial%2){f.resize(f.size()-2);FmbTransport::append_crc(f);}
        int r=parse_event_response(f.data(),f.size(),ev,a,b);
        CHECK(r>=-1&&r<=1);
        if(r<0)CHECK(ev.empty()&&a==0&&b==0);
        if(!f.empty())response_length(f.data(),f.size());
    }
}
int main(int argc,char**argv){
    if(argc==3&&std::string(argv[1])=="--transport"){
        FmbTransport bus;if(!bus.open(argv[2],115200,0,1,8,200)){std::cerr<<"OPEN FAILED\n";return 2;}
        auto r=bus.exchange(build_read_regs(12,4,403,1),200);std::vector<uint16_t>v;
        if(!validate_read(r,12,4,1,v)){std::cerr<<"READ FAILED\n";return 3;}std::cout<<"VALUE "<<v[0]<<"\n";return 0;
    }
    try{
        for(auto test:{codecs,values,response_validation,filters,engine_initial_and_events,fallback_and_retries,loss_and_recovery,write_commands,multiword_and_serial,validation_and_budget,sparse_subscriptions,reboot_and_scan,mixed_classic_and_events,output_gate,fuzz_parser}){test();++groups;}
        std::cout<<"PASS "<<groups<<" groups "<<checks<<" assertions\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" assertions\n";return 1;}
}
