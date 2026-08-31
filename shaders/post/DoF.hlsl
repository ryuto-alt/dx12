// 被写界深度（gather 方式ボケ）。
// パス1(CocDown): 半解像度へ シーン色 + CoC(錯乱円, α に 0.5 中心で符号付き格納)。
// パス2(Gather):  ゴールデンアングル渦巻き 32 タップのディスク gather。
//                 サンプル自身の CoC がその距離をカバーする時のみ寄与＝フォーカス面の滲み防止。
// パス3(Composite): フル解像度で シャープ↔ボケ を CoC でブレンド。
// 全 RT はシーンと同じ正規化 UV レイアウト（サブ矩形対応）。

#include "FullscreenTri.hlsli"

cbuffer DofCB : register(b0)
{
    float4 rectP;   // xy=UVオフセット, zw=UVスケール
    float4 focus;   // x=フォーカス距離(view), y=1/フォーカス範囲, z=最大ボケ半径(px, 半解像度基準), w=未使用
    float4 texelP;  // xy=gatherソースのテクセル, z=projA(proj._33), w=projB(proj._43)
};

Texture2D    gScene : register(t0);  // パス1/3: シーン。パス2: 半解像度(色+CoC)
Texture2D    gAux   : register(t1);  // パス3: 半解像度ボケ結果
Texture2D    gDepth : register(t2);  // パス1/3: 深度(R32_FLOAT)
SamplerState gSamp  : register(s0);

// ★DoF が実用にならなかった真犯人（2026-09-01）。
//   viewZ = projB / (d - projA)。projA = proj._33 > 1、projB = proj._43 < 0 なので
//   【分母は常に負】。旧実装は max(den, 1e-6) でクランプしていたため、分母が常に 1e-6 に
//   潰れて viewZ が -80000 のような値になり、CoC が画面全体で ±1 に張り付いていた
//   （＝「合っているはずの設定なのに画面全体がボケる」）。RenderDebug.hlsl は同じ罠を
//   既に踏んで直してあり、そちらと同じ書き方に揃える。
static float ViewZ(float d)
{
    float den = d - texelP.z;
    return texelP.w / (abs(den) < 1e-8 ? -1e-8 : den);
}

static float CocAt(float2 uvFull)
{
    float d = gDepth.Sample(gSamp, uvFull).r;
    float z = ViewZ(d);
    return clamp((z - focus.x) * focus.y, -1.0, 1.0);   // 負=手前, 正=奥
}

// ---- パス1: 半解像度へ 色 + CoC ----
float4 DofCocPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uv = i.uv * rectP.zw + rectP.xy;
    float3 c  = gScene.Sample(gSamp, uv).rgb;
    return float4(c, CocAt(uv) * 0.5 + 0.5);
}

// ---- パス2: ディスク gather ----
float4 DofGatherPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uvFull = i.uv * rectP.zw + rectP.xy;
    float4 center = gScene.Sample(gSamp, uvFull);
    float  coc    = center.a * 2.0 - 1.0;
    float  radiusPx = abs(coc) * focus.z;
    if (radiusPx < 0.5)
        return float4(center.rgb, 0.0);

    float3 acc  = center.rgb;
    float  wsum = 1.0;
    const float GA = 2.39996323;   // ゴールデンアングル
    [loop] for (int k = 1; k <= 32; ++k)
    {
        float  r = sqrt((float)k / 32.0) * radiusPx;
        float  a = (float)k * GA;
        float2 o = float2(cos(a), sin(a)) * r * texelP.xy;
        float2 su = clamp(uvFull + o, rectP.xy, rectP.xy + rectP.zw);
        float4 s  = gScene.Sample(gSamp, su);
        float  sc = abs(s.a * 2.0 - 1.0);
        float  w  = saturate(sc * focus.z - r + 1.0);   // その距離まで届く CoC のみ
        acc  += s.rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, saturate(abs(coc) * 2.0));
}

// ---- パス3: フル解像度合成 ----
float4 DofCompositePS(FSQuadVSOut i) : SV_TARGET
{
    float2 uv    = i.uv * rectP.zw + rectP.xy;
    float3 sharp = gScene.Sample(gSamp, uv).rgb;
    float4 blur  = gAux.Sample(gSamp, uv);

    // フル解像度 CoC でフォーカス境界をシャープに保ちつつ、
    // 近景の滲み出しは半解像度側のブラー量から拾う
    float coc   = abs(CocAt(uv));
    float blend = max(saturate(coc * 2.0), blur.a * 0.75);
    return float4(lerp(sharp, blur.rgb, blend), 1.0);
}
