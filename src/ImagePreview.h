#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace ff {

// 按住空格：文件名右侧固定尺寸小预览；外框透明，图片等比放入
class ImagePreview {
public:
    ~ImagePreview();
    // anchorScreen：通常为文件名区域右侧一点
    bool Show(HWND owner, const std::wstring& path, POINT anchorScreen);
    void Hide();
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    bool LoadImageFile(const std::wstring& path);
    bool LoadTextFile(const std::wstring& path);
    void PlaceNear(POINT anchor, int w, int h);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HBITMAP bmp_ = nullptr;
    int imgW_ = 0, imgH_ = 0;
    std::wstring text_;
    std::wstring title_;
    bool isText_ = false;
    HFONT font_ = nullptr;
};

bool IsImageFileName(std::wstring_view name);
bool IsTextFileName(std::wstring_view name);

} // namespace ff
