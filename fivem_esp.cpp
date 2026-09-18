// ============================================================================
// File: rat_all_in_one.cpp
//
// Build Command (MSVC):
// cl /std:c++20 /EHsc rat_all_in_one.cpp imgui/imgui.cpp imgui/imgui_draw.cpp ^
//    imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_win32.cpp ^
//    imgui/backends/imgui_impl_dx11.cpp /link d3d11.lib dxgi.lib user32.lib gdi32.lib ws2_32.lib
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ws2_32.lib")

// ═════════════════════════════════════════════════════════
// 1. THEME ENGINE
// ═════════════════════════════════════════════════════════
namespace theme {
    inline ImVec4 bg_deep  {0.055f, 0.055f, 0.075f, 1.000f};
    inline ImVec4 bg_panel {0.090f, 0.090f, 0.115f, 1.000f};
    inline ImVec4 bg_card  {0.120f, 0.120f, 0.150f, 1.000f};
    inline ImVec4 accent   {0.350f, 0.750f, 1.000f, 1.000f};
    inline ImVec4 accent2  {0.750f, 0.350f, 1.000f, 1.000f};
    inline ImVec4 success  {0.350f, 1.000f, 0.550f, 1.000f};
    inline ImVec4 danger   {1.000f, 0.300f, 0.400f, 1.000f};
    inline ImVec4 warn     {1.000f, 0.750f, 0.300f, 1.000f};
    inline ImVec4 text     {0.900f, 0.920f, 0.960f, 1.000f};
    inline ImVec4 text_dim {0.550f, 0.580f, 0.650f, 1.000f};

    inline ImU32 col(const ImVec4& c, float a = 1.0f) {
        return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * a});
    }
    inline ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }
    inline void apply() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 12; s.ChildRounding = 10; s.FrameRounding = 8;
        s.PopupRounding = 10; s.ScrollbarRounding = 8; s.GrabRounding = 6; s.TabRounding = 8;
        s.WindowBorderSize = 0; s.FrameBorderSize = 1;
        s.FramePadding = {12, 8}; s.ItemSpacing = {10, 10}; s.WindowPadding = {16, 16};
        
        auto& c = s.Colors;
        c[ImGuiCol_WindowBg]         = bg_deep;
        c[ImGuiCol_ChildBg]          = bg_panel;
        c[ImGuiCol_PopupBg]          = bg_card;
        c[ImGuiCol_Border]           = {1, 1, 1, 0.06f};
        c[ImGuiCol_FrameBg]          = bg_card;
        c[ImGuiCol_FrameBgHovered]   = lerp(bg_card, accent, 0.15f);
        c[ImGuiCol_FrameBgActive]    = lerp(bg_card, accent, 0.30f);
        c[ImGuiCol_Button]           = bg_card;
        c[ImGuiCol_ButtonHovered]    = lerp(bg_card, accent, 0.35f);
        c[ImGuiCol_ButtonActive]     = lerp(bg_card, accent, 0.60f);
        c[ImGuiCol_Header]           = lerp(bg_card, accent, 0.25f);
        c[ImGuiCol_HeaderHovered]    = lerp(bg_card, accent, 0.40f);
        c[ImGuiCol_HeaderActive]     = lerp(bg_card, accent, 0.60f);
        c[ImGuiCol_SliderGrab]       = accent;
        c[ImGuiCol_SliderGrabActive] = accent2;
        c[ImGuiCol_CheckMark]        = accent;
        c[ImGuiCol_Text]             = text;
        c[ImGuiCol_TextDisabled]     = text_dim;
        c[ImGuiCol_Separator]        = {1, 1, 1, 0.08f};
    }
}

// ═════════════════════════════════════════════════════════
// 2. ANIMATION ENGINE
// ═════════════════════════════════════════════════════════
namespace anim {
    inline float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
    inline float ease_out_back(float t) {
        t = clamp01(t); const float c1 = 1.70158f, c3 = c1 + 1;
        return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
    }
    struct Tween {
        float value = 0, target = 0, speed = 8;
        void update(float dt) { value += (target - value) * std::min(1.0f, speed * dt); }
        void set(float t) { target = t; }
        float operator()() const { return value; }
    };
    struct Pulse {
        float t = 0, speed = 3;
        void update(float dt) { t += dt * speed; }
        float value() const { return 0.5f + 0.5f * std::sin(t); }
    };
}

// ═════════════════════════════════════════════════════════
// 3. NETWORK PROTOCOL & CORE SERVER
// ═════════════════════════════════════════════════════════
namespace rat {
    #pragma pack(push, 1)
    struct PacketHeader {
        uint32_t magic  = 0x52415431; // "RAT1"
        uint32_t opcode = 0;
        uint32_t length = 0;
    };
    #pragma pack(pop)

    enum Op : uint32_t { OP_HELLO = 1, OP_BEAT = 2, OP_CMD = 3, OP_RESULT = 4 };

    struct Session {
        int id;
        std::string host;
        std::string ip;
        uint16_t port;
        std::string os;
        bool alive = true;
        float cpu = 0, ram = 0;
        std::deque<std::string> log;
    };

    class Server {
    public:
        Server(uint16_t port) : port_(port) {}
        ~Server() { stop(); }

        bool start() {
            WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return false;
            sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock_ == INVALID_SOCKET) return false;

            int yes = 1;
            setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
            
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(port_);

            if (bind(sock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) return false;
            if (listen(sock_, SOMAXCONN) == SOCKET_ERROR) return false;

            running_ = true;
            accept_thread_ = std::thread([this]{ accept_loop(); });
            return true;
        }

        void stop() {
            running_ = false;
            if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
            if (accept_thread_.joinable()) accept_thread_.join();
            WSACleanup();
        }

        void send_cmd(int session_id, const std::string& cmd) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_cmds_[session_id].push_back(cmd);
        }

        std::vector<Session> snapshot() {
            std::lock_guard<std::mutex> lk(mu_);
            std::vector<Session> out;
            for (auto& [id, s] : sessions_) out.push_back(s);
            return out;
        }

        uint16_t port() const { return port_; }

    private:
        void accept_loop() {
            int next_id = 1;
            while (running_) {
                sockaddr_in cli{}; int cl = sizeof(cli);
                SOCKET c = accept(sock_, (sockaddr*)&cli, &cl);
                if (c == INVALID_SOCKET) break;

                char ipbuf[64]{};
                inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));

                int id = next_id++;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    sessions_[id] = Session{id, "Unknown", ipbuf, ntohs(cli.sin_port), "Windows", true, 0, 0, {}};
                }
                std::thread([this, c, id]{ client_handler(c, id); }).detach();
            }
        }

        void client_handler(SOCKET c, int id) {
            log(id, "[+] Peer Handshake Initiated");
            while (running_) {
                PacketHeader head;
                int n = recv(c, (char*)&head, sizeof(head), 0);
                if (n <= 0) break;

                if (head.magic == 0x52415431) {
                    std::vector<char> body(head.length);
                    if (head.length > 0) {
                        recv(c, body.data(), head.length, 0);
                    }

                    std::lock_guard<std::mutex> lk(mu_);
                    auto& s = sessions_[id];
                    if (head.opcode == OP_HELLO) {
                        std::string info(body.begin(), body.end());
                        s.host = info;
                        s.log.push_back("[i] Registered Host: " + info);
                    } else if (head.opcode == OP_BEAT && body.size() >= 8) {
                        memcpy(&s.cpu, body.data(), 4);
                        memcpy(&s.ram, body.data() + 4, 4);
                    } else if (head.opcode == OP_RESULT) {
                        s.log.push_back("[<] " + std::string(body.begin(), body.end()));
                    }
                }

                // Push Pending Commands
                std::vector<std::string> cmds;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (pending_cmds_[id].size() > 0) {
                        cmds = pending_cmds_[id];
                        pending_cmds_[id].clear();
                    }
                }

                for (auto& cmd : cmds) {
                    PacketHeader h{0x52415431, OP_CMD, (uint32_t)cmd.size()};
                    send(c, (char*)&h, sizeof(h), 0);
                    send(c, cmd.data(), (int)cmd.size(), 0);
                    log(id, "[>] Sent: " + cmd);
                }
            }
            closesocket(c);
            std::lock_guard<std::mutex> lk(mu_);
            sessions_[id].alive = false;
            log(id, "[-] Peer Disconnected");
        }

        void log(int id, const std::string& msg) {
            std::lock_guard<std::mutex> lk(mu_);
            if (sessions_.count(id)) {
                sessions_[id].log.push_back(msg);
                if (sessions_[id].log.size() > 100) sessions_[id].log.pop_front();
            }
        }

        SOCKET sock_ = INVALID_SOCKET;
        uint16_t port_;
        std::atomic<bool> running_{false};
        std::thread accept_thread_;
        std::mutex mu_;
        std::unordered_map<int, Session> sessions_;
        std::unordered_map<int, std::vector<std::string>> pending_cmds_;
    };

    // Agent Simulator for Testing/Lab Purposes
    inline void SpawnLocalAgent(uint16_t port) {
        std::thread([port]{
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{}; a.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            a.sin_port = htons(port);

            if (connect(c, (sockaddr*)&a, sizeof(a)) != SOCKET_ERROR) {
                // HELLO
                std::string hname = "LAB-AGENT-NODE";
                PacketHeader h{0x52415431, OP_HELLO, (uint32_t)hname.size()};
                send(c, (char*)&h, sizeof(h), 0);
                send(c, hname.data(), (int)hname.size(), 0);

                for (int i = 0; i < 50; ++i) {
                    // BEAT
                    float cpu = 0.15f + (rand() % 30) / 100.0f;
                    float ram = 0.45f;
                    char b[8]; memcpy(b, &cpu, 4); memcpy(b+4, &ram, 4);
                    PacketHeader bh{0x52415431, OP_BEAT, 8};
                    send(c, (char*)&bh, sizeof(bh), 0);
                    send(c, b, 8, 0);

                    // RECV CMD
                    u_long avail = 0;
                    ioctlsocket(c, FIONREAD, &avail);
                    if (avail >= sizeof(PacketHeader)) {
                        PacketHeader ch;
                        recv(c, (char*)&ch, sizeof(ch), 0);
                        if (ch.length > 0) {
                            std::vector<char> buf(ch.length);
                            recv(c, buf.data(), ch.length, 0);
                            std::string res = "Executed: " + std::string(buf.begin(), buf.end());
                            PacketHeader rh{0x52415431, OP_RESULT, (uint32_t)res.size()};
                            send(c, (char*)&rh, sizeof(rh), 0);
                            send(c, res.data(), (int)res.size(), 0);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            }
            closesocket(c);
        }).detach();
    }
}

// ═════════════════════════════════════════════════════════
// 4. UI COMPONENTS
// ═════════════════════════════════════════════════════════
namespace ui {
    inline void TextGlow(const char* txt, ImVec4 color, float glow = 1.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImU32 c  = theme::col(color);
        ImU32 cg = theme::col(color, 0.35f * glow);
        for (int i = 0; i < 4; ++i) {
            float a = (i + 1) * 1.2f;
            dl->AddText({p.x - a, p.y}, cg, txt); dl->AddText({p.x + a, p.y}, cg, txt);
            dl->AddText({p.x, p.y - a}, cg, txt); dl->AddText({p.x, p.y + a}, cg, txt);
        }
        dl->AddText(p, c, txt);
        ImGui::Dummy(ImGui::CalcTextSize(txt));
    }

    struct AnimatedButton {
        anim::Tween hover, press;
        bool draw(const char* label, ImVec2 size, ImVec4 accent = theme::accent, float dt = 1.f/60.f) {
            ImGui::PushID(label);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##b", size);
            bool h = ImGui::IsItemHovered();
            bool a = ImGui::IsItemActive();
            hover.set(h ? 1.0f : 0.0f); press.set(a ? 1.0f : 0.0f);
            hover.update(dt); press.update(dt);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            float hv = hover(), pr = press();
            ImVec4 base = theme::lerp(theme::bg_card, accent, hv * 0.35f + pr * 0.25f);
            float sc = 1.0f - pr * 0.03f;
            ImVec2 c{p.x + size.x * 0.5f, p.y + size.y * 0.5f};
            ImVec2 tl{c.x - size.x * 0.5f * sc, c.y - size.y * 0.5f * sc};
            ImVec2 br{c.x + size.x * 0.5f * sc, c.y + size.y * 0.5f * sc};

            dl->AddRectFilled(tl, br, theme::col(base), size.y * 0.5f);
            dl->AddRect(tl, br, theme::col(accent, 0.3f + hv * 0.5f), size.y * 0.5f, 0, 1.5f);
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, theme::col(theme::text), label);
            ImGui::PopID();
            return clicked;
        }
    };

    struct Toast {
        std::string msg; ImVec4 color; float life = 0, max_life = 3.0f; bool alive = true;
        void update(float dt) { life += dt; if (life >= max_life) alive = false; }
        void draw(int idx) {
            float t_in = anim::ease_out_back(std::min(life / 0.35f, 1.0f));
            float alpha = std::min(1.0f, (max_life - life) / 0.4f);
            float slide = (1.0f - t_in) * 60.0f;
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 sz{300, 50};
            ImVec2 p{io.DisplaySize.x - sz.x - 20 + slide, io.DisplaySize.y - 20 - (sz.y + 10) * (idx + 1)};
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(p, {p.x + sz.x, p.y + sz.y}, theme::col(theme::bg_card, alpha), 8);
            dl->AddRect(p, {p.x + sz.x, p.y + sz.y}, theme::col(color, 0.6f * alpha), 8, 0, 1.5f);
            dl->AddText({p.x + 15, p.y + 16}, theme::col(theme::text, alpha), msg.c_str());
        }
    };
}

// ═════════════════════════════════════════════════════════
// 5. APPLICATION CONTROLLER
// ═════════════════════════════════════════════════════════
struct App {
    int page = 0;
    int selected_session = -1;
    ui::AnimatedButton btn_send, btn_spawn;
    anim::Pulse pulse;
    std::vector<ui::Toast> toasts;
    rat::Server server{4444};
    char input_buf[256] = "";

    App() {
        if (server.start()) toast("Server listening on 0.0.0.0:4444", theme::success);
        else toast("Failed to bind socket", theme::danger);
    }

    void toast(const std::string& m, ImVec4 c) { toasts.push_back({m, c, 0, 3.0f, true}); }

    void render(float dt) {
        pulse.update(dt);
        for (auto& t : toasts) t.update(dt);
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [](const ui::Toast& t){ return !t.alive; }), toasts.end());

        // Sidebar
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({200, ImGui::GetIO().DisplaySize.y});
        ImGui::Begin("Sidebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ui::TextGlow("LNW CONTROL", theme::accent, 0.8f);
        ImGui::Separator(); ImGui::Spacing();

        if (ImGui::Selectable("  [1] Overview", page == 0)) page = 0;
        if (ImGui::Selectable("  [2] Sessions", page == 1)) page = 1;
        if (ImGui::Selectable("  [3] Console", page == 2)) page = 2;

        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
        ImGui::TextColored(theme::lerp(theme::success, theme::accent, pulse.value()), "● Port %u", server.port());
        ImGui::End();

        // Main Panel
        ImGui::SetNextWindowPos({200, 0});
        ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x - 200, ImGui::GetIO().DisplaySize.y});
        ImGui::Begin("Content", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

        auto sessions = server.snapshot();

        if (page == 0) { // Dashboard
            ImGui::TextColored(theme::text_dim, "DASHBOARD SYSTEM");
            ImGui::Spacing();
            ImGui::Text("Active Connections: %zu", sessions.size());
            if (btn_spawn.draw("Spawn Test Agent", {160, 36}, theme::accent2, dt)) {
                rat::SpawnLocalAgent(server.port());
                toast("Local Lab Agent Spawned", theme::accent2);
            }
        } else if (page == 1) { // Sessions
            ImGui::TextColored(theme::text_dim, "ACTIVE CONNECTIONS");
            if (ImGui::BeginTable("##sess", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("Host");
                ImGui::TableSetupColumn("IP");
                ImGui::TableSetupColumn("CPU");
                ImGui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();

                for (auto& s : sessions) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", s.id);
                    ImGui::TableSetColumnIndex(1); 
                    if (ImGui::Selectable(s.host.c_str(), selected_session == s.id, ImGuiSelectableFlags_SpanAllColumns))
                        selected_session = s.id;
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%s", s.ip.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", s.cpu * 100);
                    ImGui::TableSetColumnIndex(4); ImGui::TextColored(s.alive ? theme::success : theme::danger, s.alive ? "Online" : "Offline");
                }
                ImGui::EndTable();
            }
        } else if (page == 2) { // Console
            ImGui::TextColored(theme::text_dim, "TARGET COMMAND CENTER");
            ImGui::BeginChild("##log", {0, -50}, true);
            for (auto& s : sessions) {
                if (selected_session == -1 || selected_session == s.id) {
                    for (auto& l : s.log) ImGui::TextUnformatted(l.c_str());
                }
            }
            ImGui::EndChild();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
            bool enter = ImGui::InputText("##in", input_buf, 256, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((btn_send.draw("Execute", {110, 36}, theme::accent, dt) || enter) && input_buf[0] != '\0') {
                if (selected_session != -1) {
                    server.send_cmd(selected_session, input_buf);
                    toast("Command Dispatched", theme::accent);
                } else {
                    toast("No session selected!", theme::danger);
                }
                input_buf[0] = '\0';
            }
        }
        ImGui::End();

        // Render Toasts
        for (size_t i = 0; i < toasts.size(); ++i) toasts[i].draw((int)i);
    }
};

// ═════════════════════════════════════════════════════════
// 6. WIN32 / DIRECTX11 BOILERPLATE
// ═════════════════════════════════════════════════════════
ID3D11Device*            g_pd3dDevice = nullptr;
ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
IDXGISwapChain*          g_pSwapChain = nullptr;
ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"LNWClass", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Control Surface", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    theme::apply();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    App app;
    bool done = false;
    auto last_time = std::chrono::high_resolution_clock::now();

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        auto current_time = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.render(dt);

        ImGui::Render();
        const float clear_color[4] = { 0.055f, 0.055f, 0.075f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
