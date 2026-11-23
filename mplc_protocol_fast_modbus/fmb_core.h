#pragma once
#include "fmb_frames.h"
#include "fmb_event_filter.h"
#include <functional>
#include <string>
#include <vector>
namespace fmb {
enum class Source { Initial, Poll, Event, Heartbeat, Quality, Pending };
struct Channel {
    uint8_t type{3},priority{2};
    uint16_t reg{};
    ValueType value_type{ValueType::I16};
    bool low_word_first{},valid{true},initialized{},good{},config_done{},event_accepted{};
    bool write_pending{},write_failed{};
    unsigned write_attempts{};
    uint64_t next_read{},next_config{},next_write{};
    double scale{1},offset{},value{},command{};
    FmbEventFilter filter;
    std::function<void(double,bool,Source)> sink;
    size_t words()const{return type<2?1:value_words(value_type);}
    bool check()const;
    void publish(double raw,uint64_t now,Source source);
    void bad();
};
struct Device {
    uint8_t addr{1};
    uint32_t serial{};
    bool use_serial{},group_event_ranges{},enable_events{true},active{true},suspended{},valid{true},contact{},no_fmb{},was_stale{};
    bool flag_valid{};uint8_t last_flag{};
    Frame last_batch;
    uint64_t last_reply{},next_health{};
    size_t cursor{};
    std::vector<Channel*> channels;
    std::function<void(bool,const std::string&)> status;
};
struct Counters {
    uint64_t requests{},responses{},bad_frames{},timeouts{},event_packets{},event_values{},duplicates{},reads{},writes{},write_errors{},rejected_subscriptions{},scan_devices{};
};
struct Options {
    bool fast{true},allow_writes{false};
    uint32_t response_ms{200},event_ms{50},poll_ms{1000},health_ms{5000},stale_ms{15000},retry_ms{1000};
    unsigned max_transactions{8},max_cycle_ms{100};
};
struct Bus {
    virtual ~Bus()=default;
    virtual Frame exchange(const Frame&,uint32_t timeout_ms)=0;
};
class Engine {
public:
    explicit Engine(Bus& bus):bus_(bus){}
    Options options;
    Counters counters;
    std::vector<Device*> devices;
    bool validate();
    void reset();
    void set_fast(bool enabled);
    void tick(uint64_t now);
    bool queue_write(Device&,Channel&,double physical);
    bool fault()const;
    std::string error;
    void start_scan(){scan_active_=true;scan_first_=true;scan_count_=0;}
private:
    Bus& bus_;
    size_t cursor_{};
    uint64_t next_events_{};
    uint8_t ack_slave_{},ack_flag_{},min_slave_{1};
    bool scan_active_{},scan_first_{};
    unsigned scan_count_{};
    Frame transact(const Frame&,Device* device=nullptr);
    bool step(Device&,uint64_t now);
    bool read(Device&,Channel&,uint64_t now,Source);
    void configure(Device&,Channel&,uint64_t now);
    void poll_events(uint64_t now);
    void scan();
    void write(Device&,Channel&,uint64_t now);
    void heard(Device&,uint64_t now);
    void reset_device(Device&);
    void report(Device&,uint64_t now);
};
}
