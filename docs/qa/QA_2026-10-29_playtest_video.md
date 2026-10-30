# QA review: playtest video 2026-10-29 20:20

Source: `C:\Users\Gabriel\Videos\Radeon ReLive\unknown\unknown_2026.10.29-20.20.mp4` (3840×2160, 60 fps, 2 min 24 s, AMD RX 9070 XT).
Build: `alamo_game` (the port). The Video dialog shows the Vulkan V-sync modes, so this is not the retail exe.
Method: one frame every 5 s, full-resolution crops of suspect areas, and a per-frame change scan to check frame pacing.
Docs checked: `docs/PHASE4.md` (playtest logs 1–3), `docs/re/GALACTIC_DIALOGS.md`, `docs/re/HUD_GAPS.md`.

**Correction (2026-10-30):** R1 was misdiagnosed. The dotted green lines are movement path lines, not lasers; `alamo_game` drew no laser bolts at all at the time of the capture. U8's Low / Max are the shipped end captions, not value labels. Fixes are on the roadmap, group `pt4qa`. Also: U13 is not in the port (no FPS counter in `engine/`, most likely an external overlay), P2 is the video encoder's 3-sample cycle, and R5's expectation was wrong (base shows opaque faction-tinted discs).

## Timeline
| t (s) | Scene |
|---|---|
| 0–5 | Galactic, Kashyyyk planet view, pause menu open |
| 5–33 | Galactic: fleet organiser, galaxy map, fleet moves Felucia → Kashyyyk |
| 33–36 | Transition to the Felucia space battle (about 1.1 s frozen frame, no load screen) |
| 36–78 | Tactical space battle: hero tooltip, Imperial fleet attacking the station |
| 78–98 | Pause menu → Options → Video dialog |
| 98–135 | Battle continues, Space Station Level 2 tooltip, station destroyed |
| 135–144 | Victory / battle summary screen |

## Defects

Severity: **S1** breaks the game or wrong data, **S2** clearly visible to the player, **S3** polish.
Status: **NEW** = not in the docs; **KNOWN** = already listed as open in `PHASE4.md`.

### Rendering (engine/render)
| # | Sev | Status | Where | Defect | Expected / best practice |
|---|---|---|---|---|---|
| R1 | S1 | NEW | tactical 60–80 s, 125 s | Laser fire shows as dozens of screen-long dotted green lines. They look like particles spawned along the full flight path, or bolts never removed after impact. They hide the battle | Short stretched bolts (`Projectile_Length`) that vanish on impact. Check the projectile lifetime / despawn and the particle trail emit rate (PHASE4 L210: "length scale unverified") |
| R2 | S2 | NEW | galactic 0–15 s (and the emitter in every frame, top-centre ≈ x 1230 px at 4K) | A green emitter sprite at the top edge with an **opaque, flat grey, untextured cone** reaching down to the planet | Same class as GALACTIC_DIALOGS §5.4: an unresolved texture drawn as a flat tint, or a hidden marker mesh being drawn. The cone should be an additive, textured hologram, or not drawn at all |
| R3 | S2 | NEW | galactic 0–15 s, left edge behind the advisor droid | A **solid saturated-blue triangle** (flat fill, no texture or shading) | Same diagnosis as R2: dump the mesh / component under it and skip hidden meshes |
| R4 | S2 | KNOWN-class | tactical 40–135 s | The space FOW grid is opaque and dark, drawn **over the planet backdrop** (planet looks see-through) and ends in a hard rectangle edge at the map bounds | A faint grid on the play plane only: lower alpha, depth-tested against the backdrop, soft edge fade. Same family as the open "Endor nebula through planets" (PHASE4 L210) |
| R5 | S3 | NEW | galactic orbit slots | Empty fleet-slot ovals drawn as opaque dark-grey discs | Base game: translucent outline ovals, shown while selecting or dragging (playtest 3 item 4) |

### HUD and UI (engine/ui, apps/game)
| # | Sev | Status | Where | Defect | Expected / best practice |
|---|---|---|---|---|---|
| U1 | S1 | NEW | victory screen 135–144 s | The title shows the **raw map file name** `_Space_Planet_Felucia_01.ted` | The localized battle / planet title ("Battle of Felucia" style text id). Never show asset paths to players |
| U2 | S2 | NEW | victory screen | Long unit names are **shrunk to an unreadable size** ("Corellian Corvette x2", "Tartan Patrol Cruiser", "Skipray Blastboat x12") instead of wrapping | Wrap first, then ellipsis, with a minimum font size. Microsoft's guidance: at least 14 px text height at 1080p, 18 px recommended (≈ 28–36 px at 4K) |
| U3 | S2 | NEW | victory screen | Loss rows start mid-panel (empty top third), ship icons stick out past the panel's left frame, and the "Your losses: / Enemy losses:" headers sit on the panel border | Rows top-aligned inside the panel's content rect; icons inside the frame; headers above the border |
| U4 | S1 | NEW | tactical tooltip 50 s (Accuser / Piett), 112 s (station) | Tooltip text **overflows the box**: the "Good against" list runs left out of the box and over its own label, so the label disappears. No wrapping | HUD_GAPS §2.1: lines are wrapped (0x2df0c0), right column clipped to the box |
| U5 | S2 | NEW | same tooltips | A flat light-blue box with grey text: low contrast, and the faction frame is missing | HUD_GAPS §2.1: `i_tooltip_back.tga` + `i_tooltip_empire_frame` / `_rebel_frame`. Check contrast against XAG 102 (high contrast for menu and gameplay text) |
| U6 | S2 | NEW | same tooltip | World-space unit icon / health bar drawn **on top of** the tooltip | Tooltips are layer 10, above the world-space HUD layer |
| U7 | S2 | NEW | tactical 40–80 s | Every fighter in a squadron gets its own icon box + health bar, stacked and overlapping, and the boxes float well above the ships | One marker per squadron (container unit) placed at the squadron / ship |
| U8 | S2 | KNOWN | Video dialog 90 s | Labels contradict their controls: Detail Level reads "Low" with the slider at Max, Gamma "Low" with the slider mid-way | The value label follows the slider. PHASE4 L227 also says detail, gamma and AA are "saved but not applied" |
| U9 | S2 | NEW | Video dialog | V-sync controls contradict each other: "V-sync mode: Standard (FIFO)" and "Vsync Enabled" unchecked, with the status line "presenting with no v-sync" | One control. In Vulkan, FIFO and MAILBOX are both v-sync (no tearing) and IMMEDIATE is "off". Grey out the mode list while V-sync is off, or fold "off" into the list |
| U10 | S3 | KNOWN-partly | Video dialog | Resolution list has only 3840×2160 entries (75–160 Hz), with near-duplicates 119 Hz / 120 Hz | List lower resolutions and collapse fractional rates (119.88 → 120) or show them with decimals |
| U11 | S3 | NEW | pause menu 0 s, 98 s | An empty slot between Options and Quit Game | Hide the slot and close the gap, or show the missing button (restart / objectives) |
| U12 | S3 | NEW | galactic map 20 s | Hero / unit icons cover planet labels (TATOOINE, CORELLIA) | Offset the labels or icons |
| U13 | S3 | NEW | every frame | The FPS counter (top-left, orange) is always on in a player capture | Behind a debug toggle or an options checkbox |

### Performance and frame pacing (game host, render)
| # | Sev | Status | Where | Defect | Expected / best practice |
|---|---|---|---|---|---|
| P1 | S2 | NEW | 33–36 s | Galactic → tactical switch freezes the last frame (~33–34 s) before the load screen shows (the second pass found the load screen at 34–36 s; see D4) | Show the battle load screen (BATTLE_TRANSITIONS) or fade out before the blocking load. Better still, load on a job thread |
| P2 | S3 | verify | galactic 17–30 s | Parts of the galactic view change only every 3rd frame (a 50 ms / 20 Hz step) while the tactical view updates every frame | PHASE4 says views interpolate between 30 Hz ticks. The 20 Hz object may be a flipbook animation (fine) or an object outside the interpolation path. Check against a `--profile` run |
| P3 | S3 | info | FPS counter | Galactic ≈ 490–590 fps, tactical with fleets ≈ 150–350 fps at 4K | Fine for now. Use it as the OPT1 baseline. Frame-time capped by the user's cap / V-sync in shipping config |

## Prioritized checklist

P0 and P1 items are tracked on the live roadmap as group `pt4qa` ("Playtest video QA", box Playtests). That includes the deeper-dive items D1–D7, D9 and D13 (t13–t20), with D1–D3 at P0. The P2 items are t21–t30 and the retail-verify list V1–V6 is t31–t36, so every item in this report is now on the roadmap.

**P0: fix first (wrong data, battle unreadable)**
- [ ] R1 Laser bolts: fix the projectile lifetime / trail emission so bolts are short and vanish on impact (render + sim projectile feed)
- [ ] U1 Victory screen: show the localized battle title instead of the `.ted` file name
- [ ] U4 Tooltip: wrap and clip lines inside the box; restore the "Good against" label

**P1: clearly visible defects**
- [ ] R2 / R3 Find and fix the flat grey cone and the flat blue triangle in galactic (dump the mesh / component, skip hidden meshes, resolve the texture)
- [ ] U7 One marker per fighter squadron, anchored to the ship
- [ ] R4 Space grid: lower its opacity, depth-test it against the planet backdrop, fade its edges
- [ ] U9 V-sync: one consistent control and status line (FIFO / Mailbox = on, Immediate = off)
- [ ] U8 Video dialog value labels follow the sliders, and the settings apply (already open in PHASE4 L227)
- [ ] U2 / U3 Victory screen layout: wrap with a minimum font size, top-align rows, keep icons inside the frame
- [ ] U5 / U6 Tooltip faction frame and texture, readable contrast, drawn above world-space icons
- [ ] P1 Load screen or fade on the galactic → tactical transition

**P2: polish**
- [ ] U10 Resolution list: lower resolutions, de-duplicate refresh rates
- [ ] U11 Remove the gap in the pause menu
- [ ] U12 Keep planet labels clear of icons
- [ ] R5 Fleet-slot ovals as translucent outlines
- [ ] U13 FPS counter behind a toggle
- [ ] P2 Check the 20 Hz stepping on the galactic map

**Process**
- [ ] Add a screenshot test per screen (victory, tooltips, video dialog) to the smokes, as done for `ALAMO_SMOKE_DRAG_BOX`
- [ ] Pseudo-localization pass (strings made 30–40 % longer) to catch U2 / U4-type overflow before real text does

## Deeper dive (same day, second pass)

Method: one frame per second (144 frames), reviewed by four reviewers in parallel by segment. A full-video scan for
one-frame flashes compared each frame with its neighbours, and the hits were checked as frame triplets. I re-checked
every finding marked high or medium at full resolution. Crops: `...\scratchpad\{s1crops,crops_b,crops_s3,seg111,trip}\`.

### Revised diagnosis of R2 / R3
The grey "cone" and the blue "triangle" are **galaxy-map route lanes** (white Kashyyyk–Felucia and blue Kashyyyk–Kuat,
the same colours as on the galaxy map at 20–30 s). They are drawn as flat world-space bands into the planet close-up
view, so perspective turns them into huge wedges. A light-blue route marker square rides on the blue band. During the
zoom from the planet view out to the galaxy (12.8–15.3 s), a **giant blue planet-selection ring** is also drawn across
the screen for several frames (`trip\trip_0012.85.jpg`). Base FoC hides lanes and the galaxy selection ring in the
planet close-up. Fix: don't draw galaxy-layer overlays in the planet view, or fade them with zoom.

### New defects
| # | Sev | Where | Defect | Expected |
|---|---|---|---|---|
| D1 | S1 | 137.8 → 137.9 s | The battle ends in **Victory with the enemy Space Station Level 2 still intact**: a hard cut to the summary, with no station death effect, no hold and no fade. The station is **missing from "Enemy losses"** | Space victory needs the station destroyed. Its break-up plays, then a fade to the galaxy, and the station is listed among losses. Check the victory condition and loss recording |
| D2 | S1 | tactical 37–137 s | Lower-left tactical HUD (help / pause / fast-forward, the column beside the radar, the command block, the population icon) is **drawn only while the cursor is over a unit**. It is still clickable while invisible: its tooltip shows over an empty box. The flash scan shows it toggling for single frames (44.25 s) | Always visible during tactical play |
| D3 | S1 | galactic 0–17 s | Build bar: the **space-unit row is drawn on top of the structure row**, shifted ~2.5 slots, so unit icons cover structure buttons 3–5 and the prices collide ("5\|150", "8\|180", "2\|60"). Unit icons are undersized; with Kuat selected, column 2 is empty | One grid filled from the left, one full-size icon per slot (`s1crops\build_6.png`) |
| D4 | S2 | battle load screen 34–36 s | Every info field is **empty**: Tactical Information, Planetary Advantages, Historical Reference, Weather, Terrain, Population and the planet description. Labels sit on the panel borders or over the planet render. The live scene shows around the dialog | Fields filled from the planet's text. Labels inside the panels on an opaque backdrop (`crops_b\load34.png`) |
| D5 | S2 | tactical 38–137 s | **Flat lavender squares** (no texture, grey border) above the command bar over the blue lights; their number follows the selection (2–5) | Textured group / selection tabs, or nothing (`seg111\strip116.png`) |
| D6 | S2 | 12.8–15.3 s | Giant blue selection ring during the galactic zoom (see the revised R2 / R3 above) | Hidden in planet view |
| D7 | S2 | tactical, Felucia panel | Population shows the two-person icon but **no "current / max" count** | e.g. "12/20" |
| D8 | S3 | Advanced Video 83–89, 126 s | "Heat Distortion" and "Ambient occlusion" auto-shrunk and in different case. Dropdown value text is a plain sans font touching the box edge. Captions sit on the dropdown borders. Checkbox art differs between the Video and Advanced Video pages | One label size and Title Case, Alamo UI font in dropdowns, one checkbox style |
| D9 | S2 | Video ↔ Advanced Video 86–90 s | Anti-aliasing is set in two places that don't agree: Advanced says Temporal (TAA) after Apply, while the Video page AA slider still reads Off | One setting, or both kept in sync |
| D10 | S3 | Camera and Controls 95–98 s | The dialog title uses body-text size instead of the gold header font. Key-binding fields: glyphs touch the borders with no padding, and the "Pause" field matches neither column. Labels in sentence case | Header font, padded fields, aligned columns |
| D11 | S3 | tactical 55 s | Hovering the top-left Star Destroyer button replaces its icon with an opaque black square with cyan corner brackets | The hover highlight drawn over the icon |
| D12 | S3 | galactic 0–17 s | Selected planet name (KASHYYYK) in a blue outlined font, set low enough to touch the Day progress bar | Inside the top strip in the panel's font |
| D13 | S2 | tactical 40–77 s | No hit sparks, shield flashes or explosions seen under heavy fire; laser streams end in the wreck field rather than on visible enemies (may tie to R1) | Impact effects on hit |

### To verify against retail FoC (low confidence)
- **V1:** "Enemy losses" mixes Rebel units (X-wing, Y-wing, Falcon, Nebulon-B) and Underworld units (StarViper, Skipray, Marauder) in one battle. FoC battles are two-sided, so check the defender's composition and how losses are attributed.
- **V2:** Fleet in transit is drawn as three flat blue wedges (30–32 s). EaW does use faction-coloured arrow markers, so this may be intended art drawn without its texture or lighting.
- **V3:** Small flat green / blue zig-zag shapes with no unit attached (37–71 s, `crops_b\chev37.png`). They could be fighter LOD, projectiles or markers.
- **V4:** Fogged planets drawn as opaque, hard-edged wireframe globes (Yavin 4, Mon Calamari, Kamino, Shola). Retail may draw them translucent with a glow.
- **V5:** With tactical FOV 110 the planet backdrop carries a dark halo that cuts into the nebula (103–110 s).
- **V6:** From Camera and Controls the next frame is the pause menu, skipping Options. This may just be two clicks within the 1 s sampling gap.

### Checked and fine
- Credits Day 1 → 2: 13,667 + 668 = 14,335.
- Units freeze while paused.
- The fleet total stays at 10 during organiser drags.
- Felucia stays un-owned after the space win, because the ground battle is still needed.
- Dialogs are centred.

## Evidence
Frames and crops (scratch copies from this review):
`C:\Users\Gabriel\AppData\Local\Temp\claude\E--Projects-Star-wars-empire-at-war-port\5a74d5e3-9568-4d91-9122-dc67741e73ce\scratchpad\f\` (every 5 s) and `...\scratchpad\c\` (lasers, tooltip, beam, lefttri, victory, squads, planetgrid).

## References
- [Xbox Accessibility Guideline 101: Text display](https://devdocs.xbox.com/build/game-principles/accessibility/xag-deep-dives/xag-101-text-display.md), [102: Contrast](https://devdocs.xbox.com/build/game-principles/accessibility/xag-deep-dives/xag-102-contrast.md), [handheld text-size testcases](https://devdocs.xbox.com/build/gdk-and-engines/handheld/handheld-guidelines-and-testcases)
- [VkPresentModeKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html), [Samsung: configuring the Vulkan swapchain](https://developer.samsung.com/sdp/blog/en/2019/07/26/vulkan-mobile-best-practice-how-to-configure-your-vulkan-swapchain)
- [Gridly: localization best practices for game UI](https://gridly.com/blog/game-ui-design-localization-best-practices), [Bugnet: fixing localized text overflow](https://bugnet.io/blog/how-to-fix-localized-text-overflowing-ui)
- Fixed timestep + render interpolation ("Fix Your Timestep", discussed at [gamedev.net](https://gamedev.net/forums/topic/691776-fix-your-timestep))
