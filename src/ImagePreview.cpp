#include "ImagePreview.h"
#include <wincodec.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace ff {
namespace {

constexpr size_t kMaxTextBytes = 512 * 1024;   // 超过则只读前缀并提示截断
constexpr size_t kMaxTextCharsPaint = 120000;  // 绘制上限，避免 GDI 卡死

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

// 探测编码：UTF-16 LE/BE BOM、UTF-8 BOM、否则当 UTF-8（校验），失败则 CP_ACP
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
    // 统一换行
    std::wstring norm; norm.reserve(text_.size());
    for (size_t i = 0; i < text_.size(); ++i) {
        wchar_t c = text_[i];
        if (c == L'\r') { if (i + 1 < text_.size() && text_[i + 1] == L'\n') continue; norm += L'\n'; }
        else norm += c;
    }
    text_ = std::move(norm);
    if (truncFile) text_ += L"\n\n…（文件较大，仅预览开头）";
    return true;
}

bool ImagePreview::Show(HWND owner, const std::wstring& path) {
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
        // 未知扩展名：先试 WIC，再试小文件当文本（≤64KiB）
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
        wc.lpszClassName = L"FlashFindImagePreview";
        atom = RegisterClassExW(&wc);
    }

    RECT orc{}; GetWindowRect(owner, &orc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"FlashFindImagePreview", L"",
        WS_POPUP,
        orc.left, orc.top, orc.right - orc.left, orc.bottom - orc.top,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) { Hide(); return false; }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    font_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font_) font_ = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    ShowWindow(hwnd_, SW_SHOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    HBRUSH br = CreateSolidBrush(RGB(22, 22, 26));
    FillRect(dc, &rc, br);
    DeleteObject(br);

    // 顶栏标题
    RECT titleRc{rc.left + 16, rc.top + 8, rc.right - 16, rc.top + 36};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(180, 180, 190));
    HFONT ui = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(dc, ui ? ui : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, title_.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldF);
    if (ui) DeleteObject(ui);

    RECT body{rc.left + 16, rc.top + 44, rc.right - 16, rc.bottom - 16};

    if (isText_) {
        SetTextColor(dc, RGB(230, 230, 235));
        HGDIOBJ of = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, text_.c_str(), -1, &body, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL);
        SelectObject(dc, of);
        return;
    }
    if (!bmp_ || imgW_ <= 0 || imgH_ <= 0) return;
    int maxW = (std::max)(1, static_cast<int>(body.right - body.left)), maxH = (std::max)(1, static_cast<int>(body.bottom - body.top));
    double sx = static_cast<double>(maxW) / imgW_, sy = static_cast<double>(maxH) / imgH_;
    double s = (std::min)(1.0, (std::min)(sx, sy));
    int dw = (std::max)(1, static_cast<int>(imgW_ * s));
    int dh = (std::max)(1, static_cast<int>(imgH_ * s));
    int dx = body.left + (maxW - dw) / 2, dy = body.top + (maxH - dh) / 2;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp_);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, dx, dy, dw, dh, mem, 0, 0, imgW_, imgH_, bf);
    SelectObject(mem, old);
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
