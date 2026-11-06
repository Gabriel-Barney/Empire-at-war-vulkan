// alamo_game App: the in-game cinematic camera of a battle (refixf t05) - the tactical super weapon's firing shot and the
// explosion shake it carries (docs/re/TSW_PLANET_DESTROY.md §11; the math is battle_fx.hpp).
//
// Presentation only. The shot starts on the Fire cue (TacticalBattle::special_weapon_events, read after the tick), runs on
// the tactical frame count (stops while paused, 30 Hz steps), and only overrides what is drawn and heard: the RTS camera
// controller is saved at the start and restored at the end, the HUD and the cursor are hidden, mouse input is swallowed
// or ends the shot. Nothing here is written into the sim; the shake's NoiseOffset draws come from cine_.rng (unsynced).
#include "game_app.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo_game_app {

namespace {
constexpr const char* kStationMarker = "Space Station Position";  // MARKERS.XML (Behavior MARKER)
}

// Fire (0x42a200) -> 0x2ccf30 -> 0x4a5ef0(controller, mode, TSW, dummy, 1) + cinematic mode on (0x49b1a0).
void App::tsw_shot_begin(const TswCue& c, const sim::TacticalBattle& b, const objects::ObjectStore& st) {
    // [port] In_Game_Cinematics false: no shot (the original would hold it at frame 0 until input ends it [L]).
    if (!cine_.enabled || !bview_.active()) return;
    if (c.target == objects::kNoObjectId) return;  // 0x4a5ef0 fails without a dummy: no camera move at all
    const objects::ObjectHandle dh = st.find(c.target);
    if (!st.exists(dh)) return;
    if (cine_.shot.active()) tsw_shot_end();  // a running shot is stopped first (0x2cbb80)
    TswShotInputs in;
    in.tsw_position = c.position;
    const auto dp = st.position(dh);
    in.dummy_position = {dp.x, dp.y, dp.z};
    // E: [M] the render model's box (model path; the world AABB 0x3982b0 without a model) x Scale_Factor. [port] no model
    // loaded yet: a cube of the drawn sphere radius, else 200.
    ui::Vec3 e = bview_.type_half_extents(c.type);
    if (!(e.x > 0.0f) || !(e.y > 0.0f) || !(e.z > 0.0f)) {
        const float r = bview_.type_radius(c.type);
        const float h = r > 0.0f ? r : 200.0f;
        e = {h, h, h};
    }
    in.half_extents = e;
    {  // Ranged_Target_Z_Adjust (type +0x4dc), constructor default 6.0
        static const uint32_t f = objects::find_type_field("Ranged_Target_Z_Adjust");
        const objects::FieldValue* v = f != 0xFFFFFFFFu ? data_.types.value(c.type, f) : nullptr;
        in.z_adjust = v ? v->f : 6.0f;
    }
    // M: the first `Space Station Position` marker of the store query 0x2a9ff0(store, 0x11) [M: the marker list; here
    // the primary store's objects of that type, creation order].
    if (const objects::TypeIndex mt = data_.types.resolve_name(kStationMarker); mt != objects::kNone)
        for (const objects::ObjectHandle h : st.find_all_of_type(mt))
            if (st.alive(h)) {
                const auto mp = st.position(h);
                in.have_station = true;
                in.station_position = {mp.x, mp.y, mp.z};
                break;
            }
    cine_.shot.begin(in, b.frame(), input_.mouse_x(), input_.mouse_y());
    cine_.tsw = c.object;
    cine_.clock = 0;
    ++cine_.shots;
    // Cinematic mode on: the mode camera saved once (restored with its projection at the end; the shot's own near / far
    // planes are applied to the drawn params, TswFiringShot::kNearPlane / kFarPlane); HUD and cursor hidden
    // (cinematic_hides_*); FS +0x37 (30 ticks/s, pacing only); the letterbox slides in. [L] the two toggles 0x13ecc0 /
    // 0x48a3a0 and the counter 0x140b2c198 have no known role: not modelled.
    cine_.saved_camera.save_once(bview_.camera());
    if (session_) session_->set_presentation_rate_lock(true);
    cine_.letterbox.slide_in();
    std::printf("cinematic: TSW firing shot (tsw %u, tick %u, E %.1f %.1f %.1f, station marker %s)\n", static_cast<unsigned>(c.object),
                b.frame(), static_cast<double>(e.x), static_cast<double>(e.y), static_cast<double>(e.z), in.have_station ? "yes" : "no");
}

// TSW state 9: 0x2cdbb0(controller, dummy, the chosen Death_Explosions type, fixed = 1).
void App::tsw_shake(const TswCue& c, const sim::TacticalBattle& b) {
    // §11.1 gate (cinematic_shake): only while the cinematic runs (a battle = land / space mode).
    const objects::TypeIndex ex = tsw_explosion_type(data_.types, c.type, c.pick);
    if (ex == objects::kNone) return;  // no Death_Explosions entry: no explosion, no shake (§11.3)
    if (cinematic_shake(cine_.shake, cine_.shot.active(), true, explosion_jitter_factor(data_.types, ex), kTswShakeDistance,
                        cine_.shot.clock(b.frame()), cine_.rng))
        ++cine_.shakes;
}

// The shot timer ran out, input skipped it, or a new Fire replaces it: End_Cinematic_Mode (0x49b5c0: shake trackers
// freed, HUD / cursor / rate lock back, the saved mode camera restored) and 0x2cbb80 (letterbox out).
// [L] the end-of-shot chain (combat / list / interesting-object shots, held by Tab) is the attract demo's: not modelled,
// a normal battle is expected to find nothing there (probe).
void App::tsw_shot_end() {
    if (!cine_.shot.active()) return;
    std::printf("cinematic: TSW firing shot ended at shot frame %d\n", static_cast<int>(cine_.clock));
    cine_.shot.end();
    cine_.shake.reset();
    if (bview_.active()) cine_.saved_camera.restore(bview_.camera());
    cine_.saved_camera.drop();
    if (session_) session_->set_presentation_rate_lock(false);
    // [port][L] mode +0x98 (slide out over 0.5 s scaled by how far it is in, else removed at once) is unknown: slide out.
    cine_.letterbox.slide_out();
    cine_.tsw = objects::kNoObjectId;
}

void App::cinematic_reset() {
    cine_.shot.end();
    cine_.shake.reset();
    cine_.saved_camera.drop();
    cine_.letterbox.remove();
    cine_.tsw = objects::kNoObjectId;
    cine_.clock = 0;
    if (session_) session_->set_presentation_rate_lock(false);
}

void App::cinematic_step() {
    cine_.letterbox.update(dt_);
    if (!cine_.shot.active()) return;
    const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
    if (!b || !bview_.active()) {
        cinematic_reset();
        return;
    }
    if (cine_.shot.expired(b->frame())) {  // the shot timer (765 frames)
        ++cine_.completed;
        tsw_shot_end();
        return;
    }
    cine_.clock = cine_.shot.clock(b->frame());
}

// story t13: after the frame's Lua ops (lua_presentation) - the hand-over timer on the battle frame, the scripts' lock
// (Lock_Controls on, cinematic camera or mode on) for the P11 block, the bars' slide. No battle: nothing held.
void App::linked_ui_step() {
    const sim::TacticalBattle* b = session_ ? session_->battle() : nullptr;
    if (!b) {
        linked_ui_.reset();
        return;
    }
    bool locked = false;
#ifdef ALAMO_SIM_HAVE_STORY
    if (sim::StorySession* story = session_->host().story())
        if (const lua::PresentationLog* log = story->presentation()) {
            const lua::PresentationState& ps = log->state();
            locked = ps.controls_locked || ps.cinematic_mode || ps.camera == lua::CameraMode::Cinematic;
        }
#endif
    linked_ui_.update(b->frame(), locked, dt_);
}

// 0x2cc3e0 (no GUI dialog has the focus): the controller is offered the mouse events before the HUD and the world and
// consumes them. A move within 1/20 of the screen of the cursor at Fire is swallowed; a larger one or any button / wheel
// event ends the shot at once. [M] the event codes 2, 4, 5, 7, 8, 10, 12 = button / wheel events. Keys are not consumed.
void App::cinematic_input(ui::InputFrame& raw) {
    if (!cine_.shot.active() || ui_.gui.has_focus()) return;
    const bool button = raw.buttons_pressed != 0 || raw.buttons_released != 0 || raw.wheel != 0.0f;
    const bool cancel = cine_.shot.input_cancels(raw.mouse_x, raw.mouse_y, raw.width, raw.height, button);
    raw.buttons_pressed = raw.buttons_released = raw.buttons_down = 0;
    raw.wheel = 0.0f;
    if (cancel) {
        ++cine_.skipped;
        tsw_shot_end();
    }
}

// End of 0x49cd00: the keyframed eye / look at the shot clock (attached keys on the TSW's drawn transform), the shake,
// the shot's near / far planes; FOV stays the mode camera's (no FOV keys).
bool App::cinematic_camera(ui::CameraParams& cp) {
    if (!cine_.shot.active()) return false;
    ObjectFrame f;
    const std::vector<render::SnapshotObject>& objs = bview_.frame().objects;  // ascending id
    const auto it = std::lower_bound(objs.begin(), objs.end(), cine_.tsw,
                                     [](const render::SnapshotObject& o, objects::ObjectId id) { return o.id < id; });
    if (it != objs.end() && it->id == cine_.tsw) {
        const assets::Mat3x4 m = render::object_transform(*it, 1.0f);  // rotation + translation (E carries the scale) [M]
        f.valid = true;
        f.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
        for (int j = 0; j < 3; ++j) f.axis[j] = {m.m[0][j], m.m[1][j], m.m[2][j]};
    }
    const int32_t c = cine_.clock;
    const ui::Vec3 pos = cine_.shot.eye(c, f), look = cine_.shot.look(c, f);
    const CinematicPose p = apply_shake(pos, look, 0.0f, cine_.shake, c);  // no roll keys
    cp.eye = p.eye;
    cp.target = p.look;
    cp.up = p.up;
    cp.roll = p.roll_deg * (3.14159265358979f / 180.0f);
    const ui::Vec3 d = p.eye - p.look;
    const float dist = std::sqrt(tactical::dot(d, d));
    if (dist > 0.0f) {  // the orbit form of the same view (render_bridge uses it when up is +Z)
        cp.distance = dist;
        cp.orbit_yaw = std::atan2(d.y, d.x);
        cp.orbit_pitch = std::asin(std::clamp(d.z / dist, -1.0f, 1.0f));
    }
    cp.znear = TswFiringShot::kNearPlane;
    cp.zfar = TswFiringShot::kFarPlane;
    return true;
}

// The letterbox bar overlay (0x140a28c10 [M]): two black bars, each Camera_FX_Manager_Letterbox_Height x level of the
// screen height, over the world and under the dialogs. story t13: the level is the higher of the TSW shot's bars and the
// linked-battle / story-script bars (LinkedBattleOverlay).
void App::draw_letterbox(ui::HudDrawList& dl, const ui::HudCanvas& canvas, uint32_t w, uint32_t h) const {
    const float level = std::max(cine_.letterbox.level(), linked_ui_.letterbox_level());
    if (!(level > 0.0f)) return;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float bar = std::clamp(cine_.letterbox_height, 0.0f, 0.5f) * level * fh;
    if (!(bar > 0.0f)) return;
    ui::HudQuad top, bottom;
    top.min = canvas.pixel_to_canvas(0.0f, bar);
    top.max = canvas.pixel_to_canvas(fw, 0.0f);
    bottom.min = canvas.pixel_to_canvas(0.0f, fh);
    bottom.max = canvas.pixel_to_canvas(fw, fh - bar);
    top.color = bottom.color = {0, 0, 0, 255};
    dl.add(0, ui::HudList::Alpha, top);
    dl.add(0, ui::HudList::Alpha, bottom);
}

}  // namespace alamo_game_app
