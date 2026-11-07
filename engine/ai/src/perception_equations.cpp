// AI perception equation language: parser, linker, evaluator, coverage (see the header).
#include "alamo/ai/perception_equations.hpp"

#include <charconv>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/core/hash.hpp"  // A2 tactical: crc32 ('#' reseed)
#include "alamo/core/sim_rng.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/enums.hpp"

namespace alamo::ai {

namespace {

constexpr std::string_view kRegistry[] = {
#include "token_registry.inc"
};
constexpr uint32_t kRegistrySize = static_cast<uint32_t>(sizeof(kRegistry) / sizeof(kRegistry[0]));

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string to_upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = upper(c);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || is_digit(c); }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// ---- enum literal converters (A2I_TOKENS_RE §3.1; A2J t07: dynamic ones from Data\XML\Enum, A2J_T07_LITERALS §1) ----

constexpr std::string_view kGoalCategoryEnum = "AIGoalCategoryType";

// HardPointType short names, value = index (EnumConversionClass<HardPointType>, TACTICAL_COMBAT §2.4; 11 has no name).
constexpr std::string_view kHardPointNames[] = {
    "ENGINE",       "SHIELD_GENERATOR", "GRAVITY_WELL",   "FIGHTER_BAY",       "TRACTOR_BEAM", "WEAPON_LASER", "WEAPON_MISSILE",
    "WEAPON_TORPEDO", "WEAPON_ION_CANNON", "WEAPON_MASS_DRIVER", "WEAPON_SPECIAL", "",          "DUMMY_ART",
};

// The converter list of one table. Lookup by converter name: ASCII case-insensitive, trimmed [port; L in the original,
// A2J_T07_LITERALS §3] (every shipped literal spells the name exactly as registered).
struct LiteralList {
    std::vector<objects::EnumConverter> conv;
    // A2k t12 (A2K_T06_RE.md §2) [H]: the converter name is trimmed, then compared exactly (case-sensitive).
    const objects::EnumConverter* find(std::string_view name) const {
        for (const objects::EnumConverter& c : conv)
            if (c.enum_name == trim(name)) return &c;
        return nullptr;
    }
};

// AIGoalCategoryType built-in table: AiDefinitions' shipped list (Data\XML\Enum\AIGoalCategoryType.xml).
objects::EnumConverter builtin_goal_categories() {
    const AiDefinitions defs;
    objects::EnumConverter goal;
    goal.enum_name = kGoalCategoryEnum;
    goal.dynamic = goal.valid = true;
    for (size_t i = 0; i < defs.categories.size() && i < defs.category_value.size(); ++i)
        goal.add(defs.categories[i], defs.category_value[i]);
    return goal;
}

// The static (compiled-in) converters the registry is assumed to hold besides the EnumRegistry ones [M, A2I §3.2].
void add_static_converters(LiteralList& l) {
    objects::EnumConverter diff;  // 0x5de400: Easy 0, Normal 1, Hard 2 (SCRIPT_GAPS)
    diff.enum_name = "DifficultyLevelType";
    diff.valid = true;
    diff.add("Easy", 0);
    diff.add("Normal", 1);
    diff.add("Hard", 2);
    l.conv.push_back(std::move(diff));
    objects::EnumConverter hp;
    hp.enum_name = "HardPointType";
    hp.valid = true;
    for (size_t i = 0; i < std::size(kHardPointNames); ++i)
        if (!kHardPointNames[i].empty()) hp.add(kHardPointNames[i], static_cast<int64_t>(i));
    l.conv.push_back(std::move(hp));
}

// The EnumRegistry converters that exist: built-in / static ones, and dynamic ones read from a file (a discarded file
// converter stays registered and fails every conversion, OBJECT_TYPES §9.1 [H]; SurfaceFXTriggerType without its file
// has no table and is absent).
void add_registry_converters(LiteralList& l, const objects::EnumRegistry& reg) {
    for (size_t i = 0; i < static_cast<size_t>(objects::EnumId::Count); ++i) {
        const objects::EnumConverter& c = reg.get(static_cast<objects::EnumId>(i));
        if ((c.valid || c.from_file) && !c.enum_name.empty()) l.conv.push_back(c);
    }
}

void build_builtin(LiteralList& l) {
    l.conv.clear();
    add_registry_converters(l, objects::EnumRegistry{});
    l.conv.push_back(builtin_goal_categories());
    add_static_converters(l);
}

// C atoi on an entry text (leading C white space, optional sign, decimal digits; 0 if none; int result). Vanilla
// values are small; the port stops accumulating past 2^40 (atoi overflow is undefined).
int64_t entry_atoi(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f'))
        ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int64_t v = 0;
    for (; i < s.size() && is_digit(s[i]) && v < (int64_t{1} << 40); ++i) v = v * 10 + (s[i] - '0');
    return static_cast<int64_t>(static_cast<int32_t>(neg ? -v : v));
}

// A dynamic enum read from Data\XML\Enum\<Name>.xml (OBJECT_TYPES §9.1 "Enum XML files" [H]; AIGoalCategoryType is
// loaded the same way, §9.1.2): each root child is an entry (tag = name, text = C atoi value, empty text = the running
// entry index); a duplicate name or value discards the converter. False (out unchanged) when the file cannot be read.
bool load_dynamic_enum(const FileProvider& files, std::string_view name, objects::EnumConverter& out,
                       std::vector<std::string>* diagnostics) {
    std::string path = "Data\\XML\\Enum\\";
    path += name;
    path += ".xml";
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return false;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, path);
    objects::EnumConverter fresh;
    fresh.enum_name = out.enum_name;
    fresh.dynamic = true;
    fresh.from_file = true;
    fresh.valid = doc.ok();
    if (!doc.ok() && diagnostics) diagnostics->push_back("enum file '" + path + "' unreadable: converter discarded");
    uint32_t index = 0;
    for (data::XmlNode e = doc.root().first_child(); e && fresh.valid; e = e.next_sibling(), ++index) {
        const std::string_view text = trim(e.text());
        const int64_t value = text.empty() ? static_cast<int64_t>(index) : entry_atoi(text);
        if (!fresh.add(e.name(), value) && diagnostics)
            diagnostics->push_back("ERROR: couldn't add enum entry '" + std::string(e.name()) + "' to " + std::string(name) +
                                   " (converter discarded)");
    }
    out = std::move(fresh);
    return true;
}

bool literal_delim(char c) { return c == '|' || c == ' ' || c == ',' || c == '\t' || c == '\n'; }

}  // namespace

struct EnumLiteralTable::Impl {
    LiteralList list;
};

EnumLiteralTable::EnumLiteralTable() : impl_(std::make_unique<Impl>()) { build_builtin(impl_->list); }
EnumLiteralTable::~EnumLiteralTable() = default;

void EnumLiteralTable::load(const FileProvider& files, std::vector<std::string>* diagnostics) {
    objects::EnumRegistry reg;
    std::vector<std::string> notes;
    reg.load_dynamic(files, &notes);
    if (diagnostics)
        for (std::string& n : notes)
            if (n.rfind("enum file '", 0) != 0) diagnostics->push_back(std::move(n));  // a missing file is no problem
    LiteralList l;
    add_registry_converters(l, reg);
    objects::EnumConverter goal = builtin_goal_categories();
    load_dynamic_enum(files, kGoalCategoryEnum, goal, diagnostics);  // missing file: the built-in list stays
    l.conv.push_back(std::move(goal));
    add_static_converters(l);
    impl_->list = std::move(l);
}

bool EnumLiteralTable::has(std::string_view type) const { return impl_->list.find(type) != nullptr; }

bool EnumLiteralTable::from_file(std::string_view type) const {
    const objects::EnumConverter* c = impl_->list.find(type);
    return c && c->from_file;
}

const EnumLiteralTable& EnumLiteralTable::builtin() {
    static const EnumLiteralTable t;  // immutable after the (thread-safe) first use
    return t;
}

EnumLiteralError convert_enum_literal(std::string_view type, std::string_view text, EnumLiteral& out) {
    return EnumLiteralTable::builtin().convert(type, text, out);
}

EnumLiteralError EnumLiteralTable::convert(std::string_view type, std::string_view text, EnumLiteral& out) const {
    out = EnumLiteral{};
    const objects::EnumConverter* c = impl_->list.find(type);
    if (!c) return EnumLiteralError::NoConverter;
    // One String_To_UInt call on the whole bracket text [H]: a plain enum looks it up as one name, a bitfield splits it.
    const std::optional<int64_t> v = c->convert(text);
    if (!v) return EnumLiteralError::Unconvertible;
    out.bitfield = c->bitfield;
    out.value = *v;
    if (!c->bitfield) {
        out.names.emplace_back(trim(text));
        return EnumLiteralError::None;
    }
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && literal_delim(text[i])) ++i;
        const size_t b = i;
        while (i < text.size() && !literal_delim(text[i])) ++i;
        const std::string_view tok = trim(text.substr(b, i - b));
        if (!tok.empty()) out.names.emplace_back(tok);
    }
    return EnumLiteralError::None;
}

namespace {

class Parser {
public:
    Parser(std::string_view text, TokenTable& tokens, Expression& out, const EnumLiteralTable& literals)
        : s_(text), tokens_(tokens), literals_(literals), out_(out) {}

    bool run(ParseError* err) {
        out_ = Expression{};
        uint32_t root = 0;
        bool ok = expr(root);
        if (ok) {
            ws();
            if (pos_ != s_.size()) ok = fail("unexpected character '" + std::string(1, s_[pos_]) + "'");
        }
        if (!ok) {
            if (err) *err = error_;
            out_ = Expression{};
            return false;
        }
        out_.root = root;
        return true;
    }
    bool sensitive() const { return sensitive_; }

private:
    void ws() {
        while (pos_ < s_.size() && is_space(s_[pos_])) ++pos_;
    }
    bool fail(std::string msg) {
        if (error_.message.empty()) {
            error_.offset = static_cast<uint32_t>(pos_);
            error_.message = std::move(msg);
        }
        return false;
    }
    bool peek(std::string_view tok) {
        ws();
        return s_.substr(pos_, tok.size()) == tok;
    }
    bool accept(std::string_view tok) {
        if (!peek(tok)) return false;
        pos_ += tok.size();
        return true;
    }
    uint32_t push(ExprNode n, bool paren = false) {
        out_.nodes.push_back(std::move(n));
        paren_.push_back(paren ? 1 : 0);
        return static_cast<uint32_t>(out_.nodes.size() - 1);
    }
    bool arithmetic_unparenthesised(uint32_t i) const {
        const Op op = out_.nodes[i].op;
        const bool arith = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div || op == Op::Random;
        return arith && paren_[i] == 0;
    }
    uint32_t binary(Op op, uint32_t a, uint32_t b) {
        ExprNode n;
        n.op = op;
        n.a = a;
        n.b = b;
        return push(std::move(n));
    }

    bool expr(uint32_t& out) {
        uint32_t lhs = 0;
        if (!additive(lhs)) return false;
        for (;;) {
            Op op = Op::Number;
            if (accept(">=")) op = Op::Ge;
            else if (accept("<=")) op = Op::Le;
            else if (accept("==")) op = Op::Eq;
            else if (accept("!=")) op = Op::Ne;
            else if (accept(">")) op = Op::Gt;
            else if (accept("<")) op = Op::Lt;
            else break;
            uint32_t rhs = 0;
            if (!additive(rhs)) return false;
            if (arithmetic_unparenthesised(lhs) || arithmetic_unparenthesised(rhs)) sensitive_ = true;
            lhs = binary(op, lhs, rhs);
        }
        out = lhs;
        return true;
    }
    bool additive(uint32_t& out) {
        uint32_t lhs = 0;
        if (!mult(lhs)) return false;
        for (;;) {
            Op op = Op::Number;
            if (accept("+")) op = Op::Add;
            else if (accept("-")) op = Op::Sub;
            else break;
            uint32_t rhs = 0;
            if (!mult(rhs)) return false;
            lhs = binary(op, lhs, rhs);
        }
        out = lhs;
        return true;
    }
    // S2 (STRATEGIC_GAPS §3) [H]: '*', '/' and '#' share one left-associative level.
    bool mult(uint32_t& out) {
        uint32_t lhs = 0;
        if (!unary(lhs)) return false;
        for (;;) {
            Op op = Op::Number;
            if (accept("*")) op = Op::Mul;
            else if (accept("/")) op = Op::Div;
            else if (accept("#")) op = Op::Random;
            else break;
            uint32_t rhs = 0;
            if (!unary(rhs)) return false;
            // Depends on the rule when '#' meets '*' / '/' without parentheses: a * b # c = (a*b) # c.
            const Op lop = out_.nodes[lhs].op;
            const bool l_random = lop == Op::Random && !paren_[lhs];
            const bool l_muldiv = (lop == Op::Mul || lop == Op::Div) && !paren_[lhs];
            if ((op == Op::Random && l_muldiv) || (op != Op::Random && l_random)) sensitive_ = true;
            lhs = binary(op, lhs, rhs);
        }
        out = lhs;
        return true;
    }
    bool unary(uint32_t& out) {
        if (accept("-")) {
            uint32_t a = 0;
            if (!unary(a)) return false;
            ExprNode n;
            n.op = Op::Neg;
            n.a = a;
            out = push(std::move(n));
            return true;
        }
        // S2: no unary '+'; spirit's real_p takes a sign that is part of the literal ("+3", not "+ 3", not "+X.Y").
        ws();
        if (pos_ + 1 < s_.size() && s_[pos_] == '+' &&
            (is_digit(s_[pos_ + 1]) || (s_[pos_ + 1] == '.' && pos_ + 2 < s_.size() && is_digit(s_[pos_ + 2])))) {
            ++pos_;
            ExprNode n;
            n.op = Op::Number;
            if (!number(n.number)) return false;
            out = push(std::move(n));
            return true;
        }
        return primary(out);
    }
    bool number(double& v) {
        ws();
        const size_t start = pos_;
        size_t p = pos_;
        while (p < s_.size() && is_digit(s_[p])) ++p;
        if (p < s_.size() && s_[p] == '.') {
            ++p;
            while (p < s_.size() && is_digit(s_[p])) ++p;
        }
        if (p == start || (p == start + 1 && s_[start] == '.')) return fail("number expected");
        if (p < s_.size() && (s_[p] == 'e' || s_[p] == 'E')) {
            size_t q = p + 1;
            if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) ++q;
            if (q < s_.size() && is_digit(s_[q])) {
                while (q < s_.size() && is_digit(s_[q])) ++q;
                p = q;
            }
        }
        const auto r = std::from_chars(s_.data() + start, s_.data() + p, v);
        if (r.ec != std::errc() || r.ptr != s_.data() + p) return fail("bad number");
        pos_ = p;
        return true;
    }
    bool ident(std::string_view& id) {
        ws();
        if (pos_ >= s_.size() || !is_ident_start(s_[pos_])) return fail("identifier expected");
        const size_t start = pos_;
        while (pos_ < s_.size() && is_ident(s_[pos_])) ++pos_;
        id = s_.substr(start, pos_ - start);
        return true;
    }
    bool primary(uint32_t& out) {
        ws();
        if (pos_ >= s_.size()) return fail("unexpected end of expression");
        const char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            if (!expr(out)) return false;
            if (!accept(")")) return fail("')' expected");
            paren_[out] = 1;
            return true;
        }
        if (is_digit(c) || (c == '.' && pos_ + 1 < s_.size() && is_digit(s_[pos_ + 1]))) {
            ExprNode n;
            n.op = Op::Number;
            if (!number(n.number)) return false;
            out = push(std::move(n));
            return true;
        }
        if (is_ident_start(c)) return reference(out);
        return fail("unexpected character '" + std::string(1, c) + "'");
    }
    bool reference(uint32_t& out) {
        std::vector<std::string_view> parts;
        std::string_view id;
        if (!ident(id)) return false;
        parts.push_back(id);
        while (pos_ < s_.size()) {
            // '.' directly or after whitespace, followed by an identifier
            size_t save = pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == '.') {
                ++pos_;
                if (!ident(id)) return false;
                parts.push_back(id);
            } else {
                pos_ = save;
                break;
            }
        }
        ExprNode n;
        const std::string_view head = parts[0];
        if (parts.size() == 1 && iequals(head, "clamp") && peek("(")) {
            ++pos_;
            n.op = Op::Clamp;
            if (!expr(n.a)) return false;
            if (!accept(",")) return fail("',' expected in clamp");
            if (!expr(n.b)) return false;
            if (!accept(",")) return fail("',' expected in clamp");
            if (!expr(n.c)) return false;
            if (!accept(")")) return fail("')' expected after clamp arguments");
            out = push(std::move(n), true);
            return true;
        }
        static constexpr std::pair<std::string_view, Root> kRoots[] = {
            {"Variable_Self", Root::Self},   {"Variable_Target", Root::Target},
            {"Variable_Enemy", Root::Enemy}, {"Variable_Human", Root::Human},
            {"Variable_Location", Root::Location}, {"Game", Root::Game},
        };
        bool is_root = false;
        for (const auto& [name, root] : kRoots) {
            if (iequals(head, name)) {
                is_root = true;
                n.op = Op::Token;
                n.root = root;
            }
        }
        if (is_root) {
            // S2 (STRATEGIC_GAPS §3) [H]: a bare identifier is not an operand; a lookup needs at least one dot.
            if (parts.size() < 2) return fail("'" + std::string(head) + "': a lookup needs at least one '.'");
            n.path_begin = static_cast<uint32_t>(out_.path.size());
            for (size_t i = 1; i < parts.size(); ++i) out_.path.push_back(tokens_.intern(parts[i]));
            n.path_len = static_cast<uint32_t>(parts.size() - 1);
        } else if (istarts_with(head, "Function_") || istarts_with(head, "Script_")) {
            const bool fn = istarts_with(head, "Function_");
            n.op = fn ? Op::Function : Op::Script;
            n.name = std::string(head.substr(fn ? 9 : 7));
            n.ref = kUnresolved;
            if (n.name.empty()) return fail("empty function name");
            if (parts.size() != 2 || !iequals(parts[1], "Evaluate"))
                return fail(std::string(head) + ": expected '.Evaluate'");
        } else {
            return fail("unknown reference '" + std::string(head) + "'");
        }
        if (peek("{")) {
            ++pos_;
            n.param_begin = static_cast<uint32_t>(out_.params.size());
            if (!accept("}")) {
                for (;;) {
                    Param prm;
                    if (!param(prm)) return false;
                    out_.params.push_back(std::move(prm));
                    if (accept(",")) continue;
                    if (accept("}")) break;
                    return fail("',' or '}' expected in parameter list");
                }
            }
            n.param_count = static_cast<uint32_t>(out_.params.size()) - n.param_begin;
        }
        out = push(std::move(n));
        return true;
    }
    bool param(Param& p) {
        std::string_view name;
        if (!ident(name)) return false;
        p.name = std::string(name);
        if (!accept("=")) return fail("'=' expected after " + p.name);
        ws();
        if (pos_ >= s_.size()) return fail("parameter value expected");
        const char c = s_[pos_];
        if (c == '"') {
            const size_t end = s_.find('"', pos_ + 1);
            if (end == std::string_view::npos) return fail("unterminated string");
            p.value.kind = ParamValue::Kind::String;
            p.value.text = std::string(s_.substr(pos_ + 1, end - pos_ - 1));
            pos_ = end + 1;
            return true;
        }
        if (c == '-' || is_digit(c) || c == '.') {
            bool neg = false;
            if (c == '-') {
                neg = true;
                ++pos_;
            }
            p.value.kind = ParamValue::Kind::Number;
            if (!number(p.value.number)) return false;
            if (neg) p.value.number = -p.value.number;
            return true;
        }
        std::string_view id;
        if (!ident(id)) return false;
        p.value.text = std::string(id);
        if (accept("[")) {
            // A2I_TOKENS_RE §3.1 [H]: the bracket text goes to the converter named by the identifier, whole, while the
            // equation is built; no converter or no conversion fails the build (§3.2).
            p.value.kind = ParamValue::Kind::Enum;
            const size_t end = s_.find(']', pos_);
            if (end == std::string_view::npos) return fail("']' expected in enum value");
            const std::string_view body = s_.substr(pos_, end - pos_);
            EnumLiteral lit;
            const EnumLiteralError e = literals_.convert(id, body, lit);
            if (e == EnumLiteralError::NoConverter) return fail("There is no enum converter named " + std::string(id) + "!");
            if (e != EnumLiteralError::None)
                return fail("Enum converter(" + std::string(id) + ") was unable to convert " + std::string(trim(body)) +
                            " to a valid enum value!");
            p.value.enum_values = std::move(lit.names);
            p.value.enum_value = lit.value;
            pos_ = end + 1;
        } else {
            p.value.kind = ParamValue::Kind::Identifier;
        }
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
    TokenTable& tokens_;
    const EnumLiteralTable& literals_;
    Expression& out_;
    std::vector<uint8_t> paren_;
    ParseError error_;
    bool sensitive_ = false;
};

// ---- evaluation ----

constexpr uint32_t kMaxDepth = 64;

struct Evaluator {
    const EquationSet& set;
    const EvalContext& ctx;
    const TokenResolver& resolver;
    EvalError error = EvalError::None;
    uint32_t rnd = 0;  // A2 tactical: the '#' LCG state (AI_GALACTIC_3 §7.3)

    // A2 tactical: 0x5f11a0, before every equation evaluation (32-bit wrap).
    void reseed(std::string_view name) {
        const uint32_t self = static_cast<uint32_t>(ctx.self < 0 ? 0 : ctx.self);
        const uint32_t name_crc = name.empty() ? 0u : crc32(name.data(), name.size());
        rnd = ctx.game_seed + (self * 131u + 0x123456FBu) + name_crc + ctx.target_name_crc;
    }
    // A2 tactical: 0x5f1150 - single precision, lo = min(a, b), hi = max(a, b).
    float random(float a, float b) {
        const float lo = a < b ? a : b, hi = a < b ? b : a;
        rnd = rnd * 1103515245u + 3039u;
        const float v = static_cast<float>((rnd >> 10) & 0x7FFFu) / 32767.0f;
        const float span = hi - lo;
        const float scaled = v * span;
        return scaled + lo;
    }

    const Expression* fail_expr = nullptr;  // q6 t10 diagnostics: the node of the first failure
    uint32_t fail_node = 0;

    bool fail(EvalError e, const Expression& x, uint32_t i) {
        if (error == EvalError::None) {
            error = e;
            fail_expr = &x;
            fail_node = i;
        }
        return false;
    }

    bool eval(const Expression& x, uint32_t i, uint32_t depth, double& out) {
        const ExprNode& n = x.nodes[i];
        switch (n.op) {
            case Op::Number:
                out = n.number;
                return true;
            case Op::Token: {
                TokenRef t;
                t.root = n.root;
                t.path = std::span<const TokenId>(x.path).subspan(n.path_begin, n.path_len);
                t.params = std::span<const Param>(x.params).subspan(n.param_begin, n.param_count);
                t.tokens = &set.tokens();
                if (!resolver.resolve(ctx, t, out)) return fail(EvalError::Token, x, i);
                return true;
            }
            case Op::Function: {
                if (n.ref == kUnresolved || n.ref >= set.size()) return fail(EvalError::Function, x, i);
                if (depth + 1 >= kMaxDepth) return fail(EvalError::Recursion, x, i);
                const Equation& e = set.equation(n.ref);
                if (!e.parsed || e.expr.empty()) return fail(EvalError::Function, x, i);
                reseed(e.name);  // A2 tactical: the inner Evaluate reseeds the shared '#' state [M]
                return eval(e.expr, e.expr.root, depth + 1, out);
            }
            case Op::Script: {  // A2 stage 2: the resolver may run the Script_<File> evaluator
                const std::span<const Param> params = std::span<const Param>(x.params).subspan(n.param_begin, n.param_count);
                if (!resolver.resolve_script(ctx, n.name, params, out)) return fail(EvalError::Script, x, i);
                return true;
            }
            case Op::Neg: {
                double a = 0;
                if (!eval(x, n.a, depth, a)) return false;
                out = -a;
                return true;
            }
            case Op::Clamp: {
                double v = 0, lo = 0, hi = 0;
                if (!eval(x, n.a, depth, v) || !eval(x, n.b, depth, lo) || !eval(x, n.c, depth, hi)) return false;
                if (lo > hi) return fail(EvalError::Clamp, x, i);  // A2 galactic (AI_GALACTIC_3 §7.1) [H]
                out = lo > v ? lo : std::min(hi, v);
                return true;
            }
            default:
                break;
        }
        double a = 0, b = 0;
        // Left operand first, then right (only '#' draws, so the order matters only for it).
        if (!eval(x, n.a, depth, a)) return false;
        if (!eval(x, n.b, depth, b)) return false;
        switch (n.op) {
            case Op::Add: out = a + b; return true;
            case Op::Sub: out = a - b; return true;
            case Op::Mul: out = a * b; return true;
            case Op::Div:  // A2 galactic (AI_GALACTIC_3 §7.1) [H]: 0 / x = 0 (even x = 0); x / 0 fails the equation
                if (a == 0.0) {
                    out = 0.0;
                    return true;
                }
                if (b == 0.0) return fail(EvalError::DivideByZero, x, i);
                out = a / b;
                return true;
            case Op::Random:  // A2 tactical: the own reseeded LCG, never the synced RNG (AI_GALACTIC_3 §7.3)
                out = static_cast<double>(random(static_cast<float>(a), static_cast<float>(b)));
                return true;
            case Op::Gt: out = a > b ? 1.0 : 0.0; return true;
            case Op::Ge: out = a >= b ? 1.0 : 0.0; return true;
            case Op::Lt: out = a < b ? 1.0 : 0.0; return true;
            case Op::Le: out = a <= b ? 1.0 : 0.0; return true;
            case Op::Eq: out = a == b ? 1.0 : 0.0; return true;
            case Op::Ne: out = a != b ? 1.0 : 0.0; return true;
            default: return fail(EvalError::Empty, x, i);
        }
    }
};

std::string token_path_string(Root root, std::span<const TokenId> path, const TokenTable& tokens) {
    std::string s = root_name(root);
    for (TokenId t : path) {
        s += '.';
        s += tokens.name(t);
    }
    return s;
}

}  // namespace

// ---- TokenTable ----

uint32_t TokenTable::registry_size() { return kRegistrySize; }

TokenTable::TokenTable() {
    names_.reserve(kRegistrySize + 64);
    for (std::string_view n : kRegistry) intern(n);
}

TokenId TokenTable::intern(std::string_view name) {
    std::string key = to_upper(name);
    const auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    const auto id = static_cast<TokenId>(names_.size());
    names_.emplace_back(name);
    index_.emplace(std::move(key), id);
    return id;
}

TokenId TokenTable::find(std::string_view name) const {
    const auto it = index_.find(to_upper(name));
    return it == index_.end() ? kNoToken : it->second;
}

TokenId registry_token(std::string_view name) {
    for (uint32_t i = 0; i < kRegistrySize; ++i)
        if (iequals(kRegistry[i], name)) return i;
    return kNoToken;
}

std::span<const std::string_view> registry_token_names() { return std::span<const std::string_view>(kRegistry); }

const char* root_name(Root r) {
    switch (r) {
        case Root::Self: return "Variable_Self";
        case Root::Target: return "Variable_Target";
        case Root::Enemy: return "Variable_Enemy";
        case Root::Human: return "Variable_Human";
        case Root::Location: return "Variable_Location";
        case Root::Game: return "Game";
    }
    return "?";
}

const char* eval_error_name(EvalError e) {
    switch (e) {
        case EvalError::None: return "none";
        case EvalError::Token: return "token";
        case EvalError::Function: return "function";
        case EvalError::Script: return "script";
        case EvalError::Recursion: return "recursion";
        case EvalError::Random: return "random-without-rng";
        case EvalError::Empty: return "empty";
        case EvalError::DivideByZero: return "divide-by-zero";  // A2 galactic
        case EvalError::Clamp: return "clamp-range";            // A2 galactic
    }
    return "?";
}

std::string eval_failure_site(const EquationSet& set, const EvalResult& r) {
    if (r.ok) return {};
    if (!r.fail_expr || r.fail_node >= r.fail_expr->nodes.size()) return eval_error_name(r.error);
    const Expression& x = *r.fail_expr;
    const ExprNode& n = x.nodes[r.fail_node];
    switch (n.op) {
        case Op::Token:
            return token_path_string(n.root, std::span<const TokenId>(x.path).subspan(n.path_begin, n.path_len), set.tokens());
        case Op::Function: return std::string(eval_error_name(r.error)) + " Function_" + n.name;
        case Op::Script: return "Script_" + n.name;
        default: return eval_error_name(r.error);
    }
}

// ---- parsing ----

bool parse_expression(std::string_view text, TokenTable& tokens, Expression& out, ParseError* err,
                      const EnumLiteralTable* literals) {
    Parser p(text, tokens, out, literals ? *literals : EnumLiteralTable::builtin());
    return p.run(err);
}

bool precedence_sensitive(std::string_view text) {
    TokenTable t;
    Expression x;
    Parser p(text, t, x, EnumLiteralTable::builtin());
    return p.run(nullptr) && p.sensitive();
}

uint32_t EquationSet::add(std::string_view name, std::string_view text, LoadReport* report) {
    Equation e;
    e.name = std::string(name);
    e.text = std::string(trim(text));
    std::string key = to_upper(name);
    declared_.insert(key);  // a2k t12: a Function_ token even when the equation fails to build or is a duplicate
    if (index_.count(key)) {
        if (report) {
            ++report->duplicates;
            report->messages.push_back("duplicate equation " + e.name + " (first definition kept)");
        }
        return kUnresolved;
    }
    Parser p(e.text, tokens_, e.expr, enum_literals());
    e.parsed = p.run(&e.error);
    if (report) {
        ++report->equations;
        if (!e.parsed) {
            ++report->parse_errors;
            report->messages.push_back(e.name + ": parse error at " + std::to_string(e.error.offset) + ": " +
                                       e.error.message);
        } else if (p.sensitive()) {
            ++report->precedence_sensitive;
        }
    }
    // A2I_TOKENS_RE §3.2 [H]: an equation that fails to build is not added (its file-mates still load); the port keeps
    // the record for diagnostics, out of the name index.
    const auto idx = static_cast<uint32_t>(eq_.size());
    const bool parsed = e.parsed;
    eq_.push_back(std::move(e));
    if (!parsed) return kUnresolved;
    index_.emplace(std::move(key), idx);
    return idx;
}

void EquationSet::add_file(std::string_view xml, const std::string& file, LoadReport& report) {
    ++report.files;
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    for (const auto& d : doc.diagnostics()) report.messages.push_back(d.to_string());
    if (!doc.ok()) {
        ++report.files_failed;
        return;
    }
    for (data::XmlNode n : doc.root().children()) {
        const size_t before = eq_.size();
        const uint32_t dup_before = report.duplicates;
        add(n.name(), n.text_trimmed(), &report);
        if (eq_.size() > before) {
            eq_.back().file = file;
            eq_.back().line = n.line();
            if (!eq_.back().parsed) report.messages.back() = file + "(" + std::to_string(n.line()) + "): " + report.messages.back();
        } else if (report.duplicates > dup_before) {
            report.messages.back() = file + "(" + std::to_string(n.line()) + "): " + report.messages.back();
        }
    }
}

void EquationSet::link(LoadReport* report) {
    if (report) report->unresolved_functions = 0;
    // A2k t12 (A2K_T06_RE.md §1.2-1.4) [H]: retail registers Function_<Name> for every root element of every equation file
    // before it parses any equation, so a reference to a name that no file defines is an invalid token and fails the
    // referrer's build (dropped like a parse error). A defined name whose equation was dropped stays a valid token: the
    // referrer builds and the reference evaluates false (unresolved, below). [port] the check runs here, after every
    // add, so a later duplicate of a referrer dropped by it was already rejected as a duplicate.
    for (uint32_t i = 0; i < eq_.size(); ++i) {
        Equation& e = eq_[i];
        if (!e.parsed) continue;
        for (const ExprNode& n : e.expr.nodes) {
            if (n.op != Op::Function || declared_.count(to_upper(n.name))) continue;
            e.parsed = false;
            e.error = {0, "Invalid perceptual token (Function_" + n.name + ") while trying to parse an equation!"};
            const auto it = index_.find(to_upper(e.name));
            if (it != index_.end() && it->second == i) index_.erase(it);
            if (report) {
                ++report->parse_errors;
                report->messages.push_back(e.file + "(" + std::to_string(e.line) + "): " + e.name + ": " + e.error.message);
            }
            break;
        }
    }
    for (Equation& e : eq_) {
        if (!e.parsed) continue;
        for (ExprNode& n : e.expr.nodes) {
            if (n.op != Op::Function) continue;
            n.ref = find(n.name);
            if (n.ref == kUnresolved && report) {
                ++report->unresolved_functions;
                report->messages.push_back(e.file + "(" + std::to_string(e.line) + "): " + e.name +
                                           ": unresolved Function_" + n.name);
            }
        }
    }
}

uint32_t EquationSet::find(std::string_view name) const {
    const auto it = index_.find(to_upper(name));
    return it == index_.end() ? kUnresolved : it->second;
}

void EquationSet::set_enum_literals(std::shared_ptr<const EnumLiteralTable> table) { literals_ = std::move(table); }

const EnumLiteralTable& EquationSet::enum_literals() const {
    return literals_ ? *literals_ : EnumLiteralTable::builtin();
}

LoadReport load_equation_files(const FileProvider& provider, std::span<const std::string> paths, EquationSet& out) {
    LoadReport report;
    // A2J t07 (A2J_T07_LITERALS §1): the literal converters come from the same data (Data\XML\Enum through the
    // provider, so a mod's enum files count) unless the caller set a table; missing files keep the built-in tables.
    if (!out.has_enum_literals()) {
        auto table = std::make_shared<EnumLiteralTable>();
        table->load(provider, &report.messages);
        out.set_enum_literals(std::move(table));
    }
    std::vector<uint8_t> bytes;
    for (const std::string& path : paths) {
        bytes.clear();
        if (!provider.read(path, bytes)) {
            ++report.files;
            ++report.files_failed;
            report.messages.push_back(path + ": cannot read");
            continue;
        }
        out.add_file(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, report);
    }
    out.link(&report);
    return report;
}

// ---- evaluation ----

EvalResult evaluate(const EquationSet& set, const Expression& expr, const EvalContext& ctx,
                    const TokenResolver& resolver) {
    EvalResult r;
    if (expr.empty()) {
        r.error = EvalError::Empty;
        return r;
    }
    Evaluator ev{set, ctx, resolver};
    ev.reseed({});  // A2 tactical: an anonymous expression (no equation name: CRC part 0)
    r.ok = ev.eval(expr, expr.root, 0, r.value);
    r.error = ev.error;
    r.fail_expr = ev.fail_expr;
    r.fail_node = ev.fail_node;
    if (!r.ok) r.value = 0.0;
    return r;
}

EvalResult evaluate(const EquationSet& set, uint32_t equation, const EvalContext& ctx, const TokenResolver& resolver) {
    EvalResult r;
    if (equation >= set.size() || !set.equation(equation).parsed) {
        r.error = EvalError::Function;
        return r;
    }
    const Equation& e = set.equation(equation);
    if (e.expr.empty()) {
        r.error = EvalError::Empty;
        return r;
    }
    Evaluator ev{set, ctx, resolver};
    ev.reseed(e.name);  // A2 tactical: 0x5f11a0 before the evaluation (AI_GALACTIC_3 §7.3)
    r.ok = ev.eval(e.expr, e.expr.root, 0, r.value);
    r.error = ev.error;
    r.fail_expr = ev.fail_expr;
    r.fail_node = ev.fail_node;
    if (!r.ok) r.value = 0.0;
    return r;
}

// ---- coverage ----

std::vector<CoverageEntry> coverage(const EquationSet& set, const TokenResolver* resolver) {
    std::map<std::string, CoverageEntry> m;  // sorted by path: deterministic report
    for (uint32_t i = 0; i < static_cast<uint32_t>(set.size()); ++i) {
        const Equation& e = set.equation(i);
        if (!e.parsed) continue;
        const Expression& x = e.expr;
        for (const ExprNode& n : x.nodes) {
            if (n.op == Op::Token) {
                const auto path = std::span<const TokenId>(x.path).subspan(n.path_begin, n.path_len);
                std::string key = token_path_string(n.root, path, set.tokens());
                CoverageEntry& c = m[key];
                if (c.uses == 0) {
                    c.path = key;
                    c.kind = CoverageEntry::Kind::Token;
                    c.registered = true;
                    for (TokenId t : path) c.registered = c.registered && TokenTable::is_registered(t);
                    c.implemented = resolver && resolver->implements(n.root, path, set.tokens());
                }
                ++c.uses;
            } else if (n.op == Op::Function || n.op == Op::Script) {
                const bool fn = n.op == Op::Function;
                std::string key = (fn ? "Function_" : "Script_") + n.name + ".Evaluate";
                CoverageEntry& c = m[key];
                if (c.uses == 0) {
                    c.path = key;
                    c.kind = fn ? CoverageEntry::Kind::Function : CoverageEntry::Kind::Script;
                    c.registered = false;
                    c.implemented = fn && n.ref != kUnresolved;
                }
                ++c.uses;
            }
        }
    }
    std::vector<CoverageEntry> out;
    out.reserve(m.size());
    for (auto& [k, v] : m) out.push_back(std::move(v));
    return out;
}

std::string coverage_report(const EquationSet& set, const LoadReport& report, const TokenResolver* resolver,
                            std::string_view title) {
    const std::vector<CoverageEntry> cov = coverage(set, resolver);
    uint32_t tok = 0, tok_reg = 0, tok_impl = 0, tok_uses = 0, tok_impl_uses = 0, fn = 0, fn_res = 0, scr = 0;
    for (const CoverageEntry& c : cov) {
        if (c.kind == CoverageEntry::Kind::Token) {
            ++tok;
            tok_uses += c.uses;
            if (c.registered) ++tok_reg;
            if (c.implemented) {
                ++tok_impl;
                tok_impl_uses += c.uses;
            }
        } else if (c.kind == CoverageEntry::Kind::Function) {
            ++fn;
            if (c.implemented) ++fn_res;
        } else {
            ++scr;
        }
    }
    std::string s;
    s += "AI perception equation coverage: ";
    s += title;
    s += "\n";
    s += "files " + std::to_string(report.files) + " (failed " + std::to_string(report.files_failed) + "), equations " +
         std::to_string(report.equations) + ", parse errors " + std::to_string(report.parse_errors) + ", duplicates " +
         std::to_string(report.duplicates) + ", precedence-sensitive " + std::to_string(report.precedence_sensitive) +
         ", unresolved Function_ refs " + std::to_string(report.unresolved_functions) + "\n";
    s += "token paths " + std::to_string(tok) + " (" + std::to_string(tok_uses) + " uses): every element registered " +
         std::to_string(tok_reg) + ", unregistered element " + std::to_string(tok - tok_reg) +
         ", implemented by the galactic resolver " + std::to_string(tok_impl) + " (" + std::to_string(tok_impl_uses) +
         " uses)\n";
    s += "Function_ refs " + std::to_string(fn) + " (resolved " + std::to_string(fn_res) + "), Script_ refs " +
         std::to_string(scr) + " (never evaluated)\n\n";
    s += "# status: I = implemented, R = registered token (not implemented), U = unregistered element, F = Function_ "
         "resolved, f = Function_ unresolved, S = Script_\n";
    for (const CoverageEntry& c : cov) {
        char st = '?';
        if (c.kind == CoverageEntry::Kind::Token) st = c.implemented ? 'I' : (c.registered ? 'R' : 'U');
        else if (c.kind == CoverageEntry::Kind::Function) st = c.implemented ? 'F' : 'f';
        else st = 'S';
        s += st;
        s += ' ';
        s += std::to_string(c.uses);
        s += ' ';
        s += c.path;
        s += '\n';
    }
    if (!report.messages.empty()) {
        s += "\n# messages\n";
        for (const std::string& m : report.messages) s += m + "\n";
    }
    return s;
}

}  // namespace alamo::ai
