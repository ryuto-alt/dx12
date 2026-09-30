#pragma once
// 旧 dx12_sequence_author の台本 JSON(tools/mcp-server/sequence.ts の SequenceSpec / 16 種のトラック)→ Sequence(.dxseq)。
// 写像の設計は docs/SEQUENCER_DESIGN.md §7.2、ロスレスでない点は docs/DXSEQ_FORMAT.md §旧台本の変換 を参照。
// 純関数(エンジンを読まない)。旧式が「再生開始時の現在値」を使う箇所は ConvertOptions で与えられた値を焼く。与えられなければ
// 開始キーを打たず(終点にホールド)、ConvertNote(Lossy)に残す。
// 変換は決定論的: 同じ台本 + 同じオプションなら ID も含めて同じ結果になる(ID の seed = 台本名の FNV-1a)。

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

struct ConvertOptions
{
    int fps = 30;
    // 旧式が開始時に「捕まえる」現在値。キー = エンティティ名(shader は "target.param"、post はキー名)。
    std::map<std::string, std::array<double, 3>> initialPosition;   // camera / move の from 省略時
    std::map<std::string, std::array<double, 3>> initialRotation;   // rotate
    std::map<std::string, double> initialShader;                    // shaderParam の from 省略時("target.param")
    std::map<std::string, double> initialPost;                      // post の from 省略時(exposure を含む)
    double initialExposure = 1.0;                                   // fade の from(initialPost["exposure"] が無いとき)
    double initialTimeScale = 1.0;
};

struct ConvertNote
{
    enum class Level : std::uint8_t
    {
        Info,      // 補足(仮定・後続段階の責務)
        Lossy,     // 旧式と挙動/見た目が同一にならない(近似・開始値不明など)
        Warning,   // 台本側の問題(重なり・対応する stop が無い等)
        Error,     // そのトラックは変換せず捨てた
    };
    Level level = Level::Info;
    int trackIndex = -1;       // 台本(t 昇順に並べる前の tracks[])の添字。-1 = 全体
    std::string message;       // 日本語
};

struct ConvertResult
{
    bool ok = false;           // false = 台本を読めない(JSON 不正・name/tracks 無し)
    std::string error;
    std::vector<ConvertNote> notes;
    int Count(ConvertNote::Level l) const;
    bool Lossless() const { return Count(ConvertNote::Level::Lossy) == 0 && Count(ConvertNote::Level::Error) == 0; }
};

// 旧台本(JSON テキスト)→ Sequence。out は ok のときだけ有効。
ConvertResult ConvertLegacySpec(std::string_view specJson, const ConvertOptions& opt, Sequence& out);

} // namespace dx12e::seq
