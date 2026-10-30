# Fix-pass capture checklist (screenshots only, no mod files)

Spec: `docs/re/FIXPASS_GALACTIC.md` §4–5. Use the unmodified **FoC** (`corruption\StarWarsG.exe`). Do not use EaW: the
hero-bar sort key differs between the two. Take each screenshot with F12 (Steam) or PrtScn. Name the files `C<n>_<what>.jpg`
and drop them in this folder. Nothing needs installing or cleaning up.

## C1 Endor nebula over the planet (render)
1. Start a skirmish **space** battle on Endor (`_MP_SPACE_ENDOR`).
2. Pan and rotate the camera until one of the large blue ion nebulae overlaps the Endor planet disc.
3. Take one screenshot with the overlap, and one of the same planet without nebula overlap.

Expected: the nebula is added on top of the planet, which is brighter where they overlap. No stars from the background
show through the planet.

## C2 Tactical bottom-left planet / advisor area (ui)
1. Start a galactic campaign battle, space and land, so that the planet name shows.
2. Take a screenshot of the whole screen at the start of the battle.
3. Take another screenshot while the advisor is talking so the 200×200 talking head shows. (Done 2026-10-30: Numpad −
   opens the Droid Log instead; the head plays in story missions, so use Tutorial 1 and Tutorial 3.)
4. Take another screenshot when it is silent.
5. Note your screen resolution.

## C3 Hero bar sort order (ui)
1. Start an Empire galactic conquest that has Emperor, Boba Fett, Piett (Accuser), Tarkin and Veers alive. The
   default Empire GC start usually has most of them.
2. Take one screenshot of the top-right hero bar.
3. Write down, right to left, which portrait is where.

Expected FoC order, right to left: **Emperor, Veers, Boba Fett, Piett, Tarkin**. If you see Emperor, Boba, Piett, Tarkin,
Veers instead, say so: that is the EaW order.

## C4 Trade-route colour after a capture (render)
1. In an Underworld GC, zoom fully **out**.
2. Note the colour of a route between two Underworld planets (bright yellow expected).
3. Without touching the zoom, conquer a planet so that a route now has Underworld at both ends.
4. Take a screenshot of that route right after the capture: dim olive or bright yellow?
5. Zoom in and out once and take another screenshot.

The code suggests it shows the dim "zoomed-in" colour until the zoom changes.

## C5 Corruption path lines fading (render)
1. As Rebel or Empire, use a hero's Remove Corruption on a corrupted planet that has corrupted neighbours.
2. Take a screenshot at the start, about halfway, and just before it finishes, showing the thin dark-yellow lines
   between the planets.

Expected: the lines from that planet fade to nothing. Optional: as Underworld, capture an enemy planet and screenshot
it during the following 10 s. Expected: no new lines until about 10 s, then they appear at full colour.

## C6 Hero portrait clicks on the galaxy map (ui)
1. Zoomed out, **single-click** a hero portrait whose hero sits in a fleet at a planet.

   Expected: the camera pans to the planet and the planet is selected, but the planet does not open.
2. **Double-click** the same portrait.

   Expected: the camera zooms into the planet. Double-click again: it zooms out.
3. **Right-click** the portrait.

   Expected: nothing.
4. Click the portrait of a hero that is in hyperspace, or one that is still being built.

   Expected: nothing.

Report each item as matching or not matching, with the screenshot.
