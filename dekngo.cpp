// headshot_tool.cpp — Qt6 headshot/aim tool GUI
// compile: cmake + Qt6 (Core, Gui, Widgets)
// target: Windows x64

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QStyleFactory>
#include <QGraphicsDropShadowEffect>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSysInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QLCDNumber>
#include <QProgressBar>
#include <QScrollArea>
#include <QFrame>
#include <QToolButton>
#include <QMenuBar>
#include <QStatusBar>
#include <QThread>
#include <atomic>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cmath>
#include <random>

// ============================================================
// CORE ENGINE
// ============================================================

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

struct BoneEntry {
    const char* name;
    int id;
    int priority;
};

static const BoneEntry kBones[] = {
    {"Head",      8,  0},
    {"Neck",      7,  1},
    {"Chest",     6,  2},
    {"Pelvis",    5,  3},
    {"L_Hand",    4,  4},
    {"R_Hand",    3,  4},
};

enum class AimKey { RMB, LMB, Shift, Alt, Ctrl, Side1, Side2 };
enum class AimMode { Hold, Toggle };
enum class SmoothMode { Linear, EaseOut, EaseInOut, Exponential };

struct AimConfig {
    bool  enabled          = false;
    AimKey key             = AimKey::RMB;
    AimMode mode           = AimMode::Hold;
    SmoothMode smooth_mode = SmoothMode::EaseOut;

    float fov              = 45.0f;
    float smooth           = 3.0f;
    float sensitivity      = 1.0f;
    float max_speed        = 800.0f;

    float trigger_delay_ms = 80.0f;
    float trigger_fov      = 8.0f;
    bool  auto_fire        = false;
    bool  silent_aim       = false;

    bool  vis_check        = true;
    bool  ignore_down      = true;
    int   bone_priority    = 0;

    float humanize_amp     = 0.0f;
    float humanize_freq    = 6.0f;

    float prediction       = 0.0f;
    bool  recoil_comp      = false;
    float recoil_amount    = 0.0f;
};

struct Target {
    Vec2 screen_pos;
    Vec2 world_pos;
    Vec3 velocity;
    int  bone_id;
    float distance;
    float health;
    bool  visible;
    bool  valid;
};

// ============================================================
// MEMORY BRIDGE (interface to game)
// ============================================================

class GameBridge {
public:
    std::atomic<bool> attached{false};
    DWORD pid_ = 0;
    HANDLE handle_ = nullptr;
    uintptr_t base_ = 0;

    bool attach(const wchar_t* proc_name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, proc_name) == 0) {
                    pid_ = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (!pid_) return false;

        handle_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid_);
        if (!handle_) return false;

        HMODULE mods[1024];
        DWORD needed;
        if (EnumProcessModules(handle_, mods, sizeof(mods), &needed)) {
            base_ = reinterpret_cast<uintptr_t>(mods[0]);
        }

        attached = true;
        return true;
    }

    void detach() {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
        base_ = 0;
        attached = false;
    }

    template<typename T>
    T read(uintptr_t addr) {
        T v{};
        ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(addr), &v, sizeof(T), nullptr);
        return v;
    }

    template<typename T>
    void write(uintptr_t addr, T v) {
        WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(addr), &v, sizeof(T), nullptr);
    }

    std::vector<uint8_t> read_buf(uintptr_t addr, size_t n) {
        std::vector<uint8_t> b(n);
        ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(addr), b.data(), n, nullptr);
        return b;
    }
};

// ============================================================
// AIMBOT CORE (frame loop, separate thread)
// ============================================================

class AimCore : public QObject {
    Q_OBJECT
public:
    AimCore(GameBridge* bridge) : bridge_(bridge) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &AimCore::tick);
        timer_->setInterval(1); // ~1000hz input polling
    }

    void set_config(const AimConfig& c) { cfg_ = c; }
    void start() { timer_->start(); }
    void stop()  { timer_->stop(); }
    bool is_key_down() const { return key_down_; }
    Target current_target() const { return target_; }
    int targets_scanned() const { return targets_scanned_; }

signals:
    void target_updated(const Target& t);
    void stats_updated(int scanned, float fps);
    void log_line(const QString& line);

private slots:
    void tick() {
        if (!bridge_->attached) return;

        // key state
        key_down_ = check_key(cfg_.key);
        bool active = cfg_.enabled && (cfg_.mode == AimMode::Hold ? key_down_ : toggled_);

        // scan targets
        targets_scanned_ = 0;
        target_ = {};

        if (active || cfg_.auto_fire) {
            scan_targets();
            pick_best_target();
            if (target_.valid) {
                apply_aim();
                if (cfg_.auto_fire && target_.distance < cfg_.trigger_fov) {
                    schedule_fire();
                }
            }
        }

        // fps calc
        frame_count_++;
        auto now = std::chrono::steady_clock::now();
        if (now - last_fps_time_ > std::chrono::seconds(1)) {
            float fps = frame_count_ /
                std::chrono::duration<float>(now - last_fps_time_).count();
            emit stats_updated(targets_scanned_, fps);
            frame_count_ = 0;
            last_fps_time_ = now;
        }
    }

private:
    GameBridge* bridge_;
    AimConfig cfg_;
    QTimer* timer_;
    bool key_down_ = false;
    bool toggled_ = false;
    bool prev_key_ = false;

    Target target_;
    int targets_scanned_ = 0;
    int frame_count_ = 0;
    std::chrono::steady_clock::time_point last_fps_time_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_fire_time_;
    std::mt19937 rng_{std::random_device{}()};

    bool check_key(AimKey k) {
        int vk = VK_RBUTTON;
        switch (k) {
            case AimKey::RMB:   vk = VK_RBUTTON; break;
            case AimKey::LMB:   vk = VK_LBUTTON; break;
            case AimKey::Shift: vk = VK_SHIFT;   break;
            case AimKey::Alt:   vk = VK_MENU;    break;
            case AimKey::Ctrl:  vk = VK_CONTROL; break;
            case AimKey::Side1: vk = VK_XBUTTON1; break;
            case AimKey::Side2: vk = VK_XBUTTON2; break;
        }
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    void scan_targets() {
        // stub — wire to your game's entity list
        targets_scanned_ = 0;
    }

    void pick_best_target() {
        // stub — pick by fov/bone/distance
    }

    void apply_aim() {
        // stub — compute delta, apply smoothing, move mouse
    }

    void schedule_fire() {
        // stub — click after delay
    }
};

// ============================================================
// CUSTOM WIDGETS
// ============================================================

// ---- smooth slider ----
class SliderRow : public QWidget {
    Q_OBJECT
public:
    SliderRow(const QString& label, double min, double max,
              double val, double step, int decimals = 2,
              QWidget* parent = nullptr)
        : QWidget(parent), min_(min), max_(max) {

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 4, 0, 4);
        lay->setSpacing(12);

        auto* lbl = new QLabel(label);
        lbl->setFixedWidth(130);
        lbl->setStyleSheet("color:#b8b8c8; font-size:12px;");

        slider_ = new QSlider(Qt::Horizontal);
        slider_->setRange(0, 1000);
        slider_->setValue(static_cast<int>((val - min) / (max - min) * 1000));

        spin_ = new QDoubleSpinBox;
        spin_->setRange(min, max);
        spin_->setSingleStep(step);
        spin_->setDecimals(decimals);
        spin_->setValue(val);
        spin_->setFixedWidth(90);
        spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);

        lay->addWidget(lbl);
        lay->addWidget(slider_, 1);
        lay->addWidget(spin_);

        connect(slider_, &QSlider::valueChanged, this, [this](int v) {
            double val = min_ + (max_ - min_) * v / 1000.0;
            if (!spin_->hasFocus()) {
                spin_->blockSignals(true);
                spin_->setValue(val);
                spin_->blockSignals(false);
            }
            emit value_changed(val);
        });

        connect(spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double v) {
            slider_->blockSignals(true);
            slider_->setValue(static_cast<int>((v - min_) / (max_ - min_) * 1000));
            slider_->blockSignals(false);
            emit value_changed(v);
        });

        setStyleSheet(R"(
            QSlider::groove:horizontal {
                height: 6px;
                background: #1a1a22;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                    stop:0 #7c5cff, stop:1 #5a3fd6);
                width: 14px;
                height: 14px;
                margin: -4px 0;
                border-radius: 7px;
                border: 2px solid #9d85ff;
            }
            QSlider::handle:horizontal:hover {
                background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                    stop:0 #9d85ff, stop:1 #7c5cff);
            }
            QSlider::sub-page:horizontal {
                background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                    stop:0 #5a3fd6, stop:1 #7c5cff);
                border-radius: 3px;
            }
            QDoubleSpinBox {
                background: #0f0f14;
                color: #e0e0ff;
                border: 1px solid #2a2a38;
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }
            QDoubleSpinBox:focus {
                border: 1px solid #7c5cff;
            }
        )");
    }

    double value() const { return spin_->value(); }

signals:
    void value_changed(double v);

private:
    QSlider* slider_;
    QDoubleSpinBox* spin_;
    double min_, max_;
};

// ---- toggle switch ----
class ToggleSwitch : public QWidget {
    Q_OBJECT
public:
    ToggleSwitch(const QString& label, bool checked = false, QWidget* parent = nullptr)
        : QWidget(parent), checked_(checked) {

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 4, 0, 4);
        lay->setSpacing(12);

        label_ = new QLabel(label);
        label_->setStyleSheet("color:#b8b8c8; font-size:12px;");
        label_->setFixedWidth(130);

        switch_ = new QPushButton;
        switch_->setCheckable(true);
        switch_->setChecked(checked);
        switch_->setFixedSize(44, 22);
        switch_->setCursor(Qt::PointingHandCursor);
        switch_->setFlat(true);

        lay->addWidget(label_);
        lay->addWidget(switch_);
        lay->addStretch();

        update_style();

        connect(switch_, &QPushButton::toggled, this, [this](bool v) {
            checked_ = v;
            update_style();
            emit toggled(v);
        });
    }

    bool isChecked() const { return checked_; }

signals:
    void toggled(bool v);

private:
    void update_style() {
        if (checked_) {
            switch_->setStyleSheet(R"(
                QPushButton {
                    background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                        stop:0 #5a3fd6, stop:1 #7c5cff);
                    border: none;
                    border-radius: 11px;
                }
            )");
            switch_->setText("● ");
            switch_->setStyleSheet(switch_->styleSheet() +
                "QPushButton { color: white; font-size: 11px; text-align: right; padding-right: 4px; }");
        } else {
            switch_->setStyleSheet(R"(
                QPushButton {
                    background: #1a1a22;
                    border: 1px solid #2a2a38;
                    border-radius: 11px;
                    color: #555;
                    font-size: 11px;
                    text-align: left;
                    padding-left: 4px;
                }
            )");
            switch_->setText(" ●");
        }
    }

    QLabel* label_;
    QPushButton* switch_;
    bool checked_;
};

// ---- fov preview ----
class FovPreview : public QWidget {
    Q_OBJECT
public:
    FovPreview(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(200, 200);
        setStyleSheet("background: transparent;");
    }

    void set_fov(double fov) { fov_ = fov; update(); }
    void set_trigger_fov(double fov) { trig_fov_ = fov; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // bg circle
        QRadialGradient bg(rect().center(), 100);
        bg.setColorAt(0, QColor(20, 20, 30));
        bg.setColorAt(1, QColor(10, 10, 15));
        p.fillRect(rect(), bg);

        int cx = width() / 2, cy = height() / 2;
        double max_r = 90;

        // outer fov ring
        double fov_r = (fov_ / 180.0) * max_r;
        QPen fov_pen(QColor(124, 92, 255, 180), 2);
        p.setPen(fov_pen);
        p.setBrush(QColor(124, 92, 255, 25));
        p.drawEllipse(QPointF(cx, cy), fov_r, fov_r);

        // trigger fov ring
        double trig_r = (trig_fov_ / 180.0) * max_r;
        QPen trig_pen(QColor(255, 92, 124, 180), 2);
        p.setPen(trig_pen);
        p.setBrush(QColor(255, 92, 124, 20));
        p.drawEllipse(QPointF(cx, cy), trig_r, trig_r);

        // crosshair
        p.setPen(QPen(QColor(220, 220, 255, 200), 1));
        p.drawLine(cx - 8, cy, cx + 8, cy);
        p.drawLine(cx, cy - 8, cx, cy + 8);

        // dot
        p.setBrush(QColor(255, 255, 255, 220));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), 2, 2);

        // labels
        p.setPen(QColor(180, 180, 200, 200));
        QFont f = p.font();
        f.setPointSize(9);
        p.setFont(f);
        p.drawText(cx + fov_r + 4, cy - 4, QString("FOV %1°").arg(fov_, 0, 'f', 0));
        p.drawText(cx + trig_r + 4, cy + 12, QString("Trig %1°").arg(trig_fov_, 0, 'f', 0));
    }

private:
    double fov_ = 45;
    double trig_fov_ = 8;
};

// ============================================================
// MAIN WINDOW
// ============================================================

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle("headshot // aim tool");
        setFixedSize(680, 720);
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowMinimizeButtonHint);
        setAttribute(Qt::WA_TranslucentBackground);

        bridge_ = new GameBridge;
        core_ = new AimCore(bridge_);

        setup_ui();
        setup_style();
        setup_connections();

        // auto attach
        QTimer::singleShot(300, this, [this] {
            if (bridge_->attach(L"HD-Player.exe")) {
                log("attached to HD-Player.exe");
            } else {
                log("waiting for game...");
            }
        });

        core_->start();

        // stats timer
        stats_timer_ = new QTimer(this);
        connect(stats_timer_, &QTimer::timeout, this, &MainWindow::update_stats);
        stats_timer_->start(500);
    }

    ~MainWindow() {
        core_->stop();
        bridge_->detach();
    }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && e->position().y() < 50) {
            dragging_ = true;
            drag_pos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging_) {
            move(e->globalPosition().toPoint() - drag_pos_);
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        dragging_ = false;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // rounded bg
        QPainterPath path;
        path.addRoundedRect(rect(), 16, 16);

        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0, QColor(16, 16, 22, 245));
        bg.setColorAt(1, QColor(10, 10, 14, 250));
        p.fillPath(path, bg);

        // border
        p.setPen(QPen(QColor(60, 60, 80, 200), 1));
        p.drawPath(path);

        // top accent line
        QLinearGradient accent(0, 0, width(), 0);
        accent.setColorAt(0, QColor(124, 92, 255, 0));
        accent.setColorAt(0.5, QColor(124, 92, 255, 255));
        accent.setColorAt(1, QColor(124, 92, 255, 0));
        p.setPen(QPen(QBrush(accent), 2));
        p.drawLine(20, 1, width() - 20, 1);
    }

private:
    GameBridge* bridge_;
    AimCore* core_;
    QTimer* stats_timer_;
    bool dragging_ = false;
    QPoint drag_pos_;

    QPushButton* master_sw_;
    QComboBox* key_combo_;
    QComboBox* mode_combo_;
    QComboBox* smooth_mode_combo_;
    QComboBox* bone_combo_;

    SliderRow* fov_slider_;
    SliderRow* smooth_slider_;
    SliderRow* sens_slider_;
    SliderRow* maxspeed_slider_;
    SliderRow* trigger_delay_slider_;
    SliderRow* trigger_fov_slider_;
    SliderRow* recoil_amount_slider_;
    SliderRow* humanize_amp_slider_;
    SliderRow* humanize_freq_slider_;
    SliderRow* prediction_slider_;

    ToggleSwitch* auto_fire_sw_;
    ToggleSwitch* vis_check_sw_;
    ToggleSwitch* silent_sw_;
    ToggleSwitch* recoil_sw_;

    FovPreview* fov_preview_;
    QLabel* target_lbl_;
    QLabel* scanned_lbl_;
    QLabel* fps_lbl_;
    QTextEdit* log_;

    void setup_ui() {
        auto* central = new QWidget;
        setCentralWidget(central);
        auto* root = new QVBoxLayout(central);
        root->setContentsMargins(20, 16, 20, 20);
        root->setSpacing(12);

        // ---- title bar ----
        auto* title_bar = new QHBoxLayout;
        auto* dot = new QLabel("●");
        dot->setStyleSheet("color:#7c5cff; font-size:14px;");
        auto* title = new QLabel("headshot");
        title->setStyleSheet("color:#e8e8ff; font-size:16px; font-weight:600; letter-spacing:1px;");
        auto* status_lbl = new QLabel("● offline");
        status_lbl->setObjectName("status_lbl");
        status_lbl->setStyleSheet("color:#ff5c7c; font-size:11px;");

        auto* min_btn = new QPushButton("—");
        min_btn->setFixedSize(28, 24);
        min_btn->setCursor(Qt::PointingHandCursor);
        min_btn->setStyleSheet("QPushButton{background:#1a1a22;color:#888;border:none;border-radius:4px;font-size:12px;}QPushButton:hover{background:#2a2a38;color:#fff;}");
        connect(min_btn, &QPushButton::clicked, this, &QWidget::showMinimized);

        auto* close_btn = new QPushButton("✕");
        close_btn->setFixedSize(28, 24);
        close_btn->setCursor(Qt::PointingHandCursor);
        close_btn->setStyleSheet("QPushButton{background:#1a1a22;color:#888;border:none;border-radius:4px;font-size:11px;}QPushButton:hover{background:#ff3c5c;color:#fff;}");
        connect(close_btn, &QPushButton::clicked, this, &QWidget::close);

        title_bar->addWidget(dot);
        title_bar->addWidget(title);
        title_bar->addSpacing(8);
        title_bar->addWidget(status_lbl);
        title_bar->addStretch();
        title_bar->addWidget(min_btn);
        title_bar->addWidget(close_btn);
        root->addLayout(title_bar);

        // ---- master toggle ----
        auto* master = new QFrame;
        master->setObjectName("master");
        auto* mlay = new QHBoxLayout(master);
        mlay->setContentsMargins(16, 12, 16, 12);

        auto* m_lbl = new QLabel("AIMBOT");
        m_lbl->setStyleSheet("color:#e8e8ff; font-size:14px; font-weight:700; letter-spacing:2px;");

        master_sw_ = new QPushButton("OFF");
        master_sw_->setCheckable(true);
        master_sw_->setFixedSize(90, 36);
        master_sw_->setCursor(Qt::PointingHandCursor);

        mlay->addWidget(m_lbl);
        mlay->addStretch();
        mlay->addWidget(master_sw_);
        root->addWidget(master);

        // ---- tabs ----
        auto* tabs = new QTabWidget;
        tabs->setDocumentMode(true);

        // --- aim tab ---
        auto* aim_tab = new QWidget;
        auto* aim_lay = new QVBoxLayout(aim_tab);
        aim_lay->setContentsMargins(4, 8, 4, 4);
        aim_lay->setSpacing(2);

        // key + mode
        auto* km_row = new QHBoxLayout;
        auto* key_lbl = new QLabel("Key");
        key_lbl->setFixedWidth(130);
        key_lbl->setStyleSheet("color:#b8b8c8; font-size:12px;");
        key_combo_ = new QComboBox;
        key_combo_->addItems({"RMB", "LMB", "Shift", "Alt", "Ctrl", "Mouse4", "Mouse5"});
        mode_combo_ = new QComboBox;
        mode_combo_->addItems({"Hold", "Toggle"});
        km_row->addWidget(key_lbl);
        km_row->addWidget(key_combo_);
        km_row->addWidget(mode_combo_);
        aim_lay->addLayout(km_row);

        // smooth mode
        auto* sm_row = new QHBoxLayout;
        auto* sm_lbl = new QLabel("Smoothing");
        sm_lbl->setFixedWidth(130);
        sm_lbl->setStyleSheet("color:#b8b8c8; font-size:12px;");
        smooth_mode_combo_ = new QComboBox;
        smooth_mode_combo_->addItems({"Linear", "Ease Out", "Ease In-Out", "Exponential"});
        sm_row->addWidget(sm_lbl);
        sm_row->addWidget(smooth_mode_combo_);
        aim_lay->addLayout(sm_row);

        // sliders
        fov_slider_ = new SliderRow("FOV", 1, 180, 45, 1, 0);
        smooth_slider_ = new SliderRow("Smooth", 0.1, 20, 3, 0.1, 1);
        sens_slider_ = new SliderRow("Sensitivity", 0.1, 5, 1, 0.01, 2);
        maxspeed_slider_ = new SliderRow("Max Speed", 50, 5000, 800, 50, 0);
        aim_lay->addWidget(fov_slider_);
        aim_lay->addWidget(smooth_slider_);
        aim_lay->addWidget(sens_slider_);
        aim_lay->addWidget(maxspeed_slider_);

        // bone priority
        auto* bone_row = new QHBoxLayout;
        auto* bone_lbl = new QLabel("Bone");
        bone_lbl->setFixedWidth(130);
        bone_lbl->setStyleSheet("color:#b8b8c8; font-size:12px;");
        bone_combo_ = new QComboBox;
        for (auto& b : kBones) bone_combo_->addItem(b.name);
        bone_row->addWidget(bone_lbl);
        bone_row->addWidget(bone_combo_);
        aim_lay->addLayout(bone_row);

        aim_lay->addStretch();

        // --- trigger tab ---
        auto* trig_tab = new QWidget;
        auto* trig_lay = new QVBoxLayout(trig_tab);
        trig_lay->setContentsMargins(4, 8, 4, 4);
        trig_lay->setSpacing(2);

        auto_fire_sw_ = new ToggleSwitch("Auto Fire", false);
        vis_check_sw_ = new ToggleSwitch("Visibility Check", true);
        silent_sw_ = new ToggleSwitch("Silent Aim", false);
        recoil_sw_ = new ToggleSwitch("Recoil Comp", false);

        trigger_delay_slider_ = new SliderRow("Trigger Delay", 0, 500, 80, 5, 0);
        trigger_fov_slider_ = new SliderRow("Trigger FOV", 1, 30, 8, 0.5, 1);
        recoil_amount_slider_ = new SliderRow("Recoil Amt", 0, 100, 0, 1, 0);

        trig_lay->addWidget(auto_fire_sw_);
        trig_lay->addWidget(vis_check_sw_);
        trig_lay->addWidget(silent_sw_);
        trig_lay->addWidget(trigger_delay_slider_);
        trig_lay->addWidget(trigger_fov_slider_);
        trig_lay->addWidget(recoil_sw_);
        trig_lay->addWidget(recoil_amount_slider_);
        trig_lay->addStretch();

        // --- humanize tab ---
        auto* hum_tab = new QWidget;
        auto* hum_lay = new QVBoxLayout(hum_tab);
        hum_lay->setContentsMargins(4, 8, 4, 4);
        hum_lay->setSpacing(2);

        humanize_amp_slider_ = new SliderRow("Humanize Amp", 0, 20, 0, 0.5, 1);
        humanize_freq_slider_ = new SliderRow("Humanize Freq", 1, 20, 6, 0.5, 1);
        prediction_slider_ = new SliderRow("Prediction", 0, 2, 0, 0.05, 2);

        hum_lay->addWidget(humanize_amp_slider_);
        hum_lay->addWidget(humanize_freq_slider_);
        hum_lay->addWidget(prediction_slider_);
        hum_lay->addStretch();

        // --- visual tab ---
        auto* vis_tab = new QWidget;
        auto* vis_lay = new QVBoxLayout(vis_tab);
        vis_lay->setContentsMargins(4, 8, 4, 4);

        fov_preview_ = new FovPreview;
        auto* prev_row = new QHBoxLayout;
        prev_row->addStretch();
        prev_row->addWidget(fov_preview_);
        prev_row->addStretch();
        vis_lay->addLayout(prev_row);
        vis_lay->addStretch();

        tabs->addTab(aim_tab, "AIM");
        tabs->addTab(trig_tab, "TRIGGER");
        tabs->addTab(hum_tab, "HUMANIZE");
        tabs->addTab(vis_tab, "VISUAL");

        root->addWidget(tabs, 1);

        // ---- stats bar ----
        auto* stats = new QFrame;
        stats->setObjectName("stats");
        auto* slay = new QHBoxLayout(stats);
        slay->setContentsMargins(12, 8, 12, 8);

        target_lbl_ = new QLabel("target: none");
        target_lbl_->setStyleSheet("color:#7c5cff; font-size:11px; font-family:Consolas;");
        scanned_lbl_ = new QLabel("scanned: 0");
        scanned_lbl_->setStyleSheet("color:#888; font-size:11px; font-family:Consolas;");
        fps_lbl_ = new QLabel("fps: 0");
        fps_lbl_->setStyleSheet("color:#888; font-size:11px; font-family:Consolas;");

        slay->addWidget(target_lbl_);
        slay->addStretch();
        slay->addWidget(scanned_lbl_);
        slay->addSpacing(16);
        slay->addWidget(fps_lbl_);

        root->addWidget(stats);

        // ---- log ----
        log_ = new QTextEdit;
        log_->setReadOnly(true);
        log_->setFixedHeight(80);
        log_->setObjectName("log");
        root->addWidget(log_);
    }

    void setup_style() {
        setStyleSheet(R"(
            QWidget {
                font-family: 'Segoe UI', sans-serif;
            }
            QFrame#master {
                background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                    stop:0 rgba(124,92,255,0.12), stop:1 rgba(124,92,255,0.04));
                border: 1px solid rgba(124,92,255,0.3);
                border-radius: 10px;
            }
            QFrame#stats {
                background: #0f0f14;
                border: 1px solid #1a1a22;
                border-radius: 6px;
            }
            QTabWidget::pane {
                border: 1px solid #1a1a22;
                border-radius: 8px;
                background: #0c0c10;
                top: -1px;
            }
            QTabBar::tab {
                background: transparent;
                color: #666;
                padding: 8px 18px;
                margin-right: 2px;
                border: none;
                font-size: 11px;
                font-weight: 600;
                letter-spacing: 1px;
            }
            QTabBar::tab:selected {
                color: #7c5cff;
                border-bottom: 2px solid #7c5cff;
            }
            QTabBar::tab:hover:!selected {
                color: #aaa;
            }
            QComboBox {
                background: #0f0f14;
                color: #e0e0ff;
                border: 1px solid #2a2a38;
                border-radius: 4px;
                padding: 5px 10px;
                font-size: 12px;
                min-width: 80px;
            }
            QComboBox:hover { border: 1px solid #3a3a48; }
            QComboBox:focus { border: 1px solid #7c5cff; }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 5px solid #7c5cff;
                margin-right: 6px;
            }
            QComboBox QAbstractItemView {
                background: #0f0f14;
                color: #e0e0ff;
                border: 1px solid #2a2a38;
                selection-background-color: #2a1f5a;
                outline: none;
            }
            QPushButton {
                background: #1a1a22;
                color: #888;
                border: 1px solid #2a2a38;
                border-radius: 6px;
                font-weight: 600;
            }
            QPushButton:checked {
                background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #5a3fd6, stop:1 #7c5cff);
                color: white;
                border: none;
            }
            QTextEdit#log {
                background: #0a0a0d;
                color: #666;
                border: 1px solid #1a1a22;
                border-radius: 6px;
                font-family: Consolas, monospace;
                font-size: 10px;
                padding: 4px;
            }
        )");
    }

    void setup_connections() {
        auto update_cfg = [this]() {
            AimConfig c;
            c.enabled = master_sw_->isChecked();
            c.key = static_cast<AimKey>(key_combo_->currentIndex());
            c.mode = static_cast<AimMode>(mode_combo_->currentIndex());
            c.smooth_mode = static_cast<SmoothMode>(smooth_mode_combo_->currentIndex());

            c.fov = static_cast<float>(fov_slider_->value());
            c.smooth = static_cast<float>(smooth_slider_->value());
            c.sensitivity = static_cast<float>(sens_slider_->value());
            c.max_speed = static_cast<float>(maxspeed_slider_->value());

            c.auto_fire = auto_fire_sw_->isChecked();
            c.vis_check = vis_check_sw_->isChecked();
            c.silent_aim = silent_sw_->isChecked();
            c.recoil_comp = recoil_sw_->isChecked();

            c.trigger_delay_ms = static_cast<float>(trigger_delay_slider_->value());
            c.trigger_fov = static_cast<float>(trigger_fov_slider_->value());
            c.recoil_amount = static_cast<float>(recoil_amount_slider_->value());

            c.humanize_amp = static_cast<float>(humanize_amp_slider_->value());
            c.humanize_freq = static_cast<float>(humanize_freq_slider_->value());
            c.prediction = static_cast<float>(prediction_slider_->value());
            c.bone_priority = bone_combo_->currentIndex();

            core_->set_config(c);

            fov_preview_->set_fov(c.fov);
            fov_preview_->set_trigger_fov(c.trigger_fov);
        };

        connect(master_sw_, &QPushButton::toggled, this, [this, update_cfg](bool checked) {
            master_sw_->setText(checked ? "ON" : "OFF");
            log(checked ? "aimbot enabled" : "aimbot disabled");
            update_cfg();
        });

        connect(key_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update_cfg);
        connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update_cfg);
        connect(smooth_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update_cfg);
        connect(bone_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update_cfg);

        connect(fov_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(smooth_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(sens_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(maxspeed_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(trigger_delay_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(trigger_fov_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(recoil_amount_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(humanize_amp_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(humanize_freq_slider_, &SliderRow::value_changed, this, update_cfg);
        connect(prediction_slider_, &SliderRow::value_changed, this, update_cfg);

        connect(auto_fire_sw_, &ToggleSwitch::toggled, this, update_cfg);
        connect(vis_check_sw_, &ToggleSwitch::toggled, this, update_cfg);
        connect(silent_sw_, &ToggleSwitch::toggled, this, update_cfg);
        connect(recoil_sw_, &ToggleSwitch::toggled, this, update_cfg);

        connect(core_, &AimCore::stats_updated, this, [this](int scanned, float fps) {
            scanned_lbl_->setText(QString("scanned: %1").arg(scanned));
            fps_lbl_->setText(QString("fps: %1").arg(fps, 0, 'f', 0));
        });

        connect(core_, &AimCore::log_line, this, &MainWindow::log);

        update_cfg();
    }

    void update_stats() {
        auto* status_lbl = findChild<QLabel*>("status_lbl");
        if (status_lbl) {
            if (bridge_->attached) {
                status_lbl_->setText("● online");
                status_lbl_->setStyleSheet("color:#5cff9d; font-size:11px;");
            } else {
                status_lbl_->setText("● offline");
                status_lbl_->setStyleSheet("color:#ff5c7c; font-size:11px;");
            }
        }

        Target t = core_->current_target();
        if (t.valid) {
            target_lbl_->setText(QString("target: bone[%1] dist[%2m]").arg(t.bone_id).arg(t.distance, 0, 'f', 1));
        } else {
            target_lbl_->setText("target: none");
        }
    }

    void log(const QString& msg) {
        log_->append(QString("[%1] %2")
            .arg(QTime::currentTime().toString("hh:mm:ss"))
            .arg(msg));
    }
};

#include "headshot_tool.moc"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));

    MainWindow w;
    w.show();

    return app.exec();
}

#include "dekngo.moc"
