#pragma once
// 評価: Evaluate(seq, t) → 状態(トラックごとのサンプル値)。純関数。エンジンへの書き込み(適用層)は S1b。
// イベントの発火規則・再生位置(ループ/ピンポン)・カメラカットの選択もここ。
// 意味論の正本: docs/DXSEQ_FORMAT.md

#include "sequencer/SeqCurve.h"
#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// 評価結果
// ---------------------------------------------------------------------------
// インデックスは Sequence の配列位置(binding = bindings[i]、track = そのバインディング内、channel = そのトラック内)。
// 並びは決定的: バインディング配列順 → トラック配列順 → チャンネル/クリップ配列順。
struct ChannelSample
{
    std::int32_t binding = 0, track = 0, channel = 0;
    double value = 0.0;      // 色(srgb 指定)はリニアで返る
};

struct QuatSample            // Transform(rotation=quat)の rotation.{x,y,z,w} を 1 個にまとめたもの
{
    std::int32_t binding = 0, track = 0;
    Quat q;
};

struct ClipSample            // t で有効なクリップ(start <= t < start+dur。dur==0 は t==start のみ)
{
    std::int32_t binding = 0, track = 0, clip = 0;
    Tick localTick = 0;      // t - start
    double weight = 1.0;     // params.blendIn / blendOut(ティック)による線形フェード。0..1
};

struct EvalResult
{
    Tick t = 0;
    std::vector<ChannelSample> channels;
    std::vector<QuatSample> quats;
    std::vector<ClipSample> clips;
    std::int32_t cutIndex = -1;     // 選ばれたカット(cuts[] の位置)。-1 = カット無し(=現在のアクティブカメラのまま)
    std::int32_t cutBinding = -1;   // そのカメラのバインディング位置(-1 = 無い/未知)
    void Clear()
    {
        t = 0; channels.clear(); quats.clear(); clips.clear(); cutIndex = -1; cutBinding = -1;
    }
};

// 未解決バインディングのトラックを外す等に使う(bindingEnabled[i] == 0 のバインディングは評価しない)。
struct EvalFilter
{
    std::vector<std::uint8_t> bindingEnabled;
};

// 既存の out を再利用する版(容量を保つのでアロケーションが増えない)。
void Evaluate(const Sequence& seq, Tick t, EvalResult& out, const EvalFilter* filter = nullptr);
EvalResult Evaluate(const Sequence& seq, Tick t, const EvalFilter* filter = nullptr);

// 2 つの結果がビット単位で一致するか(決定論の検査用。double は memcmp で比べる)
bool BitEqual(const EvalResult& a, const EvalResult& b);

// ---------------------------------------------------------------------------
// カメラカット
// ---------------------------------------------------------------------------
// t を含むカット(cuts[] の位置)。[start, end) で判定し、重なりは配列の後ろが勝つ。
// 例外: t が「範囲末尾ちょうど」で、その時刻を終端に持つカットがあるなら(他に含むカットが無ければ)最後のそのカット。
// 無ければ -1。
int SelectCut(const Sequence& seq, Tick t);

// ---------------------------------------------------------------------------
// マーカー
// ---------------------------------------------------------------------------
// t より後で最初/前で最後のマーカー(同時刻は配列順)。無ければ -1。
int NextMarker(const Sequence& seq, Tick t);
int PrevMarker(const Sequence& seq, Tick t);

// ---------------------------------------------------------------------------
// イベントの発火規則
// ---------------------------------------------------------------------------
enum class LoopMode : std::uint8_t { Once = 0, Loop, PingPong };

struct EventFire
{
    std::int32_t binding = 0, track = 0, event = 0;   // seq.bindings[binding].tracks[track].events[event]
    Tick time = 0;                                    // 発火した瞬間(巻き戻さない展開時刻)
};

struct EventCollectOptions
{
    Tick rangeStart = 0;             // Loop / PingPong の範囲 [start, end]
    Tick rangeEnd = 0;
    LoopMode mode = LoopMode::Once;
    bool startInclusive = false;     // 再生開始の最初の 1 歩だけ、始点を含める(t=0 のイベントを落とさない)
    bool fireBackward = true;        // 逆再生でも発火するか
};

// 展開時刻(ラップ前の連続した時計)u0 → u1 の移動で通過したイベントを out へ足す。
//   前進 (u1 > u0):  (u0, u1]       … 左開右閉。startInclusive のときだけ [u0, u1]
//   後退 (u1 < u0):  [u1, u0)       … 前進の鏡像。startInclusive のときだけ [u1, u0]
//   u0 == u1:        何も発火しない
// Loop は範囲 [start, end) の中のイベントだけが対象(end ちょうどは次周の start と同じ瞬間なので鳴らさない)、
// PingPong は [start, end] の全イベントが対象(折り返しの端は 1 回だけ)。どちらも周回/往復ごとに 1 回ずつ発火する。
// 出力の並びは移動方向に沿った時刻順(同時刻は バインディング → トラック → イベント の配列順、後退は逆順)。
// ★スクラブ(Seek)では呼ばないこと = 発火しない。SeqPlayback::Seek がその規則を実装している。
// 1 回で 64 周を超える移動は古い側を切り捨てる(暴走防止)。
void CollectEvents(const Sequence& seq, Tick u0, Tick u1, const EventCollectOptions& opt, std::vector<EventFire>& out);

// ---------------------------------------------------------------------------
// 再生位置(時計)
// ---------------------------------------------------------------------------
// 展開時刻(unwrapped)を持ち、Position() でラップ後の位置を返す。Advance が発火イベントを集める。
class SeqPlayback
{
public:
    // 再生範囲と周回モードを設定する(位置は範囲始点へ)。
    void Reset(const Sequence& seq, LoopMode mode);
    void SetRate(double rate) { m_rate = rate; }
    double Rate() const { return m_rate; }
    void SetFireBackward(bool b) { m_fireBackward = b; }

    Tick Position() const;            // ラップ後の位置(Once は範囲に clamp)
    Tick Unwrapped() const { return m_unwrapped; }
    bool IsPlaying() const { return m_playing; }
    bool IsFinished() const { return m_finished; }
    LoopMode Mode() const { return m_mode; }
    Tick RangeStart() const { return m_start; }
    Tick RangeEnd() const { return m_end; }

    // スクラブ/シーク: 位置を置く。★イベントは一切発火しない。
    void Seek(Tick pos);
    // 再生開始。位置が再生方向の起点(前進なら範囲始点、後退なら終点)にあるときだけ、最初の 1 歩で起点のイベントを含める。
    void Play();
    void Pause() { m_playing = false; }

    // dtSeconds(実時間)× rate だけ進める。発火したイベントを out に足す。Once は端で止まる。
    void Advance(const Sequence& seq, double dtSeconds, std::vector<EventFire>& out);

private:
    Tick m_start = 0, m_end = 0;
    LoopMode m_mode = LoopMode::Once;
    double m_rate = 1.0;
    bool m_fireBackward = true;
    Tick m_unwrapped = 0;
    double m_frac = 0.0;
    bool m_playing = false;
    bool m_armStart = false;
    bool m_finished = false;
};

} // namespace dx12e::seq
