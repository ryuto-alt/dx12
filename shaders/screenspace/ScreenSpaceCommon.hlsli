// ScreenSpaceCommon.hlsli - スクリーン空間系（SSR / SSGI / G-Buffer 書き出し）の共通関数。
//
// ★このヘッダは cbuffer を一切宣言しない。必要な値は全部引数で受け取る。
//   深度プリパスの PS（shaders/velocity/*）と SSR/SSGI の PS の両方から include されるため。
//
// ビューポート規約は SSAO.hlsl / ContactShadow.hlsl と完全に同じ:
//   シーンはフル RT（= m_sceneRT / 深度バッファ）の「サブ矩形」に描かれている。
//   viewport.xy = 矩形の原点(px) / viewport.zw = 矩形のサイズ(px)。
//   ゲームモードでは (0,0,w,h) なので、ここを間違えても**エディタでしか再現しない**。
#ifndef SCREENSPACE_COMMON_HLSLI
#define SCREENSPACE_COMMON_HLSLI

// フル RT の UV(0..1) → サブビューポート基準の NDC(-1..1, y上向き)
float2 SS_UVToNDC(float2 uv, float2 invRTSize, float4 viewport)
{
    float2 px   = uv / max(invRTSize, 1e-6);
    float2 vpUV = (px - viewport.xy) / max(viewport.zw, 1e-6);
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

// サブビューポート基準の NDC → フル RT UV(0..1)
float2 SS_NDCToUV(float2 ndc, float2 invRTSize, float4 viewport)
{
    float2 vpUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return (viewport.xy + vpUV * viewport.zw) * invRTSize;
}

// 深度バッファの値からビュー空間位置を復元する（invProj は転置済み＝ mul(row, invProj)）。
float3 SS_ViewPosFromDepth(float2 uv, float depth, float4x4 invProj, float2 invRTSize, float4 viewport)
{
    float4 clip = float4(SS_UVToNDC(uv, invRTSize, viewport), depth, 1.0);
    float4 v    = mul(clip, invProj);
    return v.xyz / v.w;
}

// Interleaved gradient noise（Jorge Jimenez）。ピクセル座標だけで決まる空間ディザ。
float SS_IGN(float2 px)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(px, magic.xy)));
}

// フル RT のピクセル座標がビューポート矩形の中か
bool SS_InViewport(float2 px, float4 viewport)
{
    return px.x >= viewport.x && px.x <= viewport.x + viewport.z
        && px.y >= viewport.y && px.y <= viewport.y + viewport.w;
}

// ---------------------------------------------------------------------------
// HDR ソースの非有限値を落とす。
// ★これを省くと画面が丸ごと真っ黒になる（実際に踏んだ）:
//   環境キューブや前フレームカラーに Inf が 1 テクセルでも混ざると
//   （fp16 の上限 65504 を超える太陽など）、SSR/SSGI の値が Inf → フォワードの
//   ambient が Inf → ACES トーンマップで Inf/Inf = NaN → 全ジオメトリが黒。
//   NaN は 0 へ、Inf は fp16 で表せる大きな有限値へ丸める（反射の明るさは残す）。
// ---------------------------------------------------------------------------
float SS_SanitizeF(float v)
{
    return isnan(v) ? 0.0 : min(max(v, 0.0), 60000.0);
}
float3 SS_Sanitize(float3 c)
{
    return float3(SS_SanitizeF(c.x), SS_SanitizeF(c.y), SS_SanitizeF(c.z));
}

// ---------------------------------------------------------------------------
// 八面体エンコード（Krzysztof Narkowicz 版）
//   https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
//   G-Buffer は fp16 なので量子化ノイズ（8bit だと specular がウォブルする）は出ない。
// ---------------------------------------------------------------------------
float2 SS_OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 SS_OctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    n.xy = (n.z >= 0.0) ? n.xy : SS_OctWrap(n.xy);
    return n.xy;
}

float3 SS_OctDecode(float2 f)
{
    float3 n = float3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float  t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// ---------------------------------------------------------------------------
// G-Buffer の中身（深度プリパスが MRT の SV_TARGET1 へ書くもの）
//   R16G16B16A16_FLOAT
//     .xy = oct(ワールド空間法線)   ← ビュー空間ではない。読む側が view で回すこと
//     .z  = roughness (0..1)
//     .w  = metallic  (0..1)
// ---------------------------------------------------------------------------
float4 SS_PackGBuffer(float3 worldNormal, float roughness, float metallic)
{
    return float4(SS_OctEncode(normalize(worldNormal)), roughness, metallic);
}

struct SSSurface
{
    float3 N;          // ワールド空間法線
    float  roughness;
    float  metallic;
};

SSSurface SS_UnpackGBuffer(float4 raw)
{
    SSSurface o;
    o.N         = SS_OctDecode(raw.xy);
    o.roughness = raw.z;
    o.metallic  = raw.w;
    return o;
}

// roughness/metallic を 1 float に詰める（b0 のルート定数が 40 DWORD しか無いため）。
//   packed = round(rough*255) + round(metal*255)*256   （fp32 で厳密表現できる整数）
void SS_UnpackMaterial(float packed, out float roughness, out float metallic)
{
    float m   = floor(packed * (1.0 / 256.0));
    float r   = packed - m * 256.0;
    roughness = r * (1.0 / 255.0);
    metallic  = m * (1.0 / 255.0);
}

#endif // SCREENSPACE_COMMON_HLSLI
