//! Galactic campaign state for the map view.
//!
//! The save body is packed with the port's own compressor and refers to planets and
//! unit types only by index into the loaded game data, so the launcher asks the game
//! itself to describe a save: `alamo_game --export-save SAVE --out FILE.json`.
//! That needs the same `MODPATH=` arguments the save was made with.

use crate::config;
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct SaveMeta {
    pub name: String,
    pub kind: String,
    pub campaign: String,
    pub title: String,
    pub faction: String,
    pub week: i64,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct Player {
    pub index: i32,
    pub faction: String,
    pub color: Option<String>,
    pub human: bool,
    pub ai: bool,
    pub neutral: bool,
    pub credits: f64,
    pub tech_level: i32,
    pub max_tech_level: i32,
    pub planets: i32,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct Stack {
    #[serde(rename = "type")]
    pub kind: String,
    pub owner: i32,
    pub count: u32,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct Planet {
    pub index: i32,
    pub name: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub is_planet: bool,
    pub x: f32,
    pub y: f32,
    pub owner: i32,
    pub credit_value: f64,
    pub starbase_level: i32,
    pub ground_level: i32,
    pub revealed: bool,
    pub hidden: bool,
    pub destroyed: bool,
    pub corrupted: bool,
    pub in_battle: bool,
    pub structures: Vec<Stack>,
    pub surface: Vec<Stack>,
    pub fleets: Vec<i32>,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(default)]
pub struct Route {
    pub a: i32,
    pub b: i32,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct Fleet {
    pub index: i32,
    pub owner: i32,
    pub planet: i32,
    pub from: i32,
    pub to: i32,
    pub moving: bool,
    pub x: f32,
    pub y: f32,
    pub units: Vec<Stack>,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct Galaxy {
    pub format: i32,
    pub save: SaveMeta,
    pub local_player: i32,
    pub players: Vec<Player>,
    pub planets: Vec<Planet>,
    pub routes: Vec<Route>,
    pub fleets: Vec<Fleet>,
}

#[derive(Clone, Debug, Default)]
pub struct Tally {
    pub space: u32,
    pub ground: u32,
    pub structures: u32,
    pub fleets: u32,
}

impl Galaxy {
    pub fn player(&self, index: i32) -> Option<&Player> {
        self.players.iter().find(|p| p.index == index)
    }

    /// Unit counts per player, added up from fleets and planet surfaces.
    pub fn tally(&self, player: i32) -> Tally {
        let mut t = Tally::default();
        for f in self.fleets.iter().filter(|f| f.owner == player) {
            t.fleets += 1;
            t.space += f.units.iter().map(|u| u.count).sum::<u32>();
        }
        for p in &self.planets {
            t.ground += p.surface.iter().filter(|s| s.owner == player).map(|s| s.count).sum::<u32>();
            t.structures += p.structures.iter().filter(|s| s.owner == player).map(|s| s.count).sum::<u32>();
        }
        t
    }
}

pub fn pretty(name: &str) -> String {
    name.replace('_', " ")
}

// ------------------------------------------------------------------ comparing two saves

/// Change in a faction's unit counts (A minus B).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TallyDiff {
    pub space: i64,
    pub ground: i64,
    pub structures: i64,
    pub fleets: i64,
}

impl TallyDiff {
    /// Units and buildings together (fleets are groupings, not units).
    pub fn total(&self) -> i64 {
        self.space + self.ground + self.structures
    }
}

/// How one faction's position differs between two saves. Every number is A minus B.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FactionDiff {
    pub faction: String,
    /// The faction's player index in A and in B, where it exists.
    pub a: Option<i32>,
    pub b: Option<i32>,
    pub neutral: bool,
    /// Planets it holds in A that it didn't hold in B, and the other way round.
    pub gained: u32,
    pub lost: u32,
    /// `None`: hidden by fog of war, or the faction is missing from one of the saves.
    pub credits: Option<f64>,
    pub tech: Option<i32>,
    pub units: Option<TallyDiff>,
}

impl FactionDiff {
    pub fn net(&self) -> i32 {
        self.gained as i32 - self.lost as i32
    }
}

/// A planet whose owner differs. `from` is the owner in B, `to` the owner in A; an empty
/// name means nobody owned it.
#[derive(Clone, Debug, PartialEq)]
pub struct OwnerChange {
    pub planet: String,
    /// Position in `a.planets` / `b.planets` (not the planets' own `index` fields).
    pub a: usize,
    pub b: usize,
    pub from: String,
    pub to: String,
}

/// A planet whose space station level differs (`from` in B, `to` in A).
#[derive(Clone, Debug, PartialEq)]
pub struct StationChange {
    pub planet: String,
    pub a: usize,
    pub b: usize,
    pub from: i32,
    pub to: i32,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct GalaxyDiff {
    pub owner_changes: Vec<OwnerChange>,
    pub stations: Vec<StationChange>,
    /// A's factions in A's order, then any that only B has.
    pub factions: Vec<FactionDiff>,
    /// Fleets counted on each side (only the local faction's unless everything is shown).
    pub fleets_a: usize,
    pub fleets_b: usize,
    /// Planets found in both saves, whether or not they could be compared under fog.
    pub matched: usize,
}

impl GalaxyDiff {
    pub fn fleets_delta(&self) -> i64 {
        self.fleets_a as i64 - self.fleets_b as i64
    }
}

/// Two saves can be compared when they come from the same campaign.
pub fn comparable(a: &Galaxy, b: &Galaxy) -> Result<(), String> {
    let (ca, cb) = (a.save.campaign.trim(), b.save.campaign.trim());
    if ca.eq_ignore_ascii_case(cb) {
        return Ok(());
    }
    let name = |g: &Galaxy| {
        let t = if g.save.title.is_empty() { &g.save.campaign } else { &g.save.title };
        if t.is_empty() {
            "an unnamed campaign".to_owned()
        } else {
            pretty(t)
        }
    };
    Err(format!(
        "These saves are from different campaigns ({} and {}), so they can't be compared.",
        name(a),
        name(b)
    ))
}

fn faction_of(g: &Galaxy, player: i32) -> String {
    g.player(player).map(|p| p.faction.clone()).unwrap_or_default()
}

/// Rows that are real planets. An export without the flag at all is treated as all planets.
fn real_planets(g: &Galaxy) -> Vec<bool> {
    let flagged = g.planets.iter().any(|p| p.is_planet);
    g.planets.iter().map(|p| p.is_planet || !flagged).collect()
}

/// What changed between B ("then") and A ("now").
///
/// Planets are matched by name and factions by faction name: the indices in an export are
/// only meaningful inside that export. With `show_all` off the result holds only what A's
/// local faction could know: a planet counts only when that faction has seen it in both
/// saves, and other factions' credits, tech and forces stay `None`.
pub fn diff(a: &Galaxy, b: &Galaxy, show_all: bool) -> GalaxyDiff {
    use std::collections::HashMap;

    let local = faction_of(a, a.local_player);
    let seen = |p: &Planet| show_all || (p.revealed && !p.hidden);
    let (real_a, real_b) = (real_planets(a), real_planets(b));
    let mut b_by_name: HashMap<&str, usize> = HashMap::new();
    for (i, p) in b.planets.iter().enumerate() {
        if real_b[i] {
            b_by_name.entry(p.name.as_str()).or_insert(i);
        }
    }

    let mut out = GalaxyDiff::default();
    let mut gained: HashMap<String, u32> = HashMap::new();
    let mut lost: HashMap<String, u32> = HashMap::new();
    for (ia, pa) in a.planets.iter().enumerate() {
        if !real_a[ia] {
            continue;
        }
        let Some(&ib) = b_by_name.get(pa.name.as_str()) else { continue };
        let pb = &b.planets[ib];
        out.matched += 1;
        if !(seen(pa) && seen(pb)) {
            continue;
        }
        let (to, from) = (faction_of(a, pa.owner), faction_of(b, pb.owner));
        if to != from {
            if !to.is_empty() {
                *gained.entry(to.clone()).or_default() += 1;
            }
            if !from.is_empty() {
                *lost.entry(from.clone()).or_default() += 1;
            }
            out.owner_changes.push(OwnerChange { planet: pa.name.clone(), a: ia, b: ib, from, to });
        }
        if pa.starbase_level != pb.starbase_level {
            out.stations.push(StationChange {
                planet: pa.name.clone(),
                a: ia,
                b: ib,
                from: pb.starbase_level,
                to: pa.starbase_level,
            });
        }
    }

    let b_player = |faction: &str| b.players.iter().find(|p| p.faction == faction);
    for pa in &a.players {
        if out.factions.iter().any(|f| f.faction == pa.faction) {
            continue;
        }
        let pb = b_player(&pa.faction);
        let open = show_all || pa.faction == local;
        let both = pb.filter(|_| open);
        out.factions.push(FactionDiff {
            faction: pa.faction.clone(),
            a: Some(pa.index),
            b: pb.map(|p| p.index),
            neutral: pa.neutral,
            gained: gained.get(&pa.faction).copied().unwrap_or(0),
            lost: lost.get(&pa.faction).copied().unwrap_or(0),
            credits: both.map(|pb| pa.credits - pb.credits),
            tech: both.map(|pb| pa.tech_level - pb.tech_level),
            units: both.map(|pb| {
                let (ta, tb) = (a.tally(pa.index), b.tally(pb.index));
                TallyDiff {
                    space: ta.space as i64 - tb.space as i64,
                    ground: ta.ground as i64 - tb.ground as i64,
                    structures: ta.structures as i64 - tb.structures as i64,
                    fleets: ta.fleets as i64 - tb.fleets as i64,
                }
            }),
        });
    }
    for pb in &b.players {
        if out.factions.iter().any(|f| f.faction == pb.faction) {
            continue;
        }
        out.factions.push(FactionDiff {
            faction: pb.faction.clone(),
            a: None,
            b: Some(pb.index),
            neutral: pb.neutral,
            gained: gained.get(&pb.faction).copied().unwrap_or(0),
            lost: lost.get(&pb.faction).copied().unwrap_or(0),
            credits: None,
            tech: None,
            units: None,
        });
    }

    let count = |g: &Galaxy| g.fleets.iter().filter(|f| show_all || faction_of(g, f.owner) == local).count();
    out.fleets_a = count(a);
    out.fleets_b = count(b);
    out
}

/// Runs the game's exporter. Blocking: call it from a worker thread.
pub fn export(exe: &Path, foc: &Path, data_args: &[String], save: &Path) -> Result<Galaxy, String> {
    let cache = config::data_dir().join("cache");
    std::fs::create_dir_all(&cache).map_err(|e| e.to_string())?;
    static SEQ: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let seq = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let out: PathBuf = cache.join(format!("export-{}-{seq}.json", std::process::id()));
    let _ = std::fs::remove_file(&out);

    let mut cmd = Command::new(exe);
    cmd.arg("--game").arg(foc).args(data_args).arg("--export-save").arg(save).arg("--out").arg(&out);
    if let Some(dir) = exe.parent() {
        cmd.current_dir(dir);
    }
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);
    }
    let output = cmd.output().map_err(|e| format!("Couldn't run {}: {e}", exe.display()))?;
    if !output.status.success() {
        let err = String::from_utf8_lossy(&output.stderr);
        let all = format!("{err}{}", String::from_utf8_lossy(&output.stdout));
        let lower = all.to_lowercase();
        if output.status.code() == Some(2) && lower.contains("usage") {
            return Err("This build of alamo_game.exe doesn't have the save exporter (--export-save). The campaign map needs a build that includes it.".into());
        }
        let detail = err
            .lines()
            .rev()
            .find(|l| !l.trim().is_empty())
            .unwrap_or("no message")
            .trim()
            .to_owned();
        let hint = if lower.contains("mismatch") || lower.contains("data_args") || lower.contains("fingerprint") {
            " This save was made with a different mod list than the active profile; switch to the profile it was played with."
        } else {
            ""
        };
        return Err(format!("The game couldn't read this save: {detail}.{hint}"));
    }
    let text = std::fs::read_to_string(&out).map_err(|e| format!("The exporter wrote no file: {e}"))?;
    let _ = std::fs::remove_file(&out);
    serde_json::from_str::<Galaxy>(&text).map_err(|e| format!("Couldn't understand the exported save: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A real export: 13 rows (12 planets and the galaxy core), 6 players, local player 5.
    fn sample() -> Galaxy {
        serde_json::from_str(include_str!("testdata/galaxy_sample.json")).expect("sample export parses")
    }

    fn planet<'a>(g: &'a mut Galaxy, name: &str) -> &'a mut Planet {
        g.planets.iter_mut().find(|p| p.name == name).expect("planet in sample")
    }

    fn faction<'a>(d: &'a GalaxyDiff, name: &str) -> &'a FactionDiff {
        d.factions.iter().find(|f| f.faction == name).expect("faction in diff")
    }

    #[test]
    fn sample_is_what_the_tests_assume() {
        let mut g = sample();
        assert_eq!(g.planets.len(), 13);
        assert_eq!(g.players.len(), 6);
        assert_eq!(faction_of(&g, g.local_player), "Underworld");
        let owner = planet(&mut g, "Kuat").owner;
        assert_eq!(faction_of(&g, owner), "Empire");
    }

    #[test]
    fn identical_saves_have_no_differences() {
        let g = sample();
        for show_all in [false, true] {
            let d = diff(&g, &g, show_all);
            assert!(d.owner_changes.is_empty());
            assert!(d.stations.is_empty());
            assert_eq!(d.matched, 12, "the galaxy core row is not a planet");
            assert_eq!(d.fleets_delta(), 0);
            assert!(d.factions.iter().all(|f| f.gained == 0 && f.lost == 0));
        }
        let d = diff(&g, &g, true);
        assert_eq!(d.factions.len(), 6);
        assert!(d.factions.iter().all(|f| f.credits == Some(0.0) && f.tech == Some(0)));
        assert!(d.factions.iter().all(|f| f.units == Some(TallyDiff::default())));
    }

    #[test]
    fn owner_changes_run_from_b_to_a() {
        let a = sample();
        let mut b = sample();
        planet(&mut b, "Kuat").owner = 0; // Rebel then, Empire now
        planet(&mut b, "Shola").owner = -1; // nobody then, Underworld now
        let d = diff(&a, &b, false);
        assert_eq!(d.owner_changes.len(), 2);
        let kuat = d.owner_changes.iter().find(|c| c.planet == "Kuat").unwrap();
        assert_eq!((kuat.from.as_str(), kuat.to.as_str()), ("Rebel", "Empire"));
        assert_eq!(a.planets[kuat.a].name, "Kuat");
        assert_eq!(b.planets[kuat.b].name, "Kuat");
        let shola = d.owner_changes.iter().find(|c| c.planet == "Shola").unwrap();
        assert_eq!((shola.from.as_str(), shola.to.as_str()), ("", "Underworld"));

        let empire = faction(&d, "Empire");
        assert_eq!((empire.gained, empire.lost, empire.net()), (1, 0, 1));
        let rebel = faction(&d, "Rebel");
        assert_eq!((rebel.gained, rebel.lost, rebel.net()), (0, 1, -1));
        assert_eq!(faction(&d, "Underworld").net(), 1);

        // Swapping the two sides turns every change around.
        let r = diff(&b, &a, false);
        let kuat = r.owner_changes.iter().find(|c| c.planet == "Kuat").unwrap();
        assert_eq!((kuat.from.as_str(), kuat.to.as_str()), ("Empire", "Rebel"));
        assert_eq!(faction(&r, "Empire").net(), -1);
    }

    #[test]
    fn planets_are_matched_by_name_not_index() {
        let a = sample();
        let mut b = sample();
        // Another data set: same planets, different order and numbering.
        b.planets.reverse();
        for (i, p) in b.planets.iter_mut().enumerate() {
            p.index = 100 + i as i32;
        }
        // And the players numbered differently too.
        for p in &mut b.players {
            p.index += 10;
        }
        for p in &mut b.planets {
            if p.owner >= 0 {
                p.owner += 10;
            }
            for s in p.surface.iter_mut().chain(p.structures.iter_mut()) {
                s.owner += 10;
            }
        }
        for f in &mut b.fleets {
            f.owner += 10;
        }
        b.local_player += 10;
        let d = diff(&a, &b, true);
        assert!(d.owner_changes.is_empty(), "{:?}", d.owner_changes);
        assert!(d.stations.is_empty());
        assert_eq!(d.matched, 12);
        assert!(d.factions.iter().all(|f| f.units == Some(TallyDiff::default())));
        assert_eq!(faction(&d, "Empire").b, Some(11));
    }

    #[test]
    fn credits_tech_and_units_are_a_minus_b() {
        let a = sample();
        let mut b = sample();
        for p in &mut b.players {
            match p.faction.as_str() {
                "Underworld" => {
                    p.credits -= 1500.0;
                    p.tech_level -= 1;
                }
                "Empire" => p.credits += 400.0,
                _ => {}
            }
        }
        // Then: the Underworld had one fleet fewer (three space units in it) and one more
        // building; the Empire had two more ground units.
        let before = b.fleets.len();
        b.fleets.retain(|f| f.index != 7);
        assert_eq!(b.fleets.len(), before - 1);
        planet(&mut b, "Shola").structures.push(Stack { kind: "U_Ground_Barracks".into(), owner: 5, count: 1 });
        planet(&mut b, "Kuat").surface.push(Stack { kind: "Imperial_Stormtrooper_Squad".into(), owner: 1, count: 2 });

        let d = diff(&a, &b, true);
        let uw = faction(&d, "Underworld");
        assert_eq!(uw.credits, Some(1500.0));
        assert_eq!(uw.tech, Some(1));
        assert_eq!(uw.units, Some(TallyDiff { space: 3, ground: 0, structures: -1, fleets: 1 }));
        assert_eq!(uw.units.unwrap().total(), 2);
        let empire = faction(&d, "Empire");
        assert_eq!(empire.credits, Some(-400.0));
        assert_eq!(empire.units, Some(TallyDiff { space: 0, ground: -2, structures: 0, fleets: 0 }));
        assert_eq!((d.fleets_a, d.fleets_b, d.fleets_delta()), (14, 13, 1));

        // Under fog only the local faction's books are open.
        let d = diff(&a, &b, false);
        assert_eq!(faction(&d, "Underworld").credits, Some(1500.0));
        let empire = faction(&d, "Empire");
        assert_eq!((empire.credits, empire.tech, empire.units), (None, None, None));
        // ... and only its own fleets are counted.
        assert_eq!((d.fleets_a, d.fleets_b), (5, 4));
    }

    #[test]
    fn station_level_changes_are_listed() {
        let a = sample();
        let mut b = sample();
        planet(&mut b, "Kuat").starbase_level = 1; // 3 now
        planet(&mut b, "Yavin").starbase_level = 2; // 1 now
        let d = diff(&a, &b, false);
        let mut got: Vec<(&str, i32, i32)> = d.stations.iter().map(|s| (s.planet.as_str(), s.from, s.to)).collect();
        got.sort();
        assert_eq!(got, vec![("Kuat", 1, 3), ("Yavin", 2, 1)]);
        assert!(d.owner_changes.is_empty());
    }

    #[test]
    fn fog_of_war_hides_changes_on_unseen_planets() {
        let mut a = sample();
        let mut b = sample();
        planet(&mut b, "Kuat").owner = 0;
        planet(&mut b, "Kuat").starbase_level = 0;
        // The local faction hasn't seen Kuat in A: its owner must not leak through the diff.
        planet(&mut a, "Kuat").revealed = false;
        let d = diff(&a, &b, false);
        assert!(d.owner_changes.is_empty());
        assert!(d.stations.is_empty());
        assert!(d.factions.iter().all(|f| f.gained == 0 && f.lost == 0));
        assert_eq!(d.matched, 12);
        // "Show everything" lifts the fog.
        let d = diff(&a, &b, true);
        assert_eq!(d.owner_changes.len(), 1);
        assert_eq!(d.stations.len(), 1);
        assert_eq!(faction(&d, "Empire").gained, 1);

        // Seen now but not back then: what it was then isn't known either.
        planet(&mut a, "Kuat").revealed = true;
        planet(&mut b, "Kuat").revealed = false;
        assert!(diff(&a, &b, false).owner_changes.is_empty());
        // A planet the map hides altogether is treated the same way.
        planet(&mut b, "Kuat").revealed = true;
        planet(&mut a, "Kuat").hidden = true;
        assert!(diff(&a, &b, false).owner_changes.is_empty());
        planet(&mut a, "Kuat").hidden = false;
        assert_eq!(diff(&a, &b, false).owner_changes.len(), 1);
    }

    #[test]
    fn planets_and_factions_on_one_side_only() {
        let a = sample();
        let mut b = sample();
        b.planets.retain(|p| p.name != "Yavin");
        b.players.retain(|p| p.faction != "Hostile");
        b.players.push(Player { index: 9, faction: "Hutts".into(), ..Default::default() });
        let d = diff(&a, &b, true);
        assert_eq!(d.matched, 11);
        assert!(d.owner_changes.is_empty());
        let hostile = faction(&d, "Hostile");
        assert_eq!((hostile.a, hostile.b, hostile.credits), (Some(4), None, None));
        let hutts = faction(&d, "Hutts");
        assert_eq!((hutts.a, hutts.b, hutts.units), (None, Some(9), None));
        assert_eq!(d.factions.last().unwrap().faction, "Hutts");
    }

    #[test]
    fn only_saves_of_one_campaign_compare() {
        let a = sample();
        let mut b = sample();
        assert!(comparable(&a, &b).is_ok());
        b.save.campaign = "sandbox_heart_of_the_maw_underworld".into();
        assert!(comparable(&a, &b).is_ok());
        b.save.campaign = "Sandbox_Equal_Footing_Empire".into();
        b.save.title = "Equal Footing".into();
        let e = comparable(&a, &b).unwrap_err();
        assert!(e.contains("different campaigns") && e.contains("Heart of the Maw") && e.contains("Equal Footing"), "{e}");
    }
}
