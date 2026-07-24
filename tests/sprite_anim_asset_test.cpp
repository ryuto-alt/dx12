// スプライトシートアセット(animation/SpriteAnimAsset.h)の評価テスト。
// 任意セル順・可変フレーム長・名前付きシーケンスの畳み込みを検証する。
// 純関数(GPU/ECS/JSON非依存)なのでヘッダだけで動く = 依存ゼロ CI で回る。
//
// 実行: ctest --output-on-failure （失敗があれば終了コード 1）

#include "animation/SpriteAnimAsset.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b, float tol = 1e-5f)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
}

void check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

void checki(int got, int want, const char* what)
{
    ++g_checks;
    if (got != want)
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
    }
}

void checkUv(const SpriteUvRect& r, float u0, float v0, float u1, float v1, const char* what)
{
    ++g_checks;
    if (!feq(r.u0, u0) || !feq(r.v0, v0) || !feq(r.u1, u1) || !feq(r.v1, v1))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.4f,%.4f,%.4f,%.4f want %.4f,%.4f,%.4f,%.4f)\n",
                    what, r.u0, r.v0, r.u1, r.v1, u0, v0, u1, v1);
    }
}

// --- セル → UV --------------------------------------------------------------
void TestCellUv()
{
    std::printf("TestCellUv\n");

    // 4x2 グリッド: セル幅 0.25 / 高さ 0.5
    checkUv(SpriteSheetCellUv(4, 2, 0), 0.00f, 0.0f, 0.25f, 0.5f, "セル0 は左上");
    checkUv(SpriteSheetCellUv(4, 2, 3), 0.75f, 0.0f, 1.00f, 0.5f, "セル3 は1行目の右端");
    checkUv(SpriteSheetCellUv(4, 2, 4), 0.00f, 0.5f, 0.25f, 1.0f, "セル4 は2行目の先頭（行優先）");
    checkUv(SpriteSheetCellUv(4, 2, 7), 0.75f, 0.5f, 1.00f, 1.0f, "セル7 は右下");

    // 範囲外は端へクランプ（黒画面にせず、間違ったコマを出して原因を見せる）
    checkUv(SpriteSheetCellUv(4, 2, -3), 0.00f, 0.0f, 0.25f, 0.5f, "負のセルは 0 へ");
    checkUv(SpriteSheetCellUv(4, 2, 99), 0.75f, 0.5f, 1.00f, 1.0f, "超過セルは最終セルへ");

    // cols/rows が 0 以下でも 1 に丸めて 0 除算しない
    checkUv(SpriteSheetCellUv(0, 0, 0), 0.0f, 0.0f, 1.0f, 1.0f, "cols/rows=0 は 1x1 扱い");
}

// --- 等間隔シーケンス（既存 Sprite2D と同じ畳み方） -------------------------
void TestUniformSeq()
{
    std::printf("TestUniformSeq\n");

    SpriteAnimSeq seq;
    seq.frames = {5, 6, 7, 8};   // 任意のセル番号から始められる
    seq.fps    = 10.0f;
    seq.mode   = kFlipbookLoop;

    checki(SpriteSeqIndexAt(seq, 0.00f), 0, "t=0 は先頭");
    checki(SpriteSeqIndexAt(seq, 0.15f), 1, "0.1s ごとに 1 コマ進む");
    checki(SpriteSeqIndexAt(seq, 0.35f), 3, "4コマ目");
    checki(SpriteSeqIndexAt(seq, 0.45f), 0, "ループで先頭へ戻る");

    check(feq(SpriteSeqDuration(seq), 0.4f), "総再生時間 = 4/10s");

    // 返るのは frames 配列の index であって、セル番号ではない
    SpriteAnimSheet sheet;
    sheet.cols = 4; sheet.rows = 4;
    sheet.seqs.push_back(seq);
    // index=1 → セル6 → 4x4 の (2,1)
    checkUv(SpriteSeqUvAt(sheet, seq, 0.15f), 0.5f, 0.25f, 0.75f, 0.5f,
            "index=1 はセル6 の UV");

    // 単発モードは最終フレームで止まり finished が立つ
    SpriteAnimSeq once = seq;
    once.mode = kFlipbookOnce;
    bool fin = false;
    checki(SpriteSeqIndexAt(once, 0.35f, &fin), 3, "単発の最終フレーム");
    check(!fin, "最終フレーム表示中はまだ完了ではない");
    checki(SpriteSeqIndexAt(once, 1.00f, &fin), 3, "単発は最終フレームで停止");
    check(fin, "尺を過ぎたら完了フラグが立つ");

    // 往復
    SpriteAnimSeq ping = seq;
    ping.mode = kFlipbookPingPong;
    checki(SpriteSeqIndexAt(ping, 0.35f), 3, "往復の折り返し点");
    checki(SpriteSeqIndexAt(ping, 0.45f), 2, "往復は戻ってくる");

    // 空シーケンスは -1 / 全面 UV
    SpriteAnimSeq empty;
    checki(SpriteSeqIndexAt(empty, 1.0f), -1, "空シーケンスは -1");
    check(feq(SpriteSeqDuration(empty), 0.0f), "空シーケンスの尺は 0");
    checkUv(SpriteSeqUvAt(sheet, empty, 1.0f), 0.0f, 0.0f, 1.0f, 1.0f,
            "空シーケンスはテクスチャ全面（事故が見て分かる）");
}

// --- 可変フレーム長 ----------------------------------------------------------
void TestVariableHolds()
{
    std::printf("TestVariableHolds\n");

    SpriteAnimSeq seq;
    seq.frames = {0, 1, 2};
    seq.holds  = {3.0f, 1.0f, 1.0f};   // 先頭だけ 3 倍の溜め
    seq.fps    = 10.0f;
    seq.mode   = kFlipbookLoop;

    check(feq(SpriteSeqDuration(seq), 0.5f), "総尺 = (3+1+1)/10s");
    checki(SpriteSeqIndexAt(seq, 0.00f), 0, "溜めの先頭");
    checki(SpriteSeqIndexAt(seq, 0.25f), 0, "0.3s までは先頭を保持");
    checki(SpriteSeqIndexAt(seq, 0.35f), 1, "0.3s で 2 コマ目");
    checki(SpriteSeqIndexAt(seq, 0.45f), 2, "0.4s で 3 コマ目");
    checki(SpriteSeqIndexAt(seq, 0.55f), 0, "0.5s でループ");

    // holds の長さが frames と合わないものは丸ごと無視して等間隔に落ちる
    SpriteAnimSeq bad = seq;
    bad.holds = {3.0f};
    check(feq(SpriteSeqDuration(bad), 0.3f), "長さ不一致の holds は無視して等間隔");
    checki(SpriteSeqIndexAt(bad, 0.15f), 1, "無視された holds では等間隔で進む");

    // holds が全部 0 の病的データでも 0 除算しない
    SpriteAnimSeq zero = seq;
    zero.holds = {0.0f, 0.0f, 0.0f};
    check(feq(SpriteSeqDuration(zero), 0.3f), "holds が全 0 なら 1コマ相当で代替");
    checki(SpriteSeqIndexAt(zero, 0.05f), 0, "holds 全 0 でも落ちずに index を返す");

    // 可変長 + 単発
    SpriteAnimSeq once = seq;
    once.mode = kFlipbookOnce;
    bool fin = false;
    checki(SpriteSeqIndexAt(once, 0.45f, &fin), 2, "可変長・単発の最終コマ");
    check(!fin, "尺の途中はまだ完了でない");
    checki(SpriteSeqIndexAt(once, 2.00f, &fin), 2, "可変長・単発は最終コマで停止");
    check(fin, "可変長でも完了フラグが立つ");
}

// --- シーケンス検索 ----------------------------------------------------------
void TestFindSeq()
{
    std::printf("TestFindSeq\n");

    SpriteAnimSheet sheet;
    sheet.cols = 8; sheet.rows = 4;
    SpriteAnimSeq idle;   idle.name   = "idle";
    SpriteAnimSeq run;    run.name    = "run";
    sheet.seqs = {idle, run};

    const SpriteAnimSeq* s = FindSpriteSeq(sheet, "run");
    check(s != nullptr && s->name == "run", "名前で引ける");
    check(FindSpriteSeq(sheet, "") == &sheet.seqs.front(), "空名は先頭シーケンス");
    check(FindSpriteSeq(sheet, "nope") == nullptr, "無い名前は nullptr");

    SpriteAnimSheet empty;
    check(FindSpriteSeq(empty, "") == nullptr, "シーケンス皆無なら nullptr");
}
} // namespace

int main()
{
    std::printf("=== SpriteAnimAsset tests ===\n");
    TestCellUv();
    TestUniformSeq();
    TestVariableHolds();
    TestFindSeq();

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
