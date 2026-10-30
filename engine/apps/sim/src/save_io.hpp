// N1 stage 2 (owner: n2-host): field archives for the apps/sim save code (TacticalBattle, CampaignBattles, BattleAi,
// TacticalMode). One generic field list per record serves both directions:
//     auto unit = [](auto& a, auto& u) { a(u.id, u.h, u.alive); };
//     save_io::Out o(w); o.seq(units_, unit);         // write
//     save_io::In i(r);  i.seq(units_, unit);         // read
// Encoding: ByteWriter primitives (floats as bit patterns), enums as their underlying type, ObjectHandle as u64,
// Vec3 as 3 x f32, vectors / maps as u32 count + elements, pairs as first then second. In fails (ByteReader::fail) on
// a count above kMaxCount or larger than the bytes left; callers check ok() and done().
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::sim::save_io {

constexpr uint32_t kMaxCount = 1u << 24;

class Out {
public:
    static constexpr bool kReading = false;
    explicit Out(ByteWriter& w) : w_(w) {}
    ByteWriter& writer() { return w_; }

    void v(bool x) { w_.boolean(x); }
    void v(uint8_t x) { w_.u8(x); }
    void v(int8_t x) { w_.u8(static_cast<uint8_t>(x)); }
    void v(uint16_t x) { w_.u16(x); }
    void v(uint32_t x) { w_.u32(x); }
    void v(int32_t x) { w_.i32(x); }
    void v(uint64_t x) { w_.u64(x); }
    void v(int64_t x) { w_.i64(x); }
    void v(float x) { w_.f32(x); }
    void v(double x) { w_.f64(x); }
    void v(const std::string& x) { w_.str(x); }
    void v(const Vec3& x) { w_.f32(x.x).f32(x.y).f32(x.z); }
    void v(objects::ObjectHandle x) { w_.u64(x.value); }
    // EventData: u8 type id, bool queued, the canonical payload (commands::encode_payload) as u32 length + bytes.
    void v(const commands::EventData& x) {
        std::vector<uint8_t> p;
        commands::encode_payload(x, p);
        w_.u8(x.type_id()).boolean(x.queued()).u32(static_cast<uint32_t>(p.size()));
        w_.raw(p);
    }
    template <class E>
        requires std::is_enum_v<E>
    void v(E x) {
        v(static_cast<std::underlying_type_t<E>>(x));
    }
    template <class A, class B>
    void v(const std::pair<A, B>& x) {
        v(x.first);
        v(x.second);
    }
    template <class T>
    void v(const std::vector<T>& x) {
        w_.u32(static_cast<uint32_t>(x.size()));
        for (const T& e : x) v(e);
    }
    template <class K, class T>
    void v(const std::map<K, T>& x) {
        w_.u32(static_cast<uint32_t>(x.size()));
        for (const auto& [k, e] : x) {
            v(k);
            v(e);
        }
    }

    template <class... T>
    Out& operator()(const T&... x) {
        (v(x), ...);
        return *this;
    }
    // Records: u32 count + fn(*this, element) per element.
    template <class T, class F>
    void seq(const std::vector<T>& s, F&& fn) {
        w_.u32(static_cast<uint32_t>(s.size()));
        for (const T& e : s) fn(*this, e);
    }
    template <class K, class T, class F>
    void map(const std::map<K, T>& s, F&& fn) {
        w_.u32(static_cast<uint32_t>(s.size()));
        for (const auto& [k, e] : s) {
            v(k);
            fn(*this, e);
        }
    }

private:
    ByteWriter& w_;
};

class In {
public:
    static constexpr bool kReading = true;
    explicit In(ByteReader& r) : r_(r) {}
    ByteReader& reader() { return r_; }
    bool ok() const { return r_.ok(); }

    void v(bool& x) { x = r_.boolean(); }
    void v(uint8_t& x) { x = r_.u8(); }
    void v(int8_t& x) { x = static_cast<int8_t>(r_.u8()); }
    void v(uint16_t& x) { x = r_.u16(); }
    void v(uint32_t& x) { x = r_.u32(); }
    void v(int32_t& x) { x = r_.i32(); }
    void v(uint64_t& x) { x = r_.u64(); }
    void v(int64_t& x) { x = r_.i64(); }
    void v(float& x) { x = r_.f32(); }
    void v(double& x) { x = r_.f64(); }
    void v(std::string& x) { x = r_.str(); }
    void v(Vec3& x) {
        x.x = r_.f32();
        x.y = r_.f32();
        x.z = r_.f32();
    }
    void v(objects::ObjectHandle& x) { x.value = r_.u64(); }
    void v(commands::EventData& x) {
        const uint8_t type = r_.u8();
        const bool queued = r_.boolean();
        const uint32_t n = r_.u32();
        const std::span<const uint8_t> p = r_.raw(n);
        if (!r_.ok() || commands::decode_payload(type, p, x) != commands::DecodeError::None) {
            r_.fail();
            return;
        }
        x.set_queued(queued);
    }
    template <class E>
        requires std::is_enum_v<E>
    void v(E& x) {
        std::underlying_type_t<E> u{};
        v(u);
        x = static_cast<E>(u);
    }
    template <class A, class B>
    void v(std::pair<A, B>& x) {
        v(x.first);
        v(x.second);
    }
    template <class T>
    void v(std::vector<T>& x) {
        const uint32_t n = count();
        x.clear();
        x.resize(n);
        for (T& e : x) {
            if (!r_.ok()) break;
            v(e);
        }
    }
    template <class K, class T>
    void v(std::map<K, T>& x) {
        const uint32_t n = count();
        x.clear();
        for (uint32_t i = 0; i < n && r_.ok(); ++i) {
            K k{};
            v(k);
            v(x[k]);
        }
    }

    template <class... T>
    In& operator()(T&... x) {
        (v(x), ...);
        return *this;
    }
    template <class T, class F>
    void seq(std::vector<T>& s, F&& fn) {
        const uint32_t n = count();
        s.clear();
        s.resize(n);
        for (T& e : s) {
            if (!r_.ok()) break;
            fn(*this, e);
        }
    }
    template <class K, class T, class F>
    void map(std::map<K, T>& s, F&& fn) {
        const uint32_t n = count();
        s.clear();
        for (uint32_t i = 0; i < n && r_.ok(); ++i) {
            K k{};
            v(k);
            fn(*this, s[k]);
        }
    }
    // A u32 count: fails above kMaxCount or when the bytes left cannot hold that many 1-byte records.
    uint32_t count() {
        const uint32_t n = r_.u32();
        if (!r_.ok() || n > kMaxCount || n > r_.remaining()) {
            r_.fail();
            return 0;
        }
        return n;
    }

private:
    ByteReader& r_;
};

}  // namespace alamo::sim::save_io
