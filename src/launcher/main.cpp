#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include <velyx/Version.hpp>

#include "core/Color.hpp"
#include "core/Log.hpp"
#include "core/Math.hpp"
#include "core/Paths.hpp"
#include "core/Process.hpp"
#include "core/Strings.hpp"
#include "launcher/account/AccountStore.hpp"
#include "launcher/instance/InstanceManager.hpp"

using namespace velyx;

namespace {

constexpr const char* kLog = "Launcher";
constexpr int kWindowWidth = 940;
constexpr int kWindowHeight = 620;
constexpr float kRowHeight = 76.f;

const Color kBackground = palette::kInk;
const Color kPanel = palette::kSlate;
const Color kSurface = palette::kGraphite;
const Color kSurfaceHover = palette::kSteel;
const Color kBorder = palette::kLine;
const Color kAccent = palette::kMint;
const Color kAccentDeep = palette::kJade;
const Color kText = palette::kSnow;
const Color kTextMuted = palette::kAsh;
const Color kDanger = palette::kEmber;

ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
IDWriteTextFormat* g_fontTitle = nullptr;
IDWriteTextFormat* g_fontBody = nullptr;
IDWriteTextFormat* g_fontSmall = nullptr;
IDWriteTextFormat* g_fontHeading = nullptr;

Vec2 g_mouse;
bool g_clicked = false;
int g_hotWidget = 0;
int g_nextWidget = 0;

int g_selected = -1;
int g_menu = -1;
int g_rowMenu = -1;
std::vector<InstanceManager::VersionSource> g_versions;
std::string g_status = "Prêt.";
bool g_statusIsError = false;
bool g_busy = false;

std::string g_newInstanceName;
bool g_editingName = false;

float g_scroll = 0.f;

D2D1_COLOR_F toD2D(const Color& color) { return D2D1::ColorF(color.r, color.g, color.b, color.a); }
D2D1_RECT_F toD2D(const Rect& rect) {
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

void setStatus(std::string message, bool isError = false) {
    g_status = std::move(message);
    g_statusIsError = isError;
    Log::info(kLog, "{}", g_status);
}

void fill(const Rect& rect, const Color& color, float radius = 0.f) {
    g_brush->SetColor(toD2D(color));
    if (radius <= 0.f) {
        g_target->FillRectangle(toD2D(rect), g_brush);
    } else {
        g_target->FillRoundedRectangle(D2D1::RoundedRect(toD2D(rect), radius, radius), g_brush);
    }
}

void stroke(const Rect& rect, const Color& color, float radius = 0.f, float thickness = 1.f) {
    g_brush->SetColor(toD2D(color));
    const Rect inset = rect.inflated(-thickness * 0.5f);
    if (radius <= 0.f) {
        g_target->DrawRectangle(toD2D(inset), g_brush, thickness);
    } else {
        g_target->DrawRoundedRectangle(D2D1::RoundedRect(toD2D(inset), radius, radius), g_brush,
                                       thickness);
    }
}

void write(std::wstring_view text, const Rect& rect, const Color& color, IDWriteTextFormat* format) {
    g_brush->SetColor(toD2D(color));
    g_target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, toD2D(rect), g_brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void write(std::string_view text, const Rect& rect, const Color& color, IDWriteTextFormat* format) {
    write(strings::toUtf16(text), rect, color, format);
}

bool button(const Rect& rect, std::string_view label, bool primary = false, bool enabled = true) {
    const int id = ++g_nextWidget;
    const bool hovered = enabled && rect.contains(g_mouse);
    if (hovered) g_hotWidget = id;

    Color background = primary ? (hovered ? kAccent : kAccentDeep)
                               : (hovered ? Color::rgb8(24, 61, 40) : kSurface);
    Color foreground = primary ? background.readableForeground() : kText;

    if (!enabled) {
        background = background.fade(0.35f);
        foreground = foreground.fade(0.4f);
    }

    fill(rect, background, 8.f);
    if (!primary) stroke(rect, kBorder.fade(hovered ? 1.f : 0.6f), 8.f);

    write(label, Rect{rect.left, rect.top + 1.f, rect.right, rect.bottom}, foreground, g_fontBody);

    return hovered && g_clicked && enabled;
}

void createInstance() {
    if (g_newInstanceName.empty()) {
        setStatus("Donnez un nom à l'instance avant de la créer.", true);
        return;
    }

    g_busy = true;
    setStatus("Création en cours… copie des fichiers du jeu.");

    std::string error;
    const bool ok = InstanceManager::get().create(
        g_newInstanceName, InstanceManager::CloneMode::Link,
        [](size_t done, size_t total, const std::string&) {

            if (total > 0 && done % 512 == 0) {
                Log::info(kLog, "copying {}/{}", done, total);
            }
        },
        &error);

    g_busy = false;

    if (ok) {
        setStatus("Instance « " + g_newInstanceName + " » créée.");
        g_newInstanceName.clear();
    } else {
        setStatus(error, true);
    }
}

void launchSelected() {
    auto& manager = InstanceManager::get();
    if (g_selected < 0 || g_selected >= static_cast<int>(manager.all().size())) return;

    Instance* instance = manager.find(manager.all()[static_cast<size_t>(g_selected)].id);
    if (!instance) return;

    setStatus("Lancement de « " + instance->name + " »…");

    std::string error;
    if (manager.launch(*instance, &error)) {
        setStatus("« " + instance->name + " » lancée (pid " + std::to_string(instance->pid) + ").");
        if (const Account* account = AccountStore::get().forInstance(instance->id)) {
            AccountStore::get().markUsed(account->id);
        }
    } else {
        setStatus(error, true);
    }
}

void removeSelected() {
    auto& manager = InstanceManager::get();
    if (g_selected < 0 || g_selected >= static_cast<int>(manager.all().size())) return;

    const std::string id = manager.all()[static_cast<size_t>(g_selected)].id;
    const std::string name = manager.all()[static_cast<size_t>(g_selected)].name;

    const int answer = MessageBoxW(
        nullptr,
        strings::toUtf16("Supprimer l'instance « " + name +
                         " » ainsi que ses fichiers ?\nLes mondes qu'elle contient seront perdus.")
            .c_str(),
        L"Velyx", MB_YESNO | MB_ICONWARNING);
    if (answer != IDYES) return;

    std::string error;
    if (manager.remove(id, true, &error)) {
        setStatus("Instance supprimée.");
        g_selected = -1;
    } else {
        setStatus(error, true);
    }
}

void bindAccountToSelected() {
    auto& manager = InstanceManager::get();
    auto& accounts = AccountStore::get();

    if (g_selected < 0 || g_selected >= static_cast<int>(manager.all().size())) return;
    const Instance& instance = manager.all()[static_cast<size_t>(g_selected)];

    Account& account = accounts.create("Compte " + std::to_string(accounts.all().size() + 1));
    accounts.bind(account.id, instance.id);

    setStatus("Compte « " + account.label + " » associé à « " + instance.name +
              " ». Connectez-vous dans le jeu au premier lancement.");
}

void drawInstanceRow(const Rect& rect, const Instance& instance, int index) {
    const int id = ++g_nextWidget;
    const bool hovered = rect.contains(g_mouse);
    if (hovered) g_hotWidget = id;

    const bool selected = index == g_selected;
    const bool running = instance.running();

    fill(rect, selected ? Color::rgb8(27, 30, 34) : kPanel, 14.f);
    stroke(rect, selected ? Color::rgb8(51, 58, 66) : Color::rgb8(33, 36, 41), 14.f);

    static const Color kTints[4] = {palette::kMint, Color::rgb8(124, 140, 255),
                                    palette::kHoney, Color::rgb8(232, 112, 158)};
    const Color tint = kTints[index % 4];

    const Rect avatar{rect.left + 16.f, rect.center().y - 20.f, rect.left + 56.f,
                      rect.center().y + 20.f};
    fill(avatar, tint.withAlpha(0.10f), 12.f);
    stroke(avatar, tint.withAlpha(0.22f), 12.f);
    write(instance.name.substr(0, 1), avatar, tint, g_fontTitle);

    write(instance.name, Rect{rect.left + 70.f, rect.top + 16.f, rect.left + 260.f, rect.top + 38.f},
          kText, g_fontTitle);

    const Account* account = AccountStore::get().forInstance(instance.id);
    write(account ? account->label : "Aucun compte associé",
          Rect{rect.left + 70.f, rect.top + 38.f, rect.left + 260.f, rect.bottom - 14.f},
          kTextMuted, g_fontSmall);

    const Rect chip{rect.left + 276.f, rect.center().y - 15.f, rect.left + 386.f,
                    rect.center().y + 15.f};
    const bool menuOpen = g_menu == index;
    const bool chipHover = chip.contains(g_mouse);

    fill(chip, menuOpen || chipHover ? kSurfaceHover : kSurface, 9.f);
    stroke(chip, menuOpen ? kAccent.withAlpha(0.5f) : kBorder, 9.f);
    write(instance.gameVersion.empty() ? "version inconnue" : instance.gameVersion,
          Rect{chip.left + 10.f, chip.top, chip.right - 10.f, chip.bottom}, kText, g_fontSmall);

    if (chipHover && g_clicked) {
        g_selected = index;
        g_menu = menuOpen ? -1 : index;
        if (g_menu == index) g_versions = InstanceManager::availableVersions();
    }

    const Rect more{rect.left + 394.f, rect.center().y - 15.f, rect.left + 424.f,
                    rect.center().y + 15.f};
    const bool moreHover = more.contains(g_mouse);
    if (moreHover || g_rowMenu == index) fill(more, kSurfaceHover, 9.f);
    for (int d = 0; d < 3; ++d) {
        fill(Rect::fromSize(more.center().x - 6.f + static_cast<float>(d) * 5.f,
                            more.center().y - 1.5f, 3.f, 3.f),
             moreHover ? kText : kTextMuted, 1.5f);
    }
    if (moreHover && g_clicked) {
        g_selected = index;
        g_menu = -1;
        g_rowMenu = g_rowMenu == index ? -1 : index;
    }

    const Vec2 dot{rect.left + 442.f, rect.center().y};
    fill(Rect::fromSize(dot.x - 3.f, dot.y - 3.f, 6.f, 6.f), running ? kAccent : Color::rgb8(58, 64, 72), 3.f);

    std::string state = "Prête";
    if (running) {
        state = "En cours · pid " + std::to_string(instance.pid);
    } else if (!instance.registered) {
        state = "Non enregistrée";
    }
    write(state, Rect{rect.left + 456.f, rect.top, rect.left + 640.f, rect.bottom},
          running ? kAccent : kTextMuted, g_fontSmall);

    if (running) {
        const Rect stop{rect.right - 52.f, rect.center().y - 16.f, rect.right - 16.f,
                        rect.center().y + 16.f};
        fill(stop, stop.contains(g_mouse) ? kSurfaceHover : Color{0.f, 0.f, 0.f, 0.f}, 9.f);
        stroke(stop, kBorder, 9.f);
        fill(Rect::fromSize(stop.center().x - 5.f, stop.center().y - 5.f, 10.f, 10.f), kTextMuted, 2.f);

        const Rect focus{rect.right - 140.f, rect.center().y - 16.f, rect.right - 60.f,
                         rect.center().y + 16.f};
        fill(focus, focus.contains(g_mouse) ? kSurfaceHover : Color{0.f, 0.f, 0.f, 0.f}, 9.f);
        stroke(focus, kBorder, 9.f);
        write("Focus", focus, kTextMuted, g_fontBody);

        if (stop.contains(g_mouse) && g_clicked) {
            Process::terminate(instance.pid);
            setStatus("« " + instance.name + " » arrêtée.");
        }
    } else {
        const Rect play{rect.right - 116.f, rect.center().y - 16.f, rect.right - 16.f,
                        rect.center().y + 16.f};
        const bool playHover = play.contains(g_mouse);
        fill(play, playHover ? kAccent.lighten(0.06f) : kAccent, 9.f);
        write("Jouer", play, kAccent.readableForeground(), g_fontBody);

        if (playHover && g_clicked) {
            g_selected = index;
            launchSelected();
        }
    }

    if (hovered && g_clicked && !chipHover && !moreHover) g_selected = index;
}

void drawRowMenu(const Rect& rowRect, int index) {
    if (g_rowMenu != index) return;

    auto& manager = InstanceManager::get();
    if (index >= static_cast<int>(manager.all().size())) return;

    const Rect menu{rowRect.left + 394.f, rowRect.center().y + 18.f, rowRect.left + 604.f,
                    rowRect.center().y + 122.f};

    fill(menu.translated({0.f, 3.f}), Color::rgb8(0, 0, 0, 90), 12.f);
    fill(menu, Color::rgb8(23, 25, 29), 12.f);
    stroke(menu, Color::rgb8(46, 52, 59), 12.f);

    static const char* kLabels[3] = {"Associer un compte", "Ouvrir le dossier", "Supprimer"};

    float y = menu.top + 6.f;
    for (int i = 0; i < 3; ++i) {
        const Rect item{menu.left + 4.f, y, menu.right - 4.f, y + 30.f};
        y += 30.f;

        const bool itemHover = item.contains(g_mouse);
        if (itemHover) fill(item, kSurfaceHover, 8.f);

        write(kLabels[i], Rect{item.left + 10.f, item.top, item.right - 10.f, item.bottom},
              i == 2 ? kDanger : kTextMuted, g_fontSmall);

        if (!itemHover || !g_clicked) continue;

        g_rowMenu = -1;
        g_selected = index;

        if (i == 0) {
            bindAccountToSelected();
        } else if (i == 1) {
            ShellExecuteW(nullptr, L"open",
                          manager.all()[static_cast<size_t>(index)].root.wstring().c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
        } else {
            removeSelected();
        }
        return;
    }
}

void drawVersionMenu(const Rect& rowRect, int index) {
    if (g_menu != index) return;

    const float height = 30.f + static_cast<float>(g_versions.size()) * 30.f + 40.f;
    const Rect menu{rowRect.left + 276.f, rowRect.center().y + 18.f, rowRect.left + 484.f,
                    rowRect.center().y + 18.f + height};

    fill(menu.translated({0.f, 3.f}), Color::rgb8(0, 0, 0, 90), 12.f);
    fill(menu, Color::rgb8(23, 25, 29), 12.f);
    stroke(menu, Color::rgb8(46, 52, 59), 12.f);

    write("VERSIONS DISPONIBLES",
          Rect{menu.left + 12.f, menu.top + 6.f, menu.right - 12.f, menu.top + 28.f}, kTextMuted,
          g_fontSmall);

    auto& manager = InstanceManager::get();
    Instance* instance = manager.find(manager.all()[static_cast<size_t>(index)].id);

    float y = menu.top + 30.f;
    for (const auto& source : g_versions) {
        const Rect item{menu.left + 4.f, y, menu.right - 4.f, y + 30.f};
        y += 30.f;

        const bool current = instance && source.version == instance->gameVersion;
        const bool itemHover = item.contains(g_mouse);

        if (itemHover) fill(item, kSurfaceHover, 8.f);

        write(source.version, Rect{item.left + 10.f, item.top, item.right - 40.f, item.bottom},
              current ? kText : kTextMuted, g_fontSmall);

        if (source.installed) {
            write("installée", Rect{item.right - 90.f, item.top, item.right - 26.f, item.bottom},
                  Color::rgb8(90, 98, 108), g_fontSmall);
        }
        if (current) {
            fill(Rect::fromSize(item.right - 20.f, item.center().y - 3.f, 6.f, 6.f), kAccent, 3.f);
        }

        if (itemHover && g_clicked && instance && !current) {
            g_menu = -1;
            setStatus("Passage de « " + instance->name + " » en " + source.version + "…");

            std::string error;
            if (manager.setVersion(*instance, source, nullptr, &error)) {
                setStatus("« " + instance->name + " » est maintenant en " + source.version + ".");
            } else {
                setStatus(error, true);
            }
        }
    }

    const Rect add{menu.left + 4.f, y + 4.f, menu.right - 4.f, y + 34.f};
    if (add.contains(g_mouse)) fill(add, kSurfaceHover, 8.f);
    write("Ajouter un dossier de version",
          Rect{add.left + 10.f, add.top, add.right - 10.f, add.bottom}, kTextMuted, g_fontSmall);

    if (add.contains(g_mouse) && g_clicked) {
        std::error_code ec;
        std::filesystem::create_directories(Paths::versions(), ec);
        ShellExecuteW(nullptr, L"open", Paths::versions().wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        setStatus("Déposez un dossier de jeu décompressé dans versions/, puis rouvrez ce menu.");
        g_menu = -1;
    }
}

void render(const Rect& client) {
    g_nextWidget = 0;
    g_hotWidget = 0;

    auto& manager = InstanceManager::get();

    fill(client, kBackground);

    const Rect title{client.left, client.top, client.right, client.top + 38.f};
    fill(title, kPanel);
    fill(Rect{title.left, title.bottom - 1.f, title.right, title.bottom}, Color::rgb8(30, 34, 39));

    const Vec2 mark{title.left + 22.f, title.center().y};
    g_brush->SetColor(toD2D(kAccent));
    g_target->DrawLine(D2D1::Point2F(mark.x - 7.f, mark.y - 7.f),
                       D2D1::Point2F(mark.x, mark.y + 7.f), g_brush, 2.4f);
    g_target->DrawLine(D2D1::Point2F(mark.x, mark.y + 7.f),
                       D2D1::Point2F(mark.x + 7.f, mark.y - 7.f), g_brush, 2.4f);

    write("VELYX", Rect{title.left + 38.f, title.top, title.left + 140.f, title.bottom}, kText,
          g_fontSmall);

    const Rect header{client.left + 24.f, title.bottom + 22.f, client.right - 24.f,
                      title.bottom + 68.f};

    write("Instances", Rect{header.left, header.top, header.left + 220.f, header.top + 30.f}, kText,
          g_fontHeading);

    const int live = static_cast<int>(std::ranges::count_if(
        manager.all(), [](const Instance& i) { return i.running(); }));
    write(std::to_string(manager.all().size()) + " instances · " + std::to_string(live) +
              " en cours · " + std::to_string(AccountStore::get().all().size()) + " comptes",
          Rect{header.left, header.top + 26.f, header.left + 320.f, header.bottom}, kTextMuted,
          g_fontSmall);

    const Rect nameField{header.right - 396.f, header.center().y - 17.f, header.right - 178.f,
                         header.center().y + 17.f};
    fill(nameField, kSurface, 999.f);
    stroke(nameField, g_editingName ? kAccent.withAlpha(0.6f) : kBorder, 999.f);
    write(g_newInstanceName.empty() ? "Nom de la nouvelle instance" : g_newInstanceName,
          Rect{nameField.left + 14.f, nameField.top, nameField.right - 12.f, nameField.bottom},
          g_newInstanceName.empty() ? Color::rgb8(92, 100, 110) : kText, g_fontBody);

    if (nameField.contains(g_mouse) && g_clicked) g_editingName = true;
    else if (g_clicked && !nameField.contains(g_mouse)) g_editingName = false;

    if (button(Rect{header.right - 168.f, header.center().y - 17.f, header.right,
                    header.center().y + 17.f},
               "Nouvelle instance", true, !g_busy)) {
        createInstance();
    }

    const Rect list{client.left + 24.f, header.bottom + 16.f, client.right - 24.f,
                    client.bottom - 62.f};

    if (manager.all().empty()) {
        fill(list, kPanel.withAlpha(0.6f), 14.f);
        write("Aucune instance pour l'instant.\n"
              "Velyx copie le jeu installé par liens durs, lui donne une identité de paquet "
              "distincte, et Windows accepte alors de les lancer en parallèle.",
              list.inflated(-28.f), kTextMuted, g_fontBody);
    } else {
        float y = list.top - g_scroll;
        for (size_t i = 0; i < manager.all().size(); ++i) {
            const Rect row{list.left, y, list.right, y + 76.f};
            y += 86.f;
            if (row.bottom < list.top || row.top > list.bottom) continue;
            drawInstanceRow(row, manager.all()[i], static_cast<int>(i));
        }

        y = list.top - g_scroll;
        for (size_t i = 0; i < manager.all().size(); ++i) {
            const Rect row{list.left, y, list.right, y + 76.f};
            y += 86.f;
            drawVersionMenu(row, static_cast<int>(i));
            drawRowMenu(row, static_cast<int>(i));
        }
    }

    const Rect status{client.left, client.bottom - 46.f, client.right, client.bottom};
    fill(status, kPanel);
    fill(Rect{status.left, status.top, status.right, status.top + 1.f}, Color::rgb8(30, 34, 39));

    write(g_status, Rect{status.left + 24.f, status.top, status.right - 240.f, status.bottom},
          g_statusIsError ? kDanger : kTextMuted, g_fontSmall);

    const bool devMode = InstanceManager::developerModeEnabled();
    const Rect pill{status.right - 220.f, status.center().y - 12.f, status.right - 24.f,
                    status.center().y + 12.f};
    fill(pill, (devMode ? kAccent : kDanger).withAlpha(0.10f), 999.f);
    stroke(pill, (devMode ? kAccent : kDanger).withAlpha(0.22f), 999.f);
    write(devMode ? "Mode développeur actif" : "Mode développeur désactivé", pill,
          devMode ? kAccent : kDanger, g_fontSmall);
}

IDWriteTextFormat* makeFormat(float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* format = nullptr;
    if (FAILED(g_dwriteFactory->CreateTextFormat(L"Space Grotesk", nullptr, weight,
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

    g_fontHeading = makeFormat(20.f, DWRITE_FONT_WEIGHT_BOLD);
    g_fontTitle = makeFormat(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fontBody = makeFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fontSmall = makeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL);

    return g_fontHeading && g_fontTitle && g_fontBody && g_fontSmall;
}

void destroyGraphics() {
    const auto release = [](auto*& pointer) {
        if (pointer) {
            pointer->Release();
            pointer = nullptr;
        }
    };

    release(g_fontSmall);
    release(g_fontBody);
    release(g_fontTitle);
    release(g_fontHeading);
    release(g_brush);
    release(g_target);
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
            if (g_editingName) {
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
            InstanceManager::get().refreshRunningState();
            InvalidateRect(window, nullptr, FALSE);
            return 0;

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

    InstanceManager::get().load();
    AccountStore::get().load();

    if (!InstanceManager::developerModeEnabled()) {
        setStatus("Activez le mode développeur de Windows pour créer des instances.", true);
    } else if (!InstanceManager::findInstalledGame()) {
        setStatus("Minecraft Bedrock est introuvable. Installez-le depuis le Microsoft Store.", true);
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
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

    ShowWindow(window, SW_SHOW);
    SetTimer(window, 1, 1000, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    destroyGraphics();
    Log::shutdown();
    CoUninitialize();

    return 0;
}
