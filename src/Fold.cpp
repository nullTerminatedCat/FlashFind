#include "Fold.h"

namespace ff {
uint16_t g_fold[65536];
bool g_fastAscii = true;

void InitFold() {
    static bool done = false;
    if (done) return;
    for (uint32_t i = 0; i < 65536; ++i) g_fold[i] = static_cast<uint16_t>(i);
    // 代理区不折叠
    CharLowerBuffW(reinterpret_cast<wchar_t*>(g_fold), 0xD800);
    CharLowerBuffW(reinterpret_cast<wchar_t*>(g_fold) + 0xE000, 0x2000);
    done = true;
}
}
