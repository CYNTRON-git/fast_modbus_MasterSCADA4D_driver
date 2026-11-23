#include "fmb_core.h"
#include "fmb_transport.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace fmb {
static FmbEventFilter::tp tp(uint64_t ms){return FmbEventFilter::tp(std::chrono::milliseconds(ms));}
bool Channel::check()const{
    return valid&&type<=3&&priority<=2&&unsigned(value_type)<=4&&
        std::isfinite(scale)&&scale!=0&&std::isfinite(offset)&&
        std::isfinite(filter.deadband)&&filter.deadband>=0&&uint32_t(reg)+words()<=65536;
}
void Channel::publish(double raw,uint64_t now,Source source){
    double physical=raw*scale+offset;
    if(!std::isfinite(physical)){bad();return;}
    bool recover=!good;initialized=true;good=true;value=physical;
    if(recover)filter.reset();
    if(filter.should_commit(physical,tp(now))){
        if(sink)sink(physical,true,source);filter.mark_committed(physical,tp(now));
    }
}
void Channel::bad(){
    filter.has_pending=false;
    if(good||!initialized){good=false;if(sink)sink(value,false,Source::Quality);}
}
bool Engine::validate(){
    bool ok=true;std::set<unsigned> addresses;
    for(auto*d:devices){
        d->valid=d->valid&&d->addr>=1&&d->addr<=247&&(!d->use_serial||d->serial)&&!d->channels.empty();
        // Duplicate addresses cannot safely participate in events, even with distinct serials.
        if(!addresses.insert(d->addr).second){
            for(auto*other:devices)if(other->addr==d->addr)other->valid=false;
            ok=false;
        }
        for(auto*c:d->channels)d->valid=d->valid&&c->check();
        // Two subscribers to the same value must agree on event priority and width.
        for(size_t i=0;i<d->channels.size();++i)for(size_t j=i+1;j<d->channels.size();++j){
            auto*a=d->channels[i];auto*b=d->channels[j];
            if(a->type==b->type&&a->reg==b->reg&&(a->priority!=b->priority||a->words()!=b->words()))d->valid=false;
        }
        ok=ok&&d->valid;
    }
    if(options.event_ms<10||options.poll_ms<50||options.health_ms<50||options.stale_ms<=options.health_ms||
       options.response_ms<10||options.response_ms>5000||options.max_transactions<2||options.max_transactions>64||
       options.max_cycle_ms<1||options.max_cycle_ms>5000)ok=false;
    error=ok?"":"Invalid settings or duplicate Modbus address";
    return ok;
}
void Engine::reset_device(Device&d){
    d.flag_valid=false;d.last_batch.clear();d.no_fmb=false;d.contact=false;d.next_health=0;d.cursor=0;
    for(auto*c:d.channels){
        c->bad();c->initialized=false;c->config_done=false;c->event_accepted=false;c->write_pending=false;
        c->next_read=c->next_config=c->next_write=0;c->filter.reset();
    }
}
void Engine::reset(){
    ack_slave_=ack_flag_=0;min_slave_=1;next_events_=0;cursor_=0;
    for(auto*d:devices)reset_device(*d);
}
void Engine::set_fast(bool enabled){
    if(options.fast==enabled)return;
    options.fast=enabled;
    // Re-enable must rebuild volatile slave subscriptions; do not retain old ACKs.
    reset();
}
Frame Engine::transact(const Frame&q,Device*d){
    if(q.empty())return {};
    Frame request=q;
    if(d&&d->use_serial&&q[1]!=FMB_FUNC)request=build_by_serial_request(d->serial,Frame(q.begin()+1,q.end()-2));
    ++counters.requests;
    auto r=bus_.exchange(request,options.response_ms);
    if(r.empty()){++counters.timeouts;return {};}
    if(r.size()>256||!FmbTransport::check_crc(r.data(),r.size())){++counters.bad_frames;return {};}
    ++counters.responses;
    if(d&&d->use_serial&&q[1]!=FMB_FUNC){
        Frame inner;if(!unwrap_serial(r,d->serial,d->addr,inner)){++counters.bad_frames;return {};}return inner;
    }
    return r;
}
void Engine::heard(Device&d,uint64_t now){
    d.contact=true;d.last_reply=now;
    if(d.was_stale){
        d.was_stale=false;d.no_fmb=false;d.flag_valid=false;
        for(auto*c:d.channels){c->initialized=false;c->config_done=false;c->event_accepted=false;c->next_config=0;c->filter.reset();}
    }
}
bool Engine::read(Device&d,Channel&c,uint64_t now,Source source){
    c.next_read=now+options.poll_ms;++counters.reads;
    uint8_t fc=uint8_t(c.type+1);auto q=build_read_regs(d.addr,fc,c.reg,uint16_t(c.words()));
    auto r=transact(q,&d);std::vector<uint16_t> values;
    if(!validate_read(r,d.addr,fc,uint16_t(c.words()),values)){c.bad();return false;}
    double raw=0;
    if(c.type<2)raw=values[0];
    else {
        uint8_t data[4]{};for(size_t i=0;i<values.size();++i){data[i*2]=uint8_t(values[i]>>8);data[i*2+1]=uint8_t(values[i]);}
        if(!decode_value(data,values.size()*2,false,c.value_type,c.low_word_first,raw)){c.bad();return false;}
    }
    heard(d,now);c.publish(raw,now,source);return c.good;
}
void Engine::configure(Device&d,Channel&c,uint64_t now){
    std::vector<Channel*> group{&c};uint16_t base=c.reg,last=uint16_t(c.reg+c.words()-1);
    // Opt-in for a reviewed sparse capability map (e.g. MR 12AI 403,410,...480).
    // Holes receive DISABLED; never enable this for an unknown device map.
    if(d.group_event_ranges&&c.words()==1&&c.priority>0){
        for(auto*x:d.channels)if(x!=&c&&x->type==c.type&&x->priority==c.priority&&x->words()==1&&!x->config_done){
            auto lo=std::min(base,x->reg),hi=std::max(last,x->reg);
            if(unsigned(hi)-lo<246){base=lo;last=hi;group.push_back(x);}
        }
    }
    uint8_t count=uint8_t(last-base+1);
    auto q=build_event_config(d.addr,c.type,base,count,0);
    for(auto*x:group){x->next_config=now+5000;q[8+x->reg-base]=x->priority;}
    q.resize(q.size()-2);FmbTransport::append_crc(q);
    auto r=transact(q);uint8_t ex=0;
    if(exception(r,d.addr,70,ex)&&ex==1){
        d.no_fmb=true;for(auto*x:d.channels){x->config_done=true;x->event_accepted=false;}
        heard(d,now);return;
    }
    if(exception(r,d.addr,70,ex)&&(ex==2||ex==3)){
        heard(d,now);for(auto*x:group){x->config_done=true;x->event_accepted=false;if(x->priority>0)++counters.rejected_subscriptions;}return;
    }
    int mask=parse_config_response(r,d.addr,count);
    if(mask<0)return; // Timeout/CRC is not proof that the device lacks FMB.
    heard(d,now);
    for(auto*x:group){
        unsigned index=x->reg-base;x->config_done=true;x->event_accepted=x->priority>0&&(r[4+index/8]&(1u<<(index%8)));
        if(x->priority>0&&!x->event_accepted)++counters.rejected_subscriptions;
    }
}
bool Engine::queue_write(Device&d,Channel&c,double physical){
    if(!options.allow_writes||!d.active||!d.valid||!c.check()||(c.type!=0&&c.type!=2)||!std::isfinite(physical))return false;
    double raw=(physical-c.offset)/c.scale;std::vector<uint16_t> words;
    if(c.type==0){if(raw!=0&&raw!=1)return false;}
    else if(!encode_value(raw,c.value_type,c.low_word_first,words))return false;
    c.command=raw;c.write_pending=true;c.write_failed=false;c.write_attempts=0;c.next_write=0;return true;
}
void Engine::write(Device&d,Channel&c,uint64_t now){
    Frame q;std::vector<uint16_t> words;
    if(c.type==0)q=build_write_coil(d.addr,c.reg,c.command!=0);
    else {
        if(!encode_value(c.command,c.value_type,c.low_word_first,words)){c.write_pending=false;c.write_failed=true;return;}
        q=words.size()==1?build_write_reg(d.addr,c.reg,words[0]):build_write_regs(d.addr,c.reg,words.data(),uint8_t(words.size()));
    }
    ++counters.writes;auto r=transact(q,&d);
    if(validate_write(q,r)){
        c.write_pending=false;c.write_failed=false;heard(d,now);
        // Never publish the command as observed work/status; read back the actual register.
        c.initialized=false;c.next_read=now;
    }else{
        ++counters.write_errors;c.write_failed=true;c.bad();c.next_write=now+options.retry_ms;
        if(++c.write_attempts>=3)c.write_pending=false;
    }
}
bool Engine::step(Device&d,uint64_t now){
    if(!d.active||!d.valid)return false;
    // Heartbeat continues even for constant event-only channels, detects loss of a silent slave.
    if(d.contact&&now>=d.next_health){
        d.next_health=now+options.health_ms;read(d,*d.channels.front(),now,Source::Heartbeat);return true;
    }
    for(size_t i=0;i<d.channels.size();++i){
        auto*c=d.channels[d.cursor++%d.channels.size()];
        if(c->write_pending&&now>=c->next_write){write(d,*c,now);return true;}
        if(!c->initialized&&now>=c->next_read){read(d,*c,now,Source::Initial);return true;}
        if(options.fast&&d.enable_events&&!d.no_fmb&&!c->config_done&&now>=c->next_config){configure(d,*c,now);return true;}
        if((!options.fast||!d.enable_events||!c->event_accepted||!c->good)&&now>=c->next_read){read(d,*c,now,Source::Poll);return true;}
    }
    return false;
}
void Engine::poll_events(uint64_t now){
    auto r=transact(build_event_request(min_slave_,248,ack_slave_,ack_flag_));
    if(r.empty())return; // Keep confirmation for retry; ACK loss must not lose the batch.
    std::vector<FmbEvent> events;uint8_t slave=0,flag=0;
    int result=parse_event_response(r.data(),r.size(),events,slave,flag);
    if(result<0){++counters.bad_frames;return;}
    if(!result){min_slave_=1;ack_slave_=ack_flag_=0;return;}
    ++counters.event_packets;
    // An empty payload is not a received batch: do not acknowledge undelivered entries.
    if(events.empty()){min_slave_=slave==247?1:uint8_t(slave+1);return;}
    Device* target=nullptr;
    for(auto*d:devices)if(d->addr==slave&&d->active&&d->valid&&d->enable_events){target=d;break;}
    if(target){
        auto&d=*target;heard(d,now);
        // A reboot can reuse the previous flag. Suppress only the same actual batch.
        Frame batch(r.begin()+6,r.end()-2);
        if(d.flag_valid&&d.last_flag==flag&&d.last_batch==batch)++counters.duplicates;
        else{
            for(const auto&e:events){
                if(e.type==15){
                    for(auto*c:d.channels){c->bad();c->initialized=false;c->config_done=false;c->event_accepted=false;c->next_read=c->next_config=0;}
                    d.no_fmb=false;continue;
                }
                for(auto*c:d.channels)if(c->type==e.type&&c->reg==e.reg_addr){
                    double raw=0;
                    bool ok=c->type<2?e.payload_len==1:decode_value(e.payload,e.payload_len,true,c->value_type,false,raw);
                    if(c->type<2)raw=e.payload[0];
                    if(!ok){c->bad();continue;}
                    c->publish(raw,now,Source::Event);++counters.event_values;
                }
            }
            d.flag_valid=true;d.last_flag=flag;d.last_batch=std::move(batch);
        }
    }
    // Consume a structurally valid batch, including unknown events, exactly once.
    ack_slave_=slave;ack_flag_=flag;
    min_slave_=slave==247?1:uint8_t(slave+1); // Fairness: hot low-address devices cannot starve higher addresses.
}
void Engine::scan(){
    auto r=transact(scan_first_?build_scan_start():build_scan_next());scan_first_=false;
    if(r.empty()||scan_finished(r.data(),r.size())){scan_active_=false;return;}
    FmbScanItem item;
    if(!parse_scan_response(r.data(),r.size(),item)||++scan_count_>247){scan_active_=false;++counters.bad_frames;return;}
    ++counters.scan_devices;
    // Discovery is diagnostic only: never silently change a configured address/identity.
    for(auto*d:devices)if(d->addr==item.modbus_addr&&d->serial&&d->serial!=item.serial){d->valid=false;error="Scanned serial does not match configured device";}
}
void Engine::report(Device&d,uint64_t now){
    if(d.contact&&now-d.last_reply>=options.stale_ms){
        d.contact=false;d.was_stale=true;
        for(auto*c:d.channels)c->bad();
    }
    bool bad=!d.active||!d.valid||!d.contact;
    for(auto*c:d.channels){bad=bad||!c->good||c->write_failed;double pending;
        if(d.active&&d.contact&&c->good&&c->filter.try_flush_pending(tp(now),pending)){
            if(c->sink)c->sink(pending,true,Source::Pending);c->filter.mark_committed(pending,tp(now));
        }
    }
    if(d.status)d.status(bad,!d.active?"Module disabled":!d.valid?"Invalid module settings":!d.contact?"No verified response":bad?"Channel or write error":"");
}
void Engine::tick(uint64_t now){
    if(!error.empty()&&devices.empty())return;
    auto started=std::chrono::steady_clock::now();unsigned used=0;
    for(auto*d:devices){
        if(!options.allow_writes)for(auto*c:d->channels)c->write_pending=false;
        if(!d->active){if(!d->suspended){reset_device(*d);d->suspended=true;}}
        else if(d->suspended){reset_device(*d);d->suspended=false;}
    }
    bool any=false;for(auto*d:devices)any=any||(d->active&&d->valid&&d->enable_events&&!d->no_fmb);
    if(options.fast&&any&&now>=next_events_){poll_events(now);next_events_=now+options.event_ms;++used;}
    if(scan_active_&&used<options.max_transactions){scan();++used;}
    size_t idle=0;
    while(!devices.empty()&&used<options.max_transactions&&idle<devices.size()){
        if(std::chrono::steady_clock::now()-started>=std::chrono::milliseconds(options.max_cycle_ms))break;
        auto*d=devices[cursor_++%devices.size()];
        if(step(*d,now)){++used;idle=0;}else ++idle;
    }
    for(auto*d:devices)report(*d,now);
}
bool Engine::fault()const{
    if(!error.empty()||devices.empty())return true;
    for(auto*d:devices)if(d->active&&d->valid&&d->contact){
        bool good=true;for(auto*c:d->channels)good=good&&c->good&&!c->write_failed;
        if(good)return false;
    }
    return true;
}
}
