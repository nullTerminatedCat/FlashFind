#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace ff {

// 按住空格：图片（WIC）或文本预览浮层；松开关闭
class ImagePreview {
public:
    ~ImagePreview();
    // 按扩展名尝试图片或文本；失败返回 false（不显示）
    bool Show(HWND owner, const std::wstring& path);
    void Hide();
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    bool LoadImageFile(const std::wstring& path);
    bool LoadTextFile(const std::wstring& path);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HBITMAP bmp_ = nullptr;
    int imgW_ = 0, imgH_ = 0;
    std::wstring text_;          // 文本模式（已截断）
    std::wstring title_;         // 文件名，画在顶栏
    bool isText_ = false;
    HFONT font_ = nullptr;
};

bool IsImageFileName(std::wstring_view name);
bool IsTextFileName(std::wstring_view name);

} // namespace ff
