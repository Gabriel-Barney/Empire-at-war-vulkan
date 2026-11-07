#include "alamo/ui/hud_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

#include "alamo/core/ascii.hpp"
#include "alamo/core/log.hpp"
#include "alamo/ui/hud_canvas.hpp"

#include <windows.h>

namespace alamo::ui {

namespace {

// ---- sfnt (TrueType / OpenType) parsing ----

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           p[3];
}
bool tag_is(const uint8_t* p, const char* t) { return std::equal(p, p + 4, reinterpret_cast<const uint8_t*>(t)); }

void append_utf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (c >> 6)));
        s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xE0 | (c >> 12)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// Name table: per name id the best record (Windows Unicode US English > Windows Unicode > Mac Roman).
void read_names(const uint8_t* t, size_t len, TrueTypeFontInfo& out) {
    if (len < 6) return;
    const uint16_t count = be16(t + 2), str = be16(t + 4);
    int best[7] = {0, 0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t r = 6 + static_cast<size_t>(i) * 12;
        if (r + 12 > len) break;
        const uint16_t pid = be16(t + r), eid = be16(t + r + 2), lid = be16(t + r + 4), nid = be16(t + r + 6);
        const uint16_t sl = be16(t + r + 8), so = be16(t + r + 10);
        if (nid != 1 && nid != 4 && nid != 6) continue;
        int score = 0;
        if (pid == 3 && (eid == 1 || eid == 0)) score = lid == 0x409 ? 3 : 2;
        else if (pid == 1 && eid == 0) score = 1;
        if (score <= best[nid]) continue;
        const size_t s = static_cast<size_t>(str) + so;
        if (s + sl > len) continue;
        std::string v;
        if (pid == 3) {
            for (size_t k = 0; k + 1 < sl; k += 2) append_utf8(v, be16(t + s + k));
        } else {
            for (size_t k = 0; k < sl; ++k) append_utf8(v, t[s + k]);
        }
        best[nid] = score;
        (nid == 1 ? out.family : nid == 4 ? out.full_name : out.postscript_name) = std::move(v);
    }
}

bool parse_sfnt(const uint8_t* b, size_t avail, TrueTypeFontInfo& out) {
    if (avail < 12) return false;
    const uint32_t ver = be32(b);
    if (ver != 0x00010000u && ver != 0x4F54544Fu) return false;  // TrueType / 'OTTO'
    const uint16_t n = be16(b + 4);
    if (n < 4 || n > 64) return false;
    uint32_t pow2 = 1, lg = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++lg;
    }
    const uint32_t search_range = be16(b + 6), entry_selector = be16(b + 8), range_shift = be16(b + 10);
    if (search_range != pow2 * 16 || entry_selector != lg || range_shift != uint32_t{n} * 16 - pow2 * 16) return false;
    const size_t dir_end = 12 + static_cast<size_t>(n) * 16;
    if (dir_end > avail) return false;
    size_t end = dir_end;
    const uint8_t *head = nullptr, *name = nullptr, *os2 = nullptr;
    size_t head_len = 0, name_len = 0, os2_len = 0;
    bool cmap = false, glyf = false, loca = false, cff = false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* r = b + 12 + i * 16;
        for (int k = 0; k < 4; ++k)
            if (r[k] < 32 || r[k] > 126) return false;
        const size_t off = be32(r + 8), len = be32(r + 12);
        if (off < dir_end || off > avail || len > avail - off) return false;
        end = std::max(end, off + len);
        if (tag_is(r, "head")) {
            head = b + off;
            head_len = len;
        } else if (tag_is(r, "name")) {
            name = b + off;
            name_len = len;
        } else if (tag_is(r, "OS/2")) {
            os2 = b + off;
            os2_len = len;
        } else if (tag_is(r, "cmap")) cmap = true;
        else if (tag_is(r, "glyf")) glyf = true;
        else if (tag_is(r, "loca")) loca = true;
        else if (tag_is(r, "CFF ")) cff = true;
    }
    if (!head || head_len < 54 || be32(head + 12) != 0x5F0F3CF5u || !name || !cmap || !((glyf && loca) || cff)) return false;
    out = {};
    out.size = std::min(avail, (end + 3) & ~size_t{3});
    read_names(name, name_len, out);
    if (os2 && os2_len >= 6) out.weight = be16(os2 + 4);
    return !out.family.empty() || !out.full_name.empty();
}

// ASCII case-insensitive, but an empty name never matches (not even another empty one).
bool iequal(std::string_view a, std::string_view b) { return !a.empty() && ascii_iequals(a, b); }

struct FontRegistry {
    std::mutex m;
    std::vector<GameFont> fonts;
};
FontRegistry& font_registry() {
    static FontRegistry r;
    return r;
}

class BoxRasterizer final : public GlyphRasterizer {
public:
    int32_t create_font(const FontRequest& req, FontMetrics& out) override {
        out.pixel_height = std::max(1, req.pixel_height);
        out.ascent = (out.pixel_height * 4 + 2) / 5;
        out.descent = out.pixel_height - out.ascent;
        out.line_height = out.pixel_height;
        out.actual_face = req.face;
        heights_.push_back(out.pixel_height);
        stretch_.push_back(req.stretch > 0.0f ? req.stretch : 1.0f);
        return static_cast<int32_t>(heights_.size() - 1);
    }
    bool rasterize(int32_t font, char32_t c, GlyphBitmap& out) override {
        if (font < 0 || static_cast<size_t>(font) >= heights_.size()) return false;
        const int32_t h = heights_[static_cast<size_t>(font)];
        const float s = stretch_[static_cast<size_t>(font)];
        out = {};
        // Stretch_Factor scales the glyph height only (GALACTIC_DIALOGS.md §6); widths / advances stay unstretched.
        out.advance = std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(h) * 0.6f)));
        if (c == U' ' || c == U'\t') return true;
        out.width = std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(h) * 0.5f)));
        out.height = std::max(1, static_cast<int32_t>(std::lround(std::floor(static_cast<float>(h) * s + 0.5f) * 0.7f)));
        out.origin_x = 0;
        out.origin_y = out.height;
        out.coverage.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 255);
        return true;
    }
    const char* name() const override { return "box"; }

private:
    std::vector<int32_t> heights_;
    std::vector<float> stretch_;
};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// GDI: CreateFontW (negative em height, weight 400/700, ANTIALIASED_QUALITY) and GGO_GRAY8_BITMAP glyphs
// (65 coverage levels, scaled to 0..255).
class GdiRasterizer final : public GlyphRasterizer {
public:
    GdiRasterizer() { dc_ = CreateCompatibleDC(nullptr); }
    ~GdiRasterizer() override {
        for (HFONT f : fonts_)
            if (f) DeleteObject(f);
        if (dc_) DeleteDC(dc_);
    }
    GdiRasterizer(const GdiRasterizer&) = delete;
    GdiRasterizer& operator=(const GdiRasterizer&) = delete;

    int32_t create_font(const FontRequest& req, FontMetrics& out) override {
        if (!dc_) return -1;
        const int h0 = std::max(1, req.pixel_height);
        // A registered game font is opened by its family name at its own weight ("EmpireAtWar-Bold" is the
        // weight-600 family "Empire At War Bold"; asking FW_BOLD would embolden it a second time).
        GameFont game;
        const bool is_game = find_game_font(req.face, &game) && !game.family.empty();
        const std::wstring face = widen(is_game ? game.family : req.face);
        const int weight = is_game ? static_cast<int>(game.weight) : (req.bold ? FW_BOLD : FW_NORMAL);
        auto make = [&](int height, int width) {
            return CreateFontW(-height, width, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
        };
        HFONT f = make(h0, 0);
        if (!f) return -1;
        // Stretch_Factor (GALACTIC_DIALOGS.md §6): outside 0.9999..1.0001 the font is re-created at height
        // floor(h x s + 0.5) with the width = the unstretched font's tmAveCharWidth, i.e. only the height changes.
        int h = h0;
        if (req.stretch > 0.0f && (req.stretch < 0.9999f || req.stretch > 1.0001f)) {
            SelectObject(dc_, f);
            TEXTMETRICW tm0{};
            GetTextMetricsW(dc_, &tm0);
            h = std::max(1, static_cast<int>(std::floor(static_cast<float>(h0) * req.stretch + 0.5f)));
            if (HFONT g = make(h, std::max(1, static_cast<int>(tm0.tmAveCharWidth)))) {
                SelectObject(dc_, GetStockObject(SYSTEM_FONT));
                DeleteObject(f);
                f = g;
            } else {
                h = h0;
            }
        }
        SelectObject(dc_, f);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc_, &tm);
        wchar_t actual[LF_FACESIZE] = {};
        GetTextFaceW(dc_, LF_FACESIZE, actual);
        out.pixel_height = h0;  // the requested em height (the stretched font's own metrics follow)
        out.ascent = tm.tmAscent;
        out.descent = tm.tmDescent;
        out.line_height = tm.tmHeight;
        out.actual_face = narrow(actual);
        if (is_game && iequal(out.actual_face, game.family)) out.actual_face = req.face;  // no substitution
        fonts_.push_back(f);
        return static_cast<int32_t>(fonts_.size() - 1);
    }

    bool rasterize(int32_t font, char32_t c, GlyphBitmap& out) override {
        if (font < 0 || static_cast<size_t>(font) >= fonts_.size()) return false;
        SelectObject(dc_, fonts_[static_cast<size_t>(font)]);
        const UINT ch = c <= 0xFFFF ? static_cast<UINT>(c) : static_cast<UINT>(u'?');
        GLYPHMETRICS gm{};
        MAT2 m{};
        m.eM11.value = 1;
        m.eM22.value = 1;
        out = {};
        const DWORD size = GetGlyphOutlineW(dc_, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        if (size == GDI_ERROR) return false;
        out.advance = gm.gmCellIncX;
        if (size == 0) return true;  // blank glyph (space)
        std::vector<uint8_t> buf(size);
        if (GetGlyphOutlineW(dc_, ch, GGO_GRAY8_BITMAP, &gm, size, buf.data(), &m) == GDI_ERROR) return false;
        out.width = static_cast<int32_t>(gm.gmBlackBoxX);
        out.height = static_cast<int32_t>(gm.gmBlackBoxY);
        out.origin_x = gm.gmptGlyphOrigin.x;
        out.origin_y = gm.gmptGlyphOrigin.y;
        const size_t pitch = (static_cast<size_t>(out.width) + 3u) & ~size_t{3};
        out.coverage.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0);
        for (int32_t y = 0; y < out.height; ++y)
            for (int32_t x = 0; x < out.width; ++x) {
                const size_t s = static_cast<size_t>(y) * pitch + static_cast<size_t>(x);
                const uint32_t v = s < buf.size() ? std::min<uint32_t>(buf[s], 64u) : 0u;
                out.coverage[static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)] =
                    static_cast<uint8_t>((v * 255u + 32u) / 64u);
            }
        return true;
    }
    const char* name() const override { return "gdi"; }

private:
    HDC dc_ = nullptr;
    std::vector<HFONT> fonts_;
};

}  // namespace

std::unique_ptr<GlyphRasterizer> make_box_rasterizer() { return std::make_unique<BoxRasterizer>(); }

std::unique_ptr<GlyphRasterizer> make_system_rasterizer() {
    return std::make_unique<GdiRasterizer>();
}

std::vector<TrueTypeFontInfo> find_truetype_fonts(const uint8_t* bytes, size_t size) {
    std::vector<TrueTypeFontInfo> out;
    if (!bytes) return out;
    for (size_t i = 0; i + 12 <= size;) {
        const uint8_t* p = bytes + i;
        const bool cand = (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) || tag_is(p, "OTTO");
        TrueTypeFontInfo f;
        if (cand && parse_sfnt(p, size - i, f)) {
            f.offset = i;
            i += std::max<size_t>(f.size, 12);
            out.push_back(std::move(f));
        } else {
            ++i;
        }
    }
    return out;
}

namespace {

bool read_host_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool font_file_ext(const std::filesystem::path& p) {
    const std::string e = ascii_upper(narrow(p.extension().wstring().c_str()));
    return e == ".TTF" || e == ".OTF";
}

// Registers every font of `bytes` not registered yet (by full name); appends them to `added`.
void register_blob(const std::vector<uint8_t>& bytes, const std::string& source, bool embedded, std::vector<GameFont>& added) {
    FontRegistry& reg = font_registry();
    for (const TrueTypeFontInfo& f : find_truetype_fonts(bytes.data(), bytes.size())) {
        const std::string& key = f.full_name.empty() ? f.family : f.full_name;
        std::lock_guard<std::mutex> lock(reg.m);
        const bool known = std::any_of(reg.fonts.begin(), reg.fonts.end(), [&](const GameFont& g) {
            return iequal(g.full_name.empty() ? g.family : g.full_name, key);
        });
        if (known) continue;
        DWORD count = 0;
        // AddFontMemResourceEx copies the data; the font is private to this process (no install, no registry).
        HANDLE h = AddFontMemResourceEx(const_cast<uint8_t*>(bytes.data() + f.offset), static_cast<DWORD>(f.size), nullptr,
                                        &count);
        if (!h || count == 0) {
            log::warnf("ui", "font '%s' in %s could not be registered", key.c_str(), source.c_str());
            continue;
        }
        GameFont g;
        g.full_name = f.full_name;
        g.family = f.family;
        g.postscript_name = f.postscript_name;
        g.weight = f.weight;
        char at[32];
        std::snprintf(at, sizeof at, "@0x%zx", f.offset);
        g.source = embedded ? source + at : source;
        reg.fonts.push_back(g);
        added.push_back(std::move(g));
    }
}

}  // namespace

std::vector<GameFont> register_game_fonts(const std::vector<std::string>& game_roots) {
    std::vector<GameFont> added;
    namespace fs = std::filesystem;
    for (const std::string& root : game_roots) {
        if (root.empty()) continue;
        const fs::path r(widen(root));
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (const wchar_t* sub : {L"", L"Fonts", L"Data\\Fonts", L"Data\\Art\\Fonts"}) {
            const fs::path dir = *sub ? r / sub : r;
            if (!fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && font_file_ext(it->path())) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            std::vector<uint8_t> bytes;
            for (const fs::path& p : files)
                if (read_host_file(p, bytes)) register_blob(bytes, narrow(p.wstring().c_str()), false, added);
        }
        const fs::path exe = r / L"StarWarsG.exe";
        std::vector<uint8_t> bytes;
        if (fs::is_regular_file(exe, ec) && read_host_file(exe, bytes)) register_blob(bytes, narrow(exe.wstring().c_str()), true, added);
    }
    for (const GameFont& g : added) log::infof("ui", "game font '%s' (%s, weight %u) from %s", g.full_name.c_str(), g.family.c_str(),
                                                static_cast<unsigned>(g.weight), g.source.c_str());
    return added;
}

std::vector<GameFont> registered_game_fonts() {
    FontRegistry& reg = font_registry();
    std::lock_guard<std::mutex> lock(reg.m);
    return reg.fonts;
}

bool find_game_font(std::string_view face, GameFont* out) {
    if (face.empty()) return false;
    FontRegistry& reg = font_registry();
    std::lock_guard<std::mutex> lock(reg.m);
    for (const GameFont& g : reg.fonts)
        if (iequal(g.full_name, face) || iequal(g.postscript_name, face) || iequal(g.family, face)) {
            if (out) *out = g;
            return true;
        }
    return false;
}

bool face_is_bold(std::string_view face) { return ascii_upper(face).find("BOLD") != std::string::npos; }

int32_t FontCache::font(std::string_view face, float point_size, uint32_t screen_height, float stretch) {
    if (face.empty()) face = kFallbackFontFace;
    const int32_t px = font_pixel_height_int(point_size > 0.0f ? point_size : 8.0f, screen_height);
    const auto key = std::make_tuple(ascii_upper(face), px, static_cast<int32_t>(std::lround(stretch * 1000.0f)));
    const auto it = by_key_.find(key);
    if (it != by_key_.end()) return it->second;
    FontRequest req;
    req.face = std::string(face);
    req.pixel_height = px;
    req.bold = face_is_bold(face);
    req.stretch = stretch;
    Font f;
    f.raster_id = r_->create_font(req, f.metrics);
    if (f.raster_id < 0 && ascii_upper(face) != ascii_upper(kFallbackFontFace)) {
        req.face = kFallbackFontFace;
        req.bold = false;
        f.raster_id = r_->create_font(req, f.metrics);
    }
    if (f.raster_id < 0) return -1;
    if (!f.metrics.actual_face.empty() && ascii_upper(f.metrics.actual_face) != ascii_upper(req.face)) {
        ++substitutions_;
        if (substitutions_ <= 4)
            log::warnf("ui", "font '%s' substituted by '%s'", req.face.c_str(), f.metrics.actual_face.c_str());
    }
    fonts_.push_back(std::move(f));
    const auto id = static_cast<int32_t>(fonts_.size() - 1);
    by_key_.emplace(key, id);
    return id;
}

bool FontCache::pack(uint32_t w, uint32_t h, uint16_t& page, uint32_t& x, uint32_t& y) {
    if (w + 2 > page_size_ || h + 2 > page_size_) return false;
    const uint32_t pw = w + 1, ph = h + 1;  // 1-pixel gutter (bilinear safety)
    for (int pass = 0; pass < 2; ++pass) {
        if (pages_.empty() || pass == 1) {
            Page p;
            p.rgba.assign(static_cast<size_t>(page_size_) * page_size_ * 4, 0);
            for (size_t i = 0; i < p.rgba.size(); i += 4) p.rgba[i] = p.rgba[i + 1] = p.rgba[i + 2] = 255;
            p.shelf_y = 1;
            p.cursor_x = 1;
            pages_.push_back(std::move(p));
        }
        Page& p = pages_.back();
        if (p.cursor_x + pw > page_size_) {  // new shelf
            p.shelf_y += p.shelf_h;
            p.shelf_h = 0;
            p.cursor_x = 1;
        }
        if (p.shelf_y + ph > page_size_) continue;  // page full -> next pass makes a new one
        x = p.cursor_x;
        y = p.shelf_y;
        p.cursor_x += pw;
        p.shelf_h = std::max(p.shelf_h, ph);
        page = static_cast<uint16_t>(pages_.size() - 1);
        return true;
    }
    return false;
}

const FontCache::Glyph* FontCache::glyph(int32_t font, char32_t c) {
    if (font < 0 || static_cast<size_t>(font) >= fonts_.size()) return nullptr;
    Font& f = fonts_[static_cast<size_t>(font)];
    const auto it = f.glyphs.find(c);
    if (it != f.glyphs.end()) return &it->second;
    GlyphBitmap bm;
    if (!r_->rasterize(f.raster_id, c, bm)) {
        if (c == U'?') return nullptr;
        return glyph(font, U'?');
    }
    Glyph g;
    g.origin_x = bm.origin_x;
    g.origin_y = bm.origin_y;
    g.advance = bm.advance;
    if (bm.width > 0 && bm.height > 0) {
        const auto w = static_cast<uint32_t>(bm.width), h = static_cast<uint32_t>(bm.height);
        if (pack(w, h, g.page, g.x, g.y)) {
            Page& p = pages_[g.page];
            for (uint32_t yy = 0; yy < h; ++yy)
                for (uint32_t xx = 0; xx < w; ++xx)
                    p.rgba[((static_cast<size_t>(g.y + yy) * page_size_) + g.x + xx) * 4 + 3] =
                        bm.coverage[static_cast<size_t>(yy) * w + xx];
            p.generation = ++generation_;
            g.width = w;
            g.height = h;
        }
    }
    return &f.glyphs.emplace(c, g).first->second;
}

int32_t FontCache::measure(int32_t font, std::u32string_view text) {
    int32_t w = 0;
    for (char32_t c : text)
        if (const Glyph* g = glyph(font, c)) w += g->advance;
    return w;
}

void FontCache::clear() {
    fonts_.clear();
    by_key_.clear();
    pages_.clear();
    ++generation_;
}

}  // namespace alamo::ui
