// Runtime DXC loader (see dxc.hpp). The COM interfaces below are declared from the documented DXC
// API (IIDs / CLSIDs / vtable order of dxcapi.h, DXC v1.8); only the methods we call need correct
// signatures, but every earlier vtable slot is declared so the offsets match.
#include "alamo/shaderxlat/dxc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "alamo/core/hash.hpp"

#include <windows.h>

// Named (external-linkage) namespace on purpose: if these interfaces had internal linkage, GCC's
// type-inheritance analysis would see that no class in this TU implements them and turn every
// virtual call into unreachable code (observed with MinGW GCC 14: calls to garbage addresses).
namespace alamo::shaderxlat::dxc_abi {
#if defined(_WIN32) && !defined(_WIN64)
#define ALAMO_DXC_CALL __stdcall
#else
#define ALAMO_DXC_CALL
#endif
struct Guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};
using HRes = int32_t;
using ULong = unsigned long;

struct IUnk {
    virtual HRes ALAMO_DXC_CALL QueryInterface(const Guid& iid, void** out) = 0;
    virtual ULong ALAMO_DXC_CALL AddRef() = 0;
    virtual ULong ALAMO_DXC_CALL Release() = 0;
};
struct IBlob : IUnk {
    virtual void* ALAMO_DXC_CALL GetBufferPointer() = 0;
    virtual size_t ALAMO_DXC_CALL GetBufferSize() = 0;
};
struct IBlobEncoding : IBlob {
    virtual HRes ALAMO_DXC_CALL GetEncoding(int* known, uint32_t* code_page) = 0;
};
struct IOperationResult : IUnk {   // IDxcResult extends this; only these methods are used
    virtual HRes ALAMO_DXC_CALL GetStatus(HRes* status) = 0;
    virtual HRes ALAMO_DXC_CALL GetResult(IBlob** result) = 0;
    virtual HRes ALAMO_DXC_CALL GetErrorBuffer(IBlobEncoding** errors) = 0;
};
struct Buffer {
    const void* ptr;
    size_t size;
    uint32_t encoding;
};
struct ICompiler3 : IUnk {
    virtual HRes ALAMO_DXC_CALL Compile(const Buffer* source, const wchar_t** args, uint32_t argc, IUnk* include,
                                        const Guid& riid, void** result) = 0;
    virtual HRes ALAMO_DXC_CALL Disassemble(const Buffer* object, const Guid& riid, void** result) = 0;
};
struct IVersionInfo : IUnk {
    virtual HRes ALAMO_DXC_CALL GetVersion(uint32_t* major, uint32_t* minor) = 0;
    virtual HRes ALAMO_DXC_CALL GetFlags(uint32_t* flags) = 0;
};
struct IVersionInfo2 : IVersionInfo {
    virtual HRes ALAMO_DXC_CALL GetCommitInfo(uint32_t* count, char** hash) = 0;
};
using CreateFn = HRes(ALAMO_DXC_CALL*)(const Guid& clsid, const Guid& iid, void** out);

constexpr Guid kClsidCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr Guid kIidCompiler3 = {0x228B4687, 0x5A6A, 0x4730, {0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54}};
constexpr Guid kIidResult = {0x58346CDA, 0xDDE7, 0x4497, {0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59}};
constexpr Guid kIidVersionInfo = {0xb04f5b50, 0x2059, 0x4f12, {0xa8, 0xff, 0xa1, 0xe0, 0xcd, 0xe1, 0xcc, 0x7e}};
constexpr Guid kIidVersionInfo2 = {0xfb6904c4, 0x42f0, 0x4b62, {0x9c, 0x46, 0x98, 0x3a, 0xf7, 0xda, 0x7c, 0x83}};
}  // namespace alamo::shaderxlat::dxc_abi

namespace alamo::shaderxlat {

namespace abi = dxc_abi;

namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::filesystem::path exe_dir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
}

std::string env(const char* name) {
    char buf[4096];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

constexpr const char* kLibName = "dxcompiler.dll";
constexpr const char* kDxilName = "dxil.dll";
// Absolute-path loads whose dependencies resolve only from the DLL's own folder and System32 (never
// the current directory or PATH).
constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

// "size <bytes> fnv <hash>" of a file (cache-key identity of a DXC build without version info).
std::string file_identity(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "unreadable " + path.string();
    uint64_t h = alamo::kFnv1a64Offset, size = 0;
    std::vector<char> buf(1u << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<size_t>(f.gcount());
        if (got == 0) break;
        h = alamo::fnv1a64(buf.data(), got, h);
        size += got;
    }
    char out[64];
    std::snprintf(out, sizeof(out), "size %llu fnv %016llx", static_cast<unsigned long long>(size), static_cast<unsigned long long>(h));
    return out;
}

}  // namespace

struct RuntimeDxc::Impl {
    void* lib = nullptr;
    void* dxil = nullptr;   // Windows: dxil.dll pre-loaded from dxcompiler's folder (if present)
    abi::CreateFn create = nullptr;
    std::string version, path;
    mutable std::mutex mutex;
    mutable std::vector<abi::ICompiler3*> pool;

    abi::ICompiler3* acquire() const {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!pool.empty()) {
                abi::ICompiler3* c = pool.back();
                pool.pop_back();
                return c;
            }
        }
        void* out = nullptr;
        if (create(abi::kClsidCompiler, abi::kIidCompiler3, &out) < 0) return nullptr;
        return static_cast<abi::ICompiler3*>(out);
    }
    void give_back(abi::ICompiler3* c) const {
        std::lock_guard<std::mutex> lock(mutex);
        pool.push_back(c);
    }
    void unload() {
        for (abi::ICompiler3* c : pool) c->Release();
        pool.clear();
        if (lib) {
            FreeLibrary(static_cast<HMODULE>(lib));
        }
        if (dxil) FreeLibrary(static_cast<HMODULE>(dxil));
        dxil = nullptr;
        lib = nullptr;
        create = nullptr;
    }
    ~Impl() { unload(); }
};

RuntimeDxc::RuntimeDxc() : impl_(std::make_unique<Impl>()) {}
RuntimeDxc::~RuntimeDxc() = default;

bool RuntimeDxc::loaded() const { return impl_->create != nullptr; }
const std::string& RuntimeDxc::version() const { return impl_->version; }
const std::string& RuntimeDxc::library_path() const { return impl_->path; }

bool RuntimeDxc::load(const std::string& dir, std::string* err) {
    impl_->unload();
    std::vector<std::filesystem::path> candidates;
    auto add_dir = [&](const std::filesystem::path& d) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(d, ec);
        if (!ec && !abs.empty()) candidates.push_back(abs / kLibName);
    };
    if (!dir.empty()) {
        add_dir(dir);
    } else {
        const std::string e = env("ALAMO_DXC_DIR");
        if (!e.empty()) add_dir(e);
        const auto ed = exe_dir();
        if (!ed.empty()) add_dir(ed);
    }
    std::string tried;
    for (const auto& c : candidates) {
        void* lib = nullptr;
        // dxcompiler.dll loads dxil.dll by bare name when it needs it; pre-loading the copy next to
        // it makes that lookup resolve to the already-loaded module instead of a search.
        void* dxil = nullptr;
        const auto dxil_path = c.parent_path() / kDxilName;
        std::error_code dec;
        if (std::filesystem::is_regular_file(dxil_path, dec))
            dxil = static_cast<void*>(LoadLibraryExW(dxil_path.c_str(), nullptr, kLoadFlags));
        lib = static_cast<void*>(LoadLibraryExW(c.c_str(), nullptr, kLoadFlags));
        if (!lib && dxil) {
            FreeLibrary(static_cast<HMODULE>(dxil));
            dxil = nullptr;
        }
        void (*sym)() = lib ? reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(lib), "DxcCreateInstance")) : nullptr;
        tried += (tried.empty() ? "" : ", ") + c.string();
        if (!lib) continue;
        if (!sym) {
            FreeLibrary(static_cast<HMODULE>(lib));
            if (dxil) FreeLibrary(static_cast<HMODULE>(dxil));
            continue;
        }
        impl_->dxil = dxil;
        impl_->lib = lib;
        impl_->create = reinterpret_cast<abi::CreateFn>(sym);
        impl_->path = c.string();
        break;
    }
    if (!impl_->create) {
        if (err) *err = std::string("DXC library not found (tried ") + tried + ")";
        return false;
    }
    // Version string (part of the cache key).
    abi::ICompiler3* c = impl_->acquire();
    if (!c) {
        impl_->unload();
        if (err) *err = "DxcCreateInstance(CLSID_DxcCompiler) failed";
        return false;
    }
    uint32_t major = 0, minor = 0, commits = 0;
    std::string hash;
    void* vi = nullptr;
    if (c->QueryInterface(abi::kIidVersionInfo, &vi) >= 0 && vi) {
        auto* v = static_cast<abi::IVersionInfo*>(vi);
        v->GetVersion(&major, &minor);
        v->Release();
    }
    void* vi2 = nullptr;
    if (c->QueryInterface(abi::kIidVersionInfo2, &vi2) >= 0 && vi2) {
        auto* v = static_cast<abi::IVersionInfo2*>(vi2);
        char* h = nullptr;
        if (v->GetCommitInfo(&commits, &h) >= 0 && h) {
            hash.assign(h, strnlen(h, 64));
            // Allocated with CoTaskMemAlloc by DXC.
            if (HMODULE ole = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                using FreeFn = void(WINAPI*)(void*);
                if (auto f = reinterpret_cast<FreeFn>(reinterpret_cast<void (*)()>(GetProcAddress(ole, "CoTaskMemFree")))) f(h);
                FreeLibrary(ole);
            }
        }
        v->Release();
    }
    impl_->give_back(c);
    impl_->version = "dxcompiler " + std::to_string(major) + "." + std::to_string(minor) + " (" + std::to_string(commits) +
                     (hash.empty() ? std::string() : " " + hash) + ")";
    if ((major == 0 && minor == 0) || hash.empty()) {
        // No usable version info: key on the library file itself so different builds never share
        // cache entries. Computed once per load().
        std::filesystem::path real;
        std::wstring buf(32768, L'\0');
        const DWORD n = GetModuleFileNameW(static_cast<HMODULE>(impl_->lib), buf.data(), static_cast<DWORD>(buf.size()));
        if (n > 0 && n < buf.size()) {
            buf.resize(n);
            real = std::filesystem::path(buf);
        }
        if (real.empty()) real = std::filesystem::path(impl_->path);
        impl_->version += " [" + file_identity(real) + "]";
    }
    return true;
}

bool RuntimeDxc::compile(std::string_view hlsl, std::span<const std::string> args, std::vector<uint32_t>& spirv,
                         std::string* diagnostics) const {
    spirv.clear();
    if (diagnostics) diagnostics->clear();
    if (!loaded()) {
        if (diagnostics) *diagnostics = "DXC not loaded";
        return false;
    }
    std::vector<std::wstring> wargs;
    wargs.reserve(args.size());
    for (const std::string& a : args) wargs.emplace_back(a.begin(), a.end());  // arguments are ASCII
    std::vector<const wchar_t*> ptrs;
    for (const std::wstring& w : wargs) ptrs.push_back(w.c_str());

    abi::ICompiler3* c = impl_->acquire();
    if (!c) {
        if (diagnostics) *diagnostics = "DxcCreateInstance failed";
        return false;
    }
    const abi::Buffer src{hlsl.data(), hlsl.size(), 65001u /* CP_UTF8 */};
    void* out = nullptr;
    const abi::HRes hr = c->Compile(&src, ptrs.data(), static_cast<uint32_t>(ptrs.size()), nullptr, abi::kIidResult, &out);
    impl_->give_back(c);
    if (hr < 0 || !out) {
        if (diagnostics) *diagnostics = "IDxcCompiler3::Compile failed";
        return false;
    }
    auto* res = static_cast<abi::IOperationResult*>(out);
    abi::HRes status = -1;
    res->GetStatus(&status);
    abi::IBlobEncoding* errs = nullptr;
    if (diagnostics && res->GetErrorBuffer(&errs) >= 0 && errs) {
        const char* p = static_cast<const char*>(errs->GetBufferPointer());
        size_t n = errs->GetBufferSize();
        while (n > 0 && p && p[n - 1] == '\0') --n;
        if (p) diagnostics->assign(p, n);
    }
    release(errs);
    bool ok = false;
    if (status >= 0) {
        abi::IBlob* blob = nullptr;
        if (res->GetResult(&blob) >= 0 && blob) {
            const size_t bytes = blob->GetBufferSize();
            if (bytes >= 20 && bytes % 4 == 0) {
                spirv.resize(bytes / 4);
                std::memcpy(spirv.data(), blob->GetBufferPointer(), bytes);
                ok = spirv[0] == 0x07230203u;
            }
        }
        release(blob);
    }
    res->Release();
    if (!ok) spirv.clear();
    return ok;
}

bool RuntimeDxc::compile(const Translation& t, std::vector<uint32_t>& spirv, std::string* diagnostics) const {
    const std::vector<std::string> args = dxc_arguments(t);
    return compile(t.hlsl, args, spirv, diagnostics);
}

}  // namespace alamo::shaderxlat
