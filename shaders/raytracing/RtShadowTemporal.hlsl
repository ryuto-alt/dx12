// RtShadowTemporal.hlsl — RT 影デノイザ 2/3: 時間蓄積（速度で再投影した履歴 + 2 次モーメント）
//
// 入力  t5 = 今フレームのトレース（RG16F: x=可視率, y=遮蔽物までの距離）
//       t7 = 履歴（RGBA16F: x=可視率の平均, y=2 次モーメント, z=履歴の長さ n, w=ビュー Z）
//       t8 = 速度（TAA と同じ。現 UV - 前 UV。ビューポートローカル UV）
// 出力  RGBA16F（履歴と同じ並び）。空間フィルタ(RtShadowSpatial)の入力であり、次フレームの履歴でもある。
//
// 方針（SVGF の時間蓄積を 1 チャンネルに絞ったもの）:
//   ・履歴は 4 近傍の双線形。1 点ごとに「ビュー Z の相対差 5% 以内・画面内・n>0」を確かめ、
//     通った点だけで重みを正規化する（縁で履歴が混ざらない）。1 点も通らなければ n=0 から。
//   ・n = min(n_hist + 1, 上限)、α = lerp(0.05, 1, sqrt(max(8-n,0)/8))（FFX と同じ。最小 0.05）。平均とモーメントを同じ α で更新。
//   ・2 次モーメントは空間フィルタが分散（輝度重み）を作るのに使う。
//   ・★履歴は今フレームの近傍の平均 μ・標準偏差 σ の箱 [μ-0.5σ, μ+0.5σ] へクランプし、外れた分だけ n を縮める。
//     AMD FidelityFX Shadow Denoiser（ffx_denoiser_shadows_tileclassification.h）と同じ式。
//     クランプが無いと、歩くと前のフレームの影の位置が 32 フレーム残り、影の縁が遅れてついてくる
//     （AssetBench・5 m/s で地面の影の縁の 2.3% の画素が停止後と 20/255 以上ずれていた、2026-10-17）。
//     近傍は FFX の 17×17（半径 8・重み exp(-3i²/81)）を 2 画素おきの 9×9 点で近似する（読み込み 289 → 81）。
//   ・速度は 3×3 で最も手前の画素のものを使う（FFX の GetClosestVelocity。物の縁で背景の速度を拾わない）。
#include "RtCommon.hlsli"

Texture2D<float2> gShTrace : register(t5);   // 今フレームのトレース
Texture2D<float4> gShHist  : register(t7);   // 履歴
Texture2D<float2> gShVel   : register(t8);   // 速度

float4 PSMain(FSQuadVSOut i) : SV_TARGET
{
    const int2   ip = int2(i.pos.xy);
    const float  d  = gDepth.Load(int3(ip, 0));
    if (!RtInsideViewport(i.pos.xy) || d >= 1.0) return float4(1.0, 1.0, 0.0, 0.0);

    const float  cur   = gShTrace.Load(int3(ip, 0)).x;
    const float  viewZ = RtViewZFromDepth(i.pos.xy, d);

    // ---- 今フレームの近傍の平均と分散（クランプの箱）----
    const int2 vpLo = int2(gViewport.xy);
    const int2 vpHi = int2(gViewport.xy + gViewport.zw) - 1;
    float nbM1 = 0.0, nbM2 = 0.0, nbW = 0.0;
    [unroll]
    for (int ky = -4; ky <= 4; ++ky)
    {
        [unroll]
        for (int kx = -4; kx <= 4; ++kx)
        {
            const int2  tp = clamp(ip + int2(kx, ky) * 2, vpLo, vpHi);
            const float w  = exp(-3.0 * float(kx * kx + ky * ky) * 4.0 / 81.0);
            const float v  = gShTrace.Load(int3(tp, 0)).x;
            nbM1 += v * w; nbM2 += v * v * w; nbW += w;
        }
    }
    const float nbMean = nbM1 / nbW;
    const float nbSd   = sqrt(max(nbM2 / nbW - nbMean * nbMean, 0.0));

    float histMean = 0.0, histMom = 0.0, histN = 0.0;
    if (gShHistValid > 0.5)
    {
        // 3×3 で最も手前（標準 Z なので深度が最小）の画素の速度。
        int2 cp = ip; float cd = d;
        [unroll]
        for (int vy = -1; vy <= 1; ++vy)
        {
            [unroll]
            for (int vx = -1; vx <= 1; ++vx)
            {
                const int2  tp = clamp(ip + int2(vx, vy), vpLo, vpHi);
                const float td = gDepth.Load(int3(tp, 0));
                if (td < cd) { cd = td; cp = tp; }
            }
        }
        const float2 vel   = gShVel.Load(int3(cp, 0));
        const float2 uv    = (i.pos.xy - gViewport.xy) / max(gViewport.zw, 1e-6);
        const float2 puv   = uv - vel;
        // 前フレームのピクセル座標（中心が +0.5）。双線形の基準は 0.5 を引いた連続座標。
        const float2 ppx   = puv * gViewport.zw + gViewport.xy;
        const float2 base  = ppx - 0.5;
        const int2   b0    = int2(floor(base));
        const float2 fr    = base - float2(b0);
        const int2   hi    = int2(gViewport.xy + gViewport.zw) - 1;
        const int2   lo    = int2(gViewport.xy);

        float4 acc = 0.0;
        float  wsum = 0.0;
        [unroll]
        for (int k = 0; k < 4; ++k)
        {
            const int2 o  = int2(k & 1, k >> 1);
            const int2 tp = b0 + o;
            if (any(tp < lo) || any(tp > hi)) continue;                     // 画面外
            const float4 h = gShHist.Load(int3(tp, 0));
            if (h.z < 0.5) continue;                                        // 履歴なし
            if (abs(h.w - viewZ) > 0.05 * abs(viewZ)) continue;             // ビュー Z の相対差 5% 超
            const float w = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y);
            acc  += h * w;
            wsum += w;
        }
        if (wsum > 1e-4)
        {
            acc /= wsum;
            histMean = acc.x; histMom = acc.y; histN = acc.z;
        }
    }

    // ---- 履歴のクランプと n の減衰（FFX と同じ: 箱は ±0.5σ、減衰は exp(-t²/20)）----
    if (histN > 0.0)
    {
        const float t = (histMean - nbMean) / max(0.5 * nbSd, 0.001);
        histN   *= exp(-t * t / 20.0);
        const float clamped = clamp(histMean, nbMean - 0.5 * nbSd, nbMean + 0.5 * nbSd);
        // 2 次モーメントは平均を動かした分だけ平行移動する（分散は保つ）。
        histMom  = max(histMom + clamped * clamped - histMean * histMean, 0.0);
        histMean = clamped;
    }

    const float n     = min(histN + 1.0, max(gShMaxN, 1.0));
    // 混ぜ率は FFX の lerp(0.05, 1, sqrt(max(8-n,0)/8))（履歴が短いほど今を信じる、最小 0.05）。
    // 2 次モーメントも同じ率で積む（平均と率が違うと mom - mean² が分散にならない）。
    const float a     = lerp(0.05, 1.0, sqrt(max(8.0 - n, 0.0) / 8.0));
    const float mean  = lerp(histMean, cur, a);
    const float mom   = lerp(histMom, cur * cur, a);
    return float4(mean, mom, n, viewZ);
}
