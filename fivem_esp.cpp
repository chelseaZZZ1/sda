// bs5_toolkit.cpp — Bluestacks 5 automation + management toolkit
// compile: g++ -std=c++20 -O2 bs5_toolkit.cpp -o bs5_toolkit -lpsapi -luser32
// target: Windows x64, Bluestacks 5.21+ (Android 13)

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <optional>
#include <filesystem>
#include <unordered_map>
#include <algorithm>

namespace fs = std::filesystem;

// ---------- types ----------
struct InstanceInfo {
    std::string name;
    DWORD pid;
    HWND window;
    std::string adb_port;
    bool running;
};

struct MacroStep {
    enum class Type { Click, Key, Delay, Swipe } type;
    int x, y, x2, y2;
    int key_code;
    int delay_ms;
};

struct MemoryPatch {
    std::string module;
    uintptr_t offset;
    std::vector<uint8_t> original;
    std::vector<uint8_t> patched;
    bool applied;
};

// ---------- config ----------
class Config {
public:
    static std::string bs_install_path() {
        const char* paths[] = {
            "C:\\Program Files\\BlueStacks_nxt\\",
            "C:\\Program Files\\BlueStacks\\",
            "D:\\Program Files\\BlueStacks_nxt\\"
        };
        for (auto p : paths) {
            if (fs::exists(p)) return p;
        }
        return "C:\\Program Files\\BlueStacks_nxt\\";
    }

    static std::string adb_path() {
        return bs_install_path() + "HD-Adb.exe";
    }

    static std::string instances_dir() {
        char* local = nullptr;
        size_t len = 0;
        _dupenv_s(&local, &len, "LOCALAPPDATA");
        std::string base = local ? local : "C:\\";
        free(local);
        return base + "\\BlueStacks_nxt\\Engine\\";
    }
};

// ---------- process utils ----------
class ProcessUtil {
public:
    static std::vector<DWORD> find_by_name(const std::wstring& name) {
        std::vector<DWORD> pids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return pids;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pids;
    }

    static std::optional<HWND> find_window_for_pid(DWORD pid) {
        struct Ctx { DWORD target; HWND found; };
        Ctx ctx{pid, nullptr};

        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            Ctx* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == c->target && IsWindowVisible(hwnd)) {
                c->found = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.found) return ctx.found;
        return std::nullopt;
    }

    static std::optional<DWORD> launch(const std::string& exe,
                                        const std::string& args = "") {
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        std::string cmd = "\"" + exe + "\"";
        if (!args.empty()) cmd += " " + args;

        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, nullptr, &si, &pi)) {
            return std::nullopt;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    static bool kill(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return false;
        bool ok = TerminateProcess(h, 0);
        CloseHandle(h);
        return ok;
    }
};

// ---------- input injection ----------
class InputInjector {
public:
    static void click(HWND hwnd, int x, int y) {
        POINT pt{x, y};
        ClientToScreen(hwnd, &pt);

        SetForegroundWindow(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        SetCursorPos(pt.x, pt.y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }

    static void key(HWND hwnd, int vk_code) {
        SetForegroundWindow(hwnd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        keybd_event(static_cast<BYTE>(vk_code), 0, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        keybd_event(static_cast<BYTE>(vk_code), 0, KEYEVENTF_KEYUP, 0);
    }

    static void swipe(HWND hwnd, int x1, int y1, int x2, int y2, int duration_ms) {
        POINT p1{x1, y1}, p2{x2, y2};
        ClientToScreen(hwnd, &p1);
        ClientToScreen(hwnd, &p2);

        SetForegroundWindow(hwnd);
        SetCursorPos(p1.x, p1.y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);

        int steps = duration_ms / 10;
        for (int i = 1; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps;
            int cx = static_cast<int>(p1.x + (p2.x - p1.x) * t);
            int cy = static_cast<int>(p1.y + (p2.y - p1.y) * t);
            SetCursorPos(cx, cy);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
};

// ---------- macro engine ----------
class MacroEngine {
    std::vector<MacroStep> steps_;
    bool looping_;
    std::atomic<bool> stop_flag_;

public:
    MacroEngine() : looping_(false), stop_flag_(false) {}

    void add_click(int x, int y, int delay = 100) {
        steps_.push_back({MacroStep::Type::Click, x, y, 0, 0, 0, delay});
    }

    void add_key(int vk, int delay = 100) {
        steps_.push_back({MacroStep::Type::Key, 0, 0, 0, 0, vk, delay});
    }

    void add_delay(int ms) {
        steps_.push_back({MacroStep::Type::Delay, 0, 0, 0, 0, 0, ms});
    }

    void add_swipe(int x1, int y1, int x2, int y2, int duration = 300) {
        steps_.push_back({MacroStep::Type::Swipe, x1, y1, x2, y2, 0, duration});
    }

    void clear() { steps_.clear(); }

    void run(HWND hwnd) {
        stop_flag_ = false;
        do {
            for (auto& s : steps_) {
                if (stop_flag_) return;
                switch (s.type) {
                    case MacroStep::Type::Click:
                        InputInjector::click(hwnd, s.x, s.y);
                        break;
                    case MacroStep::Type::Key:
                        InputInjector::key(hwnd, s.key_code);
                        break;
                    case MacroStep::Type::Swipe:
                        InputInjector::swipe(hwnd, s.x, s.y, s.x2, s.y2, s.delay_ms);
                        break;
                    case MacroStep::Type::Delay:
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(s.delay_ms));
                        continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(s.delay_ms));
            }
        } while (looping_ && !stop_flag_);
    }

    void set_looping(bool v) { looping_ = v; }
    void stop() { stop_flag_ = true; }

    bool save(const std::string& path) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        size_t n = steps_.size();
        f.write(reinterpret_cast<const char*>(&n), sizeof(n));
        f.write(reinterpret_cast<const char*>(steps_.data()),
                n * sizeof(MacroStep));
        return true;
    }

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        size_t n;
        f.read(reinterpret_cast<char*>(&n), sizeof(n));
        steps_.resize(n);
        f.read(reinterpret_cast<char*>(steps_.data()), n * sizeof(MacroStep));
        return true;
    }
};

// ---------- memory tools ----------
class MemoryTool {
    HANDLE process_;
    DWORD pid_;

public:
    explicit MemoryTool(DWORD pid) : pid_(pid), process_(nullptr) {
        process_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }

    ~MemoryTool() {
        if (process_) CloseHandle(process_);
    }

    bool valid() const { return process_ != nullptr; }

    std::optional<uintptr_t> get_module_base(const std::string& module) {
        HMODULE mods[1024];
        DWORD needed;
        if (!EnumProcessModules(process_, mods, sizeof(mods), &needed))
            return std::nullopt;

        int count = needed / sizeof(HMODULE);
        for (int i = 0; i < count; ++i) {
            char name[MAX_PATH];
            if (GetModuleBaseNameA(process_, mods[i], name, MAX_PATH)) {
                if (_stricmp(name, module.c_str()) == 0) {
                    return reinterpret_cast<uintptr_t>(mods[i]);
                }
            }
        }
        return std::nullopt;
    }

    template<typename T>
    std::optional<T> read(uintptr_t addr) {
        T value;
        SIZE_T bytes;
        if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(addr),
                                &value, sizeof(T), &bytes))
            return std::nullopt;
        return value;
    }

    template<typename T>
    bool write(uintptr_t addr, const T& value) {
        SIZE_T bytes;
        return WriteProcessMemory(process_, reinterpret_cast<LPVOID>(addr),
                                   &value, sizeof(T), &bytes);
    }

    std::vector<uint8_t> read_bytes(uintptr_t addr, size_t size) {
        std::vector<uint8_t> buf(size);
        SIZE_T bytes;
        ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(addr),
                          buf.data(), size, &bytes);
        return buf;
    }

    bool write_bytes(uintptr_t addr, const std::vector<uint8_t>& data) {
        SIZE_T bytes;
        DWORD old;
        VirtualProtectEx(process_, reinterpret_cast<LPVOID>(addr),
                         data.size(), PAGE_EXECUTE_READWRITE, &old);
        bool ok = WriteProcessMemory(process_, reinterpret_cast<LPVOID>(addr),
                                      data.data(), data.size(), &bytes);
        VirtualProtectEx(process_, reinterpret_cast<LPVOID>(addr),
                         data.size(), old, &old);
        return ok;
    }

    std::optional<uintptr_t> pattern_scan(const std::string& module,
                                           const std::vector<uint8_t>& pattern,
                                           const std::string& mask) {
        auto base = get_module_base(module);
        if (!base) return std::nullopt;

        MODULEINFO info;
        HMODULE hmod = reinterpret_cast<HMODULE>(*base);
        if (!GetModuleInformation(process_, hmod, &info, sizeof(info)))
            return std::nullopt;

        auto buffer = read_bytes(*base, info.SizeOfImage);
        for (size_t i = 0; i <= buffer.size() - pattern.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (mask[j] == 'x' && buffer[i + j] != pattern[j]) {
                    found = false;
                    break;
                }
            }
            if (found) return *base + i;
        }
        return std::nullopt;
    }
};

// ---------- instance manager ----------
class InstanceManager {
    std::vector<InstanceInfo> instances_;
    std::unordered_map<std::string, std::string> configs_;

public:
    void discover() {
        instances_.clear();
        auto pids = ProcessUtil::find_by_name(L"HD-Player.exe");

        for (DWORD pid : pids) {
            InstanceInfo info;
            info.pid = pid;
            info.running = true;

            auto hwnd = ProcessUtil::find_window_for_pid(pid);
            info.window = hwnd.value_or(nullptr);

            char name_buf[MAX_PATH];
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h) {
                DWORD size = MAX_PATH;
                QueryFullProcessImageNameA(h, 0, name_buf, &size);
                CloseHandle(h);
            }
            info.name = "instance_" + std::to_string(pid);

            instances_.push_back(info);
        }
    }

    std::optional<DWORD> launch_instance(const std::string& instance_name) {
        std::string exe = Config::bs_install_path() + "HD-Player.exe";
        std::string args = "--instance " + instance_name;
        return ProcessUtil::launch(exe, args);
    }

    bool stop_instance(const std::string& name) {
        for (auto& i : instances_) {
            if (i.name == name && i.running) {
                return ProcessUtil::kill(i.pid);
            }
        }
        return false;
    }

    const std::vector<InstanceInfo>& all() const { return instances_; }

    std::optional<InstanceInfo> get(const std::string& name) {
        for (auto& i : instances_) {
            if (i.name == name) return i;
        }
        return std::nullopt;
    }
};

// ---------- adb bridge ----------
class AdbBridge {
    std::string adb_;

public:
    AdbBridge() : adb_(Config::adb_path()) {}

    std::string exec(const std::string& cmd) {
        std::string full = "\"" + adb_ + "\" " + cmd + " 2>&1";
        FILE* pipe = _popen(full.c_str(), "r");
        if (!pipe) return "";

        char buf[4096];
        std::string result;
        while (fgets(buf, sizeof(buf), pipe)) {
            result += buf;
        }
        _pclose(pipe);
        return result;
    }

    std::string connect(const std::string& port) {
        return exec("connect 127.0.0.1:" + port);
    }

    std::string shell(const std::string& port, const std::string& cmd) {
        return exec("-s 127.0.0.1:" + port + " shell " + cmd);
    }

    std::string install(const std::string& port, const std::string& apk) {
        return exec("-s 127.0.0.1:" + port + " install -r \"" + apk + "\"");
    }

    std::string screenshot(const std::string& port, const std::string& out) {
        shell(port, "screencap -p /sdcard/screen.png");
        return exec("-s 127.0.0.1:" + port + " pull /sdcard/screen.png \"" + out + "\"");
    }

    std::string tap(const std::string& port, int x, int y) {
        return shell(port, "input tap " + std::to_string(x) + " " + std::to_string(y));
    }

    std::string swipe(const std::string& port, int x1, int y1, int x2, int y2, int ms) {
        return shell(port, "input swipe " + std::to_string(x1) + " " +
                     std::to_string(y1) + " " + std::to_string(x2) + " " +
                     std::to_string(y2) + " " + std::to_string(ms));
    }
};

// ---------- main demo ----------
int main(int argc, char** argv) {
    std::cout << "bs5_toolkit — Bluestacks 5 automation\n";

    InstanceManager mgr;
    mgr.discover();
    std::cout << "found " << mgr.all().size() << " running instance(s)\n";

    for (auto& inst : mgr.all()) {
        std::cout << "  [" << inst.pid << "] " << inst.name
                  << " hwnd=" << inst.window << "\n";
    }

    AdbBridge adb;

    if (argc > 1 && std::string(argv[1]) == "macro") {
        if (mgr.all().empty()) {
            std::cout << "no instance running\n";
            return 1;
        }

        HWND hwnd = mgr.all()[0].window;
        MacroEngine macro;
        macro.add_click(960, 540);
        macro.add_delay(500);
        macro.add_swipe(960, 800, 960, 200, 300);
        macro.add_delay(1000);
        macro.set_looping(true);

        std::cout << "running macro (ctrl+c to stop)\n";
        macro.run(hwnd);
    }

    if (argc > 1 && std::string(argv[1]) == "mem") {
        if (mgr.all().empty()) return 1;
        MemoryTool mem(mgr.all()[0].pid);
        if (!mem.valid()) {
            std::cout << "cannot open process\n";
            return 1;
        }

        auto base = mem.get_module_base("HD-Player.exe");
        if (base) {
            std::cout << "base: 0x" << std::hex << *base << "\n";
        }
    }

    return 0;
}
