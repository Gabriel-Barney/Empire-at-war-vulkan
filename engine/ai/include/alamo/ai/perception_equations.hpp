// AI perception equations (docs/re/HOTSPOTS.md section 2, GALACTIC_ECONOMY.md section 6.1).
//
// The original parses Data\XML\AI\PerceptualEquations\*.xml with a boost::spirit grammar inside
// PerceptionFunctionG.dll and walks the resulting tree, resolving token leaves through the exe's
// evaluator classes. This is a clean-room reimplementation of that language from the shipped files:
//
//   file      := <Equations> { <Name> expression </Name> }          (one equation per child element)
//   expr      := additive { cmp additive }        cmp: <= >= != == > <  (result 1.0 / 0.0, left-assoc chain)
//   additive  := mult { (+|-) mult }
//   mult      := unary { (*|/|#) unary }          a # b = random float in [a, b] (own reseeded LCG)
//   unary     := - unary | primary                (a '+' only as a literal's sign: +3)
//   primary   := number | ( expr ) | clamp ( expr , expr , expr ) | reference
//   reference := Variable_Self|Target|Enemy|Human|Location .Token { .Token } [params]  token path (>= 1 dot)
//              | Game .Token { .Token } [params]                                      global token path
//              | Function_<Equation>.Evaluate [params]                            another equation
//              | Script_<Name>.Evaluate [params]                                  Lua hook (resolve_script)
//   params    := { [ Parameter_X = value { , Parameter_Y = value } ] }
//   value     := "string" | number | EnumType[text] | identifier
//
// S2 grammar (STRATEGIC_GAPS §3, [H] from the DLL's spirit rules): precedence loosest -> tightest =
//   comparisons (chained left to right), + -, * / # ('#' binds like '*': a * b # c = (a*b) # c), unary
//   minus; a lookup needs at least one dot. Equations whose value depends on these rules (a comparison
//   with an unparenthesised arithmetic operand, '#' mixed with '*' / '/') are counted in LoadReport.
// Remaining notes:
//   * A2 tactical (AI_GALACTIC_3 §7.3, [H]): '#' uses its own LCG, reseeded per evaluation (EvalContext
//     game_seed / target_name_crc), lo = min, hi = max, (float)((s >> 10) & 0x7FFF) / 32767 x (hi - lo) + lo;
//     a Function_ reference reseeds with its own name [M]; 0 synced draws;
//   * any failing leaf (unknown token, unresolved Function_, Script_, recursion) fails the whole
//     evaluation (ok == false); 0 / x = 0, x / 0 fails (AI_GALACTIC_3 §7.1);
//   * clamp(x, lo, hi) = x < lo ? lo : (x > hi ? hi : x) (lo tested first; NaN passes through);
//   * duplicate equation names: the first loaded definition wins (later ones are reported);
//   * file order: as given (the host passes Vfs::list(), byte-sorted); names case-insensitive;
//   * enum literals (A2I_TOKENS_RE §3) [H]: `Type[text]` is converted when the equation is built, by the converter named
//     Type (EquationSet::enum_literals(); load_equation_files reads the data enum files, A2J t07): no such converter,
//     or text it cannot convert, is a parse error. A plain enum
//     looks the whole trimmed, case-insensitive text up as ONE name (so any '|' fails); a bitfield splits on '|', space,
//     ',', tab, LF and every token must be defined (value = their OR). An equation that fails to build (syntax or
//     enum literal) is not registered: find() does not see it, a later definition of the name is the first one, and
//     Function_ references to it stay unresolved. It is kept in equation() / size() for diagnostics only (parsed false).
//     A2J t07 [port; L in the original, A2J_T07_LITERALS §2]: the referencing equation still builds; evaluating the
//     unresolved reference fails the whole evaluation (EvalError::Function), like any failing leaf.
// Evaluation is pure ('#' included) and re-entrant: one EquationSet may be evaluated from many
// threads at once with per-call EvalContexts.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo {
class SimRng;
}

namespace alamo::ai {

// The glob the host lists through the VFS (Vfs::list) to find every equation file.
inline constexpr std::string_view kPerceptionEquationGlob = "Data\\XML\\AI\\PerceptualEquations\\*.xml";

using TokenId = uint32_t;
inline constexpr TokenId kNoToken = 0xFFFFFFFFu;

// Interned token names, ASCII case-insensitive. Ids 0..registry_size()-1 are the perception token
// names the original registers (0x5b43d0, docs/re/xml_tags.md), in a fixed order, so they are
// constants (registry_token()); names outside the registry get ids after them.
class TokenTable {
public:
    TokenTable();
    TokenId intern(std::string_view name);
    TokenId find(std::string_view name) const;  // kNoToken if absent
    std::string_view name(TokenId id) const { return names_[id]; }
    size_t size() const { return names_.size(); }
    static bool is_registered(TokenId id) { return id < registry_size(); }
    static uint32_t registry_size();

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, TokenId> index_;  // upper-cased name -> id (lookup only)
};

// Registry id of a registered token name (case-insensitive), kNoToken if it is not registered.
TokenId registry_token(std::string_view name);
std::span<const std::string_view> registry_token_names();

enum class Root : uint8_t { Self, Target, Enemy, Human, Location, Game };
const char* root_name(Root r);  // "Variable_Self", ..., "Game"

struct ParamValue {
    enum class Kind : uint8_t { Number, String, Enum, Identifier };
    Kind kind = Kind::Number;
    double number = 0.0;
    std::string text;                      // String / Identifier text, Enum type name
    std::vector<std::string> enum_values;  // Enum: the names inside [ ] (bitfield: every token; plain enum: one name)
    int64_t enum_value = 0;                // Enum: the converted value (the uint64 bit pattern the original carries)
};
struct Param {
    std::string name;  // "Parameter_Type"
    ParamValue value;
};

enum class Op : uint8_t {
    Number, Token, Function, Script,
    Neg, Add, Sub, Mul, Div, Random, Clamp,
    Gt, Ge, Lt, Le, Eq, Ne,
};

// Flat expression tree; nodes_[root()] is the top. Children always precede their parent.
struct ExprNode {
    Op op = Op::Number;
    Root root = Root::Game;   // Token
    uint32_t a = 0, b = 0, c = 0;  // operand node indices (unary: a; clamp: a, b = lo, c = hi)
    double number = 0.0;      // Number
    uint32_t path_begin = 0, path_len = 0;    // Token: into Expression::path
    uint32_t param_begin = 0, param_count = 0;  // Token/Function/Script: into Expression::params
    uint32_t ref = 0;         // Function: equation index after link (kUnresolved before / if missing)
    std::string name;         // Function: equation name; Script: script name
};
inline constexpr uint32_t kUnresolved = 0xFFFFFFFFu;

struct Expression {
    std::vector<ExprNode> nodes;
    std::vector<TokenId> path;
    std::vector<Param> params;
    uint32_t root = 0;
    bool empty() const { return nodes.empty(); }
};

struct ParseError {
    uint32_t offset = 0;  // byte offset in the expression text
    std::string message;
};
// The equation-literal converter registry (the exe's DatabaseUIntConversionClass registry as the port knows it): the
// objects::EnumRegistry converters (GameObjectCategoryType, GameObjectPropertiesType, MovementClassType, ...), plus
// AIGoalCategoryType, DifficultyLevelType (Easy 0, Normal 1, Hard 2) and HardPointType (the short names, value = index).
// Converter-name lookup ASCII case-insensitive [port; L in the original, A2J_T07_LITERALS §3]; entry names trimmed and
// case-insensitive [H]. `bitfield` tells how `text` was split; `value` = the converted value; `names` = the trimmed names /
// tokens (input case).
struct EnumLiteral {
    bool bitfield = false;
    int64_t value = 0;
    std::vector<std::string> names;
};
enum class EnumLiteralError : uint8_t { None, NoConverter, Unconvertible };

// A2J t07 (A2J_T07_LITERALS §1): one registry. Default-constructed = the built-in vanilla tables. load() reads the
// dynamic converters from Data\XML\Enum\<Name>.xml through `files` (GameObjectCategoryType, GameObjectPropertiesType,
// MovementClassType, ObjectWeatherCategoryType, SurfaceFXTriggerType as objects::EnumRegistry::load_dynamic does, and
// AIGoalCategoryType by the same dynamic-enum rules, OBJECT_TYPES §9.1 [H]): a missing file keeps the built-in table; a
// file with a duplicate entry (or a bitfield with more than 64) discards that converter, which then fails every literal
// (problems are appended to *diagnostics). DifficultyLevelType and HardPointType are compiled-in tables [H: static
// EnumConversionClass]. Immutable after load(): safe for concurrent const use.
class EnumLiteralTable {
public:
    EnumLiteralTable();
    ~EnumLiteralTable();
    EnumLiteralTable(const EnumLiteralTable&) = delete;
    EnumLiteralTable& operator=(const EnumLiteralTable&) = delete;
    void load(const FileProvider& files, std::vector<std::string>* diagnostics = nullptr);
    EnumLiteralError convert(std::string_view type, std::string_view text, EnumLiteral& out) const;
    bool has(std::string_view type) const;        // a converter of that name exists (discarded ones included)
    bool from_file(std::string_view type) const;  // its entries were read from Data\XML\Enum
    static const EnumLiteralTable& builtin();     // the shared built-in table

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// convert() on the built-in table.
EnumLiteralError convert_enum_literal(std::string_view type, std::string_view text, EnumLiteral& out);

// Parses one expression; tokens are interned into `tokens`. False (and *err) on a syntax error or an enum literal that
// does not convert. `literals` = the converter table (null: the built-in one).
bool parse_expression(std::string_view text, TokenTable& tokens, Expression& out, ParseError* err = nullptr,
                      const EnumLiteralTable* literals = nullptr);
// True when a comparison has an operand that is an unparenthesised arithmetic expression
// (the precedence assumption matters for it).
bool precedence_sensitive(std::string_view text);

struct Equation {
    std::string name;    // element name as written
    std::string file;    // source file
    uint32_t line = 0;
    std::string text;    // expression text (trimmed)
    Expression expr;
    bool parsed = false;
    ParseError error;
};

struct LoadReport {
    uint32_t files = 0, files_failed = 0, equations = 0, parse_errors = 0, duplicates = 0;
    uint32_t precedence_sensitive = 0, unresolved_functions = 0;
    std::vector<std::string> messages;  // file(line): ... for every problem
};

class EquationSet {
public:
    // Adds every equation of one file (XML bytes). `file` names it in diagnostics.
    void add_file(std::string_view xml, const std::string& file, LoadReport& report);
    // Adds one equation (tests, the galactic consumer paths). Returns its index, or kUnresolved on
    // a syntax error / duplicate (first wins).
    uint32_t add(std::string_view name, std::string_view text, LoadReport* report = nullptr);
    // Resolves every Function_ reference to an equation index. Call after the last add.
    void link(LoadReport* report = nullptr);

    uint32_t find(std::string_view name) const;  // kUnresolved if absent
    const Equation& equation(uint32_t i) const { return eq_[i]; }
    size_t size() const { return eq_.size(); }
    TokenTable& tokens() { return tokens_; }
    const TokenTable& tokens() const { return tokens_; }
    // The enum-literal converters add() builds with (A2J t07). Unset: the built-in table. Set it before the first add.
    void set_enum_literals(std::shared_ptr<const EnumLiteralTable> table);
    bool has_enum_literals() const { return literals_ != nullptr; }
    const EnumLiteralTable& enum_literals() const;

private:
    std::vector<Equation> eq_;
    std::unordered_map<std::string, uint32_t> index_;  // upper-cased name -> first definition
    TokenTable tokens_;
    std::shared_ptr<const EnumLiteralTable> literals_;
};

// Loads `paths` (game-relative, e.g. from Vfs::list(kPerceptionEquationGlob)) in the given order
// and links. Unreadable files are reported and skipped. A2J t07: when `out` has no literal table yet, one is loaded
// from `provider` first (EnumLiteralTable::load; its problems go to LoadReport::messages) and set on `out`.
LoadReport load_equation_files(const FileProvider& provider, std::span<const std::string> paths, EquationSet& out);

// ---- evaluation ----------------------------------------------------------------------------------

// Who the variables are bound to (PerceptionContextClass, GALACTIC_ECONOMY.md section 6.1):
// player ids for Self / Enemy / Human, a planet (or object) index for Target / Location.
struct EvalContext {
    int32_t self = -1;
    int32_t target = -1;
    int32_t enemy = -1;
    int32_t human = -1;
    int32_t location = -1;
    SimRng* rng = nullptr;  // Script_ evaluators (resolve_script) only; '#' never draws from it (below)
    // --- A2 tactical: the '#' generator (docs/re/AI_GALACTIC_3.md §7.3) [H]. '#' does NOT use the synced RNG: an own
    // LCG (x 1103515245 + 3039) reseeded before every equation evaluation from game_seed + (self x 131 + 0x123456FB)
    // + CRC-32(equation name) + target_name_crc (CRC-32 of the target's name, 0 when none). ---
    uint32_t game_seed = 0;        // the session seed (the value that seeded the synced LCG at game start)
    uint32_t target_name_crc = 0;  // alamo::crc32 of the target's name string
    // --- end A2 tactical ---
};

struct TokenRef {
    Root root = Root::Game;
    std::span<const TokenId> path;
    std::span<const Param> params;
    const TokenTable* tokens = nullptr;
};

// Resolves token leaves (the exe's *PerceptualEvaluatorClass family). Must be thread-safe for
// concurrent const calls.
class TokenResolver {
public:
    virtual ~TokenResolver() = default;
    // True and `out` set when the token is known and evaluable for this context.
    virtual bool resolve(const EvalContext& ctx, const TokenRef& token, double& out) const = 0;
    // Static coverage: does this resolver implement the path at all (any context)?
    virtual bool implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const = 0;
    // A2 stage 2: `Script_<name>.Evaluate [params]` (AI_GALACTIC_2 §4.4). Default: not evaluable (fails).
    // Serial sim-thread callers only (it may run Lua and draw from the synced RNG).
    virtual bool resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                                double& out) const {
        (void)ctx;
        (void)name;
        (void)params;
        (void)out;
        return false;
    }
};

enum class EvalError : uint8_t { None, Token, Function, Script, Recursion, Random, Empty, DivideByZero, Clamp };  // last two: A2 galactic
const char* eval_error_name(EvalError e);

struct EvalResult {
    bool ok = false;
    double value = 0.0;
    EvalError error = EvalError::None;
};

EvalResult evaluate(const EquationSet& set, const Expression& expr, const EvalContext& ctx,
                    const TokenResolver& resolver);
EvalResult evaluate(const EquationSet& set, uint32_t equation, const EvalContext& ctx, const TokenResolver& resolver);

// ---- coverage --------------------------------------------------------------------------------------

// Per distinct token path ("Variable_Target.EnemyForce.HasSpaceForce"): uses, and whether every
// element is a registered token / whether `resolver` implements it. Function_/Script_ references
// are listed too. Written as text by coverage_report().
struct CoverageEntry {
    std::string path;
    uint32_t uses = 0;
    enum class Kind : uint8_t { Token, Function, Script } kind = Kind::Token;
    bool registered = false;   // Token: every element in the original's registry
    bool implemented = false;  // Token: resolver->implements; Function: resolved
};
std::vector<CoverageEntry> coverage(const EquationSet& set, const TokenResolver* resolver);
std::string coverage_report(const EquationSet& set, const LoadReport& report, const TokenResolver* resolver,
                            std::string_view title);

}  // namespace alamo::ai
