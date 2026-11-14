// Part 3 of test_ai_galactic (bo3 t03: split by position; see test_ai_galactic_common.hpp).
#include "test_ai_galactic_common.hpp"

gal::GalacticRules uw_rules() {
    gal::GalacticRules r;
    gal::FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    gal::FactionRules uw = emp;
    uw.name = "Underworld";
    gal::FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, uw, neu};
    r.difficulties = {{"Normal_Default", 1.0f, 1.0f}};
    r.types.resize(uTypes);
    r.types[uPlanet].name = "UwPlanet";
    r.types[uPlanet].kind = gal::UnitKind::Planet;
    r.types[uPlanet].credit_value = 60;
    r.types[uStruct].name = "E_Ground_Research_Facility";
    r.types[uStruct].kind = gal::UnitKind::GroundStructure;
    r.types[uStruct].build_cost = 1200;
    r.types[uDeathStar].name = "Death_Star_II";
    r.types[uDeathStar].kind = gal::UnitKind::SpaceUnit;
    r.types[uDeathStar].ai_combat_power = 20000.0f;
    for (const auto& [t, name] : {std::pair{uTyber, "Tyber_Zann_Team"}, std::pair{uSaboteur, "Underworld_Saboteur_Team"},
                                  std::pair{uIG88, "IG88_Team"}}) {
        gal::TypeInfo& h = r.types[t];
        h.name = name;
        h.kind = gal::UnitKind::GroundUnit;  // heroes with their own transport (synthetic)
        h.hyperspace = true;
        h.hyperspace_speed = 2.0f;
        h.ai_combat_power = 300.0f;
    }
    r.types[uSaboteur].build_cost = 600;  // the generic hero is buildable (vanilla: the Underworld builds saboteurs)
    r.types[uSaboteur].build_time_seconds = 15;
    r.types[uSaboteur].affiliation = {1};
    r.types[uSaboteur].required_ground_base = 1;
    gal::SpecialRules& s = r.special;
    auto ability = [&s](gal::SpecialAbilityClass cls, const char* name, bool despawn) -> uint32_t {
        gal::SpecialAbilityDef a;
        a.cls = cls;
        a.name = name;
        a.name_crc = gal::special_name_crc(name);
        a.style = gal::ActivationStyle::GroundActivated;
        a.causes_despawn = despawn;
        s.abilities.push_back(a);
        return static_cast<uint32_t>(s.abilities.size() - 1);
    };
    const uint32_t bm = ability(gal::SpecialAbilityClass::BlackMarket, "Tyber_Black_Market", true);
    s.abilities[bm].bm_min_respawn = {20.0f, 20.0f, 20.0f, 20.0f, 20.0f};
    s.abilities[bm].bm_max_respawn = {40.0f, 40.0f, 40.0f, 40.0f, 40.0f};
    const uint32_t sab = ability(gal::SpecialAbilityClass::Sabotage, "Saboteur_Galactic_Sabotage", true);
    s.abilities[sab].sabotage_can[gal::kSabotageSpecialStructure] = true;  // the vanilla saboteur: only this option
    const uint32_t cor = ability(gal::SpecialAbilityClass::Corruption, "Saboteur_Corruption", true);
    const uint32_t hack = ability(gal::SpecialAbilityClass::Hack, "IG88_Hack_Death_Star", false);
    s.activated_sabotage = {"Saboteur_Galactic_Sabotage"};
    s.types.resize(uTypes);
    s.types[uTyber].abilities = {bm};
    s.types[uTyber].named_hero = true;
    s.types[uSaboteur].abilities = {sab, cor};
    s.types[uSaboteur].generic_hero = true;
    s.types[uIG88].abilities = {hack};
    s.types[uIG88].named_hero = true;
    s.types[uStruct].sabotage_cost = 1000;
    gal::CorruptionChoiceDef bmc;  // Corruption_1_Type = Black_Market (6), the only choice
    bmc.slot = 1;
    bmc.type = 6;
    bmc.credit_cost = 500.0f;
    s.types[uPlanet].corruption = {bmc};
    s.types[uPlanet].black_market_modifier = 100.0f;
    for (const auto& [name, tech, cost] : {std::tuple{"Uw_Item_Low", 1, 1000}, std::tuple{"Uw_Item_High", 3, 2000}}) {
        gal::BlackMarketItemDef it;
        it.name = name;
        it.name_crc = gal::special_name_crc(name);
        it.faction = 0;  // Empire items: offered on Empire planets
        it.tech_level = tech;
        it.base_cost = cost;
        it.uncommon_price_multiplier = 5.5f;
        s.items.push_back(it);
    }
    s.default_hero_respawn_time = 30.0f;
    s.hack_required_type = uDeathStar;
    return r;
}

gal::GalacticSetup uw_setup() {
    gal::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SyntheticUnderworld";
    for (PlanetIndex i = 0; i < kUwPlanets; ++i) {
        m.planets.add("U" + std::to_string(i));
        m.planets.position[i] = {static_cast<float>(i) * 50.0f, 0.0f, 0.0f};
        m.planets.flags[i] = kPlanetHasPlanetBehavior;
        m.planets.max_space_base[i] = 2;
        m.planets.max_ground_base[i] = 3;
    }
    for (PlanetIndex i = 0; i + 1 < kUwPlanets; ++i) {
        const uint32_t r = m.routes.add("UR" + std::to_string(i));
        m.routes.point_a[r] = i;
        m.routes.point_b[r] = i + 1;
        m.routes.hs_speed_factor[r] = 4.0f;
        m.routes.credit_gain_factor[r] = 0.0f;
    }
    s.planet_type.assign(kUwPlanets, uPlanet);
    gal::PlayerSetup emp;  // AI-controlled, no galactic AI attached: passive
    emp.faction = 0;
    emp.ai = true;
    emp.difficulty = 0;
    emp.credits = 5000.0f;
    gal::PlayerSetup uw = emp;
    uw.faction = 1;
    uw.credits = 20000.0f;
    gal::PlayerSetup neu;
    neu.faction = 2;
    s.players = {emp, uw, neu};
    s.forces = {{1, 0, uTyber}, {1, 0, uSaboteur}, {1, 0, uIG88}, {1, 0, uStruct}};  // a ground base at home
    for (PlanetIndex p = 1; p < kUwPlanets; ++p) s.forces.push_back({0, p, uStruct});
    s.forces.push_back({0, 3, uDeathStar});
    return s;
}

UwResult run_underworld(unsigned threads, int32_t frames, bool shared_fleet) {
    UwResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\Library\\SynUnderworld.lua", kUwLibrary);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_BlackMarket.lua", kUwPlanBlackMarket);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_DeploySaboteur.lua", kUwPlanSaboteur);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_HackSuperWeapon.lua", kUwPlanHack);
    files.add("Data\\Scripts\\FreeStore\\SynFreeStore.lua", kSynFreeStore);
    defs.add_goals(kUwGoals, "goals.xml");
    defs.link();
    defs.add_function_set("UwSet", kUwSet, "UwSet.xml");
    defs.add_templates(kUwTemplates, "t.xml");
    defs.add_players(kUwPlayer, "p.xml");
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    eqs.add("Offensive_Budget", "5");
    eqs.add("Infra_Budget", "5");
    // AI_EQUATIONS_UNDERWORLD_GALACTIC.XML (Can_* / Should_* verbatim except the reduced terms noted).
    eqs.add("Can_Corrupt_Planet",
            "Variable_Target.RetainsResidualInfluence * (Variable_Target.IsCorrupted == 0.0) * "
            "(Variable_Target.IsCorruptionTransitionActive == 0.0) * "
            "((Variable_Target.IsConnectedToCorruption + (Game.Age > Variable_Target.TimeSinceCorruptionChange)) > 0)");
    eqs.add("Should_Corrupt_Planet",
            "Function_Can_Corrupt_Planet.Evaluate * (13.0 + 5.0 * Variable_Target.IsConnectedToCorruption + "
            "5.0 * Variable_Target.IncomeUnnormalized / 75)");
    eqs.add("Can_Make_Black_Market_Purchase",
            "Variable_Target.IsCorrupted * Variable_Target.BlackMarketAbilitiesAvailable {Parameter_Faction = \"Underworld\"}");
    eqs.add("Should_Make_Black_Market_Purchase",
            "Function_Can_Make_Black_Market_Purchase.Evaluate * "
            "(Variable_Self.CreditsUnnormalized > Variable_Target.BlackMarketMinimumAbilityPrice {Parameter_Faction = \"Underworld\"}) * "
            "(10.0 - Variable_Target.BlackMarketPriceModifier / 300.0)");
    eqs.add("Should_Hack_Super_Weapon",  // the Palpatine / planets-controlled weights reduced to a constant
            "Variable_Target.IsCorrupted * (Variable_Target.EnemyForce.HasSpaceUnitsBitfield {Parameter_Type = \"Death_Star_II\"} > 0.0) * 10.0");
    eqs.add("Can_Sabotage_Planet", "Variable_Target.IsCorrupted * (Variable_Target.GroundbaseLevelUnnormalized > 0)");
    eqs.add("Should_Sabotage_Planet",  // GenericPlanetValue / Should_Save_For_Corruption terms dropped
            "Function_Can_Sabotage_Planet.Evaluate * (50.0 + 10.0 * Variable_Target.HasStructure {Parameter_Type = \"E_Ground_Research_Facility\"})");
    eqs.link();
    const std::vector<std::string> plan_files = list_plan_files([&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
    gal::GalacticRules rules = uw_rules();
    if (shared_fleet)  // space heroes: the starting forces put them into one orbit fleet (no auto-land)
        for (const uint32_t h : {uint32_t{uTyber}, uint32_t{uSaboteur}, uint32_t{uIG88}}) rules.types[h].kind = gal::UnitKind::SpaceUnit;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(5150u, 3u);
    gal::GalacticSim sim(rules, store, rng.synced());
    if (threads > 0) sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(uw_setup(), &diag)) {
        std::printf("underworld: start failed\n");
        ++g_fail;
        return out;
    }
    sim.set_corruption(2, 3, 1);  // Racketeering by the Underworld: a sabotage target
    sim.set_corruption(3, 3, 1);  // the Death Star II planet: a hack target
    std::vector<GalacticAiTypeInfo> tinfo(uTypes);
    tinfo[uTyber].hero = tinfo[uIG88].hero = true;
    tinfo[uSaboteur].generic_hero = true;
    tinfo[uStruct].categories = {"STRUCTURE"};
    tinfo[uDeathStar].categories = {"CAPITAL"};
    for (const uint32_t h : {uint32_t{uTyber}, uint32_t{uSaboteur}, uint32_t{uIG88}}) tinfo[h].categories = {"INFANTRY"};
    GalacticAiConfig cfg;
    cfg.players = {{1, "SynUnderworld", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd));
    for (const std::string& d : pd) std::printf("  underworld plans: %s\n", d.c_str());
    CHECK(world.plans().plans.size() == 3);
    CHECK(world.start(&pd));
    std::vector<objects::ObjectHandle> start_heroes;
    if (shared_fleet) {  // the three (space) heroes start in one orbit fleet at home
        const std::vector<uint32_t> home = sim.fleets_at(0, 1);
        CHECK(home.size() == 1 && sim.fleets()[home.front()].contents.size() == 3);
        if (!home.empty()) start_heroes = sim.fleets()[home.front()].contents;
    }
    // Σ over the budget categories of the Underworld AI (player 1): pools P and available A.
    auto budget_sums = [&world, &defs](double& pool, double& avail) {
        pool = avail = 0.0;
        const AiContext* c = world.context(1);
        if (!c) return;
        for (size_t k = 0; k < defs.categories.size(); ++k) {
            pool += static_cast<double>(c->budget.pool(static_cast<int32_t>(k)));
            avail += static_cast<double>(c->budget.available(static_cast<int32_t>(k)));
        }
    };
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) {
            const bool watch = p == 1 && out.corr_frame < 0;
            const int32_t done_before = watch ? sim.corruptions_completed(1, 6) : 0;
            double pool = 0.0, avail = 0.0;
            const float cr = sim.credits(1);
            if (watch) budget_sums(pool, avail);
            world.update_player(p, f, true, false);
            if (watch && sim.corruptions_completed(1, 6) > done_before) {
                out.corr_frame = f;
                out.corr_credits_before = cr;
                out.corr_credits_after = sim.credits(1);
                out.corr_pool_before = pool;
                out.corr_avail_before = avail;
                budget_sums(out.corr_pool_after, out.corr_avail_after);
            }
        }
        bool shared = false;
        for (const gal::FleetState& fl : sim.fleets()) {
            if (!fl.alive || !fl.moving) continue;
            int heroes = 0;  // starting heroes (free-store units claimed by the plans) in this moving fleet
            for (const objects::ObjectHandle u : fl.contents)
                heroes += store.alive(u) && std::find(start_heroes.begin(), start_heroes.end(), u) != start_heroes.end() ? 1 : 0;
            shared = shared || heroes > 1;
        }
        out.shared_moves += shared ? 1 : 0;
        if (shared && out.first_shared < 0) out.first_shared = f;
        StateHasher h;
        sim.hash_state(h);
        world.hash(h);
        rng.synced().hash(h);
        out.crc.push_back(h.digest());
    }
    for (const AiDecision& d : world.decision_log()) {
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
        out.splits += d.text.rfind("order split ", 0) == 0 ? 1 : 0;
    }
    out.errors = world.lua_errors();
    out.corruption_p1 = sim.planets()[1].corruption;
    out.completed_bm = sim.corruptions_completed(1, 6);
    out.sabotage_p2 = sim.sabotage_count(2);
    out.purchased = sim.bm_purchased(1).size();
    out.p3_destroyed = sim.planets()[3].destroyed;
    out.ds_locked = sim.type_locked(0, uDeathStar);
    out.ds_alive = false;
    for (const gal::FleetState& fl : sim.fleets())
        if (fl.alive)
            for (const objects::ObjectHandle u : fl.contents) out.ds_alive = out.ds_alive || (store.alive(u) && store.type(u) == uDeathStar);
    out.uw_credits = sim.credits(1);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "frames %d P1 corruption %d (black-market corruptions %d) P2 sabotages %d purchased %zu P3 destroyed %d "
                  "DS locked %d DS alive %d credits U=%.0f orders %llu",
                  frames, static_cast<int>(out.corruption_p1), out.completed_bm, out.sabotage_p2, out.purchased,
                  out.p3_destroyed ? 1 : 0, out.ds_locked ? 1 : 0, out.ds_alive ? 1 : 0, static_cast<double>(out.uw_credits),
                  static_cast<unsigned long long>(world.orders_issued()));
    out.summary = buf;
    return out;
}

void test_underworld_special() {
    const int32_t frames = 30 * 600;  // 10 minutes of galactic time
    const UwResult base = run_underworld(0, frames);
    std::printf("underworld (0 workers): %s, %zu decisions, %zu Lua errors\n", base.summary.c_str(), base.decisions.size(),
                base.errors.size());
    for (size_t i = 0; i < base.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", base.errors[i].c_str());
    for (const std::string& d : base.decisions)
        if (d.find("ability") != std::string::npos) std::printf("  %s\n", d.c_str());
    CHECK(base.errors.empty());
    CHECK(base.corruption_p1 == 6 && base.completed_bm >= 1);  // Corrupt_Planet: corruption_ai_pick on a clean planet
    CHECK(base.sabotage_p2 >= 1);                              // Sabotage_Planet: a structure of the corrupted P2
    CHECK(base.purchased >= 1);                                // Black_Market_Purchase at the Black_Market planet
    CHECK(base.p3_destroyed && base.ds_locked && !base.ds_alive);  // Hack_Super_Weapon through Land_Units
    // a2k t08 (docs/re/A2K_T08_RE_QUESTIONS.md §1): the AI corruption cost is not reserved or charged through the AI
    // budget. Retail HotM Rebel (a2j probe 074145): 800 credits (Intimidation at Naboo) left the Underworld's credits
    // between frames 585 and 616 while its Offensive / Infrastructure pools stayed as they were. Here: the update that
    // applies the Black_Market corruption (500) takes exactly its cost from the credits, and no budget pool or
    // available amount moves in that update.
    std::printf("  corruption apply: frame %d credits %.1f -> %.1f, budget pools %.3f -> %.3f, available %.3f -> %.3f\n",
                base.corr_frame, static_cast<double>(base.corr_credits_before), static_cast<double>(base.corr_credits_after),
                base.corr_pool_before, base.corr_pool_after, base.corr_avail_before, base.corr_avail_after);
    CHECK(base.corr_frame > 0);
    CHECK(base.corr_credits_before - base.corr_credits_after == 500.0f);
    CHECK(base.corr_pool_before == base.corr_pool_after && base.corr_avail_before == base.corr_avail_after);
    {   // a2k t08 Q2 (A2K_T08_RE_ANSWERS Q2.3, PRE_PATH_SEPERATE): the three heroes (space units here) start in one orbit
        // fleet. Each plan's Produce_Force splits its claimed hero out of the shared fleet before the gather, so no fleet
        // ever jumps with two of those heroes (Move_To moves the task force's fleet, which then holds only its own hero),
        // and the corruption still lands. Produced units are not split (PRODUCTION_END paths the unit's fleet as it is).
        // (The hack needs Land_Units of a ground hero: not checked here.)
        const UwResult sh = run_underworld(0, frames, true);
        std::printf("  shared home fleet: %s; %zu splits, %d frames with a shared moving fleet, %zu Lua errors\n", sh.summary.c_str(),
                    sh.splits, sh.shared_moves, sh.errors.size());
        if (sh.shared_moves > 0)  // the decisions up to the first such frame
            for (const std::string& d : sh.decisions)
                if (std::atoi(d.c_str()) <= sh.first_shared) std::printf("    %s\n", d.c_str());
        CHECK(sh.errors.empty() && sh.splits >= 2 && sh.shared_moves == 0);
        CHECK(sh.corruption_p1 == 6 && sh.completed_bm >= 1);
    }
    for (const unsigned t : {1u, 4u, 15u}) {
        const UwResult r = run_underworld(t, frames);
        size_t first = r.crc.size() == base.crc.size() ? r.crc.size() : 0;
        for (size_t i = 0; i < std::min(r.crc.size(), base.crc.size()); ++i)
            if (r.crc[i] != base.crc[i]) {
                first = i;
                break;
            }
        std::printf("  underworld workers %u vs 0: %s\n", t, first == r.crc.size() ? "identical" : ("DIVERGED at frame " + std::to_string(first + 1)).c_str());
        CHECK(first == r.crc.size() && r.decisions == base.decisions);
    }
    std::ostringstream o;
    o << "# P4 ai synthetic Underworld special systems: per-frame CRC (every 300th frame + last), decisions\n";
    for (size_t i = 0; i < base.crc.size(); ++i)
        if ((i + 1) % 300 == 0 || i + 1 == base.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(base.crc[i]));
            o << b;
        }
    o << base.summary << "\n";
    for (const std::string& d : base.decisions) o << d << "\n";
    const std::string text = o.str();
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_underworld_crc.txt";
    if (g_write_golden) {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
        std::printf("  wrote %s\n", path.c_str());
    } else {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
        const bool same = want == text;
        std::printf("  golden %s: %s\n", path.c_str(), same ? "MATCH" : (want.empty() ? "MISSING" : "DIFFERS"));
        if (!same) {
            const std::string actual = std::string(ALAMO_AI_LOG_DIR) + "/ai_underworld_actual.txt";
            std::ofstream(std::filesystem::path(actual), std::ios::binary) << text;
            std::printf("  actual written to %s\n", actual.c_str());
        }
        CHECK(same);
    }
}

// LuaVm::write / read implemented (false while engine/lua still has the N1 stubs).
bool lua_state_persists() {
    lua::LuaVm a{lua::LuaHost{}};
    lua::LuaVm b{lua::LuaHost{}};
    ByteWriter w;
    a.write(w);
    ByteReader r(w.bytes());
    return b.read(r) && r.done();
}

// Ticks `a` and every rig of `others` together; false at the first frame whose CRC or AI orders differ from a's.
bool save_lockstep(SaveRig& a, std::initializer_list<SaveRig*> others, int32_t first, int32_t frames, const char* label) {
    for (int32_t f = first; f < first + frames; ++f) {
        a.tick(f);
        const uint64_t ca = a.crc();
        for (SaveRig* o : others) {
            o->tick(f);
            const uint64_t co = o->crc();
            if (ca != co || a.orders != o->orders) {
                std::printf("  %s: rigs differ at frame %d (crc %016llx / %016llx, %zu / %zu orders)\n", label, f,
                            static_cast<unsigned long long>(ca), static_cast<unsigned long long>(co), a.orders.size(),
                            o->orders.size());
                return false;
            }
        }
    }
    return true;
}

// Broken streams: read() fails and leaves the running world (and the shared RNG) exactly as it was.
void save_rejects(SaveRig& a, const std::vector<uint8_t>& bytes, int cuts, const char* label) {
    const uint64_t digest = a.ai_digest();
    const GameRandom rng = a.rng;
    const std::vector<uint8_t> before = a.save();
    int accepted = 0;
    auto reject = [&](const std::vector<uint8_t>& d, const char* what) {
        if (a.load(d)) {
            ++accepted;
            std::printf("  %s: %s (%zu bytes) accepted\n", label, what, d.size());
        }
        CHECK(a.ai_digest() == digest && a.rng == rng);
    };
    reject({}, "empty stream");
    std::vector<uint8_t> v = bytes;
    v[0] = static_cast<uint8_t>(v[0] ^ 0xFFu);
    reject(v, "bad version");
    v = bytes;
    v[4] = static_cast<uint8_t>(v[4] ^ 1u);
    reject(v, "AI player count");
    v = bytes;
    v.push_back(0);
    reject(v, "trailing byte");
    reject(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1), "truncated by one byte");
    for (int k = 1; k < cuts; ++k) {
        const auto cut = static_cast<std::ptrdiff_t>(bytes.size() * static_cast<size_t>(k) / static_cast<size_t>(cuts));
        reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut), "truncated stream");
    }
    CHECK(accepted == 0);
    CHECK(a.save() == before);
}

// `full_session`: also load the whole session (store, sim, conflicts, AI) into a third, fresh rig C. Needs object types
// from a GameObjectTypes database (ObjectStore::read rejects type indices it does not know).
// `max_bytes` / `max_packed` (0: unchecked): size guards (N1 save size) on the AI save, as written and as a save file
// packs it (lz_compress).
void check_save_load(const SaveRigInputs& in, const char* label, int32_t warm, int32_t after, int cuts, bool full_session,
                            size_t max_bytes, size_t max_packed) {
    SaveRig a(in), b(in), c(in);
    const bool started = a.start() && b.start() && (!full_session || c.start());
    CHECK(started);
    if (!started) return;
    // Save at the first frame from `warm` on with running plans (task forces, statuses, build tasks live).
    int32_t frame = warm;
    bool twins = save_lockstep(a, {&b}, 1, warm, label);
    while (twins && a.world->running_plans() == 0 && frame < warm + 30 * 600) twins = save_lockstep(a, {&b}, ++frame, 1, label);
    CHECK(twins);  // the rigs are deterministic twins
    if (!twins) return;
    const bool lua = lua_state_persists();
    const std::vector<uint8_t> bytes = a.save();
    if (bytes != b.save()) {
        // Twin worlds must save the same bytes; tell the AI's own sections from the Lua VM images.
        a.world->set_save_lua_state(false);
        b.world->set_save_lua_state(false);
        const bool core = a.save() == b.save();
        a.world->set_save_lua_state(true);
        b.world->set_save_lua_state(true);
        CHECK(core && lua);
        std::printf("  %s: twin saves differ %s\n", label,
                    core ? "only inside the Lua VM images (LuaVm::write of twin VMs not byte-identical)" : "in the AI sections");
    }
    const uint64_t digest = a.ai_digest();
    const size_t packed = lz_compress(bytes).size();
    std::printf("N1 save %s: frame %d, %zu bytes (%zu packed), %zu running plans, %llu orders so far\n", label, frame,
                bytes.size(), packed, a.world->running_plans(), static_cast<unsigned long long>(a.world->orders_issued()));
    CHECK(a.world->running_plans() > 0);
    if (max_bytes && bytes.size() > max_bytes) std::printf("  %s: AI save larger than its %zu-byte guard\n", label, max_bytes);
    if (max_packed && packed > max_packed) std::printf("  %s: packed AI save larger than its %zu-byte guard\n", label, max_packed);
    CHECK(!max_bytes || bytes.size() <= max_bytes);
    CHECK(!max_packed || packed <= max_packed);
    // The rest of the session at the save point (n1-objects / n1-galactic), for the full load into rig C.
    const std::vector<uint8_t> s_rng = save_chunk(a.rng), s_store = save_chunk(*a.store), s_sim = save_chunk(*a.sim);
    const std::vector<uint8_t> s_conflicts = a.conflicts ? save_chunk(*a.conflicts) : std::vector<uint8_t>{};

    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(false);  // the same with the deeper sections reached (fresh VMs staged, then dropped)
    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(true);
    CHECK(a.ai_digest() == digest && a.crc() == b.crc());

    // B: the loader's rebuild of the AI world on B's (twin) sim. B's RNG as it was: building a world keeps no draw.
    const GameRandom rng_before = b.rng;
    const bool rebuilt = b.make_world();
    CHECK(rebuilt);
    if (!rebuilt) return;
    b.rng = rng_before;
    // C: a whole session loaded into a fresh rig (started from the setup, never ticked): RNG, objects, galactic sim,
    // conflicts, then the AI (objects resolve by id in the restored store).
    const char* failed = !full_session                                           ? nullptr
                         : !load_chunk(c.rng, s_rng)                               ? "RNG"
                         : !load_chunk(*c.store, s_store)                         ? "ObjectStore"
                         : !load_chunk(*c.sim, s_sim)                             ? "GalacticSim"
                         : c.conflicts && !load_chunk(*c.conflicts, s_conflicts) ? "ConflictManager"
                                                                                  : nullptr;
    const bool session = failed == nullptr;
    if (failed) std::printf("  %s: full session: %s read failed\n", label, failed);
    CHECK(session);
    if (!lua) {
        b.world->set_save_lua_state(false);
        if (full_session) c.world->set_save_lua_state(false);
    }
    const bool loaded_b = b.load(bytes), loaded_c = !full_session || (session && c.load(bytes));
    CHECK(loaded_b && loaded_c);
    if (!loaded_b || !loaded_c) {
        std::printf("  %s: read() failed (rebuilt world %s, full session %s)\n", label, loaded_b ? "ok" : "FAILED",
                    !session ? "not loaded" : loaded_c ? "ok" : "FAILED");
        return;
    }
    // Without Lua state (stubs) A's images are empty too, so the byte comparisons hold in both cases.
    const bool hashes = b.ai_digest() == digest && b.crc() == a.crc() &&
                        (!full_session || (c.ai_digest() == digest && c.crc() == a.crc()));
    const bool resave = b.save() == bytes && (!full_session || c.save() == bytes);  // save -> load -> save
    CHECK(hashes && resave);
    if (!lua) {
        std::printf("N1 save %s: PENDING n1-lua (LuaVm::write / read are stubs): rebuilt world%s loaded without the Lua VM "
                    "images, AI hash + paths + CRC %s, re-save %s; lockstep continuation not run\n", label,
                    full_session ? " + full session" : "", hashes ? "identical" : "DIFFER", resave ? "byte-identical" : "DIFFERS");
        return;
    }
    const size_t mark = a.world->decision_log().size();
    const bool same = full_session ? save_lockstep(a, {&b, &c}, frame + 1, after, label)
                                   : save_lockstep(a, {&b}, frame + 1, after, label);
    CHECK(same);
    const auto& la = a.world->decision_log();
    bool logs = true;
    for (const SaveRig* r : {&b, &c}) {
        if (r == &c && !full_session) continue;
        const auto& lr = r->world->decision_log();
        logs = logs && la.size() - mark == lr.size();
        for (size_t i = 0; logs && i < lr.size(); ++i)
            logs = la[mark + i].frame == lr[i].frame && la[mark + i].player == lr[i].player && la[mark + i].text == lr[i].text;
    }
    CHECK(logs);
    std::printf("N1 save %s: loaded into a rebuilt world%s, %d lockstep frames %s (per-tick CRC + orders), %zu decisions "
                "after the save %s, final crc %016llx\n", label, full_session ? " and a fully loaded session" : "", after,
                same ? "identical" : "DIFFER",
                la.size() - mark, logs ? "identical" : "DIFFER", static_cast<unsigned long long>(a.crc()));
}

void test_save_synthetic() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    SaveRigInputs in;
    syn_data(files, defs, eqs, in.plan_files);
    const gal::GalacticRules rules = syn_rules();
    const gal::GalacticSetup setup = syn_setup();
    objects::GameObjectTypes types;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = &files;
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo.resize(tTypes);
    in.tinfo[tFrigate].categories = {"FRIGATE"};
    in.tinfo[tCorvette].categories = {"CORVETTE"};
    in.tinfo[tInfantry].categories = {"INFANTRY"};
    in.tinfo[tSB1].categories = in.tinfo[tSB2].categories = {"STRUCTURE"};
    in.cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    in.cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    in.seed_synced = 4242u;
    in.seed_free = 77u;
    in.conflicts = true;
    check_save_load(in, "synthetic", 30 * 90, 30 * 240, 48, false);
}

// q6 t07: Variable_Target.Type.IsType on a planet reads the planet's own type name (Thrawn's Revenge picks planets by
// name with it). ParityWorld: P2 is SynPlanetRestricted, the others SynPlanet.
void test_planet_type_is_type() {
    ParityWorld pw(false, {"BuildBarracks"});
    pw.run(2);
    auto token = [&pw](const char* text, int32_t target, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        c.target = target;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    bool ok = false;
    CHECK(token("Variable_Target.Type.IsType {Parameter_Type = \"SynPlanet\"}", 0, ok) == 1.0 && ok);
    CHECK(token("Variable_Target.Type.IsType {Parameter_Type = \"synplanet\"}", 1, ok) == 1.0 && ok);  // case-insensitive
    CHECK(token("Variable_Target.Type.IsType {Parameter_Type = \"SynPlanetRestricted\"}", 0, ok) == 0.0 && ok);
    CHECK(token("Variable_Target.Type.IsType {Parameter_Type = \"SynPlanetRestricted\"}", 2, ok) == 1.0 && ok);
    CHECK(token("Variable_Target.Type.IsType {Parameter_Type = \"SynPlanet\"}", 2, ok) == 0.0 && ok);  // own name only
    CHECK(token("Variable_Target.Type.IsType", 0, ok) == 0.0 && ok);                                   // no names listed
}

// q6 t03 (AI_GALACTIC_TOKENS §8.2): the player tokens TimeSince{Space,Ground}{Defender,Attacker}. Per player stamps
// (-1 = never: the float 1e18 widened), (logic frame - stamp) / 30 s; roots Self / Enemy / Human / Target.Owner, never a
// planet root. Stamped from the ConflictManager: a space record that starts or is dropped (0x11), a land record whose
// auto-resolve begins (0x13); a player that is the defender gets the defender stamp, else the attacker the attacker one.
void test_player_conflict_tokens() {
    const double never = static_cast<double>(1.0e18f);
    struct Rig {
        ParityWorld pw;
        gal::ConflictRules crules;
        gal::ConflictManager cm;
        int32_t frame = 0;
        explicit Rig(const std::function<void(gal::GalacticSetup&)>& tweak = {})
            : pw(false, {"HeroAttach"}, tweak), cm(*pw.sim, crules, pw.rng->synced()) {
            cm.attach();
            pw.world->set_conflicts(&cm);
        }
        void step(int32_t n) {
            for (int32_t i = 0; i < n; ++i) {
                ++frame;
                pw.sim->update();
                pw.world->update_player(0, frame, true, cm.state() == gal::ConflictState::AutoRunning);
            }
        }
        double token(const char* text, int32_t target, bool& ok) const {
            EquationSet e;
            e.add("T", text);
            e.link();
            EvalContext c;
            c.self = 0;
            c.enemy = 1;
            c.human = 1;
            c.target = target;
            const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
            ok = r.ok;
            return r.value;
        }
        int32_t stamp(int which, int32_t player) const { return pw.world->resolver().player_stamp(which, player); }
    };
    bool ok = false;
    {
        // A space record the service drops (the attacker has no ship at P0): 0x11 before the head goes.
        Rig g;
        g.step(30);
        for (const char* t : {"Variable_Self.TimeSinceSpaceDefender", "Variable_Self.TimeSinceSpaceAttacker",
                              "Variable_Self.TimeSinceGroundDefender", "Variable_Self.TimeSinceGroundAttacker",
                              "Variable_Enemy.TimeSinceSpaceAttacker"})
            CHECK(g.token(t, 0, ok) == never && ok);
        CHECK(g.cm.initiate_space_conflict(0, 1, 0));
        g.step(1);
        CHECK(!g.cm.log().empty() && g.cm.log().back().how == 0 && g.cm.log().back().kind == gal::ConflictKind::Space);
        CHECK(g.stamp(0, 0) == 31 && g.stamp(1, 0) == -1);  // P0's owner: the defender stamp only
        CHECK(g.stamp(1, 1) == 31 && g.stamp(0, 1) == -1);  // the attacker
        CHECK(g.stamp(2, 0) == -1 && g.stamp(3, 1) == -1);  // no ground stamp
        g.step(59);                                         // logic frame 90
        const double t = static_cast<double>(90 - 31) / 30.0;
        CHECK(g.token("Variable_Self.TimeSinceSpaceDefender", 0, ok) == t && ok);
        CHECK(g.token("Variable_Self.TimeSinceSpaceAttacker", 0, ok) == never && ok);
        CHECK(g.token("Variable_Enemy.TimeSinceSpaceAttacker", 0, ok) == t && ok);
        CHECK(g.token("Variable_Human.TimeSinceSpaceAttacker", 0, ok) == t && ok);
        CHECK(g.token("Variable_Enemy.TimeSinceSpaceDefender", 0, ok) == never && ok);
        CHECK(g.token("Variable_Target.Owner.TimeSinceSpaceDefender", 0, ok) == t && ok);  // P0's owner = Self
        g.token("Variable_Target.TimeSinceSpaceDefender", 0, ok);
        CHECK(!ok);  // not a planet token
        // The N1 image carries the stamps and the logic frame.
        ByteWriter w;
        g.pw.world->resolver().write(w);
        GalacticSimTokenResolver copy(*g.pw.sim, {}, nullptr, {});
        ByteReader rd(w.bytes());
        CHECK(copy.read(rd) && rd.ok());
        for (int which = 0; which < 4; ++which)
            for (int32_t p = 0; p < 3; ++p) CHECK(copy.player_stamp(which, p) == g.stamp(which, p));
        ByteWriter w2;
        copy.write(w2);
        CHECK(w2.bytes().size() == w.bytes().size() &&
              std::equal(w2.bytes().begin(), w2.bytes().end(), w.bytes().begin()));
        std::printf("player conflict tokens: space drop stamps defender %d / attacker %d, %.4f s later\n", g.stamp(0, 0),
                    g.stamp(1, 1), t);
    }
    {
        // A land invasion of P1 (the Empire's infantry on the surface) by the Rebel infantry carried in orbit: AI vs AI,
        // auto-resolved -> 0x13 for both players.
        Rig g([](gal::GalacticSetup& s) {
            s.forces.push_back({1, 1, tFrigate});
            s.forces.push_back({1, 1, tInfantry});
        });
        g.step(30);
        const int32_t sd0 = g.stamp(0, 0), sa1 = g.stamp(1, 1);
        CHECK(g.stamp(2, 0) == -1 && g.stamp(3, 1) == -1);
        std::vector<objects::ObjectHandle> invaders;
        for (const uint32_t f : g.pw.sim->fleets_at(1, 1))
            for (const objects::ObjectHandle h : g.pw.sim->fleets()[f].contents)
                if (g.pw.rules.types[g.pw.store->type(h)].kind == gal::UnitKind::GroundUnit) invaders.push_back(h);
        if (invaders.empty()) {
            std::vector<objects::ObjectHandle> mine;
            for (const objects::ObjectHandle h : g.pw.sim->planets()[1].surface)
                if (g.pw.store->owner(h) == 1) mine.push_back(h);
            CHECK(g.pw.sim->launch_units(1, 1, mine) == commands::RejectReason::None);
            invaders = mine;
        }
        CHECK(!invaders.empty() && g.pw.sim->invade(1, 1, invaders) == commands::RejectReason::None);
        g.step(1);
        bool land_auto = false;
        for (const gal::ConflictLogEntry& e : g.cm.log()) land_auto = land_auto || (e.kind == gal::ConflictKind::Land && e.how == 1);
        CHECK(land_auto || g.cm.state() == gal::ConflictState::AutoRunning);
        CHECK(g.stamp(2, 0) == 31 && g.stamp(3, 0) == -1);  // the defender
        CHECK(g.stamp(3, 1) == 31 && g.stamp(2, 1) == -1);  // the attacker
        g.step(29);  // logic frame 60
        CHECK(g.stamp(2, 0) == 31);  // the end of the auto-resolve does not stamp again
        CHECK(g.token("Variable_Self.TimeSinceGroundDefender", 0, ok) == static_cast<double>(60 - 31) / 30.0 && ok);
        CHECK(g.token("Variable_Enemy.TimeSinceGroundAttacker", 0, ok) == static_cast<double>(60 - 31) / 30.0 && ok);
        CHECK(g.token("Variable_Self.TimeSinceGroundAttacker", 0, ok) == never && ok);
        std::printf("player conflict tokens: land auto-resolve stamps defender %d / attacker %d (space stamps %d / %d)\n",
                    g.stamp(2, 0), g.stamp(3, 1), sd0, sa1);
    }
}

// q6 t03 (AI_GALACTIC_TOKENS §8.3): Variable_Self.BudgetAllocation{Parameter_Category}: the normalised weight of the
// category key (AIGoalCategoryType value, low 32 bits) in Self's budget; 0.0 for a key the budget does not hold or no
// parameter (-1); fails on any root but Self.
void test_budget_allocation_token() {
    ParityWorld pw(false, {"HeroAttach"});
    pw.run(2);  // first service: templates, budget
    auto token = [&pw](const char* text, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        c.enemy = 1;
        c.human = 1;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    bool ok = false;
    // Offensive_Budget = 5 + 10 x planets, Infra_Budget = 3, no other category has an equation.
    const double off = token("Variable_Self.BudgetAllocation {Parameter_Category = AIGoalCategoryType[Offensive]}", ok);
    CHECK(ok);
    const double inf = token("Variable_Self.BudgetAllocation {Parameter_Category = AIGoalCategoryType[Infrastructure]}", ok);
    CHECK(ok);
    std::printf("budget allocation: offensive %.6f infrastructure %.6f\n", off, inf);
    CHECK(off > inf && inf > 0.0 && std::fabs(off + inf - 1.0) < 1e-6);
    CHECK(token("Variable_Self.BudgetAllocation {Parameter_Category = AIGoalCategoryType[Defensive]}", ok) == 0.0 && ok);
    CHECK(token("Variable_Self.BudgetAllocation {Parameter_Category = 3}", ok) == inf && ok);    // the XML value of Infrastructure
    CHECK(token("Variable_Self.BudgetAllocation {Parameter_Category = 99}", ok) == 0.0 && ok);   // no such category
    CHECK(token("Variable_Self.BudgetAllocation", ok) == 0.0 && ok);                             // key -1
    // An unknown AIGoalCategoryType literal does not convert: the equation never builds and evaluating it fails (a2i t07,
    // A2I_TOKENS_RE §3.2-3.3 [H]).
    token("Variable_Self.BudgetAllocation {Parameter_Category = AIGoalCategoryType[NoSuchGoal]}", ok);
    CHECK(!ok);
    token("Variable_Enemy.BudgetAllocation {Parameter_Category = AIGoalCategoryType[Offensive]}", ok);
    CHECK(!ok);
    token("Variable_Human.BudgetAllocation {Parameter_Category = AIGoalCategoryType[Offensive]}", ok);
    CHECK(!ok);
}

// q6 t03: the AI's GameObjectCategoryType name -> bit lookup reads the loaded enum (Thrawn's Revenge inserts Gunship at 0x8
// and moves Corvette..SpaceHero up); null = the vanilla table, a discarded converter converts nothing (A2k t06).
void test_loaded_category_enum() {
    objects::EnumConverter tr;
    tr.enum_name = "GameObjectCategoryType";
    tr.bitfield = tr.valid = true;
    const std::pair<const char*, int64_t> rows[] = {{"Fighter", 0x1},       {"Bomber", 0x2},          {"Transport", 0x4},
                                                    {"Gunship", 0x8},       {"Corvette", 0x10},       {"Frigate", 0x20},
                                                    {"Capital", 0x40},      {"SuperCapital", 0x80},   {"Infantry", 0x100},
                                                    {"SpaceHero", 0x20000000}};
    for (const auto& [n, v] : rows) CHECK(tr.add(n, v));
    CHECK(ai_category_bit("Corvette", &tr) == 0x10 && ai_category_bit("Corvette") == 0x8);
    CHECK(ai_category_bit("gunship", &tr) == 0x8 && ai_category_bit("Gunship") == 0);
    CHECK(ai_category_bit("Capital", &tr) == 0x40 && ai_category_bit("Super", &tr) == 0 && ai_category_bit("Super") == 0x40);
    objects::EnumConverter bad = tr;
    bad.valid = false;
    CHECK(ai_category_bit("Corvette", &bad) == 0);  // discarded: every conversion fails (OBJECT_TYPES §9.1)
    std::vector<GalacticAiTypeInfo> ti(2);
    ti[0].categories = {"CORVETTE"};
    ti[1].categories = {"GUNSHIP", "INFANTRY"};
    fill_category_masks(ti, &tr);
    CHECK(ti[0].category_mask == 0x10 && ti[1].category_mask == 0x108);
    fill_category_masks(ti);
    CHECK(ti[0].category_mask == 0x8 && ti[1].category_mask == 0x80);
}

// q6 t07: GalacticAiWorld::set_ai_type (the SWITCH_CONTROL story reward, AI_GALACTIC §1.2). A player with no AI gets
// one mid-game and plays; an unknown type is refused and logged; "Human" removes the AI; the N1 save carries the AI
// player types, so a loader's world rebuilt from the starting configuration takes the switched players back.
void test_ai_type_switch() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    SaveRigInputs in;
    syn_data(files, defs, eqs, in.plan_files);
    const gal::GalacticRules rules = syn_rules();
    const gal::GalacticSetup setup = syn_setup();
    objects::GameObjectTypes types;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = &files;
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo.resize(tTypes);
    in.tinfo[tFrigate].categories = {"FRIGATE"};
    in.tinfo[tCorvette].categories = {"CORVETTE"};
    in.tinfo[tInfantry].categories = {"INFANTRY"};
    in.tinfo[tSB1].categories = in.tinfo[tSB2].categories = {"STRUCTURE"};
    in.cfg.players = {{0, "SynEmpire", 1}};  // the Rebels start without an AI
    in.cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    in.seed_synced = 4242u;
    in.seed_free = 77u;
    in.conflicts = true;
    SaveRig a(in), b(in);
    const bool started = a.start() && b.start();
    CHECK(started);
    if (!started) return;
    CHECK(save_lockstep(a, {&b}, 1, 300, "ai type switch"));
    CHECK(!a.world->is_ai_player(1) && a.world->ai_type(1).empty() && a.world->ai_type(0) == "SynEmpire");
    std::vector<std::string> diag;
    CHECK(!a.world->set_ai_type(1, "Bogus", 1, &diag) && !a.world->is_ai_player(1));
    CHECK(diag.size() == 1 && diag[0] == "Unrecognized AI player type Bogus");
    CHECK(!a.world->set_ai_type(1, "Human", 1) && !a.world->set_ai_type(99, "SynRebel", 1));  // no AI to remove; bad slot
    CHECK(!a.world->set_ai_type(0, "SynEmpire", 1));                                          // same type: no change
    for (SaveRig* r : {&a, &b}) CHECK(r->world->set_ai_type(1, " SynRebel ", 1));
    CHECK(a.world->is_ai_player(1) && a.world->ai_type(1) == "SynRebel" && a.crc() == b.crc());
    CHECK(!a.world->set_ai_type(1, "synrebel", 1));
    CHECK(save_lockstep(a, {&b}, 301, 30 * 240, "ai type switch"));
    size_t p1 = 0;
    bool switch_logged = false;
    for (const AiDecision& d : a.world->decision_log())
        if (d.player == 1) {
            ++p1;
            switch_logged = switch_logged || (d.frame == 300 && d.text == "ai type SynRebel");
        }
    std::printf("ai type switch: the Rebels' AI from frame 300: %zu decisions, %zu running plans, %llu orders\n", p1,
                a.world->running_plans(), static_cast<unsigned long long>(a.world->orders_issued()));
    CHECK(switch_logged && p1 > 5);
    // N1: B's world rebuilt from the starting configuration (Rebels without an AI) takes the switched type back.
    const bool lua = lua_state_persists();
    if (!lua)
        for (SaveRig* r : {&a, &b}) r->world->set_save_lua_state(false);
    const std::vector<uint8_t> bytes = a.save();
    const GameRandom rng_before = b.rng;
    CHECK(b.make_world());
    b.rng = rng_before;
    if (!lua) b.world->set_save_lua_state(false);
    CHECK(!b.world->is_ai_player(1));
    CHECK(b.load(bytes) && b.world->ai_type(1) == "SynRebel" && b.ai_digest() == a.ai_digest() && b.save() == bytes);
    const int32_t at = 301 + 30 * 240;
    if (lua) CHECK(save_lockstep(a, {&b}, at, 30 * 60, "ai type switch (loaded)"));
    // "Human" removes the AI; a save without it loaded into a world that has it drops it.
    CHECK(a.world->set_ai_type(1, "Human", 1) && !a.world->is_ai_player(1) && a.world->running_plans() <= b.world->running_plans());
    CHECK(a.world->decision_log().back().text == "ai type Human");
    const std::vector<uint8_t> human = a.save();
    CHECK(b.world->is_ai_player(1) && b.load(human) && !b.world->is_ai_player(1) && b.ai_digest() == a.ai_digest());
    if (lua) CHECK(save_lockstep(a, {&b}, at + 30 * 60, 30 * 30, "ai type switch (human)"));
}

#ifdef ALAMO_AI_HAVE_VFS

// a2m r3 (A2M_R3_RE_A §1.2): GalacticAiWorld::starbase_candidate (0x3f8fd0) against the rule on every planet: with a
// starbase item queued -> none; no starbase -> Rebel_Star_Base_1 / Empire_Star_Base_1 for a Rebel / Empire owner, none
// otherwise (level gate: allowed 1 when Max_Space_Base != 0); a starbase -> its Next_Level_Base when Base_Level <=
// level + 1 <= Max_Space_Base (level < 5).
size_t starbase_candidates_check(const gal::GalacticSim& sim, const gal::GalacticRules& rules, const GalacticAiWorld& world,
                                        const char* when) {
    const uint32_t rsb1 = rules.find_type("Rebel_Star_Base_1"), esb1 = rules.find_type("Empire_Star_Base_1");
    const auto& msb = sim.map().planets.max_space_base;
    size_t checked = 0, none = 0, empty = 0;
    for (PlanetIndex t = 0; t < sim.planets().size(); ++t) {
        const gal::PlanetState& ps = sim.planets()[t];
        if (!ps.is_planet || ps.type >= rules.types.size()) continue;
        const int32_t mx = t < msb.size() && msb[t] >= 0 ? msb[t] : 5;
        bool queued = false;
        for (const auto& q : ps.queue)
            for (const gal::BuildEntry& e : q) queued = queued || (e.type < rules.types.size() && gal::is_star_base_type(rules.types[e.type]));
        uint32_t want = gal::kNoIndex;
        const uint32_t sb = sim.starbase_type(t);
        if (sb == gal::kNoIndex) {
            ++empty;
            const bool owned = ps.owner >= 0 && static_cast<size_t>(ps.owner) < sim.players().size();
            const std::string fac = owned ? rules.factions[sim.players()[static_cast<size_t>(ps.owner)].faction].name : "";
            want = ai_iequal(fac, "Rebel") ? rsb1 : ai_iequal(fac, "Empire") ? esb1 : gal::kNoIndex;
            if (queued || mx == 0) want = gal::kNoIndex;
        } else {
            const uint32_t nl = rules.types[sb].next_level_base;
            const int32_t allowed = queued ? 0 : (ps.starbase_level < 5 && mx >= ps.starbase_level + 1 ? ps.starbase_level + 1 : 0);
            if (nl < rules.types.size() && rules.types[nl].base_level <= allowed) want = nl;
        }
        const uint32_t c = world.starbase_candidate(t);
        CHECK(c == want);
        if (c != want)
            std::printf("  starbase candidate %s at %s: %u, want %u (starbase %u level %d, max %d, queued %d)\n", when,
                        rules.types[ps.type].name.c_str(), c, want, sb, ps.starbase_level, mx, queued ? 1 : 0);
        none += c == gal::kNoIndex ? 1u : 0u;
        ++checked;
    }
    std::printf("FoC starbase candidates (0x3f8fd0) %s: %zu planets, %zu without a starbase, %zu with none\n", when, checked, empty, none);
    return checked;
}
#endif

#ifdef ALAMO_AI_HAVE_VFS

void test_foc_data() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("FoC data: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("FoC data: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    const AiDataPaths paths = list_ai_data([&v](std::string_view g) { return v->list(g); });
    CHECK(load_ai_definitions(*v, paths, defs));
    std::printf("FoC AI data: %zu categories, %zu goal types, %zu function sets, %zu player types, %zu templates, %zu diagnostics\n",
                defs.categories.size(), defs.goals.size(), defs.sets.size(), defs.players.size(), defs.templates.size(),
                defs.diagnostics.size());
    for (size_t i = 0; i < defs.diagnostics.size() && i < 12; ++i) std::printf("  %s\n", defs.diagnostics[i].c_str());
    CHECK(defs.goals.size() > 100 && defs.find_player("BasicEmpire") && defs.find_template("Basic_Empire_Default"));
    const uint32_t cq = defs.find_goal("Conquer_Opponent");
    CHECK(cq != kAiNone && defs.goals[cq].time_limit == 240.0f && defs.goals[cq].category == defs.find_category("Offensive"));

    // Every shipped plan script runs Base_Definitions on the Lua bridge and yields a definition.
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("FoC data: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(99u, 1u);
    gal::GalacticSim sim(rules, store, rng.synced());
    CHECK(sim.start(setup, &diag));
    GalacticAiConfig gcfg;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (sim.players()[p].ai) {
            const std::string fac = rules.factions[sim.players()[p].faction].name;
            gcfg.players.push_back({static_cast<int32_t>(p),
                                    ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    gcfg.difficulties = load_ai_difficulties(*v);
    // A2 t03: the starbases' Starting_Spawned_Units_Tech_0 lists (type, count; repeated tags appended) reach the AI type
    // info, so the typed force rows hold squadrons instead of the starbase's own AI_Combat_Power.
    const std::vector<GalacticAiTypeInfo> foc_tinfo = make_galactic_ai_type_info(types);
    {
        // A2k t06: the masks come from the shipped Data\XML\Enum\GameObjectCategoryType.xml and equal the built-in table's
        // bit for bit (so no golden moves); the converted CategoryMask field (whole-value conversion) is reported.
        CHECK(types.enums().get(objects::EnumId::GameObjectCategory).from_file);
        std::vector<GalacticAiTypeInfo> builtin = foc_tinfo;
        fill_category_masks(builtin);
        size_t differ = 0, field_differ = 0;
        for (size_t t = 0; t < foc_tinfo.size(); ++t) {
            differ += foc_tinfo[t].category_mask != builtin[t].category_mask ? 1u : 0u;
            field_differ += foc_tinfo[t].category_mask != types.get_mask(static_cast<objects::TypeIndex>(t), "CategoryMask") ? 1u : 0u;
        }
        CHECK(differ == 0 && !foc_tinfo.empty());
        std::printf("FoC category masks: %zu types, %zu differ from the built-in table, %zu from the CategoryMask field\n",
                    foc_tinfo.size(), differ, field_differ);
    }
    {
        const uint32_t sb1 = rules.find_type("Rebel_Star_Base_1"), xw = rules.find_type("Rebel_X-Wing_Squadron"), yw = rules.find_type("Y-Wing_Squadron");
        CHECK(sb1 != gal::kNoIndex && sb1 < foc_tinfo.size() && xw != gal::kNoIndex && yw != gal::kNoIndex);
        if (sb1 < foc_tinfo.size()) {
            const auto& l = foc_tinfo[sb1].spawn_tech[0];
            CHECK(l.size() == 2 && l[0].type == xw && l[0].count == 2 && l[1].type == yw && l[1].count == 2);
            CHECK(foc_tinfo[sb1].spawn_tech[1].empty() && rules.types[sb1].ai_combat_power == 5000.0f);
        }
    }
    GalacticAiWorld world(sim, store, &types, v.get(), rng, defs, eqs, foc_tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, gcfg);
    std::vector<std::string> pd;
    const std::vector<std::string> plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    CHECK(world.load_plans(plan_files, &pd));
    std::printf("FoC plans: %zu files, %zu definitions loaded, %zu diagnostics\n", plan_files.size(), world.plans().plans.size(), pd.size());
    for (size_t i = 0; i < pd.size() && i < 15; ++i) std::printf("  %s\n", pd[i].c_str());
    CHECK(world.plans().find("ConquerOpponentPlan") != kAiNone && world.plans().find("BuildSpaceForcesPlan") != kAiNone);
    const uint32_t cop = world.plans().find("ConquerOpponentPlan");
    if (cop != kAiNone) {
        const PlanDefinition& d = world.plans().plans[cop];
        CHECK(d.task_forces.size() == 2 && d.task_forces[0].name == "SpaceForce" && d.task_forces[0].minimum_total_size == 4 &&
              d.min_contrast_scale == 1.25f && d.per_failure_contrast_adjust == 0.5f && d.required_categories.size() == 2);
    }
    // Token coverage of the galactic goal / budget / template equations.
    CHECK(world.start(&pd));
    // a2m r3 (A2M_R3_RE_A §1.2): 0x3f8fd0 at the HotM start (every planet has a starbase and empty queues): the
    // candidate is the current starbase's Next_Level_Base when its Base_Level <= level + 1 <= Max_Space_Base, else none.
    // Shola starts with Underworld_Star_Base_1 in this campaign -> Underworld_Star_Base_2.
    starbase_candidates_check(sim, rules, world, "start");
    for (PlanetIndex t = 0; t < sim.planets().size(); ++t)
        if (sim.planets()[t].is_planet && sim.planets()[t].type < rules.types.size() && ai_iequal(rules.types[sim.planets()[t].type].name, "Shola"))
            CHECK(world.starbase_candidate(t) == rules.find_type("Underworld_Star_Base_2"));
    for (int32_t f = 1; f <= 30 * 120; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
    }
    // Which goal functions evaluate on this galaxy (per AI player, every planet / global).
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const AiContext* c = world.context(static_cast<int32_t>(p));
        if (!c) continue;
        uint32_t ok = 0, bad = 0, positive = 0;
        std::map<std::string, std::string> failing;
        for (const GoalSource& s : c->goals.sources()) {
            const uint32_t e = eqs.find(s.function);
            const bool global = defs.goals[s.goal_type].global();
            for (int32_t t = global ? -1 : 0; t < (global ? 0 : static_cast<int32_t>(sim.planets().size())); ++t) {
                EvalContext ctx;
                ctx.self = static_cast<int32_t>(p);
                ctx.target = ctx.location = t;
                ctx.enemy = c->opponent;
                ctx.human = c->opponent;
                SimRng scratch(1);
                ctx.rng = &scratch;
                const EvalResult r = e == kUnresolved ? EvalResult{} : evaluate(eqs, e, ctx, world.resolver());
                if (r.ok) {
                    ++ok;
                    positive += r.value > 0.0 ? 1u : 0u;
                } else {
                    ++bad;
                    failing[s.function] = e == kUnresolved ? "missing" : eval_error_name(r.error);
                }
            }
        }
        std::printf("FoC player %zu goal functions: %u evaluations ok (%u > 0), %u failed:", p, ok, positive, bad);
        for (const auto& [n, why] : failing) std::printf(" %s(%s)", n.c_str(), why.c_str());
        std::printf("\n");
    }
    size_t active = 0;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (const AiContext* c = world.context(static_cast<int32_t>(p)))
            for (const auto& cat : c->goals.active()) active += cat.size();
    std::printf("FoC HotM 120 s: %zu decisions, %zu running plans, %zu active goals, orders %llu (rejected %llu), %zu Lua errors\n",
                world.decision_log().size(), world.running_plans(), active, static_cast<unsigned long long>(world.orders_issued()),
                static_cast<unsigned long long>(world.orders_rejected()), world.lua_errors().size());
    for (const auto& [k, n] : world.proposal_stats()) std::printf("  proposals: %s x%u\n", k.c_str(), n);
    const std::vector<std::string> le = world.lua_errors();
    for (size_t i = 0; i < le.size() && i < 10; ++i) std::printf("  lua: %s\n", le[i].c_str());
    for (size_t i = 0; i < world.decision_log().size() && i < 25; ++i)
        std::printf("  %lld p%d %s\n", static_cast<long long>(world.decision_log()[i].frame), world.decision_log()[i].player,
                    world.decision_log()[i].text.c_str());
    CHECK(world.running_plans() + active > 0);
    // AI execution path finder on the real galaxy: every AI player's finder exists and is on the 90-frame cadence;
    // its Get_Path answers vs the former Lua Find_Path stand-in (fleet A*, all request flags) for every planet pair.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        CHECK(f->created() && f->recomputes() >= 3600u / 90u - 1u);
        uint32_t pairs = 0, reachable = 0, astar_reachable = 0, same = 0;
        gal::PlayerView view = sim.player_view(pi);
        if (view.is_human) view.revealed = gal::Bitset{};
        gal::PathScratch scratch;
        std::vector<PlanetIndex> ap;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const std::vector<PlanetIndex> fw = world.path(pi, a, b);
                gal::PathRequest req;
                req.source = a;
                req.destination = b;
                const bool ok = gal::find_fleet_path(sim.graph(), view, req, scratch, ap);
                reachable += fw.empty() ? 0u : 1u;
                astar_reachable += ok ? 1u : 0u;
                same += (ok ? ap : std::vector<PlanetIndex>{}) == fw ? 1u : 0u;
                if (!fw.empty()) CHECK(fw.front() == a && fw.back() == b);
            }
        std::printf("FoC player %zu AI path finder: %u recomputes, %u pairs, %u reachable (A* stand-in %u), %u identical to A*\n",
                    p, f->recomputes(), pairs, reachable, astar_reachable, same);
        CHECK(reachable > 0);
        // A2k t02: the Lua _FindStageArea stage is list[0] of the target's per-target reachability list at the TF level
        // (held by the player or an ally), the target at level 4, nil above 5 and at levels 0 / 1 for a target not held.
        uint32_t staged = 0;
        const gal::PlayerView sv = sim.player_view(pi);
        for (PlanetIndex t = 0; t < sim.planets().size(); ++t) {
            if (!sim.planets()[t].is_planet) continue;
            const PlanetIndex s = world.stage_area(pi, t, 5);
            const std::vector<PlanetIndex> list = world.stage_list(pi, t, 5);
            CHECK(s == (list.empty() ? kInvalidPlanet : list.front()));
            for (const PlanetIndex k : list) CHECK(gal::PlayerView::bit(sv.held_by_ally, k, false));
            CHECK(world.stage_area(pi, t, 4) == t && world.stage_area(pi, t, 6) == kInvalidPlanet);
            const PlanetIndex s0 = world.stage_area(pi, t, 0);
            if (!gal::PlayerView::bit(sv.held_by_ally, t, false)) CHECK(s0 == kInvalidPlanet);
            else CHECK(s0 == kInvalidPlanet || gal::PlayerView::bit(sv.held_by_ally, s0, false));
            if (s == kInvalidPlanet) continue;
            ++staged;
            CHECK(world.path(pi, s, t).empty() || world.path(pi, s, t).front() == s);
        }
        CHECK(staged > 0 || sim.planets_owned(pi) == 0);
    }
    // FIXPASS_GALACTIC §1 on the real galaxy (shipped GalacticFreeStore.lua drives the moves):
    //   * the Friendly_Only source list holds only planets held by the player or an ally (§1.4);
    //   * the free-store Reachable_Target calls return such a planet or nil;
    //   * Get_Next_Hop passes a pair exactly when every planet after the start has node cost 0 on the matrix.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        const gal::PlayerView view = sim.player_view(pi);
        const std::vector<PlanetIndex> r0 = world.reach_planets(pi, 0);
        for (const PlanetIndex k : r0) CHECK(gal::PlayerView::bit(view.held_by_ally, k, false));
        CHECK(world.reach_planets(pi, 5).size() >= r0.size());
        for (const char* fn : {"Space_Priority_Defense_Score", "Low_Space_Defense_Score", "Ground_Priority_Defense_Score", "One"}) {
            const int32_t t = world.reachable_target(pi, fn, "Friendly", "Friendly_Only", 0.1);
            // a2l t03: no argument 6, so no reachability test; the Friendly application alone keeps it held.
            CHECK(t == -1 || gal::PlayerView::bit(view.held_by_ally, static_cast<PlanetIndex>(t), false));
        }
        uint32_t pairs = 0, pass = 0, blocked = 0;
        std::vector<PlanetIndex> path;
        std::vector<uint32_t> w;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const FreeStoreHopResult r = free_store_next_hop(*f, pi, a, b, false, {});
                f->get_path(a, b, path, &w);
                bool free_nodes = !path.empty();
                for (size_t k = 0; k + 1 < path.size(); ++k) free_nodes = free_nodes && (w[k] >> 16) == 0u;
                CHECK((r.kind == FreeStoreHop::Issue) == (free_nodes && path.size() >= 2));
                if (r.kind == FreeStoreHop::Issue) {
                    ++pass;
                    CHECK(r.hop == path[1]);
                } else {
                    ++blocked;
                }
            }
        size_t no_path = 0;
        for (const AiDecision& d : world.decision_log())
            if (d.player == pi && d.text.find("Unable to find a path") != std::string::npos) ++no_path;
        std::printf("FoC player %zu free store: Friendly_Only reach %zu planets, Get_Next_Hop %u of %u pairs pass (%u fail), "
                    "%zu moves ended without a path\n", p, r0.size(), pass, pairs, blocked, no_path);
        CHECK((!r0.empty() || sim.planets_owned(pi) == 0) && pass > 0);
    }
    {
        // a2m r3: the no-starbase branch of 0x3f8fd0. Destroy the starbases at the end of the run (nothing runs after):
        // Rebel -> Rebel_Star_Base_1, Empire -> Empire_Star_Base_1, the Underworld -> none.
        size_t rebel = 0, empire = 0, other = 0;
        for (PlanetIndex t = 0; t < sim.planets().size(); ++t) {
            const gal::PlanetState& ps = sim.planets()[t];
            if (!ps.is_planet || sim.starbase_type(t) == gal::kNoIndex) continue;
            sim.replace_starbase(t, gal::kNoIndex);
            const bool owned = ps.owner >= 0 && static_cast<size_t>(ps.owner) < sim.players().size();
            const std::string fac = owned ? rules.factions[sim.players()[static_cast<size_t>(ps.owner)].faction].name : "";
            (ai_iequal(fac, "Rebel") ? rebel : ai_iequal(fac, "Empire") ? empire : other) += 1;
        }
        CHECK(starbase_candidates_check(sim, rules, world, "after the starbases are destroyed") > 0 && rebel > 0 && empire > 0 && other > 0);
    }
}
#endif

#ifdef ALAMO_AI_HAVE_VFS

// N1 save / load on the shipped scripts (HotM, as test_foc_data).
void test_save_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("N1 save FoC: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(vcfg, &err);
    if (!v) {
        std::printf("N1 save FoC: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    CHECK(load_ai_definitions(*v, list_ai_data([&v](std::string_view g) { return v->list(g); }), defs));
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("N1 save FoC: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    SaveRigInputs in;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = v.get();
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo = make_galactic_ai_type_info(types);
    for (size_t p = 0; p < setup.players.size(); ++p)
        if (setup.players[p].ai) {
            const std::string fac = rules.factions[setup.players[p].faction].name;
            in.cfg.players.push_back({static_cast<int32_t>(p),
                                      ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    in.cfg.difficulties = load_ai_difficulties(*v);
    in.plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    in.seed_synced = 99u;
    in.seed_free = 1u;
    // Size guards (N1 save size), about twice the 2026-10-29 sizes: 1.72 MB, 141 KB packed. Before pristine pooled VMs
    // were saved by reference the AI chunk held every pooled VM's image (15.3 MB in the headless HotM save at 1 min).
    check_save_load(in, "FoC HotM", 30 * 120, 30 * 60, 12, true, 4000000, 300000);
}
#endif

// A2k t06: the type side of a category test (GalacticAiTypeInfo::category_mask, the Capital structure default) uses the
// data-loaded GameObjectCategoryType converter, so a mod category gets its bit and a renumbered vanilla name its new bits.
void test_category_masks_from_enum_files() {
    MemoryFiles mf;
    // Mod table: Capital renumbered to 0x40, Super moved, Infantry dropped, Mod_Walker = 2^39 (hex, sscanf %llx).
    mf.add("Data\\XML\\Enum\\GameObjectCategoryType.xml",
           "<?xml version=\"1.0\"?>\n<EnumDefinition>\n<Fighter>1</Fighter>\n<Frigate>10</Frigate>\n<Capital>40</Capital>\n"
           "<Super>20</Super>\n<Structure>400</Structure>\n<Mod_Walker>8000000000</Mod_Walker>\n<None>0</None>\n</EnumDefinition>\n");
    objects::EnumRegistry mod_reg;
    mod_reg.load_dynamic(mf);
    const objects::EnumConverter* mod = &mod_reg.get(objects::EnumId::GameObjectCategory);
    const objects::EnumRegistry vanilla_reg;  // built-in FoC table
    const objects::EnumConverter* vanilla = &vanilla_reg.get(objects::EnumId::GameObjectCategory);
    const uint64_t walker = 0x8000000000ull;
    CHECK(ai_category_bit("Mod_Walker", mod) == walker && ai_category_bit("mod_walker ", mod) == walker);
    CHECK(ai_category_bit("Mod_Walker") == 0 && ai_category_bit("Mod_Walker", vanilla) == 0);
    CHECK(ai_category_bit("Capital", mod) == 0x40 && ai_category_bit("Capital") == 0x20 && ai_category_bit("Capital", vanilla) == 0x20);
    CHECK(ai_category_bit("Infantry", mod) == 0 && ai_category_bit("Infantry") == 0x80);
    {
        std::vector<GalacticAiTypeInfo> t(3);
        t[0].categories = {"MOD_WALKER", "FRIGATE"};
        t[1].categories = {"CAPITAL", "INFANTRY"};  // a dropped name adds nothing [port: the OR of the known names]
        t[2].categories = {"STRUCTURE"};
        t[2].garrison_mask = 0x10;
        std::vector<GalacticAiTypeInfo> v = t;
        fill_category_masks(t, mod);
        fill_category_masks(v);  // null = the built-in table (unchanged behaviour)
        CHECK(t[0].category_mask == (walker | 0x10) && t[1].category_mask == 0x40 && t[2].category_mask == 0x400);
        CHECK(v[0].category_mask == 0x10 && v[1].category_mask == (0x20 | 0x80) && v[2].category_mask == 0x400);
        // Structure mask: +0x1658 when set, else the Capital bits of the same converter.
        CHECK(galactic_ai_structure_mask(t, 0, mod) == 0x40 && galactic_ai_structure_mask(t, 2, mod) == 0x10);
        CHECK(galactic_ai_structure_mask(v, 0) == 0x20 && galactic_ai_structure_mask(t, 9) == 0x20);
    }
    {
        // A discarded converter (duplicate value) fails every conversion: no bits, and the Capital default is 0 too (a
        // structure without +0x1658 then meets no category row), not the built-in bit.
        MemoryFiles bad;
        bad.add("Data\\XML\\Enum\\GameObjectCategoryType.xml",
                "<?xml version=\"1.0\"?>\n<EnumDefinition>\n<Capital>20</Capital>\n<Frigate>20</Frigate>\n</EnumDefinition>\n");
        objects::EnumRegistry discarded_reg;
        discarded_reg.load_dynamic(bad);
        const objects::EnumConverter* discarded = &discarded_reg.get(objects::EnumId::GameObjectCategory);
        std::vector<GalacticAiTypeInfo> t(1);
        t[0].categories = {"CAPITAL"};
        fill_category_masks(t, discarded);
        CHECK(t[0].category_mask == 0 && galactic_ai_structure_mask(t, 0, discarded) == 0);
    }
}

// A2k t06: GameObjectTypes loads Data\XML\Enum\GameObjectCategoryType.xml through the provider and
// make_galactic_ai_type_info / GalacticAiWorld take the mask bits from it (a mod category gets its bit, a renumbered Capital
// its new bits, also as the structure-mask default).
void test_type_info_mod_categories() {
    MemoryFiles mf;
    mf.add("Data\\XML\\Enum\\GameObjectCategoryType.xml",
           "<?xml version=\"1.0\"?>\n<EnumDefinition>\n<Fighter>1</Fighter>\n<Frigate>10</Frigate>\n<Capital>40</Capital>\n"
           "<Structure>400</Structure>\n<Mod_Walker>8000000000</Mod_Walker>\n<None>0</None>\n</EnumDefinition>\n");
    mf.add("Data\\XML\\GameObjectFiles.xml", "<L><File>Mod_Units.xml</File></L>");
    mf.add("Data\\XML\\Mod_Units.xml",
           "<R>\n <SpaceUnit Name=\"Mod_AT_AT\"><CategoryMask>Mod_Walker | Capital</CategoryMask></SpaceUnit>\n"
           " <SpaceUnit Name=\"Mod_Ion\"><CategoryMask>Structure</CategoryMask></SpaceUnit>\n"
           " <SpaceUnit Name=\"Mod_Old\"><CategoryMask>Frigate</CategoryMask></SpaceUnit>\n</R>\n");
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    std::vector<std::string> diag;
    CHECK(raw.load(mf, &diag) && types.build(raw, &mf, &diag));
    const objects::TypeIndex atat = raw.find("Mod_AT_AT"), ion = raw.find("Mod_Ion"), old = raw.find("Mod_Old");
    CHECK(atat != objects::kNone && ion != objects::kNone && old != objects::kNone &&
          types.enums().get(objects::EnumId::GameObjectCategory).from_file);
    if (atat == objects::kNone || ion == objects::kNone || old == objects::kNone) return;
    const uint64_t walker = 0x8000000000ull;
    std::vector<GalacticAiTypeInfo> ti = make_galactic_ai_type_info(types);
    CHECK(ti.size() == types.size());
    if (ti.size() != types.size()) return;
    CHECK(ti[atat].category_mask == (walker | 0x40) && ti[atat].category_mask == types.get_mask(atat, "CategoryMask"));
    CHECK(ti[old].category_mask == 0x10 && ti[ion].category_mask == 0x400);
    const objects::EnumConverter* cat = &types.enums().get(objects::EnumId::GameObjectCategory);
    CHECK(galactic_ai_structure_mask(ti, static_cast<uint32_t>(ion), cat) == 0x40);  // Garrison_Category unset -> Capital (mod)
    // Before A2k t06 (built-in table): the mod bit was lost and Capital read 0x20.
    std::vector<GalacticAiTypeInfo> builtin = ti;
    fill_category_masks(builtin);
    CHECK(builtin[atat].category_mask == 0x20 && galactic_ai_structure_mask(builtin, static_cast<uint32_t>(ion)) == 0x20);
    CHECK(ai_category_bit("Mod_Walker", cat) == walker && ai_category_bit("Mod_Walker") == 0);
}
