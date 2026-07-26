// RtCommon.hlsli — DXR 1.1 inline raytracing（RayQuery）の共有定義。
//
// ★プロファイルは ps_6_5 以上。RayQuery は SM 6.5 必須で、6.4 以下では
//   「Opcode AllocateRayQuery not valid in shader model ps_6_4」でコンパイルが落ちる（実測）。
//   DXR 1.1 の inline raytracing は「任意のシェーダステージ」で使えるので、
//   このエンジンでは compute ではなくフルスクリーン三角形の **ピクセルシェーダ**で走らせる。
//   理由: 出力が R8_UNORM / RGBA16F の RenderTarget（RTV+SRV を持つ既存クラス）で、
//   SSAO / コンタクトシャドウとまったく同じ配線のまま t8 / t11 へ差し込めるため。
//   compute にすると UAV ディスクリプタと状態遷移が増えるだけで得が無い。
//
// ★TLAS は **ルート SRV ディスクリプタ**で渡す（ディスクリプタテーブルではない）。
//   "It is bound as a raw buffer SRV in a descriptor table or root descriptor SRV"
//   — Microsoft Learn / RaytracingAccelerationStructure。
//   Microsoft の DXR サンプルも InitAsShaderResourceView + Set*RootShaderResourceView(VA) 方式。
//   毎フレーム TLAS のバッファが作り直されても SRV を書き直す必要が無いのが利点。
#ifndef RT_COMMON_HLSLI
#define RT_COMMON_HLSLI

#include "../post/FullscreenTri.hlsli"   // FSTriVS / FSQuadVSOut

Texture2D<float>                gDepth : register(t0);   // カメラ深度（R32_FLOAT・フル RT サイズ）
RaytracingAccelerationStructure gTlas  : register(t1);   // ルート SRV（VA 直指定）
Texture2D<float>                gSsao  : register(t2);   // SSAO の結果（RT-AO と min 合成する用）

// RtScreenPass.cpp の RtParamsCB とバイト単位で一致させること（static_assert あり）。
// ★可変部は必ず float4 の配列に押し込む。DXC は cbuffer の未参照メンバを消して
//   残りを詰め直す（00-COORDINATION N25）が、配列は要素単位で消えないので、
//   3 本のシェーダが別々のメンバしか読まなくてもオフセットがズレない。
cbuffer RtParams : register(b0)
{
    float4x4 gInvViewProj;   // サブビューポート NDC → ワールド（転置済み: mul(row, M)）
    float4x4 gInvProj;       // サブビューポート NDC → ビュー（転置済み）
    float4   gRtP[8];
};

#define gCameraPos    gRtP[0].xyz   // カメラのワールド位置
#define gZNear        gRtP[0].w
#define gLightDir     gRtP[1].xyz   // 太陽の「進行方向」（PerFrame の lightDir と同じ向き）
#define gSunTan       gRtP[1].w     // tan(太陽の角半径)。0 でハードシャドウ
#define gViewport     gRtP[2]       // xy = 原点(px), zw = サイズ(px)
#define gRtSize       gRtP[3].xy    // 出力 RT のサイズ(px)
#define gRtInvSize    gRtP[3].zw
#define gNormalBias   gRtP[4].x     // レイ始点の法線方向オフセット(m)
#define gMaxDistance  gRtP[4].y     // レイの最大距離(m)。0 以下で無限
#define gIntensity    gRtP[4].z
#define gFrameJitter  gRtP[4].w     // 時間ディザの位相（TAA 無効時は C++ が 0 を送る）
#define gAoRadius     gRtP[5].x
#define gAoRayCount   gRtP[5].y
#define gAoPower      gRtP[5].z
#define gDebugRange   gRtP[5].w     // デバッグ表示のレンジ(m)
#define gZFar         gRtP[6].x
#define gCombineSsao  gRtP[6].y     // 1 で RT-AO と SSAO を min() 合成する

static const float RT_PI    = 3.14159265f;
static const float RT_2PI   = 6.28318531f;
// レイの既定 TMax。1e30 のような極端な値は traversal の精度を落とすので、
// 「どんなシーンでも十分遠い」程度に留める（1000km）。
static const float RT_TMAX  = 1e6f;

// 出力 RT のピクセル座標 → サブビューポート基準の NDC(-1..1, y 上向き)。
// ★シーンは m_sceneRT の**サブ矩形**に描かれている。この原点引きを忘れると
//   ゲームモードでは正しく、エディタのシーンビューでだけ横にずれる（計画09 R14）。
float2 RtPixelToNdc(float2 px)
{
    float2 vpUV = (px - gViewport.xy) / max(gViewport.zw, 1e-6);
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

bool RtInsideViewport(float2 px)
{
    return px.x >= gViewport.x && px.x <= gViewport.x + gViewport.z
        && px.y >= gViewport.y && px.y <= gViewport.y + gViewport.w;
}

float3 RtWorldFromDepth(float2 px, float depth)
{
    float4 clip = float4(RtPixelToNdc(px), depth, 1.0);
    float4 w = mul(clip, gInvViewProj);
    return w.xyz / w.w;
}

// ビュー空間 Z（メートル、カメラ前方が正）。距離フェード用。
float RtViewZFromDepth(float2 px, float depth)
{
    float4 clip = float4(RtPixelToNdc(px), depth, 1.0);
    float4 v = mul(clip, gInvProj);
    return v.z / v.w;
}

// Interleaved gradient noise（Jorge Jimenez）。ピクセル座標だけで決まる空間ディザ。
// gFrameJitter は TAA 有効時のみ非 0（無効時に回すとチラつくだけ）。
float RtIgn(float2 px)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(px + gFrameJitter, magic.xy)));
}

float RtHash(float2 px, float seed)
{
    return frac(sin(dot(px + seed * 17.13, float2(12.9898, 78.233))) * 43758.5453);
}

// N を軸とする正規直交基底。
void RtBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = (abs(N.y) < 0.99) ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// 太陽を「角半径 θ の円盤」とみなしてレイ方向をコーン内へ散らす。
// tanTheta = 0 なら完全なハードシャドウ（ノイズゼロ）。
float3 RtJitterSunDir(float3 L, float2 xi, float tanTheta)
{
    if (tanTheta <= 0.0) return L;
    float3 T, B;
    RtBasis(L, T, B);
    float r = tanTheta * sqrt(saturate(xi.x));
    float a = RT_2PI * xi.y;
    return normalize(L + T * (r * cos(a)) + B * (r * sin(a)));
}

// コサイン重み付き半球サンプル（AO 用）。
float3 RtCosineHemisphere(float3 N, float2 xi)
{
    float3 T, B;
    RtBasis(N, T, B);
    float r   = sqrt(saturate(xi.x));
    float phi = RT_2PI * xi.y;
    return normalize(T * (r * cos(phi)) + B * (r * sin(phi))
                   + N * sqrt(saturate(1.0 - xi.x)));
}

// 遮蔽判定だけのレイ。1.0 = 遮蔽なし / 0.0 = 遮蔽あり。
// ACCEPT_FIRST_HIT_AND_END_SEARCH で最初に当たった時点で打ち切る。
// 全ジオメトリが OPAQUE（RaytracingScene が FLAG_OPAQUE を立てている）なので
// シェーダ側で処理すべき候補は発生せず、Proceed() は 1 回で完了する。
float RtTraceOcclusion(float3 origin, float3 dir, float tMin, float tMax)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_CULL_NON_OPAQUE
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// 最近傍ヒットまでの距離。ミスなら -1。
float RtTraceClosestT(float3 origin, float3 dir, float tMin, float tMax)
{
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    // 全ジオメトリが OPAQUE かつ手続き型プリミティブ無しなので Proceed() は必ず 1 回で false を
    // 返す（シェーダ側で処理すべき候補が発生しない）。それでも上限を切っておく:
    // 万一 true を返し続けると GPU が無限ループして TDR（画面が固まる）になるため。
    [loop] for (int guard = 0; guard < 8 && q.Proceed(); ++guard) {}
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0;
}

// 深度から復元したワールド位置の画面微分で法線を作る。
// ★ddx/ddy は quad 内で分岐すると値が未定義になるので、早期 return より前に必ず評価すること
//   （ContactShadow.hlsl と同じ作法）。
float3 RtNormalFromWorldPos(float3 P)
{
    float3 N = normalize(cross(ddx(P), ddy(P)));
    return (dot(N, gCameraPos - P) < 0.0) ? -N : N;
}

#endif // RT_COMMON_HLSLI
