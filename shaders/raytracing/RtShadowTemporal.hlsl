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
//   ・n = min(n_hist + 1, 上限)、α = 1/n（上限 32 なので最小 1/32）。平均とモーメントを同じ α で更新。
//   ・2 次モーメントは空間フィルタが分散（輝度重み）を作るのに使う。
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

    float histMean = 0.0, histMom = 0.0, histN = 0.0;
    if (gShHistValid > 0.5)
    {
        const float2 vel   = gShVel.Load(int3(ip, 0));
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

    const float n     = min(histN + 1.0, max(gShMaxN, 1.0));
    const float a     = 1.0 / n;                       // α = 1/n。上限 32 なら最小 1/32。履歴なしは n=1 → α=1
    const float mean  = lerp(histMean, cur, a);
    const float mom   = lerp(histMom, cur * cur, a);
    return float4(mean, mom, n, viewZ);
}
