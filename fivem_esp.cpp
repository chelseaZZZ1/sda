// fivem_esp.cpp - FiveM ESP overlay
// compile: cl /std:c++20 /EHsc /O2 fivem_esp.cpp /link d3d11.lib dxgi.lib
// รันเป็น external overlay, อ่าน memory จาก GTAProcess.exe

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---------- config ----------
static const wchar_t* kTargetProcess = L"GTAProcess.exe";
static const wchar_t* kWindowClass    = L"FiveM_ESP_Overlay";

// offsets — อัปเดตตาม build b3258
namespace offsets {
    constexpr uintptr_t World     = 0x25B8A40; // pointer to world
    constexpr uintptr_t LocalPlayer = 0x08;    // world + 0x08 -> local ped
    constexpr uintptr_t EntityList  = 0x18;    // world + 0x18 -> entity list
    constexpr uintptr_t EntityCount = 0x20;    // world + 0x20 -> count
    constexpr uintptr_t PedPos      = 0x90;    // ped + 0x90 -> vec3
    constexpr uintptr_t PedHealth   = 0x280;   // ped + 0x280 -> float
    constexpr uintptr_t PedArmor    = 0x284;   // ped + 0x284 -> float
    constexpr uintptr_t PedName     = 0x2A0;   // ped + 0x2A0 -> char[32]
    constexpr uintptr_t ViewMatrix  = 0x24C1A80; // view matrix base
    constexpr uintptr_t PedType     = 0x10A8;  // ped + 0x10A8 -> int (1=player)
}

// ---------- globals ----------
static HANDLE g_process = nullptr;
static uintptr_t g_base = 0;
static std::atomic<bool> g_running{ true };
static HWND g_overlay = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

// ---------- memory ----------
template <typename T>
static bool read_mem(uintptr_t addr, T& out) {
    SIZE_T read = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &read) && read == sizeof(T);
}

static bool read_bytes(uintptr_t addr, void* buf, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(addr), buf, size, &read) && read == size;
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

// ---------- math ----------
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

static bool world_to_screen(const Vec3& world, const float matrix[16], int w, int h, Vec2& out) {
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

static std::vector<EspEntity> g_entities;
static Vec3 g_local_pos{};

// ---------- esp thread ----------
static void esp_thread() {
    while (g_running) {
        DWORD pid = find_process(kTargetProcess);
        if (!pid) { Sleep(1000); continue; }

        if (!g_process) {
            g_process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!g_process) { Sleep(1000); continue; }
            g_base = get_module_base(pid, kTargetProcess);
            if (!g_base) { CloseHandle(g_process); g_process = nullptr; Sleep(1000); continue; }
        }

        uintptr_t world = 0;
        if (!read_mem(g_base + offsets::World, world) || !world) {
            Sleep(100); continue;
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

        std::vector<EspEntity> fresh;
        fresh.reserve(64);

        if (entity_list && entity_count > 0 && entity_count < 1024) {
            for (uint32_t i = 0; i < entity_count; ++i) {
                uintptr_t ped = 0;
                if (!read_mem(entity_list + i * sizeof(uintptr_t), ped) || !ped) continue;
                if (ped == local_ped) continue;

                EspEntity e{};
                if (!read_mem(ped + offsets::PedPos, e.pos)) continue;
                read_mem(ped + offsets::PedHealth, e.health);
                read_mem(ped + offsets::PedArmor, e.armor);
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

                fresh.push_back(e);
            }
        }

        g_entities = std::move(fresh);
        Sleep(16);
    }
}

// ---------- overlay ----------
static bool create_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap, &g_device, &fl, &g_context);
    if (FAILED(hr)) return false;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    return g_rtv != nullptr;
}

static void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    // ใช้ swap chain เป็น overlay — วาดผ่าน GDI fallback ถ้าไม่มี ImGui
    // ตัวอย่างนี้ใช้ GDI บน layered window แทน เพื่อความเรียบง่าย
    (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a;
}

// ---------- gdi overlay (ทางเลือกที่เบา) ----------
static void gdi_render(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    // clear
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    float matrix[16]{};
    if (g_base) {
        read_bytes(g_base + offsets::ViewMatrix, matrix, sizeof(matrix));
    }

    HPEN pen_red   = CreatePen(PS_SOLID, 1, RGB(255, 60, 60));
    HPEN pen_white = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN pen_green = CreatePen(PS_SOLID, 1, RGB(60, 255, 60));
    HPEN pen_old = (HPEN)SelectObject(hdc, pen_red);
    SetBkMode(hdc, TRANSPARENT);

    for (const auto& e : g_entities) {
        Vec2 screen{};
        if (!world_to_screen(e.pos, matrix, w, h, screen)) continue;

        float box_h = 1200.0f / e.distance;
        float box_w = box_h * 0.5f;
        if (box_h < 4.0f) continue;

        SelectObject(hdc, e.is_player ? pen_red : pen_white);

        // box
        Rectangle(hdc,
            (int)(screen.x - box_w * 0.5f),
            (int)(screen.y - box_h),
            (int)(screen.x + box_w * 0.5f),
            (int)(screen.y));

        // health bar
        float hp = e.health > 200.0f ? 200.0f : e.health;
        if (hp < 0) hp = 0;
        int bar_h = (int)(box_h * (hp / 200.0f));
        SelectObject(hdc, pen_green);
        Rectangle(hdc,
            (int)(screen.x - box_w * 0.5f - 5),
            (int)(screen.y - bar_h),
            (int)(screen.x - box_w * 0.5f - 2),
            (int)(screen.y));

        // name + distance
        char label[64];
        snprintf(label, sizeof(label), "%s [%.0fm]",
                 e.name[0] ? e.name : (e.is_player ? "player" : "ped"),
                 e.distance);
        SelectObject(hdc, pen_white);
        TextOutA(hdc, (int)(screen.x - box_w * 0.5f), (int)(screen.y - box_h - 16),
                 label, (int)strlen(label));
    }

    SelectObject(hdc, pen_old);
    DeleteObject(pen_red);
    DeleteObject(pen_white);
    DeleteObject(pen_green);
    ReleaseDC(hwnd, hdc);
}

// ---------- window ----------
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
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
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        kWindowClass, L"esp", WS_POPUP,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);

    SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(g_overlay, SW_SHOW);

    std::thread esp(esp_thread);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        gdi_render(g_overlay);
        Sleep(16);
    }

    esp.join();
    if (g_process) CloseHandle(g_process);
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    return 0;
}
