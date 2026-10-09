#include "hardware/HwSerialTransport.h"

#include <windows.h>

namespace dx12e::hw
{

namespace
{
HANDLE H(void* p) { return static_cast<HANDLE>(p); }

std::string ErrText(const char* what, DWORD code)
{
    return std::string(what) + " (Win32 error " + std::to_string(code) + ")";
}
} // namespace

HwSerialTransport::HwSerialTransport(std::string portName, int baud, bool dtr)
    : m_port(std::move(portName)), m_baud(baud), m_dtr(dtr)
{
}

HwSerialTransport::~HwSerialTransport() { Close(); }

bool HwSerialTransport::Open()
{
    Close();
    m_lastError.clear();

    // "COM10" 以上は \\.\ 接頭辞が必須。ASCII 前提なので単純拡張でよい。
    std::wstring wpath = L"\\\\.\\";
    for (char c : m_port) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));

    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        m_lastError = ErrText("ポートを開けません", ::GetLastError());
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb))
    {
        m_lastError = ErrText("GetCommState に失敗", ::GetLastError());
        ::CloseHandle(h);
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(m_baud);
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = m_dtr ? DTR_CONTROL_ENABLE    // Nano Every: 書き込み直後でもシリアルを流させる
                            : DTR_CONTROL_DISABLE;  // DTR=off（リセット / 書き込みモードに落とさない）
    dcb.fRtsControl = RTS_CONTROL_DISABLE;   // RTS=off
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!::SetCommState(h, &dcb))
    {
        m_lastError = ErrText("SetCommState に失敗", ::GetLastError());
        ::CloseHandle(h);
        return false;
    }

    // 読み取りは待たずに即座に返す（受信バッファにある分だけ）。
    // ★IO スレッドは 1 本で全デバイスを順に回すので、ここで 20ms 待つと台数ぶん遅れが積み上がり、
    //   心拍と出力の送信まで遅れる。待ちは IO ループ側の「何も無ければ 3ms 寝る」だけにする。
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 200;
    ::SetCommTimeouts(h, &to);
    ::SetupComm(h, 4096, 4096);
    ::PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    m_handle = h;
    return true;
}

void HwSerialTransport::Close()
{
    if (m_handle)
    {
        ::CloseHandle(H(m_handle));
        m_handle = nullptr;
    }
}

int HwSerialTransport::Read(uint8_t* buf, int max)
{
    if (!m_handle) return -1;
    DWORD got = 0;
    if (!::ReadFile(H(m_handle), buf, static_cast<DWORD>(max), &got, nullptr))
    {
        m_lastError = ErrText("ReadFile に失敗（抜かれた可能性）", ::GetLastError());
        return -1;
    }
    return static_cast<int>(got);
}

bool HwSerialTransport::Write(std::string_view data)
{
    if (!m_handle) return false;
    size_t off = 0;
    while (off < data.size())
    {
        DWORD wrote = 0;
        if (!::WriteFile(H(m_handle), data.data() + off, static_cast<DWORD>(data.size() - off), &wrote, nullptr))
        {
            m_lastError = ErrText("WriteFile に失敗", ::GetLastError());
            return false;
        }
        if (wrote == 0) { m_lastError = "WriteFile がタイムアウトしました"; return false; }
        off += wrote;
    }
    return true;
}

std::string HwSerialTransport::Describe() const
{
    return m_port + " " + std::to_string(m_baud) + "bps";
}

bool HwSerialTransport::PulseReset()
{
    if (!m_handle) return false;
    if (!::EscapeCommFunction(H(m_handle), SETRTS)) return false;
    ::Sleep(120);
    return ::EscapeCommFunction(H(m_handle), CLRRTS) != 0;
}

} // namespace dx12e::hw
