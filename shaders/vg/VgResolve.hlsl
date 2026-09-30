// VgResolve.hlsl ― 仮想ジオメトリ P4（H3）: 材質 resolve。可視性バッファの VG 画素を、フォワードと同じライティングで sceneRT へ塗る。
//
//   ・全画面 1 三角形。VG でない画素（可視性 = 空）は discard（スカイ / 後から描く非 VG のフォワードに任せる）。深度テスト無し。
//   ・材質評価は Forward.hlsl の PSMain と同じ手順（アルベド × ティント → 法線マップ → MR → 自己発光）。違いは
//       - テクスチャは材質表（VgMaterialGpu）の SRV 添字から ResourceDescriptorHeap[] で引き、**SampleGrad**（解析 UV 勾配）で読む
//       - 接線は三角形の UV 勾配から作る（VgTangentFrame。assimp の CalcTangentSpace と同じ式・符号）
//       - 頂点カラーは持たない（白）
//   ・シェーディングは ForwardShade.hlsli の UnoShadeLighting → 自己発光 → UnoShadeFinish を**そのまま**呼ぶ
//     （デカール → 法線フィルタ → CSM + コンタクト影 → 平行光 → クラスタライト → SSAO / SSR / SSGI / DDGI / IBL → デバッグ）。
//     Forward.hlsl を直したら、この resolve も同じ結果になる（複製しない。VG 設計書 §2.4.8）。
//   ・画面空間微分: ForwardShade 系の ddx / ddy 2 か所（PBR.hlsli の法線フィルタ / DecalApply.hlsli のデカール UV）は
//     マクロのフックで解析値（1 画素差分）へ差し替える。
//
//   ★メイン RS で動く（G2a で HEAP_DIRECTLY_INDEXED が立っている）。レジスタは Forward.hlsl と同じ（RootSignature.cpp が正）:
//     b0 = この resolve のルート定数（40 DWORD 中 28。VgResolveConstants）/ b1 = PerFrame / t4 CSM / t5-7 IBL / t8 SSAO /
//     t9,t10 スポット・ポイント影 / t11 コンタクト / t13-15 クラスタ / t16 SSR / t17 SSGI / t18-21 デカール / t22,23 DDGI / s0-s5。
//     b2（材質のルート定数）と t0-t2,t24（材質テーブル）は使わない。
//   ★コンパイル: ps_6_6 / vs_6_6（ResourceDescriptorHeap）。

// ── 画面空間微分のフック（Lighting.hlsli → PBR.hlsli、DecalApply.hlsli より前に定義する）──
static float3 g_vgDNdx;
static float3 g_vgDNdy;
static float3 g_vgDWdx;
static float3 g_vgDWdy;
#define UNO_SHADE_DDX_N(v) (g_vgDNdx)
#define UNO_SHADE_DDY_N(v) (g_vgDNdy)
#define UNO_DECAL_DDX_W(v) (g_vgDWdx)
#define UNO_DECAL_DDY_W(v) (g_vgDWdy)

#include "../forward/Lighting.hlsli"

SamplerState g_sampler : register(s0);

Texture2DArray         g_shadowMap     : register(t4);
#include "../forward/ShadowPcss.hlsli"

TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);
SamplerState g_brdfSampler    : register(s3);

Texture2D<float> g_ssao        : register(t8);
SamplerState     g_ssaoSampler : register(s4);
Texture2D<float> g_contactShadow : register(t11);
Texture2D<float4> g_ssr  : register(t16);
Texture2D<float4> g_ssgi : register(t17);

#include "../forward/DecalApply.hlsli"
#include "../forward/ForwardShade.hlsli"
#include "VgVisShade.hlsli"

// b0 = メイン RS のオブジェクト定数（40 DWORD）を読み替える。C++ の VgResolveConstants（VgGpuTypes.h）と一致させる。
cbuffer VgResolveCB : register(b0)
{
    float4x4 gRvViewProjJ;   // ジッタ付き VP（ラスタと同じ）
    uint4    gRvHeap0;       // x = visSrv, y = workUav, z = instancesSrv, w = assetsSrv
    uint4    gRvHeap1;       // x = materialsSrv, y = 可視リストのバイト位置, z = デバッグ（0 = 通常）, w = フラグ
    float4   gRvViewport;    // x = 幅, y = 高さ, z = 1/幅, w = 1/高さ
};

struct VSOut { float4 pos : SV_Position; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 VgSampleTex(uint srv, float2 uv, float2 dx, float2 dy)
{
    Texture2D t = ResourceDescriptorHeap[NonUniformResourceIndex(srv)];
    return t.SampleGrad(g_sampler, uv, dx, dy);
}

// 重心座標を baryOff だけずらした点（= 隣の画素。同じ三角形の平面上）のシェーディング法線（法線フィルタの 1 画素差分用）
float3 VgShadingNormalAt(VgVisSurface s, VgMaterialGpu m, float3 baryOff, float2 uv, float2 uvOff,
                         float2 dUVx, float2 dUVy, float3 T, float tw, bool hasNormalMap)
{
    const float3 b = s.bary + baryOff;
    const float3 wn = s.n0 * b.x + s.n1 * b.y + s.n2 * b.z;
    if (!hasNormalMap) return normalize(wn);
    return PerturbNormal(wn, T, tw, VgSampleTex(m.normalSrv, uv + uvOff, dUVx, dUVy).rgb);
}

float4 PSMain(VSOut i) : SV_Target0
{
    Texture2D<uint> visTex = ResourceDescriptorHeap[gRvHeap0.x];
    const uint vis = visTex.Load(int3(int2(i.pos.xy), 0));
    VgVisSurface s;
    if (!VgReconstruct(vis, i.pos.xy, gRvViewProjJ, gRvViewport.xy, gRvHeap0.y, gRvHeap1.y, gRvHeap0.z, gRvHeap0.w, s))
        discard;

    const VgMaterialGpu m = VgLoadMaterial(gRvHeap1.x, s.asset, s.materialIndex);
    // UV（材質の uvScaleOffset。勾配はスケールだけ掛かる）
    const float2 uv   = s.uv * m.uvScaleOffset.xy + m.uvScaleOffset.zw;
    const float2 dUVx = s.uvDx * m.uvScaleOffset.xy;
    const float2 dUVy = s.uvDy * m.uvScaleOffset.xy;

    // ===== マテリアル評価（Forward.hlsl の PSMain と同じ手順）=====
    const float3 objectTint = float3((s.inst.packedTint >> 16) & 0xFFu,
                                     (s.inst.packedTint >>  8) & 0xFFu,
                                     (s.inst.packedTint      ) & 0xFFu) / 255.0;
    const float4 albedo4 = (m.albedoSrv != VG_NONE) ? VgSampleTex(m.albedoSrv, uv, dUVx, dUVy) : float4(1, 1, 1, 1);
    const float3 albedo = albedo4.rgb * objectTint;

    // 法線（法線マップは三角形の UV 勾配から作った接線で）
    const bool hasNormalMap = (m.flags & VG_MAT_NORMAL_TEX) != 0u && m.normalSrv != VG_NONE;
    float3 T = float3(1, 0, 0);
    float  tw = 1.0;
    float3 N;
    if (hasNormalMap)
    {
        VgTangentFrame(s, m.uvScaleOffset, normalize(s.worldNormal), T, tw);
        N = PerturbNormal(s.worldNormal, T, tw, VgSampleTex(m.normalSrv, uv, dUVx, dUVy).rgb);
    }
    else
    {
        N = normalize(s.worldNormal);
    }

    // Metallic / Roughness（テクスチャ × 係数。係数はインスタンスの上書き > 材質）
    float metallic  = VgEffMetallic(m, s.inst);
    float roughness = VgEffRoughness(m, s.inst);
    if ((m.flags & VG_MAT_MR_TEX) != 0u && m.metalRoughSrv != VG_NONE)
    {
        const float4 mr = VgSampleTex(m.metalRoughSrv, uv, dUVx, dUVy);
        roughness = mr.g * roughness;
        metallic  = mr.b * metallic;
    }

    // ===== 画面空間微分（フックへ渡す解析値）=====
    g_vgDWdx = s.worldPosDx;
    g_vgDWdy = s.worldPosDy;
    if (normalFilterParams.x > 0.0)
    {
        g_vgDNdx = VgShadingNormalAt(s, m, s.baryDx, uv, dUVx, dUVx, dUVy, T, tw, hasNormalMap) - N;
        g_vgDNdy = VgShadingNormalAt(s, m, s.baryDy, uv, dUVy, dUVx, dUVy, T, tw, hasNormalMap) - N;
    }
    else
    {
        g_vgDNdx = float3(0, 0, 0);
        g_vgDNdy = float3(0, 0, 0);
    }

    UnoSurface su = UnoSurfaceDefault();
    su.baseColor = albedo;
    su.metallic  = metallic;
    su.roughness = roughness;

    const float viewDepth = mul(float4(s.worldPos, 1.0), view).z;   // フォワード VS と同じ（LH: 前方 +z）
    const UnoShadeInput si = UnoMakeShadeInput(s.worldPos, s.worldNormal, i.pos, viewDepth);
    const float3 lit = UnoShadeLighting(su, N, si);

    // ===== 自己発光（影・AO・ライティングを通さずに足す。フォワードと同じ 8bit 量子化の値）=====
    float3 emissive = VgResolveEmissive(m, s.inst);
    if ((m.flags & VG_MAT_EMISSIVE_TEX) != 0u && m.emissiveSrv != VG_NONE && any(emissive > 0.0))
        emissive *= VgSampleTex(m.emissiveSrv, uv, dUVx, dUVy).rgb;

    return float4(UnoShadeFinish(lit + emissive, si), albedo4.a);
}
