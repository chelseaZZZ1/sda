// fivem_esp.cpp — FiveM external ESP overlay
// compile: cl /std:c++20 /EHsc /O2 fivem_esp.cpp /link d3d11.lib dxgi.lib d2d1.lib dcomp.lib dwmapi.lib
// รันเป็น external overlay, อ่าน memory จาก GTAProcess.exe
// ใช้ Direct2D + DirectComposition สำหรับ per-pixel alpha, ไม่กระพริบ

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ---------- config ----------
static const wchar_t* kTargetProcess = L"GTAProcess.exe";
static const wchar_t* kWindowClass    = L"FiveM_ESP_Overlay";

// offsets — อัปเดตตาม build b3258
// คำเตือน: FiveM update บ่อย → offset พวกนี้เปลี่ยนทุก patch
// ควรใช้ pattern scan แทน static offset
namespace offsets {
    constexpr uintptr_t World       = 0x25B8A40;
    constexpr uintptr_t LocalPlayer = 0x08;
    constexpr uintptr_t EntityList  = 0x18;
    constexpr uintptr_t EntityCount = 0x20;
    constexpr uintptr_t PedPos      = 0x90;
    constexpr uintptr_t PedHealth   = 0x280;
    constexpr uintptr_t PedArmor    = 0x284;
    constexpr uintptr_t PedName     = 0x2A0;
    constexpr uintptr_t ViewMatrix  = 0x24C1A80;
    constexpr uintptr_t PedType     = 0x10A8;
}

// ---------- globals ----------
static HANDLE g_process = nullptr;
static uintptr_t g_base = 0;
static DWORD g_pid = 0;
static std::atomic<bool> g_running{ true };
static HWND g_overlay = nullptr;

// entity buffer — double buffered กัน data race
static std::mutex g_entity_mutex;
static std::vector<struct EspEntity> g_entities_read;  // renderer อ่าน
static std::vector<struct EspEntity> g_entities_write; // thread เขียน

// ---------- memory ----------
template <typename T>
static bool read_mem(uintptr_t addr, T& out) {
    if (!g_process) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(addr),
                             &out, sizeof(T), &read) && read == sizeof(T);
}

static bool read_bytes(uintptr_t addr, void* buf, size_t size) {
    if (!g_process) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(addr),
                             buf, size, &read) && read == size;
}

static uintptr_t get_module_base(DWORD pid, const wchar_t* module) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, module) == 0) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static DWORD find_process(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool attach_to_process(DWORD pid) {
    g_process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_process) return false;
    g_base = get_module_base(pid, kTargetProcess);
    if (!g_base) {
        CloseHandle(g_process);
        g_process = nullptr;
        return false;
    }
    g_pid = pid;
    return true;
}

static void detach_process() {
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }
    g_base = 0;
    g_pid = 0;
}

// ---------- math ----------
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

static bool world_to_screen(const Vec3& world, const float matrix[16],
                            int w, int h, Vec2& out) {
    float clip_x = world.x * matrix[0] + world.y * matrix[4] + world.z * matrix[8]  + matrix[12];
    float clip_y = world.x * matrix[1] + world.y * matrix[5] + world.z * matrix[9]  + matrix[13];
    float clip_w = world.x * matrix[3] + world.y * matrix[7] + world.z * matrix[11] + matrix[15];

    if (clip_w < 0.01f) return false;

    float ndc_x = clip_x / clip_w;
    float ndc_y = clip_y / clip_w;

    out.x = (w * 0.5f) * (ndc_x + 1.0f);
    out.y = (h * 0.5f) * (1.0f - ndc_y);
    return true;
}

// ---------- esp data ----------
struct EspEntity {
    Vec3 pos;
    float health;
    float armor;
    float distance;
    char name[32];
    bool is_player;
};

static Vec3 g_local_pos{};

// ---------- esp thread ----------
static void esp_thread() {
    while (g_running) {
        // ตรวจสอบว่า process ยังอยู่
        if (g_process) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(g_process, &exit_code) && exit_code != STILL_ACTIVE) {
                detach_process();
            }
        }

        if (!g_process) {
            DWORD pid = find_process(kTargetProcess);
            if (!pid) { Sleep(1000); continue; }
            if (!attach_to_process(pid)) { Sleep(1000); continue; }
        }

        uintptr_t world = 0;
        if (!read_mem(g_base + offsets::World, world) || !world) {
            Sleep(100);
            continue;
        }

        uintptr_t local_ped = 0;
        read_mem(world + offsets::LocalPlayer, local_ped);
        if (local_ped) {
            read_mem(local_ped + offsets::PedPos, g_local_pos);
        }

        uintptr_t entity_list = 0;
        uint32_t entity_count = 0;
        read_mem(world + offsets::EntityList, entity_list);
        read_mem(world + offsets::EntityCount, entity_count);

        g_entities_write.clear();
        g_entities_write.reserve(64);

        if (entity_list && entity_count > 0 && entity_count < 1024) {
            for (uint32_t i = 0; i < entity_count; ++i) {
                uintptr_t ped = 0;
                if (!read_mem(entity_list + i * sizeof(uintptr_t), ped) || !ped) continue;
                if (ped == local_ped) continue;

                EspEntity e{};
                if (!read_mem(ped + offsets::PedPos, e.pos)) continue;
                read_mem(ped + offsets::PedHealth, e.health);
                read_mem(ped + offsets::PedArmor,  e.armor);
                read_bytes(ped + offsets::PedName, e.name, sizeof(e.name));
                e.name[31] = 0;

                int type = 0;
                read_mem(ped + offsets::PedType, type);
                e.is_player = (type == 1);

                float dx = e.pos.x - g_local_pos.x;
                float dy = e.pos.y - g_local_pos.y;
                float dz = e.pos.z - g_local_pos.z;
                e.distance = std::sqrt(dx*dx + dy*dy + dz*dz);

                if (e.distance > 500.0f) continue;
                if (e.health <= 0.0f) continue;

                g_entities_write.push_back(e);
            }
        }

        // swap buffer ใต้ mutex
        {
            std::lock_guard<std::mutex> lock(g_entity_mutex);
            g_entities_read = g_entities_write;
        }

        Sleep(16);
    }
}

// ---------- D2D overlay ----------
class D2DOverlay {
public:
    bool init(HWND hwnd, int width, int height) {
        hwnd_ = hwnd;
        width_ = width;
        height_ = height;

        // 1. D3D11 device
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &d3d_device_, nullptr, &d3d_context_);
        if (FAILED(hr)) return false;

        // 2. DXGI factory
        ComPtr<IDXGIDevice> dxgi_device;
        d3d_device_.As(&dxgi_device);
        ComPtr<IDXGIAdapter> adapter;
        dxgi_device->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));

        // 3. Swap chain for composition — FLIP_DISCARD + PREMULTIPLIED
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width              = width;
        scd.Height             = height;
        scd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count   = 1;
        scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount        = 2;
        scd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode          = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        hr = factory->CreateSwapChainForComposition(
            d3d_device_.Get(), &scd, nullptr, &swap_chain_);
        if (FAILED(hr)) return false;

        // 4. Frame latency
        ComPtr<IDXGISwapChain2> sc2;
        swap_chain_.As(&sc2);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
        sc2->SetMaximumFrameLatency(1);

        // 5. D2D factory + render target
        D2D1_FACTORY_OPTIONS opts = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory1), &opts, &d2d_factory_);
        d2d_factory_.As(&d2d_factory1_);

        ComPtr<IDXGISurface> surface;
        swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2d_factory1_->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &d2d_rt_);

        // 6. Brushes
        d2d_rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red),   &brush_red_);
        d2d_rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_white_);
        d2d_rt_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Lime),  &brush_green_);

        // 7. DirectComposition
        DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&comp_device_));
        comp_device_->CreateTargetForHwnd(hwnd_, TRUE, &comp_target_);
        comp_device_->CreateVisual(&comp_visual_);
        comp_visual_->SetContent(swap_chain_.Get());
        comp_target_->SetRoot(comp_visual_.Get());
        comp_device_->Commit();

        return true;
    }

    void begin_frame() {
        WaitForSingleObject(waitable_, 1000);
        d2d_rt_->BeginDraw();
        d2d_rt_->Clear(D2D1::ColorF(0, 0.0f)); // โปร่งใส
        d2d_rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void draw_rect(float x, float y, float w, float h,
                   ID2D1SolidColorBrush* brush, float thickness = 1.0f) {
        d2d_rt_->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush, thickness);
    }

    void fill_rect(float x, float y, float w, float h,
                   ID2D1SolidColorBrush* brush) {
        d2d_rt_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
    }

    void draw_text(float x, float y, const wchar_t* text,
                   ID2D1SolidColorBrush* brush) {
        if (!text_format_) return;
        d2d_rt_->DrawTextW(text, (UINT32)wcslen(text), text_format_,
                           D2D1::RectF(x, y, x + 300.0f, y + 20.0f), brush);
    }

    void end_frame() {
        d2d_rt_->EndDraw();
        swap_chain_->Present(1, 0); // vsync
    }

    ID2D1SolidColorBrush* brush_red()   { return brush_red_.Get(); }
    ID2D1SolidColorBrush* brush_white() { return brush_white_.Get(); }
    ID2D1SolidColorBrush* brush_green() { return brush_green_.Get(); }

    ~D2DOverlay() {
        if (waitable_) CloseHandle(waitable_);
    }

private:
    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;

    ComPtr<ID3D11Device>        d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<IDXGISwapChain1>     swap_chain_;
    ComPtr<ID2D1Factory>        d2d_factory_;
    ComPtr<ID2D1Factory1>       d2d_factory1_;
    ComPtr<ID2D1RenderTarget>   d2d_rt_;
    ComPtr<ID2D1SolidColorBrush> brush_red_;
    ComPtr<ID2D1SolidColorBrush> brush_white_;
    ComPtr<ID2D1SolidColorBrush> brush_green_;
    ComPtr<IDWriteTextFormat>   text_format_;

    ComPtr<IDCompositionDevice>  comp_device_;
    ComPtr<IDCompositionTarget>  comp_target_;
    ComPtr<IDCompositionVisual>  comp_visual_;

    HANDLE waitable_ = nullptr;
};

// ---------- render ----------
static void render_frame(D2DOverlay& overlay, int w, int h) {
    float matrix[16]{};
    if (g_base) {
        read_bytes(g_base + offsets::ViewMatrix, matrix, sizeof(matrix));
    }

    // copy entities ใต้ mutex
    std::vector<EspEntity> entities;
    {
        std::lock_guard<std::mutex> lock(g_entity_mutex);
        entities = g_entities_read;
    }

    overlay.begin_frame();

    for (const auto& e : entities) {
        Vec2 screen{};
        if (!world_to_screen(e.pos, matrix, w, h, screen)) continue;

        float box_h = 1200.0f / e.distance;
        float box_w = box_h * 0.5f;
        if (box_h < 4.0f) continue;

        auto* brush = e.is_player ? overlay.brush_red() : overlay.brush_white();

        // box
        overlay.draw_rect(
            screen.x - box_w * 0.5f,
            screen.y - box_h,
            box_w,
            box_h,
            brush, 1.0f);

        // health bar
        float hp = e.health > 200.0f ? 200.0f : e.health;
        if (hp < 0) hp = 0;
        float bar_h = box_h * (hp / 200.0f);
        overlay.fill_rect(
            screen.x - box_w * 0.5f - 5.0f,
            screen.y - bar_h,
            3.0f,
            bar_h,
            overlay.brush_green());

        // name + distance
        wchar_t label[64];
        const char* nm = e.name[0] ? e.name : (e.is_player ? "player" : "ped");
        swprintf(label, 64, L"%hs [%.0fm]", nm, e.distance);
        overlay.draw_text(
            screen.x - box_w * 0.5f,
            screen.y - box_h - 16.0f,
            label,
            overlay.brush_white());
    }

    overlay.end_frame();
}

// ---------- window ----------
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // สำคัญ — ห้าม Windows ลบพื้นหลัง
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // D2D จัดการเอง
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kWindowClass, L"esp", WS_POPUP,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);

    if (!g_overlay) return 1;

    ShowWindow(g_overlay, SW_SHOW);

    // init D2D overlay
    D2DOverlay overlay;
    if (!overlay.init(g_overlay, sw, sh)) {
        MessageBoxW(nullptr, L"D2D init failed", L"err", MB_OK);
        return 1;
    }

    std::thread esp(esp_thread);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        render_frame(overlay, sw, sh);
    }

    g_running = false;
    esp.join();
    detach_process();

    DestroyWindow(g_overlay);
    return 0;
}
