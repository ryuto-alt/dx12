// ストリーミング再生の純粋ロジック（src/audio/AudioStreamRing.h）のテスト。
//
// 偽のデコーダ（フレーム i の値が i になる）でチャンクを詰め、
//   - ループしない曲: 最後のチャンクで endOfStream、途中で値が飛ばない
//   - ループ（全体 / ループ点つき）: 終点の次のフレームが始点になる（チャンクの途中でも）
//   - リングのカウンタ: 鳴らし終わったチャンクしか上書きしない
// を確かめる。XAudio2 も stb_vorbis も要らない。
//
// 実行: ctest --output-on-failure -R AudioStream

#include "audio/AudioStreamRing.h"

#include <cstdio>
#include <vector>

using namespace dx12e::audio;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const char* label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label);
}

// フレーム i の全チャンネルが (i % 30000) になるデコーダ。
struct FakeDecoder
{
    std::uint64_t total = 0;
    std::uint64_t pos   = 0;
    std::uint32_t ch    = 2;
    int           seeks = 0;
    std::uint32_t Read(std::int16_t* dst, std::uint32_t frames)
    {
        std::uint32_t n = 0;
        while (n < frames && pos < total)
        {
            for (std::uint32_t c = 0; c < ch; ++c) dst[n * ch + c] = static_cast<std::int16_t>(pos % 30000);
            ++n;
            ++pos;
        }
        return n;
    }
    bool Seek(std::uint64_t f)
    {
        if (f > total) return false;
        pos = f;
        ++seeks;
        return true;
    }
};

// n チャンクぶん詰めて、フレーム値の列（左チャンネル）と各チャンクの結果を返す。
std::vector<int> Pull(FakeDecoder& d, std::uint64_t total, const StreamLoop& lp,
                      std::uint32_t chunk, int chunks, std::vector<FillResult>* results = nullptr)
{
    std::vector<int> seq;
    std::vector<std::int16_t> buf(static_cast<std::size_t>(chunk) * d.ch);
    std::uint64_t pos = d.pos;
    for (int i = 0; i < chunks; ++i)
    {
        const FillResult r = FillChunk(d, pos, total, lp, buf.data(), chunk, d.ch);
        if (results) results->push_back(r);
        for (std::uint32_t f = 0; f < r.frames; ++f) seq.push_back(buf[f * d.ch]);
        if (r.endOfStream) break;
    }
    return seq;
}

void TestNoLoop()
{
    std::printf("[ループしない]\n");
    FakeDecoder d;
    d.total = 100;
    std::vector<FillResult> rs;
    const auto seq = Pull(d, 100, StreamLoop{}, 64, 5, &rs);
    Check(rs.size() == 2, "100 フレームを 64 ずつ詰めると 2 チャンク");
    Check(rs[0].frames == 64 && !rs[0].endOfStream, "1 チャンク目は満杯で終端ではない");
    Check(rs[1].frames == 36 && rs[1].endOfStream, "2 チャンク目は残り 36 で終端");
    bool cont = seq.size() == 100;
    for (std::size_t i = 0; cont && i < seq.size(); ++i) cont = (seq[i] == static_cast<int>(i));
    Check(cont, "値が 0..99 で途切れず並ぶ");

    FakeDecoder e;
    e.total = 128;
    std::vector<FillResult> rs2;
    Pull(e, 128, StreamLoop{}, 64, 5, &rs2);
    Check(rs2.size() == 2 && rs2[1].frames == 64 && rs2[1].endOfStream,
          "ちょうど割り切れる長さでも最後のチャンクに終端が立つ（空チャンクを作らない）");
}

void TestLoopWhole()
{
    std::printf("[曲全体のループ]\n");
    FakeDecoder d;
    d.total = 100;
    StreamLoop lp;
    lp.loop = true;
    std::vector<FillResult> rs;
    const auto seq = Pull(d, 100, lp, 64, 4, &rs);
    Check(seq.size() == 256, "4 チャンクぶん途切れず出る");
    bool ok = true;
    for (std::size_t i = 0; ok && i < seq.size(); ++i) ok = (seq[i] == static_cast<int>(i % 100));
    Check(ok, "99 の次は 0（チャンクの途中でもループ点をまたいで詰める）");
    Check(rs[1].wraps == 1, "2 チャンク目の途中で 1 回戻る");
    bool noEos = true;
    for (const auto& r : rs) noEos = noEos && !r.endOfStream;
    Check(noEos, "ループ中は終端にならない");
}

void TestLoopPoints()
{
    std::printf("[ループ点（イントロ付きの BGM）]\n");
    FakeDecoder d;
    d.total = 100;
    StreamLoop lp;
    lp.loop  = true;
    lp.start = 20;
    lp.end   = 80;
    const auto seq = Pull(d, 100, lp, 50, 6);
    // 0..79 → 20..79 → 20..79 ...
    bool ok = seq.size() == 300;
    for (std::size_t i = 0; ok && i < seq.size(); ++i)
    {
        const int expect = (i < 80) ? static_cast<int>(i) : 20 + static_cast<int>((i - 80) % 60);
        ok = (seq[i] == expect);
    }
    Check(ok, "イントロ 0..79 の後は 20..79 を繰り返す（80 以降は一度も出ない）");

    // 壊れた指定（start >= end）は 0..end をループ
    FakeDecoder e;
    e.total = 100;
    StreamLoop bad;
    bad.loop  = true;
    bad.start = 90;
    bad.end   = 50;
    const auto s2 = Pull(e, 100, bad, 40, 3);
    bool ok2 = s2.size() == 120;
    for (std::size_t i = 0; ok2 && i < s2.size(); ++i) ok2 = (s2[i] == static_cast<int>(i % 50));
    Check(ok2, "start >= end は 0..end のループとして扱う");

    // end が曲より長い指定は曲の終わりで折り返す
    FakeDecoder f;
    f.total = 100;
    StreamLoop longEnd;
    longEnd.loop = true;
    longEnd.end  = 5000;
    const auto s3 = Pull(f, 100, longEnd, 64, 3);
    Check(s3.size() == 192 && s3[100] == 0, "end が曲より長ければ曲の終わりで折り返す");
}

void TestLoopEmpty()
{
    std::printf("[壊れたデコーダでも止まる]\n");
    FakeDecoder d;
    d.total = 0;   // 何も返さない
    StreamLoop lp;
    lp.loop = true;
    std::uint64_t pos = 0;
    std::vector<std::int16_t> buf(64 * 2);
    const FillResult r = FillChunk(d, pos, 0, lp, buf.data(), 64, 2);
    Check(r.endOfStream && r.frames == 0, "何も読めないループは無限ループせず終端扱い");
}

void TestWrapPosition()
{
    std::printf("[再生位置の畳み込み]\n");
    StreamLoop lp;
    lp.loop = true; lp.start = 20; lp.end = 80;
    Check(WrapLoopPosition(50, lp, 100) == 50, "ループ範囲内はそのまま");
    Check(WrapLoopPosition(80, lp, 100) == 20, "終点ちょうどは始点");
    Check(WrapLoopPosition(145, lp, 100) == 25, "2 周目以降も始点から数える");
    StreamLoop none;
    Check(WrapLoopPosition(150, none, 100) == 100, "ループしない曲は終わりで止まる");
}

void TestRing()
{
    std::printf("[リングのカウンタ]\n");
    Check(RingWritable(0, 0, 8) == 8, "空なら全部書ける");
    Check(RingWritable(8, 0, 8) == 0, "満杯なら書けない（鳴らし終わっていないチャンクは上書きしない）");
    Check(RingWritable(10, 5, 8) == 3, "鳴らし終えた分だけ空く");
    Check(RingSubmittable(10, 7) == 3 && RingSubmittable(7, 7) == 0, "渡していない分だけ渡せる");
    Check(RingReleased(10, 3, 0) == 7, "released = submitted - BuffersQueued");
    Check(RingReleased(10, 12, 5) == 5, "おかしな値でも released は戻らない（単調）");
    Check(RingSlot(9, 8) == 1, "スロットは seq % N");

    // 生産者と消費者を交互に回して、書き込みが鳴っているチャンクを追い越さないこと
    const std::uint32_t cap = 4;
    std::uint64_t dec = 0, sub = 0, rel = 0;
    bool safe = true;
    for (int step = 0; step < 1000; ++step)
    {
        if (step % 3 != 2 && RingWritable(dec, rel, cap) > 0) ++dec;              // ワーカー
        while (RingSubmittable(dec, sub) > 0) ++sub;                               // ポンプ
        const std::uint32_t queued = static_cast<std::uint32_t>(sub - rel);
        if (step % 2 == 0 && queued > 0) rel = RingReleased(sub, queued - 1, rel); // 1 つ鳴り終わる
        if (dec - rel > cap || sub > dec || rel > sub) safe = false;
    }
    Check(safe, "1000 手回しても decoded-released <= N、released <= submitted <= decoded");
}
} // namespace

int main()
{
    TestNoLoop();
    TestLoopWhole();
    TestLoopPoints();
    TestLoopEmpty();
    TestWrapPosition();
    TestRing();
    std::printf("\naudio_stream: %d チェック / %d 件 NG\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
