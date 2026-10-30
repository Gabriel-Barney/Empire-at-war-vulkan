// PresentationLog / PresentationState (see alamo/lua/presentation.hpp).
#include "alamo/lua/presentation.hpp"

#include <array>
#include <cmath>
#include <iterator>
#include <new>
#include <utility>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

uint32_t seconds_to_frames(double seconds) {
    if (!(seconds > 0.0)) return 0;  // also NaN
    const double f = std::ceil(seconds * static_cast<double>(kSimFramesPerSecond));
    if (f >= 2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(f);
}

std::string_view op_name(PresentationOp op) {
    static constexpr std::string_view kNames[] = {
        "Start_Cinematic_Camera",
        "End_Cinematic_Camera",
        "Start_Cinematic_Mode",
        "End_Cinematic_Mode",
        "Set_Cinematic_Camera_Key",
        "Set_Cinematic_Target_Key",
        "Transition_Cinematic_Camera_Key",
        "Transition_Cinematic_Target_Key",
        "Transition_To_Tactical_Camera",
        "Point_Camera_At",
        "Scroll_Camera_To",
        "Camera_To_Follow",
        "Rotate_Camera_By",
        "Rotate_Camera_To",
        "Zoom_Camera",
        "Cinematic_Zoom",
        "Set_Cinematic_Environment",
        "Set_New_Environment",
        "Promote_To_Space_Cinematic_Layer",
        "Do_End_Cinematic_Cleanup",
        "Fade_Screen_In",
        "Fade_Screen_Out",
        "Fade_On",
        "Fade_Off",
        "Letter_Box_In",
        "Letter_Box_Out",
        "Letter_Box_On",
        "Letter_Box_Off",
        "Lock_Controls",
        "Cancel_Fast_Forward",
        "Enable_Fog",
        "Enable_Distance_Fog",
        "Allow_Localized_SFX",
        "Play_Music",
        "Stop_All_Music",
        "Resume_Mode_Based_Music",
        "Stop_All_Speech",
        "Master_Volume_Restore",
        "Weather_Audio_Pause",
        "LuaSFXCommandsClassClass.Allow_Ambient_VO",
        "LuaSFXCommandsClassClass.Allow_Enemy_Sighted_VO",
        "LuaSFXCommandsClassClass.Allow_HUD_VO",
        "LuaSFXCommandsClassClass.Allow_Localized_SFXEvents",
        "LuaSFXCommandsClassClass.Allow_Unit_Reponse_VO",
        "GameObjectWrapper.Play_SFX_Event",
        "GameObjectWrapper.Stop_SFX_Event",
        "Play_Bink_Movie",
        "Stop_Bink_Movie",
        "Play_Lightning_Effect",
        "Game_Message",
        "Remove_All_Text",
        "Activate_Retry_Dialog",
        "GUI_Button_Icon",
        "GUI_Component_Blink",
        "GUI_Component_Color",
        "GUI_Component_Enable",
        "GUI_Component_Flash",
        "GUI_Component_Play_Anim",
        "GUI_Component_Stop_Anim",
        "GUI_Component_Stop_Flash",
        "GUI_Component_Text",
        "GUI_Component_Visibility",
        "GUI_Text_Color",
        "Hide_Sub_Object",
        "GameObjectWrapper.Attach_Particle_Effect",
        "GameObjectWrapper.Cinematic_Hyperspace_In",
        "GameObjectWrapper.In_End_Cinematic",
        "Resume_Hyperspace_In",
        "GameObjectWrapper.Show_Emitter",
        "GameObjectWrapper.Play_Cinematic_Engine_Flyby",
    };
    static_assert(std::size(kNames) == static_cast<size_t>(PresentationOp::Count));
    const auto i = static_cast<size_t>(op);
    return i < std::size(kNames) ? kNames[i] : std::string_view("?");
}

double Ramp::at(uint32_t frame) const {
    if (frame >= end) return to;
    if (frame <= start) return from;
    return from + (to - from) * (static_cast<double>(frame - start) / static_cast<double>(end - start));
}

void Ramp::go(double target, uint32_t frame, uint32_t frames) {
    const double now = at(frame);
    from = now;
    to = target;
    start = frame;
    end = frame + frames;
}

PresentationEvent& PresentationLog::record(uint32_t frame, PresentationOp op) {
    PresentationEvent& e = events_.emplace_back();
    e.frame = frame;
    e.seq = next_seq_++;
    e.op = op;
    return e;
}

uint32_t PresentationLog::open_block(uint32_t end_frame) {
    const uint32_t id = next_block_++;
    blocks_[id].end = end_frame;
    return id;
}

void PresentationLog::finish_block(uint32_t id, uint32_t frame) {
    const auto it = blocks_.find(id);
    if (it != blocks_.end() && it->second.end > frame) it->second.end = frame;
}

bool PresentationLog::block_finished(uint32_t id, uint32_t frame) const {
    const auto it = blocks_.find(id);
    return it == blocks_.end() || frame >= it->second.end;
}

namespace {

void hash_arg(StateHasher& h, const PresentationArg& a) {
    h.u8(static_cast<uint8_t>(a.kind));
    h.boolean(a.b);
    h.f64(a.number);
    h.i32(a.id);
    h.str(a.text);
}

void hash_ramp(StateHasher& h, const Ramp& r) {
    h.f64(r.from);
    h.f64(r.to);
    h.u32(r.start);
    h.u32(r.end);
}

void hash_key(StateHasher& h, const CameraKey& k) {
    h.boolean(k.set);
    h.u32(static_cast<uint32_t>(k.args.size()));
    for (const PresentationArg& a : k.args) hash_arg(h, a);
    h.u32(k.start);
    h.u32(k.end);
}

}  // namespace

uint64_t PresentationLog::digest() const {
    StateHasher h;
    h.u32(static_cast<uint32_t>(events_.size()));
    for (const PresentationEvent& e : events_) {
        h.u32(e.frame);
        h.u32(e.seq);
        h.u16(static_cast<uint16_t>(e.op));
        h.u32(e.block);
        h.u32(static_cast<uint32_t>(e.args.size()));
        for (const PresentationArg& a : e.args) hash_arg(h, a);
    }
    const PresentationState& s = state_;
    h.u8(static_cast<uint8_t>(s.camera));
    h.boolean(s.cinematic_mode);
    h.boolean(s.cinematic_letterbox_arg);
    hash_key(h, s.camera_key);
    hash_key(h, s.target_key);
    hash_ramp(h, s.tactical_blend);
    hash_arg(h, s.camera_focus);
    h.boolean(s.following);
    h.boolean(s.cinematic_environment);
    hash_ramp(h, s.fade);
    hash_ramp(h, s.letterbox);
    for (const bool b : {s.controls_locked, s.fast_forward, s.fog_enabled, s.distance_fog_enabled, s.localized_sfx,
                         s.weather_audio_paused, s.ambient_vo, s.enemy_sighted_vo, s.hud_vo, s.localized_sfx_events,
                         s.unit_response_vo, s.mode_based_music, s.retry_dialog})
        h.boolean(b);
    h.str(s.music);
    h.str(s.movie);
    h.u32(s.movie_block);
    for (const auto* m : {&s.hidden_sub_objects, &s.emitters}) {
        h.u32(static_cast<uint32_t>(m->size()));
        for (const auto& [k, v] : *m) {
            h.str(k);
            h.boolean(v);
        }
    }
    h.u32(static_cast<uint32_t>(s.in_end_cinematic.size()));
    for (const auto& [k, v] : s.in_end_cinematic) {
        h.i32(k);
        h.boolean(v);
    }
    h.u32(static_cast<uint32_t>(blocks_.size()));
    for (const auto& [id, b] : blocks_) {
        h.u32(id);
        h.u32(b.end);
    }
    return h.digest();
}

// ---------------------------------------------------------------- N1 save / load

namespace {

constexpr uint32_t kPresentationSaveVersion = 1;
constexpr size_t kArgBytes = 1 + 1 + 8 + 4 + 4;

bool read_count(ByteReader& r, size_t record_bytes, uint32_t& n) {
    n = r.u32();
    if (!r.ok() || static_cast<uint64_t>(n) * record_bytes > r.remaining()) {
        r.fail();
        return false;
    }
    return true;
}

void put_arg(ByteWriter& w, const PresentationArg& a) {
    w.u8(static_cast<uint8_t>(a.kind));
    w.boolean(a.b);
    w.f64(a.number);
    w.i32(a.id);
    w.str(a.text);
}

bool get_arg(ByteReader& r, PresentationArg& a) {
    const uint8_t kind = r.u8();
    if (kind > static_cast<uint8_t>(PresentationArg::Kind::Other)) r.fail();
    a.kind = static_cast<PresentationArg::Kind>(kind);
    a.b = r.boolean();
    a.number = r.f64();
    a.id = r.i32();
    a.text = r.str();
    return r.ok();
}

void put_args(ByteWriter& w, const std::vector<PresentationArg>& args) {
    w.u32(static_cast<uint32_t>(args.size()));
    for (const PresentationArg& a : args) put_arg(w, a);
}

bool get_args(ByteReader& r, std::vector<PresentationArg>& args) {
    uint32_t n = 0;
    if (!read_count(r, kArgBytes, n)) return false;
    args.assign(n, PresentationArg{});
    for (PresentationArg& a : args)
        if (!get_arg(r, a)) return false;
    return true;
}

void put_ramp(ByteWriter& w, const Ramp& x) {
    w.f64(x.from);
    w.f64(x.to);
    w.u32(x.start);
    w.u32(x.end);
}

void get_ramp(ByteReader& r, Ramp& x) {
    x.from = r.f64();
    x.to = r.f64();
    x.start = r.u32();
    x.end = r.u32();
}

void put_key(ByteWriter& w, const CameraKey& k) {
    w.boolean(k.set);
    put_args(w, k.args);
    w.u32(k.start);
    w.u32(k.end);
}

bool get_key(ByteReader& r, CameraKey& k) {
    k.set = r.boolean();
    if (!get_args(r, k.args)) return false;
    k.start = r.u32();
    k.end = r.u32();
    return r.ok();
}

void put_flag_map(ByteWriter& w, const std::map<std::string, bool>& m) {
    w.u32(static_cast<uint32_t>(m.size()));
    for (const auto& [k, v] : m) {
        w.str(k);
        w.boolean(v);
    }
}

// Keys strictly ascending (the map order), so a valid image has exactly one encoding.
bool get_flag_map(ByteReader& r, std::map<std::string, bool>& m) {
    uint32_t n = 0;
    if (!read_count(r, 5, n)) return false;
    m.clear();
    for (uint32_t i = 0; i < n; ++i) {
        std::string k = r.str();
        const bool v = r.boolean();
        if (!r.ok() || (!m.empty() && !(m.rbegin()->first < k))) {
            r.fail();
            return false;
        }
        m.emplace_hint(m.end(), std::move(k), v);
    }
    return true;
}

// The 13 switches in digest() order.
template <class S>
auto switches(S& s) {
    return std::array<decltype(&s.controls_locked), 13>{&s.controls_locked, &s.fast_forward, &s.fog_enabled, &s.distance_fog_enabled,
                                                        &s.localized_sfx, &s.weather_audio_paused, &s.ambient_vo, &s.enemy_sighted_vo,
                                                        &s.hud_vo, &s.localized_sfx_events, &s.unit_response_vo, &s.mode_based_music,
                                                        &s.retry_dialog};
}

}  // namespace

void PresentationLog::write(ByteWriter& w) const {
    w.u32(kPresentationSaveVersion);
    w.u32(next_seq_);
    w.u32(next_block_);
    w.u32(static_cast<uint32_t>(blocks_.size()));
    for (const auto& [id, b] : blocks_) {
        w.u32(id);
        w.u32(b.end);
    }
    const PresentationState& s = state_;
    w.u8(static_cast<uint8_t>(s.camera));
    w.boolean(s.cinematic_mode);
    w.boolean(s.cinematic_letterbox_arg);
    put_key(w, s.camera_key);
    put_key(w, s.target_key);
    put_ramp(w, s.tactical_blend);
    put_arg(w, s.camera_focus);
    w.boolean(s.following);
    w.boolean(s.cinematic_environment);
    put_ramp(w, s.fade);
    put_ramp(w, s.letterbox);
    for (const bool* b : switches(s)) w.boolean(*b);
    w.str(s.music);
    w.str(s.movie);
    w.u32(s.movie_block);
    put_flag_map(w, s.hidden_sub_objects);
    put_flag_map(w, s.emitters);
    w.u32(static_cast<uint32_t>(s.in_end_cinematic.size()));
    for (const auto& [k, v] : s.in_end_cinematic) {
        w.i32(k);
        w.boolean(v);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const PresentationEvent& e : events_) {
        w.u32(e.frame);
        w.u32(e.seq);
        w.u16(static_cast<uint16_t>(e.op));
        w.u32(e.block);
        put_args(w, e.args);
    }
    w.u32(static_cast<uint32_t>(particle_effects_.size()));
    for (const ParticleEffectRequest& p : particle_effects_) {
        w.u32(p.frame);
        w.u32(p.seq);
        w.i32(p.object);
        w.i32(p.type);
        w.str(p.type_name);
        w.str(p.bone);
    }
}

bool PresentationLog::read(ByteReader& r) {
    if (r.u32() != kPresentationSaveVersion || !r.ok()) {
        r.fail();
        return false;
    }
    const uint32_t next_seq = r.u32();
    const uint32_t next_block = r.u32();
    if (!r.ok() || next_block == 0) {
        r.fail();
        return false;
    }
    uint32_t n = 0;
    if (!read_count(r, 8, n)) return false;
    std::map<uint32_t, Block> blocks;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = r.u32();
        const uint32_t end = r.u32();
        // Ids strictly ascending, each handed out before (1 .. next block - 1).
        if (!r.ok() || id == 0 || id >= next_block || (!blocks.empty() && id <= blocks.rbegin()->first)) {
            r.fail();
            return false;
        }
        blocks.emplace_hint(blocks.end(), id, Block{end});
    }
    PresentationState s;
    const uint8_t camera = r.u8();
    if (camera > static_cast<uint8_t>(CameraMode::Cinematic)) r.fail();
    s.camera = static_cast<CameraMode>(camera);
    s.cinematic_mode = r.boolean();
    s.cinematic_letterbox_arg = r.boolean();
    if (!get_key(r, s.camera_key) || !get_key(r, s.target_key)) return false;
    get_ramp(r, s.tactical_blend);
    if (!get_arg(r, s.camera_focus)) return false;
    s.following = r.boolean();
    s.cinematic_environment = r.boolean();
    get_ramp(r, s.fade);
    get_ramp(r, s.letterbox);
    for (bool* b : switches(s)) *b = r.boolean();
    s.music = r.str();
    s.movie = r.str();
    s.movie_block = r.u32();
    if (!r.ok() || !get_flag_map(r, s.hidden_sub_objects) || !get_flag_map(r, s.emitters)) return false;
    if (!read_count(r, 5, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t k = r.i32();
        const bool v = r.boolean();
        if (!r.ok() || (!s.in_end_cinematic.empty() && k <= s.in_end_cinematic.rbegin()->first)) {
            r.fail();
            return false;
        }
        s.in_end_cinematic.emplace_hint(s.in_end_cinematic.end(), k, v);
    }
    if (!read_count(r, 4 + 4 + 2 + 4 + 4, n)) return false;
    std::vector<PresentationEvent> evs(n);
    for (PresentationEvent& e : evs) {
        e.frame = r.u32();
        e.seq = r.u32();
        const uint16_t op = r.u16();
        if (op >= static_cast<uint16_t>(PresentationOp::Count)) r.fail();
        e.op = static_cast<PresentationOp>(op);
        e.block = r.u32();
        if (!r.ok() || !get_args(r, e.args)) {
            r.fail();
            return false;
        }
    }
    if (!read_count(r, 4 + 4 + 4 + 4 + 4 + 4, n)) return false;
    std::vector<ParticleEffectRequest> particles(n);
    for (ParticleEffectRequest& p : particles) {
        p.frame = r.u32();
        p.seq = r.u32();
        p.object = r.i32();
        p.type = r.i32();
        p.type_name = r.str();
        p.bone = r.str();
    }
    if (!r.ok()) return false;
    next_seq_ = next_seq;
    next_block_ = next_block;
    blocks_ = std::move(blocks);
    state_ = std::move(s);
    events_ = std::move(evs);
    particle_effects_ = std::move(particles);
    return true;
}

// ---------------------------------------------------------------- per-VM routing

namespace {

constexpr char kLogKey[] = "alamo.presentation";

// Lua 5.0 userdata blocks are aligned like L_Umaxalign (double / void* / long): 8 bytes here.
static_assert(alignof(PresentationLog) <= 8, "Lua 5.0 userdata alignment");

int owned_log_gc(lua_State* L) {
    auto* log = static_cast<PresentationLog*>(lua_touserdata(L, 1));
    if (log) log->~PresentationLog();
    return 0;
}

}  // namespace

PresentationLog& detail::presentation_of(lua_State* L) {
    lua_pushstring(L, kLogKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA || lua_type(L, -1) == LUA_TUSERDATA) {
        auto* log = static_cast<PresentationLog*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return *log;
    }
    lua_pop(L, 1);
    // VM-owned log: a full userdata in the registry, destroyed by __gc at lua_close.
    void* mem = lua_newuserdata(L, sizeof(PresentationLog));
    auto* log = new (mem) PresentationLog();
    lua_newtable(L);
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, owned_log_gc);
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);
    lua_pushstring(L, kLogKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    return *log;
}

void attach_presentation(LuaVm& vm, PresentationLog* log) {
    lua_State* L = vm.state();
    lua_pushstring(L, kLogKey);
    if (log)
        lua_pushlightuserdata(L, log);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

PresentationLog& presentation(LuaVm& vm) { return detail::presentation_of(vm.state()); }

}  // namespace alamo::lua
