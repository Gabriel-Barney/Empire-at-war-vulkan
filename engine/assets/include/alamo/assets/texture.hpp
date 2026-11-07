// alamo_assets - DDS / TGA texture loading into upload-ready mip levels.
//
// Supported DDS: DXT1 (BC1), DXT2/DXT3 (BC2), DXT4/DXT5 (BC3), uncompressed 32-bit BGRA/BGRX/RGBA,
// 24-bit RGB, 16-bit 565/1555/4444/X1555/X4444 (expanded to BGRA8), L8, A8, A8L8 (kept as R8 / R8G8
// with a component swizzle), legacy D3DFMT FourCC codes for those, DX10 headers for the same
// formats, full mip chains (truncated chains are cut to the levels present, with a warning), and
// cubemaps (6 faces; partial cubemaps are rejected). Volume textures are rejected.
// Supported TGA: types 2/10 (true colour 15/16/24/32-bit, RLE or not) -> BGRA8, types 3/11
// (8-bit grey) -> R8 + luminance swizzle; bottom-up/right-to-left origins are flipped to top-left.
// Supported BMP: BITMAPCOREHEADER and BITMAPINFOHEADER..V5, uncompressed 16/24/32-bit BI_RGB and
// 16/32-bit BI_BITFIELDS / BI_ALPHABITFIELDS, bottom-up or top-down -> BGRA8. A 32-bit BI_RGB file
// keeps its fourth byte as alpha unless that byte is 0 everywhere (then opaque). Palettized and
// RLE BMPs are rejected.
// Mips: a TGA or BMP, and an uncompressed DDS that carries a single level, get a full chain built by
// generate_mips() (the original D3D9 path left this to D3DX / driver auto-gen; see there).
// Block-compressed DDS files keep exactly the levels they ship.
// 32-bit TGA is always A8R8G8B8 (alpha kept even when the descriptor declares 0 alpha bits, as
// D3DX does; shipped MT_COMMANDBAR.TGA relies on that).
// Memory budget: a texture whose decoded data (all layers and mips, including generated ones)
// would exceed kMaxDecodedTextureBytes is rejected before anything is allocated.
//
// Colour space: the original is a D3D9 renderer without sRGB reads, so everything is UNORM.
//
// Thread safety: all functions are pure; safe to call concurrently.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}

namespace alamo::assets {

// Per-texture decoded-size budget (bytes, all layers and mips). The largest shipped texture is far
// below it (see test_assets' real-data scan); 256 MiB still admits e.g. a 4096^2 RGBA8 2D texture
// with mips (~85 MiB) or a 2048^2 RGBA8 cubemap with mips (~128 MiB).
inline constexpr std::uint64_t kMaxDecodedTextureBytes = std::uint64_t(256) << 20;

// TextureFormat -> VkFormat (Vulkan 1.3 core names; no vulkan headers included here):
//   BC1_RGBA   -> VK_FORMAT_BC1_RGBA_UNORM_BLOCK   (DXT1, 8 bytes / 4x4 block, 1-bit alpha kept)
//   BC2        -> VK_FORMAT_BC2_UNORM_BLOCK        (DXT2/DXT3, 16 bytes / block)
//   BC3        -> VK_FORMAT_BC3_UNORM_BLOCK        (DXT4/DXT5, 16 bytes / block)
//   RGBA8      -> VK_FORMAT_R8G8B8A8_UNORM
//   BGRA8      -> VK_FORMAT_B8G8R8A8_UNORM         (mandatory sampled format in Vulkan)
//   R8         -> VK_FORMAT_R8_UNORM               (L8, A8; see swizzle)
//   R8G8       -> VK_FORMAT_R8G8_UNORM             (A8L8: r = L, g = A; see swizzle)
// DXT2/DXT4 (premultiplied) are uploaded as BC2/BC3; `premultiplied` is set.
enum class TextureFormat : std::uint8_t { Unknown = 0, BC1_RGBA, BC2, BC3, RGBA8, BGRA8, R8, R8G8 };

// Swizzle -> VkComponentSwizzle: R/G/B/A -> VK_COMPONENT_SWIZZLE_R/G/B/A, Zero -> _ZERO,
// One -> _ONE. Identity for colour formats; L8 = (R,R,R,One), A8 = (Zero,Zero,Zero,R),
// A8L8 = (R,R,R,G).
enum class Swizzle : std::uint8_t { R, G, B, A, Zero, One };
struct ComponentMapping {
    Swizzle r = Swizzle::R, g = Swizzle::G, b = Swizzle::B, a = Swizzle::A;
};

const char* texture_format_name(TextureFormat f);
const char* texture_format_vk_name(TextureFormat f);  // e.g. "VK_FORMAT_BC3_UNORM_BLOCK"
// Bytes of one tightly packed mip level.
std::uint64_t texture_level_size(TextureFormat f, std::uint32_t width, std::uint32_t height);

struct TextureLevel {
    std::uint32_t layer = 0;   // array layer / cube face (+X,-X,+Y,-Y,+Z,-Z = Vulkan layer order)
    std::uint32_t mip = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t offset = 0;  // into TextureAsset::data
    std::uint64_t size = 0;    // tightly packed (bufferRowLength = 0 in VkBufferImageCopy)
};

struct TextureAsset {
    std::string name;  // resolved path, or "<fallback>"
    TextureFormat format = TextureFormat::Unknown;
    ComponentMapping swizzle;
    std::uint32_t width = 0, height = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t layers = 1;  // 6 for a cubemap
    bool cubemap = false;
    bool premultiplied = false;
    bool is_fallback = false;
    std::vector<std::uint8_t> data;     // owns every level's bytes
    std::vector<TextureLevel> levels;   // layer-major: layer 0 mips 0..n-1, layer 1 ...
    std::vector<std::string> warnings;

    const TextureLevel* level(std::uint32_t layer, std::uint32_t mip) const;
    std::span<const std::uint8_t> level_bytes(const TextureLevel& l) const {
        return std::span<const std::uint8_t>(data).subspan(static_cast<std::size_t>(l.offset),
                                                            static_cast<std::size_t>(l.size));
    }
};

// Decoders take ownership of the file bytes (DDS data in a native format is kept in place, no copy).
bool decode_dds(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error = nullptr);
bool decode_tga(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error = nullptr);
bool decode_bmp(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error = nullptr);
// The format comes from the file's content, never its extension (the original loads image files by
// content: Thrawn's Revenge ships a BMP named MT_Commandbar.tga): "DDS " -> DDS, "BM" -> BMP, else
// TGA (which has no magic; a TGA's second byte, the colour-map type, is 0 or 1, never 'M').
enum class TextureFileKind : std::uint8_t { Dds, Bmp, Tga };
TextureFileKind sniff_texture_file(std::span<const std::uint8_t> bytes);
bool decode_texture(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error = nullptr);

// Box-filter mip chain for an uncompressed texture (RGBA8 / BGRA8 / R8 / R8G8) that has exactly
// one level per layer: levels down to 1x1, each texel of level k+1 the rounded integer mean
// ((sum + n/2) / n, per channel, alpha not weighted) of the 2x2 texels of level k it covers;
// a 1-wide / 1-high level averages 2 texels, and an odd dimension drops its last row/column
// (dst = max(1, src >> 1)). Integer-only, so bit-identical everywhere. This approximates D3D9
// D3DX / D3DUSAGE_AUTOGENMIPMAP box filtering; the original's exact filter is not recovered.
// Returns false (texture untouched) for block-compressed formats or when mips already exist.
bool generate_mips(TextureAsset& t);

// Texture lookup (docs/re/SPACE_AND_OPEN.md B.2, alGraphicsDriver::Load_Texture). On the
// normalised name (alamo::normalize_path: upper-case, '\\' separators; VFS reads are
// case-insensitive):
//   AsNamed       : the name as written (mod paths, loose files, MEGs - whatever the VFS resolves)
//   TextureDir    : "Data\Art\Textures\" + the part after the last '\'
//   DdsAsNamed    : the name cut at its first '.' (or whole if none) + ".DDS"
//   DdsTextureDir : "Data\Art\Textures\" + the part after the last '\' of that .DDS name
// So the named extension is tried first and .dds second; no other extension is tried. Duplicate
// candidates (e.g. a name already ending in .dds) are dropped, keeping the earliest step. The
// first candidate that exists wins; if it then fails to decode, the lookup fails (no further
// candidates), as the original only checks existence before loading.
enum class TextureLookupStep : std::uint8_t { AsNamed = 0, TextureDir, DdsAsNamed, DdsTextureDir };
const char* texture_lookup_step_name(TextureLookupStep s);
struct TextureCandidate {
    std::string path;
    TextureLookupStep step = TextureLookupStep::AsNamed;
};
std::vector<TextureCandidate> texture_lookup(std::string_view name);
// Paths of texture_lookup(), in order.
std::vector<std::string> texture_candidates(std::string_view name);
// Try texture_lookup() in order. On failure out is untouched and *error lists the reason
// ("not found: <name>" when no candidate exists).
bool load_texture(const FileProvider& files, std::string_view name, TextureAsset& out,
                  std::string* error = nullptr, std::string* resolved_path = nullptr,
                  TextureLookupStep* resolved_step = nullptr);

// Fallbacks for missing/broken textures (RGBA8):
//   Color  : the original returns one shared 256x256 texture built from an image embedded in the
//            exe (SPACE_AND_OPEN.md B.2 step 5); its contents are not recovered. Here: 256x256
//            magenta/black checker of 32x32-texel squares, alpha 255, full box-filtered mip
//            chain (9 levels) [inferred content].
//   Normal : 4x4 flat tangent-space normal (128,128,255) with alpha 0 (no specular mask), 1 mip
//   White  : 4x4 opaque white, 1 mip
// Normal and White are engine additions (the original has the single fallback above).
enum class TextureRole : std::uint8_t { Color, Normal, White };
TextureAsset make_fallback_texture(TextureRole role);
// Role guess from a material parameter name: "*Normal*" / "*Bump*" -> Normal, else Color.
TextureRole texture_role_for_param(std::string_view param_name);

}  // namespace alamo::assets
