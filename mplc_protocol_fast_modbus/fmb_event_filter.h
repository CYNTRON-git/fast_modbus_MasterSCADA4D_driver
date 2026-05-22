#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>

// Per-channel anti-spam / rate-limit state machine.
// All time points use steady_clock.
struct FmbEventFilter {
    // ---- Configuration (set once from channel settings) ----
    double   deadband{0.0};         // Minimum absolute change to pass through (0 = any change)
    uint32_t min_interval_ms{0};    // Minimum time between committed values (0 = no limit)
    uint32_t burst_window_ms{0};    // Burst throttle window width in ms (0 = disabled)
    uint16_t burst_max_events{0};   // Max events allowed per burst window (0 = disabled)

    // ---- State ----
    double   last_committed{0.0};   // Last value committed to MS4
    bool     has_last_committed{false};
    double   pending_value{0.0};    // Buffered value waiting for min_interval to elapse
    bool     has_pending{false};

    using clock = std::chrono::steady_clock;
    using tp    = clock::time_point;

    tp last_commit_time{};
    tp burst_window_start{};
    uint16_t burst_count{0};

    // Decide whether value should be committed immediately.
    // Returns true  тЖТ commit immediately.
    // Returns false тЖТ either filter blocked or stored as pending.
    // Sets pending_value when blocked only by min_interval.
    bool should_commit(double value, tp now)
    {
        // Deadband check
        if (deadband > 0.0 && has_last_committed) {
            if (std::fabs(value - last_committed) < deadband)
                return false;
        }

        // Burst throttle check
        if (burst_window_ms > 0 && burst_max_events > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - burst_window_start).count();
            if (static_cast<uint32_t>(elapsed) >= burst_window_ms) {
                burst_window_start = now;
                burst_count = 0;
            }
            if (burst_count >= burst_max_events) {
                // Throttled тАФ store as pending
                pending_value = value;
                has_pending   = true;
                return false;
            }
        }

        // Min-interval check
        if (min_interval_ms > 0 && has_last_committed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - last_commit_time).count();
            if (static_cast<uint32_t>(elapsed) < min_interval_ms) {
                // Too soon тАФ store as pending
                pending_value = value;
                has_pending   = true;
                return false;
            }
        }

        return true;
    }

    // Called when value is actually committed тАФ update state.
    void mark_committed(double value, tp now)
    {
        last_committed     = value;
        has_last_committed = true;
        last_commit_time   = now;
        has_pending        = false;
        if (burst_max_events > 0) ++burst_count;
    }

    // If there is a pending value and throttle conditions have relaxed, pop it.
    // Returns true and sets out_value if flush should happen.
    bool try_flush_pending(tp now, double& out_value)
    {
        if (!has_pending) return false;

        // Burst window: check if window has expired since it was opened
        if (burst_window_ms > 0 && burst_max_events > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - burst_window_start).count();
            if (static_cast<uint32_t>(elapsed) >= burst_window_ms) {
                // Window expired тАФ reset counter, allow flush
                burst_window_start = now;
                burst_count = 0;
            } else if (burst_count >= burst_max_events) {
                return false; // still throttled within window
            }
        }

        // Min-interval: check if enough time has passed since last commit
        if (min_interval_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - last_commit_time).count();
            if (static_cast<uint32_t>(elapsed) < min_interval_ms)
                return false;
        }

        out_value   = pending_value;
        has_pending = false;
        return true;
    }
};
