#include "alamo/formats/common.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace alamo {

std::string ByteReader::fixed_string(std::size_t n) { return to_cstring(bytes(n)); }

std::string to_cstring(Bytes b) {
    std::size_t n = 0;
    while (n < b.size() && b[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(b.data()), n);
}

std::string hex_id(std::uint32_t id) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", id);
    return buf;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    f.seekg(0, std::ios::end);
    auto len = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> buf(len);
    if (len && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len)))
        throw std::runtime_error("read failed: " + path.string());
    return buf;
}

namespace {
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();
}  // namespace

std::uint32_t crc32(Bytes data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (auto b : data) c = kCrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
std::uint32_t crc32(std::string_view s) {
    return crc32(Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

// ---- Mat34 ----
Vec3 Mat34::transform_point(const Vec3& p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}
Vec3 Mat34::transform_dir(const Vec3& p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z,
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
}
Mat34 Mat34::operator*(const Mat34& b) const {
    Mat34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
            if (j == 3) v += m[i][3];
            r.m[i][j] = v;
        }
    }
    return r;
}
Mat34 Mat34::inverse() const {
    const float a = m[0][0], b = m[0][1], c = m[0][2];
    const float d = m[1][0], e = m[1][1], f = m[1][2];
    const float g = m[2][0], h = m[2][1], i = m[2][2];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    Mat34 r;
    if (std::fabs(det) < 1e-20f) return r;  // degenerate: return identity
    const float id = 1.0f / det;
    r.m[0][0] = A * id;              r.m[0][1] = -(b * i - c * h) * id; r.m[0][2] = (b * f - c * e) * id;
    r.m[1][0] = B * id;              r.m[1][1] = (a * i - c * g) * id;  r.m[1][2] = -(a * f - c * d) * id;
    r.m[2][0] = C * id;              r.m[2][1] = -(a * h - b * g) * id; r.m[2][2] = (a * e - b * d) * id;
    for (int k = 0; k < 3; ++k)
        r.m[k][3] = -(r.m[k][0] * m[0][3] + r.m[k][1] * m[1][3] + r.m[k][2] * m[2][3]);
    return r;
}

}  // namespace alamo
