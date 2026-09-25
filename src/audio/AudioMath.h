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

// ---- 時間の平滑 ----------------------------------------------------------------
// 1 次遅れ（時定数 tau 秒）で target へ寄せる係数。dt が大きくても 1 を超えない（行き過ぎない）。
// tau <= 0 は即座に追従。
inline float SmoothFactor(float dt, float tau)
{
    if (!(tau > 0.0f)) return 1.0f;
    if (!(dt > 0.0f)) return 0.0f;
    return 1.0f - std::exp(-dt / tau);
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---- リバーブ ------------------------------------------------------------------
// XAudio2 の I3DL2 パラメータと同じ並び（室内音響の標準。mB = 1/100 dB）。
// 補間はこの空間で行う（mB は dB の線形なので耳の感覚に近い）。ネイティブ値への変換は
// AudioSystem 側の ReverbConvertI3DL2ToNative（xaudio2fx.h）に任せる。
struct ReverbParams
{
    float room             = -10000.0f;  // mB
    float roomHF           = 0.0f;       // mB
    float roomRolloff      = 0.0f;
    float decayTime        = 1.0f;       // s
    float decayHFRatio     = 0.5f;
    float reflections      = -10000.0f;  // mB
    float reflectionsDelay = 0.020f;     // s
    float reverb           = -10000.0f;  // mB
    float reverbDelay      = 0.040f;     // s
    float diffusion        = 100.0f;     // %
    float density          = 100.0f;     // %
    float hfReference      = 5000.0f;    // Hz
};

inline ReverbParams LerpReverb(const ReverbParams& a, const ReverbParams& b, float t)
{
    ReverbParams r;
    r.room             = Lerp(a.room, b.room, t);
    r.roomHF           = Lerp(a.roomHF, b.roomHF, t);
    r.roomRolloff      = Lerp(a.roomRolloff, b.roomRolloff, t);
    r.decayTime        = Lerp(a.decayTime, b.decayTime, t);
    r.decayHFRatio     = Lerp(a.decayHFRatio, b.decayHFRatio, t);
    r.reflections      = Lerp(a.reflections, b.reflections, t);
    r.reflectionsDelay = Lerp(a.reflectionsDelay, b.reflectionsDelay, t);
    r.reverb           = Lerp(a.reverb, b.reverb, t);
    r.reverbDelay      = Lerp(a.reverbDelay, b.reverbDelay, t);
    r.diffusion        = Lerp(a.diffusion, b.diffusion, t);
    r.density          = Lerp(a.density, b.density, t);
    r.hfReference      = Lerp(a.hfReference, b.hfReference, t);
    return r;
}

// 2 つの設定がどれだけ違うか（毎フレーム SetEffectParameters を呼ばないための判定）。
inline float ReverbDistance(const ReverbParams& a, const ReverbParams& b)
{
    float d = 0.0f;
    d = (std::max)(d, std::fabs(a.room - b.room) / 100.0f);             // dB
    d = (std::max)(d, std::fabs(a.roomHF - b.roomHF) / 100.0f);
    d = (std::max)(d, std::fabs(a.reflections - b.reflections) / 100.0f);
    d = (std::max)(d, std::fabs(a.reverb - b.reverb) / 100.0f);
    d = (std::max)(d, std::fabs(a.decayTime - b.decayTime) * 10.0f);    // 0.1 s = 1
    d = (std::max)(d, std::fabs(a.decayHFRatio - b.decayHFRatio) * 10.0f);
    d = (std::max)(d, std::fabs(a.reflectionsDelay - b.reflectionsDelay) * 200.0f);
    d = (std::max)(d, std::fabs(a.reverbDelay - b.reverbDelay) * 200.0f);
    d = (std::max)(d, std::fabs(a.diffusion - b.diffusion) / 10.0f);
    d = (std::max)(d, std::fabs(a.density - b.density) / 10.0f);
    return d;
}

// プリセット。値は xaudio2fx.h の XAUDIO2FX_I3DL2_PRESET_* をそのまま写したもの
// （WetDryMix は送りバス側で 100% 固定なので持たない）。名前はシーン JSON / Lua で使う。
struct ReverbPreset
{
    const char*  name;
    const char*  label;   // エディタ表示用
    ReverbParams p;
};

inline const ReverbPreset* ReverbPresetTable(std::size_t& count)
{
    //                                         room   roomHF rolloff decay  hfRat  refl   reflDl  rev    revDl  diff   dens   hfRef
    static const ReverbPreset kTable[] = {
        {"none",          "なし",              {-10000,     0, 0.0f, 1.00f, 0.50f,-10000, 0.020f,-10000, 0.040f,100.0f,100.0f,5000.0f}},
        {"generic",       "汎用",              { -1000,  -100, 0.0f, 1.49f, 0.83f, -2602, 0.007f,   200, 0.011f,100.0f,100.0f,5000.0f}},
        {"closet",        "狭い所（ロッカー）", { -1000, -6000, 0.0f, 0.17f, 0.10f, -1204, 0.001f,   207, 0.002f,100.0f,100.0f,5000.0f}},
        {"room",          "部屋",              { -1000,  -454, 0.0f, 0.40f, 0.83f, -1646, 0.002f,    53, 0.003f,100.0f,100.0f,5000.0f}},
        {"smallroom",     "小部屋",            { -1000,  -600, 0.0f, 1.10f, 0.83f,  -400, 0.005f,   500, 0.010f,100.0f,100.0f,5000.0f}},
        {"largeroom",     "大部屋",            { -1000,  -600, 0.0f, 1.50f, 0.83f, -1600, 0.020f, -1000, 0.040f,100.0f,100.0f,5000.0f}},
        {"bathroom",      "浴室・タイル",      { -1000, -1200, 0.0f, 1.49f, 0.54f,  -370, 0.007f,  1030, 0.011f,100.0f, 60.0f,5000.0f}},
        {"stoneroom",     "石の部屋",          { -1000,  -300, 0.0f, 2.31f, 0.64f,  -711, 0.012f,    83, 0.017f,100.0f,100.0f,5000.0f}},
        {"hallway",       "廊下",              { -1000,  -300, 0.0f, 1.49f, 0.59f, -1219, 0.007f,   441, 0.011f,100.0f,100.0f,5000.0f}},
        {"stonecorridor", "石の廊下",          { -1000,  -237, 0.0f, 2.70f, 0.79f, -1214, 0.013f,   395, 0.020f,100.0f,100.0f,5000.0f}},
        {"hall",          "ホール",            { -1000,  -500, 0.0f, 3.92f, 0.70f, -1230, 0.020f,    -2, 0.029f,100.0f,100.0f,5000.0f}},
        {"cave",          "洞窟",              { -1000,     0, 0.0f, 2.91f, 1.30f,  -602, 0.015f,  -302, 0.022f,100.0f,100.0f,5000.0f}},
        {"sewer",         "下水道・管",        { -1000, -1000, 0.0f, 2.81f, 0.14f,   429, 0.014f,   648, 0.021f, 80.0f, 60.0f,5000.0f}},
        {"hangar",        "格納庫・倉庫",      { -1000, -1000, 0.0f,10.05f, 0.23f,  -602, 0.020f,   198, 0.030f,100.0f,100.0f,5000.0f}},
        {"forest",        "森",                { -1000, -3300, 0.0f, 1.49f, 0.54f, -2560, 0.162f,  -613, 0.088f, 79.0f,100.0f,5000.0f}},
        {"city",          "街",                { -1000,  -800, 0.0f, 1.49f, 0.67f, -2273, 0.007f, -2217, 0.011f, 50.0f,100.0f,5000.0f}},
        {"outdoor",       "屋外（平地）",      { -1000, -2000, 0.0f, 1.49f, 0.50f, -2466, 0.179f, -2514, 0.100f, 21.0f,100.0f,5000.0f}},
        {"underwater",    "水中",              { -1000, -4000, 0.0f, 1.49f, 0.10f,  -449, 0.007f,  1700, 0.011f,100.0f,100.0f,5000.0f}},
    };
    count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

// 名前からプリセットを引く（大文字小文字は区別しない）。無ければ false で out は触らない。
inline bool FindReverbPreset(const char* name, ReverbParams& out)
{
    if (!name) return false;
    std::size_t n = 0;
    const ReverbPreset* t = ReverbPresetTable(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const char* a = t[i].name;
        const char* b = name;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { ++a; ++b; }
        if (*a == 0 && *b == 0) { out = t[i].p; return true; }
    }
    return false;
}

// ---- リバーブ域（ゾーン）の重み --------------------------------------------------
// 形の「外側への距離」（内側は 0）を出し、境界から fade の幅で 1 → 0 へ下げる。
// 座標はゾーンのローカル空間（エンティティのワールド行列の逆で写したもの。スケール込み）。
inline float BoxOutsideDistance(const float local[3], const float half[3])
{
    const float dx = (std::max)(std::fabs(local[0]) - half[0], 0.0f);
    const float dy = (std::max)(std::fabs(local[1]) - half[1], 0.0f);
    const float dz = (std::max)(std::fabs(local[2]) - half[2], 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline float SphereOutsideDistance(const float local[3], float radius)
{
    const float len = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
    return (std::max)(len - radius, 0.0f);
}

inline float ZoneWeight(float outsideDistance, float fade)
{
    if (outsideDistance <= 0.0f) return 1.0f;
    if (!(fade > 0.0f)) return 0.0f;
    return std::clamp(1.0f - outsideDistance / fade, 0.0f, 1.0f);
}

// 重なったゾーンを混ぜる。優先度の低い順に「今までの結果 → そのゾーン」へ重みで寄せる
// （入れ子の部屋: 洞窟(0) の中の小部屋(1) は、小部屋の中では小部屋が勝つ）。
// ★響きが無いところ（wet≈0）からゾーンへ入るときは、響きの性格（パラメータ）は即座に
//   ゾーンのものにして量（wet）だけを補間する。性格まで "none" から補間すると、
//   境界の中ほどでは room が -50dB 近くまで落ちていて、量と二重に絞られて
//   「フェード域に入った途端に響きがほぼ消える」になる。
struct ZoneSample
{
    int          priority = 0;
    float        weight   = 0.0f;   // 0..1
    float        wet      = 0.0f;   // 0..1
    ReverbParams params;
};

struct ReverbMix
{
    ReverbParams params;
    float        wet = 0.0f;
};

// zones は呼び出し側で優先度の昇順に並べておくこと（同じ優先度は渡した順）。
inline ReverbMix BlendZones(const ZoneSample* zones, std::size_t n,
                            const ReverbParams& baseParams, float baseWet)
{
    ReverbMix m;
    m.params = baseParams;
    m.wet    = baseWet;
    for (std::size_t i = 0; i < n; ++i)
    {
        const float w = std::clamp(zones[i].weight, 0.0f, 1.0f);
        if (w <= 0.0f) continue;
        const float tp = (m.wet <= 1e-4f) ? 1.0f : w;
        m.params = LerpReverb(m.params, zones[i].params, tp);
        m.wet    = Lerp(m.wet, zones[i].wet, w);
    }
    return m;
}

} // namespace dx12e::audio
