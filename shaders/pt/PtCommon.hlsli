// PtCommon.hlsli — DXR パストレーサー(地上真値レンダラ)の共有定義。
//
// ★C++ 側の実体は src/renderer/pt/PtShared.h。構造体・定数バッファのレイアウトは
//   バイト単位で一致させること(C++ 側に static_assert あり)。
// ★このファイルは RtBindless.hlsli / RtCommon.hlsli を include しない。パストレーサーは
//   専用の TLAS・専用のテーブル(instances / materials / lights / emissive)を持つ。
//   フォワードの BRDF(PBR.hlsli + Lighting.hlsli の ShadePunctual)と【同じ式】を PtEvalBrdf に写している。
//   片方を直したらもう片方も直すこと(比較で「アルゴリズム差だけ」を測るための前提)。

#ifndef PT_COMMON_HLSLI
#define PT_COMMON_HLSLI

#define PT_NO_INDEX 0xFFFFFFFFu

// 拡張シェーディング（マルチスキャッタ / クリアコート / 異方性 / シーン）の式はフォワードと同じ純関数を使う。
// UnoSurface.hlsli は 8 DWORD → 値の復元（UnoApplyShadingExt）だけに使う（リソース宣言を持たない）。
#include "../forward/BrdfExt.hlsli"
#include "../forward/UnoSurface.hlsli"

static const float PT_PI      = 3.14159265359;
static const float PT_TWO_PI  = 6.28318530718;
static const float PT_INV_PI  = 0.31830988618;

// ---- ジョブ定数(b0)のフラグ ----
#define PT_FLAG_RR             1u     // ロシアンルーレット
#define PT_FLAG_PHYS_FALLOFF   2u     // 点/スポット光を逆二乗にする(0 = エンジンの saturate(1-d/range)^2)
#define PT_FLAG_ENV_NEE        4u     // 環境光の NEE(コサイン重みサンプル)を行う
#define PT_FLAG_CULL_BACKFACE  8u     // 未使用(予約)
#define PT_FLAG_EMISSIVE_BOTH  16u    // エミッシブを両面に出す(既定は法線側のみ)
#define PT_FLAG_FORCE_LAMBERT  32u    // 全材質を純ランバートにする(解析解テスト用)
#define PT_FLAG_NO_NORMALMAP   64u
#define PT_FLAG_MULTISCATTER   256u   // マルチスキャッタのエネルギー補償（フォワードと同じ。gBrdfLutSrv の LUT の A+B）
#define PT_FLAG_ATMOSPHERE     128u   // (PT_ATMOSPHERE 版の .cso=PathTraceAtmos_CS だけが使う。従来の .cso は DXIL が 1 ビットも変わらない)   // 環境 = 物理大気(A1)。gAtSrvBase の SRV ブロックから LUT とパラメータを引く(従来の envCube / envConst は使わない)

// ---- 材質フラグ ----
#define PT_MAT_NORMALMAP   1u
#define PT_MAT_MRTEX       2u
#define PT_MAT_EMISSIVETEX 8u
#define PT_MAT_LAMBERT     256u   // この材質だけ純ランバート(kD=1、スペキュラ無し。テスト用)
#define PT_MAT_EMIT_BOTH   512u   // この材質のエミッシブは両面

// ---- インスタンスフラグ ----
#define PT_INST_SKINNED    1u

// 128B。RaytracingScene の GeometryInfo とは別物(専用 TLAS の InstanceID がこの配列の添字)。
struct PtInstance
{
    float4 o2w0; float4 o2w1; float4 o2w2;   // オブジェクト→ワールド 3x4(行 = xyz 係数, w = 平行移動)
    float4 w2o0; float4 w2o1; float4 w2o2;   // ワールド→オブジェクト 3x4
    uint vbSrv; uint ibSrv; uint materialIndex; uint flags;
    uint emissiveBase;   // エミッシブ三角形表での先頭添字(この Instance の全三角形が prim 順に並ぶ)。無ければ PT_NO_INDEX
    uint defVbSrv;       // スキンド: 変形後位置(float3 詰め)の raw SRV。静的は PT_NO_INDEX
    uint pad0; uint pad1;
};

// 112B。
struct PtMaterial
{
    float3 tint;      float opacity;         // packedTint(RGB)と不透明度
    float  metallic;  float roughness;  float alphaCutoff;  uint flags;
    float3 emissive;  uint albedoSrv;        // 放射輝度 = 色 × 強度(テクスチャ前)
    uint   normalSrv; uint mrSrv;       uint emissiveSrv;   uint alphaMode;   // 0=Opaque 1=Mask 2=Blend
    float4 uvScaleOffset;
    uint4  ext0;      uint4 ext1;            // 拡張シェーディングの 8 DWORD(src/renderer/ShadingExt.h。0 = 無し)
};

// 64B。ClusterLight(shaders/forward/ClusterCommon.hlsli)と同じ並び。
struct PtLight
{
    float3 position;  float range;
    float3 color;     float type;            // color は intensity 乗算済み。0=point / 1=spot
    float3 direction; float cosOuter;
    float  cosInner;  float sinOuter;  float pad0;  float pad1;
};

// 32B。エミッシブ三角形の Walker alias 表(1 要素 = 1 三角形)。
struct PtEmissiveTri
{
    uint  instance; uint prim; float pmf; float thresh;   // pmf = この三角形が選ばれる確率
    uint  alias;    float area; uint pad0; uint pad1;
};

// ---- ジョブ定数(b0)。行ごとに float4 で並べる(C++ 側 pt::JobConstants と一致)----
cbuffer JobCB : register(b0)
{
    uint   gWidth;   uint gHeight;   uint gBounces;   uint gSeed;
    uint   gFlags;   float gMaxRadiance;  uint gEnvCubeSrv;  uint gLightCount;
    float3 gCamPos;    float gTanHalfFovY;
    float3 gCamRight;  float gAspect;
    float3 gCamUp;     uint  gEmissiveCount;
    float3 gCamFwd;    uint  gInstanceCount;
    float3 gSunToLight; float gSunTanRadius;    // 太陽へ向かう単位ベクトル / 角半径の tan(0 = デルタ)
    float3 gSunE;       float gSunEnabled;      // 太陽の放射照度(色×強度。フォワードの lightColor)
    float3 gEnvConst;   float gEnvLightScale;   // キューブ無し時の一様な空の放射輝度 / ライティング用スケール(iblIntensity)
    float  gEnvBgScale; uint  gRrStart;  float gLensRadius;  float gFocusDist;  // 背景スケール(skyboxIntensity)/ RR 開始深さ / 薄レンズ
    #ifdef PT_ATMOSPHERE
    float  gPixelFilter; uint gCountRays; uint gAtSrvBase; uint gBrdfLutSrv;    // 画素フィルタ半径(0=箱) / 統計を数えるか / 大気の SRV ブロック先頭 / BRDF LUT
#else
    float  gPixelFilter; uint gCountRays; uint gPad0; uint gBrdfLutSrv;         // 画素フィルタ半径(0=箱) / 統計を数えるか / BRDF LUT(rg = split-sum / b = シーンの方向アルベド)
#endif
};

// タイルごとに変わる値(ルート定数 b1、8 DWORD)
cbuffer TileCB : register(b1)
{
    uint gTileX; uint gTileY; uint gSampleBase; uint gSampleCount;
    uint gTileW; uint gTileH; uint gTilePad0; uint gTilePad1;
};

RaytracingAccelerationStructure gTlas : register(t0);
StructuredBuffer<PtInstance>    gInstances : register(t1);
StructuredBuffer<PtMaterial>    gMaterials : register(t2);
StructuredBuffer<PtLight>       gLights    : register(t3);
StructuredBuffer<PtEmissiveTri> gEmissive  : register(t4);

RWStructuredBuffer<float4> gAccum : register(u0);   // rgb = 和 / a = サンプル数
RWStructuredBuffer<uint>   gStats : register(u1);   // [0]=NaN/Inf サンプル [1]=経路数(下位) [2]=クランプされたサンプル [3]=RR 終了 [4]=経路数(上位)

SamplerState gSampWrap  : register(s0);   // LINEAR / WRAP
SamplerState gSampClamp : register(s1);   // LINEAR / CLAMP

// ---- 乱数(PCG ハッシュ。画素 × サンプル番号 × シードだけで決まる＝決定論)----
uint PtPcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float PtRand(inout uint rng)
{
    rng = PtPcg(rng);
    return float(rng >> 8u) * (1.0 / 16777216.0);   // [0,1)
}

float2 PtRand2(inout uint rng) { float a = PtRand(rng); float b = PtRand(rng); return float2(a, b); }

float PtLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 正規直交基底(Duff et al. 2017)
void PtBasis(float3 n, out float3 t, out float3 b)
{
    const float s = (n.z >= 0.0) ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 PtToWorld(float3 v, float3 n)
{
    float3 t, b;
    PtBasis(n, t, b);
    return t * v.x + b * v.y + n * v.z;
}

float3 PtCosineHemisphere(float2 u)
{
    const float r   = sqrt(u.x);
    const float phi = PT_TWO_PI * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

// ---- Ray Tracing Gems ch.6: 自己交差を避ける堅牢なレイ始点オフセット ----
float3 PtOffsetRay(float3 p, float3 n)
{
    const float origin = 1.0 / 32.0, floatScale = 1.0 / 65536.0, intScale = 256.0;
    const int3  of = int3(intScale * n);
    const float3 pi = float3(asfloat(asint(p.x) + ((p.x < 0.0) ? -of.x : of.x)),
                             asfloat(asint(p.y) + ((p.y < 0.0) ? -of.y : of.y)),
                             asfloat(asint(p.z) + ((p.z < 0.0) ? -of.z : of.z)));
    return float3(abs(p.x) < origin ? p.x + floatScale * n.x : pi.x,
                  abs(p.y) < origin ? p.y + floatScale * n.y : pi.y,
                  abs(p.z) < origin ? p.z + floatScale * n.z : pi.z);
}

// ---------------------------------------------------------------------------
//  BRDF(フォワードと同じ式。PBR.hlsli / Lighting.hlsli::ShadePunctual)
// ---------------------------------------------------------------------------
float PtD_GGX(float NdotH, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;
    const float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PT_PI * d * d, 0.0000001);
}

float PtG1(float NdotX, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

float3 PtFresnel(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// 表面のシェーディング入力(材質評価後)
struct PtSurf
{
    float3 albedo;
    float  metallic;
    float  roughness;   // 下限 0.04 適用済み
    float3 F0;
    bool   lambert;     // 純ランバート(テスト用)
    // ---- 拡張シェーディング(フォワードの UnoBrdfCtx と同じ意味。PtFinalizeSurf が視線から作る)----
    float  F90;
    float3 msComp;        // マルチスキャッタの補償(1 = 無効)
    uint   flags;         // UNO_EXT_COAT | UNO_EXT_ANISO | UNO_EXT_SHEEN
    float  anisoStrength; float anisoRotation;      // 材質の値(PtEvalMaterial が入れる)
    float3 anisoT; float3 anisoB; float at; float ab;
    float3 sheenColor; float sheenRoughness; float sheenScaleV;
    float  coat; float coatRoughness; float3 Nc; float coatAttenV; float coatMs; float pCoat;
};

// 8 DWORD の拡張を PtSurf の材質値へ（視線に依る量は PtFinalizeSurf）。誘電体の F0 / F90 もここで直す。
// ★albedo / metallic / roughness / lambert を入れてから呼ぶこと（F0 を作り直す）。
void PtApplyShadingExt(inout PtSurf s, uint4 e0, uint4 e1)
{
    s.F90 = 1.0; s.msComp = float3(1.0, 1.0, 1.0); s.flags = 0u;
    s.anisoStrength = 0.0; s.anisoRotation = 0.0;
    s.anisoT = float3(1, 0, 0); s.anisoB = float3(0, 1, 0); s.at = 1.0; s.ab = 1.0;
    s.sheenColor = 0.0.xxx; s.sheenRoughness = 0.0; s.sheenScaleV = 1.0;
    s.coat = 0.0; s.coatRoughness = 0.04; s.Nc = float3(0, 0, 1); s.coatAttenV = 1.0; s.coatMs = 1.0; s.pCoat = 0.0;
    UnoSurface u = UnoSurfaceDefault();
    UnoApplyShadingExt(u, e0, e1);
    // 誘電体の F0（フォワード: lerp(0.08·specular·specularTint, albedo, metallic)）。既定なら 0.04 のまま
    s.F0  = lerp((0.08 * u.specular) * u.specularTint, s.albedo, s.metallic);
    s.F90 = lerp(u.specularF90, 1.0, s.metallic);
    if (u.clearCoat > 0.0)
    {
        s.flags |= UNO_EXT_COAT;
        s.coat = saturate(u.clearCoat);
        s.coatRoughness = max(saturate(u.clearCoatRoughness), 0.04);
    }
    if (u.anisotropy != 0.0)
    {
        s.flags |= UNO_EXT_ANISO;
        s.anisoStrength = u.anisotropy;
        s.anisoRotation = u.anisotropyRotation;
    }
    if (any(u.sheenColor > 0.0))
    {
        s.flags |= UNO_EXT_SHEEN;
        s.sheenColor = saturate(u.sheenColor);
        s.sheenRoughness = clamp(u.sheenRoughness, 0.07, 1.0);
    }
}

float4 PtSampleLut(float NoV, float r)
{
    Texture2D<float4> lut = ResourceDescriptorHeap[gBrdfLutSrv];
    return lut.SampleLevel(gSampClamp, float2(NoV, r), 0);
}

// 視線に依る量（フォワードの UnoShadeLighting と同じ式）。N = シェーディング法線（視線側）/ Nv = 頂点法線（コート用）
// T / Tw = 頂点の接線と符号（異方性の向きの基準）
void PtFinalizeSurf(inout PtSurf s, float3 N, float3 V, float3 Nv, float3 T, float Tw)
{
    if (s.lambert) return;
    const bool lut = (gBrdfLutSrv != PT_NO_INDEX);
    const float NoV = max(dot(N, V), 0.0);
    float rMs = s.roughness;
    float nvMs = NoV;
    if ((s.flags & UNO_EXT_ANISO) != 0u)
    {
        float3 aT, aB;
        UnoBxAnisoFrame(N, T, Tw, s.anisoRotation, s.anisoStrength, aT, aB);
        s.anisoT = aT; s.anisoB = aB;
        const float2 aa = UnoBxAnisoAlpha(s.roughness, s.anisoStrength);
        s.at = aa.x; s.ab = aa.y;
        rMs = UnoBxAnisoEqRoughness(aa.x, aa.y);
        nvMs = UnoBxAnisoMsNoV(NoV, aa.x, aa.y);
    }
    // マルチスキャッタ: PT の BSDF は直接光と同じ G（k = (r+1)²/8）なので LUT .a の E で割る（フォワードの直接光と同じ）
    if (lut && (gFlags & PT_FLAG_MULTISCATTER) != 0u)
        s.msComp = UnoBxMultiScatterComp(s.F0, PtSampleLut(nvMs, rMs).a);
    if ((s.flags & UNO_EXT_SHEEN) != 0u && lut)
        s.sheenScaleV = 1.0 - UnoBxMax3(s.sheenColor) * PtSampleLut(NoV, s.sheenRoughness).b;
    if ((s.flags & UNO_EXT_COAT) != 0u)
    {
        float3 Nc = normalize(Nv);
        if (dot(Nc, N) < 0.0) Nc = -Nc;
        s.Nc = Nc;
        const float NcV = max(dot(Nc, V), 0.0);
        const float Fcv = 0.04 + 0.96 * pow(saturate(1.0 - NcV), 5.0);
        s.coatAttenV = 1.0 - s.coat * Fcv;
        if (lut && (gFlags & PT_FLAG_MULTISCATTER) != 0u)
        {
            s.coatMs = UnoBxMultiScatterComp(float3(0.04, 0.04, 0.04), PtSampleLut(NcV, s.coatRoughness).a).x;
        }
        // コートの葉を選ぶ確率（重要度サンプリングの効率だけに効く）
        s.pCoat = clamp(s.coat * Fcv * 2.0, 0.1, 0.9);
    }
}

// f(V, L) · (N·L)（コートの葉は自分の法線の N_c·L）。フォワードの UnoShadePunctualCtx(…, radiance = 1) と同じ値。
float3 PtEvalBrdfCos(float3 N, float3 V, float3 L, PtSurf s)
{
    const float NdotL = max(dot(N, L), 0.0);
    if (s.lambert) return s.albedo * PT_INV_PI * NdotL;
    const float3 H     = normalize(V + L);
    const float  NdotV = max(dot(N, V), 0.001);
    const float  NdotH = max(dot(N, H), 0.0);
    const float  VdotH = max(dot(H, V), 0.0);
    const float3 F     = UnoBxFresnel(s.F0, s.F90, VdotH);
    float3 spec;
    if ((s.flags & UNO_EXT_ANISO) != 0u)
    {
        const float D = UnoBxD_GGXAniso(NdotH, dot(s.anisoT, H), dot(s.anisoB, H), s.at, s.ab);
        const float G = UnoBxG1Aniso(max(dot(N, V), 0.0), dot(s.anisoT, V), dot(s.anisoB, V), s.at, s.ab)
                      * UnoBxG1Aniso(NdotL, dot(s.anisoT, L), dot(s.anisoB, L), s.at, s.ab);
        spec = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    }
    else
    {
        const float NDF = PtD_GGX(NdotH, s.roughness);
        const float G   = PtG1(max(dot(N, V), 0.0), s.roughness) * PtG1(NdotL, s.roughness);
        spec = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    }
    const float3 kD = (1.0 - F) * (1.0 - s.metallic);
    float3 col = kD * s.albedo * PT_INV_PI + spec * s.msComp;
    if ((s.flags & UNO_EXT_SHEEN) != 0u)
    {
        const float Ds = UnoBxD_Charlie(s.sheenRoughness, NdotH);
        const float Vs = UnoBxV_Charlie(NdotL, NdotV, s.sheenRoughness);
        col = col * s.sheenScaleV + s.sheenColor * (Ds * Vs);
    }
    float3 o = col * NdotL;
    if ((s.flags & UNO_EXT_COAT) != 0u)
    {
        const float NcL = max(dot(s.Nc, L), 0.0);
        const float NcV = max(dot(s.Nc, V), 0.001);
        const float NcH = max(dot(s.Nc, H), 0.0);
        const float Fc  = 0.04 + 0.96 * pow(saturate(1.0 - VdotH), 5.0);
        const float Dc  = UnoBxD_GGX(NcH, s.coatRoughness);
        const float Gc  = UnoBxG1(NcV, s.coatRoughness) * UnoBxG1(NcL, s.coatRoughness);
        const float specC = (Dc * Gc * Fc) / (4.0 * NcV * NcL + 0.0001) * s.coatMs;
        o = o * s.coatAttenV + (s.coat * specC * NcL);
    }
    return o;
}

// スペキュラ葉を選ぶ確率(拡散とスペキュラの重みの比。重要度サンプリングの効率だけに効く)
float PtSpecProb(float3 N, float3 V, PtSurf s)
{
    if (s.lambert) return 0.0;
    const float NdotV = saturate(dot(N, V));
    const float3 Fv   = PtFresnel(NdotV, s.F0);
    const float ws = PtLuma(Fv);
    const float wd = (1.0 - s.metallic) * PtLuma(s.albedo) * (1.0 - ws);
    if (wd < 1e-4) return 1.0;
    if (s.metallic > 0.999) return 1.0;
    return clamp(ws / (ws + wd), 0.05, 0.95);
}

// 半ベクトル H に対する基底のスペキュラ葉の pdf(立体角、L 側)
float PtSpecLobePdf(float3 N, float3 V, float3 H, PtSurf s)
{
    const float NdotH = max(dot(N, H), 0.0);
    const float VdotH = max(dot(V, H), 1e-4);
    float D;
    if ((s.flags & UNO_EXT_ANISO) != 0u) D = UnoBxD_GGXAniso(NdotH, dot(s.anisoT, H), dot(s.anisoB, H), s.at, s.ab);
    else                                  D = PtD_GGX(NdotH, s.roughness);
    return D * NdotH / (4.0 * VdotH);
}

// L 方向を選ぶ確率密度(立体角)。pCoat·[コート] + (1 - pCoat)·[拡散(コサイン)とスペキュラ(GGX NDF の半ベクトル)の混合]。
float PtBsdfPdf(float3 N, float3 V, float3 L, PtSurf s, float pSpec)
{
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0.0;
    float pdf = (1.0 - pSpec) * NdotL * PT_INV_PI;
    const float3 H = normalize(V + L);
    if (pSpec > 0.0)
        pdf += pSpec * PtSpecLobePdf(N, V, H, s);
    if (s.pCoat > 0.0)
    {
        const float NcH = max(dot(s.Nc, H), 0.0);
        const float VdotH = max(dot(V, H), 1e-4);
        const float pc = (dot(s.Nc, L) > 0.0) ? UnoBxD_GGX(NcH, s.coatRoughness) * NcH / (4.0 * VdotH) : 0.0;
        pdf = s.pCoat * pc + (1.0 - s.pCoat) * pdf;
    }
    return pdf;
}

// 等方 GGX の半ベクトル(Walter 2007)。局所座標(z = 法線)
float3 PtSampleGgxH(float2 u, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;
    const float cosH = sqrt((1.0 - u.x) / (1.0 + (a2 - 1.0) * u.x));
    const float sinH = sqrt(max(0.0, 1.0 - cosH * cosH));
    const float phi  = PT_TWO_PI * u.y;
    return float3(sinH * cos(phi), sinH * sin(phi), cosH);
}

// 方向をサンプルする。valid=false は下半球など。weight = f * NdotL / pdf。
bool PtSampleBsdf(float3 N, float3 V, PtSurf s, float pSpec, inout uint rng,
                  out float3 L, out float pdf, out float3 weight)
{
    L = float3(0, 0, 1); pdf = 0.0; weight = 0.0.xxx;
    const float3 u3 = float3(PtRand(rng), PtRand(rng), PtRand(rng));
    bool coatLobe = false;
    if (s.pCoat > 0.0)
        coatLobe = (PtRand(rng) < s.pCoat);   // 拡張の無い材質は乱数列を変えない（従来と同じ経路）
    if (coatLobe)
    {
        L = reflect(-V, PtToWorld(PtSampleGgxH(u3.xy, s.coatRoughness), s.Nc));
    }
    else if (u3.z < pSpec)
    {
        if ((s.flags & UNO_EXT_ANISO) != 0u)
        {
            // 異方 GGX の傾き空間サンプル(pdf_H = D·N·H)
            const float r   = sqrt(u3.x / max(1.0 - u3.x, 1e-7));
            const float phi = PT_TWO_PI * u3.y;
            const float2 slope = r * float2(s.at * cos(phi), s.ab * sin(phi));
            const float3 Hl = normalize(float3(-slope.x, -slope.y, 1.0));
            const float3 H  = Hl.x * s.anisoT + Hl.y * s.anisoB + Hl.z * N;
            L = reflect(-V, H);
        }
        else
        {
            // GGX NDF から半ベクトルをサンプル(Walter 2007)。
            L = reflect(-V, PtToWorld(PtSampleGgxH(u3.xy, s.roughness), N));
        }
    }
    else
    {
        L = PtToWorld(PtCosineHemisphere(u3.xy), N);
    }
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0) return false;
    pdf = PtBsdfPdf(N, V, L, s, pSpec);
    if (pdf <= 1e-9) return false;
    weight = PtEvalBrdfCos(N, V, L, s) / pdf;
    return true;
}

// パワーヒューリスティック(β=2)
float PtMisPower(float pa, float pb)
{
    const float a = pa * pa;
    const float b = pb * pb;
    return (a + b > 0.0) ? a / (a + b) : 0.0;
}

// ---------------------------------------------------------------------------
//  環境(キューブマップ or 一様)
// ---------------------------------------------------------------------------
#ifdef PT_ATMOSPHERE
// 物理大気(A1)。AtmosphereCommon.hlsli の AT_BINDLESS 版。名前が JobCB と衝突するもの(gCamPos / gFlags / gPad0)は
// インクルードの間だけ別名にする。パラメータはカーネルの先頭で PtAtmosphereInit() が読む(PT_FLAG_ATMOSPHERE のときだけ)。
#define AT_SAMPLER gSampClamp
#define AT_BINDLESS
#define gCamPos gAtCamPos
#define gFlags  gAtFlags
#define gPad0   gAtPad0
#include "../atmosphere/AtmosphereCommon.hlsli"
#include "../atmosphere/AtmosphereSkyRadiance.hlsli"
#undef gCamPos
#undef gFlags
#undef gPad0

void PtAtmosphereInit()
{
    if ((gFlags & PT_FLAG_ATMOSPHERE) != 0u) AtLoadParams();
}

// 一次レイのミス = カメラから見える空(太陽円盤・月・星込み。gSkyScale に skyboxIntensity 込み。背景を出さないときは gEnvBgScale = 0)
float3 PtAtmosphereBackground(float3 dir)
{
    return SkyRadiance(dir) * gEnvBgScale;
}

#endif // PT_ATMOSPHERE

float3 PtEnvRadiance(float3 dir, float scale)
{
#ifdef PT_ATMOSPHERE
    if ((gFlags & PT_FLAG_ATMOSPHERE) != 0u)
        return AtEnvRadiance(dir) * scale;
#endif
    if (gEnvCubeSrv != PT_NO_INDEX)
    {
        TextureCube<float4> cube = ResourceDescriptorHeap[gEnvCubeSrv];
        return cube.SampleLevel(gSampClamp, dir, 0).rgb * scale;
    }
    return gEnvConst * scale;
}

#endif // PT_COMMON_HLSLI
