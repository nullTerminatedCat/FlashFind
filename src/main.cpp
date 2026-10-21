// 闪寻 FlashFind —— 入口
#include "App.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    ff::App app;
    return app.Run(inst, show);
}
