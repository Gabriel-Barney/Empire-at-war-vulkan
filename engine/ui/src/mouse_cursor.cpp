#include "alamo/ui/mouse_cursor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::ui {

namespace {

constexpr const char* kNames[kPointerCount] = {
    "POINTER_NORMAL", "POINTER_WAIT", "POINTER_SELECT", "POINTER_MOVE", "POINTER_CAN_GRAB", "POINTER_GRABBING", "POINTER_DRAG",
    "POINTER_DROP", "POINTER_COMBINE", "POINTER_CANT_SELECT", "POINTER_CANT_MOVE", "POINTER_CANT_DROP", "POINTER_CANT_GRAB",
    "POINTER_CANT_COMBINE", "POINTER_CANT_ATTACK", "POINTER_ATTACK", "POINTER_ATTACK_MOVE", "POINTER_ATTACK_OUT_OF_RANGE",
    "POINTER_ATTACK_WITH_SPECIAL_WEAPON_0", "POINTER_ATTACK_WITH_SPECIAL_WEAPON_1", "POINTER_ATTACK_WITH_SPECIAL_WEAPON_2",
    "POINTER_ATTACK_WITH_SPECIAL_WEAPON_INVALID", "POINTER_DRAG_SELECT_MODE", "POINTER_MAP_SCROLLING_MODE",
    "POINTER_MAP_ROTATING_MODE", "POINTER_MOVIE_MODE", "POINTER_WAYPOINT_FOLLOW", "POINTER_WAYPOINT_PLACEMENT",
    "POINTER_TACTICAL_BUILD", "POINTER_TACTICAL_SELL", "POINTER_GUARD", "POINTER_BARRAGE", "POINTER_PLACE_BEACON",
    "POINTER_BOMBING_RUN_TARGET", "POINTER_REINFORCEMENTS_LANDING_POINT", "POINTER_ATTACK_ONLY_MODE_UNIT_TARGETED",
    "POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED", "POINTER_MOVE_ONLY_MODE_PASSABLE_TARGETED",
    "POINTER_MOVE_ONLY_MODE_NO_PASSABLE_TARGETED", "POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT",
    "POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT_INVALID", "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT",
    "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT_INVALID", "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN",
    "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN_INVALID", "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION",
    "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION_INVALID", "POINTER_GARRISON_IN", "POINTER_GARRISON_OUT",
    "POINTER_TARGET_SPECIAL_ABILITY_TO_BRIBE_ENEMY_OBJECT", "POINTER_TARGET_SPECIAL_ABILITY_CANT_BRIBE_ENEMY_OBJECT",
    "POINTER_TARGET_SPECIAL_ABILITY_BLAST", "POINTER_REPAIR_HARDPOINT"};

}  // namespace

const char* pointer_name(Pointer p) {
    const auto i = static_cast<size_t>(p);
    return i < kPointerCount ? kNames[i] : "?";
}

bool pointer_from_name(std::string_view name, Pointer& out) {
    for (size_t i = 0; i < kPointerCount; ++i)
        if (iequals(name, kNames[i])) {
            out = static_cast<Pointer>(i);
            return true;
        }
    return false;
}

size_t CursorTable::parse(const data::XmlNode& root) {
    size_t n = 0;
    for (data::XmlNode m : root.children("MousePointer")) {
        CursorDef d;
        d.name = std::string(m.attribute("Name"));
        if (const data::XmlNode b = m.child("Base_Texture")) d.base_texture = std::string(b.text_trimmed());
        if (const data::XmlNode x = m.child("Hot_X")) d.hot_x = data::parse_int_or(x.text_trimmed(), 0);
        if (const data::XmlNode y = m.child("Hot_Y")) d.hot_y = data::parse_int_or(y.text_trimmed(), 0);
        if (const data::XmlNode fd = m.child("Anim_Frame_Delay")) d.frame_delay = data::parse_int_or(fd.text_trimmed(), 5);
        add(std::move(d));
        ++n;
    }
    return n;
}

void CursorTable::add(CursorDef d) {
    for (CursorDef& e : defs_)
        if (iequals(e.name, d.name)) {
            e = std::move(d);  // later duplicates replace
            return;
        }
    defs_.push_back(std::move(d));
}

size_t CursorTable::resolve_frames(const std::function<bool(std::string_view)>& exists, size_t max_frames) {
    size_t total = 0;
    for (CursorDef& d : defs_) {
        d.frames.clear();
        const std::string& b = d.base_texture;
        const size_t dot = b.find_last_of('.');
        const std::string stem = dot == std::string::npos ? b : b.substr(0, dot);
        const std::string ext = dot == std::string::npos ? std::string() : b.substr(dot);
        if (stem.size() >= 3 && stem[stem.size() - 3] == '_' && stem.substr(stem.size() - 2) == "00") {
            const std::string prefix = stem.substr(0, stem.size() - 2);
            for (size_t k = 0; k < max_frames; ++k) {
                char num[8];
                std::snprintf(num, sizeof num, "%02u", static_cast<unsigned>(k));
                const std::string n = prefix + num + ext;
                if (k > 0 && !(exists && exists(n))) break;
                d.frames.push_back(n);
            }
        } else {
            d.frames.push_back(b);
        }
        total += d.frames.size();
    }
    return total;
}

const CursorDef* CursorTable::find(std::string_view name) const {
    for (const CursorDef& d : defs_)
        if (iequals(d.name, name)) return &d;
    return nullptr;
}
const CursorDef* CursorTable::find(Pointer p) const { return find(pointer_name(p)); }

Pointer choose_pointer(const CursorContext& c) {
    using T = CursorContext::Targeting;
    using H = CursorContext::Hover;
    if (c.hidden || c.movie) return Pointer::MovieMode;
    if (c.busy) return Pointer::Wait;
    if (c.rotating) return Pointer::MapRotatingMode;
    if (c.scrolling) return Pointer::MapScrollingMode;
    if (c.fleet_drag) {
        if (c.drop_on_fleet) return c.drop_valid ? Pointer::Combine : Pointer::CantCombine;
        if (c.drop_on_planet) return c.drop_valid ? Pointer::Drop : Pointer::CantDrop;
        return Pointer::Grabbing;
    }
    if (c.over_ui) return c.over_draggable ? Pointer::CanGrab : Pointer::Normal;
    if (c.drag_selecting) return Pointer::DragSelectMode;
    const bool v = c.target_valid;
    switch (c.targeting) {
        case T::None: break;
        case T::AbilityEnemy: return v ? Pointer::AbilityEnemy : Pointer::AbilityEnemyInvalid;
        case T::AbilityFriendly: return v ? Pointer::AbilityFriendly : Pointer::AbilityFriendlyInvalid;
        case T::AbilityTerrain: return v ? Pointer::AbilityTerrain : Pointer::AbilityTerrainInvalid;
        case T::AbilitySpace: return v ? Pointer::AbilitySpace : Pointer::AbilitySpaceInvalid;
        case T::AbilityBribe: return v ? Pointer::AbilityBribe : Pointer::AbilityCantBribe;
        case T::AbilityBlast: return Pointer::AbilityBlast;
        case T::SpecialWeapon:
            if (!v) return Pointer::AttackWithSpecialWeaponInvalid;
            return c.special_weapon <= 0   ? Pointer::AttackWithSpecialWeapon0
                   : c.special_weapon == 1 ? Pointer::AttackWithSpecialWeapon1
                                           : Pointer::AttackWithSpecialWeapon2;
        case T::Beacon: return Pointer::PlaceBeacon;
        case T::Reinforcement: return v ? Pointer::ReinforcementsLandingPoint : Pointer::CantDrop;
        case T::BombingRun: return v ? Pointer::BombingRunTarget : Pointer::AttackWithSpecialWeaponInvalid;
        case T::TacticalBuild: return v ? Pointer::TacticalBuild : Pointer::CantMove;
        case T::TacticalSell: return Pointer::TacticalSell;
        case T::Waypoint: return Pointer::WaypointPlacement;
        case T::WaypointFollow: return Pointer::WaypointFollow;
        case T::Guard: return Pointer::Guard;
        case T::Barrage: return Pointer::Barrage;
        case T::Repair: return Pointer::RepairHardpoint;
        case T::GarrisonOut: return Pointer::GarrisonOut;
        case T::AttackMove: return Pointer::AttackMove;
        case T::MoveOnly: return c.passable ? Pointer::MoveOnlyPassableTargeted : Pointer::MoveOnlyNoPassableTargeted;
        case T::AttackOnly: return c.hover == H::Enemy ? Pointer::AttackOnlyUnitTargeted : Pointer::AttackOnlyNoUnitTargeted;
    }
    switch (c.hover) {
        case H::Enemy:
            if (!c.selection) return Pointer::Normal;
            if (!c.can_attack) return Pointer::CantAttack;
            return c.in_range ? Pointer::Attack : Pointer::AttackOutOfRange;
        case H::Own: return c.selectable ? Pointer::Select : Pointer::CantSelect;
        case H::Friendly:
            if (c.selection && c.garrisonable) return Pointer::GarrisonIn;
            return Pointer::Normal;
        case H::Ground:
            if (!c.selection) return Pointer::Normal;
            return c.passable ? Pointer::Move : Pointer::CantMove;
        default: return Pointer::Normal;
    }
}

void MouseCursor::init(const CursorTable& table, const TextureAtlas* atlas, GuiImageResolver images) {
    table_ = &table;
    atlas_ = atlas;
    images_ = std::move(images);
}

void MouseCursor::update(Pointer p, double now) {
    if (p == current_) return;
    current_ = p;
    since_ = now;
}

size_t MouseCursor::frame(double now) const {
    const CursorDef* d = table_ ? table_->find(current_) : nullptr;
    if (!d || d->frames.size() <= 1) return 0;
    const double per = static_cast<double>(std::max(1, d->frame_delay)) / 30.0;
    const double t = std::max(0.0, now - since_);
    return static_cast<size_t>(std::floor(t / per)) % d->frames.size();
}

std::string MouseCursor::texture(double now) const {
    const CursorDef* d = table_ ? table_->find(current_) : nullptr;
    if (!d) d = table_ ? table_->find(Pointer::Normal) : nullptr;
    if (!d) return {};
    if (d->frames.empty()) return d->base_texture;
    return d->frames[std::min(frame(now), d->frames.size() - 1)];
}

bool MouseCursor::build(HudDrawList& dl, const HudCanvas& cv, Vec2f px, double now) const {
    const CursorDef* d = table_ ? table_->find(current_) : nullptr;
    if (!d) d = table_ ? table_->find(Pointer::Normal) : nullptr;
    if (!d) return false;
    auto lookup = [&](const std::string& tex) {
        GuiImage img;
        if (atlas_)
            if (const AtlasEntry* e = atlas_->find(tex)) {
                img.valid = true;
                img.page = kHudPageAtlas;
                const float tw = static_cast<float>(atlas_->texture_width), th = static_cast<float>(atlas_->texture_height);
                img.uv0 = {static_cast<float>(e->x) / tw, static_cast<float>(e->y) / th};
                img.uv1 = {static_cast<float>(e->x + e->width) / tw, static_cast<float>(e->y + e->height) / th};
                img.size = {static_cast<float>(e->width), static_cast<float>(e->height)};
            }
        if (!img.valid && images_) img = images_(tex);
        if (img.size.x <= 0 || img.size.y <= 0) img.valid = false;
        return img;
    };
    GuiImage img = lookup(texture(now));
    // [port] a frame whose image page is not uploaded yet (requested this frame) shows the first frame, else the
    // normal pointer, so the cursor never blinks out while its pages load.
    if (!img.valid && !d->frames.empty()) img = lookup(d->frames.front());
    if (!img.valid)
        if (const CursorDef* n = table_->find(Pointer::Normal); n && n != d) {
            img = lookup(n->frames.empty() ? n->base_texture : n->frames.front());
            if (img.valid) d = n;
        }
    if (!img.valid) return false;
    // Top-left of the texture at (cursor - hot spot), in pixels; constant pixel size.
    const float x0 = px.x - static_cast<float>(d->hot_x), y0 = px.y - static_cast<float>(d->hot_y);
    const Vec2f a = cv.pixel_to_canvas(x0, y0), b = cv.pixel_to_canvas(x0 + img.size.x, y0 + img.size.y);
    HudQuad q;
    q.min = {std::min(a.x, b.x), std::min(a.y, b.y)};
    q.max = {std::max(a.x, b.x), std::max(a.y, b.y)};
    q.uv0 = img.uv0;
    q.uv1 = img.uv1;
    q.page = img.page;
    dl.add(kHudLayers - 1, HudList::Alpha, q);
    return true;
}

}  // namespace alamo::ui
