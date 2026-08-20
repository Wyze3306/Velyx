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

const Color kBackground = palette::kVoid;
const Color kPanel = palette::kForest;
const Color kSurface = palette::kMoss;
const Color kBorder = palette::kSage;
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

    fill(rect, selected ? Color::rgb8(18, 48, 31) : (hovered ? kSurface.fade(0.85f) : kPanel), 10.f);
    if (selected) stroke(rect, kAccent.fade(0.8f), 10.f, 1.5f);

    const Vec2 dot{rect.left + 22.f, rect.center().y};
    fill(Rect::fromSize(dot.x - 4.f, dot.y - 4.f, 8.f, 8.f),
         instance.running() ? kAccent : kTextMuted.fade(0.4f), 4.f);

    write(instance.name, Rect{rect.left + 42.f, rect.top + 12.f, rect.right - 200.f, rect.top + 36.f},
          kText, g_fontTitle);

    const Account* account = AccountStore::get().forInstance(instance.id);
    const std::string subtitle =
        std::string(account ? account->label : "Aucun compte associé") + "   ·   " +
        (instance.gameVersion.empty() ? "version inconnue" : instance.gameVersion) +
        (instance.registered ? "" : "   ·   non enregistrée");

    write(subtitle, Rect{rect.left + 42.f, rect.top + 36.f, rect.right - 200.f, rect.bottom - 10.f},
          kTextMuted, g_fontSmall);

    const std::string right =
        instance.running() ? "en cours" : (instance.lastPlayedMs > 0 ? "déjà jouée" : "jamais lancée");
    write(right, Rect{rect.right - 190.f, rect.top, rect.right - 20.f, rect.bottom},
          instance.running() ? kAccent : kTextMuted, g_fontSmall);

    if (hovered && g_clicked) g_selected = index;
}

void render(const Rect& client) {
    g_nextWidget = 0;
    g_hotWidget = 0;

    auto& manager = InstanceManager::get();

    fill(client, kBackground);

    const Rect header{client.left, client.top, client.right, client.top + 72.f};
    fill(header, kPanel);
    fill(Rect{header.left, header.bottom - 1.f, header.right, header.bottom}, kBorder.fade(0.7f));

    g_brush->SetColor(toD2D(kAccent));
    const Vec2 mark{header.left + 34.f, header.center().y};
    g_target->DrawLine(D2D1::Point2F(mark.x - 10.f, mark.y - 10.f),
                       D2D1::Point2F(mark.x, mark.y + 10.f), g_brush, 3.f);
    g_target->DrawLine(D2D1::Point2F(mark.x, mark.y + 10.f),
                       D2D1::Point2F(mark.x + 10.f, mark.y - 10.f), g_brush, 3.f);

    write("VELYX", Rect{header.left + 58.f, header.top + 16.f, header.left + 200.f,
                        header.top + 44.f},
          kText, g_fontHeading);
    write("Launcher " + std::string(version::kString),
          Rect{header.left + 58.f, header.top + 40.f, header.left + 260.f, header.bottom - 10.f},
          kTextMuted, g_fontSmall);

    const bool devMode = InstanceManager::developerModeEnabled();
    write(devMode ? "Mode développeur activé" : "Mode développeur désactivé",
          Rect{header.right - 300.f, header.top, header.right - 24.f, header.bottom},
          devMode ? kAccent : kDanger, g_fontSmall);

    const Rect body{client.left + 24.f, header.bottom + 20.f, client.right - 300.f,
                    client.bottom - 64.f};

    write("INSTANCES", Rect{body.left, body.top, body.right, body.top + 20.f}, kTextMuted,
          g_fontSmall);

    const Rect list{body.left, body.top + 26.f, body.right, body.bottom};

    if (manager.all().empty()) {
        fill(list, kPanel.fade(0.6f), 10.f);
        write("Aucune instance pour l'instant.\n"
              "Créez-en une à droite : Velyx copie le jeu installé, lui donne une identité "
              "de paquet distincte, et Windows accepte alors de la lancer en parallèle.",
              list.inflated(-24.f), kTextMuted, g_fontBody);
    } else {
        float y = list.top - g_scroll;
        for (size_t i = 0; i < manager.all().size(); ++i) {
            const Rect row{list.left, y, list.right, y + kRowHeight - 8.f};
            y += kRowHeight;

            if (row.bottom < list.top || row.top > list.bottom) continue;
            drawInstanceRow(row, manager.all()[i], static_cast<int>(i));
        }
    }

    const Rect side{client.right - 276.f, header.bottom + 20.f, client.right - 24.f,
                    client.bottom - 64.f};

    write("NOUVELLE INSTANCE", Rect{side.left, side.top, side.right, side.top + 20.f}, kTextMuted,
          g_fontSmall);

    const Rect nameField{side.left, side.top + 26.f, side.right, side.top + 62.f};
    fill(nameField, kSurface, 8.f);
    stroke(nameField, g_editingName ? kAccent : kBorder.fade(0.6f), 8.f);

    if (nameField.contains(g_mouse) && g_clicked) g_editingName = true;
    else if (g_clicked && !nameField.contains(g_mouse)) g_editingName = false;

    write(g_newInstanceName.empty() ? "Nom de l'instance" : g_newInstanceName,
          Rect{nameField.left + 12.f, nameField.top, nameField.right - 12.f, nameField.bottom},
          g_newInstanceName.empty() ? kTextMuted.fade(0.7f) : kText, g_fontBody);

    if (button(Rect{side.left, side.top + 72.f, side.right, side.top + 110.f}, "Créer l'instance",
               true, !g_busy)) {
        createInstance();
    }

    write("ACTIONS", Rect{side.left, side.top + 130.f, side.right, side.top + 150.f}, kTextMuted,
          g_fontSmall);

    const bool hasSelection = g_selected >= 0 &&
                              g_selected < static_cast<int>(manager.all().size());

    if (button(Rect{side.left, side.top + 156.f, side.right, side.top + 194.f}, "Lancer", true,
               hasSelection && !g_busy)) {
        launchSelected();
    }
    if (button(Rect{side.left, side.top + 202.f, side.right, side.top + 238.f},
               "Associer un compte", false, hasSelection)) {
        bindAccountToSelected();
    }
    if (button(Rect{side.left, side.top + 246.f, side.right, side.top + 282.f},
               "Ouvrir le dossier", false, hasSelection)) {
        const auto& instance = manager.all()[static_cast<size_t>(g_selected)];
        ShellExecuteW(nullptr, L"open", instance.root.wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
    if (button(Rect{side.left, side.top + 290.f, side.right, side.top + 326.f}, "Supprimer", false,
               hasSelection)) {
        removeSelected();
    }

    write("COMPTES", Rect{side.left, side.top + 348.f, side.right, side.top + 368.f}, kTextMuted,
          g_fontSmall);

    float accountY = side.top + 374.f;
    for (const Account* account : AccountStore::get().recent()) {
        if (accountY + 30.f > side.bottom) break;

        const Rect row{side.left, accountY, side.right, accountY + 28.f};
        accountY += 32.f;

        const Color chip = Color::fromHex(account->colorHex, kAccent);
        fill(Rect{row.left, row.top + 8.f, row.left + 10.f, row.top + 18.f}, chip, 5.f);

        write(account->label, Rect{row.left + 18.f, row.top, row.right - 60.f, row.bottom}, kText,
              g_fontSmall);
        write(account->instanceId.empty() ? "libre" : "liée",
              Rect{row.right - 60.f, row.top, row.right, row.bottom}, kTextMuted, g_fontSmall);
    }

    const Rect status{client.left, client.bottom - 44.f, client.right, client.bottom};
    fill(status, kPanel);
    fill(Rect{status.left, status.top, status.right, status.top + 1.f}, kBorder.fade(0.7f));

    write(g_status, Rect{status.left + 24.f, status.top, status.right - 24.f, status.bottom},
          g_statusIsError ? kDanger : kTextMuted, g_fontSmall);
}

IDWriteTextFormat* makeFormat(float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* format = nullptr;
    if (FAILED(g_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, weight,
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
