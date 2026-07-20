#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Types.h"
#include "renderer/SpriteAnim.h"   // SpriteUvRect / FlipbookMode（Sprite2D と共通）

namespace dx12e
{

// .spranim（スプライトシート = 連番アニメアセット）の中身と評価。
// 既存の UIImage/Sprite2D の animFrames/animCols は「等分割・連続フレーム」しか表現できないので、
// こちらは **任意のセル順・可変フレーム長・名前付き複数シーケンス** を扱う。
// GPU/ECS に触れない純データ + 純関数なので単体テスト可能（tests/sprite_anim_asset_test.cpp）。

// 1シーケンス（"idle" / "run" / "attack" …）。
struct SpriteAnimSeq
{
    std::string name;
    // シート内のセル番号（行優先: 0 が左上、cols-1 が1行目右端）。任意順・重複可。
    std::vector<i32> frames;
    // frames と同じ長さなら「各フレームの表示時間倍率」。空 or 長さ違いなら全部 1.0 扱い
    // （長さ違いを黙って埋めると編集ミスが分かりにくいので、丸ごと無視する）。
    std::vector<f32> holds;
    f32 fps  = 12.0f;
    i32 mode = kFlipbookLoop;   // FlipbookMode（ループ/単発/往復）
    // 再生完了時に EventBus へ発火（単発モードのみ意味を持つ）。空なら発火しない。
    std::string finishEvent;
};

// シート全体。cols x rows の等分割グリッドを前提にする（トリム済みアトラスは非対応）。
struct SpriteAnimSheet
{
    std::string texturePath;   // assets 相対（例 "sprites/player.png"）
    i32 cols = 1;
    i32 rows = 1;
    std::vector<SpriteAnimSeq> seqs;
};

// ---------------------------------------------------------------------------
// 純関数
// ---------------------------------------------------------------------------

// セル番号 → UV 矩形。cols/rows は 1 未満なら 1 に丸める。
// 範囲外のセル番号は [0, cols*rows) にクランプ（黒画面より「間違ったコマが出る」方が原因を追いやすい）。
inline SpriteUvRect SpriteSheetCellUv(i32 cols, i32 rows, i32 cell)
{
    const i32 c = (cols > 0) ? cols : 1;
    const i32 r = (rows > 0) ? rows : 1;
    const i32 total = c * r;
    if (cell < 0) cell = 0;
    if (cell >= total) cell = total - 1;

    const i32 cx = cell % c;
    const i32 cy = cell / c;
    const f32 cw = 1.0f / static_cast<f32>(c);
    const f32 ch = 1.0f / static_cast<f32>(r);
    return { cx * cw, cy * ch, (cx + 1) * cw, (cy + 1) * ch };
}

// シーケンスの総再生時間（秒）。holds を考慮する。frames が空なら 0。
inline f32 SpriteSeqDuration(const SpriteAnimSeq& seq)
{
    if (seq.frames.empty()) return 0.0f;
    const f32 fps = (seq.fps > 0.0f) ? seq.fps : 12.0f;
    const bool useHolds = seq.holds.size() == seq.frames.size();

    f32 units = 0.0f;
    for (size_t i = 0; i < seq.frames.size(); ++i)
        units += useHolds ? (std::max)(0.0f, seq.holds[i]) : 1.0f;
    // holds が全部 0 の病的なデータでも 0 除算しないよう最低1フレーム分は確保する
    if (units <= 0.0f) units = static_cast<f32>(seq.frames.size());
    return units / fps;
}

// 経過時間 → frames 配列内のインデックス（セル番号ではない）。
// mode に従って畳む。frames が空なら -1。
// outFinished は「単発モードで最終フレームに到達済みか」（他モードでは常に false）。
inline i32 SpriteSeqIndexAt(const SpriteAnimSeq& seq, f32 time, bool* outFinished = nullptr)
{
    if (outFinished) *outFinished = false;
    const i32 n = static_cast<i32>(seq.frames.size());
    if (n == 0) return -1;

    const f32 fps = (seq.fps > 0.0f) ? seq.fps : 12.0f;
    const bool useHolds = seq.holds.size() == seq.frames.size();

    if (!useHolds)
    {
        // 等間隔 = 既存 Sprite2D と完全に同じ畳み方を使う（挙動の二重定義を避ける）
        const i32 idx = ComputeFlipbookFrame(n, fps, seq.mode, time);
        if (outFinished) *outFinished = IsFlipbookFinished(n, fps, seq.mode, time);
        return idx;
    }

    // 可変フレーム長: 累積 units で線形に引く。
    f32 total = 0.0f;
    for (i32 i = 0; i < n; ++i) total += (std::max)(0.0f, seq.holds[i]);
    if (total <= 0.0f) return 0;

    f32 u = time * fps;                    // 経過を「フレーム単位」へ
    if (seq.mode == kFlipbookOnce)
    {
        if (u >= total)
        {
            if (outFinished) *outFinished = true;
            return n - 1;
        }
        if (u < 0.0f) u = 0.0f;
    }
    else if (seq.mode == kFlipbookPingPong && n > 1)
    {
        // 往復: 前半 total + 後半 total（両端は holds ぶん素直に2回止まる）。
        // 等間隔版と違い端の重複を削らないのは、可変長だと「端だけ倍の長さ」が
        // 意図的な溜めであることが多いため。
        const f32 period = total * 2.0f;
        u = std::fmod(u, period);
        if (u < 0.0f) u += period;
        if (u >= total) u = period - u;    // 折り返し
    }
    else
    {
        u = std::fmod(u, total);
        if (u < 0.0f) u += total;
    }

    f32 acc = 0.0f;
    for (i32 i = 0; i < n; ++i)
    {
        acc += (std::max)(0.0f, seq.holds[i]);
        if (u < acc) return i;
    }
    return n - 1;
}

// 経過時間 → UV 矩形。frames が空なら 0..1 の全面を返す（テクスチャ全体 = 事故が見て分かる）。
inline SpriteUvRect SpriteSeqUvAt(const SpriteAnimSheet& sheet, const SpriteAnimSeq& seq,
                                  f32 time, bool* outFinished = nullptr)
{
    const i32 idx = SpriteSeqIndexAt(seq, time, outFinished);
    if (idx < 0) return { 0.0f, 0.0f, 1.0f, 1.0f };
    return SpriteSheetCellUv(sheet.cols, sheet.rows, seq.frames[static_cast<size_t>(idx)]);
}

// 名前でシーケンスを引く。見つからなければ nullptr（名前が空なら先頭を返す）。
inline const SpriteAnimSeq* FindSpriteSeq(const SpriteAnimSheet& sheet, const std::string& name)
{
    if (sheet.seqs.empty()) return nullptr;
    if (name.empty()) return &sheet.seqs.front();
    for (const auto& s : sheet.seqs)
        if (s.name == name) return &s;
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON IO（SpriteAnimAsset.cpp）
// ---------------------------------------------------------------------------

// JSON バイト列 → SpriteAnimSheet。パース失敗なら false（out は不定）。
bool ParseSpriteAnimSheet(const std::vector<uint8_t>& jsonBytes, SpriteAnimSheet& out);

// SpriteAnimSheet → .spranim 用 JSON 文字列（整形あり）。
std::string SerializeSpriteAnimSheet(const SpriteAnimSheet& sheet);

} // namespace dx12e
