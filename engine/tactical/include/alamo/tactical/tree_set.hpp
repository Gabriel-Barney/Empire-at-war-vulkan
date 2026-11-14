// S6 - the set of spatial trees GameObjectManager keeps (docs/re/TACTICAL_COMBAT.md 1.5):
//   player(p)       GOM+0xd0 + p*0x38: player p's projectile-collidable objects (targeting,
//                   opportunity fire, area damage, projectile hits, auras, AI threat queries)
//   all()           GOM+0xd8: projectile-collidable objects of every owner (radius query with
//                   player -1, placement search, explosions)
//   obstacle()      GOM+0xe0: objects some behaviour flags, impassable asteroids, or collidable +
//                   Is_Valid_Target ([L] purpose not pinned; kept for parity)
//   space_avoidance() GOM+0x558, space mode: fighter locomotor avoidance set
// Render scene trees are separate SpatialTree instances owned by the renderer.
//
// Membership is decided by S1 from type data (Collidable_By_Projectile_Living / _Dead depending on
// the dead state, owner, the 0x39ac80 predicate, space-layer rules) and passed in as a
// TreeMembership; this class only keeps the trees consistent with it. Owner changes
// (ObjectOwnerChanged) and collidability changes go through set_membership / set_owner.
// Handles are gameplay object ids (see spatial_tree.hpp).
//
// maintain() visits the trees in a fixed "registry" order: all, players ascending, obstacle,
// space_avoidance. Rebuild order cannot change results; it is fixed anyway.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "alamo/tactical/spatial_tree.hpp"

namespace alamo::tactical {

struct TreeMembership {
    int32_t owner = -1;                 // player id; outside [0, player_count): no player tree
    bool projectile_collidable = true;  // player tree + all() (Collidable_By_Projectile_Living/_Dead)
    bool obstacle = false;              // obstacle() tree (GOM+0xe0)
    bool space_avoidance = false;       // space_avoidance() tree (GOM+0x558)
};

class TreeSet {
public:
    explicit TreeSet(uint32_t player_count, SpatialTreeConfig config = {});

    uint32_t player_count() const noexcept { return static_cast<uint32_t>(players_.size()); }
    const SpatialTree& player(uint32_t p) const { return players_[p]; }
    const SpatialTree& all() const noexcept { return all_; }
    const SpatialTree& obstacle() const noexcept { return obstacle_; }
    const SpatialTree& space_avoidance() const noexcept { return space_; }
    // Player tree for p, or all() for p < 0 (the GOM radius query's "player -1").
    const SpatialTree& for_player(int32_t p) const;

    // false if the id is already present / invalid, or the box is not valid().
    bool add(EntityHandle id, const Aabb& box, uint32_t mask, const TreeMembership& m);
    bool remove(EntityHandle id);
    bool update(EntityHandle id, const Aabb& box);
    // SP5: the same as update() for each entry in order (invalid boxes and unknown ids skipped); returns how many were
    // applied. The trees are independent, so with a parallel pf each tree takes its own entries in entry order on one
    // worker: every tree ends exactly as after the serial calls.
    struct BoxUpdate {
        EntityHandle id;
        Aabb box;
    };
    uint32_t update_batch(std::span<const BoxUpdate> updates, const ParallelFor& pf = serial_for);
    bool set_membership(EntityHandle id, const TreeMembership& m);
    bool set_owner(EntityHandle id, int32_t owner);
    bool set_mask(EntityHandle id, uint32_t mask);
    bool set_dead(EntityHandle id, bool dead = true);
    const TreeMembership* membership(EntityHandle id) const;
    uint32_t size() const noexcept { return static_cast<uint32_t>(objects_.size()); }

    // Original Rebuild_All_Dirty pass; returns the number of trees rebuilt.
    uint32_t maintain(double game_time, const ParallelFor& pf = serial_for);
    void rebuild_all(const ParallelFor& pf = serial_for);
    // Map load / GOM reinit (Rebuild_With_Threshold on every tree).
    void rebuild_all_with_threshold(float large_threshold, const ParallelFor& pf = serial_for);

    // Q1 hook: per tree in registry order, u32 tree index then that tree's hash_state stream.
    void hash_state(const std::function<void(const void* data, size_t len)>& sink) const;

    // ---- N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/tree_save.cpp) ----
    // For the host's "spatial" chunk (CrcRegistry "spatial" = hash_state of the top tactical mode's set).
    // Layout: u32 version, u32 player count (must equal player_count(): the host builds the set for the
    // same session first), per tree in registry order f32 large_threshold, then u32 object count and per
    // object by ascending id: u32 id, 6 x f32 box, u32 mask (incl. kDeadCategory), i32 owner, u8 flags
    // (1 projectile_collidable, 2 obstacle, 4 space_avoidance). Every tree's entries are a pure function of
    // these objects (add / update / set_mask / set_membership keep them so), so read() re-adds the objects
    // and rebuilds every tree: identical hash_state() and query results (see SpatialTree::write for why the
    // rebuilt shape is exact). read() returns false and leaves the set unchanged on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Object {
        Aabb box;
        uint32_t mask;
        TreeMembership m;
    };
    template <class Fn>
    void for_trees(const TreeMembership& m, Fn&& fn);
    template <class Fn>
    void for_all_trees(Fn&& fn);
    SpatialTree& tree_at(uint32_t k);  // registry order: all, players, obstacle, space_avoidance

    std::vector<SpatialTree> players_;
    SpatialTree all_, obstacle_, space_;
    std::unordered_map<EntityHandle, Object> objects_;  // lookup only, never iterated
};

}  // namespace alamo::tactical
