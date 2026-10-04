#pragma once
// Win32 シリアル。DTR=off / RTS=off で開く（ESP32 を書き込みモードに落とさない）。
#include "hardware/HwTransport.h"

#include <cstdint>
#include <string>

namespace dx12e::hw
{

class HwSerialTransport final : public IHwTransport
{
public:
    HwSerialTransport(std::string portName, int baud);
    ~HwSerialTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override { return m_handle != nullptr; }
    int  Read(uint8_t* buf, int max) override;
    bool Write(std::string_view data) override;
    std::string Describe() const override;
    std::string PortName() const override { return m_port; }
    std::string LastError() const override { return m_lastError; }

    // RTS を 120ms 立ててリセットを掛ける（Uno / ESP32 DevKit の EN）。普段は呼ばない。
    bool PulseReset();

private:
    std::string m_port;
    int         m_baud;
    void*       m_handle = nullptr;   // HANDLE（windows.h をヘッダに出さない）
    std::string m_lastError;
};

} // namespace dx12e::hw
