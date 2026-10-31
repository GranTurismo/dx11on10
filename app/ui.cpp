// The window's contents. Everything is laid out and drawn in one pass per frame; the pass
// also records where each control is, which is what mouse clicks are checked against.
#include "ui.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>

#include "resource.h"

namespace app {

namespace {

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1) {
    return {float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f, float(rgb & 0xFF) / 255.f, a};
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_RECT_F Rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

D2D1_COLOR_F LevelColor(Level l, D2D1_COLOR_F info, D2D1_COLOR_F success, D2D1_COLOR_F warning, D2D1_COLOR_F danger) {
    switch (l) {
        case Level::Success: return success;
        case Level::Warning: return warning;
        case Level::Danger: return danger;
        default: return info;
    }
}

std::wstring Thousands(unsigned n) {
    std::wstring s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), L",");
    return s;
}

std::wstring Ago(const FILETIME& t) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    auto value = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    int64_t seconds = (int64_t(value(now)) - int64_t(value(t))) / 10000000;
    if (seconds < 90) return L"just now";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" minutes ago";
    if (seconds < 7200) return L"an hour ago";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" hours ago";
    if (seconds < 172800) return L"yesterday";
    return std::to_wstring(seconds / 86400) + L" days ago";
}

std::wstring LevelText(const std::string& level) {
    if (level == "10_0") return L"DirectX 10";
    if (level == "10_1") return L"DirectX 10.1";
    if (level == "11_0") return L"DirectX 11";
    if (level == "11_1") return L"DirectX 11.1";
    return Widen(level);
}

}  // namespace

std::vector<int> VisibleGames(const AppState& s) {
    std::vector<int> out;
    for (int i = 0; i < int(s.games.size()); i++) {
        const Game& g = s.games[i];
        if (s.showAll || (g.apis & ApiD3D11) || g.install != Install::Off || g.store == Store::Custom ||
            (g.machine == 0 && g.apis == 0))
            out.push_back(i);
    }
    return out;
}

// ---- setup ----

bool View::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                 reinterpret_cast<void**>(factory_.Put()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.Put()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     reinterpret_cast<void**>(wic_.Put()));
    D2D1_STROKE_STYLE_PROPERTIES stroke = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                           D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
    factory_->CreateStrokeStyle(&stroke, nullptr, 0, round_.Put());

    // Windows 11's Segoe UI Variable if it's there, Segoe UI otherwise (Windows 7 to 10).
    std::wstring text = L"Segoe UI", display = L"Segoe UI";
    Com<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(fonts.Put(), FALSE))) {
        UINT32 index;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists) {
            text = L"Segoe UI Variable Text";
            display = L"Segoe UI Variable Display";
        }
    }
    auto make = [&](Com<IDWriteTextFormat>& f, const std::wstring& family, DWRITE_FONT_WEIGHT weight, float size) {
        dwrite_->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  size, L"en-us", f.Put());
        Com<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(f.get(), sign.Put());
        ellipsis_[f.get()] = sign;
    };
    make(title_, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 21);
    make(h1_, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22);
    make(h2_, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.5f);
    make(body_, text, DWRITE_FONT_WEIGHT_NORMAL, 13.5f);
    make(strong_, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f);
    make(small_, text, DWRITE_FONT_WEIGHT_NORMAL, 12.5f);
    make(smallStrong_, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f);
    make(chip_, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.5f);
    make(section_, text, DWRITE_FONT_WEIGHT_BOLD, 11);
    make(glyph_, text, DWRITE_FONT_WEIGHT_BOLD, 11);
    if (!title_ || !body_) return false;
    SetDark(false);
    return true;
}

void View::SetDark(bool dark) {
    dark_ = dark;
    if (dark)
        t_ = {Rgb(0x111317), Rgb(0x191C22), Rgb(0x21252D), Rgb(0x262B34), Rgb(0x2B303A), Rgb(0xECEEF3),
              Rgb(0x9CA3B2), Rgb(0x6C7485), Rgb(0x5B8CFF), Rgb(0x7AA2FF), Rgb(0x3CCB8A), Rgb(0xF2B544),
              Rgb(0xF2626A), Rgb(0x6AA8FF), Rgb(0x3A404C), Rgb(0x5B8CFF, 0.16f)};
    else
        t_ = {Rgb(0xF2F3F6), Rgb(0xFFFFFF), Rgb(0xF5F6F8), Rgb(0xEEF0F4), Rgb(0xE3E6EC), Rgb(0x161A22),
              Rgb(0x5D6576), Rgb(0x8A92A3), Rgb(0x2F6BFF), Rgb(0x1F58E6), Rgb(0x15985F), Rgb(0xB7741A),
              Rgb(0xD9363E), Rgb(0x2F6BFF), Rgb(0xC5CAD4), Rgb(0x2F6BFF, 0.09f)};
}

void View::DiscardDeviceResources() {
    brush_.Reset();
    images_.clear();
    rt_ = nullptr;
}

// ---- primitives ----

void View::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    brush_->SetColor(&c);
    if (radius > 0) {
        D2D1_ROUNDED_RECT rr = {r, radius, radius};
        rt_->FillRoundedRectangle(&rr, brush_.get());
    } else {
        rt_->FillRectangle(&r, brush_.get());
    }
}

void View::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width) {
    brush_->SetColor(&c);
    D2D1_RECT_F in = {r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2};
    if (radius > 0) {
        D2D1_ROUNDED_RECT rr = {in, radius, radius};
        rt_->DrawRoundedRectangle(&rr, brush_.get(), width, nullptr);
    } else {
        rt_->DrawRectangle(&in, brush_.get(), width, nullptr);
    }
}

Com<IDWriteTextLayout> View::Layout(const std::wstring& text, IDWriteTextFormat* format, float width, bool wrap,
                                    float height) {
    Com<IDWriteTextLayout> layout;
    dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), format, std::max(width, 1.f), height, layout.Put());
    if (!layout) return layout;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trim, ellipsis_[format].get());
    }
    return layout;
}

float View::TextHeight(const std::wstring& text, IDWriteTextFormat* format, float width) {
    Com<IDWriteTextLayout> l = Layout(text, format, width, true);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.height;
}

float View::TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
    Com<IDWriteTextLayout> l = Layout(text, format, 10000, false);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float View::Text(const std::wstring& text, IDWriteTextFormat* format, float x, float y, float width, D2D1_COLOR_F c,
                 bool wrap, DWRITE_TEXT_ALIGNMENT align) {
    Com<IDWriteTextLayout> l = Layout(text, format, width, wrap);
    if (!l) return 0;
    l->SetTextAlignment(align);
    brush_->SetColor(&c);
    rt_->DrawTextLayout({x, y}, l.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
}

void View::Region(const D2D1_RECT_F& r, uint32_t id) {
    D2D1_RECT_F clipped = clips_.empty() ? r : Intersect(r, clips_.back());
    if (clipped.right > clipped.left && clipped.bottom > clipped.top) hits_.push_back({clipped, id});
}

uint32_t View::HitTest(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (Inside(it->rect, x, y)) return it->id;
    return 0;
}

// Small line icons, drawn in a size x size box centred on (cx, cy).
void View::Icon(Glyph g, float cx, float cy, float size, D2D1_COLOR_F c) {
    brush_->SetColor(&c);
    float s = size / 16.f, w = 1.6f * std::max(1.f, s * 0.9f);
    auto P = [&](float x, float y) { return D2D1_POINT_2F{cx + (x - 8) * s, cy + (y - 8) * s}; };
    auto line = [&](float x1, float y1, float x2, float y2) {
        rt_->DrawLine(P(x1, y1), P(x2, y2), brush_.get(), w, round_.get());
    };
    auto path = [&](std::initializer_list<D2D1_POINT_2F> points, bool closed, bool fill) {
        Com<ID2D1PathGeometry> geo;
        factory_->CreatePathGeometry(geo.Put());
        Com<ID2D1GeometrySink> sink;
        geo->Open(sink.Put());
        auto it = points.begin();
        sink->BeginFigure(*it, fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != points.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        if (fill) rt_->FillGeometry(geo.get(), brush_.get(), nullptr);
        else rt_->DrawGeometry(geo.get(), brush_.get(), w, round_.get());
    };
    auto letter = [&](const wchar_t* text, float dy = 0) {
        Com<IDWriteTextLayout> l = Layout(text, glyph_.get(), size, false);
        l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        l->SetFontSize(size * 0.62f, {0, 1});
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        rt_->DrawTextLayout({cx - size / 2, cy - m.height / 2 + dy * s}, l.get(), brush_.get(),
                            D2D1_DRAW_TEXT_OPTIONS_NONE);
    };
    D2D1_ELLIPSE circle = {{cx, cy}, 6.6f * s, 6.6f * s};
    switch (g) {
        case Glyph::Plus:
            line(8, 3, 8, 13);
            line(3, 8, 13, 8);
            break;
        case Glyph::Close:
            line(4, 4, 12, 12);
            line(12, 4, 4, 12);
            break;
        case Glyph::Refresh: {
            Com<ID2D1PathGeometry> geo;
            factory_->CreatePathGeometry(geo.Put());
            Com<ID2D1GeometrySink> sink;
            geo->Open(sink.Put());
            sink->BeginFigure(P(13, 8), D2D1_FIGURE_BEGIN_HOLLOW);
            D2D1_ARC_SEGMENT arc = {P(9.2f, 3.1f), {5 * s, 5 * s}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE};
            sink->AddArc(&arc);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt_->DrawGeometry(geo.get(), brush_.get(), w, round_.get());
            path({P(9.6f, 1), P(9.4f, 3.3f), P(11.8f, 4.3f)}, false, false);
            break;
        }
        case Glyph::Help:
            rt_->DrawEllipse(&circle, brush_.get(), w, nullptr);
            letter(L"?", 0.2f);
            break;
        case Glyph::Play: path({P(5, 3), P(13, 8), P(5, 13)}, true, true); break;
        case Glyph::Folder:
            path({P(2, 4.5f), P(6.5f, 4.5f), P(8, 6), P(14, 6), P(14, 12.5f), P(2, 12.5f)}, true, false);
            break;
        case Glyph::Document:
            path({P(4, 2), P(10, 2), P(13, 5), P(13, 14), P(4, 14)}, true, false);
            line(6.5f, 8, 10.5f, 8);
            line(6.5f, 11, 10.5f, 11);
            break;
        case Glyph::File:
            path({P(3, 3), P(13, 3), P(13, 13), P(3, 13)}, true, false);
            line(3, 6, 13, 6);
            break;
        case Glyph::Shield:
            path({P(8, 1.5f), P(13.5f, 3.5f), P(13, 9), P(8, 14.5f), P(3, 9), P(2.5f, 3.5f)}, true, false);
            break;
        case Glyph::Warning:
            path({P(8, 1.8f), P(14.6f, 13.6f), P(1.4f, 13.6f)}, true, true);
            {
                D2D1_COLOR_F white = dark_ ? Rgb(0x16181d) : Rgb(0xFFFFFF);
                brush_->SetColor(&white);
                line(8, 6.2f, 8, 9.6f);
                D2D1_ELLIPSE dot = {P(8, 11.9f), 0.95f * s, 0.95f * s};
                rt_->FillEllipse(&dot, brush_.get());
            }
            break;
        case Glyph::Info:
        case Glyph::Danger:
        case Glyph::Check: {
            rt_->FillEllipse(&circle, brush_.get());
            D2D1_COLOR_F inner = dark_ ? Rgb(0x16181d) : Rgb(0xFFFFFF);
            brush_->SetColor(&inner);
            if (g == Glyph::Check) {
                path({P(5.2f, 8.2f), P(7.2f, 10.2f), P(10.9f, 6.2f)}, false, false);
            } else if (g == Glyph::Info) {
                line(8, 7.4f, 8, 11);
                D2D1_ELLIPSE dot = {P(8, 5), 0.95f * s, 0.95f * s};
                rt_->FillEllipse(&dot, brush_.get());
            } else {
                line(8, 4.6f, 8, 8.6f);
                D2D1_ELLIPSE dot = {P(8, 11), 0.95f * s, 0.95f * s};
                rt_->FillEllipse(&dot, brush_.get());
            }
            break;
        }
        case Glyph::Chip: {
            D2D1_ROUNDED_RECT body = {{P(3, 3).x, P(3, 3).y, P(13, 13).x, P(13, 13).y}, 1.5f * s, 1.5f * s};
            rt_->DrawRoundedRectangle(&body, brush_.get(), w, nullptr);
            D2D1_RECT_F core = {P(6, 6).x, P(6, 6).y, P(10, 10).x, P(10, 10).y};
            rt_->FillRectangle(&core, brush_.get());
            for (float p : {5.5f, 8.f, 10.5f}) {
                line(p, 0.8f, p, 2.2f);
                line(p, 13.8f, p, 15.2f);
                line(0.8f, p, 2.2f, p);
                line(13.8f, p, 15.2f, p);
            }
            break;
        }
    }
}

float View::Chip(const std::wstring& text, float x, float y, D2D1_COLOR_F c, bool measureOnly) {
    float w = TextWidth(text, chip_.get()) + 18;
    if (measureOnly) return w;
    Fill(Rect(x, y, w, 22), Alpha(c, dark_ ? 0.18f : 0.12f), 11);
    Text(text, chip_.get(), x + 9, y + 3.5f, w, c);
    return w;
}

void View::Switch(uint32_t id, const std::wstring& key, float x, float y, float w, float h, bool on, bool enabled) {
    float target = on ? 1.f : 0.f;
    auto it = anim_.find(key);
    if (it == anim_.end()) it = anim_.emplace(key, target).first;
    float& v = it->second;
    if (std::fabs(v - target) > 0.002f) {
        v += (target - v) * std::min(1.f, dt_ * 16.f);
        animating_ = true;
    } else {
        v = target;
    }
    D2D1_COLOR_F off = t_.switchOff, onc = Hovered(id) ? t_.accentHover : t_.accent;
    D2D1_COLOR_F track = {off.r + (onc.r - off.r) * v, off.g + (onc.g - off.g) * v, off.b + (onc.b - off.b) * v, 1};
    if (!enabled) track = Alpha(track, 0.45f);
    Fill(Rect(x, y, w, h), track, h / 2);
    float r = h / 2 - 3;
    float cx = x + h / 2 + (w - h) * v;
    D2D1_ELLIPSE knob = {{cx, y + h / 2}, r, r};
    D2D1_COLOR_F shadow = Rgb(0x000000, 0.18f), white = Rgb(0xFFFFFF, enabled ? 1 : 0.8f);
    brush_->SetColor(&shadow);
    D2D1_ELLIPSE shade = {{cx, y + h / 2 + 0.8f}, r + 0.4f, r + 0.4f};
    rt_->FillEllipse(&shade, brush_.get());
    brush_->SetColor(&white);
    rt_->FillEllipse(&knob, brush_.get());
    if (enabled) Region(Rect(x - 4, y - 4, w + 8, h + 8), id);
}

float View::ButtonWidth(const std::wstring& label, bool hasGlyph) {
    return TextWidth(label, strong_.get()) + 30 + (hasGlyph ? 22 : 0);
}

// style: 0 primary, 1 secondary, 2 quiet danger
void View::Button(uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, int style, Glyph glyph,
                  bool hasGlyph) {
    bool hot = Hovered(id), down = hot && pressed == id;
    D2D1_COLOR_F fg;
    if (style == 0) {
        Fill(r, down ? Alpha(t_.accentHover, 0.85f) : hot ? t_.accentHover : t_.accent, 8);
        fg = Rgb(0xFFFFFF);
    } else {
        Fill(r, hot ? t_.hover : t_.surface2, 8);
        Stroke(r, t_.border, 8);
        fg = style == 2 ? t_.danger : t_.text;
    }
    float tw = TextWidth(label, strong_.get());
    float total = tw + (hasGlyph ? 22 : 0);
    float x = (r.left + r.right) / 2 - total / 2;
    float h = TextHeight(label, strong_.get(), 1000);
    if (hasGlyph) {
        Icon(glyph, x + 7, (r.top + r.bottom) / 2, 14, fg);
        x += 22;
    }
    Text(label, strong_.get(), x, (r.top + r.bottom) / 2 - h / 2, tw + 2, fg);
    Region(r, id);
}

void View::IconButton(uint32_t id, const D2D1_RECT_F& r, Glyph glyph) {
    if (Hovered(id)) Fill(r, t_.hover, 8);
    Icon(glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 17, t_.dim);
    Region(r, id);
}

void View::Segmented(uint32_t base, const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int selected) {
    Fill(r, t_.surface2, 8);
    Stroke(r, t_.border, 8);
    float w = (r.right - r.left - 4) / float(options.size());
    for (size_t i = 0; i < options.size(); i++) {
        D2D1_RECT_F seg = {r.left + 2 + w * float(i), r.top + 2, r.left + 2 + w * float(i + 1), r.bottom - 2};
        uint32_t id = MakeHit(Hit(base), uint32_t(i));
        if (int(i) == selected) {
            Fill(seg, dark_ ? Rgb(0x343A46) : Rgb(0xFFFFFF), 6);
            if (!dark_) Stroke(seg, t_.border, 6);
        } else if (Hovered(id)) {
            Fill(seg, t_.hover, 6);
        }
        float h = TextHeight(options[i], smallStrong_.get(), 1000);
        Text(options[i], int(i) == selected ? smallStrong_.get() : small_.get(), seg.left, (seg.top + seg.bottom) / 2 - h / 2,
             seg.right - seg.left, int(i) == selected ? t_.text : t_.dim, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (int(i) != selected) Region(seg, id);
    }
}

float View::Banner(const Notice& n, float x, float y, float width, bool draw) {
    D2D1_COLOR_F c = LevelColor(n.level, t_.info, t_.success, t_.warning, t_.danger);
    float textX = x + 40, textW = width - 52;
    float th = TextHeight(n.title, strong_.get(), textW);
    float bh = TextHeight(n.body, small_.get(), textW);
    float h = 14 + th + 3 + bh + 14;
    if (!draw) return h;
    Fill(Rect(x, y, width, h), Alpha(c, dark_ ? 0.11f : 0.075f), 10);
    Stroke(Rect(x, y, width, h), Alpha(c, dark_ ? 0.30f : 0.26f), 10);
    Glyph g = n.level == Level::Danger    ? Glyph::Danger
              : n.level == Level::Warning ? Glyph::Warning
              : n.level == Level::Success ? Glyph::Check
                                          : Glyph::Info;
    Icon(g, x + 21, y + 14 + th / 2, 18, c);
    Text(n.title, strong_.get(), textX, y + 14, textW, t_.text, true);
    Text(n.body, small_.get(), textX, y + 14 + th + 3, textW, t_.dim, true);
    return h;
}

void View::Spinner(float cx, float cy, float radius, D2D1_COLOR_F c) {
    float angle = float(GetTickCount() % 1000) / 1000.f * 6.2831853f;
    Com<ID2D1PathGeometry> geo;
    factory_->CreatePathGeometry(geo.Put());
    Com<ID2D1GeometrySink> sink;
    geo->Open(sink.Put());
    D2D1_POINT_2F start = {cx + radius * std::cos(angle), cy + radius * std::sin(angle)};
    D2D1_POINT_2F end = {cx + radius * std::cos(angle + 4.2f), cy + radius * std::sin(angle + 4.2f)};
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT arc = {end, {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE};
    sink->AddArc(&arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    brush_->SetColor(&c);
    rt_->DrawGeometry(geo.get(), brush_.get(), 2.2f, round_.get());
    animating_ = true;
}

// ---- images ----

View::Image View::ToImage(IWICBitmapSource* source, float maxWidth) {
    Image img;
    if (!source || !rt_) return img;
    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    if (!w || !h) return img;
    // Shrinking here (Fant filter) looks much better than letting Direct2D skip pixels.
    Com<IWICBitmapScaler> scaler;
    UINT maxPx = UINT(std::ceil(maxWidth * dpi_ / 96.f));
    if (maxWidth > 0 && w > maxPx) {
        UINT sh = std::max(1u, UINT(double(h) * maxPx / w));
        if (SUCCEEDED(wic_->CreateBitmapScaler(scaler.Put())) &&
            SUCCEEDED(scaler->Initialize(source, maxPx, sh, WICBitmapInterpolationModeFant))) {
            w = maxPx, h = sh;
            source = scaler.get();
        }
    }
    Com<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return img;
    D2D1_BITMAP_PROPERTIES props = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, dpi_, dpi_};
    if (FAILED(rt_->CreateBitmapFromWicBitmap(converter.get(), &props, img.bitmap.Put()))) return img;
    img.width = float(w) * 96.f / dpi_;
    img.height = float(h) * 96.f / dpi_;
    return img;
}

View::Image* View::LoadArt(const std::wstring& path, float maxWidth) {
    std::wstring key = L"art:" + std::to_wstring(int(maxWidth)) + L":" + path;
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.bitmap ? &it->second : nullptr;
    Image& img = images_[key];
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    if (wic_ && SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, decoder.Put())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.Put())))
        img = ToImage(frame.get(), maxWidth);
    return img.bitmap ? &img : nullptr;
}

View::Image* View::LoadExeIcon(const std::wstring& exe) {
    std::wstring key = L"icon:" + exe;
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.bitmap ? &it->second : nullptr;
    Image& img = images_[key];
    HICON icon = nullptr;
    if (wic_ && !exe.empty() && SUCCEEDED(SHDefExtractIconW(exe.c_str(), 0, 0, &icon, nullptr, 64)) && icon) {
        Com<IWICBitmap> bitmap;
        if (SUCCEEDED(wic_->CreateBitmapFromHICON(icon, bitmap.Put()))) img = ToImage(bitmap.get(), 0);
        DestroyIcon(icon);
    }
    return img.bitmap ? &img : nullptr;
}

View::Image* View::LoadLogo() {
    auto it = images_.find(L"logo");
    if (it != images_.end()) return it->second.bitmap ? &it->second : nullptr;
    Image& img = images_[L"logo"];
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), MAKEINTRESOURCEW(10));
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    if (res && wic_ && SUCCEEDED(wic_->CreateStream(stream.Put())) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(LoadResource(nullptr, res))),
                                               SizeofResource(nullptr, res))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.Put())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.Put())))
        img = ToImage(frame.get(), 48);
    return img.bitmap ? &img : nullptr;
}

// Fills r with the image, cropped to keep its proportions.
void View::Cover(const Image& img, const D2D1_RECT_F& r, float radius) {
    float rw = r.right - r.left, rh = r.bottom - r.top;
    float scale = std::max(rw / img.width, rh / img.height);
    D2D1_BITMAP_BRUSH_PROPERTIES bp = {D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR};
    Com<ID2D1BitmapBrush> brush;
    if (FAILED(rt_->CreateBitmapBrush(img.bitmap.get(), &bp, nullptr, brush.Put()))) return;
    D2D1_MATRIX_3X2_F m = {scale, 0, 0, scale, r.left + (rw - img.width * scale) / 2, r.top + (rh - img.height * scale) / 2};
    brush->SetTransform(&m);
    D2D1_ROUNDED_RECT rr = {r, radius, radius};
    rt_->FillRoundedRectangle(&rr, brush.get());
}

// The store's artwork, or the .exe's icon on a tinted tile.
void View::GameTile(const Game& g, const D2D1_RECT_F& r, float radius, bool large) {
    float w = r.right - r.left;
    if (!g.art.empty())
        if (Image* art = LoadArt(g.art, large ? 0 : w)) {
            Cover(*art, r, radius);
            Stroke(r, Rgb(dark_ ? 0xFFFFFF : 0x000000, 0.06f), radius);
            return;
        }
    D2D1_GRADIENT_STOP stops[2] = {{0, Alpha(t_.accent, dark_ ? 0.30f : 0.20f)},
                                   {1, Alpha(Rgb(0x7A45F5), dark_ ? 0.22f : 0.14f)}};
    Com<ID2D1GradientStopCollection> collection;
    Com<ID2D1LinearGradientBrush> gradient;
    rt_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, collection.Put());
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = {{r.left, r.top}, {r.right, r.bottom}};
    if (collection && SUCCEEDED(rt_->CreateLinearGradientBrush(&lp, nullptr, collection.get(), gradient.Put()))) {
        D2D1_ROUNDED_RECT rr = {r, radius, radius};
        rt_->FillRoundedRectangle(&rr, gradient.get());
    }
    float size = large ? 64 : 30;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    if (Image* icon = LoadExeIcon(g.exe)) {
        D2D1_RECT_F dst = {cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2};
        rt_->DrawBitmap(icon->bitmap.get(), &dst, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
        Icon(Glyph::File, cx, cy, size * 0.7f, t_.dim);
    }
}

void View::Logo(const D2D1_RECT_F& r) {
    if (Image* logo = LoadLogo()) rt_->DrawBitmap(logo->bitmap.get(), &r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}

// ---- scrolling ----

void View::Scroll::Clamp() { offset = std::max(0.f, std::min(offset, std::max(0.f, content - view))); }

void View::ScrollBar(Scroll& s, float x, uint32_t id) {
    s.bar = {};
    if (s.content <= s.view + 0.5f) return;
    float thumb = std::max(36.f, s.view * s.view / s.content);
    float y = s.top + (s.view - thumb) * (s.offset / (s.content - s.view));
    s.bar = Rect(x - 10, y, 10, thumb);
    bool hot = Hovered(id) || dragging_ == KindOf(id);
    Fill(Rect(x - (hot ? 7.f : 5.f), y + 2, hot ? 5.f : 3.f, thumb - 4), Alpha(t_.dim, hot ? 0.55f : 0.32f), 3);
    Region(s.bar, id);
}

void View::Wheel(float x, float, float delta) {
    Scroll& s = x < listRight_ ? list_ : detail_;
    s.offset -= delta / 120.f * 64.f;
    s.Clamp();
}

void View::BeginDrag(uint32_t bar, float y) {
    dragging_ = KindOf(bar);
    dragStart_ = y;
    dragOffset_ = (dragging_ == HitListBar ? list_ : detail_).offset;
}

void View::Drag(float y) {
    Scroll& s = dragging_ == HitListBar ? list_ : detail_;
    float thumb = std::max(36.f, s.view * s.view / s.content);
    float track = s.view - thumb;
    if (track <= 0) return;
    s.offset = dragOffset_ + (y - dragStart_) / track * (s.content - s.view);
    s.Clamp();
}

void View::ScrollToRow(int position) {
    float top = float(position) * rowHeight_, bottom = top + rowHeight_;
    if (top < list_.offset) list_.offset = top;
    else if (bottom > list_.offset + list_.view) list_.offset = bottom - list_.view;
    list_.Clamp();
}

// ---- the window ----

void View::Render(ID2D1RenderTarget* target, const AppState& s, float width, float height, float dpi) {
    if (rt_ != target) {
        DiscardDeviceResources();
        rt_ = target;
    }
    dpi_ = dpi;
    if (!brush_) {
        D2D1_COLOR_F black = Rgb(0);
        rt_->CreateSolidColorBrush(&black, nullptr, brush_.Put());
    }
    DWORD now = GetTickCount();
    dt_ = lastFrame_ ? std::min(0.05f, float(now - lastFrame_) / 1000.f) : 0.016f;
    lastFrame_ = now;
    animating_ = false;
    hits_.clear();
    clips_.clear();
    std::wstring selectedId = s.selected >= 0 ? s.games[s.selected].id : L"";
    if (selectedId != selectedId_) {
        selectedId_ = selectedId;
        detail_.offset = 0;
    }

    rt_->Clear(&t_.bg);
    const float pad = 22;
    Header(s, width);
    GpuCard(s, Rect(pad, 78, width - 2 * pad, 66));
    float top = 78 + 66 + 14;
    float detailW = std::min(470.f, std::max(370.f, width * 0.40f));
    float listW = width - 3 * pad + 8 - detailW;
    listRight_ = pad + listW + 4;
    List(s, Rect(pad, top, listW, height - top - pad));
    Details(s, Rect(pad + listW + pad - 8, top, detailW, height - top - pad));
    Toast(s, width, height);
}

void View::Header(const AppState& s, float width) {
    const float pad = 22;
    Logo(Rect(pad, 18, 42, 42));
    Text(L"DX11on10", title_.get(), pad + 54, 16, 300, t_.text);
    Text(L"Run DirectX 11 games on DirectX 10 graphics cards", small_.get(), pad + 55, 44, 500, t_.dim);
    float x = width - pad;
    float addW = ButtonWidth(L"Add game", true);
    x -= addW;
    Button(MakeHit(HitAdd), Rect(x, 24, addW, 36), L"Add game", 0, Glyph::Plus, true);
    x -= 44;
    if (s.scanning) Spinner(x + 18, 42, 7, t_.dim);
    else IconButton(MakeHit(HitRefresh), Rect(x, 24, 36, 36), Glyph::Refresh);
    x -= 40;
    IconButton(MakeHit(HitAbout), Rect(x, 24, 36, 36), Glyph::Help);
}

void View::GpuCard(const AppState& s, const D2D1_RECT_F& r) {
    Fill(r, t_.surface, 12);
    Stroke(r, t_.border, 12);
    const GpuInfo& gpu = s.gpu;
    D2D1_COLOR_F c = t_.dim;
    std::wstring title, body, badge;
    if (!s.gpuKnown) {
        title = L"Checking your graphics card…";
    } else if (!gpu.runtime) {
        c = t_.danger;
        title = L"DirectX 11 is missing from Windows";
        body = L"Windows 7 needs Service Pack 1 and the Platform Update (KB2670838) for DX11on10 and DirectX 11 games.";
    } else if (gpu.name.empty() || gpu.level == 0) {
        c = t_.warning;
        title = gpu.name.empty() ? L"No graphics card found" : gpu.name;
        body = L"Windows didn't report a DirectX 10 graphics card. Install the graphics driver, then click refresh.";
    } else if (gpu.level < 0xa000) {
        c = t_.danger;
        title = gpu.name;
        badge = LevelName(gpu.level);
        body = L"This card only supports DirectX 9. DX11on10 needs a DirectX 10 card; games can only run with CPU "
               L"rendering, which is very slow.";
    } else if (gpu.level < 0xb000) {
        c = t_.success;
        title = gpu.name;
        badge = LevelName(gpu.level);
        body = L"Ready. DirectX 11 games that refuse to start on this card can run with DX11on10.";
    } else {
        c = t_.info;
        title = gpu.name;
        badge = LevelName(gpu.level);
        body = L"This card already runs DirectX 11 games, so they don't need DX11on10. Turning it on limits a game to "
               L"DirectX 10 features: useful for testing only.";
    }
    if (gpu.others) title += Format(L"  (+%u more)", gpu.others);
    float cy = (r.top + r.bottom) / 2;
    D2D1_ELLIPSE disc = {{r.left + 34, cy}, 18, 18};
    D2D1_COLOR_F tint = Alpha(c, dark_ ? 0.18f : 0.12f);
    brush_->SetColor(&tint);
    rt_->FillEllipse(&disc, brush_.get());
    Icon(Glyph::Chip, r.left + 34, cy, 19, c);
    float badgeW = badge.empty() ? 0 : Chip(badge, 0, 0, c, true);
    float textX = r.left + 64, textW = r.right - textX - badgeW - 28;
    float th = TextHeight(title, strong_.get(), textW);
    float bh = body.empty() ? 0 : TextHeight(body, small_.get(), textW);
    float y = cy - (th + (bh ? bh + 2 : 0)) / 2;
    Text(title, strong_.get(), textX, y, textW, t_.text);
    if (!body.empty()) Text(body, small_.get(), textX, y + th + 2, textW, t_.dim, true);
    if (!badge.empty()) Chip(badge, r.right - 18 - badgeW, cy - 11, c);
}

void View::List(const AppState& s, const D2D1_RECT_F& r) {
    Fill(r, t_.surface, 12);
    Stroke(r, t_.border, 12);
    std::vector<int> visible = VisibleGames(s);
    int dx11 = 0;
    for (const Game& g : s.games)
        if ((g.apis & ApiD3D11) || g.install != Install::Off || g.store == Store::Custom) dx11++;
    Segmented(HitFilter, Rect(r.left + 14, r.top + 13, 286, 32),
              {L"DirectX 11 games  " + std::to_wstring(dx11), L"All games  " + std::to_wstring(s.games.size())},
              s.showAll ? 1 : 0);
    if (s.scanning) {
        Spinner(r.right - 26, r.top + 29, 6.5f, t_.dim);
        Text(L"Looking for games…", small_.get(), r.right - 190, r.top + 21, 150, t_.dim, false,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    float top = r.top + 58;
    Fill(Rect(r.left, top, r.right - r.left, 1), t_.border);
    D2D1_RECT_F area = {r.left + 1, top + 1, r.right - 1, r.bottom - 6};
    list_.top = area.top;
    list_.view = area.bottom - area.top;
    list_.content = float(visible.size()) * rowHeight_ + 8;
    list_.Clamp();

    if (visible.empty()) {
        float cy = (area.top + area.bottom) / 2 - 40;
        float cx = (r.left + r.right) / 2, w = r.right - r.left - 60;
        if (s.scanning && s.games.empty()) {
            Spinner(cx, cy, 13, t_.accent);
            Text(L"Looking for installed games…", body_.get(), r.left + 30, cy + 28, w, t_.dim, true,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (!s.games.empty()) {
            Text(L"None of your games use DirectX 11", h2_.get(), r.left + 30, cy - 10, w, t_.text, true,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(L"DirectX 9 and 10 games already run on DirectX 10 cards.", small_.get(), r.left + 30, cy + 14, w,
                 t_.dim, true, DWRITE_TEXT_ALIGNMENT_CENTER);
            float bw = ButtonWidth(L"Show all games", false);
            Button(MakeHit(HitShowAll), Rect(cx - bw / 2, cy + 46, bw, 34), L"Show all games", 1);
        } else {
            Fill(Rect(cx - 28, cy - 46, 56, 56), Alpha(t_.accent, 0.12f), 28);
            Icon(Glyph::Plus, cx, cy - 18, 22, t_.accent);
            Text(L"No games found", h2_.get(), r.left + 30, cy + 22, w, t_.text, true, DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(L"DX11on10 looks for Steam, GOG and Epic games. For other games, click Add game and pick the game's "
                 L".exe, or drop the .exe on this window.",
                 small_.get(), r.left + 30, cy + 48, w, t_.dim, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }
    rt_->PushAxisAlignedClip(&area, D2D1_ANTIALIAS_MODE_ALIASED);
    clips_.push_back(area);
    for (size_t i = 0; i < visible.size(); i++) {
        float y = area.top + 4 + float(i) * rowHeight_ - list_.offset;
        if (y + rowHeight_ < area.top || y > area.bottom) continue;
        Row(s, visible[i], Rect(area.left + 6, y, area.right - area.left - 12, rowHeight_));
    }
    float after = area.top + 4 + float(visible.size()) * rowHeight_ - list_.offset;
    if (!s.scanning && after + 70 < area.bottom)
        Text(L"Missing a game? Click Add game, or drop the game's .exe on this window.", small_.get(), r.left + 30,
             after + 22, r.right - r.left - 60, t_.faint, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    clips_.pop_back();
    rt_->PopAxisAlignedClip();
    ScrollBar(list_, r.right - 2, MakeHit(HitListBar));
}

void View::Row(const AppState& s, int index, const D2D1_RECT_F& r) {
    const Game& g = s.games[index];
    bool selected = index == s.selected;
    uint32_t id = MakeHit(HitRow, uint32_t(index));
    D2D1_RECT_F box = {r.left, r.top + 2, r.right, r.bottom - 2};
    if (selected) {
        Fill(box, t_.selected, 10);
        Fill(Rect(box.left, box.top + 14, 3, box.bottom - box.top - 28), t_.accent, 1.5f);
    } else if (Hovered(id) || Hovered(MakeHit(HitRowSwitch, uint32_t(index)))) {
        Fill(box, t_.hover, 10);
    }
    Region(box, id);
    float cy = (box.top + box.bottom) / 2;
    GameTile(g, Rect(box.left + 12, cy - 24, 103, 48), 7, false);

    bool on = g.install == Install::On || g.install == Install::Outdated;
    bool usable = g.machine == IMAGE_FILE_MACHINE_AMD64 || g.machine == IMAGE_FILE_MACHINE_I386;
    float switchX = box.right - 14 - 40;
    Switch(MakeHit(HitRowSwitch, uint32_t(index)), L"row:" + g.id, switchX, cy - 11, 40, 22, on, usable || on);

    std::vector<std::pair<std::wstring, D2D1_COLOR_F>> chips;
    if (g.running) chips.push_back({L"Running", t_.info});
    if (g.install == Install::On) chips.push_back({L"On", t_.success});
    else if (g.install == Install::Outdated) chips.push_back({L"Update", t_.warning});
    else if (g.install == Install::Foreign) chips.push_back({L"Other d3d11.dll", t_.warning});
    if (!g.antiCheat.empty()) chips.push_back({L"Anti-cheat", t_.danger});
    float x = switchX - 10;
    for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
        float w = Chip(it->first, 0, 0, it->second, true);
        if (x - w < box.left + 220) break;
        x -= w;
        Chip(it->first, x, cy - 11, it->second);
        x -= 6;
    }
    float textX = box.left + 128, textW = x - textX - 8;
    Text(g.name, h2_.get(), textX, cy - 20, textW, t_.text);
    std::wstring sub = StoreName(g.store);
    if (g.machine) sub += L"  ·  " + MachineName(g.machine);
    std::wstring apis = ApiNames(g.apis);
    if (!apis.empty()) sub += L"  ·  " + apis;
    Text(sub, small_.get(), textX, cy + 2, textW, t_.dim);
}

void View::Details(const AppState& s, const D2D1_RECT_F& r) {
    Fill(r, t_.surface, 12);
    Stroke(r, t_.border, 12);
    if (s.selected < 0) {
        Text(s.scanning ? L"" : L"Select a game", body_.get(), r.left, (r.top + r.bottom) / 2 - 10, r.right - r.left,
             t_.dim, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    const Game& g = s.games[s.selected];
    D2D1_RECT_F area = {r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    detail_.top = area.top;
    detail_.view = area.bottom - area.top;
    rt_->PushAxisAlignedClip(&area, D2D1_ANTIALIAS_MODE_ALIASED);
    clips_.push_back(area);

    const float x = r.left + 20, w = r.right - r.left - 40;
    const float start = r.top + 20 - detail_.offset;
    float y = start;
    float heroH = std::min(w * 215.f / 460.f, 210.f);
    GameTile(g, Rect(x, y, w, heroH), 10, true);
    y += heroH + 18;
    y += Text(g.name, h1_.get(), x, y, w, t_.text, true) + 8;

    // store, CPU, APIs
    float cx = x;
    auto chip = [&](const std::wstring& text, D2D1_COLOR_F c) {
        float cw = Chip(text, 0, 0, c, true);
        if (cx + cw > x + w) {
            cx = x;
            y += 28;
        }
        Chip(text, cx, y, c);
        cx += cw + 6;
    };
    chip(StoreName(g.store), t_.dim);
    if (g.machine) chip(MachineName(g.machine), t_.dim);
    if (g.apis) chip(ApiNames(g.apis), (g.apis & ApiD3D11) ? t_.accent : t_.dim);
    for (const std::wstring& a : g.antiCheat) chip(a, t_.danger);
    y += 22 + 18;

    // the main switch
    bool on = g.install == Install::On || g.install == Install::Outdated;
    bool usable = g.machine == IMAGE_FILE_MACHINE_AMD64 || g.machine == IMAGE_FILE_MACHINE_I386;
    std::wstring status;
    D2D1_COLOR_F dot = t_.faint;
    if (g.install == Install::On) {
        status = L"On. The game starts with DX11on10 " DX11ON10_VERSION_W L".";
        dot = t_.success;
    } else if (g.install == Install::Outdated) {
        status = L"On, with " + (g.installedVersion.empty() ? std::wstring(L"another version") : L"version " + g.installedVersion) +
                 L". Update to use this app's version.";
        dot = t_.warning;
    } else if (g.install == Install::Foreign) {
        status = L"Off. The game folder has " + g.foreignName + L"'s d3d11.dll.";
    } else {
        status = L"Off. The game runs as it normally does.";
    }
    float cardTextW = w - 32 - 60;
    float sh = TextHeight(status, small_.get(), cardTextW - 14);
    float cardH = 16 + 20 + 4 + sh + 16 + 36 + 16;
    D2D1_RECT_F card = Rect(x, y, w, cardH);
    Fill(card, t_.surface2, 12);
    Stroke(card, t_.border, 12);
    Text(L"DX11on10 for this game", h2_.get(), x + 16, y + 16, cardTextW, t_.text);
    D2D1_ELLIPSE d = {{x + 20, y + 16 + 24 + 7.5f}, 3.5f, 3.5f};
    brush_->SetColor(&dot);
    rt_->FillEllipse(&d, brush_.get());
    Text(status, small_.get(), x + 30, y + 16 + 24, cardTextW - 14, t_.dim, true);
    Switch(MakeHit(HitSwitch), L"main:" + g.id, x + w - 16 - 50, y + 18, 50, 28, on, usable || on);
    float by = y + cardH - 16 - 36;
    float bx = x + 16;
    float pw = ButtonWidth(L"Play", true);
    Button(MakeHit(HitPlay), Rect(bx, by, pw, 36), L"Play", on ? 0 : 1, Glyph::Play, true);
    bx += pw + 8;
    if (g.install == Install::Outdated) {
        float uw = ButtonWidth(L"Update", false);
        Button(MakeHit(HitUpdate), Rect(bx, by, uw, 36), L"Update", 1);
    }
    y += cardH + 14;

    for (const Notice& n : GameNotices(g, s.gpu)) y += Banner(n, x, y, w) + 8;
    y += 14;

    // settings
    Text(L"SETTINGS", section_.get(), x, y, w, t_.faint);
    y += 22;
    auto setting = [&](const std::wstring& label, const std::wstring& help, float controlW, auto&& control) {
        float lw = w - controlW - 16;
        float lh = TextHeight(label, strong_.get(), lw);
        float hh = TextHeight(help, small_.get(), lw);
        float h = std::max(lh + 2 + hh, 32.f);
        Text(label, strong_.get(), x, y, lw, t_.text, true);
        Text(help, small_.get(), x, y + lh + 2, lw, t_.dim, true);
        control(x + w - controlW, y + std::max(0.f, (h - 32) / 2));
        y += h + 16;
    };
    setting(L"Tell the game it has DirectX 11", L"Most DirectX 11 games need this to start on a DirectX 10 card.", 196,
            [&](float cx2, float cy2) {
                Segmented(HitFake, Rect(cx2, cy2, 196, 32), {L"Always", L"Only if needed"},
                          g.settings.fakeAlways ? 0 : 1);
            });
    setting(L"Graphics level", L"Automatic uses DirectX 10.1 when the card has it. Try 10.0 if a game shows errors.", 196,
            [&](float cx2, float cy2) {
                Segmented(HitLevel, Rect(cx2, cy2, 196, 32), {L"Automatic", L"10.0"}, g.settings.level100 ? 1 : 0);
            });
    setting(L"CPU rendering", L"Draws on the processor instead of the graphics card. Very slow: only for cards without "
                              L"DirectX 10.",
            46, [&](float cx2, float cy2) {
                Switch(MakeHit(HitWarp), L"warp:" + g.id, cx2, cy2 + 5, 46, 24, g.settings.warp, true);
            });
    if (on) {
        Text(L"Changes apply the next time the game starts.", small_.get(), x, y - 6, w, t_.faint);
        y += 18;
    }
    y += 8;

    // last run
    if (on && g.report.present && !g.report.realLevel.empty()) {
        const RunReport& rep = g.report;
        Text(L"LAST RUN", section_.get(), x, y, w, t_.faint);
        Text(Ago(rep.written), small_.get(), x, y - 1, w, t_.faint, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 22;
        struct Stat {
            std::wstring label, value;
        };
        std::vector<Stat> stats = {
            {L"Graphics card ran", LevelText(rep.realLevel)},
            {L"Game was told", LevelText(rep.gameLevel)},
            {L"Frame rate", rep.fps > 0 ? Format(L"%.0f fps", rep.fps) : std::wstring(L"–")},
            {L"Shaders converted", rep.converted ? Thousands(rep.converted) : std::wstring(L"–")},
        };
        float tw = (w - 10) / 2;
        for (size_t i = 0; i < stats.size(); i++) {
            float tx = x + float(i % 2) * (tw + 10), ty = y + float(i / 2) * 70;
            Fill(Rect(tx, ty, tw, 62), t_.surface2, 10);
            Stroke(Rect(tx, ty, tw, 62), t_.border, 10);
            Text(stats[i].label, small_.get(), tx + 14, ty + 10, tw - 28, t_.dim);
            Text(stats[i].value, h2_.get(), tx + 14, ty + 30, tw - 28, t_.text);
        }
        y += 140 + 12;
    }

    // files
    Text(L"GAME FILES", section_.get(), x, y, w, t_.faint);
    y += 22;
    std::wstring where = g.exe.empty() ? L"No .exe selected" : g.exe;
    y += Text(where, small_.get(), x, y, w, t_.dim, true) + 12;
    float fx = x;
    auto fileButton = [&](uint32_t hit, const std::wstring& label, int style, Glyph glyph, bool hasGlyph) {
        float bw = ButtonWidth(label, hasGlyph);
        if (fx + bw > x + w) {
            fx = x;
            y += 44;
        }
        Button(hit, Rect(fx, y, bw, 36), label, style, glyph, hasGlyph);
        fx += bw + 8;
    };
    fileButton(MakeHit(HitFolder), L"Open folder", 1, Glyph::Folder, true);
    if (on && g.report.present) fileButton(MakeHit(HitLog), L"Open log", 1, Glyph::Document, true);
    fileButton(MakeHit(HitChangeExe), L"Change .exe…", 1, Glyph::File, false);
    if (g.store == Store::Custom) fileButton(MakeHit(HitRemove), L"Remove from list", 2, Glyph::Close, false);
    y += 36 + 22;
    y += Text(L"DX11on10 is experimental. Some games won't start, and graphics can look different from a DirectX 11 "
              L"card. Turning it off removes everything it added.",
              small_.get(), x, y, w, t_.faint, true);
    y += 20;

    detail_.content = y - start + 20;
    detail_.Clamp();
    clips_.pop_back();
    rt_->PopAxisAlignedClip();
    ScrollBar(detail_, r.right - 2, MakeHit(HitDetailBar));
}

void View::Toast(const AppState& s, float width, float height) {
    if (s.toast.empty()) return;
    float age = float(GetTickCount() - s.toastAt) / 1000.f;
    const float shown = 4.5f;
    if (age > shown) return;
    float alpha = std::min(1.f, std::min(age / 0.15f, (shown - age) / 0.4f));
    animating_ = true;
    D2D1_COLOR_F c = LevelColor(s.toastLevel, t_.info, t_.success, t_.warning, t_.danger);
    float maxW = std::min(640.f, width - 80);
    float tw = std::min(TextWidth(s.toast, strong_.get()), maxW - 60);
    float th = TextHeight(s.toast, strong_.get(), tw + 1);
    float w = tw + 60, h = std::max(46.f, th + 24);
    float x = (width - w) / 2, y = height - 34 - h + (1 - alpha) * 12;
    D2D1_COLOR_F bg = dark_ ? Rgb(0x2A2F39) : Rgb(0x1E222B);
    Fill(Rect(x, y + 2, w, h), Rgb(0x000000, 0.18f * alpha), 12);
    Fill(Rect(x, y, w, h), Alpha(bg, alpha), 12);
    Glyph g = s.toastLevel == Level::Danger    ? Glyph::Danger
              : s.toastLevel == Level::Warning ? Glyph::Warning
              : s.toastLevel == Level::Success ? Glyph::Check
                                               : Glyph::Info;
    bool wasDark = dark_;
    dark_ = true;  // the icon's inner mark matches the dark toast
    Icon(g, x + 26, y + h / 2, 18, Alpha(c, alpha));
    dark_ = wasDark;
    Com<IDWriteTextLayout> l = Layout(s.toast, strong_.get(), tw + 1, true);
    D2D1_COLOR_F white = Rgb(0xFFFFFF, alpha);
    brush_->SetColor(&white);
    rt_->DrawTextLayout({x + 44, y + (h - th) / 2}, l.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

}  // namespace app
