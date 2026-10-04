#pragma once
// ============================================================================
// ハードウェア連携プロトコル v1 の純ロジック（docs/HARDWARE.md §1）。
// Win32 / Logger / nlohmann に依存しない（tests が HwProtocol.cpp 単体でビルドする）。
//   - HwLineAssembler : バイト列 → 行（\n 終端、\r 無視、128 バイト超は捨てて数える）
//   - ParseDeviceLine : デバイス → エンジンの 1 行を HwMessage へ
//   - HwFormat*       : エンジン → デバイスの行を作る（末尾の \n は付けない）
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dx12e::hw
{

inline constexpr size_t kMaxLineBytes = 128;   // 1 行の上限（改行を除く）

enum class HwChType { Bool, Int, Float };

// @ch 宣言 1 件。
struct HwChannelDecl
{
    std::string name;
    bool        isOutput = false;
    HwChType    type     = HwChType::Float;
    bool        hasRange = false;
    double      min      = 0.0;
    double      max      = 1.0;
    double      safe     = 0.0;   // 出力の安全値（生値）
};

enum class HwMsgType { Unknown, Hello, Channel, Ready, Values, Pong, Log, Err };

struct HwMessage
{
    HwMsgType type = HwMsgType::Unknown;

    // Hello
    std::string name;
    int         proto = 0;
    int         fw    = 0;
    std::string board;

    // Channel
    HwChannelDecl channel;

    // Values（同じ行の値は同時刻）
    std::vector<std::pair<std::string, double>> values;

    // Pong
    int seq = 0;

    // Log / Err の本文。Unknown のときは元の行
    std::string text;
};

// バイト列を行に切る。
class HwLineAssembler
{
public:
    // data を食わせ、完成した行（\r・\n を含まない・空行を除く）を lines の末尾へ追加する。
    void Feed(const void* data, size_t len, std::vector<std::string>& lines);
    // 128 バイト超で捨てた行の数
    uint64_t OverflowCount() const { return m_overflow; }
    void Reset() { m_buf.clear(); m_discarding = false; }

private:
    std::string m_buf;
    bool        m_discarding = false;
    uint64_t    m_overflow   = 0;
};

HwMessage ParseDeviceLine(std::string_view line);

// 数値 1 個を厳密にパース（全文字を消費・有限値のみ）。
bool HwParseNumber(std::string_view s, double& out);

// float を短く整形する。整数値なら整数、それ以外は "%.4g"。
std::string HwFormatValue(double v);

std::string HwFormatHello();                                   // "?hello"
std::string HwFormatPing(int seq);                             // "?ping 17"
std::string HwFormatSet(const std::vector<std::pair<std::string, double>>& kv);   // "!a=1 b=0.4"
// 128 バイトを超えないよう複数行に分ける（普通は 1 行）。
std::vector<std::string> HwFormatSetLines(const std::vector<std::pair<std::string, double>>& kv);
std::string HwFormatSafe();                                    // "!safe"
std::string HwFormatPin(int pin, std::string_view mode, std::string_view name);   // "?pin 34 adc dial"

const char* HwChTypeName(HwChType t);

} // namespace dx12e::hw
