// ff_headshot_panel.cpp
// Compile: g++ -O2 -std=c++17 ff_headshot_panel.cpp -o ff_headshot_panel.exe -lpsapi
// Run as Administrator

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <conio.h>
#include <iomanip>

#pragma comment(lib, "psapi.lib")

// ============================================================
// CONFIG — ปรับ offset ตามเวอร์ชันเกม
// ============================================================
struct Offsets {
    DWORD localPlayer    = 0x0;  // offset ไปยัง local player pointer
    DWORD entityList     = 0x0;  // offset ไปยัง entity list
    DWORD headBone       = 0x0;  // offset ไปยัง head bone position
    DWORD aimTarget      = 0x0;  // offset ไปยัง aim target
    DWORD spread         = 0x0;  // offset ไปยัง spread value
    DWORD recoil         = 0x0;  // offset ไปยัง recoil value
    DWORD hitbox         = 0x0;  // offset ไปยัง hitbox multiplier
};

// ============================================================
// GLOBAL STATE
// ============================================================
struct CheatState {
    bool  enabled        = false;
    bool  headshotOnly   = true;
    float headshotRate   = 100.0f;  // 0-100%
    float spreadMod      = 0.0f;    // 0 = no spread
    float recoilMod      = 0.0f;    // 0 = no recoil
    float hitboxScale    = 2.5f;    // ขยาย hitbox หัว
    DWORD processId      = 0;
    HANDLE processHandle = nullptr;
    uintptr_t baseAddress = 0;
    Offsets offsets;
};

CheatState g_state;

// ============================================================
// PROCESS UTILS
// ============================================================
DWORD FindProcessId(const std::wstring& processName) {
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

uintptr_t GetModuleBase(DWORD pid, const std::wstring& moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0) {
                base = (uintptr_t)entry.modBaseAddr;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return base;
}

template<typename T>
T ReadMem(uintptr_t address) {
    T value{};
    ReadProcessMemory(g_state.processHandle, (LPCVOID)address, &value, sizeof(T), nullptr);
    return value;
}

template<typename T>
bool WriteMem(uintptr_t address, T value) {
    return WriteProcessMemory(g_state.processHandle, (LPVOID)address, &value, sizeof(T), nullptr);
}

// ============================================================
// CHEAT LOGIC
// ============================================================
uintptr_t GetLocalPlayer() {
    uintptr_t ptr = g_state.baseAddress + g_state.offsets.localPlayer;
    return ReadMem<uintptr_t>(ptr);
}

uintptr_t GetEntityByIndex(int index) {
    uintptr_t listPtr = g_state.baseAddress + g_state.offsets.entityList;
    uintptr_t entityList = ReadMem<uintptr_t>(listPtr);
    if (!entityList) return 0;
    return ReadMem<uintptr_t>(entityList + (index * 0x8));
}

void ApplyHeadshotRate() {
    if (!g_state.enabled) return;
    uintptr_t localPlayer = GetLocalPlayer();
    if (!localPlayer) return;

    // คำนวณ hitbox multiplier จาก rate
    // rate 100% = hitbox หัวขยายเต็มที่
    float rate = g_state.headshotRate / 100.0f;
    float finalScale = 1.0f + (g_state.hitboxScale - 1.0f) * rate;

    WriteMem<float>(localPlayer + g_state.offsets.hitbox, finalScale);
    WriteMem<float>(localPlayer + g_state.offsets.spread, g_state.spreadMod);
    WriteMem<float>(localPlayer + g_state.offsets.recoil, g_state.recoilMod);
}

void ApplyHeadshotOnly() {
    if (!g_state.headshotOnly) return;
    uintptr_t localPlayer = GetLocalPlayer();
    if (!localPlayer) return;

    // บังคับ aim target ไปที่ head bone
    uintptr_t target = ReadMem<uintptr_t>(localPlayer + g_state.offsets.aimTarget);
    if (target) {
        WriteMem<uintptr_t>(localPlayer + g_state.offsets.headBone, target);
    }
}

// ============================================================
// PANEL UI
// ============================================================
void ClearScreen() { system("cls"); }

void PrintHeader() {
    std::cout << "========================================\n";
    std::cout << "   FF HEADSHOT PANEL v1.0 — [L]nw\n";
    std::cout << "   PID: " << g_state.processId 
              << "  Base: 0x" << std::hex << g_state.baseAddress << std::dec << "\n";
    std::cout << "========================================\n";
}

void PrintMenu() {
    std::cout << "\n[1] Toggle Cheat        : " << (g_state.enabled ? "ON" : "OFF") << "\n";
    std::cout << "[2] Headshot Only       : " << (g_state.headshotOnly ? "ON" : "OFF") << "\n";
    std::cout << "[3] Headshot Rate       : " << std::fixed << std::setprecision(0) 
              << g_state.headshotRate << "%\n";
    std::cout << "[4] Spread Mod          : " << std::setprecision(2) << g_state.spreadMod << "\n";
    std::cout << "[5] Recoil Mod          : " << std::setprecision(2) << g_state.recoilMod << "\n";
    std::cout << "[6] Hitbox Scale        : " << std::setprecision(1) << g_state.hitboxScale << "x\n";
    std::cout << "[7] Set All Offsets     : manual\n";
    std::cout << "[8] Attach Process      : re-scan\n";
    std::cout << "[0] Exit\n";
    std::cout << "\nเลือก: ";
}

void SetOffsets() {
    std::cout << "\n--- ตั้งค่า Offsets (hex) ---\n";
    std::cout << "LocalPlayer offset  : 0x"; std::cin >> std::hex >> g_state.offsets.localPlayer;
    std::cout << "EntityList offset   : 0x"; std::cin >> std::hex >> g_state.offsets.entityList;
    std::cout << "HeadBone offset     : 0x"; std::cin >> std::hex >> g_state.offsets.headBone;
    std::cout << "AimTarget offset    : 0x"; std::cin >> std::hex >> g_state.offsets.aimTarget;
    std::cout << "Spread offset       : 0x"; std::cin >> std::hex >> g_state.offsets.spread;
    std::cout << "Recoil offset       : 0x"; std::cin >> std::hex >> g_state.offsets.recoil;
    std::cout << "Hitbox offset       : 0x"; std::cin >> std::hex >> g_state.offsets.hitbox;
    std::cin >> std::dec;
    std::cout << "Offsets updated.\n";
}

// ============================================================
// MAIN LOOP
// ============================================================
int main() {
    SetConsoleTitleW(L"FF Headshot Panel — [L]nw");
    ClearScreen();

    std::wcout << L"[*] Scanning for Free Fire process...\n";
    g_state.processId = FindProcessId(L"FreeFire.exe");
    if (!g_state.processId) g_state.processId = FindProcessId(L"FreeFire MAX.exe");
    
    if (!g_state.processId) {
        std::cout << "[!] Free Fire not found. Run game first.\n";
        std::cout << "Press any key..."; _getch();
        return 1;
    }

    g_state.processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_state.processId);
    if (!g_state.processHandle) {
        std::cout << "[!] OpenProcess failed. Run as Administrator.\n";
        std::cout << "Press any key..."; _getch();
        return 1;
    }

    g_state.baseAddress = GetModuleBase(g_state.processId, L"FreeFire.exe");
    if (!g_state.baseAddress) g_state.baseAddress = GetModuleBase(g_state.processId, L"FreeFire MAX.exe");

    std::cout << "[+] Attached! PID: " << g_state.processId << "\n";
    std::cout << "[+] Base: 0x" << std::hex << g_state.baseAddress << std::dec << "\n";
    std::cout << "[!] ตั้ง offsets ด้วยเมนู [7] ก่อนใช้งาน\n";
    std::cout << "Press any key..."; _getch();

    while (true) {
        ClearScreen();
        PrintHeader();
        PrintMenu();

        int choice;
        std::cin >> choice;

        switch (choice) {
            case 1:
                g_state.enabled = !g_state.enabled;
                std::cout << "Cheat " << (g_state.enabled ? "ENABLED" : "DISABLED") << "\n";
                break;
            case 2:
                g_state.headshotOnly = !g_state.headshotOnly;
                break;
            case 3: {
                std::cout << "ใส่ rate (0-100): ";
                float r; std::cin >> r;
                if (r < 0) r = 0; if (r > 100) r = 100;
                g_state.headshotRate = r;
                break;
            }
            case 4:
                std::cout << "Spread mod (0.0 = none): ";
                std::cin >> g_state.spreadMod;
                break;
            case 5:
                std::cout << "Recoil mod (0.0 = none): ";
                std::cin >> g_state.recoilMod;
                break;
            case 6:
                std::cout << "Hitbox scale (1.0-10.0): ";
                std::cin >> g_state.hitboxScale;
                break;
            case 7:
                SetOffsets();
                break;
            case 8:
                g_state.processId = FindProcessId(L"FreeFire.exe");
                if (!g_state.processId) g_state.processId = FindProcessId(L"FreeFire MAX.exe");
                if (g_state.processId) {
                    if (g_state.processHandle) CloseHandle(g_state.processHandle);
                    g_state.processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_state.processId);
                    g_state.baseAddress = GetModuleBase(g_state.processId, L"FreeFire.exe");
                    if (!g_state.baseAddress) g_state.baseAddress = GetModuleBase(g_state.processId, L"FreeFire MAX.exe");
                    std::cout << "[+] Re-attached PID: " << g_state.processId << "\n";
                }
                break;
            case 0:
                if (g_state.processHandle) CloseHandle(g_state.processHandle);
                return 0;
        }

        if (g_state.enabled && g_state.processHandle) {
            ApplyHeadshotRate();
            ApplyHeadshotOnly();
        }

        Sleep(10); // loop rate ~100Hz
    }
}
