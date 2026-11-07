#include "alamo/assets/texture.hpp"

#include <algorithm>
#include <cstring>
#include <exception>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"

namespace alamo::assets {

namespace {

constexpr std::uint32_t kMaxDim = 16384;
constexpr std::size_t kMaxRlePixels = std::size_t(8192) * 8192;

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
std::uint16_t rd16(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint16_t v = 0;
    std::memcpy(&v, b.data() + off, 2);
    return v;
}
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) |
           (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24);
}
bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}
std::uint32_t mip_dim(std::uint32_t d, std::uint32_t mip) { return std::max<std::uint32_t>(1, d >> mip); }
std::uint32_t max_mips(std::uint32_t w, std::uint32_t h) {
    std::uint32_t n = 1;
    for (std::uint32_t d = std::max(w, h); d > 1; d >>= 1) ++n;
    return n;
}

// Uncompressed source pixel description (DDS masks).
struct MaskFormat {
    std::uint32_t bits = 0;
    std::uint32_t r = 0, g = 0, b = 0, a = 0;
    bool luminance = false;
};

struct Channel {
    std::uint32_t mask = 0, shift = 0, max = 0;
    explicit Channel(std::uint32_t m) : mask(m) {
        if (!m) return;
        while (!((m >> shift) & 1u)) ++shift;
        max = m >> shift;
    }
    // Returns -1 when the channel is absent.
    int get(std::uint32_t px) const {
        if (!mask || !max) return -1;
        const std::uint64_t v = (px & mask) >> shift;
        return static_cast<int>((v * 255u + max / 2) / max);
    }
};

// Convert one tightly packed level of `mf` pixels to RGBA8.
void convert_masked(const std::uint8_t* src, std::uint32_t w, std::uint32_t h, const MaskFormat& mf, std::uint8_t* dst) {
    const std::uint32_t bpp = mf.bits / 8;
    const Channel cr(mf.r), cg(mf.g), cb(mf.b), ca(mf.a);
    const std::size_t count = std::size_t(w) * h;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t px = 0;
        std::memcpy(&px, src + i * bpp, bpp);
        int r = cr.get(px), g = cg.get(px), b = cb.get(px);
        const int a = ca.get(px);
        if (mf.luminance) g = b = r;
        dst[i * 4 + 0] = static_cast<std::uint8_t>(r < 0 ? 0 : r);
        dst[i * 4 + 1] = static_cast<std::uint8_t>(g < 0 ? 0 : g);
        dst[i * 4 + 2] = static_cast<std::uint8_t>(b < 0 ? 0 : b);
        dst[i * 4 + 3] = static_cast<std::uint8_t>(a < 0 ? 255 : a);
    }
}

// Legacy D3DFMT values used as a DDS FourCC.
bool d3dfmt_masks(std::uint32_t f, MaskFormat& mf) {
    switch (f) {
        case 20: mf = {24, 0xFF0000, 0xFF00, 0xFF, 0, false}; return true;           // R8G8B8
        case 21: mf = {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u, false}; return true; // A8R8G8B8
        case 22: mf = {32, 0xFF0000, 0xFF00, 0xFF, 0, false}; return true;           // X8R8G8B8
        case 23: mf = {16, 0xF800, 0x7E0, 0x1F, 0, false}; return true;              // R5G6B5
        case 24: mf = {16, 0x7C00, 0x3E0, 0x1F, 0, false}; return true;              // X1R5G5B5
        case 25: mf = {16, 0x7C00, 0x3E0, 0x1F, 0x8000, false}; return true;         // A1R5G5B5
        case 26: mf = {16, 0xF00, 0xF0, 0xF, 0xF000, false}; return true;            // A4R4G4B4
        case 28: mf = {8, 0, 0, 0, 0xFF, false}; return true;                        // A8
        case 30: mf = {16, 0xF00, 0xF0, 0xF, 0, false}; return true;                 // X4R4G4B4
        case 32: mf = {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u, false}; return true; // A8B8G8R8
        case 33: mf = {32, 0xFF, 0xFF00, 0xFF0000, 0, false}; return true;           // X8B8G8R8
        case 50: mf = {8, 0xFF, 0, 0, 0, true}; return true;                         // L8
        case 51: mf = {16, 0xFF, 0, 0, 0xFF00, true}; return true;                   // A8L8
        default: return false;
    }
}

enum class Plan { Native, Convert };

// Bytes of `mips` levels x `layers` in format f.
std::uint64_t chain_bytes(TextureFormat f, std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t layers) {
    std::uint64_t per_layer = 0;
    for (std::uint32_t k = 0; k < mips; ++k) per_layer += texture_level_size(f, mip_dim(w, k), mip_dim(h, k));
    return per_layer * layers;
}

// Final decoded size of a texture: a single uncompressed level gets a full generated chain.
bool within_budget(TextureFormat f, std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t layers,
                   std::string* error) {
    const bool gen = mips == 1 && f != TextureFormat::BC1_RGBA && f != TextureFormat::BC2 && f != TextureFormat::BC3;
    const std::uint64_t total = chain_bytes(f, w, h, gen ? max_mips(w, h) : mips, layers);
    if (total <= kMaxDecodedTextureBytes) return true;
    return fail(error, "decoded texture would need " + std::to_string(total >> 20) + " MiB (budget " +
                           std::to_string(kMaxDecodedTextureBytes >> 20) + " MiB)");
}

}  // namespace

const char* texture_format_name(TextureFormat f) {
    switch (f) {
        case TextureFormat::BC1_RGBA: return "BC1_RGBA";
        case TextureFormat::BC2: return "BC2";
        case TextureFormat::BC3: return "BC3";
        case TextureFormat::RGBA8: return "RGBA8";
        case TextureFormat::BGRA8: return "BGRA8";
        case TextureFormat::R8: return "R8";
        case TextureFormat::R8G8: return "R8G8";
        default: return "Unknown";
    }
}

const char* texture_format_vk_name(TextureFormat f) {
    switch (f) {
        case TextureFormat::BC1_RGBA: return "VK_FORMAT_BC1_RGBA_UNORM_BLOCK";
        case TextureFormat::BC2: return "VK_FORMAT_BC2_UNORM_BLOCK";
        case TextureFormat::BC3: return "VK_FORMAT_BC3_UNORM_BLOCK";
        case TextureFormat::RGBA8: return "VK_FORMAT_R8G8B8A8_UNORM";
        case TextureFormat::BGRA8: return "VK_FORMAT_B8G8R8A8_UNORM";
        case TextureFormat::R8: return "VK_FORMAT_R8_UNORM";
        case TextureFormat::R8G8: return "VK_FORMAT_R8G8_UNORM";
        default: return "VK_FORMAT_UNDEFINED";
    }
}

std::uint64_t texture_level_size(TextureFormat f, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bw = (std::uint64_t(w) + 3) / 4, bh = (std::uint64_t(h) + 3) / 4;
    switch (f) {
        case TextureFormat::BC1_RGBA: return bw * bh * 8;
        case TextureFormat::BC2:
        case TextureFormat::BC3: return bw * bh * 16;
        case TextureFormat::RGBA8:
        case TextureFormat::BGRA8: return std::uint64_t(w) * h * 4;
        case TextureFormat::R8: return std::uint64_t(w) * h;
        case TextureFormat::R8G8: return std::uint64_t(w) * h * 2;
        default: return 0;
    }
}

bool generate_mips(TextureAsset& t) {
    std::size_t bpp = 0;
    switch (t.format) {
        case TextureFormat::RGBA8:
        case TextureFormat::BGRA8: bpp = 4; break;
        case TextureFormat::R8G8: bpp = 2; break;
        case TextureFormat::R8: bpp = 1; break;
        default: return false;
    }
    if (t.mip_levels != 1 || t.layers == 0 || t.levels.size() != t.layers || t.width == 0 || t.height == 0) return false;
    const std::uint32_t mips = max_mips(t.width, t.height);
    if (mips == 1) return false;
    std::uint64_t per_layer = 0;
    for (std::uint32_t k = 0; k < mips; ++k) per_layer += texture_level_size(t.format, mip_dim(t.width, k), mip_dim(t.height, k));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(per_layer * t.layers));
    std::vector<TextureLevel> levels;
    levels.reserve(std::size_t(t.layers) * mips);
    for (std::uint32_t l = 0; l < t.layers; ++l) {
        const TextureLevel& base = t.levels[l];
        if (base.layer != l || base.mip != 0 || base.width != t.width || base.height != t.height ||
            base.offset + base.size > t.data.size() || base.size != texture_level_size(t.format, t.width, t.height))
            return false;
        std::uint64_t off = std::uint64_t(l) * per_layer;
        std::memcpy(data.data() + off, t.data.data() + base.offset, static_cast<std::size_t>(base.size));
        levels.push_back({l, 0, t.width, t.height, off, base.size});
        for (std::uint32_t k = 1; k < mips; ++k) {
            const TextureLevel& src = levels.back();
            const std::uint32_t sw = src.width, sh = src.height;
            const std::uint32_t dw = mip_dim(t.width, k), dh = mip_dim(t.height, k);
            const std::uint64_t doff = src.offset + src.size;
            const std::uint64_t dsz = texture_level_size(t.format, dw, dh);
            const std::uint8_t* s = data.data() + src.offset;
            std::uint8_t* d = data.data() + doff;
            const std::uint32_t nx = sw > 1 ? 2u : 1u, ny = sh > 1 ? 2u : 1u;
            const std::uint32_t n = nx * ny;
            for (std::uint32_t y = 0; y < dh; ++y)
                for (std::uint32_t x = 0; x < dw; ++x)
                    for (std::size_t c = 0; c < bpp; ++c) {
                        std::uint32_t sum = 0;
                        for (std::uint32_t j = 0; j < ny; ++j)
                            for (std::uint32_t i = 0; i < nx; ++i)
                                sum += s[(std::size_t(y * ny + j) * sw + (x * nx + i)) * bpp + c];
                        d[(std::size_t(y) * dw + x) * bpp + c] = static_cast<std::uint8_t>((sum + n / 2) / n);
                    }
            levels.push_back({l, k, dw, dh, doff, dsz});
        }
    }
    t.data = std::move(data);
    t.levels = std::move(levels);
    t.mip_levels = mips;
    return true;
}

const TextureLevel* TextureAsset::level(std::uint32_t layer, std::uint32_t mip) const {
    if (layer >= layers || mip >= mip_levels) return nullptr;
    const std::size_t i = std::size_t(layer) * mip_levels + mip;
    return i < levels.size() ? &levels[i] : nullptr;
}

// ---- DDS -------------------------------------------------------------------------------------

bool decode_dds(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error) {
    if (file.size() < 128 || rd32(file, 0) != fourcc('D', 'D', 'S', ' ')) return fail(error, "not a DDS file");
    const std::size_t H = 4;
    if (rd32(file, H + 0) != 124) return fail(error, "DDS header size is not 124");
    const std::uint32_t height = rd32(file, H + 8), width = rd32(file, H + 12);
    const std::uint32_t depth = rd32(file, H + 20), mip_field = rd32(file, H + 24);
    const std::uint32_t pf_flags = rd32(file, H + 76), pf_fourcc = rd32(file, H + 80);
    const std::uint32_t caps2 = rd32(file, H + 108);
    if (width == 0 || height == 0 || width > kMaxDim || height > kMaxDim)
        return fail(error, "DDS dimensions out of range (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    if ((caps2 & 0x200000u) && depth > 1) return fail(error, "volume DDS textures are not supported");

    TextureAsset t;
    std::size_t data_off = 128;
    std::uint32_t layers = 1;
    if (caps2 & 0x200u) {
        if ((caps2 & 0xFC00u) != 0xFC00u) return fail(error, "partial cubemap (not all 6 faces present)");
        layers = 6;
        t.cubemap = true;
    }

    Plan plan = Plan::Native;
    MaskFormat mf;
    TextureFormat fmt = TextureFormat::Unknown;
    ComponentMapping sw;

    if (pf_flags & 0x4u) {  // FourCC
        switch (pf_fourcc) {
            case fourcc('D', 'X', 'T', '1'): fmt = TextureFormat::BC1_RGBA; break;
            case fourcc('D', 'X', 'T', '2'): fmt = TextureFormat::BC2; t.premultiplied = true; break;
            case fourcc('D', 'X', 'T', '3'): fmt = TextureFormat::BC2; break;
            case fourcc('D', 'X', 'T', '4'): fmt = TextureFormat::BC3; t.premultiplied = true; break;
            case fourcc('D', 'X', 'T', '5'): fmt = TextureFormat::BC3; break;
            case fourcc('D', 'X', '1', '0'): {
                if (file.size() < 148) return fail(error, "truncated DX10 header");
                const std::uint32_t dxgi = rd32(file, 128), misc = rd32(file, 136), array = rd32(file, 140);
                data_off = 148;
                if (array > 1) return fail(error, "DDS texture arrays are not supported");
                if (misc & 0x4u) { layers = 6; t.cubemap = true; }
                switch (dxgi) {
                    case 71: case 72: fmt = TextureFormat::BC1_RGBA; break;
                    case 74: case 75: fmt = TextureFormat::BC2; break;
                    case 77: case 78: fmt = TextureFormat::BC3; break;
                    case 28: case 29: fmt = TextureFormat::RGBA8; break;
                    case 87: case 91: fmt = TextureFormat::BGRA8; break;
                    case 88: case 93: fmt = TextureFormat::BGRA8; sw.a = Swizzle::One; break;
                    case 61: fmt = TextureFormat::R8; sw = {Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One}; break;
                    case 65: fmt = TextureFormat::R8; sw = {Swizzle::Zero, Swizzle::Zero, Swizzle::Zero, Swizzle::R}; break;
                    case 49: fmt = TextureFormat::R8G8; sw = {Swizzle::R, Swizzle::G, Swizzle::Zero, Swizzle::One}; break;
                    default: return fail(error, "unsupported DXGI format " + std::to_string(dxgi));
                }
                break;
            }
            default:
                if (!d3dfmt_masks(pf_fourcc, mf)) return fail(error, "unsupported DDS FourCC " + std::to_string(pf_fourcc));
                break;
        }
    } else if (pf_flags & (0x40u | 0x20000u | 0x2u)) {  // RGB / LUMINANCE / ALPHA
        mf.bits = rd32(file, H + 84);
        mf.r = rd32(file, H + 88);
        mf.g = rd32(file, H + 92);
        mf.b = rd32(file, H + 96);
        mf.a = (pf_flags & (0x1u | 0x2u)) ? rd32(file, H + 100) : 0;
        mf.luminance = (pf_flags & 0x20000u) != 0;
        if (pf_flags & 0x2u) mf.r = mf.g = mf.b = 0;  // alpha-only
        if (!mf.luminance && mf.r && mf.r == mf.g && mf.g == mf.b) mf.luminance = true;
        if (mf.luminance) mf.g = mf.b = 0;
    } else {
        return fail(error, "DDS pixel format has neither FourCC nor RGB/luminance/alpha flags");
    }

    if (fmt == TextureFormat::Unknown) {
        if (mf.bits != 8 && mf.bits != 16 && mf.bits != 24 && mf.bits != 32)
            return fail(error, "unsupported DDS bit count " + std::to_string(mf.bits));
        // Native layouts where possible, otherwise expand to RGBA8.
        if (mf.luminance && mf.bits == 8 && mf.r == 0xFF && !mf.a) {
            fmt = TextureFormat::R8;
            sw = {Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One};
        } else if (mf.luminance && mf.bits == 16 && mf.r == 0xFF && mf.a == 0xFF00) {
            fmt = TextureFormat::R8G8;
            sw = {Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::G};
        } else if (!mf.luminance && mf.bits == 8 && !mf.r && !mf.g && !mf.b && mf.a == 0xFF) {
            fmt = TextureFormat::R8;
            sw = {Swizzle::Zero, Swizzle::Zero, Swizzle::Zero, Swizzle::R};
        } else if (mf.bits == 32 && mf.r == 0xFF0000 && mf.g == 0xFF00 && mf.b == 0xFF &&
                   (mf.a == 0xFF000000u || mf.a == 0)) {
            fmt = TextureFormat::BGRA8;
            if (!mf.a) sw.a = Swizzle::One;
        } else if (mf.bits == 32 && mf.r == 0xFF && mf.g == 0xFF00 && mf.b == 0xFF0000 &&
                   (mf.a == 0xFF000000u || mf.a == 0)) {
            fmt = TextureFormat::RGBA8;
            if (!mf.a) sw.a = Swizzle::One;
        } else {
            fmt = TextureFormat::RGBA8;
            plan = Plan::Convert;
        }
    }

    // Source level sizes (for Convert: source bytes are bits/8 per pixel, tightly packed).
    auto src_size = [&](std::uint32_t w, std::uint32_t h) -> std::uint64_t {
        return plan == Plan::Native ? texture_level_size(fmt, w, h) : std::uint64_t(w) * h * (mf.bits / 8);
    };
    std::uint32_t mips = std::max<std::uint32_t>(1, mip_field);
    mips = std::min(mips, max_mips(width, height));
    const std::uint64_t avail = file.size() - data_off;
    std::uint32_t fit = 0;
    for (std::uint32_t m = 1; m <= mips; ++m) {
        std::uint64_t per_layer = 0;
        for (std::uint32_t k = 0; k < m; ++k) per_layer += src_size(mip_dim(width, k), mip_dim(height, k));
        if (per_layer * layers <= avail) fit = m;
        else break;
    }
    if (fit == 0) return fail(error, "DDS data truncated (first mip level incomplete)");
    if (fit < mips) {
        t.warnings.push_back("mip chain truncated: " + std::to_string(mips) + " declared, " + std::to_string(fit) + " present");
        // Layer strides in the file still use the declared chain length for cubemaps; only a
        // single-layer texture can be cut safely.
        if (layers > 1) return fail(error, "cubemap DDS data truncated");
    }
    std::uint64_t layer_stride = 0;  // in the file
    for (std::uint32_t k = 0; k < (layers > 1 ? mips : fit); ++k)
        layer_stride += src_size(mip_dim(width, k), mip_dim(height, k));
    mips = fit;
    if (!within_budget(fmt, width, height, mips, layers, error)) return false;

    t.format = fmt;
    t.swizzle = sw;
    t.width = width;
    t.height = height;
    t.mip_levels = mips;
    t.layers = layers;
    t.levels.reserve(std::size_t(layers) * mips);

    if (plan == Plan::Native) {
        for (std::uint32_t l = 0; l < layers; ++l) {
            std::uint64_t off = data_off + l * layer_stride;
            for (std::uint32_t k = 0; k < mips; ++k) {
                const std::uint32_t w = mip_dim(width, k), h = mip_dim(height, k);
                const std::uint64_t sz = texture_level_size(fmt, w, h);
                t.levels.push_back({l, k, w, h, off, sz});
                off += sz;
            }
        }
        t.data = std::move(file);
    } else {
        std::uint64_t total = 0;
        for (std::uint32_t k = 0; k < mips; ++k) total += texture_level_size(fmt, mip_dim(width, k), mip_dim(height, k));
        t.data.resize(static_cast<std::size_t>(total * layers));
        std::uint64_t dst_off = 0;
        for (std::uint32_t l = 0; l < layers; ++l) {
            std::uint64_t src_off = data_off + l * layer_stride;
            for (std::uint32_t k = 0; k < mips; ++k) {
                const std::uint32_t w = mip_dim(width, k), h = mip_dim(height, k);
                const std::uint64_t sz = texture_level_size(fmt, w, h);
                convert_masked(file.data() + src_off, w, h, mf, t.data.data() + dst_off);
                t.levels.push_back({l, k, w, h, dst_off, sz});
                src_off += src_size(w, h);
                dst_off += sz;
            }
        }
        std::vector<std::uint8_t>().swap(file);  // release the source before mips are generated
    }
    if (t.mip_levels == 1) generate_mips(t);  // no-op for BC formats
    out = std::move(t);
    return true;
}

// ---- TGA -------------------------------------------------------------------------------------

bool decode_tga(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error) {
    if (file.size() < 18) return fail(error, "TGA too small");
    const std::uint8_t id_len = file[0], cmap_type = file[1], type = file[2];
    const std::uint16_t cmap_len = rd16(file, 5);
    const std::uint8_t cmap_bits = file[7];
    const std::uint32_t width = rd16(file, 12), height = rd16(file, 14);
    const std::uint8_t bpp = file[16], desc = file[17];
    const bool rle = type == 10 || type == 11;
    const bool grey = type == 3 || type == 11;
    if (!(type == 2 || type == 3 || type == 10 || type == 11))
        return fail(error, "unsupported TGA image type " + std::to_string(type));
    if (width == 0 || height == 0 || width > kMaxDim || height > kMaxDim) return fail(error, "TGA dimensions out of range");
    if (grey ? (bpp != 8 && bpp != 16) : (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        return fail(error, "unsupported TGA bit depth " + std::to_string(bpp));
    std::size_t pos = 18 + std::size_t(id_len);
    if (cmap_type == 1) pos += (std::size_t(cmap_len) * cmap_bits + 7) / 8;
    if (pos > file.size()) return fail(error, "TGA truncated header");

    const std::size_t spp = (bpp + 7u) / 8u;  // source bytes per pixel
    const std::size_t count = std::size_t(width) * height;
    // Never allocate for a size the file cannot back: uncompressed data must be fully present; an
    // RLE packet (1 header byte + >= 1 pixel) expands to at most 128 pixels, and RLE output is
    // capped at kMaxRlePixels (8192^2; the largest shipped TGA is 2048^2).
    const std::size_t remaining = file.size() - pos;
    if (!rle && remaining < count * spp) return fail(error, "TGA pixel data truncated");
    if (!within_budget(grey ? (bpp == 8 ? TextureFormat::R8 : TextureFormat::R8G8) : TextureFormat::BGRA8, width,
                       height, 1, 1, error))
        return false;
    if (rle) {
        if (count > kMaxRlePixels) return fail(error, "RLE TGA larger than 8192x8192 is not supported");
        if (count > remaining / (1 + spp) * 128 + 128) return fail(error, "TGA RLE data too short for the image size");
    }
    std::vector<std::uint8_t> raw(count * spp);
    if (!rle) {
        if (file.size() - pos < raw.size()) return fail(error, "TGA pixel data truncated");
        std::memcpy(raw.data(), file.data() + pos, raw.size());
    } else {
        std::size_t px = 0;
        while (px < count) {
            if (pos >= file.size()) return fail(error, "TGA RLE data truncated");
            const std::uint8_t hdr = file[pos++];
            const std::size_t n = std::size_t(hdr & 0x7Fu) + 1;
            if (n > count - px) return fail(error, "TGA RLE packet overruns the image");
            if (hdr & 0x80u) {
                if (file.size() - pos < spp) return fail(error, "TGA RLE data truncated");
                for (std::size_t k = 0; k < n; ++k) std::memcpy(raw.data() + (px + k) * spp, file.data() + pos, spp);
                pos += spp;
            } else {
                if (file.size() - pos < n * spp) return fail(error, "TGA RLE data truncated");
                std::memcpy(raw.data() + px * spp, file.data() + pos, n * spp);
                pos += n * spp;
            }
            px += n;
        }
    }
    std::vector<std::uint8_t>().swap(file);  // only `raw` is needed from here on

    TextureAsset t;
    const unsigned alpha_bits = desc & 0x0Fu;
    std::size_t dpp = 4;
    if (grey) {
        dpp = bpp == 8 ? 1 : 2;
        t.format = bpp == 8 ? TextureFormat::R8 : TextureFormat::R8G8;
        t.swizzle = bpp == 8 ? ComponentMapping{Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One}
                             : ComponentMapping{Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::G};
    } else {
        // 32 bpp is A8R8G8B8 whatever the descriptor's alpha-bit count says (D3DX behaviour):
        // shipped MT_COMMANDBAR.TGA declares 0 alpha bits but carries a real alpha channel.
        t.format = TextureFormat::BGRA8;
    }
    t.data.resize(count * dpp);
    const bool top = (desc & 0x20u) != 0, right = (desc & 0x10u) != 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t sy = top ? y : height - 1 - y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t sx = right ? width - 1 - x : x;
            const std::uint8_t* s = raw.data() + (std::size_t(sy) * width + sx) * spp;
            std::uint8_t* d = t.data.data() + (std::size_t(y) * width + x) * dpp;
            if (grey) {
                d[0] = s[0];
                if (dpp == 2) d[1] = s[1];
            } else if (spp == 2) {
                const std::uint16_t v = static_cast<std::uint16_t>(s[0] | (s[1] << 8));
                const auto x5 = [](unsigned c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
                d[0] = x5(v & 0x1Fu);
                d[1] = x5((v >> 5) & 0x1Fu);
                d[2] = x5((v >> 10) & 0x1Fu);
                d[3] = (bpp == 16 && alpha_bits > 0) ? ((v & 0x8000u) ? 255 : 0) : 255;
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = spp == 4 ? s[3] : 255;
            }
        }
    }
    t.width = width;
    t.height = height;
    t.mip_levels = 1;
    t.layers = 1;
    t.levels.push_back({0, 0, width, height, 0, t.data.size()});
    std::vector<std::uint8_t>().swap(raw);  // release the pre-expansion pixels before mips
    generate_mips(t);
    out = std::move(t);
    return true;
}

// ---- BMP -------------------------------------------------------------------------------------

bool decode_bmp(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error) {
    if (file.size() < 26 || file[0] != 'B' || file[1] != 'M') return fail(error, "not a BMP file");
    const std::uint32_t pix_off = rd32(file, 10);
    const std::uint32_t hsz = rd32(file, 14);
    std::int64_t w = 0, h = 0;
    std::uint32_t bpp = 0, comp = 0;
    if (hsz == 12) {  // BITMAPCOREHEADER: 16-bit unsigned dimensions, always bottom-up
        w = rd16(file, 18);
        h = rd16(file, 20);
        bpp = rd16(file, 24);
    } else if (hsz >= 40) {  // BITMAPINFOHEADER / V2..V5: signed 32-bit dimensions, height < 0 = top-down
        if (file.size() < 54) return fail(error, "BMP header truncated");
        w = static_cast<std::int32_t>(rd32(file, 18));
        h = static_cast<std::int32_t>(rd32(file, 22));
        bpp = rd16(file, 28);
        comp = rd32(file, 30);
    } else {
        return fail(error, "unsupported BMP header size " + std::to_string(hsz));
    }
    const bool top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim)
        return fail(error, "BMP dimensions out of range (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    const std::uint32_t width = static_cast<std::uint32_t>(w), height = static_cast<std::uint32_t>(h);

    MaskFormat mf;
    mf.bits = bpp;
    bool all_zero_alpha_is_opaque = false;
    if (comp == 0) {  // BI_RGB
        switch (bpp) {
            case 16: mf.r = 0x7C00; mf.g = 0x3E0; mf.b = 0x1F; break;
            case 24: mf.r = 0xFF0000; mf.g = 0xFF00; mf.b = 0xFF; break;
            case 32:
                // The fourth byte is "reserved" in the format, but files converted from 32-bit TGAs carry their alpha
                // there (Thrawn's Revenge MT_Commandbar.tga is such a BMP), so it is kept; a file whose fourth byte is
                // zero everywhere is opaque [port: inferred, as most BMP readers do].
                mf.r = 0xFF0000; mf.g = 0xFF00; mf.b = 0xFF; mf.a = 0xFF000000u;
                all_zero_alpha_is_opaque = true;
                break;
            default: return fail(error, "unsupported BMP bit depth " + std::to_string(bpp) + " (BI_RGB)");
        }
    } else if (comp == 3 || comp == 6) {  // BI_BITFIELDS / BI_ALPHABITFIELDS
        if (bpp != 16 && bpp != 32) return fail(error, "unsupported BMP bit depth " + std::to_string(bpp) + " (bit fields)");
        // Masks sit inside a V2+ header (offset 40 of the DIB header), else right after a 40-byte one.
        const std::size_t m = 14 + 40;
        const bool alpha = comp == 6 || hsz >= 56;
        if (file.size() < m + (alpha ? 16u : 12u)) return fail(error, "BMP bit-field masks truncated");
        mf.r = rd32(file, m);
        mf.g = rd32(file, m + 4);
        mf.b = rd32(file, m + 8);
        mf.a = alpha ? rd32(file, m + 12) : 0;
        if (bpp == 16) {
            mf.r &= 0xFFFFu; mf.g &= 0xFFFFu; mf.b &= 0xFFFFu; mf.a &= 0xFFFFu;
        }
    } else {
        return fail(error, "unsupported BMP compression " + std::to_string(comp));
    }

    const std::uint32_t spp = bpp / 8;
    const std::uint64_t stride = (std::uint64_t(width) * bpp + 31) / 32 * 4;
    const std::uint64_t need = stride * (height - 1) + std::uint64_t(width) * spp;  // last row may lack its padding
    if (pix_off < 26 || pix_off > file.size() || file.size() - pix_off < need) return fail(error, "BMP pixel data truncated");
    if (!within_budget(TextureFormat::BGRA8, width, height, 1, 1, error)) return false;

    TextureAsset t;
    t.format = TextureFormat::BGRA8;
    t.data.resize(std::size_t(width) * height * 4);
    const Channel cr(mf.r), cg(mf.g), cb(mf.b), ca(mf.a);
    bool any_alpha = false;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t sy = top_down ? y : height - 1 - y;
        const std::uint8_t* s = file.data() + pix_off + sy * stride;
        std::uint8_t* d = t.data.data() + std::size_t(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x, s += spp, d += 4) {
            std::uint32_t px = 0;
            std::memcpy(&px, s, spp);
            const int r = cr.get(px), g = cg.get(px), b = cb.get(px), a = ca.get(px);
            d[0] = static_cast<std::uint8_t>(b < 0 ? 0 : b);
            d[1] = static_cast<std::uint8_t>(g < 0 ? 0 : g);
            d[2] = static_cast<std::uint8_t>(r < 0 ? 0 : r);
            d[3] = static_cast<std::uint8_t>(a < 0 ? 255 : a);
            any_alpha = any_alpha || a > 0;
        }
    }
    std::vector<std::uint8_t>().swap(file);
    if (all_zero_alpha_is_opaque && !any_alpha)
        for (std::size_t i = 3; i < t.data.size(); i += 4) t.data[i] = 255;
    t.width = width;
    t.height = height;
    t.mip_levels = 1;
    t.layers = 1;
    t.levels.push_back({0, 0, width, height, 0, t.data.size()});
    generate_mips(t);
    out = std::move(t);
    return true;
}

TextureFileKind sniff_texture_file(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') return TextureFileKind::Dds;
    // A TGA cannot start with "BM": its second byte is the colour-map type (0 or 1), never 'M'.
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return TextureFileKind::Bmp;
    return TextureFileKind::Tga;
}

bool decode_texture(std::vector<std::uint8_t> file, TextureAsset& out, std::string* error) {
    switch (sniff_texture_file(file)) {
        case TextureFileKind::Dds: return decode_dds(std::move(file), out, error);
        case TextureFileKind::Bmp: return decode_bmp(std::move(file), out, error);
        case TextureFileKind::Tga: break;
    }
    return decode_tga(std::move(file), out, error);
}

// ---- lookup ----------------------------------------------------------------------------------

const char* texture_lookup_step_name(TextureLookupStep s) {
    switch (s) {
        case TextureLookupStep::AsNamed: return "as named";
        case TextureLookupStep::TextureDir: return "Data\\Art\\Textures\\ + name";
        case TextureLookupStep::DdsAsNamed: return ".dds as named";
        case TextureLookupStep::DdsTextureDir: return "Data\\Art\\Textures\\ + .dds name";
    }
    return "?";
}

std::vector<TextureCandidate> texture_lookup(std::string_view name) {
    std::vector<TextureCandidate> out;
    const std::string s = normalize_path(name);
    if (s.empty()) return out;
    auto add = [&](std::string c, TextureLookupStep step) {
        for (const auto& o : out)
            if (o.path == c) return;
        out.push_back({std::move(c), step});
    };
    auto in_dir = [](const std::string& p) {
        const auto slash = p.find_last_of('\\');
        return normalize_path("Data\\Art\\Textures\\" + (slash == std::string::npos ? p : p.substr(slash + 1)));
    };
    add(s, TextureLookupStep::AsNamed);
    add(in_dir(s), TextureLookupStep::TextureDir);
    const std::string dds = s.substr(0, s.find('.')) + ".DDS";  // npos -> whole name
    add(dds, TextureLookupStep::DdsAsNamed);
    add(in_dir(dds), TextureLookupStep::DdsTextureDir);
    return out;
}

std::vector<std::string> texture_candidates(std::string_view name) {
    std::vector<std::string> out;
    for (auto& c : texture_lookup(name)) out.push_back(std::move(c.path));
    return out;
}

bool load_texture(const FileProvider& files, std::string_view name, TextureAsset& out, std::string* error,
                  std::string* resolved_path, TextureLookupStep* resolved_step) {
    for (const auto& c : texture_lookup(name)) {
        std::vector<std::uint8_t> bytes;
        if (!files.read(c.path, bytes)) continue;
        std::string e;
        TextureAsset t;
        if (!decode_texture(std::move(bytes), t, &e)) return fail(error, c.path + ": " + e);
        t.name = c.path;
        out = std::move(t);
        if (resolved_path) *resolved_path = c.path;
        if (resolved_step) *resolved_step = c.step;
        return true;
    }
    return fail(error, "not found: " + std::string(name));
}

TextureAsset make_fallback_texture(TextureRole role) {
    TextureAsset t;
    t.name = "<fallback>";
    t.is_fallback = true;
    t.format = TextureFormat::RGBA8;
    const std::uint32_t n = role == TextureRole::Color ? 256 : 4;
    t.width = t.height = n;
    t.mip_levels = 1;
    t.layers = 1;
    t.data.resize(std::size_t(n) * n * 4);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            std::uint8_t* d = t.data.data() + (std::size_t(y) * n + x) * 4;
            if (role == TextureRole::Normal) {
                d[0] = 128; d[1] = 128; d[2] = 255; d[3] = 0;
            } else if (role == TextureRole::White) {
                d[0] = d[1] = d[2] = d[3] = 255;
            } else {
                const bool on = ((x / 32) + (y / 32)) % 2 == 0;
                d[0] = on ? 255 : 0; d[1] = 0; d[2] = on ? 255 : 0; d[3] = 255;
            }
        }
    }
    t.levels.push_back({0, 0, n, n, 0, t.data.size()});
    if (role == TextureRole::Color) generate_mips(t);
    return t;
}

TextureRole texture_role_for_param(std::string_view p) {
    const std::string u = normalize_name(p);
    if (u.find("NORMAL") != std::string::npos || u.find("BUMP") != std::string::npos) return TextureRole::Normal;
    return TextureRole::Color;
}

}  // namespace alamo::assets
