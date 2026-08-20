#pragma once

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wincodec.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Color.hpp"
#include "core/Math.hpp"
#include "dll/render/ComPtr.hpp"
#include "dll/render/Font.hpp"

namespace velyx {

class Renderer {
public:
    Renderer() = default;

    bool begin(ID2D1DeviceContext* context, Vec2 screenSize, float deltaSeconds);
    void end();

    [[nodiscard]] ID2D1DeviceContext* context() const { return context_; }
    [[nodiscard]] Vec2 screenSize() const { return screenSize_; }
    [[nodiscard]] float delta() const { return delta_; }
    [[nodiscard]] bool drawing() const { return context_ != nullptr; }

    void pushClip(const Rect& rect);
    void pushClipRounded(const Rect& rect, float radius);
    void popClip();

    void pushRotation(float degrees, Vec2 pivot);
    void pushScale(Vec2 scale, Vec2 pivot);
    void pushTransform(const D2D1_MATRIX_3X2_F& matrix);
    void popTransform();

    void pushOpacity(float opacity);
    void popOpacity();
    [[nodiscard]] float opacity() const { return opacityStack_.empty() ? 1.f : opacityStack_.back(); }

    void fillRect(const Rect& rect, const Color& color);
    void strokeRect(const Rect& rect, const Color& color, float thickness = 1.f);
    void fillRounded(const Rect& rect, const Color& color, float radius);
    void strokeRounded(const Rect& rect, const Color& color, float radius, float thickness = 1.f);

    void fillRoundedCorners(const Rect& rect, const Color& color, float topLeft, float topRight,
                            float bottomRight, float bottomLeft);

    void fillGradient(const Rect& rect, const Color& from, const Color& to, float angleDegrees,
                      float radius = 0.f);

    void fillCircle(Vec2 center, float radius, const Color& color);
    void strokeCircle(Vec2 center, float radius, const Color& color, float thickness = 1.f);

    void arc(Vec2 center, float radius, float thickness, float startDegrees, float sweepDegrees,
             const Color& color);

    void line(Vec2 from, Vec2 to, const Color& color, float thickness = 1.f);
    void polyline(const std::vector<Vec2>& points, const Color& color, float thickness = 1.f,
                  bool closed = false);
    void fillPolygon(const std::vector<Vec2>& points, const Color& color);

    void dropShadow(const Rect& rect, const Color& color, float radius, float spread,
                    Vec2 offset = {0.f, 2.f});

    void text(std::string_view value, const Rect& bounds, const Color& color, const FontSpec& spec);

    void textShadowed(std::string_view value, const Rect& bounds, const Color& color,
                      const FontSpec& spec, Vec2 offset = {1.f, 1.f});

    void formattedText(std::string_view value, const Rect& bounds, const Color& baseColor,
                       const FontSpec& spec, bool shadow = true);

    Vec2 measure(std::string_view value, const FontSpec& spec, float maxWidth = 1e6f);

    ID2D1Bitmap1* image(const std::filesystem::path& path);
    void drawImage(ID2D1Bitmap1* bitmap, const Rect& destination, float alpha = 1.f);
    void drawImageRounded(ID2D1Bitmap1* bitmap, const Rect& destination, float radius,
                          float alpha = 1.f);

    void blurBehind(const Rect& rect, float sigma, float radius = 0.f);

    void colorMatrix(const Rect& rect, const float matrix[20]);

    void setEffectsEnabled(bool enabled) { effectsEnabled_ = enabled; }
    [[nodiscard]] bool effectsEnabled() const { return effectsEnabled_; }

    void onDeviceLost();

    struct FrameStats {
        int drawCalls = 0;
        int textDraws = 0;
        int blurs = 0;
    };
    [[nodiscard]] const FrameStats& stats() const { return stats_; }

private:
    ID2D1SolidColorBrush* solid(const Color& color);
    ID2D1LinearGradientBrush* gradient(const Color& from, const Color& to);
    [[nodiscard]] Color applyOpacity(const Color& color) const;
    void ensureWic();

    ID2D1DeviceContext* context_ = nullptr;
    Vec2 screenSize_;
    float delta_ = 0.f;

    ComPtr<ID2D1SolidColorBrush> solidBrush_;
    ComPtr<ID2D1LinearGradientBrush> gradientBrush_;
    Color gradientFrom_;
    Color gradientTo_;

    ComPtr<IWICImagingFactory> wic_;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>> images_;

    ComPtr<ID2D1Effect> blurEffect_;
    ComPtr<ID2D1Effect> matrixEffect_;
    ComPtr<ID2D1Bitmap1> blurSnapshot_;
    Vec2 blurSnapshotSize_;

    bool snapshot(const Rect& rect);

    int clipDepth_ = 0;
    std::vector<D2D1_MATRIX_3X2_F> transformStack_;
    std::vector<float> opacityStack_;
    std::vector<bool> clipIsLayer_;

    bool effectsEnabled_ = true;
    FrameStats stats_;
};

}
