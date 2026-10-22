#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace ff {

// 按住空格：在锚点（选中行/光标）右下角附近显示小预览浮层，松开关闭
class ImagePreview {
public:
    ~ImagePreview();
    // anchorScreen：屏幕坐标锚点（通常为选中行右下或光标）；失败返回 false
    bool Show(HWND owner, const std::wstring& path, POINT anchorScreen);
    void Hide();
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    bool LoadImageFile(const std::wstring& path);
    bool LoadTextFile(const std::wstring& path);
    SIZE MeasureContent(HDC dc, int maxW, int maxH) const;
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
