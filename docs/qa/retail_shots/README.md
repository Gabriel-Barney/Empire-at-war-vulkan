# Retail screenshot attempt, 2026-11-01 (pt4qa t32 / t35 / t51)

Result: **no verdict for any of the three tasks. No retail screenshot was saved.**
Nothing here confirms or contradicts the port.

## What blocked it
1. **Another Claude session was driving the same game and desktop.** At 05:09-05:10 the process list showed a python
   `g1.py dxvk --non-interactive --start-steam --drive KS ... --probe-mod admon` from worktree `recursing-robinson-a93c78`
   starting and stopping `StarWarsG.exe` (windowed, MODPATH to its own probe mod). My game (PID 21408, no mod) was gone
   the moment I pressed "Start Game" for the Felucia skirmish, and a second `StarWarsG.exe` with a foreign MODPATH appeared
   and disappeared within seconds. I did not touch that session's game. The "only agent on the desktop" assumption did not hold.
2. **The keyboard tools were unusable for the whole run.** Every `key` / `wait` call returned "aborted (user interrupt)".
   Early Escape presses were still delivered late and kept re-opening the in-game main menu, so I worked mouse-only
   (the GC "Main Menu" button at the lower left replaces Esc).
3. **Could not issue a fleet move order in Galactic Conquest** (see t32).

## t32 (fleet in transit, dart tip direction): not reached
Galactic Conquest, map "Origin of Corruption", Zann Consortium, start planet Hypori (fleet pads: Fleet 1 = 13 pop, Fleet 2 = 1 ship, Fleet 3 = hero).
Selecting a fleet works (pad glows, hover tooltip "Zann Consortium Fleet"), but about ten right-click orders to the neighbour
Bothawui (and left-click variants) never started a move: no move cursor, no departure. Not established whether the
right-click is not delivered by the input tool or whether the order is refused (an enemy fleet attack on Hypori was
pending the whole time). Enemy fleets arrived at Hypori without a transit marker I could catch. Then both my fleets died in
two battles. **Never saw a fleet marker in flight, so nothing about dart direction.**

## t35 (Felucia space battle, nebula and star-dome gap): not reached
Skirmish > Space > "(6) High Point Felucia" was selected and Start Game pressed. The game process disappeared at that
moment (item 1). Caveat for a later retry: this is a skirmish map; the port's case is the campaign map
`_Space_Planet_Felucia_01.ted`, so it may not be the same backdrop. Not looked at in retail.

## t51 (ALT1 planets, Kuat ring band, Nal Hutta rings): not reached
In "Origin of Corruption" Nal Hutta is a blue wireframe globe (not scouted) and a double-click on it selects nothing, so
no close-up is possible without scouting it; Kuat was not on the screens I reached. Retail planet close-up (double-click a
visible planet, e.g. Hypori or Bothawui) works and zooms to a planet view, but I did not look at Kuat or Nal Hutta.
A retry should start as Empire on a map that owns Kuat, and find a map where Nal Hutta is visible (or scout it with a fleet).

## Useful notes for a retry
- Run it only when no other probe is running (check for `g1.py` / `StarWarsG.exe` first).
- The game window needs about 1-3 s per UI reaction; after "Resume Game" wait for the menu fade before clicking.
- GC "Main Menu" (lower-left button) > Quit Game > Yes returns to the title screen without the keyboard.
- Retail GC at Hypori: lost fleets, space battle "Begin" opens a load screen then the battle (Hypori map has a
  yellow-brown nebula band at the top, not Felucia).
