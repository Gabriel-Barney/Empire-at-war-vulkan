// BM1 benchmark statistics, result comparison and the result JSON value (bench_stats.hpp, bench_json.hpp).
#include "alamo/sim/bench_stats.hpp"
#include "alamo/sim/bench_json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace alamo::sim::bench {

// ------------------------------------------------------------------------------------------------- stats
double percentile(std::span<const double> ms, double p) {
    if (ms.empty()) return 0;
    std::vector<double> v(ms.begin(), ms.end());
    std::sort(v.begin(), v.end());
    const double rank = std::ceil(p / 100.0 * static_cast<double>(v.size()));
    const size_t i = rank < 1 ? 0 : std::min(v.size() - 1, static_cast<size_t>(rank) - 1);
    return v[i];
}

TickStats summarize(std::span<const double> ms) {
    TickStats s;
    s.count = ms.size();
    if (ms.empty()) return s;
    for (size_t i = 0; i < ms.size(); ++i) {
        s.total_ms += ms[i];
        if (i == 0 || ms[i] > s.worst_ms) {
            s.worst_ms = ms[i];
            s.worst_index = i;
        }
    }
    s.mean_ms = s.total_ms / static_cast<double>(ms.size());
    std::vector<double> v(ms.begin(), ms.end());
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    s.median_ms = n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    s.p99_ms = percentile(ms, 99.0);
    return s;
}

Comparison compare(const ResultKey& base, const TickStats& base_stats, const ResultKey& now, const TickStats& now_stats) {
    Comparison c;
    auto no = [&](std::string why) {
        c.comparable = false;
        c.reason = std::move(why);
        c.text = "scenario changed, not comparable: " + c.reason;
        return c;
    };
    if (!base.fingerprint_ok) return no("the baseline did not match its scenario fingerprint");
    if (!now.fingerprint_ok) return no("this run did not match the scenario fingerprint");
    if (base.scenario != now.scenario) return no("other scenario ('" + base.scenario + "' vs '" + now.scenario + "')");
    if (base.version != now.version)
        return no("scenario version " + std::to_string(base.version) + " vs " + std::to_string(now.version));
    if (base.fingerprint_digest != now.fingerprint_digest)
        return no("fingerprint " + base.fingerprint_digest + " vs " + now.fingerprint_digest);
    if (base.ticks != now.ticks) return no("ticks per run " + std::to_string(base.ticks) + " vs " + std::to_string(now.ticks));
    if (base_stats.count == 0 || now_stats.count == 0 || now_stats.median_ms <= 0 || now_stats.p99_ms <= 0 || now_stats.worst_ms <= 0)
        return no("no tick times");
    c.comparable = true;
    c.median_ratio = base_stats.median_ms / now_stats.median_ms;
    c.p99_ratio = base_stats.p99_ms / now_stats.p99_ms;
    c.worst_ratio = base_stats.worst_ms / now_stats.worst_ms;
    char buf[256];
    const double r = c.median_ratio;
    std::snprintf(buf, sizeof buf, "median %.3f -> %.3f ms (%.2fx %s), p99 %.3f -> %.3f ms, worst %.3f -> %.3f ms", base_stats.median_ms,
                  now_stats.median_ms, r >= 1 ? r : 1.0 / r, r >= 1 ? "faster" : "slower", base_stats.p99_ms, now_stats.p99_ms,
                  base_stats.worst_ms, now_stats.worst_ms);
    c.text = buf;
    return c;
}

// ------------------------------------------------------------------------------------------------- JSON value
Json Json::boolean(bool v) {
    Json j;
    j.type_ = Type::Bool;
    j.b_ = v;
    return j;
}
Json Json::number(double v) {
    Json j;
    j.type_ = Type::Number;
    j.n_ = v;
    return j;
}
Json Json::string(std::string v) {
    Json j;
    j.type_ = Type::String;
    j.s_ = std::move(v);
    return j;
}
Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}
Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json& Json::set(std::string key, Json v) {
    if (type_ != Type::Object) *this = object();
    for (auto& m : members_)
        if (m.first == key) {
            m.second = std::move(v);
            return m.second;
        }
    members_.emplace_back(std::move(key), std::move(v));
    return members_.back().second;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

const Json* Json::at(std::string_view path) const {
    const Json* j = this;
    size_t i = 0;
    while (j && i <= path.size()) {
        const size_t e = std::min(path.find('.', i), path.size());
        j = j->find(path.substr(i, e - i));
        i = e + 1;
        if (e == path.size()) break;
    }
    return j;
}

Json& Json::push(Json v) {
    if (type_ != Type::Array) *this = array();
    items_.push_back(std::move(v));
    return items_.back();
}

namespace {

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

void write_number(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[40];
    if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.6g", v);
    out += buf;
}

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}
    bool parse(Json& out, std::string* err) {
        ws();
        if (!value(out, 0)) {
            if (err) *err = "JSON offset " + std::to_string(i_) + ": " + why_;
            return false;
        }
        ws();
        if (i_ != t_.size()) {
            if (err) *err = "JSON offset " + std::to_string(i_) + ": trailing text";
            return false;
        }
        return true;
    }

private:
    bool bad(const char* w) {
        why_ = w;
        return false;
    }
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view w) {
        if (t_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    bool str(std::string& s) {
        if (i_ >= t_.size() || t_[i_] != '"') return bad("expected a string");
        ++i_;
        while (i_ < t_.size()) {
            const char c = t_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (i_ >= t_.size()) break;
            const char e = t_[i_++];
            switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                if (i_ + 4 > t_.size()) return bad("short \\u escape");
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = t_[i_++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return bad("bad \\u escape");
                }
                s += cp < 0x80 ? static_cast<char>(cp) : '?';
                break;
            }
            default: return bad("bad escape");
            }
        }
        return bad("unterminated string");
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return bad("nested too deep");
        if (i_ >= t_.size()) return bad("unexpected end");
        const char c = t_[i_];
        if (c == '{') {
            ++i_;
            out = Json::object();
            ws();
            if (i_ < t_.size() && t_[i_] == '}') {
                ++i_;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i_ >= t_.size() || t_[i_] != ':') return bad("expected ':'");
                ++i_;
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.set(std::move(key), std::move(v));
                ws();
                if (i_ < t_.size() && t_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return bad("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i_;
            out = Json::array();
            ws();
            if (i_ < t_.size() && t_[i_] == ']') {
                ++i_;
                return true;
            }
            for (;;) {
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (i_ < t_.size() && t_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return bad("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!str(s)) return false;
            out = Json::string(std::move(s));
            return true;
        }
        if (lit("true")) {
            out = Json::boolean(true);
            return true;
        }
        if (lit("false")) {
            out = Json::boolean(false);
            return true;
        }
        if (lit("null")) {
            out = Json();
            return true;
        }
        const size_t start = i_;
        while (i_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[i_])) || t_[i_] == '-' || t_[i_] == '+' || t_[i_] == '.' ||
                                  t_[i_] == 'e' || t_[i_] == 'E'))
            ++i_;
        if (i_ == start) return bad("unexpected character");
        const std::string num(t_.substr(start, i_ - start));
        char* end = nullptr;
        const double v = std::strtod(num.c_str(), &end);
        if (end != num.c_str() + num.size()) return bad("bad number");
        out = Json::number(v);
        return true;
    }

    std::string_view t_;
    size_t i_ = 0;
    std::string why_;
};

}  // namespace

void Json::write(std::string& out, int depth) const {
    const std::string pad(static_cast<size_t>(depth + 1) * 2, ' ');
    const std::string close(static_cast<size_t>(depth) * 2, ' ');
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: write_number(out, n_); break;
    case Type::String: write_string(out, s_); break;
    case Type::Array: {
        if (items_.empty()) {
            out += "[]";
            break;
        }
        // Arrays of scalars stay on one line.
        bool flat = true;
        for (const Json& j : items_)
            if (j.type_ == Type::Array || j.type_ == Type::Object) flat = false;
        out += '[';
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i) out += flat ? ", " : ",";
            if (!flat) out += "\n" + pad;
            items_[i].write(out, depth + 1);
        }
        if (!flat) out += "\n" + close;
        out += ']';
        break;
    }
    case Type::Object: {
        if (members_.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (size_t i = 0; i < members_.size(); ++i) {
            if (i) out += ',';
            out += "\n" + pad;
            write_string(out, members_[i].first);
            out += ": ";
            members_[i].second.write(out, depth + 1);
        }
        out += "\n" + close + "}";
        break;
    }
    }
}

std::string Json::to_text() const {
    std::string out;
    write(out, 0);
    out += '\n';
    return out;
}

bool Json::parse(std::string_view text, Json& out, std::string* err) {
    out = Json();
    Parser p(text);
    return p.parse(out, err);
}

}  // namespace alamo::sim::bench
