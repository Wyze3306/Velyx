#include "Renderer.hpp"

#include <algorithm>
#include <cstring>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Renderer";

D2D1_COLOR_F toD2D(const Color& color) { return D2D1::ColorF(color.r, color.g, color.b, color.a); }

D2D1_RECT_F toD2D(const Rect& rect) {
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

D2D1_POINT_2F toD2D(const Vec2& point) { return D2D1::Point2F(point.x, point.y); }

float clampRadius(const Rect& rect, float radius) {
    return clamp(radius, 0.f, std::min(rect.width(), rect.height()) * 0.5f);
}

const Color kFormatColors[16] = {
    Color::rgb8(0, 0, 0),       Color::rgb8(0, 0, 170),     Color::rgb8(0, 170, 0),
    Color::rgb8(0, 170, 170),   Color::rgb8(170, 0, 0),     Color::rgb8(170, 0, 170),
    Color::rgb8(255, 170, 0),   Color::rgb8(170, 170, 170), Color::rgb8(85, 85, 85),
    Color::rgb8(85, 85, 255),   Color::rgb8(85, 255, 85),   Color::rgb8(85, 255, 255),
    Color::rgb8(255, 85, 85),   Color::rgb8(255, 85, 255),  Color::rgb8(255, 255, 85),
    Color::rgb8(255, 255, 255),
};

int formatCodeIndex(char code) {
    if (code >= '0' && code <= '9') return code - '0';
    if (code >= 'a' && code <= 'f') return code - 'a' + 10;
    if (code >= 'A' && code <= 'F') return code - 'A' + 10;
    return -1;
}

}

bool Renderer::begin(ID2D1DeviceContext* context, Vec2 screenSize, float deltaSeconds) {
    if (!context) return false;

    context_ = context;
    screenSize_ = screenSize;
    delta_ = deltaSeconds;
    stats_ = {};

    if (!solidBrush_) {
        if (FAILED(context_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                   solidBrush_.put()))) {
            Log::error(kLog, "création du pinceau impossible");
            context_ = nullptr;
            return false;
        }
    }

    transformStack_.clear();
    opacityStack_.clear();
    clipIsLayer_.clear();
    clipDepth_ = 0;

    return true;
}

void Renderer::end() {

    if (clipDepth_ != 0) {
        Log::warn(kLog, "pile de découpe déséquilibrée ({} restantes)", clipDepth_);
        while (clipDepth_ > 0) popClip();
    }
    if (!transformStack_.empty()) {
        Log::warn(kLog, "pile de transformations déséquilibrée ({} restantes)", transformStack_.size());
        while (!transformStack_.empty()) popTransform();
    }

    opacityStack_.clear();
    context_ = nullptr;
}

void Renderer::onDeviceLost() {
    solidBrush_.reset();
    gradientBrush_.reset();
    blurEffect_.reset();
    matrixEffect_.reset();
    blurSnapshot_.reset();
    images_.clear();
    context_ = nullptr;
}

Color Renderer::applyOpacity(const Color& color) const {
    const float factor = opacity();
    return factor >= 1.f ? color : color.fade(factor);
}

ID2D1SolidColorBrush* Renderer::solid(const Color& color) {
    if (!solidBrush_) return nullptr;
    solidBrush_->SetColor(toD2D(applyOpacity(color)));
    ++stats_.drawCalls;
    return solidBrush_.get();
}

ID2D1LinearGradientBrush* Renderer::gradient(const Color& from, const Color& to) {
    if (!context_) return nullptr;

    if (gradientBrush_ && gradientFrom_ == from && gradientTo_ == to) return gradientBrush_.get();

    const D2D1_GRADIENT_STOP stops[2] = {
        {0.f, toD2D(from)},
        {1.f, toD2D(to)},
    };

    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(context_->CreateGradientStopCollection(stops, 2, collection.put()))) return nullptr;

    gradientBrush_.reset();
    if (FAILED(context_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F()),
            collection.get(), gradientBrush_.put()))) {
        return nullptr;
    }

    gradientFrom_ = from;
    gradientTo_ = to;
    return gradientBrush_.get();
}

void Renderer::pushClip(const Rect& rect) {
    if (!context_) return;
    context_->PushAxisAlignedClip(toD2D(rect), D2D1_ANTIALIAS_MODE_ALIASED);
    clipIsLayer_.push_back(false);
    ++clipDepth_;
}

void Renderer::pushClipRounded(const Rect& rect, float radius) {
    if (!context_) return;

    ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.put());

    ComPtr<ID2D1Factory1> factory1;
    if (FAILED(factory->QueryInterface(__uuidof(ID2D1Factory1),
                                       reinterpret_cast<void**>(factory1.put())))) {
        pushClip(rect);
        return;
    }

    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    const D2D1_ROUNDED_RECT rounded{toD2D(rect), clampRadius(rect, radius),
                                    clampRadius(rect, radius)};
    if (FAILED(factory1->CreateRoundedRectangleGeometry(rounded, geometry.put()))) {
        pushClip(rect);
        return;
    }

    context_->PushLayer(D2D1::LayerParameters(toD2D(rect), geometry.get()), nullptr);
    clipIsLayer_.push_back(true);
    ++clipDepth_;
}

void Renderer::popClip() {
    if (!context_ || clipDepth_ == 0) return;

    const bool isLayer = clipIsLayer_.back();
    clipIsLayer_.pop_back();
    --clipDepth_;

    if (isLayer) {
        context_->PopLayer();
    } else {
        context_->PopAxisAlignedClip();
    }
}

void Renderer::pushTransform(const D2D1_MATRIX_3X2_F& matrix) {
    if (!context_) return;

    D2D1_MATRIX_3X2_F current{};
    context_->GetTransform(&current);
    transformStack_.push_back(current);

    context_->SetTransform(matrix * current);
}

void Renderer::pushRotation(float degrees, Vec2 pivot) {
    pushTransform(D2D1::Matrix3x2F::Rotation(degrees, toD2D(pivot)));
}

void Renderer::pushScale(Vec2 scale, Vec2 pivot) {
    pushTransform(D2D1::Matrix3x2F::Scale(scale.x, scale.y, toD2D(pivot)));
}

void Renderer::popTransform() {
    if (!context_ || transformStack_.empty()) return;
    context_->SetTransform(transformStack_.back());
    transformStack_.pop_back();
}

void Renderer::pushOpacity(float value) {
    opacityStack_.push_back(clamp(opacity() * value, 0.f, 1.f));
}

void Renderer::popOpacity() {
    if (!opacityStack_.empty()) opacityStack_.pop_back();
}

void Renderer::fillRect(const Rect& rect, const Color& color) {
    if (!context_ || color.a <= 0.f) return;
    context_->FillRectangle(toD2D(rect), solid(color));
}

void Renderer::strokeRect(const Rect& rect, const Color& color, float thickness) {
    if (!context_ || color.a <= 0.f) return;

    context_->DrawRectangle(toD2D(rect.inflated(-thickness * 0.5f)), solid(color), thickness);
}

void Renderer::fillRounded(const Rect& rect, const Color& color, float radius) {
    if (!context_ || color.a <= 0.f) return;

    if (radius <= 0.f) {
        fillRect(rect, color);
        return;
    }

    const float r = clampRadius(rect, radius);
    context_->FillRoundedRectangle(D2D1_ROUNDED_RECT{toD2D(rect), r, r}, solid(color));
}

void Renderer::strokeRounded(const Rect& rect, const Color& color, float radius, float thickness) {
    if (!context_ || color.a <= 0.f) return;

    const Rect inset = rect.inflated(-thickness * 0.5f);
    const float r = clampRadius(inset, radius);
    context_->DrawRoundedRectangle(D2D1_ROUNDED_RECT{toD2D(inset), r, r}, solid(color), thickness);
}

void Renderer::fillRoundedCorners(const Rect& rect, const Color& color, float topLeft,
                                  float topRight, float bottomRight, float bottomLeft) {
    if (!context_ || color.a <= 0.f) return;

    ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.put());

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(path.put()))) return;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;

    const float maxRadius = std::min(rect.width(), rect.height()) * 0.5f;
    const float tl = clamp(topLeft, 0.f, maxRadius);
    const float tr = clamp(topRight, 0.f, maxRadius);
    const float br = clamp(bottomRight, 0.f, maxRadius);
    const float bl = clamp(bottomLeft, 0.f, maxRadius);

    const auto arcTo = [&](float x, float y, float r) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(r, r), 0.f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    };

    sink->BeginFigure(D2D1::Point2F(rect.left + tl, rect.top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(rect.right - tr, rect.top));
    if (tr > 0.f) arcTo(rect.right, rect.top + tr, tr);
    sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - br));
    if (br > 0.f) arcTo(rect.right - br, rect.bottom, br);
    sink->AddLine(D2D1::Point2F(rect.left + bl, rect.bottom));
    if (bl > 0.f) arcTo(rect.left, rect.bottom - bl, bl);
    sink->AddLine(D2D1::Point2F(rect.left, rect.top + tl));
    if (tl > 0.f) arcTo(rect.left + tl, rect.top, tl);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    context_->FillGeometry(path.get(), solid(color));
}

void Renderer::fillGradient(const Rect& rect, const Color& from, const Color& to,
                            float angleDegrees, float radius) {
    if (!context_) return;

    ID2D1LinearGradientBrush* brush = gradient(applyOpacity(from), applyOpacity(to));
    if (!brush) {
        fillRounded(rect, from, radius);
        return;
    }

    const float radians = toRadians(angleDegrees);
    const Vec2 center = rect.center();
    const Vec2 half{rect.width() * 0.5f, rect.height() * 0.5f};
    const Vec2 direction{std::cos(radians), std::sin(radians)};
    const float extent = std::abs(direction.x) * half.x + std::abs(direction.y) * half.y;

    brush->SetStartPoint(toD2D(center - direction * extent));
    brush->SetEndPoint(toD2D(center + direction * extent));

    ++stats_.drawCalls;

    if (radius > 0.f) {
        const float r = clampRadius(rect, radius);
        context_->FillRoundedRectangle(D2D1_ROUNDED_RECT{toD2D(rect), r, r}, brush);
    } else {
        context_->FillRectangle(toD2D(rect), brush);
    }
}

void Renderer::fillCircle(Vec2 center, float radius, const Color& color) {
    if (!context_ || color.a <= 0.f) return;
    context_->FillEllipse(D2D1::Ellipse(toD2D(center), radius, radius), solid(color));
}

void Renderer::strokeCircle(Vec2 center, float radius, const Color& color, float thickness) {
    if (!context_ || color.a <= 0.f) return;
    context_->DrawEllipse(D2D1::Ellipse(toD2D(center), radius, radius), solid(color), thickness);
}

void Renderer::arc(Vec2 center, float radius, float thickness, float startDegrees,
                   float sweepDegrees, const Color& color) {
    if (!context_ || color.a <= 0.f || std::abs(sweepDegrees) < 0.01f) return;

    ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.put());

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(path.put()))) return;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;

    const float startRad = toRadians(startDegrees);
    const float endRad = toRadians(startDegrees + sweepDegrees);

    const Vec2 start{center.x + std::cos(startRad) * radius, center.y + std::sin(startRad) * radius};
    const Vec2 end{center.x + std::cos(endRad) * radius, center.y + std::sin(endRad) * radius};

    sink->BeginFigure(toD2D(start), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        toD2D(end), D2D1::SizeF(radius, radius), 0.f,
        sweepDegrees >= 0.f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                            : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
        std::abs(sweepDegrees) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    ComPtr<ID2D1StrokeStyle> style;
    ComPtr<ID2D1Factory1> factory1;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(ID2D1Factory1),
                                          reinterpret_cast<void**>(factory1.put())))) {
        factory1->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, style.put());
    }

    context_->DrawGeometry(path.get(), solid(color), thickness, style.get());
}

void Renderer::line(Vec2 from, Vec2 to, const Color& color, float thickness) {
    if (!context_ || color.a <= 0.f) return;
    context_->DrawLine(toD2D(from), toD2D(to), solid(color), thickness);
}

void Renderer::polyline(const std::vector<Vec2>& points, const Color& color, float thickness,
                        bool closed) {
    if (!context_ || points.size() < 2 || color.a <= 0.f) return;

    ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.put());

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(path.put()))) return;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;

    sink->BeginFigure(toD2D(points.front()), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < points.size(); ++i) sink->AddLine(toD2D(points[i]));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();

    ComPtr<ID2D1Factory1> factory1;
    ComPtr<ID2D1StrokeStyle> style;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(ID2D1Factory1),
                                          reinterpret_cast<void**>(factory1.put())))) {
        factory1->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, style.put());
    }

    context_->DrawGeometry(path.get(), solid(color), thickness, style.get());
}

void Renderer::fillPolygon(const std::vector<Vec2>& points, const Color& color) {
    if (!context_ || points.size() < 3 || color.a <= 0.f) return;

    ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.put());

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(path.put()))) return;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;

    sink->BeginFigure(toD2D(points.front()), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < points.size(); ++i) sink->AddLine(toD2D(points[i]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    context_->FillGeometry(path.get(), solid(color));
}

void Renderer::dropShadow(const Rect& rect, const Color& color, float radius, float spread,
                          Vec2 offset) {
    if (!context_ || !effectsEnabled_ || color.a <= 0.f || spread <= 0.f) return;

    const int steps = static_cast<int>(clamp(spread, 1.f, 24.f));
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float grow = spread * t;
        const float alpha = color.a * (1.f - t) * (1.f - t) * 0.6f;

        const Rect layer = rect.translated(offset).inflated(grow);
        fillRounded(layer, color.withAlpha(alpha), radius + grow);
    }
}

void Renderer::text(std::string_view value, const Rect& bounds, const Color& color,
                    const FontSpec& spec) {
    if (!context_ || value.empty() || color.a <= 0.f) return;

    IDWriteTextLayout* layout =
        FontManager::get().layout(value, spec, {bounds.width(), bounds.height()});
    if (!layout) return;

    ++stats_.textDraws;
    context_->DrawTextLayout(toD2D(bounds.topLeft()), layout, solid(color),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::textShadowed(std::string_view value, const Rect& bounds, const Color& color,
                            const FontSpec& spec, Vec2 offset) {
    const Color shadow{color.r * 0.25f, color.g * 0.25f, color.b * 0.25f, color.a * 0.85f};
    text(value, bounds.translated(offset), shadow, spec);
    text(value, bounds, color, spec);
}

void Renderer::formattedText(std::string_view value, const Rect& bounds, const Color& baseColor,
                             const FontSpec& spec, bool shadow) {
    if (!context_ || value.empty()) return;

    struct Run {
        std::string text;
        Color color;
        bool bold = false;
        bool italic = false;
    };

    std::vector<Run> runs;
    Run current{"", baseColor, spec.weight >= FontWeight::SemiBold, spec.italic};

    for (size_t i = 0; i < value.size();) {
        const auto byte = static_cast<unsigned char>(value[i]);
        const bool isSection =
            (byte == 0xA7 && i + 1 < value.size()) ||
            (byte == 0xC2 && i + 2 < value.size() &&
             static_cast<unsigned char>(value[i + 1]) == 0xA7);

        if (isSection) {
            const size_t codeIndex = byte == 0xA7 ? i + 1 : i + 2;
            const char code = value[codeIndex];

            if (!current.text.empty()) {
                runs.push_back(current);
                current.text.clear();
            }

            if (const int index = formatCodeIndex(code); index >= 0) {
                current.color = kFormatColors[index].withAlpha(baseColor.a);
            } else if (code == 'l' || code == 'L') {
                current.bold = true;
            } else if (code == 'o' || code == 'O') {
                current.italic = true;
            } else if (code == 'r' || code == 'R') {
                current.color = baseColor;
                current.bold = spec.weight >= FontWeight::SemiBold;
                current.italic = spec.italic;
            }

            i = codeIndex + 1;
            continue;
        }

        current.text.push_back(value[i]);
        ++i;
    }
    if (!current.text.empty()) runs.push_back(current);

    float x = bounds.left;
    for (const Run& run : runs) {
        FontSpec runSpec = spec;
        runSpec.weight = run.bold ? FontWeight::Bold : spec.weight;
        runSpec.italic = run.italic;
        runSpec.align = TextAlign::Left;

        const Vec2 size = measure(run.text, runSpec);
        const Rect runBounds{x, bounds.top, x + size.x + 1.f, bounds.bottom};

        if (shadow) {
            textShadowed(run.text, runBounds, run.color, runSpec);
        } else {
            text(run.text, runBounds, run.color, runSpec);
        }

        x += size.x;
        if (x > bounds.right) break;
    }
}

Vec2 Renderer::measure(std::string_view value, const FontSpec& spec, float maxWidth) {
    return FontManager::get().measure(value, spec, maxWidth);
}

void Renderer::ensureWic() {
    if (wic_) return;

    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IWICImagingFactory),
                                        reinterpret_cast<void**>(wic_.put()));
    if (FAILED(hr)) Log::warn(kLog, "WIC indisponible (0x{:08X}) : images désactivées",
                              static_cast<unsigned>(hr));
}

ID2D1Bitmap1* Renderer::image(const std::filesystem::path& path) {
    if (!context_) return nullptr;

    const std::string key = path.string();
    if (const auto it = images_.find(key); it != images_.end()) return it->second.get();

    ensureWic();
    if (!wic_) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnLoad, decoder.put()))) {
        images_.emplace(key, ComPtr<ID2D1Bitmap1>{});
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.put()))) return nullptr;

    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.f,
                                     WICBitmapPaletteTypeMedianCut))) {
        return nullptr;
    }

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(context_->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put()))) {
        return nullptr;
    }

    ID2D1Bitmap1* raw = bitmap.get();
    images_.emplace(key, std::move(bitmap));
    return raw;
}

void Renderer::drawImage(ID2D1Bitmap1* bitmap, const Rect& destination, float alpha) {
    if (!context_ || !bitmap) return;

    ++stats_.drawCalls;
    context_->DrawBitmap(bitmap, toD2D(destination), alpha * opacity(),
                         D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
}

void Renderer::drawImageRounded(ID2D1Bitmap1* bitmap, const Rect& destination, float radius,
                                float alpha) {
    if (!context_ || !bitmap) return;

    pushClipRounded(destination, radius);
    drawImage(bitmap, destination, alpha);
    popClip();
}

bool Renderer::snapshot(const Rect& rect) {
    if (!context_ || rect.width() <= 1.f || rect.height() <= 1.f) return false;

    const D2D1_SIZE_U size{static_cast<UINT32>(rect.width()), static_cast<UINT32>(rect.height())};

    if (!blurSnapshot_ || blurSnapshotSize_.x < rect.width() ||
        blurSnapshotSize_.y < rect.height()) {
        blurSnapshot_.reset();

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        if (FAILED(context_->CreateBitmap(size, nullptr, 0, properties, blurSnapshot_.put()))) {
            effectsEnabled_ = false;
            Log::warn(kLog, "effets desactives : allocation du tampon impossible");
            return false;
        }
        blurSnapshotSize_ = {rect.width(), rect.height()};
    }

    const D2D1_POINT_2U destination{0, 0};
    const D2D1_RECT_U source{
        static_cast<UINT32>(std::max(0.f, rect.left)),
        static_cast<UINT32>(std::max(0.f, rect.top)),
        static_cast<UINT32>(std::min(screenSize_.x, rect.right)),
        static_cast<UINT32>(std::min(screenSize_.y, rect.bottom)),
    };

    if (source.right <= source.left || source.bottom <= source.top) return false;
    return SUCCEEDED(blurSnapshot_->CopyFromRenderTarget(&destination, context_, &source));
}

void Renderer::blurBehind(const Rect& rect, float sigma, float radius) {
    if (!context_ || !effectsEnabled_ || sigma <= 0.f) return;
    if (!snapshot(rect)) return;

    if (!blurEffect_) {
        if (FAILED(context_->CreateEffect(CLSID_D2D1GaussianBlur, blurEffect_.put()))) {
            effectsEnabled_ = false;
            return;
        }
        blurEffect_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    }

    blurEffect_->SetInput(0, blurSnapshot_.get());
    blurEffect_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);

    ++stats_.blurs;

    if (radius > 0.f) pushClipRounded(rect, radius);
    context_->DrawImage(blurEffect_.get(), toD2D(rect.topLeft()));
    if (radius > 0.f) popClip();
}

void Renderer::colorMatrix(const Rect& rect, const float matrix[20]) {
    if (!context_ || !effectsEnabled_ || !matrix) return;
    if (!snapshot(rect)) return;

    if (!matrixEffect_) {
        if (FAILED(context_->CreateEffect(CLSID_D2D1ColorMatrix, matrixEffect_.put()))) {
            effectsEnabled_ = false;
            return;
        }
    }

    D2D1_MATRIX_5X4_F value{};
    std::memcpy(&value, matrix, sizeof(float) * 20);

    matrixEffect_->SetInput(0, blurSnapshot_.get());
    matrixEffect_->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, value);
    matrixEffect_->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);

    context_->DrawImage(matrixEffect_.get(), toD2D(rect.topLeft()));
}

}
