// Scaffolding shared by the campaign-battle test executables (test_battles.cpp, test_battle_l1.cpp): the synthetic data set
// holder, the load tail every make_data() ends with, the type lookup and the three-planet galaxy both setups start from.
// Tests only; needs g_fail / CHECK from alamo/core/test_support.hpp (included here).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/battle_catalog.hpp"
#include "alamo/vfs/vfs.hpp"

namespace alamo::sim::battle_fixture {

struct Data {
    std::filesystem::path root;
    std::unique_ptr<DirectoryProvider> files;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::unique_ptr<BattleCatalog> catalog;
};

// Opens `d.root` as the data tree and loads the database, types, galactic / conflict rules and the battle catalog from it.
inline void load_data(Data& d) {
    d.files = std::make_unique<DirectoryProvider>(path_to_utf8(d.root));
    std::vector<std::string> diag;
    CHECK(d.db.load(*d.files, {}, &diag));
    CHECK(d.raw.load(*d.files));
    CHECK(d.types.build(d.raw, d.files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*d.files, d.db, d.types, d.rules, &diag));
    CHECK(galactic::load_conflict_rules(*d.files, d.db, d.types, d.rules, d.crules, &diag));
    d.catalog = std::make_unique<BattleCatalog>(d.types, d.files.get(), &d.db.constants(), &diag);
}

inline uint32_t type_of(const Data& d, const char* name) { return static_cast<uint32_t>(d.types.resolve_name(name)); }

// Planets S10_A, S10_B, S10_C in a row joined by the routes A_B and B_C, with their planet types.
inline void triangle_map(galactic::GalacticSetup& s, const Data& d, const char* campaign_name) {
    GalaxyMap& m = s.map;
    m.campaign_name = campaign_name;
    const char* names[] = {"S10_A", "S10_B", "S10_C"};
    for (uint32_t i = 0; i < 3; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
        m.planets.flags[i] = static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b) {
        const uint32_t r = m.routes.add(n);
        m.routes.point_a[r] = a;
        m.routes.point_b[r] = b;
        m.routes.hs_speed_factor[r] = 5.0f;
    };
    route("A_B", 0, 1);
    route("B_C", 1, 2);
    s.planet_type = {type_of(d, "S10_A"), type_of(d, "S10_B"), type_of(d, "S10_C")};
}

}  // namespace alamo::sim::battle_fixture
