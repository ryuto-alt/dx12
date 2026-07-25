// RenderDebug.hlsl — 中間バッファの可視化（dx12_render_debug のバックエンド）。
//
// ★フォワード PS には一切足さない。ここは完全に独立したフルスクリーンパスで、
//   シーン RT（ポスト前）へ後掛けで上書きする。00-COORDINATION §6 N24（[branch] で
//   飛ばしてもコードが在るだけでフォワード PS が遅くなる）を構造的に回避するため。
//
// 出力はそのまま「表示したい色」（ガンマ済みの見た目）。MCP の readback は
// render debug 中だけトーンマップ/ガンマを掛けずに 8bit へ落とすので、
// ここで書いた値がそのまま PNG に出る。
//
// ★配置場所について: shaders/debug/ には置けない（.gitignore の `Debug/` が
//   大文字小文字を無視して当たり、新規ファイルが git に追跡されない。N1）。
#include "../post/FullscreenTri.hlsli"
#include "../screenspace/ScreenSpaceCommon.hlsli"

// モード（C++ 側 RenderDebugMode と 1:1）
#define DBG_NORMAL          1
#define DBG_ROUGHNESS       2
#define DBG_METALLIC        3
#define DBG_DEPTH           4
#define DBG_AO              5
#define DBG_CONTACT_SHADOW  6
#define DBG_VELOCITY        7
#define DBG_SSR             8
#define DBG_SSGI            9

cbuffer RenderDebugCB : register(b0)
{
    float4 rectP;    // xy = UV オフセット, zw = UV スケール（シーン RT 内のビューポート矩形）
    float4 modeP;    // x = モード, y = 表示倍率, z = projA(_33), w = projB(_43)
    float4 rangeP;   // x = 深度の表示レンジ(m), y = 露出(SSR/SSGI 用), zw = 予約
};

Texture2D<float4> gSource : register(t0);   // モードごとに差し替わる 2D テクスチャ
Texture2D<float>  gDepth  : register(t1);   // 深度（常にバインド）
SamplerState      gPoint  : register(s0);

// 青→シアン→緑→黄→赤（Turbo 風の簡易版）。数値の大小を目で追える色に写す。
float3 Heat(float t)
{
    t = saturate(t);
    float3 c;
    if (t < 0.25)      c = lerp(float3(0.0, 0.0, 0.5), float3(0.0, 0.8, 0.9), t * 4.0);
    else if (t < 0.5)  c = lerp(float3(0.0, 0.8, 0.9), float3(0.1, 0.9, 0.1), (t - 0.25) * 4.0);
    else if (t < 0.75) c = lerp(float3(0.1, 0.9, 0.1), float3(1.0, 0.9, 0.0), (t - 0.5) * 4.0);
    else               c = lerp(float3(1.0, 0.9, 0.0), float3(1.0, 0.05, 0.0), (t - 0.75) * 4.0);
    return c;
}

float3 LinearToGamma(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

float4 RenderDebugPS(FSQuadVSOut i) : SV_TARGET
{
    const float2 uv   = i.uv * rectP.zw + rectP.xy;
    const uint   mode = (uint)(modeP.x + 0.5);
    const float  gain = modeP.y;

    float3 outColor = float3(1.0, 0.0, 1.0);   // 未対応モードはマゼンタ（気づけるように）

    if (mode == DBG_NORMAL)
    {
        // G-Buffer .xy = oct(ワールド法線)。0.5 + 0.5*N の定番表示。
        float3 n = SS_OctDecode(gSource.SampleLevel(gPoint, uv, 0).xy);
        outColor = 0.5 + 0.5 * n;
    }
    else if (mode == DBG_ROUGHNESS)
    {
        outColor = saturate(gSource.SampleLevel(gPoint, uv, 0).zzz * gain);
    }
    else if (mode == DBG_METALLIC)
    {
        outColor = saturate(gSource.SampleLevel(gPoint, uv, 0).www * gain);
    }
    else if (mode == DBG_DEPTH)
    {
        // 非線形深度 → ビュー空間 Z(m) → 表示レンジで正規化してヒートマップ
        float d = gDepth.SampleLevel(gPoint, uv, 0);
        if (d >= 1.0 - 1e-6)
        {
            outColor = float3(0.0, 0.0, 0.0);   // 空（何も描かれていない）は黒
        }
        else
        {
            // viewZ = projB / (d - projA)。projA = proj._33 > 1、projB = proj._43 < 0 なので
            // 分母は常に負。max(..., 1e-6) でクランプすると符号が壊れる（実際に踏んだ）。
            float den   = d - modeP.z;
            float viewZ = modeP.w / (abs(den) < 1e-8 ? -1e-8 : den);
            outColor = Heat(viewZ / max(rangeP.x, 1e-3));
        }
    }
    else if (mode == DBG_AO || mode == DBG_CONTACT_SHADOW)
    {
        // R8_UNORM。1 = 遮蔽なし。暗いほど遮蔽されている。
        outColor = saturate(gSource.SampleLevel(gPoint, uv, 0).rrr);
    }
    else if (mode == DBG_VELOCITY)
    {
        // R = +X, G = +Y(画面下)。静止していれば全面が均一な (0.5,0.5,0.5)。
        float2 v = gSource.SampleLevel(gPoint, uv, 0).rg * gain;
        outColor = float3(saturate(0.5 + v.x), saturate(0.5 + v.y), 0.5);
    }
    else if (mode == DBG_SSR || mode == DBG_SSGI)
    {
        // リニア HDR。露出を掛けてガンマへ（トーンマップは掛けない＝値の大小がそのまま見える）
        float3 c = SS_Sanitize(gSource.SampleLevel(gPoint, uv, 0).rgb) * max(rangeP.y, 1e-4);
        outColor = LinearToGamma(c);
    }

    return float4(outColor, 1.0);
}
