#include "alamo/core/galaxy.hpp"

#include <cmath>

#include "alamo/core/hash.hpp"

namespace alamo {

PlanetIndex PlanetTable::add(std::string_view planet_name) {
    name.emplace_back(planet_name);
    name_crc.push_back(alamo::name_crc(planet_name));
    position.push_back({});
    flags.push_back(0);
    credit_value.push_back(0);
    max_space_base.push_back(0);
    max_ground_base.push_back(0);
    return static_cast<PlanetIndex>(name.size() - 1);
}

PlanetIndex PlanetTable::find(std::string_view planet_name) const {
    const uint32_t crc = alamo::name_crc(planet_name);
    for (size_t i = 0; i < name.size(); ++i)
        if (name_crc[i] == crc && iequals(name[i], planet_name)) return static_cast<PlanetIndex>(i);
    return kInvalidPlanet;
}

uint32_t TradeRouteTable::add(std::string_view route_name) {
    name.emplace_back(route_name);
    name_crc.push_back(alamo::name_crc(route_name));
    point_a.push_back(kInvalidPlanet);
    point_b.push_back(kInvalidPlanet);
    hs_speed_factor.push_back(1.0f);
    political_control_gain.push_back(0.0f);
    credit_gain_factor.push_back(0.0f);
    return static_cast<uint32_t>(name.size() - 1);
}

std::vector<std::string> GalaxyMap::validate() const {
    std::vector<std::string> errs;
    const size_t n = planets.size();
    auto bad = [&](std::string s) { errs.push_back(std::move(s)); };
    if (planets.name_crc.size() != n || planets.position.size() != n || planets.flags.size() != n ||
        planets.credit_value.size() != n || planets.max_space_base.size() != n ||
        planets.max_ground_base.size() != n)
        bad("planet table columns have different lengths");
    for (size_t i = 0; i < n && i < planets.position.size(); ++i) {
        const auto& p = planets.position[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            bad("planet " + planets.name[i] + " has a non-finite position");
    }
    const size_t r = routes.size();
    if (routes.point_a.size() != r || routes.point_b.size() != r || routes.hs_speed_factor.size() != r)
        bad("route table columns have different lengths");
    for (size_t i = 0; i < r; ++i) {
        if (routes.point_a[i] >= n || routes.point_b[i] >= n)
            bad("route " + routes.name[i] + " references a planet not in the campaign");
        else if (routes.point_a[i] == routes.point_b[i])
            bad("route " + routes.name[i] + " connects a planet to itself");
        if (!(routes.hs_speed_factor[i] > 0.0f)) bad("route " + routes.name[i] + " has HS_Speed_Factor <= 0");
    }
    if (!(max_fleet_move_distance >= 0.0f)) bad("max_fleet_move_distance is negative or NaN");
    return errs;
}

}  // namespace alamo
