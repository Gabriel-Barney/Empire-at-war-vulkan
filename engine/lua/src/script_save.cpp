// q6 t19: the shared script block (include/alamo/lua/script_save.hpp, docs/re/LUA_SAVE.md §2).
//
// Layout (little-endian): u32 version, u64 FNV-1a 64 of the rest, then: str name, u8 option flags (bit 0 math library,
// bit 1 upper-cased type names), u8 saved, str save error, u32 thread count, per thread (id order) i32 id, str start
// function, u8 started, u8 killed; i32 next thread id; u32 image length, the LuaVm::write image.
#include "alamo/lua/script_save.hpp"

#include "alamo/core/hash.hpp"

namespace alamo::lua {

namespace {

constexpr uint8_t kOptMath = 1u << 0;
constexpr uint8_t kOptUpperTypes = 1u << 1;
constexpr uint32_t kMaxThreads = 1u << 20;

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

// Parses everything after the checksum; `image` = the LuaVm::write record.
bool parse_body(ByteReader& r, ScriptBlockHeader& h, std::span<const uint8_t>& image, std::string* err) {
    h.name = r.str();
    const uint8_t opts = r.u8();
    h.saved = r.boolean();
    h.save_error = r.str();
    const uint32_t n = r.u32();
    if (!r.ok()) return fail(err, "truncated script block");
    if ((opts & ~(kOptMath | kOptUpperTypes)) != 0) return fail(err, "unknown script block options");
    h.math_library = (opts & kOptMath) != 0;
    h.upper_type_names = (opts & kOptUpperTypes) != 0;
    if (n > kMaxThreads || n > r.remaining() / 10) return fail(err, "bad thread record count");
    h.threads.clear();
    h.threads.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        LuaVm::ThreadInfo t;
        t.id = r.i32();
        t.function = r.str();
        t.started = r.boolean();
        t.killed = r.boolean();
        if (!r.ok()) return fail(err, "truncated thread record");
        if (t.id < 0 || (!h.threads.empty() && t.id <= h.threads.back().id)) return fail(err, "thread records out of order");
        h.threads.push_back(std::move(t));
    }
    h.next_thread = r.i32();
    const uint32_t len = r.u32();
    if (!r.ok() || len > r.remaining()) return fail(err, "truncated script block");
    if (h.next_thread < 0 || (!h.threads.empty() && h.threads.back().id >= h.next_thread)) return fail(err, "bad next thread id");
    image = r.raw(len);
    if (!r.ok() || !r.done()) return fail(err, "trailing bytes after the script image");
    return true;
}

bool parse(ByteReader& r, ScriptBlockHeader& h, std::span<const uint8_t>& image, std::string* err) {
    h.version = r.u32();
    const uint64_t sum = r.u64();
    if (!r.ok()) return fail(err, "truncated script block");
    if (h.version != kScriptBlockVersion) return fail(err, "unknown script block version");
    const std::span<const uint8_t> rest = r.raw(r.remaining());
    if (fnv1a64(rest.data(), rest.size()) != sum) return fail(err, "script block checksum mismatch");
    ByteReader body(rest);
    return parse_body(body, h, image, err);
}

}  // namespace

void write_script_block(ByteWriter& w, const LuaVm& vm) {
    ByteWriter image;
    vm.write(image);  // logs and records save_error() itself when the state cannot be saved
    const LuaVmOptions& o = vm.options();
    const std::vector<LuaVm::ThreadInfo> threads = vm.thread_list();
    ByteWriter body;
    body.str(o.script_name);
    body.u8(static_cast<uint8_t>((o.math_library ? kOptMath : 0u) | (o.upper_type_names ? kOptUpperTypes : 0u)));
    body.boolean(vm.save_error().empty());
    body.str(vm.save_error());
    body.u32(static_cast<uint32_t>(threads.size()));
    for (const LuaVm::ThreadInfo& t : threads) body.i32(t.id).str(t.function).boolean(t.started).boolean(t.killed);
    body.i32(vm.next_thread_id());
    body.u32(static_cast<uint32_t>(image.size()));
    body.raw(image.bytes());
    w.u32(kScriptBlockVersion);
    w.u64(fnv1a64(body.bytes().data(), body.size()));
    w.raw(body.bytes());
}

bool peek_script_block(std::span<const uint8_t> block, ScriptBlockHeader& out, std::string* err) {
    ByteReader r(block);
    ScriptBlockHeader h;
    std::span<const uint8_t> image;
    if (!parse(r, h, image, err)) return false;
    out = std::move(h);
    return true;
}

bool read_script_block(ByteReader& r, LuaVm& vm, ScriptLoad mode, std::string* err) {
    ScriptBlockHeader h;
    std::span<const uint8_t> image;
    if (!parse(r, h, image, err)) {
        r.fail();
        return false;
    }
    const LuaVmOptions& o = vm.options();
    if (h.name != o.script_name) return fail(err, "the script block is for another script");
    if (h.math_library != o.math_library || h.upper_type_names != o.upper_type_names)
        return fail(err, "the script block was saved with other VM options");
    if (!h.saved) {
        if (err) *err = "the script could not be saved: " + h.save_error;
        return false;
    }
    if (mode == ScriptLoad::FromSave && !vm.fresh())
        return fail(err, "not a from-save instance (code ran or a thread was created before the load)");
    ByteReader ir(image);
    if (!vm.read(ir) || !ir.ok() || !ir.done()) return fail(err, "the script image was refused");
    // The header and the image come from one checksummed write: a mismatch here is a writer bug.
    if (vm.thread_list() != h.threads || vm.next_thread_id() != h.next_thread)
        return fail(err, "the restored thread records differ from the block's");
    return true;
}

}  // namespace alamo::lua
