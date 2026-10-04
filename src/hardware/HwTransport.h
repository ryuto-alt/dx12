#pragma once
// 抽象トランスポート。IO スレッドだけが触る（スレッド安全は要求しない）。
#include <cstdint>
#include <string>
#include <string_view>

namespace dx12e::hw
{

class IHwTransport
{
public:
    virtual ~IHwTransport() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // 最大 max バイトを読む。最大 ~20ms ブロックして返ってよい。
    // 戻り値: 読めたバイト数（0=無し）、-1=切断・エラー
    virtual int Read(uint8_t* buf, int max) = 0;
    virtual bool Write(std::string_view data) = 0;

    virtual std::string Describe() const = 0;    // 人間向けの説明（"COM8 115200bps" など）
    virtual std::string PortName() const = 0;    // ポート識別子（"COM8" / "virtual:radio"）
    virtual std::string LastError() const { return {}; }
};

} // namespace dx12e::hw
