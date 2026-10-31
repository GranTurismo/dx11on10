// Drawing the window with Direct2D and DirectWrite (works on Windows 7 and later) and
// telling the window code which control is under the mouse.
#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <map>

#include "app.h"

namespace app {

template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Com& operator=(const Com& o) {
        if (o.p_) o.p_->AddRef();
        Reset();
        p_ = o.p_;
        return *this;
    }
    ~Com() { Reset(); }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** Put() {
        Reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }
    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

struct AppState {
    std::vector<Game> games;
    GpuInfo gpu;
    bool gpuKnown = false;
    bool scanning = false;
    int selected = -1;  // index into games
    bool showAll = false;
    std::wstring toast;
    Level toastLevel = Level::Success;
    DWORD toastAt = 0;
};

// The games the list shows, in order.
std::vector<int> VisibleGames(const AppState& s);

// What a click lands on: kind in the top bits, an index (game, option) in the low ones.
enum Hit : uint32_t {
    HitNone,
    HitRow,
    HitRowSwitch,
    HitFilter,
    HitAdd,
    HitRefresh,
    HitAbout,
    HitSwitch,
    HitPlay,
    HitUpdate,
    HitFolder,
    HitLog,
    HitChangeExe,
    HitRemove,
    HitFake,
    HitLevel,
    HitWarp,
    HitShowAll,
    HitListBar,
    HitDetailBar,
};
constexpr uint32_t MakeHit(Hit kind, uint32_t index = 0) { return (uint32_t(kind) << 20) | index; }
constexpr Hit KindOf(uint32_t id) { return Hit(id >> 20); }
constexpr uint32_t IndexOf(uint32_t id) { return id & 0xFFFFF; }

class View {
public:
    bool Init();
    void Render(ID2D1RenderTarget* target, const AppState& state, float width, float height, float dpi);
    void DiscardDeviceResources();  // the render target was lost

    uint32_t HitTest(float x, float y) const;
    void Wheel(float x, float y, float delta);
    void BeginDrag(uint32_t bar, float y);
    void Drag(float y);
    void EndDrag() { dragging_ = HitNone; }
    bool Dragging() const { return dragging_ != HitNone; }
    void ScrollToRow(int position);  // keep the n-th visible row in view
    void ResetDetailScroll() { detail_.offset = 0; }
    bool Animating() const { return animating_; }
    void SetDark(bool dark);
    bool Dark() const { return dark_; }

    struct HitRect {
        D2D1_RECT_F rect;
        uint32_t id;
    };
    const std::vector<HitRect>& Hits() const { return hits_; }

    uint32_t hover = 0, pressed = 0;
    ID2D1Factory* Factory() const { return factory_.get(); }
    IWICImagingFactory* Wic() const { return wic_.get(); }

private:
    struct Theme {
        D2D1_COLOR_F bg, surface, surface2, hover, border, text, dim, faint, accent, accentHover, success, warning,
            danger, info, switchOff, selected;
    };
    struct Scroll {
        float offset = 0, content = 0, top = 0, view = 0;
        D2D1_RECT_F bar{};
        void Clamp();
    };
    struct Image {
        Com<ID2D1Bitmap> bitmap;
        float width = 0, height = 0;  // in DIPs
    };

    // drawing helpers
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0, float width = 1);
    Com<IDWriteTextLayout> Layout(const std::wstring& text, IDWriteTextFormat* format, float width, bool wrap,
                                  float height = 1000);
    float TextHeight(const std::wstring& text, IDWriteTextFormat* format, float width);
    float TextWidth(const std::wstring& text, IDWriteTextFormat* format);
    float Text(const std::wstring& text, IDWriteTextFormat* format, float x, float y, float width, D2D1_COLOR_F c,
               bool wrap = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void Region(const D2D1_RECT_F& r, uint32_t id);
    bool Hovered(uint32_t id) const { return hover == id; }

    enum class Glyph { Plus, Refresh, Help, Play, Folder, Document, File, Shield, Warning, Info, Check, Danger, Chip,
                       Close };
    void Icon(Glyph g, float cx, float cy, float size, D2D1_COLOR_F c);
    float Chip(const std::wstring& text, float x, float y, D2D1_COLOR_F c, bool measureOnly = false);
    void Switch(uint32_t id, const std::wstring& key, float x, float y, float w, float h, bool on, bool enabled);
    void Button(uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, int style, Glyph glyph = Glyph::Close,
                bool hasGlyph = false);
    float ButtonWidth(const std::wstring& label, bool hasGlyph);
    void IconButton(uint32_t id, const D2D1_RECT_F& r, Glyph glyph);
    void Segmented(uint32_t base, const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int selected);
    float Banner(const Notice& n, float x, float y, float width, bool draw = true);
    void Spinner(float cx, float cy, float radius, D2D1_COLOR_F c);
    void Cover(const Image& img, const D2D1_RECT_F& r, float radius);
    void GameTile(const Game& g, const D2D1_RECT_F& r, float radius, bool large);
    void Logo(const D2D1_RECT_F& r);
    void ScrollBar(Scroll& s, float x, uint32_t id);

    Image* LoadArt(const std::wstring& path, float maxWidth);
    Image* LoadExeIcon(const std::wstring& exe);
    Image* LoadLogo();
    Image ToImage(IWICBitmapSource* source, float maxWidth);

    // parts of the window
    void Header(const AppState& s, float width);
    void GpuCard(const AppState& s, const D2D1_RECT_F& r);
    void List(const AppState& s, const D2D1_RECT_F& r);
    void Row(const AppState& s, int index, const D2D1_RECT_F& r);
    void Details(const AppState& s, const D2D1_RECT_F& r);
    void Toast(const AppState& s, float width, float height);

    Com<ID2D1Factory> factory_;
    Com<IDWriteFactory> dwrite_;
    Com<IWICImagingFactory> wic_;
    Com<ID2D1StrokeStyle> round_;
    Com<IDWriteTextFormat> title_, h1_, h2_, body_, strong_, small_, smallStrong_, chip_, section_, glyph_;
    std::map<IDWriteTextFormat*, Com<IDWriteInlineObject>> ellipsis_;

    ID2D1RenderTarget* rt_ = nullptr;
    Com<ID2D1SolidColorBrush> brush_;
    std::map<std::wstring, Image> images_;
    float dpi_ = 96;

    Theme t_{};
    bool dark_ = false;
    std::vector<HitRect> hits_;
    std::vector<D2D1_RECT_F> clips_;
    Scroll list_, detail_;
    Hit dragging_ = HitNone;
    float dragStart_ = 0, dragOffset_ = 0;
    std::map<std::wstring, float> anim_;
    bool animating_ = false;
    DWORD lastFrame_ = 0;
    float dt_ = 0;
    float rowHeight_ = 76;
    float listRight_ = 0;  // where the list pane ends (mouse wheel target)
    std::wstring selectedId_;
};

}  // namespace app
