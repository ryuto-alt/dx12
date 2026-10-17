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
// RT2: アルファテスト(MASK)の候補ループ（RT_PROCEED_WITH_ALPHA）と GeometryInfo（t6。ルート SRV）。
// ★RtBindless.hlsli 自体は RtCommon を include しない作り（DDGI の compute からも使うため）。
// ★このファイルは頂点シェーダ（Rt_VS = vs_6_0。RtShadow.hlsl の FSTriVS）からも include される。
//   ResourceDescriptorHeap は SM 6.6 以上でないと未宣言なので、6.6 未満では中身を読まず
//   候補ループは Proceed() だけにする（VS では RayQuery 関数を呼ばないので実害は無い）。
#if defined(__SHADER_TARGET_MAJOR) && (__SHADER_TARGET_MAJOR > 6 || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6))
#include "RtBindless.hlsli"
#else
#define RT_PROCEED_WITH_ALPHA(q, samp) (q).Proceed()
#endif

// α を引くサンプラ（ルートシグネチャの静的サンプラ s0 = LINEAR / WRAP）。RtAlbedo もこれを使う。
SamplerState gLinearWrap : register(s0);

Texture2D<float>                gDepth : register(t0);   // カメラ深度（R32_FLOAT・フル RT サイズ）
RaytracingAccelerationStructure gTlas  : register(t1);   // ルート SRV（VA 直指定）
Texture2D<float>                gSsao  : register(t2);   // SSAO の結果（RT-AO と min 合成する用）
// G-Buffer（深度+速度プリパスの RTV1）。xy=oct(ワールド法線) z=roughness w=metallic。
// ★AO のレイ方向とデノイザの bilateral 重みに使う。ddx/ddy 法線は quad 境界で壊れるので使わない。
Texture2D<float4>               gGBuffer : register(t3);
// デノイザの入力（生の可視率）。空間フィルタパスだけが読む。
Texture2D<float>                gAoRaw   : register(t4);

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
// ★フレーム連番。gFrameJitter と違い TAA の有無に関係なく必ず回る。
//   これが止まっていると毎フレーム同じレイ方向になり、時間方向にいくら平均しても
//   分散が 1 ミリも下がらない（SSGI が同じ罠を踏んだ記録が SSGI.hlsl にある）。
#define gFrameIndex   gRtP[6].z
#define gAoDenoiseRadius gRtP[6].w  // 空間フィルタの半径(px)。0 でフィルタ無効
#define gGBufferValid gRtP[7].x     // 1 で G-Buffer が書かれている（法線を読んでよい）
// --- RT 影デノイザ（RtShadowTemporal / RtShadowSpatial）専用 ---
#define gShHistValid  gRtP[7].y     // 1 で履歴が使える（初回・リサイズ直後・1 フレーム空いたら 0）
#define gShPixelAngle gRtP[7].z     // 透視: 1 画素の視角(rad) = 2/(proj11*高さ)。正射: 負（-1 画素のワールド幅）
#define gShMaxN       gRtP[7].w     // 履歴の長さの上限（通常 32。太陽が動いたフレームは小さくして残像を抑える）

// 八面体デコード（ScreenSpaceCommon.hlsli の SS_OctDecode と同一の式）。
// G-Buffer の .xy から ワールド法線を復元する。
float3 RtOctDecode(float2 f)
{
    float3 n = float3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float  t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

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
// 不透明材質の BLAS は OPAQUE で建っているので候補は発生せず従来と同じ 1 回で完了する。
// アルファテスト(MASK。葉・柵)は非不透明で建っているので、候補ごとに RtAlphaTestPass で α を見る（RT2）。
float RtTraceOcclusion(float3 origin, float3 dir, float tMin, float tMax)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    RT_PROCEED_WITH_ALPHA(q, gLinearWrap);
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// RtTraceOcclusion の「ヒットした t も返す」版。t = 最初に見つかったヒットまでの距離（遮蔽なしは 0）。
// ACCEPT_FIRST_HIT なので最近傍とは限らない。影デノイザが半影の幅を見積もるためだけに使う。
float RtTraceOcclusionT(float3 origin, float3 dir, float tMin, float tMax, out float hitT)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    RT_PROCEED_WITH_ALPHA(q, gLinearWrap);
    const bool hit = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
    hitT = hit ? q.CommittedRayT() : 0.0;
    return hit ? 0.0 : 1.0;
}

// 最近傍ヒットまでの距離。ミスなら -1。
float RtTraceClosestT(float3 origin, float3 dir, float tMin, float tMax)
{
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    // 不透明材質は OPAQUE の BLAS なので候補が出ない。MASK だけが候補ループを回す（RT2）。
    // 候補は有限個の三角形しか無いので、Proceed() は必ず false に収束する。
    RT_PROCEED_WITH_ALPHA(q, gLinearWrap);
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0;
}

// 深度の隣接 4 点から「より連続している側」を選んで幾何法線を作る（Wicked/Humus 方式）。
// ★ddx/ddy の法線は 2x2 クアッドの片側差分で、深度の段差（建物の凹凸の縁・柱・看板）をまたぐと
//   縁だけ法線がでたらめになる。RT 影はこの法線でレイ始点を押し出す（NdotL とオフセット量）ので、
//   縁の画素だけ始点がずれて、TAA のジッタごとに影が出たり消えたりして縦線状にちらつく
//   （AssetBench の Bistro 遠景ファサードで実測、2026-10-17）。
//   隣の画素を Load で直接読み、カメラからの距離が近い方（段差をまたがない方）の差分を採る。
//   ddx/ddy を使わないので quad 内の分岐に関する制約も無い。
float3 RtNormalFromDepthRobust(float2 px, float3 P)
{
    const int2 ip = int2(px);
    const float3 Pl = RtWorldFromDepth(px + float2(-1, 0), gDepth.Load(int3(ip + int2(-1, 0), 0)));
    const float3 Pr = RtWorldFromDepth(px + float2( 1, 0), gDepth.Load(int3(ip + int2( 1, 0), 0)));
    const float3 Pu = RtWorldFromDepth(px + float2(0, -1), gDepth.Load(int3(ip + int2(0, -1), 0)));
    const float3 Pd = RtWorldFromDepth(px + float2(0,  1), gDepth.Load(int3(ip + int2(0,  1), 0)));
    const float  d0 = length(P - gCameraPos);
    // 片側ずつ、カメラからの距離の差が小さい方を採る。
    const float3 dx = (abs(length(Pr - gCameraPos) - d0) < abs(length(Pl - gCameraPos) - d0)) ? (Pr - P) : (P - Pl);
    const float3 dy = (abs(length(Pd - gCameraPos) - d0) < abs(length(Pu - gCameraPos) - d0)) ? (Pd - P) : (P - Pu);
    float3 N = normalize(cross(dx, dy));
    // cross の向きは dx/dy の符号選択で反転しうるので、画面側(カメラ側)へ揃える。
    return (dot(N, gCameraPos - P) < 0.0) ? -N : N;
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
