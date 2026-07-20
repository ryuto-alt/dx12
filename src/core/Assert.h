#pragma once

#include <Windows.h>
#include <comdef.h>
#include <cstdio>
#include <stdexcept>
#include <string>

// Logger は前方参照で使う
#include "Logger.h"

namespace dx12e
{

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        _com_error err(hr);
        LPCTSTR errMsg = err.ErrorMessage();
        // wchar_t → char 安全変換
        int size = WideCharToMultiByte(CP_UTF8, 0, errMsg, -1, nullptr, 0, nullptr, nullptr);
        std::string msg(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, errMsg, -1, msg.data(), size, nullptr, nullptr);
        msg.pop_back();
        char hexBuf[16];
        snprintf(hexBuf, sizeof(hexBuf), "0x%08X", static_cast<unsigned int>(hr));
        throw std::runtime_error(std::string("HRESULT failed (") + hexBuf + "): " + msg);
    }
}

} // namespace dx12e

#ifdef _DEBUG
#define DX_ASSERT(condition, message)                           \
    do {                                                        \
        if (!(condition))                                       \
        {                                                       \
            dx12e::Logger::Error("アサーション失敗: {}", message); \
            __debugbreak();                                     \
        }                                                       \
    } while (false)
#else
#define DX_ASSERT(condition, message) ((void)0)
#endif
