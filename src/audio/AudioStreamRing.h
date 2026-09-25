#pragma once
// ===========================================================================
// ストリーミング再生の純粋ロジック（XAudio2 にも stb_vorbis にも依存しない）。
// ---------------------------------------------------------------------------
// リングは「チャンク（固定長の PCM バッファ）」を N 個持ち、3 本の単調増加カウンタで回す:
//
//   decoded   … ワーカーが書き終えたチャンク数（ワーカーだけが増やす）
//   submitted … XAudio2 へ渡したチャンク数（ポンプだけが増やす）
//   released  … XAudio2 が鳴らし終えたチャンク数（= submitted - BuffersQueued）
//
//   ワーカーが次に書いてよいのは decoded - released < N のとき（鳴らし終わったチャンクしか
//   上書きしない＝XAudio2 が読んでいるメモリを書き換えない）。
//   ポンプが渡してよいのは submitted < decoded のとき。
//   チャンク番号 = カウンタ % N。
//
// FillChunk はデコーダから 1 チャンクぶんを詰める。ループ終点に来たらループ始点へ
// シークしてそのまま同じチャンクを埋め続ける（ループ点でチャンクを短く切らない＝
// XAudio2 のバッファ境界でプチッと鳴らない）。tests/audio_stream_test.cpp が偽デコーダで検査する。
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dx12e::audio
{

inline std::uint32_t RingWritable(std::uint64_t decoded, std::uint64_t released, std::uint32_t cap)
{
    const std::uint64_t used = decoded - released;
    return (used >= cap) ? 0u : static_cast<std::uint32_t>(cap - used);
}

inline std::uint32_t RingSubmittable(std::uint64_t decoded, std::uint64_t submitted)
{
    return (decoded > submitted) ? static_cast<std::uint32_t>(decoded - submitted) : 0u;
}

// XAudio2 の BuffersQueued から「鳴らし終えた数」を出す。キューに残っている方が
// 多いことは無いはずだが、壊れた値で released を戻さない（単調性を守る）。
inline std::uint64_t RingReleased(std::uint64_t submitted, std::uint32_t buffersQueued,
                                  std::uint64_t prevReleased)
{
    const std::uint64_t r = (submitted >= buffersQueued) ? submitted - buffersQueued : 0u;
    return (r > prevReleased) ? r : prevReleased;
}

inline std::uint32_t RingSlot(std::uint64_t seq, std::uint32_t cap)
{
    return static_cast<std::uint32_t>(seq % cap);
}

// ループの指定。end = 0 は「曲の終わりまで」。start >= end（壊れた指定）は 0..end として扱う。
struct StreamLoop
{
    bool          loop  = false;
    std::uint64_t start = 0;
    std::uint64_t end   = 0;
};

inline std::uint64_t LoopEndOf(const StreamLoop& lp, std::uint64_t total)
{
    if (lp.end == 0 || (total > 0 && lp.end > total)) return total;
    return lp.end;
}

inline std::uint64_t LoopStartOf(const StreamLoop& lp, std::uint64_t total)
{
    const std::uint64_t e = LoopEndOf(lp, total);
    return (lp.start < e) ? lp.start : 0u;
}

// 再生位置をループ範囲へ畳む（位置の表示・仮想ボイスの位置送り用）。
inline std::uint64_t WrapLoopPosition(std::uint64_t pos, const StreamLoop& lp, std::uint64_t total)
{
    if (total == 0) return 0;
    if (!lp.loop) return (pos < total) ? pos : total;
    const std::uint64_t e = LoopEndOf(lp, total);
    const std::uint64_t s = LoopStartOf(lp, total);
    if (pos < e || e <= s) return (e <= s) ? pos % total : pos;
    return s + (pos - e) % (e - s);
}

struct FillResult
{
    std::uint32_t frames      = 0;      // 詰めたフレーム数
    bool          endOfStream = false;  // これが最後のチャンク（ループしない曲の終わり）
    std::uint32_t wraps       = 0;      // このチャンクの中でループ始点へ戻った回数
};

// Dec は次の 2 つを持つこと:
//   std::uint32_t Read(std::int16_t* dst, std::uint32_t frames)  … 読めたフレーム数（終端で 0）
//   bool          Seek(std::uint64_t frame)
// pos はデコーダの現在位置（呼び出しの間で持ち越す）。total = 0 は長さ不明（デコーダの終端まで）。
template <class Dec>
FillResult FillChunk(Dec& dec, std::uint64_t& pos, std::uint64_t total, const StreamLoop& lp,
                     std::int16_t* out, std::uint32_t chunkFrames, std::uint32_t channels)
{
    FillResult r;
    const std::uint64_t kNoBound = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t boundary = (total > 0) ? (lp.loop ? LoopEndOf(lp, total) : total) : kNoBound;
    int emptyWraps = 0;
    while (r.frames < chunkFrames)
    {
        std::uint32_t want = chunkFrames - r.frames;
        if (boundary != kNoBound)
        {
            if (pos >= boundary) want = 0;
            else if (boundary - pos < want) want = static_cast<std::uint32_t>(boundary - pos);
        }
        const std::uint32_t got = want ? dec.Read(out + static_cast<std::size_t>(r.frames) * channels, want) : 0u;
        r.frames += got;
        pos      += got;
        const bool atEnd = (want == 0) || (got < want) || (boundary != kNoBound && pos >= boundary);
        if (!atEnd) continue;
        if (!lp.loop) { r.endOfStream = true; break; }
        // ★ループ範囲が空 / デコーダが何も返さない、で無限ループしない
        if (got == 0 && ++emptyWraps > 2) { r.endOfStream = true; break; }
        if (got > 0) emptyWraps = 0;
        const std::uint64_t ls = (total > 0) ? LoopStartOf(lp, total) : lp.start;
        if (!dec.Seek(ls)) { r.endOfStream = true; break; }
        pos = ls;
        ++r.wraps;
    }
    return r;
}

} // namespace dx12e::audio
