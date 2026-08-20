#pragma once

#include <dwrite_3.h>

#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Math.hpp"
#include "dll/render/ComPtr.hpp"

namespace velyx {

enum class FontWeight {
    Thin = 100,
    ExtraLight = 200,
    Light = 300,
    Regular = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
    ExtraBold = 800,
    Black = 900,
};

enum class TextAlign { Left, Center, Right };
enum class TextVAlign { Top, Middle, Bottom };

struct FontSpec {
    std::string family = "Space Grotesk";
    float size = 16.f;
    FontWeight weight = FontWeight::Regular;
    bool italic = false;
    float letterSpacing = 0.f;
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Middle;
    bool wrap = false;

    bool operator==(const FontSpec& o) const {
        return family == o.family && size == o.size && weight == o.weight &&
               italic == o.italic && letterSpacing == o.letterSpacing && align == o.align &&
               valign == o.valign && wrap == o.wrap;
    }
};

class FontManager {
public:
    static FontManager& get();

    bool initialize(IDWriteFactory5* factory);
    void shutdown();

    int loadDirectory(const std::filesystem::path& directory);
    bool loadFile(const std::filesystem::path& file);

    [[nodiscard]] std::vector<std::string> availableFamilies() const;

    IDWriteTextFormat* format(const FontSpec& spec);

    IDWriteTextLayout* layout(std::string_view text, const FontSpec& spec, Vec2 maxSize);

    Vec2 measure(std::string_view text, const FontSpec& spec, float maxWidth = 1e6f);

    void invalidate();

    [[nodiscard]] size_t layoutCacheSize() const { return layoutOrder_.size(); }

private:
    FontManager() = default;

    void rebuildCollection();

    struct LayoutKey {
        std::string text;
        FontSpec spec;
        float maxWidth = 0.f;
        float maxHeight = 0.f;

        bool operator==(const LayoutKey& o) const {
            return text == o.text && spec == o.spec && maxWidth == o.maxWidth &&
                   maxHeight == o.maxHeight;
        }
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const;
    };

    struct FormatKey {
        std::string family;
        float size = 0.f;
        FontWeight weight = FontWeight::Regular;
        bool italic = false;
        TextAlign align = TextAlign::Left;
        TextVAlign valign = TextVAlign::Middle;
        bool wrap = false;
        float letterSpacing = 0.f;

        bool operator==(const FormatKey& o) const {
            return family == o.family && size == o.size && weight == o.weight &&
                   italic == o.italic && align == o.align && valign == o.valign &&
                   wrap == o.wrap && letterSpacing == o.letterSpacing;
        }
    };

    struct FormatKeyHash {
        size_t operator()(const FormatKey& key) const;
    };

    static constexpr size_t kMaxLayouts = 512;

    ComPtr<IDWriteFactory5> factory_;
    ComPtr<IDWriteFontSetBuilder1> setBuilder_;
    ComPtr<IDWriteFontCollection1> collection_;
    std::vector<std::string> bundledFamilies_;

    std::unordered_map<FormatKey, ComPtr<IDWriteTextFormat>, FormatKeyHash> formats_;

    std::unordered_map<LayoutKey, std::pair<ComPtr<IDWriteTextLayout>,
                                            std::list<LayoutKey>::iterator>, LayoutKeyHash> layouts_;
    std::list<LayoutKey> layoutOrder_;
};

}
