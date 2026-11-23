#pragma once
#include <cmath>
namespace fmb {
// Observe defaults without issuing a command; hold the latest changed value until the gate opens.
struct OutputGate {
    bool seen{};
    double submitted{};
    bool take(double value, bool allow_writes, bool permit) {
        if (!std::isfinite(value)) return false;
        if (!seen || !allow_writes) { seen = true; submitted = value; return false; }
        if (!permit || value == submitted) return false;
        submitted = value;
        return true;
    }
};
}
