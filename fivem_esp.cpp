// rat_all_in_one.cpp
// build: cl /std:c++20 /EHsc rat_all_in_one.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_win32.cpp imgui/backends/imgui_impl_dx11.cpp /link d3d11.lib dxgi.lib user32.lib gdi32.lib ws2_32.lib
// deps: imgui/ (core + backends win32/dx11)

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
#include <optional>
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
// THEME
// ═════════════════════════════════════════════════════════
namespace theme {
    inline ImVec4 bg_deep  {0.055f,0.055f,0.075f,1};
    inline ImVec4 bg_panel {0.090f,0.090f,0.115f,1};
    inline ImVec4 bg_card  {0.120f,0.120f,0.150f,1};
    inline ImVec4 accent   {0.35f,0.75f,1.00f,1};
    inline ImVec4 accent2  {0.75f,0.35f,1.00f,1};
    inline ImVec4 success  {0.35f,1.00f,0.55f,1};
    inline ImVec4 danger   {1.00f,0.30f,0.40f,1};
    inline ImVec4 warn     {1.00f,0.75f,0.30f,1};
    inline ImVec4 text     {0.90f,0.92f,0.96f,1};
    inline ImVec4 text_dim {0.55f,0.58f,0.65f,1};

    inline ImU32 col(const ImVec4& c, float a=1.0f){
        return ImGui::ColorConvertFloat4ToU32({c.x,c.y,c.z,c.w*a});
    }
    inline ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t){
        return {a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t};
    }
    inline void apply(){
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding=12; s.ChildRounding=10; s.FrameRounding=8;
        s.PopupRounding=10; s.ScrollbarRounding=8; s.GrabRounding=6; s.TabRounding=8;
        s.WindowBorderSize=0; s.FrameBorderSize=1;
        s.FramePadding={12,8}; s.ItemSpacing={10,10}; s.WindowPadding={16,16};
        auto& c = s.Colors;
        c[ImGuiCol_WindowBg]=bg_deep; c[ImGuiCol_ChildBg]=bg_panel; c[ImGuiCol_PopupBg]=bg_card;
        c[ImGuiCol_Border]={1,1,1,0.06f};
        c[ImGuiCol_FrameBg]=bg_card;
        c[ImGuiCol_FrameBgHovered]=lerp(bg_card,accent,0.15f);
        c[ImGuiCol_FrameBgActive]=lerp(bg_card,accent,0.30f);
        c[ImGuiCol_Button]=bg_card;
        c[ImGuiCol_ButtonHovered]=lerp(bg_card,accent,0.35f);
        c[ImGuiCol_ButtonActive]=lerp(bg_card,accent,0.60f);
        c[ImGuiCol_Header]=lerp(bg_card,accent,0.25f);
        c[ImGuiCol_HeaderHovered]=lerp(bg_card,accent,0.40f);
        c[ImGuiCol_HeaderActive]=lerp(bg_card,accent,0.60f);
        c[ImGuiCol_SliderGrab]=accent; c[ImGuiCol_SliderGrabActive]=accent2;
        c[ImGuiCol_CheckMark]=accent;
        c[ImGuiCol_Text]=text; c[ImGuiCol_TextDisabled]=text_dim;
        c[ImGuiCol_Separator]={1,1,1,0.08f};
        c[ImGuiCol_Tab]=bg_panel;
        c[ImGuiCol_TabHovered]=lerp(bg_panel,accent,0.35f);
        c[ImGuiCol_TabActive]=lerp(bg_panel,accent,0.55f);
    }
}

// ═════════════════════════════════════════════════════════
// ANIM
// ═════════════════════════════════════════════════════════
namespace anim {
    inline float clamp01(float t){ return t<0?0:t>1?1:t; }
    inline float ease_out_cubic(float t){ t=clamp01(t); float u=1-t; return 1-u*u*u; }
    inline float ease_in_out_cubic(float t){ t=clamp01(t); return t<0.5f?4*t*t*t:1-std::pow(-2*t+2,3)*0.5f; }
    inline float ease_out_back(float t){ t=clamp01(t); const float c1=1.70158f,c3=c1+1; return 1+c3*std::pow(t-1,3)+c1*std::pow(t-1,2); }
    inline float ease_out_elastic(float t){
        t=clamp01(t); if(t==0||t==1) return t;
        const float c4=(2*3.14159265f)/3;
        return std::pow(2.f,-10*t)*std::sin((t*10-0.75f)*c4)+1.f;
    }
    struct Tween {
        float value=0, target=0, speed=8;
        void update(float dt){ value += (target-value)*std::min(1.f, speed*dt); }
        void set(float t){ target=t; }
        void snap(float v){ value=target=v; }
        float operator()() const { return value; }
    };
    struct Pulse {
        float t=0, speed=3;
        void update(float dt){ t+=dt*speed; }
        float value() const { return 0.5f+0.5f*std::sin(t); }
    };
    struct Shake {
        float amp=0, decay=8, t=0;
        void trigger(float a){ amp=a; t=0; }
        void update(float dt){ t+=dt; amp=std::max(0.f, amp-decay*dt); }
        float offset() const { return amp*std::sin(t*60.f); }
    };
}

// ═════════════════════════════════════════════════════════
// RAT CORE — transport + sessions (loopback / LAN lab)
// ═════════════════════════════════════════════════════════
namespace rat {

    struct Packet {
        uint32_t magic   = 0x52415431; // "RAT1"
        uint32_t opcode  = 0;
        uint32_t length  = 0;
        uint32_t seq     = 0;
    };

    enum Op : uint32_t {
        OP_HELLO   = 1,
        OP_BEAT    = 2,
        OP_CMD     = 3,
        OP_RESULT  = 4,
        OP_STREAM  = 5,
        OP_BYE     = 6,
    };

    struct Session {
        int         id;
        std::string host;
        std::string ip;
        uint16_t    port;
        std::string os;
        std::string cc;
        bool        alive = true;
        float       cpu = 0, ram = 0;
        uint32_t    ping_ms = 0;
        std::chrono::steady_clock::time_point last_beat;
        std::deque<std::string> log;
    };

    class Listener {
    public:
        Listener(uint16_t port) : port_(port) {}
        ~Listener(){ stop(); }

        bool start(){
            WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return false;
            sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock_ == INVALID_SOCKET) return false;
            int yes = 1;
            setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port_);
            if (bind(sock_,(sockaddr*)&a,sizeof(a)) == SOCKET_ERROR) return false;
            if (listen(sock_, 16) == SOCKET_ERROR) return false;
            running_ = true;
            th_ = std::thread([this]{ accept_loop(); });
            return true;
        }
        void stop(){
            running_ = false;
            if (sock_ != INVALID_SOCKET){ closesocket(sock_); sock_ = INVALID_SOCKET; }
            if (th_.joinable()) th_.join();
            WSACleanup();
        }

        void send_cmd(int session_id, const std::string& cmd){
            std::lock_guard lk(mu_);
            pending_[session_id].push_back(cmd);
        }

        std::vector<Session> snapshot(){
            std::lock_guard lk(mu_);
            std::vector<Session> out;
            for (auto& [id, s] : sessions_) out.push_back(s);
            return out;
        }

        void push_log(int id, const std::string& line){
            std::lock_guard lk(mu_);
            auto it = sessions_.find(id);
            if (it == sessions_.end()) return;
            it->second.log.push_back(line);
            if (it->second.log.size() > 200) it->second.log.pop_front();
        }

        uint16_t port() const { return port_; }

    private:
        void accept_loop(){
            int next_id = 1;
            while (running_){
                sockaddr_in cli{}; int cl = sizeof(cli);
                SOCKET c = accept(sock_, (sockaddr*)&cli, &cl);
                if (c == INVALID_SOCKET) break;

                char ipbuf[64]{};
                inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));

                Session s{};
                s.id = next_id++;
                s.ip = ipbuf;
                s.port = ntohs(cli.sin_port);
                s.host = "peer-" + std::to_string(s.id);
                s.os = "unknown";
                s.cc = "??";
                s.last_beat = std::chrono::steady_clock::now();
                {
                    std::lock_guard lk(mu_);
                    sessions_[s.id] = s;
                }
                std::thread([this, c, id=s.id]{ serve(c, id); }).detach();
            }
        }

        void serve(SOCKET c, int id){
            push_log(id, "[+] peer connected");
            char buf[4096];
            while (running_){
                int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (n >= (int)sizeof(Packet)){
                    Packet p{};
                    memcpy(&p, buf, sizeof(Packet));
                    if (p.magic == 0x52415431){
                        handle_packet(id, p, buf + sizeof(Packet), n - (int)sizeof(Packet));
                    }
                }
                std::vector<std::string> out;
                {
                    std::lock_guard lk(mu_);
                    auto it = pending_.find(id);
                    if (it != pending_.end() && !it->second.empty()){
                        out.assign(it->second.begin(), it->second.end());
                        it->second.clear();
                    }
                }
                for (auto& cmd : out){
                    Packet p{0x52415431, OP_CMD, (uint32_t)cmd.size(), 0};
                    std::vector<char> wire(sizeof(Packet) + cmd.size());
                    memcpy(wire.data(), &p, sizeof(Packet));
                    memcpy(wire.data()+sizeof(Packet), cmd.data(), cmd.size());
                    send(c, wire.data(), (int)wire.size(), 0);
                    push_log(id, "[>] queued: " + cmd);
                }
            }
            closesocket(c);
            std::lock_guard lk(mu_);
            if (auto it = sessions_.find(id); it != sessions_.end())
                it->second.alive = false;
            push_log(id, "[-] peer disconnected");
        }

        void handle_packet(int id, const Packet& p, const char* body, int blen){
            std::lock_guard lk(mu_);
            auto it = sessions_.find(id);
            if (it == sessions_.end()) return;
            auto& s = it->second;
            s.last_beat = std::chrono::steady_clock::now();
            switch (p.opcode){
                case OP_HELLO: {
                    std::string payload(body, blen);
                    s.host = payload.substr(0, payload.find('|'));
                    auto p2 = payload.find('|');
                    if (p2 != std::string::npos) s.os = payload.substr(p2+1);
                    s.log.push_back("[i] hello from " + s.host + " (" + s.os + ")");
                    break;
                }
                case OP_BEAT: {
                    if (blen >= (int)sizeof(float)*2){
                        float cpu, ram; memcpy(&cpu, body, 4); memcpy(&ram, body+4, 4);
                        s.cpu = cpu; s.ram = ram;
                    }
                    break;
                }
                case OP_RESULT: {
                    s.log.push_back("[<] " + std::string(body, blen));
                    break;
                }
                case OP_BYE: s.alive = false; break;
            }
        }

        SOCKET sock_ = INVALID_SOCKET;
        uint16_t port_;
        std::atomic<bool> running_{false};
        std::thread th_;
        std::mutex mu_;
        std::unordered_map<int, Session> sessions_;
        std::unordered_map<int, std::vector<std::string>> pending_;
    };
}

// ═════════════════════════════════════════════════════════
// UI WIDGETS
// ═════════════════════════════════════════════════════════
namespace ui {

inline void TextGlow(const char* txt, ImVec4 color, float glow=1.f){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImU32 c  = theme::col(color);
    ImU32 cg = theme::col(color, 0.35f*glow);
    for (int i=0;i<6;++i){
        float a=(i+1)*1.2f;
        dl->AddText({p.x-a,p.y},cg,txt); dl->AddText({p.x+a,p.y},cg,txt);
        dl->AddText({p.x,p.y-a},cg,txt); dl->AddText({p.x,p.y+a},cg,txt);
    }
    dl->AddText(p,c,txt);
    ImGui::Dummy(ImGui::CalcTextSize(txt));
}

struct AnimatedButton {
    anim::Tween hover, press;
    anim::Shake shake;
    bool draw(const char* label, ImVec2 size, ImVec4 accent=theme::accent, float dt=1.f/60.f){
        ImGui::PushID(label);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##b", size);
        bool h = ImGui::IsItemHovered();
        bool a = ImGui::IsItemActive();
        hover.set(h?1.f:0.f); press.set(a?1.f:0.f);
        hover.update(dt); press.update(dt); shake.update(dt);
        p.x += shake.offset();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float hv=hover(), pr=press();
        ImVec4 base = theme::lerp(theme::bg_card, accent, hv*0.35f + pr*0.25f);
        float sc = 1.f - pr*0.03f;
        ImVec2 c{p.x+size.x*0.5f, p.y+size.y*0.5f};
        ImVec2 tl{c.x-size.x*0.5f*sc, c.y-size.y*0.5f*sc};
        ImVec2 br{c.x+size.x*0.5f*sc, c.y+size.y*0.5f*sc};
        if (hv>0.01f){
            for (int i=8;i>0;--i){
                float a_ = hv*0.05f*(9-i)/8.f;
                dl->AddRectFilled({tl.x-i,tl.y-i},{br.x+i,br.y+i},
                                  theme::col(accent,a_), size.y*0.5f);
            }
        }
        dl->AddRectFilled(tl,br,theme::col(base),size.y*0.5f);
        dl->AddRect(tl,br,theme::col(accent,0.3f+hv*0.5f),size.y*0.5f,0,1.5f);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText({c.x-ts.x*0.5f, c.y-ts.y*0.5f}, theme::col(theme::text), label);
        ImGui::PopID();
        return clicked && !a;
    }
};

inline void ProgressGlow(const char* label, float pct, ImVec4 color){
    ImGui::TextColored(theme::text_dim, "%s", label);
    ImGui::SameLine(ImGui::GetWindowWidth()-90);
    ImGui::TextColored(color, "%.1f%%", pct*100);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({w, 12});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p,{p.x+w,p.y+8},theme::col(theme::bg_card),4);
    ImVec4 c2 = theme::lerp(color,{1,1,1,1},0.4f);
    float fw = w*pct;
    dl->AddRectFilledMultiColor(p,{p.x+fw,p.y+8},
        theme::col(color),theme::col(c2),theme::col(c2),theme::col(color));
    float t = (float)ImGui::GetTime()*2.f;
    float shine = p.x + fmodf(t,1.5f)/1.5f * w;
    if (shine < p.x+fw){
        dl->AddRectFilledMultiColor({shine-30,p.y},{shine,p.y+8},
            theme::col({1,1,1,0}),theme::col({1,1,1,0.25f}),
            theme::col({1,1,1,0.25f}),theme::col({1,1,1,0}));
    }
}

struct SidebarItem {
    anim::Tween active, hover;
    bool draw(const char* icon, const char* label, bool selected, float dt){
        ImGui::PushID(label);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz = {ImGui::GetContentRegionAvail().x, 44};
        bool clicked = ImGui::InvisibleButton("##sb", sz);
        bool h = ImGui::IsItemHovered();
        hover.set(h?1.f:0.f); active.set(selected?1.f:0.f);
        hover.update(dt); active.update(dt);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float av=active(), hv=hover();
        if (hv>0.01f || av>0.01f){
            ImU32 bg = theme::col(theme::lerp(theme::accent,theme::accent2,av),
                                  0.12f*(0.5f+hv*0.5f+av*0.5f));
            dl->AddRectFilled(p,{p.x+sz.x,p.y+sz.y},bg,8);
        }
        if (av>0.01f){
            float bh = sz.y*0.6f*av;
            float y0 = p.y + (sz.y-bh)*0.5f;
            dl->AddRectFilled({p.x,y0},{p.x+3,y0+bh},
                theme::col(theme::lerp(theme::accent,theme::accent2,av)),2);
        }
        ImVec4 tc = theme::lerp(theme::text_dim, theme::text, std::max(hv,av));
        dl->AddText({p.x+16,p.y+13}, theme::col(tc), icon);
        dl->AddText({p.x+48,p.y+13}, theme::col(tc), label);
        ImGui::PopID();
        return clicked;
    }
};

inline bool BeginCard(const char* id, ImVec2 size, ImVec4 accent=theme::accent){
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg_card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16,16});
    bool open = ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilledMultiColor(p,{p.x+size.x,p.y+2},
        theme::col(accent,0),theme::col(accent,1),
        theme::col(accent,1),theme::col(accent,0));
    return open;
}
inline void EndCard(){
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

struct Toast {
    std::string msg; ImVec4 color; float life=0, max_life=3; bool alive=true;
    void update(float dt){ life+=dt; if (life>=max_life) alive=false; }
    void draw(int idx){
        float t_in  = anim::ease_out_back(std::min(life/0.35f,1.f));
        float t_out = anim::ease_out_cubic(std::min(std::max((life-(max_life-0.4f))/0.4f,0.f),1.f));
        float alpha = 1.f - t_out;
        float slide = (1.f-t_in)*60.f;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 sz{320,56};
        ImVec2 p{io.DisplaySize.x-sz.x-24+slide, io.DisplaySize.y-24-(sz.y+12)*(idx+1)+slide};
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled({p.x+4,p.y+4},{p.x+sz.x+4,p.y+sz.y+4},theme::col({0,0,0,1},0.35f*alpha),10);
        dl->AddRectFilled(p,{p.x+sz.x,p.y+sz.y},theme::col(theme::bg_card,alpha),10);
        dl->AddRect(p,{p.x+sz.x,p.y+sz.y},theme::col(color,0.6f*alpha),10,0,1.5f);
        dl->AddRectFilled(p,{p.x+4,p.y+sz.y},theme::col(color,alpha),10,ImDrawFlags_RoundCornersLeft);
        dl->AddText({p.x+18,p.y+20},theme::col(theme::text,alpha),msg.c_str());
    }
};

} // namespace ui

// ═════════════════════════════════════════════════════════
// APP
// ═════════════════════════════════════════════════════════
struct App {
    int page = 0;
    int selected = -1;
    ui::SidebarItem sidebar[4];
    ui::AnimatedButton btn_refresh, btn_send, btn_labspawn;
    anim::Pulse pulse;
    std::vector<ui::Toast> toasts;
    rat::Listener listener{4444};
    char cmd_input_buf[256] = "";
    std::vector<std::string> console_lines;

    App(){
        if (listener.start()) toast("listener bound to 127.0.0.1:4444", theme::success);
        else                  toast("listener failed to bind", theme::danger);
        console_lines.push_back("[*] ready. sessions appear when peers connect.");
    }
    ~App(){ listener.stop(); }

    void toast(const std::string& m, ImVec4 c){ toasts.push_back({m,c,0,3,true}); }
    void tick_toasts(float dt){
        for (auto& t : toasts) t.update(dt);
        toasts.erase(std::remove_if(toasts.begin(),toasts.end(),
            [](const ui::Toast& t){return !t.alive;}), toasts.end());
    }

    void draw_sidebar(float dt){
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg_panel);
        ImGui::BeginChild("##side", {220,0}, true);
        ImGui::Dummy({0,8});
        ui::TextGlow("LNW", theme::accent, 0.6f);
        ImGui::TextColored(theme::text_dim, "control surface");
        ImGui::Dummy({0,24});

        const char* icons[4] = {"◇","◆","▣","⚙"};
        const char* names[4] = {"dashboard","sessions","console","settings"};
        for (int i=0;i<4;++i){
            if (sidebar[i].draw(icons[i], names[i], page==i, dt))
                page = i;
        }

        ImGui::SetCursorPosY(ImGui::GetWindowHeight()-70);
        ImGui::TextColored(theme::text_dim, "port %u", listener.port());
        float p = pulse.value();
        ImVec4 dot = theme::lerp(theme::success, theme::accent, p);
        ImGui::TextColored(dot, "● listening");

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void draw_dashboard(float dt){
        auto sessions = listener.snapshot();
        int total = (int)sessions.size();
        int online = 0; float avg_cpu=0, avg_ram=0;
        for (auto& s : sessions){ if (s.alive){ online++; avg_cpu+=s.cpu; avg_ram+=s.ram; } }
        if (online){ avg_cpu/=online; avg_ram/=online; }

        ui::BeginCard("stats", {0,120}, theme::accent);
        ImGui::TextColored(theme::text_dim, "OVERVIEW");
        ImGui::Spacing();
        auto stat = [](const char* lbl, const std::string& v, ImVec4 c){
            ImGui::BeginGroup();
            ui::TextGlow(v.c_str(), c, 0.6f);
            ImGui::TextColored(theme::text_dim, "%s", lbl);
            ImGui::EndGroup();
            ImGui::SameLine(0, 60);
        };
        stat("total",   std::to_string(total),        theme::accent);
        stat("online",  std::to_string(online),       theme::success);
        stat("offline", std::to_string(total-online), theme::danger);
        char b[32]; snprintf(b,32,"%.0f%%", avg_cpu*100);
        stat("avg cpu", b, theme::warn);
        ui::EndCard();
        ImGui::Spacing();

        float half = ImGui::GetContentRegionAvail().x*0.5f - 5;
        ui::BeginCard("res", {half, 220}, theme::accent2);
        ImGui::TextColored(theme::text_dim, "AGGREGATE");
        ImGui::Spacing();
        ui::ProgressGlow("CPU", avg_cpu, theme::accent);  ImGui::Spacing();
        ui::ProgressGlow("RAM", avg_ram, theme::success); ImGui::Spacing();
        float net = 0.3f + 0.4f*std::sin((float)ImGui::GetTime()*1.7f);
        ui::ProgressGlow("NET", net, theme::accent2);
        ui::EndCard();

        ImGui::SameLine();

        ui::BeginCard("event", {0, 220}, theme::warn);
        ImGui::TextColored(theme::text_dim, "EVENT LOG");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0,0,0,0.25f});
        ImGui::BeginChild("##ev", {0,0}, true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (auto& s : sessions)
            for (auto& l : s.log)
                ImGui::TextColored(theme::text_dim, "[%d] %s", s.id, l.c_str());
        ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ui::EndCard();
    }

    void draw_sessions(float dt){
        ui::BeginCard("clients", {0,0}, theme::accent);
        ImGui::TextColored(theme::text_dim, "ACTIVE SESSIONS");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 240);
        if (btn_refresh.draw("refresh", {110,32}, theme::accent, dt))
            toast("session list refreshed", theme::accent);
        ImGui::SameLine();
        if (btn_labspawn.draw("lab spawn", {110,32}, theme::accent2, dt))
            toast("spawn a peer on 127.0.0.1:4444 to appear here", theme::accent2);
        ImGui::Spacing();

        auto sessions = listener.snapshot();
        const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##tbl", 6, tf, {0,-1})){
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("HOST");
            ImGui::TableSetupColumn("IP");
            ImGui::TableSetupColumn("OS");
            ImGui::TableSetupColumn("CC", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("STATE", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();

            for (auto& s : sessions){
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", s.id);
                ImGui::TableSetColumnIndex(1);
                bool sel = (selected==s.id);
                if (ImGui::Selectable(s.host.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                    selected = s.id;
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(theme::text_dim,"%s",s.ip.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextColored(theme::text_dim,"%s",s.os.c_str());
                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", s.cc.c_str());
                ImGui::TableSetColumnIndex(5); {
                    float p = pulse.value();
                    ImVec4 c = s.alive ? theme::lerp(theme::success, theme::accent, p) : theme::danger;
                    ImGui::TextColored(c, "%s", s.alive ? "● online" : "○ offline");
                }
            }
            ImGui::EndTable();
        }
        ui::EndCard();
    }

    void draw_console(float dt){
        ui::BeginCard("con", {0,0}, theme::accent2);
        ImGui::TextColored(theme::text_dim, "COMMAND CONSOLE");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0,0,0,0.25f});
        ImGui::BeginChild("##scroll", {0,-50}, true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (auto& l : console_lines)
            ImGui::TextUnformatted(l.c_str());
        ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
        bool enter = ImGui::InputText("##cmd", cmd_input_buf, IM_ARRAYSIZE(cmd_input_buf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool send = btn_send.draw("send", {110,36}, theme::accent, dt);

        if ((enter || send) && cmd_input_buf[0] != '\0'){
            std::string cmd_str(cmd_input_buf);
            if (selected < 0){
                toast("select a session first", theme::danger);
                console_lines.push_back("[!] no session selected");
            } else {
                listener.send_cmd(selected, cmd_str);
                console_lines.push_back("[>] queued to #" + std::to_string(selected) + ": " + cmd_str);
                toast("command queued", theme::accent);
            }
            cmd_input_buf[0] = '\0';
        }
        ui::EndCard();
    }

    void draw_settings(float dt){
        ui::BeginCard("sett", {0,0}, theme::accent);
        ImGui::TextColored(theme::text_dim, "SETTINGS");
        ImGui::Spacing();
        ImGui::Text("Listener Port: %u", listener.port());
        ui::EndCard();
    }

    void render(float dt){
        pulse.update(dt);
        tick_toasts(dt);

        draw_sidebar(dt);
        ImGui::SameLine();

        ImGui::BeginGroup();
        if (page == 0) draw_dashboard(dt);
        else if (page == 1) draw_sessions(dt);
        else if (page == 2) draw_console(dt);
        else if (page == 3) draw_settings(dt);
        ImGui::EndGroup();

        for (int i = 0; i < (int)toasts.size(); ++i) {
            toasts[i].draw(i);
        }
    }
};

// ═════════════════════════════════════════════════════════
// WIN32 & DIRECTX11 MAIN ENTRY
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Class", nullptr };
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

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);
        app.render(dt);
        ImGui::End();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.055f, 0.055f, 0.075f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
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
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
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
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

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
