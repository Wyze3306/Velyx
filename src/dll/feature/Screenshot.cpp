#include "Screenshot.hpp"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <vector>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/render/ComPtr.hpp"
#include "dll/render/GraphicsContext.hpp"

namespace velyx::screenshot {
namespace {

constexpr const char* kLog = "Screenshot";

std::tm localNow() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &time);
    return tm;
}

Result fail(std::string message) {
    Log::error(kLog, "{}", message);
    return Result{false, {}, std::move(message)};
}

} // namespace

std::filesystem::path suggestedPath(std::string_view server) {
    const std::tm tm = localNow();

    char day[16]{};
    std::snprintf(day, sizeof(day), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char name[32]{};
    std::snprintf(name, sizeof(name), "velyx-%02d%02d%02d.png", tm.tm_hour, tm.tm_min, tm.tm_sec);

    const std::string folder = server.empty() ? "Solo" : Paths::sanitize(server);
    return Paths::screenshots() / folder / day / name;
}

Result capture(const std::filesystem::path& destination) {
    GraphicsContext& graphics = GraphicsContext::get();

    ID2D1DeviceContext* context = graphics.d2d();
    if (!context || !graphics.ready()) return fail("le rendu n'est pas prêt");

    const auto width = static_cast<UINT32>(graphics.size().x);
    const auto height = static_cast<UINT32>(graphics.size().y);
    if (width == 0 || height == 0) return fail("taille d'écran invalide");

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> staging;
    if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties,
                                     staging.put()))) {
        return fail("impossible d'allouer le tampon de capture");
    }

    const D2D1_POINT_2U origin{0, 0};
    const D2D1_RECT_U source{0, 0, width, height};
    if (FAILED(staging->CopyFromRenderTarget(&origin, context, &source))) {
        return fail("copie du back buffer impossible");
    }

    D2D1_MAPPED_RECT mapped{};
    if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        return fail("lecture du tampon impossible");
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (UINT32 y = 0; y < height; ++y) {
        const uint8_t* sourceRow = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        uint8_t* destinationRow = pixels.data() + static_cast<size_t>(y) * width * 4;

        std::memcpy(destinationRow, sourceRow, static_cast<size_t>(width) * 4);
        for (UINT32 x = 0; x < width; ++x) destinationRow[x * 4 + 3] = 0xFF;
    }
    staging->Unmap();

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IWICImagingFactory),
                                reinterpret_cast<void**>(wic.put())))) {
        return fail("WIC indisponible");
    }

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(destination.wstring().c_str(), GENERIC_WRITE))) {
        return fail("écriture impossible dans " + destination.string());
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        return fail("encodeur PNG indisponible");
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(frame.put(), options.put())) ||
        FAILED(frame->Initialize(options.get()))) {
        return fail("initialisation de l'image impossible");
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format))) {
        return fail("format de sortie refusé");
    }

    if (FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()),
                                  pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return fail("encodage PNG interrompu");
    }

    Log::info(kLog, "screenshot saved to {}", destination.string());
    return Result{true, destination, {}};
}

std::vector<Entry> gallery(size_t limit) {
    std::vector<Entry> entries;

    std::error_code ec;
    if (!std::filesystem::exists(Paths::screenshots(), ec)) return entries;

    for (const auto& item :
         std::filesystem::recursive_directory_iterator(Paths::screenshots(), ec)) {
        if (!item.is_regular_file(ec)) continue;
        if (strings::toLower(item.path().extension().string()) != ".png") continue;

        Entry entry;
        entry.path = item.path();
        entry.sizeBytes = static_cast<long long>(item.file_size(ec));

        const auto day = item.path().parent_path();
        entry.day = day.filename().string();
        entry.server = day.parent_path().filename().string();

        const auto written = std::filesystem::last_write_time(item.path(), ec);
        entry.modifiedMs = static_cast<long long>(written.time_since_epoch().count() / 10000);

        entries.push_back(std::move(entry));
    }

    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        return a.modifiedMs > b.modifiedMs;
    });

    if (entries.size() > limit) entries.resize(limit);
    return entries;
}

bool revealInExplorer(const std::filesystem::path& path) {
    const std::wstring argument = L"/select,\"" + path.wstring() + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                                   argument.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

} // namespace velyx::screenshot
