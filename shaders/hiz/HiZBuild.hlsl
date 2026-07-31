// HiZBuild.hlsl — 階層深度（Hi-Z）ピラミッドの構築（cs_6_0 / CSCopy + CSReduce）。
//
// カメラ深度バッファから「各タイルの最も遠い深度」を持つミップ連鎖を作る。
// オクルージョンカリングはこれを引いて「箱の最近点がタイルの最遠面よりさらに遠いか」を見る。
//
// ── 縮約は max（最大値）────────────────────────────────────────────
// このエンジンは標準 Z（0=near / 1=far。根拠は renderer/HiZMath.h の先頭）なので、
// 深度が大きいほど遠い。タイルの代表値は **最も遠い面** = max でなければならない。
// min にすると「一番手前の面より奥にある物は全部隠れている」と誤判定して、
// 壁の手前にある物まで消える。リバース Z へ移行するならここも min へ反転する。
//
// ── 奇数サイズの取りこぼし ────────────────────────────────────────
// 入力の幅/高さが奇数のとき、単純な 2x2 縮約では最後の 1 列/1 行が
// どの出力テクセルにも入らない。そこに一番遠い面があると、その深度が消えて
// タイルの max が実際より手前になる ＝ **見えている物を隠れていると誤判定する**。
// 1052x592 のような非 2 冪解像度では連鎖の途中で必ず奇数が出るので、
// 端のスレッドだけ 3x3（片側だけ奇数なら 3x2 / 2x3）へ広げて取りこぼしを塞ぐ。

cbuffer HiZBuildCB : register(b0)
{
    uint2 gDstSize;   // 出力ミップの解像度
    uint2 gSrcSize;   // 入力の解像度（CSCopy では深度バッファの解像度）
};

Texture2D<float>   g_srcDepth : register(t0);   // CSCopy 用: カメラ深度（R32_FLOAT）
RWTexture2D<float> g_src      : register(u0);   // CSReduce 用: 1 段細かいミップ
RWTexture2D<float> g_dst      : register(u1);   // 出力ミップ

// mip 0 = カメラ深度のコピー。
// 深度バッファと Hi-Z は同じ解像度なので単純な写しでよい。
// ★描画がレンダーターゲットの部分矩形にしか行われないエディタ視点では、
//   矩形の外はクリア値 1.0 のまま入る。1.0 は「何も描かれていない＝背景」で、
//   遮蔽判定側（HiZMath.h の IsOccludedByHiZ）が 1.0 を必ず「遮蔽しない」と扱うので
//   ここで特別扱いする必要はない。
[numthreads(8, 8, 1)]
void CSCopy(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gDstSize.x || dtid.y >= gDstSize.y) return;
    g_dst[dtid.xy] = g_srcDepth.Load(int3((int2)dtid.xy, 0));
}

// mip N-1 → mip N。2x2 の最大値（端だけ 3x3 へ拡張）。
[numthreads(8, 8, 1)]
void CSReduce(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gDstSize.x || dtid.y >= gDstSize.y) return;

    const uint2 s   = dtid.xy * 2;
    const uint2 lim = gSrcSize - 1;

    float d = g_src[min(s,                lim)];
    d = max(d, g_src[min(s + uint2(1, 0), lim)]);
    d = max(d, g_src[min(s + uint2(0, 1), lim)]);
    d = max(d, g_src[min(s + uint2(1, 1), lim)]);

    // 入力が奇数幅なら、最後の出力列だけ +2 列目も畳む（取りこぼし防止）。
    const bool oddX = (gSrcSize.x & 1u) != 0u;
    const bool oddY = (gSrcSize.y & 1u) != 0u;
    const bool lastX = (dtid.x + 1u == gDstSize.x);
    const bool lastY = (dtid.y + 1u == gDstSize.y);

    if (oddX && lastX)
    {
        d = max(d, g_src[min(s + uint2(2, 0), lim)]);
        d = max(d, g_src[min(s + uint2(2, 1), lim)]);
    }
    if (oddY && lastY)
    {
        d = max(d, g_src[min(s + uint2(0, 2), lim)]);
        d = max(d, g_src[min(s + uint2(1, 2), lim)]);
    }
    // 両方奇数なら角の 1 テクセルも残る
    if (oddX && lastX && oddY && lastY)
    {
        d = max(d, g_src[min(s + uint2(2, 2), lim)]);
    }

    g_dst[dtid.xy] = d;
}
