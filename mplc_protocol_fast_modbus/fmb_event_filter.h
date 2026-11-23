#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
struct FmbEventFilter {
    double deadband{},last_committed{},pending_value{};
    uint32_t min_interval_ms{},burst_window_ms{},burst_max_events{},burst_count{};
    bool has_last_committed{},has_pending{};
    using clock=std::chrono::steady_clock;using tp=clock::time_point;
    tp last_commit_time{},burst_window_start{};
    void reset(){has_last_committed=has_pending=false;burst_count=0;last_commit_time={};burst_window_start={};}
    void rotate(tp now){
        if(burst_window_ms && now-burst_window_start>=std::chrono::milliseconds(burst_window_ms)){
            burst_window_start=now;burst_count=0;
        }
    }
    bool permitted(tp now){
        rotate(now);
        return !(burst_window_ms&&burst_max_events&&burst_count>=burst_max_events) &&
            !(min_interval_ms&&has_last_committed&&now-last_commit_time<std::chrono::milliseconds(min_interval_ms));
    }
    bool should_commit(double v,tp now){
        if(!std::isfinite(v))return false;
        // Latest sample wins: returning to the committed value cancels an older pending excursion.
        if(has_last_committed&&(v==last_committed||(deadband>0&&std::fabs(v-last_committed)<deadband))){
            has_pending=false;return false;
        }
        if(!permitted(now)){pending_value=v;has_pending=true;return false;}
        return true;
    }
    void mark_committed(double v,tp now){
        last_committed=v;has_last_committed=true;last_commit_time=now;has_pending=false;
        if(burst_max_events&&burst_count<UINT32_MAX)++burst_count;
    }
    bool try_flush_pending(tp now,double& v){
        if(!has_pending||!permitted(now))return false;
        v=pending_value;has_pending=false;return true;
    }
};
