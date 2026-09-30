// VgGBuffer.hlsl ― 仮想ジオメトリ P4（H2）: 可視性バッファから VG 画素の速度 + G-Buffer を書く全画面パス。
//
//   深度プリパス（DepthVelocityGBuffer モード）の MRT と同じ形式・同じ式:
//     SV_Target0 = 速度 R16G16_FLOAT = (今の NDC − 前の NDC) * (0.5, −0.5)。今 / 前ともジッタ無しの VP（VelocityCommon.hlsli の規約）。
//     SV_Target1 = G-Buffer R16G16B16A16_FLOAT = SS_PackGBuffer(ワールド法線, roughness, metallic)。
//       法線 = 頂点法線の補間を正規化し、裏面なら反転（プリパスの SV_IsFrontFace と同じ）。法線マップ / MR テクスチャは読まない
//       （プリパスと同じ水準。VelocityCommon.hlsli の先頭コメント）。roughness / metallic はプリパスと同じく 8bit に量子化。
//   VG でない画素は discard（プリパスが書いた値を残す）。深度は書かない（VG のラスタが既に書いている）。
//
//   ★VG の専用 RS（VgCommon.hlsli の b0 = VgCullCB / b1 = ルート定数）。ヒープはアプリの SRV ヒープ（P4 で一本化）。
#include "VgCommon.hlsli"
#include "VgVisShade.hlsli"
#include "../screenspace/ScreenSpaceCommon.hlsli"

struct VSOut { float4 pos : SV_Position; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

struct GOut
{
    float2 velocity : SV_Target0;
    float4 gbuffer  : SV_Target1;
};

GOut PSMain(VSOut i)
{
    Texture2D<uint> visTex = ResourceDescriptorHeap[gHeap2.y];
    const uint vis = visTex.Load(int3(int2(i.pos.xy), 0));
    VgVisSurface s;
    if (!VgReconstruct(vis, i.pos.xy, gViewProj, float2(gRast.yz), gHeap0.z, gOffs1.y, gHeap0.y, gHeap0.x, s))
        discard;

    GOut o;
    // 速度: 補間したアセット空間の位置を今 / 前フレームの行列で動かし、ジッタ無しの VP で投影した差。
    const float4 cur  = mul(float4(s.worldPos, 1.0), gViewProjNJ);
    const float3 prevW = VgXform(s.inst.p0, s.inst.p1, s.inst.p2, s.localPos);
    const float4 prev = mul(float4(prevW, 1.0), gPrevViewProjNJ);
    const float2 curNdc  = cur.xy  / max(abs(cur.w),  1e-6);
    const float2 prevNdc = prev.xy / max(abs(prev.w), 1e-6);
    o.velocity = (curNdc - prevNdc) * float2(0.5, -0.5);

    float3 n = normalize(s.worldNormal);
    if (!VgIsFrontFacing(s, gCamPos.xyz)) n = -n;

    const VgMaterialGpu m = VgLoadMaterial(gHeap1.w, s.asset, s.materialIndex);
    const float rough = floor(saturate(max(VgEffRoughness(m, s.inst), 0.04)) * 255.0 + 0.5) / 255.0;
    const float metal = floor(saturate(VgEffMetallic(m, s.inst)) * 255.0 + 0.5) / 255.0;
    o.gbuffer = SS_PackGBuffer(n, rough, metal);
    return o;
}
