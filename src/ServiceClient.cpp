#include "ServiceClient.h"
#include <windows.h>

namespace ff {
bool ServiceClient::Connect(const std::wstring& pipe) {
    // TODO(M3): CreateFileW(pipe) + SetNamedPipeHandleState(PIPE_READMODE_MESSAGE)，握手版本号
    // 现在只探测管道是否存在，以便日后启用
    return WaitNamedPipeW(pipe.c_str(), 0) != FALSE && false;
}
}
