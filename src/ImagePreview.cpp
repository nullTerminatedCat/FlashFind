#include "ImagePreview.h"
#include <wincodec.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

namespace ff {
namespace {

constexpr size_t kMaxTextBytes = 256 * 1024;
constexpr size_t kMaxTextCharsPaint = 6000;

// 色键：外框/留白透明（非黑色底）
constexpr COLORREF kClear = RGB(255, 0, 255);

// 固定预览客户区逻辑像素（与图片原始尺寸无关）
constexpr int kFixedW = 320;
constexpr int kFixedH = 240;

void FoldAscii(std::wstring& s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
}

std::wstring_view ExtOf(std::wstring_view name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot + 1 >= name.size()) return {};
    return name.substr(dot + 1);
}

bool ExtIn(std::wstring_view ext, std::initializer_list<const wchar_t*> list) {
    std::wstring e(ext); FoldAscii(e);
    for (auto x : list) if (e == x) return true;
    return false;
}

int DpiOf(HWND h) {
    UINT d = 96;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        using Fn = UINT (WINAPI*)(HWND);
        if (auto f = reinterpret_cast<Fn>(GetProcAddress(u, "GetDpiForWindow"))) d = f(h);
    }
    return static_cast<int>(d ? d : 96);
}

int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

HBITMAP WicToDib(IWICBitmapSource* src, int& outW, int& outH) {
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || w == 0 || h == 0) return nullptr;
    outW = static_cast<int>(w); outH = static_cast<int>(h);
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return nullptr;
    IWICFormatConverter* conv = nullptr;
    HBITMAP hbmp = nullptr;
    if (SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = outW;
        bi.bmiHeader.biHeight = -outH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        hbmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hbmp && bits) {
            const UINT stride = static_cast<UINT>(outW * 4);
            if (FAILED(conv->CopyPixels(nullptr, stride, stride * outH, static_cast<BYTE*>(bits)))) {
                DeleteObject(hbmp); hbmp = nullptr;
            }
        }
    }
    if (conv) conv->Release();
    fac->Release();
    return hbmp;
}

std::wstring DecodeBytes(const std::vector<char>& raw, bool& truncated) {
    truncated = false;
    if (raw.empty()) return {};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    size_t n = raw.size();
    auto asUtf16 = [&](bool be) {
        size_t start = (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) ? 2 : 0;
        size_t bytes = n - start; if (bytes & 1) --bytes;
        std::wstring out; out.resize(bytes / 2);
        for (size_t i = 0; i < out.size(); ++i) {
            unsigned char a = p[start + i * 2], b = p[start + i * 2 + 1];
            out[i] = static_cast<wchar_t>(be ? (a << 8) | b : a | (b << 8));
        }
        return out;
    };
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) return asUtf16(false);
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) return asUtf16(true);
    size_t start = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data() + start, static_cast<int>(n - start), nullptr, 0);
    UINT cp = CP_UTF8;
    if (need <= 0) {
        need = MultiByteToWideChar(CP_ACP, 0, raw.data() + start, static_cast<int>(n - start), nullptr, 0);
        cp = CP_ACP;
    }
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, raw.data() + start, static_cast<int>(n - start), out.data(), need);
    return out;
}

} // namespace

bool IsImageFileName(std::wstring_view name) {
    return ExtIn(ExtOf(name), {
        L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"gif", L"tif", L"tiff",
        L"ico", L"webp", L"heic", L"heif", L"jxl", L"avif", L"wdp", L"hdp", L"dds",
    });
}

bool IsTextFileName(std::wstring_view name) {
    return ExtIn(ExtOf(name), {
        L"txt", L"log", L"md", L"markdown", L"csv", L"tsv", L"json", L"xml", L"html", L"htm", L"css", L"js", L"ts",
        L"c", L"cc", L"cpp", L"cxx", L"h", L"hpp", L"hxx", L"cs", L"java", L"py", L"rb", L"go", L"rs", L"swift",
        L"kt", L"kts", L"php", L"sql", L"yaml", L"yml", L"toml", L"ini", L"cfg", L"conf", L"properties",
        L"cmake", L"makefile", L"mk", L"sh", L"bat", L"cmd", L"ps1", L"psm1", L"vbs", L"reg", L"gitignore",
        L"editorconfig", L"gitattributes", L"dockerfile", L"gradle", L"r", L"m", L"mm", L"pl", L"pm", L"lua",
        L"vim", L"tex", L"bib", L"rtf", L"svg", L"diff", L"patch", L"nfo", L"asc", L"lst", L"me", L"readme",
    });
}

ImagePreview::~ImagePreview() { Hide(); }

bool ImagePreview::LoadImageFile(const std::wstring& path) {
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    imgW_ = imgH_ = 0;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        IWICBitmapFrameDecode* frame = nullptr;
        if (SUCCEEDED(dec->GetFrame(0, &frame))) {
            bmp_ = WicToDib(frame, imgW_, imgH_);
            ok = bmp_ != nullptr;
            frame->Release();
        }
        dec->Release();
    }
    fac->Release();
    return ok;
}

bool ImagePreview::LoadTextFile(const std::wstring& path) {
    text_.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    size_t toRead = static_cast<size_t>((std::min)(sz.QuadPart, static_cast<LONGLONG>(kMaxTextBytes)));
    std::vector<char> buf(toRead);
    DWORD got = 0;
    bool ok = toRead == 0 || ReadFile(f, buf.data(), static_cast<DWORD>(toRead), &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    buf.resize(got);
    bool truncFile = static_cast<LONGLONG>(got) < sz.QuadPart;
    bool dummy = false;
    text_ = DecodeBytes(buf, dummy);
    if (text_.size() > kMaxTextCharsPaint) {
        text_.resize(kMaxTextCharsPaint);
        truncFile = true;
    }
    std::wstring norm; norm.reserve(text_.size());
    for (size_t i = 0; i < text_.size(); ++i) {
        wchar_t c = text_[i];
        if (c == L'\r') { if (i + 1 < text_.size() && text_[i + 1] == L'\n') continue; norm += L'\n'; }
        else norm += c;
    }
    text_ = std::move(norm);
    if (truncFile) text_ += L"\n\n…（仅预览开头）";
    return true;
}

void ImagePreview::PlaceNear(POINT anchor, int w, int h) {
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    const int gap = 4;   // 文件名右侧一点点
    int x = anchor.x + gap;
    int y = anchor.y;
    if (x + w > wa.right) x = anchor.x - gap - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

bool ImagePreview::Show(HWND owner, const std::wstring& path, POINT anchorScreen) {
    Hide();
    owner_ = owner;
    size_t slash = path.find_last_of(L"/\\");
    title_ = slash == std::wstring::npos ? path : path.substr(slash + 1);
    isText_ = false;

    bool loaded = false;
    if (IsImageFileName(title_)) {
        loaded = LoadImageFile(path);
        isText_ = false;
    } else if (IsTextFileName(title_) || ExtOf(title_).empty()) {
        loaded = LoadTextFile(path);
        isText_ = loaded;
    } else {
        if (LoadImageFile(path)) { loaded = true; isText_ = false; }
        else {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
                fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 0 && fad.nFileSizeLow <= 64 * 1024) {
                loaded = LoadTextFile(path);
                isText_ = loaded;
            }
        }
    }
    if (!loaded) return false;

    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"FlashFindImagePreview";
        atom = RegisterClassExW(&wc);
        if (!atom) atom = 1;
    }

    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    const int cw = Scale(kFixedW, dpi);
    const int ch = Scale(kFixedH, dpi);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"FlashFindImagePreview", L"",
        WS_POPUP,
        0, 0, 1, 1,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) { Hide(); return false; }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // 品红作色键 → 外框/留白透明，不再用黑色底板
    SetLayeredWindowAttributes(hwnd_, kClear, 0, LWA_COLORKEY);

    font_ = CreateFontW(Scale(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font_) font_ = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    PlaceNear(anchorScreen, cw, ch);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    return true;
}

void ImagePreview::Hide() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    if (font_ && font_ != GetStockObject(DEFAULT_GUI_FONT)) { DeleteObject(font_); }
    font_ = nullptr;
    imgW_ = imgH_ = 0;
    text_.clear(); title_.clear();
    isText_ = false;
    owner_ = nullptr;
}

void ImagePreview::Paint(HDC dc) {
    RECT rc{}; GetClientRect(hwnd_, &rc);
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP dib = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    // 色键底（透明）
    HBRUSH clear = CreateSolidBrush(kClear);
    FillRect(mem, &rc, clear);
    DeleteObject(clear);

    if (isText_) {
        // 文本需要可读底：半暗圆角矩形感用实心小面板（非整窗黑框）
        RECT panel{rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2};
        HBRUSH panelBr = CreateSolidBrush(RGB(40, 40, 44));
        FillRect(mem, &panel, panelBr);
        DeleteObject(panelBr);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(170, 170, 180));
        RECT titleRc{panel.left + 8, panel.top + 4, panel.right - 8, panel.top + 22};
        HFONT ui = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(mem, ui ? ui : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(mem, title_.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, oldF);
        if (ui) DeleteObject(ui);
        RECT body{panel.left + 8, panel.top + 24, panel.right - 8, panel.bottom - 8};
        SetTextColor(mem, RGB(230, 230, 235));
        HGDIOBJ of = SelectObject(mem, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(mem, text_.c_str(), -1, &body, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL);
        SelectObject(mem, of);
    } else if (bmp_ && imgW_ > 0 && imgH_ > 0) {
        // 固定窗内等比缩放居中；周围保持色键透明
        int maxW = (std::max)(1, W), maxH = (std::max)(1, H);
        double s = (std::min)(static_cast<double>(maxW) / imgW_, static_cast<double>(maxH) / imgH_);
        int dw = (std::max)(1, static_cast<int>(imgW_ * s));
        int dh = (std::max)(1, static_cast<int>(imgH_ * s));
        int dx = (maxW - dw) / 2, dy = (maxH - dh) / 2;
        HDC img = CreateCompatibleDC(mem);
        HGDIOBJ old = SelectObject(img, bmp_);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        // 先不透明画到临时，避免 AlphaBlend 与色键打架导致半透发黑
        StretchBlt(mem, dx, dy, dw, dh, img, 0, 0, imgW_, imgH_, SRCCOPY);
        SelectObject(img, old);
        DeleteDC(img);
    }

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
}

LRESULT CALLBACK ImagePreview::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<ImagePreview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<LPCREATESTRUCTW>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (self) self->Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if (self) self->Hide();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace ff
