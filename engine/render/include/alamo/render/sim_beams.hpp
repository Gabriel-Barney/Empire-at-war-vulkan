// alamo_render (lib alamo_render_sim) - R5 ability beams, sim side of the pipeline (docs/re/ABILITY_VISUALS.md 2, 4, 6):
//   * BeamStyleTable: the beam fields of every special ability of the game's types (Beam_Color / Beam_Width / Beam_Frames /
//     Beam_Texture[_Name], Line_*, Laser_*, Owner_Particle_Bone_Name, Bone_Names ...) resolved over the GameConstants
//     fallbacks (4.2), keyed by the special ability's name CRC (= UnitAbilityRecord::gui_name_crc), plus the per-type
//     Ranged_Target_Z_Adjust and <HardPoints> names (the energy weapon ends at the targeted hardpoint's bone [M: the
//     bone is named like the hardpoint]).
//   * beams_from_frame: SnapshotBeam records of an InterpolatedFrame -> world BeamInstances: start = the owner's bone
//     (HP_trac_bone for tractor / energy weapon, the first Owner_Particle_Bone_Name for the super laser, the Bone_Names
//     bone nearest the destroyed projectile for the zap, Beam_Bone_Name for leech) else the owner position; end = the
//     target centre + Ranged_Target_Z_Adjust, the targeted hardpoint's bone (energy weapon), or the record's fixed point.
//     Beams whose owner or target is hidden from the local player (fog / stealth) or missing from the frame are left out.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/render/beams.hpp"
#include "alamo/render/sim_snapshot.hpp"

namespace alamo::render {

class BeamStyleTable {
public:
    const BeamConstants& constants() const { return gc_; }
    // Reads the specials (<Abilities> sub-objects: Tractor_Beam_Attack_Ability, Energy_Weapon_Attack_Ability,
    // Super_Laser_Ability, Laser_Defense_Ability, Leech_Shields_Ability), Ranged_Target_Z_Adjust and the <HardPoints> names
    // of every type of `types`; replaces earlier content. `gc` = the fallbacks.
    void build(const objects::GameObjectTypes& types, const BeamConstants& gc);
    // Tests / tools.
    void add(uint32_t special_name_crc, BeamStyle style) { styles_[special_name_crc] = std::move(style); }
    void set_z_adjust(objects::TypeIndex t, float z);
    void set_hardpoint_bones(objects::TypeIndex t, std::vector<std::string> bones);
    const BeamStyle* find(uint32_t special_name_crc) const;
    // The listed style of the special, else the GC-only default of `kind`.
    BeamStyle style_for(uint8_t kind, uint32_t special_name_crc) const;
    float z_adjust(objects::TypeIndex t) const { return t < z_adjust_.size() ? z_adjust_[t] : 0.0f; }
    // Bone name of hardpoint `index` of type `t` ("" unknown).
    std::string_view hardpoint_bone(objects::TypeIndex t, uint32_t index) const;
    size_t size() const { return styles_.size(); }

private:
    BeamConstants gc_;
    std::map<uint32_t, BeamStyle> styles_;  // special name CRC -> style
    std::vector<float> z_adjust_;           // per type
    std::vector<std::vector<std::string>> hardpoint_bones_;  // per type, HardPoints order
};

// World position of a named bone of an object (the SimDecorations::BoneWorldFn contract: model_bone_source()); false when
// the object's model lacks the bone.
using BeamBoneFn = std::function<bool(const SnapshotObject& o, std::string_view bone, Vec3& out)>;

// The beams of `frame` with world endpoints (record order). `bone` may be empty (every start is the owner position, the
// energy weapon ends at the target centre).
std::vector<BeamInstance> beams_from_frame(const InterpolatedFrame& frame, const BeamStyleTable* styles, const BeamBoneFn& bone);

// Stable identity of a beam across frames: owner id and kind; a zap also carries its battle frame (one-shot).
uint64_t beam_key(const SnapshotBeam& b);

#ifdef ALAMO_SIM_HAVE_BATTLES
}  // namespace alamo::render
namespace alamo::tactical::abilities {
class AbilitySystem;
}
namespace alamo::tactical::combat {
class CombatSystem;
}
namespace alamo::render {
// apply_battle_beams on the two systems directly (tests): snap.beams from the abilities' applied specials; `frame` = the
// battle frame stamped on the records.
void apply_beams(const tactical::abilities::AbilitySystem& ab, const tactical::combat::CombatSystem& combat, uint32_t frame,
                 SimSnapshot& snap, const BeamStyleTable* styles = nullptr);
// The laser-defense zaps of this tick, derived from two consecutive snapshots (the host's tick clears AbilityEvent::Zap
// before the snapshot is taken, and the event carries no position): a projectile of `prev` that is gone from `snap`, enemy
// to an owner with an applied Laser_Defense_Ability, one flight step from its last position inside Protection_Radius of
// that owner, and left no hit event of its shooter there. Appends kind-56 fixed-end records (the exact position the sim
// zapped it at, up to the move order [M]); owners that already have one are skipped. `prev` null: nothing.
void apply_beam_zaps(const tactical::abilities::AbilitySystem& ab, const tactical::combat::CombatSystem& combat, uint32_t frame,
                     SimSnapshot& snap, const SimSnapshot* prev, const BeamStyleTable* styles = nullptr);
#endif

}  // namespace alamo::render
