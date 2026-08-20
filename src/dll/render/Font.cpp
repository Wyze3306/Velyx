#include "Font.hpp"

#include <algorithm>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Font";

const char* kFallbackFamilies[] = {"Velyx Sans", "Segoe UI Variable Text", "Segoe UI", "Arial"};

DWRITE_FONT_WEIGHT toDWrite(FontWeight weight) {
    return static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(weight));
}

DWRITE_TEXT_ALIGNMENT toDWrite(TextAlign align) {
    switch (align) {
        case TextAlign::Center: return DWRITE_TEXT_ALIGNMENT_CENTER;
        case TextAlign::Right:  return DWRITE_TEXT_ALIGNMENT_TRAILING;
        case TextAlign::Left:   break;
    }
    return DWRITE_TEXT_ALIGNMENT_LEADING;
}

DWRITE_PARAGRAPH_ALIGNMENT toDWrite(TextVAlign align) {
    switch (align) {
        case TextVAlign::Middle: return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        case TextVAlign::Bottom: return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        case TextVAlign::Top:    break;
    }
    return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
}

template <typename T>
void hashCombine(size_t& seed, const T& value) {
    seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

}

size_t FontManager::FormatKeyHash::operator()(const FormatKey& key) const {
    size_t seed = 0;
    hashCombine(seed, key.family);
    hashCombine(seed, key.size);
    hashCombine(seed, static_cast<int>(key.weight));
    hashCombine(seed, key.italic);
    hashCombine(seed, static_cast<int>(key.align));
    hashCombine(seed, static_cast<int>(key.valign));
    hashCombine(seed, key.wrap);
    hashCombine(seed, key.letterSpacing);
    return seed;
}

size_t FontManager::LayoutKeyHash::operator()(const LayoutKey& key) const {
    size_t seed = 0;
    hashCombine(seed, key.text);
    hashCombine(seed, key.maxWidth);
    hashCombine(seed, key.maxHeight);
    seed ^= FormatKeyHash{}(FormatKey{key.spec.family, key.spec.size, key.spec.weight,
                                      key.spec.italic, key.spec.align, key.spec.valign,
                                      key.spec.wrap, key.spec.letterSpacing});
    return seed;
}

FontManager& FontManager::get() {
    static FontManager instance;
    return instance;
}

bool FontManager::initialize(IDWriteFactory5* factory) {
    if (!factory) return false;

    factory_ = ComPtr<IDWriteFactory5>(factory);
    formats_.clear();
    layouts_.clear();
    layoutOrder_.clear();

    if (FAILED(factory_->CreateFontSetBuilder(setBuilder_.put()))) {
        Log::warn(kLog, "bundled fonts unavailable");
        setBuilder_.reset();
    }

    return true;
}

void FontManager::shutdown() {
    layouts_.clear();
    layoutOrder_.clear();
    formats_.clear();
    collection_.reset();
    setBuilder_.reset();
    factory_.reset();
    bundledFamilies_.clear();
}

void FontManager::invalidate() {
    layouts_.clear();
    layoutOrder_.clear();
}

bool FontManager::loadFile(const std::filesystem::path& file) {
    if (!factory_ || !setBuilder_) return false;

    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(factory_->CreateFontFileReference(file.wstring().c_str(), nullptr,
                                                 fontFile.put()))) {
        Log::warn(kLog, "could not open font {}", file.filename().string());
        return false;
    }

    if (FAILED(setBuilder_->AddFontFile(fontFile.get()))) {
        Log::warn(kLog, "{} is not a usable font file", file.filename().string());
        return false;
    }

    return true;
}

int FontManager::loadDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return 0;

    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        const auto extension = strings::toLower(entry.path().extension().string());
        if (extension != ".ttf" && extension != ".otf" && extension != ".ttc") continue;

        if (loadFile(entry.path())) ++loaded;
    }

    if (loaded > 0) {
        rebuildCollection();
        Log::info(kLog, "loaded {} bundled font file(s) from {}", loaded,
                  directory.filename().string());
    }

    return loaded;
}

void FontManager::rebuildCollection() {
    if (!factory_ || !setBuilder_) return;

    ComPtr<IDWriteFontSet> fontSet;
    if (FAILED(setBuilder_->CreateFontSet(fontSet.put()))) return;

    collection_.reset();
    if (FAILED(factory_->CreateFontCollectionFromFontSet(fontSet.get(), collection_.put()))) {
        Log::warn(kLog, "could not create the font collection");
        return;
    }

    bundledFamilies_.clear();
    const UINT32 count = collection_->GetFontFamilyCount();
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IDWriteFontFamily1> family;
        if (FAILED(collection_->GetFontFamily(i, family.put()))) continue;

        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(family->GetFamilyNames(names.put()))) continue;

        UINT32 length = 0;
        if (FAILED(names->GetStringLength(0, &length))) continue;

        std::wstring name(length + 1, L'\0');
        if (FAILED(names->GetString(0, name.data(), length + 1))) continue;
        name.resize(length);

        bundledFamilies_.push_back(strings::toUtf8(name));
    }

    formats_.clear();
    invalidate();
}

std::vector<std::string> FontManager::availableFamilies() const {
    std::vector<std::string> families = bundledFamilies_;

    if (factory_) {

        auto* base = static_cast<IDWriteFactory*>(factory_.get());

        ComPtr<IDWriteFontCollection> system;
        if (SUCCEEDED(base->GetSystemFontCollection(system.put(), FALSE))) {
            const UINT32 count = system->GetFontFamilyCount();
            for (UINT32 i = 0; i < count; ++i) {
                ComPtr<IDWriteFontFamily> family;
                if (FAILED(system->GetFontFamily(i, family.put()))) continue;

                ComPtr<IDWriteLocalizedStrings> names;
                if (FAILED(family->GetFamilyNames(names.put()))) continue;

                UINT32 length = 0;
                if (FAILED(names->GetStringLength(0, &length))) continue;

                std::wstring name(length + 1, L'\0');
                if (FAILED(names->GetString(0, name.data(), length + 1))) continue;
                name.resize(length);

                families.push_back(strings::toUtf8(name));
            }
        }
    }

    std::ranges::sort(families);
    families.erase(std::ranges::unique(families).begin(), families.end());
    return families;
}

IDWriteTextFormat* FontManager::format(const FontSpec& spec) {
    if (!factory_) return nullptr;

    const FormatKey key{spec.family, spec.size, spec.weight, spec.italic,
                        spec.align,  spec.valign, spec.wrap, spec.letterSpacing};

    if (const auto it = formats_.find(key); it != formats_.end()) return it->second.get();

    ComPtr<IDWriteTextFormat> format;
    const std::wstring family = strings::toUtf16(spec.family);

    HRESULT hr = factory_->CreateTextFormat(
        family.c_str(), collection_.get(), toDWrite(spec.weight),
        spec.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, spec.size, L"", format.put());

    if (FAILED(hr)) {
        for (const char* fallback : kFallbackFamilies) {
            const std::wstring wide = strings::toUtf16(fallback);
            hr = factory_->CreateTextFormat(wide.c_str(), nullptr, toDWrite(spec.weight),
                                            spec.italic ? DWRITE_FONT_STYLE_ITALIC
                                                        : DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, spec.size, L"",
                                            format.put());
            if (SUCCEEDED(hr)) break;
        }
    }

    if (FAILED(hr) || !format) {
        Log::error(kLog, "no usable font for family '{}'", spec.family);
        return nullptr;
    }

    format->SetTextAlignment(toDWrite(spec.align));
    format->SetParagraphAlignment(toDWrite(spec.valign));
    format->SetWordWrapping(spec.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    if (!spec.wrap) {
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(factory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.get());
        }
    }

    IDWriteTextFormat* raw = format.get();
    formats_.emplace(key, std::move(format));
    return raw;
}

IDWriteTextLayout* FontManager::layout(std::string_view text, const FontSpec& spec, Vec2 maxSize) {
    if (!factory_) return nullptr;

    LayoutKey key{std::string(text), spec, maxSize.x, maxSize.y};

    if (const auto it = layouts_.find(key); it != layouts_.end()) {
        layoutOrder_.splice(layoutOrder_.begin(), layoutOrder_, it->second.second);
        return it->second.first.get();
    }

    IDWriteTextFormat* textFormat = format(spec);
    if (!textFormat) return nullptr;

    const std::wstring wide = strings::toUtf16(text);

    ComPtr<IDWriteTextLayout> created;
    if (FAILED(factory_->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                          textFormat, maxSize.x, maxSize.y, created.put()))) {
        return nullptr;
    }

    if (spec.letterSpacing != 0.f) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(created->QueryInterface(__uuidof(IDWriteTextLayout1),
                                              reinterpret_cast<void**>(layout1.put())))) {
            const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(wide.size())};
            layout1->SetCharacterSpacing(spec.letterSpacing, spec.letterSpacing, 0.f, range);
        }
    }

    while (layoutOrder_.size() >= kMaxLayouts) {
        layouts_.erase(layoutOrder_.back());
        layoutOrder_.pop_back();
    }

    layoutOrder_.push_front(key);
    IDWriteTextLayout* raw = created.get();
    layouts_.emplace(std::move(key), std::pair{std::move(created), layoutOrder_.begin()});
    return raw;
}

Vec2 FontManager::measure(std::string_view text, const FontSpec& spec, float maxWidth) {
    IDWriteTextLayout* textLayout = layout(text, spec, {maxWidth, 4096.f});
    if (!textLayout) return {};

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(textLayout->GetMetrics(&metrics))) return {};

    return {metrics.widthIncludingTrailingWhitespace, metrics.height};
}

}
