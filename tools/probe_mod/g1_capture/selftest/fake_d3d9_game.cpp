// fake_d3d9_game.cpp - stand-in "StarWarsG.exe" for the G1 capture kit self-test (not engine code).
// A tiny D3D9 app whose frame mimics the call shapes the kit's analysis looks for:
//   engine default CULLMODE=CW at device creation, a draw with a pixel shader whose CTAB has `Colorization`,
//   a stencil-darken style draw (STENCILFUNC NOTEQUAL, ref 0, mask 0x4F), a heat target cleared to 0x008080FF,
//   StretchRect copies (SceneTexture, DistortionTexture) and a full-screen 2-triangle strip (SceneHeat stand-in).
// Usage: fake_d3d9_game.exe [any game args...]; runs ~G1_FAKE_SECONDS seconds (default 12) or until closed.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool g_quit = false;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { g_quit = true; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static ID3DBlob* compile(const char* src, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), "g1", nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "%s\n", static_cast<const char*>(err->GetBufferPointer()));
        ExitProcess(3);
    }
    if (err) err->Release();
    return code;
}

static const char* kVs =
    "float4 RESOLUTION_CONSTANTS; float4 Offset;\n"
    "struct O { float4 p : POSITION; float2 uv : TEXCOORD0; };\n"
    "O main(float2 p : POSITION) { O o; o.p = float4(p * Offset.z + Offset.xy, 0.5, 1);\n"
    "  o.uv = float2(p.x * 0.5 + 0.5, -p.y * 0.5 + 0.5) - RESOLUTION_CONSTANTS.zw; return o; }\n";
static const char* kPsShip =
    "float4 Colorization; float4 Diffuse;\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { float p = 0.5 + 0.25 * sin(uv.x * 90) + 0.25 * sin(uv.y * 70 + uv.x * 20);\n"
    "  return Diffuse * lerp(float4(1,1,1,1), Colorization, uv.x) * p; }\n";
static const char* kPsHeat =
    "sampler SceneTexture : register(s0); sampler DistortionTexture : register(s1); float DistortionAmount;\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { float2 o = (tex2D(DistortionTexture, uv).rg - 0.5) * 2 * DistortionAmount;\n"
    "  return tex2D(SceneTexture, uv + o); }\n";

static UINT reg_of(ID3DBlob* code, const char* name) {
    // Find the register of a constant through the CTAB (D3DXGetShaderConstantTable is not in the SDK any more).
    const DWORD* p = static_cast<const DWORD*>(code->GetBufferPointer());
    const size_t n = code->GetBufferSize() / 4;
    for (size_t i = 1; i + 2 < n; ++i) {
        if ((p[i] & 0xFFFF) == 0xFFFE && p[i + 1] == 0x42415443u) {  // comment + 'CTAB'
            const char* base = reinterpret_cast<const char*>(&p[i + 2]);
            const DWORD* hdr = reinterpret_cast<const DWORD*>(base);
            const DWORD count = hdr[3];
            const DWORD info = hdr[4];
            for (DWORD c = 0; c < count; ++c) {
                const DWORD* ci = reinterpret_cast<const DWORD*>(base + info + c * 20);
                const char* nm = base + ci[0];
                if (strcmp(nm, name) == 0) return ci[1] >> 16;  // RegisterSet(u16) | RegisterIndex(u16)
            }
        }
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    const char* secs_env = getenv("G1_FAKE_SECONDS");
    const double seconds = secs_env ? atof(secs_env) : 12.0;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"G1FakeGame";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const int W = 640, H = 360;
    RECT r{0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"G1FakeGame", L"G1 fake StarWarsG", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 2;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return 2;
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);  // the "engine default" the kit wants to observe

    ID3DBlob* vsb = compile(kVs, "vs_3_0");
    ID3DBlob* psb = compile(kPsShip, "ps_3_0");
    ID3DBlob* phb = compile(kPsHeat, "ps_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps_ship = nullptr;
    IDirect3DPixelShader9* ps_heat = nullptr;
    dev->CreateVertexShader(static_cast<const DWORD*>(vsb->GetBufferPointer()), &vs);
    dev->CreatePixelShader(static_cast<const DWORD*>(psb->GetBufferPointer()), &ps_ship);
    dev->CreatePixelShader(static_cast<const DWORD*>(phb->GetBufferPointer()), &ps_heat);
    const UINT reg_color = reg_of(psb, "Colorization");
    const UINT reg_diffuse = reg_of(psb, "Diffuse");
    const UINT reg_amount = reg_of(phb, "DistortionAmount");
    const UINT reg_res = reg_of(vsb, "RESOLUTION_CONSTANTS");
    const UINT reg_off = reg_of(vsb, "Offset");

    const D3DVERTEXELEMENT9 decl_el[] = {{0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                         D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->CreateVertexDeclaration(decl_el, &decl);
    const float quad[8] = {-1.005f, -1.005f, 1.005f, -1.005f, -1.005f, 1.005f, 1.005f, 1.005f};
    IDirect3DVertexBuffer9* vb = nullptr;
    dev->CreateVertexBuffer(sizeof(quad), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    void* map = nullptr;
    vb->Lock(0, 0, &map, 0);
    memcpy(map, quad, sizeof(quad));
    vb->Unlock();

    IDirect3DTexture9* scene_tex = nullptr;
    IDirect3DTexture9* dist_tex = nullptr;
    IDirect3DTexture9* heat_rt = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scene_tex, nullptr);
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dist_tex, nullptr);
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &heat_rt, nullptr);
    IDirect3DSurface9 *bb = nullptr, *scene_s = nullptr, *dist_s = nullptr, *heat_s = nullptr;
    dev->GetRenderTarget(0, &bb);
    scene_tex->GetSurfaceLevel(0, &scene_s);
    dist_tex->GetSurfaceLevel(0, &dist_s);
    heat_rt->GetSurfaceLevel(0, &heat_s);

    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    unsigned frame = 0;
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        QueryPerformanceCounter(&t);
        if (static_cast<double>(t.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) > seconds) break;
        const float res[4] = {static_cast<float>(W), static_cast<float>(H), 0.5f / W, 0.5f / H};
        dev->BeginScene();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
        dev->SetVertexDeclaration(decl);
        dev->SetStreamSource(0, vb, 0, 8);
        dev->SetVertexShader(vs);
        dev->SetVertexShaderConstantF(reg_res, res, 1);
        // "ship": a quad with Colorization (MP palette blue 78,150,237).
        const float off_ship[4] = {0.02f * static_cast<float>(frame % 60) / 60.0f, 0.0f, 0.95f, 0};
        const float colz[4] = {78.0f / 255.0f, 150.0f / 255.0f, 237.0f / 255.0f, 1.0f};
        const float diff[4] = {0.9f, 0.9f, 0.9f, 1.0f};
        dev->SetVertexShaderConstantF(reg_off, off_ship, 1);
        dev->SetPixelShader(ps_ship);
        dev->SetPixelShaderConstantF(reg_color, colz, 1);
        dev->SetPixelShaderConstantF(reg_diffuse, diff, 1);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        // calibration draws for the DXVK cull mapping (tiny, in a corner): CCW then NONE, then back to CW
        const float off_tiny[4] = {-0.95f, -0.95f, 0.02f, 0};
        dev->SetVertexShaderConstantF(reg_off, off_tiny, 1);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        // stencil-darken style draw
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
        dev->SetRenderState(D3DRS_STENCILREF, 0);
        dev->SetRenderState(D3DRS_STENCILMASK, 0x4F);
        dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        const float off_full[4] = {0, 0, 1, 0};
        dev->SetVertexShaderConstantF(reg_off, off_full, 1);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        // heat: SceneTexture copy, heat target cleared to 0x008080FF, DistortionTexture copy, SceneHeat strip
        dev->StretchRect(bb, nullptr, scene_s, nullptr, D3DTEXF_POINT);
        dev->SetRenderTarget(0, heat_s);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x008080FF, 1.0f, 0);
        dev->StretchRect(heat_s, nullptr, dist_s, nullptr, D3DTEXF_POINT);
        dev->SetRenderTarget(0, bb);
        dev->SetVertexShaderConstantF(reg_off, off_full, 1);
        dev->SetPixelShader(ps_heat);
        const float amount[4] = {0.25f, 0, 0, 0};
        dev->SetPixelShaderConstantF(reg_amount, amount, 1);
        dev->SetTexture(0, scene_tex);
        dev->SetTexture(1, dist_tex);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        ++frame;
    }
    heat_s->Release(); dist_s->Release(); scene_s->Release(); bb->Release();
    heat_rt->Release(); dist_tex->Release(); scene_tex->Release();
    vb->Release(); decl->Release(); ps_heat->Release(); ps_ship->Release(); vs->Release();
    vsb->Release(); psb->Release(); phb->Release();
    dev->Release();
    d3d->Release();
    return 0;
}
