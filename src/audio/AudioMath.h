#pragma once
// ===========================================================================
// 音まわりの純粋ロジック（XAudio2 にも Windows にも依存しない）。
// ---------------------------------------------------------------------------
// AudioSystem が「何を鳴らすか / どれだけの大きさか」を決める計算はここへ寄せる。
// ここにある関数は tests/audio_math_test.cpp が単体で検査する（GPU も音声デバイスも要らない）。
// XAudio2 の呼び出しは AudioSystem.cpp 側だけに置くこと＝ここは数字しか扱わない。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dx12e::audio
{

// 無音の表現。-inf は JSON に載らない（nlohmann は null にする）ので、床を決めて数値で返す。
// ★「メーターが -120」は「何も鳴っていない」の意味。極小の実音と区別したいなら peak を見る。
inline constexpr float kSilenceDb = -120.0f;

inline float LinearToDb(float lin)
{
    if (!(lin > 0.000001f)) return kSilenceDb;   // NaN もここで床へ落とす
    return (std::max)(20.0f * std::log10(lin), kSilenceDb);
}

inline float DbToLinear(float db)
{
    if (db <= kSilenceDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

// ---- ローパス ----------------------------------------------------------------
// XAudio2 の状態変数フィルタは Frequency = 2*sin(pi*fc/fs)。上限 1.0（= fs/6）で、
// 「Frequency=1, OneOverQ=1」は素通しと等価（公式ドキュメントの Remarks）。
// ★fs/6 を超える遮断周波数は表せない（48kHz で 8kHz まで）。それ以上を指定されたら素通しにする。
// hz <= 0 も「ローパス無し」。
inline float CutoffToFilterFrequency(float hz, float sampleRate)
{
    if (!(hz > 0.0f) || !(sampleRate > 0.0f)) return 1.0f;
    const float f = 2.0f * std::sin(3.14159265f * (std::min)(hz, sampleRate * 0.5f) / sampleRate);
    return std::clamp(f, 0.0f, 1.0f);
}

// 2 つのローパス（0 = 無し）を直列に掛けたときの近似。低い方が支配的なので min を取る。
inline float CombineLowpass(float a, float b)
{
    if (!(a > 0.0f)) return (b > 0.0f) ? b : 0.0f;
    if (!(b > 0.0f)) return a;
    return (std::min)(a, b);
}

// ---- バスのスナップショット補正 --------------------------------------------------
// バスの最終値 = ユーザー設定（setBusVolume / setBusLowpass）× スナップショットの補正。
// 補正を別に持つので、オプション画面の音量スライダーとゲーム演出（「追跡中」で環境音を絞る）が
// 互いの値を上書きし合わない。
struct BusMod
{
    float gain      = 1.0f;   // 線形。1 = 変えない
    float lowpassHz = 0.0f;   // 0 = ローパスを足さない
};

inline bool operator==(const BusMod& a, const BusMod& b)
{
    return a.gain == b.gain && a.lowpassHz == b.lowpassHz;
}

} // namespace dx12e::audio
