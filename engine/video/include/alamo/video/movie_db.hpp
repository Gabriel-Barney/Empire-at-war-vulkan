// Movies.xml (docs/re/STRATEGIC_GAPS.md §7.3 [H]: Movie_File, Commandbar_Offset, Alpha, Cannot_Skip, Loop) for the
// movie players: which movies have an alpha plane (decoded with OpenOptions::alpha, drawn with straight alpha on
// PrimAlpha - ui::add_movie_quad(..., alpha = true)) and which are opaque (PrimOpaque). Built only with engine/data.
#pragma once
#include <string_view>
#include <vector>

#include "alamo/video/video.hpp"

namespace alamo::data {
class XmlNode;
}

namespace alamo::video {

// Every <Movie Name="..."> under the <Movies> root, in file order (a later entry of the same name is kept too;
// find_movie returns the last one). Missing fields keep the MovieEntry defaults.
std::vector<MovieEntry> parse_movies(const data::XmlNode& movies_root);
// By Name (ASCII case-insensitive), else by the Movie_File's base name without extension (the host opens movies by
// file name, e.g. "pirate_loop2"); nullptr if none.
const MovieEntry* find_movie(const std::vector<MovieEntry>& movies, std::string_view name_or_file);

}  // namespace alamo::video
