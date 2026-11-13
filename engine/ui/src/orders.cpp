#include "alamo/ui/orders.hpp"

#include <algorithm>

#include "alamo/commands/pipeline.hpp"
#include "alamo/core/dmath.hpp"

namespace alamo::ui {

namespace ev = commands::ev;

namespace {
void sort_unique(std::vector<uint32_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
const PickSphere* find_item(std::span<const PickSphere> items, uint32_t id) {
    for (const PickSphere& s : items)
        if (s.id == id) return &s;
    return nullptr;
}
}  // namespace

void OrderTranslator::set_selection(std::vector<uint32_t> ids) {
    sort_unique(ids);
    sel_ = std::move(ids);
}

void OrderTranslator::emit_select(std::vector<uint32_t> ids) {
    sort_unique(ids);
    if (ids.size() > kMaxSelect) ids.resize(kMaxSelect);
    sel_ = ids;
    pending_.push_back(ev::select(std::move(ids)));
}

bool OrderTranslator::is_selectable(const OrderContext& ctx, uint32_t id) const {
    if (ctx.selectable) return ctx.selectable(id);
    if (ctx.mode == UiMode::Galactic) {
        const PickSphere* s = find_item(ctx.galactic_items, id);
        return s && s->kind != PickKind::Planet && s->owner == ctx.local_player;
    }
    const tactical::TreeMembership* m = ctx.trees ? ctx.trees->membership(id) : nullptr;
    return m && m->owner == ctx.local_player;
}

bool OrderTranslator::is_hostile(const OrderContext& ctx, uint32_t id) const {
    if (ctx.hostile) return ctx.hostile(id);
    int32_t owner = -1;
    if (ctx.mode == UiMode::Galactic) {
        const PickSphere* s = find_item(ctx.galactic_items, id);
        owner = s ? s->owner : -1;
    } else if (const tactical::TreeMembership* m = ctx.trees ? ctx.trees->membership(id) : nullptr) {
        owner = m->owner;
    }
    return owner >= 0 && owner != ctx.local_player;
}

bool OrderTranslator::ground_point(const OrderContext& ctx, int32_t px, int32_t py, Vec3& out) const {
    const tactical::Ray r = pixel_ray(ctx.view, px, py);
    if (!ray_plane_z(r, ctx.ground_z, out)) return false;
    if (ctx.ground_height) {
        for (int i = 0; i < 4; ++i) {  // fixed iteration count [port]
            Vec3 p;
            if (!ray_plane_z(r, ctx.ground_height(out.x, out.y), p)) break;
            out = p;
        }
    }
    return true;
}

void OrderTranslator::update(const InputState& in, const Bindings& keys, const OrderContext& ctx) {
    queue_ = keys.queue_held(in);  // [port: Gabriel]
    if (ctx.mode == UiMode::Galactic) {
        galactic(in, ctx);
    } else {
        tactical(in, ctx);
    }
    sync_meta();
}

void OrderTranslator::tactical(const InputState& in, const OrderContext& ctx) {
    if (!ctx.trees) return;
    const tactical::SpatialTree& all = ctx.trees->all();
    const ButtonState& lb = in.button(Button::Left);
    const ButtonState& rb = in.button(Button::Right);
    // q6 t19 (OBJECT_SCRIPTS §10.3): the box pick 0x267680 skips hidden models as the point pick does, so the pick
    // tests' skip (garrisoned / Lua-hidden objects) applies to box and double-click selection too.
    const auto box_skipped = [&ctx](uint32_t id) { return ctx.pick_tests && ctx.pick_tests->skip && ctx.pick_tests->skip(id); };

    if (lb.drag_ended) {
        std::vector<uint32_t> ids;
        if (ctx.local_player >= 0 && static_cast<uint32_t>(ctx.local_player) < ctx.trees->player_count()) {
            const auto& tree = ctx.trees->player(static_cast<uint32_t>(ctx.local_player));
            for (uint32_t id : box_select(tree, ctx.view, lb.drag, ctx.pick_filter, qctx_))
                if (!box_skipped(id) && is_selectable(ctx, id)) ids.push_back(id);
        }
        if (in.shift()) ids.insert(ids.end(), sel_.begin(), sel_.end());
        if (!ids.empty() || !sel_.empty()) emit_select(std::move(ids));
    } else if (lb.click) {
        const auto hit = pick_world(all, ctx.view, pixel_ray(ctx.view, in.mouse_x(), in.mouse_y()), ctx.pick_filter, qctx_, ctx.pick_tests);
        if (hit && is_selectable(ctx, hit->id)) {
            if (lb.double_click && ctx.type_of) {
                const uint32_t type = ctx.type_of(hit->id);
                const DragRect screen{0, 0, static_cast<int32_t>(ctx.view.width) - 1, static_cast<int32_t>(ctx.view.height) - 1};
                std::vector<uint32_t> ids;
                const auto& tree = ctx.trees->player(static_cast<uint32_t>(ctx.local_player));
                for (uint32_t id : box_select(tree, ctx.view, screen, ctx.pick_filter, qctx_))
                    if (!box_skipped(id) && is_selectable(ctx, id) && ctx.type_of(id) == type) ids.push_back(id);
                if (in.shift()) ids.insert(ids.end(), sel_.begin(), sel_.end());
                emit_select(std::move(ids));
            } else if (in.shift()) {
                std::vector<uint32_t> ids = sel_;
                const auto it = std::find(ids.begin(), ids.end(), hit->id);
                if (it != ids.end()) {
                    ids.erase(it);
                } else {
                    ids.push_back(hit->id);
                }
                emit_select(std::move(ids));
            } else {
                emit_select({hit->id});
            }
        } else if (alternate_ && !sel_.empty()) {
            // ui1 t02 [port] alternate scheme (P1_DOUBLE_CLICK_MOVE §1.2: ground orders on the left button): a left click
            // that does not hit an own selectable object is the order click; the release of a double press is swallowed.
            if (!lb.in_double_press) {
                if (!((in.alt() && !in.ctrl()) || toggle_waypoint_)) waypoint_mode_ = false;
                right_release(in, ctx);
            }
        } else if (!hit && !in.shift()) {  // P1_CAMERA_INPUT.md §5: a plain click on nothing submits Select{} [port: Shift]
            emit_select({});
        }
    }
    if (alternate_) {
        if (!((in.alt() && !in.ctrl()) || toggle_waypoint_)) waypoint_mode_ = false;
        if (sel_.empty()) return;
        if (lb.double_press) {
            // The double click of the order button acts at the press (action 2 in the alternate table), unless the press
            // is on an own selectable object (that double click selects every one of its type on the release).
            const auto hit = pick_world(all, ctx.view, pixel_ray(ctx.view, in.mouse_x(), in.mouse_y()), ctx.pick_filter, qctx_, ctx.pick_tests);
            if (!(hit && is_selectable(ctx, hit->id))) right_double(in, ctx);
        } else if (rb.click && !rb.in_double_press) {
            emit_select({});  // [port] a right click clears the selection (the alternate table's right actions 3..5 are not recovered)
        }
        return;
    }

    // Waypoint mode lasts while Alt (without Ctrl) or the waypoint toggle is held (spec §2.1 step 3).
    if (!((in.alt() && !in.ctrl()) || toggle_waypoint_)) waypoint_mode_ = false;
    if (sel_.empty()) return;
    if (rb.double_press) {
        right_double(in, ctx);
    } else if (rb.released && !rb.in_double_press) {  // the release after a double click is swallowed (spec §1.1)
        right_release(in, ctx);
    }
}

void OrderTranslator::arm_move_toggle(MoveToggle t) {
    switch (t) {
        case MoveToggle::AttackMove: toggle_attack_ = true; break;
        case MoveToggle::GuardMove: toggle_guard_ = true; break;
        case MoveToggle::Waypoint: toggle_waypoint_ = true; break;
    }
}

uint8_t OrderTranslator::move_flags(const InputState& in, bool dbl) {
    using namespace move_flag;
    uint8_t f = kPlayerOrder;
    const bool guard = (in.ctrl() && in.alt()) || toggle_guard_;
    if (guard) {
        f |= kGuardMove;
    } else if (in.ctrl() || toggle_attack_) {
        f |= kAttackMove;
    }
    if (dbl) f |= kDoubleClick;
    const bool alt_run = (in.alt() && !in.ctrl()) || toggle_waypoint_;
    if (waypoint_mode_) f |= kWaypoint;
    waypoint_mode_ = alt_run;  // the first Alt order starts the run; any other order ends it
    if (queue_) f |= kWaypoint;  // [port: Gabriel] the queue modifier appends too
    return f;
}

// Order button release (default scheme action 13 -> `0x41f70`): the cursor action at the release point.
void OrderTranslator::right_release(const InputState& in, const OrderContext& ctx) {
    const auto hit = pick_world(ctx.trees->all(), ctx.view, pixel_ray(ctx.view, in.mouse_x(), in.mouse_y()), ctx.pick_filter, qctx_, ctx.pick_tests);
    if (hit) {
        waypoint_mode_ = false;  // any other order ends an Alt run
        if (is_hostile(ctx, hit->id)) {
            commands::EventData e = ev::attack(hit->id, sel_);
            e.set("flag1", in.ctrl() ? 1u : 0u);  // +0x51 Ctrl (spec §3.4)
            push_order(std::move(e), queue_);
        } else {
            push_order(ev::move_to_object(hit->id), queue_);
        }
        return;
    }
    ground_move(in, ctx, in.mouse_x(), in.mouse_y(), false);
}

// Double click of the order button (action 14, spec §1.3): acts at the press point; tactical modes only.
void OrderTranslator::right_double(const InputState& in, const OrderContext& ctx) {
    const auto hit = pick_world(ctx.trees->all(), ctx.view, pixel_ray(ctx.view, in.mouse_x(), in.mouse_y()), ctx.pick_filter, qctx_, ctx.pick_tests);
    if (hit) {
        if (!is_hostile(ctx, hit->id)) return;  // a double click on a friendly object adds nothing
        waypoint_mode_ = false;
        commands::EventData e = ev::attack(hit->id, sel_);
        e.set("flag0", 1u).set("flag1", in.ctrl() ? 1u : 0u);  // +0x50 "double", +0x51 Ctrl
        push_order(std::move(e), queue_, false, true);  // [port] no second voice
        return;
    }
    ground_move(in, ctx, in.mouse_x(), in.mouse_y(), true);
}

// `0x3b360`: one move order at pixel (px, py), its flags and its acknowledgement (spec §2).
void OrderTranslator::ground_move(const InputState& in, const OrderContext& ctx, int32_t px, int32_t py, bool dbl) {
    const ButtonState& rb = in.button(alternate_ ? Button::Left : Button::Right);  // ui1 t02: the order button
    // Facing variant: a press anchor and a Chebyshev drag > MinimumDragDistance. A double click never has an anchor.
    const int32_t ddx = rb.drag.x1 - rb.drag.x0, ddy = rb.drag.y1 - rb.drag.y0;
    const int32_t cheb = std::max(ddx < 0 ? -ddx : ddx, ddy < 0 ? -ddy : ddy);
    const bool facing = !dbl && static_cast<float>(cheb) > in.config().drag_threshold_px;
    auto angle_deg = [](const Vec3& from, const Vec3& to) {
        return dmath::atan2(to.y - from.y, to.x - from.x) * (180.0f / 3.14159265358979323846f);
    };
    commands::EventData e;
    MoveAck ack;
    ack.space = ctx.mode == UiMode::Space;
    ack.double_click = dbl;
    if (ctx.mode == UiMode::Land) {
        Vec3 p;
        if (!ground_point(ctx, px, py, p)) return;
        if (facing) {
            Vec3 a;
            if (!ground_point(ctx, rb.drag.x0, rb.drag.y0, a)) return;
            const float ang = angle_deg(a, p);
            const auto on = pick_world(ctx.trees->all(), ctx.view, pixel_ray(ctx.view, rb.drag.x0, rb.drag.y0), ctx.pick_filter, qctx_, ctx.pick_tests);
            if (on && std::binary_search(sel_.begin(), sel_.end(), on->id)) {
                e = commands::EventData(commands::EventType::Facing);  // rotate only
                e.set_f("angle", ang).set_ids("units", std::vector<uint32_t>{});
            } else {
                e = commands::EventData(commands::EventType::MoveToPositionFacing);
                e.set_f("x", a.x).set_f("y", a.y).set_f("z", a.z).set_f("angle", ang).set_ids("units", std::vector<uint32_t>{});
            }
            ack.point = a;
        } else {
            e = ev::move_to_position(p.x, p.y, p.z);
            ack.point = p;
        }
    } else {
        const tactical::Ray r = pixel_ray(ctx.view, px, py);
        Vec3 p;
        if (!ray_plane_z(r, ctx.ground_z, p)) return;
        if (facing) {
            const tactical::Ray ra = pixel_ray(ctx.view, rb.drag.x0, rb.drag.y0);
            Vec3 a;
            if (!ray_plane_z(ra, ctx.ground_z, a)) return;
            e = commands::EventData(commands::EventType::MoveToRayFacing);
            e.set_f("ax", ra.origin.x).set_f("ay", ra.origin.y).set_f("az", ra.origin.z);
            e.set_f("bx", a.x).set_f("by", a.y).set_f("bz", a.z).set_f("angle", angle_deg(a, p));
            ack.point = a;
        } else {
            e = commands::EventData(commands::EventType::MoveToRay);
            e.set_f("ax", r.origin.x).set_f("ay", r.origin.y).set_f("az", r.origin.z);
            e.set_f("bx", p.x).set_f("by", p.y).set_f("bz", p.z);
            ack.point = p;
        }
    }
    const uint8_t flags = move_flags(in, dbl);
    if (e.type() != commands::EventType::Facing) e.set("flags", flags);
    ack.attack = (flags & move_flag::kAttackMove) != 0;
    ack.guard = (flags & move_flag::kGuardMove) != 0;
    toggle_attack_ = toggle_guard_ = toggle_waypoint_ = false;  // one-shot toggles
    push_order(std::move(e), (flags & move_flag::kWaypoint) != 0, !dbl, dbl);
    acks_.push_back(ack);
}

AckEffect MoveAck::effect() const {
    if (attack) return AckEffect::AttackMove;
    if (guard) return AckEffect::GuardMove;
    return double_click ? AckEffect::DoubleClickMove : AckEffect::Move;
}

AckRadar MoveAck::radar() const {
    if (attack) return AckRadar::AttackClick;
    return double_click ? AckRadar::DoubleClick : AckRadar::Click;
}

std::string_view ack_effect_constant(AckEffect e) {
    switch (e) {
        case AckEffect::DoubleClickMove: return "GUI_Double_Click_Move_Command_Ack_Effect";
        case AckEffect::AttackMove: return "GUI_Attack_Move_Command_Ack_Effect";
        case AckEffect::GuardMove: return "GUI_Guard_Move_Command_Ack_Effect";
        case AckEffect::Move: break;
    }
    return "GUI_Move_Command_Ack_Effect";
}

std::string_view ack_radar_constant(AckRadar r) {
    switch (r) {
        case AckRadar::DoubleClick: return "GUI_Movement_Double_Click_Radar_Event_Name";
        case AckRadar::AttackClick: return "GUI_Attack_Movement_Click_Radar_Event_Name";
        case AckRadar::Click: break;
    }
    return "GUI_Movement_Click_Radar_Event_Name";
}

void OrderTranslator::galactic(const InputState& in, const OrderContext& ctx) {
    const ButtonState& lb = in.button(Button::Left);
    const ButtonState& rb = in.button(Button::Right);
    if (!lb.click && !rb.click) return;
    const tactical::Ray ray = pixel_ray(ctx.view, in.mouse_x(), in.mouse_y());
    const auto idx = pick_spheres(ctx.galactic_items, ray);
    const PickSphere* hit = idx ? &ctx.galactic_items[*idx] : nullptr;
    if (lb.click) {
        if (hit && hit->kind == PickKind::Planet) {
            if (lb.double_click) planet_zoom_ = hit->id;
        } else if (hit && is_selectable(ctx, hit->id)) {
            emit_select({hit->id});
        } else if (!hit) {  // Select{} and the HUD planet selection is cleared (§5; the host checks the zoom state)
            emit_select({});
            planet_deselect_ = true;
        }
    }
    if (rb.click && hit && hit->kind == PickKind::Planet) {
        for (uint32_t fleet : sel_) push_order(ev::move_object_to_object(fleet, hit->id), queue_);
    }
}

size_t OrderTranslator::submit(commands::CommandPipeline& pipe, uint8_t player) {
    size_t ok = 0;
    for (commands::EventData& e : pending_)
        if (pipe.submit(player, commands::CommandSource::Player, std::move(e)).ok()) ++ok;
    pending_.clear();
    pending_queued_.clear();
    pending_voice_.clear();
    pending_double_.clear();
    return ok;
}

void OrderTranslator::sync_meta() {
    pending_queued_.resize(pending_.size(), 0);
    pending_voice_.resize(pending_.size(), 1);
    pending_double_.resize(pending_.size(), 0);
}

void OrderTranslator::push_order(commands::EventData e, bool queued, bool voice, bool dbl) {
    sync_meta();
    pending_.push_back(std::move(e));
    pending_queued_.push_back(queued ? 1 : 0);
    pending_voice_.push_back(voice ? 1 : 0);
    pending_double_.push_back(dbl ? 1 : 0);
}

std::vector<UiOrder> OrderTranslator::take_orders() {
    sync_meta();
    std::vector<UiOrder> out;
    for (size_t i = 0; i < pending_.size(); ++i)
        out.push_back({std::move(pending_[i]), pending_queued_[i] != 0, pending_voice_[i] != 0, pending_double_[i] != 0});
    pending_.clear();
    pending_queued_.clear();
    pending_voice_.clear();
    pending_double_.clear();
    return out;
}

commands::EventData pause_event(bool pause) {
    const uint8_t b = pause ? 1 : 0;
    return ev::port_extension(kPortPauseSubtype, std::span<const uint8_t>(&b, 1));
}

bool read_pause_event(const commands::EventData& e, bool& pause) {
    if (e.type() != commands::EventType::PortExtension || e.u("subtype") != kPortPauseSubtype) return false;
    const std::span<const uint8_t> d = e.bytes_view("data");
    if (d.size() != 1) return false;
    pause = d[0] != 0;
    return true;
}

}  // namespace alamo::ui
