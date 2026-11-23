#include "fast_modbus_protocol.h"
#include <mplc/vm/node_typese.h>
#include <chrono>
static uint64_t now_ms(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
FastModbusProtocol::~FastModbusProtocol(){
    transport_.close();for(auto*m:modules_){Unregister(m);delete m;}
}
mplc::api::ScadaModule* FastModbusProtocol::Create(const mplc::vm::IOModule* node){
    auto*m=new FastModbusDeviceModule();m->BaseInit(node,this);Register(m);modules_.push_back(m);engine_.devices.push_back(&m->state);return m;
}
void FastModbusProtocol::configure(){
    auto&o=engine_.options;
    o.fast=EnableFastModbus;o.allow_writes=AllowWrites;o.response_ms=uint32_t(ResponseTimeoutMs);
    o.event_ms=uint32_t(EventPollIntervalMs);o.poll_ms=uint32_t(FallbackPollPeriodMs);
    o.health_ms=uint32_t(HealthPollPeriodMs);o.stale_ms=uint32_t(DeviceTimeoutMs);
    o.max_transactions=unsigned(MaxTransactionsPerCycle);o.max_cycle_ms=unsigned(MaxCycleMs);
    config_ok_=engine_.validate()&&EventPollIntervalMs>=10&&FallbackPollPeriodMs>=50&&HealthPollPeriodMs>=50&&
        DeviceTimeoutMs>HealthPollPeriodMs&&BaudRate>=1200&&BaudRate<=230400&&Parity>=0&&Parity<=2&&
        (StopBits==1||StopBits==2)&&(DataBits==7||DataBits==8)&&InterFrameDelayMs>=0&&InterFrameDelayMs<=1000;
    transport_.extra_delay_ms=uint32_t(InterFrameDelayMs);configured_=true;engine_.reset();
}
void FastModbusProtocol::Init(){configure();}
void FastModbusProtocol::Execute(){
    if(!configured_)configure();
    if(!isConnect()||!isExecute()){
        transport_.close();engine_.reset();UpdateFaultState(true,"Protocol suspended");return;
    }
    if(!config_ok_){UpdateFaultState(true,"Invalid FastModbus settings");return;}
    auto now=now_ms();
    if(!transport_.is_open()){
        if(now<reconnect_at_)return;
        reconnect_at_=now+1000;
        if(!transport_.open(PortName,int(BaudRate),int(Parity),int(StopBits),int(DataBits),int(ResponseTimeoutMs))){
            UpdateFaultState(true,"Cannot open FastModbus port");engine_.reset();return;
        }
        engine_.reset();if(AutoScan)engine_.start_scan();
    }
    engine_.set_fast(EnableFastModbus);engine_.options.allow_writes=AllowWrites;
    bool write_permit=isWrite();
    for(auto*m:modules_){
        m->state.active=m->isConnect()&&m->isExecute();
        if(m->state.active)m->collect_writes(engine_,write_permit);
    }
    engine_.tick(now);
    // Runtime marks untouched channel buffers uncertain each cycle. Event values remain valid
    // until the engine's verified device contact expires; do not bypass its per-channel errors.
    for(auto*m:modules_)for(auto*c:m->channels)if(c->InVar)
        c->InVar.SetQuality(c->state.good&&m->state.active&&m->state.contact?OpcUa_Good:OpcUa_BadCommunicationError);
    UpdateFaultState(engine_.fault(),engine_.fault()?"FastModbus has no healthy device":"");
    diagnostics();
    AcknowledgeAllChanges();
}
void FastModbusProtocol::diagnostics(){
    const auto&c=engine_.counters;
    Requests=LINT(c.requests);Responses=LINT(c.responses);Timeouts=LINT(c.timeouts);BadFrames=LINT(c.bad_frames);
    EventPackets=LINT(c.event_packets);EventValues=LINT(c.event_values);DuplicatePackets=LINT(c.duplicates);
    ReadRequests=LINT(c.reads);WriteRequests=LINT(c.writes);WriteErrors=LINT(c.write_errors);
    RejectedSubscriptions=LINT(c.rejected_subscriptions);ScannedDevices=LINT(c.scan_devices);
}
