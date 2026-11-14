#include "alamo/formats/minichunk_file.hpp"

#include <cstring>

namespace alamo::minifile {

namespace {
void walk(ChunkCursor cur, File& f) {
    while (auto c = cur.next()) {
        if (c->has_children) {
            walk(c->children(), f);
            continue;
        }
        Record r;
        r.chunk_id = c->id;
        auto mc = c->minis();
        while (auto m = mc.next()) {
            r.mini_ids.push_back(m->id);
            r.mini_data.emplace_back(m->data.begin(), m->data.end());
        }
        f.records.push_back(std::move(r));
    }
}
}  // namespace

File parse(Bytes data) {
    File f;
    walk(top_level_chunks(data), f);
    return f;
}

File load(const std::filesystem::path& p) {
    auto b = read_file(p);
    return parse(b);
}

const std::vector<std::uint8_t>* Record::get(std::uint8_t id) const {
    for (std::size_t i = 0; i < mini_ids.size(); ++i)
        if (mini_ids[i] == id) return &mini_data[i];
    return nullptr;
}

std::vector<std::string> Record::strings() const {
    std::vector<std::string> out;
    for (const auto& d : mini_data) {
        if (d.size() < 2 || d.back() != 0) continue;
        bool printable = true;
        for (std::size_t i = 0; i + 1 < d.size(); ++i)
            if (d[i] < 0x20 || d[i] > 0x7E) { printable = false; break; }
        if (printable) out.emplace_back(reinterpret_cast<const char*>(d.data()), d.size() - 1);
    }
    return out;
}

std::optional<float> Record::f32(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->size() != 4) return std::nullopt;
    float v;
    std::memcpy(&v, d->data(), 4);
    return v;
}
std::optional<std::uint32_t> Record::u32(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->empty() || d->size() > 4 || d->size() == 3) return std::nullopt;
    std::uint32_t v = 0;
    std::memcpy(&v, d->data(), d->size());
    return v;
}
std::optional<Vec3> Record::vec3(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->size() != 12) return std::nullopt;
    Vec3 v;
    std::memcpy(&v, d->data(), 12);
    return v;
}
std::optional<std::string> Record::str(std::uint8_t id) const {
    auto* d = get(id);
    if (!d) return std::nullopt;
    return to_cstring(Bytes(d->data(), d->size()));
}

}  // namespace alamo::minifile
