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
#include <cstddef>
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

// ---- 距離減衰 ----------------------------------------------------------------
// X3DAudio に渡している曲線と同じもの（minDistance までフル音量、maxDistance で 0 の直線）。
// 仮想化の判定と audio_state の「どれだけ聞こえているか」に使う（実際のパンニングは X3DAudio）。
inline float DistanceGain(float dist, float minDist, float maxDist)
{
    if (!(maxDist > 0.01f)) return 1.0f;
    const float mn = std::clamp(minDist, 0.0f, maxDist * 0.99f);
    if (dist <= mn) return 1.0f;
    if (dist >= maxDist) return 0.0f;
    return 1.0f - (dist - mn) / (maxDist - mn);
}

// ---- 仮想化 ------------------------------------------------------------------
// 「聞こえていない音」は XAudio2 のボイスを手放し、再生位置だけ進める（仮想ボイス）。
// 聞こえる距離へ戻ったら、進めた位置から鳴らし直す（ループ音が頭から鳴り直さない）。
// ★入る閾値と出る閾値をずらす（ヒステリシス）。同じ値だと境界の上で毎フレーム
//   ボイスを作っては壊す（プチプチ鳴る・CPU を食う）。
inline constexpr float kVirtualOnGain  = 0.001f;   // これ未満で仮想へ（-60 dB）
inline constexpr float kVirtualOffGain = 0.002f;   // これを超えたら実ボイスへ戻す（-54 dB）

inline bool NextVirtualState(bool isVirtual, float audibility)
{
    return isVirtual ? !(audibility > kVirtualOffGain) : (audibility < kVirtualOnGain);
}

// ---- ボイスを奪う相手の選び方 --------------------------------------------------
// 上限に達したとき「一番どうでもいい 1 本」を選ぶ。順序は
//   1) 優先度が低い  2) 同じ優先度なら小さく聞こえている  3) それも同じなら古い
// その 1 本が新しい音より大事（優先度が高い / 同じ優先度で大きく聞こえている）なら奪わない。
// margin は「同じ優先度で奪うには何倍大きく聞こえていないといけないか」。
// 仮想 → 実へ戻すときは 2 にして、似た音量同士が毎フレーム奪い合う（ちらつく）のを防ぐ。
struct VoiceCandidate
{
    int           slot       = -1;     // 呼び出し側の添字（そのまま返す）
    int           priority   = 128;
    float         audibility = 1.0f;   // 0..（最終的な聞こえ具合。線形）
    std::uint64_t order      = 0;      // 鳴らし始めた順（小さいほど古い）
};

// 戻り値は c の添字（奪ってよい相手）。奪えないなら -1。
inline int PickVictim(const VoiceCandidate* c, std::size_t n,
                      int newPriority, float newAudibility, float margin = 1.0f)
{
    int weakest = -1;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (weakest < 0) { weakest = static_cast<int>(i); continue; }
        const VoiceCandidate& a = c[i];
        const VoiceCandidate& w = c[weakest];
        if (a.priority != w.priority)       { if (a.priority < w.priority) weakest = static_cast<int>(i); continue; }
        if (a.audibility != w.audibility)   { if (a.audibility < w.audibility) weakest = static_cast<int>(i); continue; }
        if (a.order < w.order) weakest = static_cast<int>(i);
    }
    if (weakest < 0) return -1;
    const VoiceCandidate& w = c[weakest];
    if (w.priority < newPriority) return weakest;
    if (w.priority > newPriority) return -1;
    return (w.audibility * margin <= newAudibility) ? weakest : -1;
}

} // namespace dx12e::audio
