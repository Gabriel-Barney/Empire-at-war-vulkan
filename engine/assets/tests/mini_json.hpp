// Minimal JSON reader for the test (reads the Phase 1 glTF examples). Not a general parser:
// no \u escapes beyond pass-through, throws std::runtime_error on malformed input.
#pragma once
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mini_json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value* get(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    const Value& operator[](const std::string& k) const {
        auto* v = get(k);
        if (!v) throw std::runtime_error("json: missing key " + k);
        return *v;
    }
    const Value& operator[](std::size_t i) const {
        if (i >= arr.size()) throw std::runtime_error("json: index out of range");
        return arr[i];
    }
    std::size_t as_size() const { return static_cast<std::size_t>(num); }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    Value parse() {
        Value v = value();
        ws();
        if (p_ != s_.size()) throw std::runtime_error("json: trailing data");
        return v;
    }

private:
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) ++p_;
    }
    char peek() {
        ws();
        if (p_ >= s_.size()) throw std::runtime_error("json: unexpected end");
        return s_[p_];
    }
    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("json: expected ") + c);
        ++p_;
    }
    std::string string() {
        expect('"');
        std::string o;
        while (p_ < s_.size() && s_[p_] != '"') {
            if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
                ++p_;
                const char e = s_[p_];
                o += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                o += s_[p_];
            }
            ++p_;
        }
        if (p_ >= s_.size()) throw std::runtime_error("json: unterminated string");
        ++p_;
        return o;
    }
    Value value() {
        Value v;
        const char c = peek();
        if (c == '{') {
            v.kind = Value::Kind::Object;
            ++p_;
            if (peek() == '}') { ++p_; return v; }
            for (;;) {
                std::string k = string();
                expect(':');
                v.obj[k] = value();
                if (peek() == ',') { ++p_; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.kind = Value::Kind::Array;
            ++p_;
            if (peek() == ']') { ++p_; return v; }
            for (;;) {
                v.arr.push_back(value());
                if (peek() == ',') { ++p_; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') {
            v.kind = Value::Kind::String;
            v.str = string();
            return v;
        }
        if (s_.substr(p_, 4) == "true") { p_ += 4; v.kind = Value::Kind::Bool; v.b = true; return v; }
        if (s_.substr(p_, 5) == "false") { p_ += 5; v.kind = Value::Kind::Bool; return v; }
        if (s_.substr(p_, 4) == "null") { p_ += 4; return v; }
        const std::size_t start = p_;
        while (p_ < s_.size() && (std::string_view("+-.eE0123456789").find(s_[p_]) != std::string_view::npos)) ++p_;
        if (start == p_) throw std::runtime_error("json: bad token");
        v.kind = Value::Kind::Number;
        v.num = std::strtod(std::string(s_.substr(start, p_ - start)).c_str(), nullptr);
        return v;
    }

    std::string_view s_;
    std::size_t p_ = 0;
};

inline Value parse(std::string_view s) { return Parser(s).parse(); }

}  // namespace mini_json
