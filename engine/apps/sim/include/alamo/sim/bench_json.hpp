// BM1: a small JSON value for the benchmark result files (write, and read back a baseline to compare against).
// Objects keep their member order, so a written file is stable. Not a general-purpose JSON library: UTF-8 text is
// passed through, \uXXXX escapes are decoded only for code points below U+0080 (others become '?').
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alamo::sim::bench {

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json boolean(bool v);
    static Json number(double v);
    static Json string(std::string v);
    static Json array();
    static Json object();

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool as_bool(bool fallback = false) const noexcept { return type_ == Type::Bool ? b_ : fallback; }
    double as_number(double fallback = 0) const noexcept { return type_ == Type::Number ? n_ : fallback; }
    const std::string& as_string() const noexcept { return s_; }  // empty unless a string

    // Object: set (replace or append) / find a member. Array: push / items.
    Json& set(std::string key, Json v);
    const Json* find(std::string_view key) const;
    // Dotted path through objects ("scenario.name"); null when a step is missing.
    const Json* at(std::string_view path) const;
    Json& push(Json v);
    const std::vector<Json>& items() const noexcept { return items_; }
    const std::vector<std::pair<std::string, Json>>& members() const noexcept { return members_; }

    // Two-space indented text with a trailing newline.
    std::string to_text() const;
    static bool parse(std::string_view text, Json& out, std::string* err = nullptr);

private:
    void write(std::string& out, int depth) const;
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace alamo::sim::bench
