// Minimal streaming JSON writer used by the command-line tools (not part of the library API).
#pragma once

#include <cmath>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::tools {

class JsonWriter {
public:
    explicit JsonWriter(std::ostream& os, bool pretty = true) : os_(os), pretty_(pretty) {}

    JsonWriter& begin_object() { value_prefix(); os_ << '{'; stack_.push_back({true, true}); return *this; }
    JsonWriter& end_object() { close('}'); return *this; }
    JsonWriter& begin_array() { value_prefix(); os_ << '['; stack_.push_back({false, true}); return *this; }
    JsonWriter& end_array() { close(']'); return *this; }

    JsonWriter& key(std::string_view k) {
        auto& f = stack_.back();
        if (!f.first) os_ << ',';
        f.first = false;
        newline();
        string(k);
        os_ << (pretty_ ? ": " : ":");
        after_key_ = true;
        return *this;
    }
    JsonWriter& value(std::string_view s) { value_prefix(); string(s); return *this; }
    JsonWriter& value(const char* s) { return value(std::string_view(s)); }
    JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
    JsonWriter& value(bool b) { value_prefix(); os_ << (b ? "true" : "false"); return *this; }
    JsonWriter& value(double d) {
        value_prefix();
        if (!std::isfinite(d)) { os_ << "null"; return *this; }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", d);
        os_ << buf;
        return *this;
    }
    JsonWriter& value(float f) { return value(static_cast<double>(f)); }
    JsonWriter& value(long long v) { value_prefix(); os_ << v; return *this; }
    JsonWriter& value(unsigned long long v) { value_prefix(); os_ << v; return *this; }
    JsonWriter& value(int v) { return value(static_cast<long long>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<unsigned long long>(v)); }
    JsonWriter& value(long v) { return value(static_cast<long long>(v)); }
    JsonWriter& value(unsigned long v) { return value(static_cast<unsigned long long>(v)); }
    JsonWriter& null() { value_prefix(); os_ << "null"; return *this; }
    template <class T> JsonWriter& kv(std::string_view k, const T& v) { key(k); return value(v); }
    JsonWriter& floats(std::initializer_list<float> fs) {
        // compact inline array
        value_prefix();
        os_ << '[';
        bool first = true;
        for (float f : fs) {
            if (!first) os_ << ',';
            first = false;
            char buf[32];
            if (!std::isfinite(f)) std::snprintf(buf, sizeof buf, "null");
            else std::snprintf(buf, sizeof buf, "%.7g", static_cast<double>(f));
            os_ << buf;
        }
        os_ << ']';
        return *this;
    }

private:
    struct Frame { bool object; bool first; };
    void value_prefix() {
        if (after_key_) { after_key_ = false; return; }
        if (stack_.empty()) return;
        auto& f = stack_.back();
        if (!f.first) os_ << ',';
        f.first = false;
        newline();
    }
    void close(char c) {
        bool empty = stack_.back().first;
        stack_.pop_back();
        if (!empty) newline();
        os_ << c;
    }
    void newline() {
        if (!pretty_) return;
        os_ << '\n';
        for (std::size_t i = 0; i < stack_.size(); ++i) os_ << "  ";
    }
    void string(std::string_view s) {
        os_ << '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': os_ << "\\\""; break;
                case '\\': os_ << "\\\\"; break;
                case '\n': os_ << "\\n"; break;
                case '\r': os_ << "\\r"; break;
                case '\t': os_ << "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); os_ << b; }
                    else os_ << c;
            }
        }
        os_ << '"';
    }
    std::ostream& os_;
    bool pretty_;
    bool after_key_ = false;
    std::vector<Frame> stack_;
};

}  // namespace alamo::tools
