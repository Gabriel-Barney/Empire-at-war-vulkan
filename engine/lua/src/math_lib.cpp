// Optional script `math` table (LuaVmOptions::math_library). The original exposes no math library
// (PGBase.lua: "Nasty hack of a floor function to be replaced if a math library floor function is
// exposed"), so it is off by default; it exists for tools and mods that expect Lua 5.0's lmathlib.
// The vendored lmathlib.c is NOT compiled: every function here is deterministic across compilers:
//   * transcendentals use alamo::dmath (bit-identical on MSVC / GCC / Clang);
//   * floor / ceil / fmod / fabs / sqrt / frexp / ldexp are exact IEEE operations;
//   * math.random draws from the SYNCHRONIZED GameRandom (r15 / 32768 in [0, 1)), so scripts stay in
//     lockstep; math.randomseed is ignored (scripts must not reseed the session stream).
// Same names, argument order and radians as Lua 5.0's lmathlib, including the global __pow that
// 5.0's `^` operator calls.
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "vm_internal.hpp"

namespace alamo::lua::detail {

namespace {

constexpr double kRadiansPerDegree = dmath::kPi / 180.0;

LuaVm::Impl* vm_of(lua_State* L) { return static_cast<LuaVm::Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }

// These are plain C-style functions (no C++ objects), so luaL_check* errors are safe here.
double arg(lua_State* L, int i) { return luaL_checknumber(L, i); }
int ret(lua_State* L, double v) {
    lua_pushnumber(L, v);
    return 1;
}

int m_abs(lua_State* L) { return ret(L, std::fabs(arg(L, 1))); }
int m_sin(lua_State* L) { return ret(L, dmath::sin(arg(L, 1))); }
int m_cos(lua_State* L) { return ret(L, dmath::cos(arg(L, 1))); }
int m_tan(lua_State* L) { return ret(L, dmath::tan(arg(L, 1))); }
int m_asin(lua_State* L) { return ret(L, dmath::asin(arg(L, 1))); }
int m_acos(lua_State* L) { return ret(L, dmath::acos(arg(L, 1))); }
int m_atan(lua_State* L) { return ret(L, dmath::atan(arg(L, 1))); }
int m_atan2(lua_State* L) { return ret(L, dmath::atan2(arg(L, 1), arg(L, 2))); }
int m_ceil(lua_State* L) { return ret(L, std::ceil(arg(L, 1))); }
int m_floor(lua_State* L) { return ret(L, std::floor(arg(L, 1))); }
int m_mod(lua_State* L) { return ret(L, std::fmod(arg(L, 1), arg(L, 2))); }
int m_sqrt(lua_State* L) { return ret(L, dmath::sqrt(arg(L, 1))); }
int m_log(lua_State* L) { return ret(L, dmath::log(arg(L, 1))); }
int m_log10(lua_State* L) { return ret(L, dmath::log10(arg(L, 1))); }
int m_exp(lua_State* L) { return ret(L, dmath::exp(arg(L, 1))); }
int m_deg(lua_State* L) { return ret(L, arg(L, 1) / kRadiansPerDegree); }
int m_rad(lua_State* L) { return ret(L, arg(L, 1) * kRadiansPerDegree); }
int m_pow(lua_State* L) { return ret(L, dmath::pow(arg(L, 1), arg(L, 2))); }

int m_frexp(lua_State* L) {
    int e = 0;
    lua_pushnumber(L, std::frexp(arg(L, 1), &e));
    lua_pushnumber(L, e);
    return 2;
}

int m_ldexp(lua_State* L) { return ret(L, std::ldexp(arg(L, 1), to_int(luaL_checknumber(L, 2)))); }

int m_min(lua_State* L) {
    const int n = lua_gettop(L);
    double best = arg(L, 1);
    for (int i = 2; i <= n; ++i) {
        const double d = arg(L, i);
        if (d < best) best = d;
    }
    return ret(L, best);
}

int m_max(lua_State* L) {
    const int n = lua_gettop(L);
    double best = arg(L, 1);
    for (int i = 2; i <= n; ++i) {
        const double d = arg(L, i);
        if (d > best) best = d;
    }
    return ret(L, best);
}

// lmathlib semantics on the synchronized stream: random() in [0,1), random(m) in [1,m],
// random(m,n) in [m,n].
int m_random(lua_State* L) {
    LuaVm::Impl* vm = vm_of(L);
    if (!vm->host.rng) return luaL_error(L, "math.random: no game random attached to this script");
    const double r = static_cast<double>(vm->host.rng->synced().next15()) / 32768.0;
    switch (lua_gettop(L)) {
        case 0: return ret(L, r);
        case 1: {
            const int32_t m = to_int(luaL_checknumber(L, 1));
            luaL_argcheck(L, 1 <= m, 1, "interval is empty");
            return ret(L, std::floor(r * m) + 1);
        }
        case 2: {
            const int32_t lo = to_int(luaL_checknumber(L, 1));
            const int hi = to_int(luaL_checknumber(L, 2));
            luaL_argcheck(L, lo <= hi, 2, "interval is empty");
            return ret(L, std::floor(r * (static_cast<double>(hi) - lo + 1.0)) + lo);
        }
        default: return luaL_error(L, "wrong number of arguments");
    }
}

int m_randomseed(lua_State*) { return 0; }

}  // namespace

void install_math_library(lua_State* L, LuaVm::Impl& vm) {
    const luaL_reg fns[] = {
        {"abs", m_abs},     {"sin", m_sin},       {"cos", m_cos},     {"tan", m_tan},     {"asin", m_asin},
        {"acos", m_acos},   {"atan", m_atan},     {"atan2", m_atan2}, {"ceil", m_ceil},   {"floor", m_floor},
        {"mod", m_mod},     {"frexp", m_frexp},   {"ldexp", m_ldexp}, {"sqrt", m_sqrt},   {"min", m_min},
        {"max", m_max},     {"log", m_log},       {"log10", m_log10}, {"exp", m_exp},     {"deg", m_deg},
        {"pow", m_pow},     {"rad", m_rad},       {"random", m_random}, {"randomseed", m_randomseed},
    };
    lua_pushstring(L, "math");
    lua_newtable(L);
    for (const luaL_reg& r : fns) {
        lua_pushstring(L, r.name);
        lua_pushlightuserdata(L, &vm);
        lua_pushcclosure(L, r.func, 1);
        lua_rawset(L, -3);
    }
    lua_pushstring(L, "pi");
    lua_pushnumber(L, dmath::kPi);
    lua_rawset(L, -3);
    lua_pushstring(L, "huge");
    lua_pushnumber(L, HUGE_VAL);
    lua_rawset(L, -3);
    lua_rawset(L, LUA_GLOBALSINDEX);
    // Lua 5.0's `^` operator calls the global __pow.
    lua_pushstring(L, "__pow");
    lua_pushlightuserdata(L, &vm);
    lua_pushcclosure(L, m_pow, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace alamo::lua::detail
