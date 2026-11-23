#pragma once
// SDK conversions can successfully coerce an empty Variant to zero/false.
// Test absence explicitly when a setting has a compatibility fallback.
// Native exports also omit scalar zero values: numeric channel defaults must
// remain zero, and new optional booleans must use a positive opt-in/opt-out.
template<class Node, class Value>
Value fmb_setting(const Node* node, const char* name, Value fallback) {
    const auto value = node->get(name);
    return value.IsNull() ? fallback : value.template Get<Value>(fallback);
}
