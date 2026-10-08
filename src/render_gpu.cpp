// GPU renderer: Direct3D 11 draws all the copies in one call (a textured quad each) into a DirectComposition surface, and
// the desktop compositor shows it. Written in C style; it is C++ only because the DirectComposition headers are.
#include "app.h"
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>

template <class T> static void Rel(T *&p) { if (p) { p->Release(); p = nullptr; } }
static HRESULT hr;
#define OK(call) (SUCCEEDED(hr = (call)) || (Log("GPU: %s failed (0x%08lx)", #call, (unsigned long)hr), false))   // logs what failed

static HWND wnd;
static RECT mon;                       // the overlay window covers the monitor the cursor is on
static ID3D11Device *dev;
static ID3D11DeviceContext1 *ctx;
static IDCompositionDevice *dcomp;
static IDCompositionTarget *target;
static IDCompositionVisual *visual;
static IDCompositionSurface *surf;     // grows only; frames use its top-left corner
static int surfW, surfH;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *inst, *consts;
static ID3D11BlendState *blend;
static ID3D11SamplerState *samp;
static ID3D11ShaderResourceView *tex;  // the cursor picture
static int texId, fails;

// Each copy is a quad: 4 corners from the vertex id, placed and faded by its instance data (position in px, opacity).
static const char hlsl[] = R"(
cbuffer C : register(b0) { float2 view; float2 size; float2 origin; float2 pad; }
struct V { float2 pos : P; float a : A; uint id : SV_VertexID; };
struct O { float4 pos : SV_Position; float2 uv : UV; float a : A; };
O vs(V v)
{
    O o;
    float2 c = float2(v.id & 1, v.id >> 1);
    float2 px = origin + v.pos + c * size;
    o.pos = float4(px / view * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = c;
    o.a = v.a;
    return o;
}
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 ps(O o) : SV_Target { return t.Sample(s, o.uv) * o.a; }
)";

static void ReleaseAll(void)
{
    Rel(tex); Rel(samp); Rel(blend); Rel(consts); Rel(inst); Rel(layout); Rel(ps); Rel(vs);
    Rel(surf); Rel(visual); Rel(target); Rel(dcomp); Rel(ctx); Rel(dev);
    surfW = surfH = texId = 0;
}

static ID3DBlob *Compile(pD3DCompile compile, const char *entry, const char *profile)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    compile(hlsl, sizeof hlsl - 1, nullptr, nullptr, nullptr, entry, profile, 0, 0, &code, &err);
    if (err) { Log("GPU: shader %s: %s", entry, (const char *)err->GetBufferPointer()); err->Release(); }
    return code;
}

static bool Setup(void)
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ID3D11DeviceContext *c0 = nullptr;
    if (!OK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                              levels, 3, D3D11_SDK_VERSION, &dev, nullptr, &c0))) return false;
    c0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx);
    c0->Release();
    IDXGIDevice *dxgi = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
    if (!ctx || !dxgi || !OK(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void **)&dcomp))) { Rel(dxgi); return false; }
    dxgi->Release();
    if (!OK(dcomp->CreateTargetForHwnd(wnd, TRUE, &target)) || !OK(dcomp->CreateVisual(&visual))) return false;
    target->SetRoot(visual);

    // the shader compiler is only needed for a moment
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = lib ? (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile") : nullptr;
    ID3DBlob *v = compile ? Compile(compile, "vs", "vs_4_0") : nullptr, *p = compile ? Compile(compile, "ps", "ps_4_0") : nullptr;
    D3D11_INPUT_ELEMENT_DESC in[] = {
        { "P", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "A", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 } };
    bool ok = v && p
        && OK(dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs))
        && OK(dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps))
        && OK(dev->CreateInputLayout(in, 2, v->GetBufferPointer(), v->GetBufferSize(), &layout));
    Rel(v); Rel(p);   // the compiled code lives in the compiler's DLL, so release it before unloading that
    if (lib) FreeLibrary(lib);
    if (!ok) return false;

    D3D11_BUFFER_DESC bd = { COPY_BUF * 12, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
    D3D11_BUFFER_DESC cd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0] = { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                           D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };   // premultiplied "over"
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return OK(dev->CreateBuffer(&bd, nullptr, &inst)) && OK(dev->CreateBuffer(&cd, nullptr, &consts))
        && OK(dev->CreateBlendState(&bl, &blend)) && OK(dev->CreateSamplerState(&sd, &samp));
}

static void CreateOverlay(void)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CursorMotionBlurOverlay";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                          wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(wnd, 0, 255, LWA_ALPHA);   // layered + transparent = clicks go through
}

static bool Frame(const Sprite *sp, int sw, int sh, const Copy *c, int n, int x, int y, int w, int h)
{
    if (texId != sp->id)
    {
        Rel(tex);
        D3D11_TEXTURE2D_DESC td = { (UINT)sp->w, (UINT)sp->h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE };
        D3D11_SUBRESOURCE_DATA data = { sp->px, (UINT)sp->w * 4 };
        ID3D11Texture2D *t;
        if (!OK(dev->CreateTexture2D(&td, &data, &t))) return false;
        dev->CreateShaderResourceView(t, nullptr, &tex);
        t->Release();
        texId = sp->id;
    }
    if (!surf || w > surfW || h > surfH)
    {
        Rel(surf);
        surfW = (w + 127) / 128 * 128 > surfW ? (w + 127) / 128 * 128 : surfW;
        surfH = (h + 127) / 128 * 128 > surfH ? (h + 127) / 128 * 128 : surfH;
        if (!OK(dcomp->CreateSurface(surfW, surfH, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surf))) return false;
        visual->SetContent(surf);
    }

    ID3D11Texture2D *rt;
    POINT off;   // where our surface sits inside the texture the compositor hands out
    if (!OK(surf->BeginDraw(nullptr, __uuidof(ID3D11Texture2D), (void **)&rt, &off))) return false;
    D3D11_TEXTURE2D_DESC rd;
    rt->GetDesc(&rd);
    ID3D11RenderTargetView *rtv = nullptr;
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    rt->Release();
    if (!rtv) { surf->EndDraw(); return false; }
    float clear[4] = {};
    D3D11_RECT all = { off.x, off.y, off.x + surfW, off.y + surfH };
    ctx->ClearView(rtv, clear, &all, 1);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(inst, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        float *f = (float *)m.pData;
        for (int i = 0; i < n; i++) { f[3 * i] = (float)c[i].x; f[3 * i + 1] = (float)c[i].y; f[3 * i + 2] = c[i].a; }
        ctx->Unmap(inst, 0);
    }
    if (SUCCEEDED(ctx->Map(consts, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        float k[8] = { (float)rd.Width, (float)rd.Height, (float)sw, (float)sh, (float)off.x, (float)off.y };
        memcpy(m.pData, k, sizeof k);
        ctx->Unmap(consts, 0);
    }
    D3D11_VIEWPORT vp = { 0, 0, (float)rd.Width, (float)rd.Height, 0, 1 };
    UINT stride = 12, zero = 0;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend, nullptr, ~0u);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetVertexBuffers(0, 1, &inst, &stride, &zero);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &consts);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &tex);
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->DrawInstanced(4, n, 0, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release();
    surf->EndDraw();

    // move the picture, not the window: content and position then change in the same compositor frame
    visual->SetOffsetX((float)(x - mon.left));
    visual->SetOffsetY((float)(y - mon.top));
    return OK(dcomp->Commit());
}

BOOL R_Draw(const Sprite *sp, int sw, int sh, const Copy *c, int n, int x, int y, int w, int h)
{
    if (!wnd) CreateOverlay();
    if (!dev)
    {
        if (fails >= 3) return FALSE;   // no usable GPU: give up rather than retry every frame
        if (!Setup()) { ReleaseAll(); fails++; return FALSE; }
    }
    POINT centre = { x + w / 2, y + h / 2 };
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST), &mi);
    if (!EqualRect(&mi.rcMonitor, &mon))
    {
        mon = mi.rcMonitor;
        // one pixel short of the whole monitor: a window covering all of it counts as a fullscreen app (Windows would then mute
        // notifications, and our own fullscreen pause would switch the blur off)
        SetWindowPos(wnd, HWND_TOPMOST, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top - 1, SWP_NOACTIVATE);
    }
    if (!Frame(sp, sw, sh, c, n, x, y, w, h)) { ReleaseAll(); fails++; return FALSE; }   // e.g. the graphics driver was updated: start over
    fails = 0;
    if (!IsWindowVisible(wnd)) SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return TRUE;
}

void R_Hide(void) { if (wnd) ShowWindow(wnd, SW_HIDE); }

void R_Trim(void)   // idle: drop the drawing surface and let the driver free its scratch memory
{
    if (!dev) return;
    Rel(surf);
    surfW = surfH = 0;
    visual->SetContent(nullptr);
    dcomp->Commit();
    ctx->ClearState();
    ctx->Flush();
    IDXGIDevice3 *d3;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice3), (void **)&d3))) { d3->Trim(); d3->Release(); }
}
