// R3 animation events (docs/re/AUDIO.md §5 / §6.2: Data\XML\AnimationSFXMaps.txt, AnimSFXMapper 0x4770a0).
// Each line: `SOUND <clip> <time ms> <SFX event>` or `SURFACE <clip> <time ms> <SurfaceFX> <bone>` (tab / space
// separated; `;`, `#`, `//` comment lines). <clip> is the ALA file stem (e.g. ei_trooper_move_00). SURFACE rows are
// the foot plants (INFANTRY_FOOT_PLANT on B_Foot_L / B_Foot_R), body impacts of death clips, etc.
//
// Firing rule [port] (AUDIO.md tags the timing [M]): an event fires when a slot's clip time
// (frame / fps x 1000 ms) passes its time during one sim advance, half-open [before, after); a loop wrap fires
// [before, end) then [0, after) with end = (count - 1) / fps; a clip started this tick starts at its start frame
// (frame 0 fires its 0 ms events on the first advance). Events never feed back into the sim (visual / audio
// output only), so they are not part of any CRC.
// Slots [port] (the RE docs say nothing about overlay events; ANIM_PARTICLES.md §A says the original has no event
// track at all, AUDIO.md §5 only gives the file): the primary slot AND the overlay slot (Attack_Animation_Is_Overlay
// clips, AnimWorld::play_overlay) fire, with the same window rule; the previous (fading-out) slot fires nothing.
// Order per object: the primary slot's events first, then the overlay's, each in time order (a wrap: tail, then the
// new cycle). A slot cleared or given another clip between the mark and the query fires nothing for that advance; a
// timed or one-shot slot that finishes during the advance fires up to the frame where it stopped. The per-slot
// bookkeeping is AnimWorld::mark_events / crossed_since / fired_events (anim_state.hpp).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::anim {

struct AnimEventDef {
    enum class Kind : uint8_t { Sound = 0, Surface = 1 };
    Kind kind = Kind::Surface;
    uint32_t time_ms = 0;
    std::string name;  // SFX event / SurfaceFX name
    std::string bone;  // SURFACE: bone name ("" for SOUND)
};

class AnimEventMap {
public:
    // Parses AnimationSFXMaps.txt text; appends (file, line) diagnostics for malformed lines. Returns rows added.
    size_t parse(std::string_view text, std::vector<std::string>* diagnostics = nullptr);
    // Events of a clip, by ALA stem or full path (case-insensitive, directory and .ALA stripped); time order.
    const std::vector<AnimEventDef>* find(std::string_view clip) const;
    size_t size() const { return rows_; }
    size_t clips() const { return by_clip_.size(); }

private:
    std::map<std::string, std::vector<AnimEventDef>> by_clip_;  // upper-case stem (lookup only)
    size_t rows_ = 0;
};

// Upper-case ALA stem of a clip path ("DATA\ART\MODELS\EI_TROOPER_MOVE_00.ALA" -> "EI_TROOPER_MOVE_00").
std::string clip_stem(std::string_view path);

// Indices into `events` crossed by an advance of a clip of `count` frames at `fps` from frame `before` to frame
// `after` (rule above). `wrapped` = the loop wrapped during the advance.
void crossed_events(const std::vector<AnimEventDef>& events, float fps, uint32_t count, float before, float after, bool wrapped,
                    std::vector<uint32_t>& out);

}  // namespace alamo::anim
