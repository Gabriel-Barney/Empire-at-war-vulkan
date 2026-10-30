// DynamicTrackFX.xml / ShadowBlobMaterials.xml readers (DECORATIONS.md 3.1, 5.2). They use the
// tolerant XML DOM of alamo_data when that module is configured (read-only use); otherwise they
// report that the module is missing.
#include <cmath>
#include <string_view>

#include "alamo/terrain/decorations.hpp"

#if defined(ALAMO_TERRAIN_HAVE_XML)
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#endif

namespace alamo::terrain {

#if defined(ALAMO_TERRAIN_HAVE_XML)
namespace {

data::XmlDocument parse_doc(std::span<const uint8_t> xml, const char* file, std::string* error) {
    data::XmlDocument doc =
        data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()), file);
    if (!doc.ok() && error) *error = std::string(file) + ": no XML root element";
    return doc;
}

void read_float(const data::XmlNode& n, std::string_view tag, float& v) {
    const data::XmlNode c = n.child(tag);
    if (!c) return;
    const auto f = data::parse_float(c.text_trimmed());  // atof-style: "20.0f" -> 20
    if (f && std::isfinite(*f)) v = *f;
}

}  // namespace

bool parse_shadow_blob_materials(std::span<const uint8_t> xml, std::vector<ShadowBlobMaterial>& inout, std::string* error) {
    const data::XmlDocument doc = parse_doc(xml, "ShadowBlobMaterials.xml", error);
    if (!doc.ok()) return false;
    for (const data::XmlNode m : doc.root().children("Material")) {
        ShadowBlobMaterial s;
        s.name = std::string(data::trim(m.attribute("name")));
        s.texture = std::string(m.child_text("Texture_Name"));
        if (s.texture.empty()) s.texture = "w_shadow_blob.tga";  // documented default (file header)
        s.texture2 = std::string(m.child_text("Texture_Name_2"));
        s.shader = std::string(m.child_text("Shader_Name"));
        if (const data::XmlNode rm = m.child("Render_Mode")) s.mode = render_mode_from_string(rm.text_trimmed());
        inout.push_back(std::move(s));
    }
    return true;
}

bool parse_dynamic_track_fx(std::span<const uint8_t> xml, std::vector<DynamicTrackParams>& inout, std::string* error) {
    const data::XmlDocument doc = parse_doc(xml, "DynamicTrackFX.xml", error);
    if (!doc.ok()) return false;
    for (const data::XmlNode n : doc.root().children("DynamicTrack")) {
        DynamicTrackParams p;
        p.name = std::string(data::trim(n.attribute("name")));
        p.texture = std::string(n.child_text("texture_name"));
        read_float(n, "opacity", p.opacity);
        read_float(n, "width", p.width);
        read_float(n, "segment_length", p.segment_length);
        read_float(n, "fade_begin_distance", p.fade_begin_distance);
        read_float(n, "fade_end_distance", p.fade_end_distance);
        read_float(n, "fade_distance_per_second", p.fade_distance_per_second);
        read_float(n, "min_geometry_lod", p.min_geometry_lod);
        if (const data::XmlNode rm = n.child("Render_Mode")) p.mode = render_mode_from_string(rm.text_trimmed());
        inout.push_back(std::move(p));
    }
    return true;
}

#else

bool parse_shadow_blob_materials(std::span<const uint8_t>, std::vector<ShadowBlobMaterial>&, std::string* error) {
    if (error) *error = "alamo_terrain built without alamo_data (no XML reader)";
    return false;
}
bool parse_dynamic_track_fx(std::span<const uint8_t>, std::vector<DynamicTrackParams>&, std::string* error) {
    if (error) *error = "alamo_terrain built without alamo_data (no XML reader)";
    return false;
}

#endif

}  // namespace alamo::terrain
