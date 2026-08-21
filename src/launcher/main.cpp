#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <velyx/Version.hpp>

#include "core/Color.hpp"
#include "core/Log.hpp"
#include "core/Math.hpp"
#include "core/Paths.hpp"
#include "core/Process.hpp"
#include "core/Resources.hpp"
#include "core/Strings.hpp"
#include "launcher/account/AccountStore.hpp"
#include "launcher/instance/InstanceManager.hpp"

using namespace velyx;

namespace {

constexpr const char* kLog = "Launcher";
constexpr int kWindowWidth = 1020;
constexpr int kWindowHeight = 700;
constexpr float kRowHeight = 76.f;
constexpr float kRowGap = 10.f;

const Color kBackground = palette::kInk;
const Color kPanel = palette::kSlate;
const Color kSurface = palette::kGraphite;
const Color kSurfaceHover = palette::kSteel;
const Color kBorder = palette::kLine;
const Color kAccent = palette::kMint;
const Color kText = palette::kSnow;
const Color kTextMuted = palette::kAsh;
const Color kTextDim = palette::kDim;
const Color kDanger = palette::kEmber;

ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
IDWriteFactory5* g_dwrite5 = nullptr;
IDWriteFontCollection1* g_bundledFonts = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
IDWriteTextFormat* g_fontTitle = nullptr;
IDWriteTextFormat* g_fontBody = nullptr;
IDWriteTextFormat* g_fontSmall = nullptr;
IDWriteTextFormat* g_fontHeading = nullptr;
IDWriteTextFormat* g_fontCentre = nullptr;
IDWriteTextFormat* g_fontCentreLight = nullptr;
IDWriteTextFormat* g_fontSmallRight = nullptr;

HWND g_window = nullptr;

Vec2 g_mouse;
bool g_clicked = false;
int g_hotWidget = 0;
int g_nextWidget = 0;

int g_selected = -1;
int g_menu = -1;
int g_rowMenu = -1;
float g_scroll = 0.f;

std::string g_status = "Prêt.";
bool g_statusIsError = false;

std::string g_newInstanceName;
bool g_editingName = false;

std::vector<Instance> g_instances;
std::vector<InstanceManager::VersionSource> g_versions;
bool g_devMode = true;

struct Job {
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::string phase;
    std::string detail;
    size_t done = 0;
    size_t total = 0;
    std::string result;
    bool resultIsError = false;
};

Job g_job;

D2D1_COLOR_F toD2D(const Color& color) { return D2D1::ColorF(color.r, color.g, color.b, color.a); }
D2D1_RECT_F toD2D(const Rect& rect) {
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

void setStatus(std::string message, bool isError = false) {
    g_status = std::move(message);
    g_statusIsError = isError;
    Log::info(kLog, "{}", g_status);
}

void refreshSnapshot() {
    g_instances = InstanceManager::get().all();
}

void setPhase(std::string phase, std::string detail = {}) {
    const std::lock_guard lock(g_job.mutex);
    g_job.phase = std::move(phase);
    g_job.detail = std::move(detail);
    g_job.done = 0;
    g_job.total = 0;
}

void setProgress(size_t done, size_t total, const std::string& label) {
    const std::lock_guard lock(g_job.mutex);
    g_job.done = done;
    g_job.total = total;
    g_job.detail = label;
}

void finishJob(std::string message, bool isError) {
    const std::lock_guard lock(g_job.mutex);
    g_job.result = std::move(message);
    g_job.resultIsError = isError;
}

bool busy() { return g_job.running.load(std::memory_order_acquire); }

void startJob(std::string phase, std::function<void()> work) {
    if (busy()) return;
    if (g_job.worker.joinable()) g_job.worker.join();

    setPhase(std::move(phase));
    finishJob({}, false);

    g_job.running.store(true, std::memory_order_release);
    g_job.finished.store(false, std::memory_order_release);

    g_job.worker = std::thread([work = std::move(work)] {
        work();
        g_job.running.store(false, std::memory_order_release);
        g_job.finished.store(true, std::memory_order_release);
    });
}

void collectJob() {
    if (!g_job.finished.exchange(false, std::memory_order_acq_rel)) return;
    if (g_job.worker.joinable()) g_job.worker.join();

    std::string message;
    bool isError = false;
    {
        const std::lock_guard lock(g_job.mutex);
        message = g_job.result;
        isError = g_job.resultIsError;
    }

    refreshSnapshot();
    if (!message.empty()) setStatus(message, isError);
}

float clampRadius(const Rect& rect, float radius) {
    return clamp(radius, 0.f, std::min(rect.width(), rect.height()) * 0.5f);
}

void fill(const Rect& rect, const Color& color, float radius = 0.f) {
    if (color.a <= 0.f) return;
    g_brush->SetColor(toD2D(color));

    const float r = clampRadius(rect, radius);
    if (r <= 0.f) {
        g_target->FillRectangle(toD2D(rect), g_brush);
    } else {
        g_target->FillRoundedRectangle(D2D1::RoundedRect(toD2D(rect), r, r), g_brush);
    }
}

void stroke(const Rect& rect, const Color& color, float radius = 0.f, float thickness = 1.f) {
    if (color.a <= 0.f) return;
    g_brush->SetColor(toD2D(color));

    const Rect inset = rect.inflated(-thickness * 0.5f);
    const float r = clampRadius(inset, radius);

    if (r <= 0.f) {
        g_target->DrawRectangle(toD2D(inset), g_brush, thickness);
    } else {
        g_target->DrawRoundedRectangle(D2D1::RoundedRect(toD2D(inset), r, r), g_brush, thickness);
    }
}

void write(std::wstring_view text, const Rect& rect, const Color& color, IDWriteTextFormat* format) {
    if (!format || text.empty()) return;
    g_brush->SetColor(toD2D(color));
    g_target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, toD2D(rect), g_brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void write(std::string_view text, const Rect& rect, const Color& color, IDWriteTextFormat* format) {
    write(strings::toUtf16(text), rect, color, format);
}

float measureText(std::string_view text, IDWriteTextFormat* format) {
    if (!g_dwriteFactory || !format || text.empty()) return 0.f;

    const std::wstring wide = strings::toUtf16(text);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_dwriteFactory->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                                 format, 2000.f, 60.f, &layout))) {
        return 0.f;
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();

    return metrics.widthIncludingTrailingWhitespace;
}

void drawChevron(Vec2 centre, const Color& color) {
    g_brush->SetColor(toD2D(color));
    g_target->DrawLine(D2D1::Point2F(centre.x - 3.5f, centre.y - 1.5f),
                       D2D1::Point2F(centre.x, centre.y + 2.f), g_brush, 1.4f);
    g_target->DrawLine(D2D1::Point2F(centre.x, centre.y + 2.f),
                       D2D1::Point2F(centre.x + 3.5f, centre.y - 1.5f), g_brush, 1.4f);
}

void drawPlayGlyph(Vec2 centre, const Color& color) {
    ID2D1PathGeometry* path = nullptr;
    if (FAILED(g_d2dFactory->CreatePathGeometry(&path))) return;

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) {
        path->Release();
        return;
    }

    sink->BeginFigure(D2D1::Point2F(centre.x - 4.f, centre.y - 5.5f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(centre.x + 5.f, centre.y));
    sink->AddLine(D2D1::Point2F(centre.x - 4.f, centre.y + 5.5f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();

    g_brush->SetColor(toD2D(color));
    g_target->FillGeometry(path, g_brush);
    path->Release();
}

bool button(const Rect& rect, std::string_view label, bool primary = false, bool enabled = true) {
    const int id = ++g_nextWidget;
    const bool hovered = enabled && rect.contains(g_mouse);
    if (hovered) g_hotWidget = id;

    Color background = primary ? (hovered ? kAccent.lighten(0.06f) : kAccent)
                               : (hovered ? kSurfaceHover : kSurface);
    Color foreground = primary ? background.readableForeground() : kText;

    if (!enabled) {
        background = background.fade(0.35f);
        foreground = foreground.fade(0.4f);
    }

    fill(rect, background, 9.f);
    if (!primary) stroke(rect, kBorder.fade(hovered ? 1.f : 0.7f), 9.f);

    write(label, rect, foreground, g_fontCentre);

    return hovered && g_clicked && enabled;
}

void createInstance() {
    const std::string name = g_newInstanceName.empty() ? "Instance" : g_newInstanceName;

    startJob("Création de « " + name + " »", [name] {
        setPhase("Copie des fichiers du jeu", "recherche de l'installation");

        std::string error;
        const bool ok = InstanceManager::get().create(
            name, InstanceManager::CloneMode::Link,
            [](size_t done, size_t total, const std::string& label) {
                setProgress(done, total, label);
            },
            &error);

        if (ok) {
            finishJob("Instance « " + name + " » prête.", false);
        } else {
            finishJob(error, true);
        }
    });

    g_newInstanceName.clear();
    g_editingName = false;
}

void launchInstance(const std::string& id, const std::string& name) {
    startJob("Lancement de « " + name + " »", [id, name] {
        setPhase("Démarrage du jeu", "activation du paquet");

        auto& manager = InstanceManager::get();
        Instance* instance = manager.find(id);
        if (!instance) {
            finishJob("Instance introuvable.", true);
            return;
        }

        std::string error;
        if (manager.launch(*instance, &error)) {
            finishJob("« " + name + " » lancée (pid " + std::to_string(instance->pid) + ").", false);
        } else {
            finishJob(error, true);
        }

        if (const Account* account = AccountStore::get().forInstance(id)) {
            AccountStore::get().markUsed(account->id);
        }
    });
}

void switchVersion(const std::string& id, const std::string& name,
                   const InstanceManager::VersionSource& source) {
    startJob("Passage de « " + name + " » en " + source.version, [id, name, source] {
        setPhase("Réinstallation du jeu", source.version);

        auto& manager = InstanceManager::get();
        Instance* instance = manager.find(id);
        if (!instance) {
            finishJob("Instance introuvable.", true);
            return;
        }

        std::string error;
        if (manager.setVersion(*instance, source,
                               [](size_t done, size_t total, const std::string& label) {
                                   setProgress(done, total, label);
                               },
                               &error)) {
            finishJob("« " + name + " » est maintenant en " + source.version + ".", false);
        } else {
            finishJob(error, true);
        }
    });
}

void removeInstance(const std::string& id, const std::string& name) {
    const int answer = MessageBoxW(
        g_window,
        strings::toUtf16("Supprimer l'instance « " + name +
                         " » et ses fichiers ?\nLes mondes de cette instance seront perdus.")
            .c_str(),
        L"Velyx", MB_YESNO | MB_ICONWARNING);
    if (answer != IDYES) return;

    startJob("Suppression de « " + name + " »", [id, name] {
        setPhase("Désinscription du paquet", name);

        std::string error;
        if (InstanceManager::get().remove(id, true, &error)) {
            finishJob("Instance supprimée.", false);
        } else {
            finishJob(error, true);
        }
    });

    g_selected = -1;
}

void loadVersions() {
    startJob("Lecture des versions disponibles", [] {
        setPhase("Recherche des versions", "paquet installé et dossier versions/");
        const auto sources = InstanceManager::availableVersions();
        {
            const std::lock_guard lock(g_job.mutex);
            g_versions = sources;
        }
        finishJob({}, false);
    });
}

void bindAccount(const std::string& id, const std::string& name) {
    auto& accounts = AccountStore::get();

    Account& account = accounts.create("Compte " + std::to_string(accounts.all().size() + 1));
    accounts.bind(account.id, id);

    setStatus("« " + account.label + " » est associé à « " + name +
              " ». Connectez-vous dans le jeu au premier lancement.");
}

void drawInstanceRow(const Rect& rect, const Instance& instance, int index) {
    const int id = ++g_nextWidget;
    const bool hovered = rect.contains(g_mouse) && !busy();
    if (hovered) g_hotWidget = id;

    const bool selected = index == g_selected;
    const bool running = instance.running();

    fill(rect, selected ? kSurfaceHover.fade(0.5f) : kPanel, 14.f);
    stroke(rect, selected ? kBorder : kBorder.fade(0.55f), 14.f);

    static const Color kTints[4] = {palette::kMint, Color::rgb8(124, 140, 255), palette::kHoney,
                                    Color::rgb8(232, 112, 158)};
    const Color tint = kTints[index % 4];

    const Rect avatar{rect.left + 16.f, rect.center().y - 19.f, rect.left + 54.f,
                      rect.center().y + 19.f};
    fill(avatar, tint.withAlpha(0.12f), 12.f);
    stroke(avatar, tint.withAlpha(0.26f), 12.f);
    write(strings::toUpper(instance.name.substr(0, 1)), avatar, tint, g_fontCentre);

    const Rect more{rect.right - 46.f, rect.center().y - 15.f, rect.right - 16.f,
                    rect.center().y + 15.f};
    const Rect action{more.left - 108.f, rect.center().y - 16.f, more.left - 10.f,
                      rect.center().y + 16.f};

    write(instance.name, Rect{rect.left + 68.f, rect.top + 15.f, rect.left + 300.f, rect.top + 39.f},
          kText, g_fontTitle);

    const Account* account = AccountStore::get().forInstance(instance.id);
    write(account ? account->label : "Aucun compte associé",
          Rect{rect.left + 68.f, rect.top + 38.f, rect.left + 300.f, rect.bottom - 14.f}, kTextDim,
          g_fontSmall);

    const std::string version = instance.gameVersion.empty() ? "inconnue" : instance.gameVersion;
    const float chipWidth = std::max(96.f, measureText(version, g_fontSmall) + 44.f);
    const Rect chip{rect.left + 312.f, rect.center().y - 15.f, rect.left + 312.f + chipWidth,
                    rect.center().y + 15.f};

    const bool menuOpen = g_menu == index;
    const bool chipHover = chip.contains(g_mouse) && !busy();

    fill(chip, menuOpen || chipHover ? kSurfaceHover : kSurface, 9.f);
    stroke(chip, menuOpen ? kAccent.withAlpha(0.55f) : kBorder, 9.f);
    write(version, Rect{chip.left + 12.f, chip.top, chip.right - 24.f, chip.bottom},
          menuOpen || chipHover ? kText : kTextMuted, g_fontSmall);
    drawChevron({chip.right - 14.f, chip.center().y}, kTextDim);

    if (chipHover) {
        g_hotWidget = ++g_nextWidget;
        if (g_clicked) {
            g_selected = index;
            g_rowMenu = -1;
            g_menu = menuOpen ? -1 : index;
            if (g_menu == index && g_versions.empty()) loadVersions();
        }
    }

    std::string state = "Prête";
    Color stateColor = kTextMuted;
    if (running) {
        state = "En cours";
        stateColor = kAccent;
    } else if (!instance.registered) {
        state = "Non enregistrée";
        stateColor = palette::kHoney;
    }

    const float stateWidth = measureText(state, g_fontSmallRight);
    const float stateRight = action.left - 18.f;

    write(state, Rect{stateRight - stateWidth - 4.f, rect.top, stateRight, rect.bottom}, stateColor,
          g_fontSmallRight);
    fill(Rect::fromSize(stateRight - stateWidth - 18.f, rect.center().y - 3.5f, 7.f, 7.f),
         running ? kAccent : stateColor.fade(0.55f), 3.5f);

    if (running) {
        const bool stopHover = action.contains(g_mouse) && !busy();
        fill(action, stopHover ? kSurfaceHover : Color{}, 9.f);
        stroke(action, kBorder, 9.f);
        fill(Rect::fromSize(action.left + 16.f, action.center().y - 5.f, 10.f, 10.f),
             stopHover ? kText : kTextMuted, 2.f);
        write("Arrêter", Rect{action.left + 34.f, action.top, action.right - 10.f, action.bottom},
              stopHover ? kText : kTextMuted, g_fontSmall);

        if (stopHover) {
            g_hotWidget = ++g_nextWidget;
            if (g_clicked) {
                Process::terminate(instance.pid);
                setStatus("« " + instance.name + " » arrêtée.");
                refreshSnapshot();
            }
        }
    } else {
        const bool playHover = action.contains(g_mouse) && !busy();
        const Color background = playHover ? kAccent.lighten(0.06f) : kAccent;
        const Color foreground = background.readableForeground();

        fill(action, busy() ? background.fade(0.35f) : background, 9.f);
        drawPlayGlyph({action.left + 22.f, action.center().y},
                      busy() ? foreground.fade(0.4f) : foreground);
        write("Jouer", Rect{action.left + 36.f, action.top, action.right - 12.f, action.bottom},
              busy() ? foreground.fade(0.4f) : foreground, g_fontBody);

        if (playHover) {
            g_hotWidget = ++g_nextWidget;
            if (g_clicked) {
                g_selected = index;
                launchInstance(instance.id, instance.name);
            }
        }
    }

    const bool moreHover = more.contains(g_mouse) && !busy();
    if (moreHover || g_rowMenu == index) fill(more, kSurfaceHover, 9.f);
    for (int d = 0; d < 3; ++d) {
        fill(Rect::fromSize(more.center().x - 7.f + static_cast<float>(d) * 6.f,
                            more.center().y - 1.5f, 3.f, 3.f),
             moreHover ? kText : kTextMuted, 1.5f);
    }
    if (moreHover) {
        g_hotWidget = ++g_nextWidget;
        if (g_clicked) {
            g_selected = index;
            g_menu = -1;
            g_rowMenu = g_rowMenu == index ? -1 : index;
        }
    }

    if (hovered && g_clicked && !chipHover && !moreHover && !action.contains(g_mouse)) {
        g_selected = index;
    }
}

void drawRowMenu(const Rect& rowRect, int index) {
    if (g_rowMenu != index || index >= static_cast<int>(g_instances.size())) return;

    const Instance& instance = g_instances[static_cast<size_t>(index)];
    const Rect menu{rowRect.right - 226.f, rowRect.center().y + 18.f, rowRect.right - 16.f,
                    rowRect.center().y + 120.f};

    fill(menu.translated({0.f, 4.f}), Color::rgb8(0, 0, 0, 80), 12.f);
    fill(menu, Color::rgb8(23, 25, 29), 12.f);
    stroke(menu, Color::rgb8(46, 52, 59), 12.f);

    static const char* kLabels[3] = {"Associer un compte", "Ouvrir le dossier", "Supprimer"};

    float y = menu.top + 6.f;
    for (int i = 0; i < 3; ++i) {
        const Rect item{menu.left + 4.f, y, menu.right - 4.f, y + 30.f};
        y += 30.f;

        const bool itemHover = item.contains(g_mouse) && !busy();
        if (itemHover) {
            fill(item, kSurfaceHover, 8.f);
            g_hotWidget = ++g_nextWidget;
        }

        write(kLabels[i], Rect{item.left + 12.f, item.top, item.right - 10.f, item.bottom},
              i == 2 ? kDanger : kTextMuted, g_fontSmall);

        if (!itemHover || !g_clicked) continue;

        g_rowMenu = -1;

        if (i == 0) {
            bindAccount(instance.id, instance.name);
        } else if (i == 1) {
            ShellExecuteW(nullptr, L"open", instance.root.wstring().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        } else {
            removeInstance(instance.id, instance.name);
        }
        return;
    }
}

void drawVersionMenu(const Rect& rowRect, int index) {
    if (g_menu != index || index >= static_cast<int>(g_instances.size())) return;

    const Instance& instance = g_instances[static_cast<size_t>(index)];

    const float rows = static_cast<float>(std::max<size_t>(g_versions.size(), 1));
    const Rect menu{rowRect.left + 312.f, rowRect.center().y + 18.f, rowRect.left + 520.f,
                    rowRect.center().y + 18.f + 30.f + rows * 30.f + 40.f};

    fill(menu.translated({0.f, 4.f}), Color::rgb8(0, 0, 0, 80), 12.f);
    fill(menu, Color::rgb8(23, 25, 29), 12.f);
    stroke(menu, Color::rgb8(46, 52, 59), 12.f);

    write("VERSIONS DISPONIBLES",
          Rect{menu.left + 12.f, menu.top + 6.f, menu.right - 12.f, menu.top + 28.f}, kTextDim,
          g_fontSmall);

    float y = menu.top + 30.f;

    if (g_versions.empty()) {
        write("Aucune version détectée",
              Rect{menu.left + 12.f, y, menu.right - 12.f, y + 30.f}, kTextMuted, g_fontSmall);
        y += 30.f;
    }

    for (const auto& source : g_versions) {
        const Rect item{menu.left + 4.f, y, menu.right - 4.f, y + 30.f};
        y += 30.f;

        const bool current = source.version == instance.gameVersion;
        const bool itemHover = item.contains(g_mouse) && !busy();

        if (itemHover) {
            fill(item, kSurfaceHover, 8.f);
            g_hotWidget = ++g_nextWidget;
        }

        write(source.version, Rect{item.left + 12.f, item.top, item.right - 46.f, item.bottom},
              current ? kText : kTextMuted, g_fontSmall);

        if (current) {
            fill(Rect::fromSize(item.right - 22.f, item.center().y - 3.f, 6.f, 6.f), kAccent, 3.f);
        } else if (source.installed) {
            write("installée", Rect{item.right - 88.f, item.top, item.right - 14.f, item.bottom},
                  kTextDim, g_fontSmall);
        }

        if (itemHover && g_clicked && !current) {
            g_menu = -1;
            switchVersion(instance.id, instance.name, source);
            return;
        }
    }

    const Rect add{menu.left + 4.f, y + 4.f, menu.right - 4.f, y + 34.f};
    const bool addHover = add.contains(g_mouse) && !busy();
    if (addHover) {
        fill(add, kSurfaceHover, 8.f);
        g_hotWidget = ++g_nextWidget;
    }
    write("Ajouter un dossier de version",
          Rect{add.left + 12.f, add.top, add.right - 10.f, add.bottom}, kTextDim, g_fontSmall);

    if (addHover && g_clicked) {
        std::error_code ec;
        std::filesystem::create_directories(Paths::versions(), ec);
        ShellExecuteW(nullptr, L"open", Paths::versions().wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        setStatus("Déposez un build décompressé dans versions/, puis rouvrez ce menu.");
        g_menu = -1;
    }
}

void drawEmptyState(const Rect& rect) {
    fill(rect, kPanel.fade(0.45f), 14.f);
    stroke(rect, kBorder.fade(0.5f), 14.f);

    const float blockHeight = 172.f;
    const float top = rect.top + std::max(24.f, (rect.height() - blockHeight) * 0.42f);
    const Vec2 centre{rect.center().x, top + 26.f};

    const Rect badge = Rect::fromSize(centre.x - 26.f, top, 52.f, 52.f);
    fill(badge, kAccent.withAlpha(0.10f), 15.f);
    stroke(badge, kAccent.withAlpha(0.22f), 15.f);

    g_brush->SetColor(toD2D(kAccent));
    g_target->DrawLine(D2D1::Point2F(centre.x - 9.f, centre.y - 8.f),
                       D2D1::Point2F(centre.x, centre.y + 9.f), g_brush, 2.6f);
    g_target->DrawLine(D2D1::Point2F(centre.x, centre.y + 9.f),
                       D2D1::Point2F(centre.x + 9.f, centre.y - 8.f), g_brush, 2.6f);

    write("Aucune instance pour l'instant",
          Rect{rect.left, top + 66.f, rect.right, top + 92.f}, kText, g_fontCentre);

    write("Velyx copie le jeu installé par liens durs et lui donne",
          Rect{rect.left, top + 96.f, rect.right, top + 118.f}, kTextDim, g_fontCentreLight);
    write("une identité de paquet distincte. Windows accepte alors",
          Rect{rect.left, top + 116.f, rect.right, top + 138.f}, kTextDim, g_fontCentreLight);
    write("de lancer plusieurs copies en même temps.",
          Rect{rect.left, top + 136.f, rect.right, top + 158.f}, kTextDim, g_fontCentreLight);
}

void drawBusyOverlay(const Rect& client) {
    if (!busy()) return;

    std::string phase;
    std::string detail;
    size_t done = 0;
    size_t total = 0;
    {
        const std::lock_guard lock(g_job.mutex);
        phase = g_job.phase;
        detail = g_job.detail;
        done = g_job.done;
        total = g_job.total;
    }

    fill(client, kBackground.withAlpha(0.72f));

    const Rect card = Rect::fromSize(client.center().x - 210.f, client.center().y - 62.f, 420.f,
                                     124.f);
    fill(card.translated({0.f, 6.f}), Color::rgb8(0, 0, 0, 90), 16.f);
    fill(card, kPanel, 16.f);
    stroke(card, kBorder, 16.f);

    write(phase, Rect{card.left + 24.f, card.top + 20.f, card.right - 24.f, card.top + 44.f}, kText,
          g_fontTitle);

    const Rect track{card.left + 24.f, card.center().y + 6.f, card.right - 24.f,
                     card.center().y + 12.f};
    fill(track, kSurface, 3.f);

    if (total > 0) {
        const float ratio = clamp(static_cast<float>(done) / static_cast<float>(total), 0.f, 1.f);
        fill(Rect{track.left, track.top, track.left + track.width() * ratio, track.bottom}, kAccent,
             3.f);
    } else {
        const auto ticks = static_cast<float>(GetTickCount64() % 1400) / 1400.f;
        const float width = track.width() * 0.28f;
        const float x = track.left + (track.width() + width) * ticks - width;
        fill(Rect{std::max(track.left, x), track.top, std::min(track.right, x + width),
                  track.bottom},
             kAccent, 3.f);
    }

    const std::string line =
        total > 0 ? std::to_string(done) + " / " + std::to_string(total) + " fichiers" : detail;
    write(line, Rect{card.left + 24.f, card.bottom - 34.f, card.right - 24.f, card.bottom - 14.f},
          kTextDim, g_fontSmall);
}

void render(const Rect& client) {
    g_nextWidget = 0;
    g_hotWidget = 0;

    fill(client, kBackground);

    const Rect brand{client.left + 24.f, client.top + 16.f, client.left + 300.f, client.top + 40.f};
    const Vec2 mark{brand.left + 8.f, brand.center().y};
    g_brush->SetColor(toD2D(kAccent));
    g_target->DrawLine(D2D1::Point2F(mark.x - 7.f, mark.y - 7.f),
                       D2D1::Point2F(mark.x, mark.y + 7.f), g_brush, 2.4f);
    g_target->DrawLine(D2D1::Point2F(mark.x, mark.y + 7.f),
                       D2D1::Point2F(mark.x + 7.f, mark.y - 7.f), g_brush, 2.4f);

    write("VELYX", Rect{brand.left + 24.f, brand.top, brand.left + 100.f, brand.bottom}, kTextMuted,
          g_fontSmall);
    write(version::kString, Rect{brand.left + 74.f, brand.top, brand.left + 140.f, brand.bottom},
          kTextDim, g_fontSmall);

    const Rect header{client.left + 24.f, client.top + 46.f, client.right - 24.f, client.top + 94.f};

    write("Instances", Rect{header.left, header.top, header.left + 240.f, header.top + 32.f}, kText,
          g_fontHeading);

    const int live = static_cast<int>(
        std::ranges::count_if(g_instances, [](const Instance& i) { return i.running(); }));
    write(std::to_string(g_instances.size()) + " instances · " + std::to_string(live) +
              " en cours · " + std::to_string(AccountStore::get().all().size()) + " comptes",
          Rect{header.left, header.top + 28.f, header.left + 340.f, header.bottom}, kTextDim,
          g_fontSmall);

    const Rect nameField{header.right - 388.f, header.center().y - 17.f, header.right - 174.f,
                         header.center().y + 17.f};
    fill(nameField, kSurface, 9.f);
    stroke(nameField, g_editingName ? kAccent.withAlpha(0.6f) : kBorder, 9.f);
    write(g_newInstanceName.empty() ? "Nom de la nouvelle instance" : g_newInstanceName,
          Rect{nameField.left + 12.f, nameField.top, nameField.right - 12.f, nameField.bottom},
          g_newInstanceName.empty() ? kTextDim : kText, g_fontBody);

    if (!busy() && nameField.contains(g_mouse)) {
        g_hotWidget = ++g_nextWidget;
        if (g_clicked) g_editingName = true;
    } else if (g_clicked && !nameField.contains(g_mouse)) {
        g_editingName = false;
    }

    if (button(Rect{header.right - 164.f, header.center().y - 17.f, header.right,
                    header.center().y + 17.f},
               "Créer une instance", true, !busy())) {
        createInstance();
    }

    fill(Rect{client.left + 24.f, header.bottom + 6.f, client.right - 24.f, header.bottom + 7.f},
         kBorder.fade(0.5f));

    Rect list{client.left + 24.f, header.bottom + 20.f, client.right - 24.f, client.bottom - 56.f};

    if (!g_devMode) {
        const Rect banner{list.left, list.top, list.right, list.top + 58.f};
        list = Rect{list.left, banner.bottom + 12.f, list.right, list.bottom};

        fill(banner, kDanger.withAlpha(0.09f), 12.f);
        stroke(banner, kDanger.withAlpha(0.22f), 12.f);
        fill(Rect{banner.left + 1.f, banner.top + 12.f, banner.left + 3.f, banner.bottom - 12.f},
             kDanger, 1.f);
        write("Le mode développeur de Windows est désactivé",
              Rect{banner.left + 18.f, banner.top + 10.f, banner.right - 16.f, banner.top + 30.f},
              kDanger, g_fontBody);
        write("Paramètres > Confidentialité et sécurité > Espace développeurs. Sans lui, Windows "
              "refuse d'enregistrer une instance.",
              Rect{banner.left + 18.f, banner.top + 30.f, banner.right - 16.f, banner.bottom - 8.f},
              kTextMuted, g_fontSmall);
    }

    if (g_instances.empty()) {
        drawEmptyState(list);
    } else {
        float y = list.top - g_scroll;
        for (size_t i = 0; i < g_instances.size(); ++i) {
            const Rect row{list.left, y, list.right, y + kRowHeight};
            y += kRowHeight + kRowGap;
            if (row.bottom < list.top || row.top > list.bottom) continue;
            drawInstanceRow(row, g_instances[i], static_cast<int>(i));
        }

        y = list.top - g_scroll;
        for (size_t i = 0; i < g_instances.size(); ++i) {
            const Rect row{list.left, y, list.right, y + kRowHeight};
            y += kRowHeight + kRowGap;
            drawVersionMenu(row, static_cast<int>(i));
            drawRowMenu(row, static_cast<int>(i));
        }
    }

    const Rect status{client.left, client.bottom - 40.f, client.right, client.bottom};
    fill(Rect{status.left + 24.f, status.top, status.right - 24.f, status.top + 1.f},
         kBorder.fade(0.6f));
    write(g_status, Rect{status.left + 24.f, status.top, status.right - 24.f, status.bottom},
          g_statusIsError ? kDanger : kTextDim, g_fontSmall);

    drawBusyOverlay(client);
}

std::filesystem::path fontDirectory() {
    const auto unpacked = Paths::assets() / "fonts";

    std::error_code ec;
    if (std::filesystem::exists(unpacked, ec)) return unpacked;

    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / "assets" / "fonts";
}

void loadBundledFonts() {
    if (!g_dwriteFactory) return;
    if (FAILED(g_dwriteFactory->QueryInterface(__uuidof(IDWriteFactory5),
                                               reinterpret_cast<void**>(&g_dwrite5)))) {
        Log::warn(kLog, "IDWriteFactory5 unavailable, falling back to system fonts");
        return;
    }

    IDWriteFontSetBuilder1* builder = nullptr;
    if (FAILED(g_dwrite5->CreateFontSetBuilder(&builder))) return;

    int loaded = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(fontDirectory(), ec)) {
        if (!entry.is_regular_file(ec)) continue;

        const auto extension = strings::toLower(entry.path().extension().string());
        if (extension != ".ttf" && extension != ".otf") continue;

        IDWriteFontFile* file = nullptr;
        if (FAILED(g_dwrite5->CreateFontFileReference(entry.path().wstring().c_str(), nullptr,
                                                      &file))) {
            continue;
        }
        if (SUCCEEDED(builder->AddFontFile(file))) ++loaded;
        file->Release();
    }

    IDWriteFontSet* set = nullptr;
    if (loaded > 0 && SUCCEEDED(builder->CreateFontSet(&set))) {
        g_dwrite5->CreateFontCollectionFromFontSet(set, &g_bundledFonts);
        set->Release();
    }
    builder->Release();

    Log::info(kLog, "{} bundled font file(s) loaded from {}", loaded,
              fontDirectory().string());
}

IDWriteTextFormat* makeFormat(float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {

    IDWriteTextFormat* format = nullptr;
    if (FAILED(g_dwriteFactory->CreateTextFormat(L"Space Grotesk", g_bundledFonts, weight,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, size, L"", &format)) &&
        FAILED(g_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) {
        return nullptr;
    }
    format->SetParagraphAlignment(paragraph);
    format->SetTextAlignment(align);
    return format;
}

bool createGraphics(HWND window) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory))) return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dwriteFactory)))) {
        return false;
    }

    RECT client{};
    GetClientRect(window, &client);

    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(client.right - client.left),
                                         static_cast<UINT32>(client.bottom - client.top));

    if (FAILED(g_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window, size, D2D1_PRESENT_OPTIONS_NONE), &g_target))) {
        return false;
    }

    g_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_brush);

    loadBundledFonts();

    g_fontHeading = makeFormat(21.f, DWRITE_FONT_WEIGHT_BOLD);
    g_fontTitle = makeFormat(14.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    g_fontBody = makeFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fontSmall = makeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fontCentre = makeFormat(12.5f, DWRITE_FONT_WEIGHT_MEDIUM,
                              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fontCentreLight = makeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fontSmallRight = makeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_TRAILING);

    return g_fontHeading && g_fontTitle && g_fontBody && g_fontSmall && g_fontCentre &&
           g_fontCentreLight && g_fontSmallRight;
}

void destroyGraphics() {
    const auto release = [](auto*& pointer) {
        if (pointer) {
            pointer->Release();
            pointer = nullptr;
        }
    };

    release(g_fontSmallRight);
    release(g_fontCentreLight);
    release(g_fontCentre);
    release(g_fontSmall);
    release(g_fontBody);
    release(g_fontTitle);
    release(g_fontHeading);
    release(g_brush);
    release(g_target);
    release(g_bundledFonts);
    release(g_dwrite5);
    release(g_dwriteFactory);
    release(g_d2dFactory);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_MOUSEMOVE:
            g_mouse = {static_cast<float>(GET_X_LPARAM(lParam)),
                       static_cast<float>(GET_Y_LPARAM(lParam))};
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            g_clicked = true;
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            g_scroll = std::max(0.f, g_scroll - static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                                    WHEEL_DELTA * 40.f);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_CHAR:
            if (g_editingName && !busy()) {
                const auto codepoint = static_cast<wchar_t>(wParam);
                if (codepoint == VK_BACK) {
                    if (!g_newInstanceName.empty()) g_newInstanceName.pop_back();
                } else if (codepoint == VK_RETURN) {
                    g_editingName = false;
                    createInstance();
                } else if (codepoint >= 32 && g_newInstanceName.size() < 32) {
                    g_newInstanceName += strings::toUtf8(std::wstring(1, codepoint));
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_SIZE:
            if (g_target) {
                g_target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            }
            return 0;

        case WM_TIMER:
            collectJob();
            if (!busy()) {
                InstanceManager::get().refreshRunningState();
                refreshSnapshot();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            RECT client{};
            GetClientRect(window, &client);

            if (g_target) {
                g_target->BeginDraw();
                render(Rect{0.f, 0.f, static_cast<float>(client.right),
                            static_cast<float>(client.bottom)});
                g_target->EndDraw();
            }

            g_clicked = false;
            ValidateRect(window, nullptr);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g_hotWidget != 0
                                                   ? reinterpret_cast<LPCWSTR>(IDC_HAND)
                                                   : reinterpret_cast<LPCWSTR>(IDC_ARROW)));
                return TRUE;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Paths::ensureLayout();
    Log::init(Paths::logs() / "launcher.log", false);
    Log::info(kLog, "Velyx Launcher {}", version::kFull);

    resources::unpack(GetModuleHandleW(nullptr), Paths::root());

    InstanceManager::get().load();
    AccountStore::get().load();

    g_devMode = InstanceManager::developerModeEnabled();
    refreshSnapshot();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = CreateSolidBrush(RGB(14, 15, 17));
    windowClass.lpszClassName = L"VelyxLauncher";
    RegisterClassExW(&windowClass);

    const HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L"Velyx", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        kWindowWidth, kWindowHeight, nullptr, nullptr, instance, nullptr);

    if (!window || !createGraphics(window)) {
        MessageBoxW(nullptr, L"Direct2D n'a pas pu être initialisé.", L"Velyx", MB_ICONERROR);
        return 1;
    }

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window, 20 , &darkMode,
                          sizeof(darkMode));

    g_window = window;

    ShowWindow(window, SW_SHOW);
    SetTimer(window, 1, 60, nullptr);

    loadVersions();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_job.worker.joinable()) g_job.worker.join();

    destroyGraphics();
    Log::shutdown();
    CoUninitialize();

    return 0;
}
