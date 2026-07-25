// FogComposite.hlsl - パス④: 積分済みボリュームをシーン RT へ合成する
//                      （vs_6_0 / FSTriVS + ps_6_0 / PSMain）。
//
// ブレンドは SrcBlend=ONE / DestBlend=SRC_ALPHA / BlendOp=ADD なので
//   dst.rgb = src.rgb + dst.rgb * src.a
// ＝ PS が float4(in-scattering, transmittance) を返せば、それが正しいフォグ合成式そのもの。
// アルファは SrcBlendAlpha=ZERO / DestBlendAlpha=ONE でシーン RT のものを保存する。
//
// ★ビューポートはシーンのサブ矩形に設定して呼ばれる。したがって FSTriVS の uv が
//   そのままビューポートローカル UV になる（RT 座標との差を自分で引く必要がない）。
//   深度だけは RT 全面のバッファなので SV_POSITION.xy（= RT 座標）で Load する。

#include "../post/FullscreenTri.hlsli"
#include "FogCommon.hlsli"

Texture2D<float>  g_depth      : register(t0);   // R32_FLOAT（m_depthBuffer の SRV）
Texture3D<float4> g_integrated : register(t1);
SamplerState      g_linClamp   : register(s1);

// D3D の 0..1 深度 → view 空間 Z（LH 透視・非リバース Z）
float LinearizeViewZ(float d)
{
    float n = gDepthLin.x, f = gDepthLin.y;
    return (n * f) / max(f - d * (f - n), 1.0e-6);
}

float4 PSMain(FSQuadVSOut i) : SV_TARGET
{
    float d     = g_depth.Load(int3((int2)i.pos.xy, 0));
    bool  isSky = (d >= 0.999999);
    // 空（skybox）は深度 1.0 のまま残るので、ボリューム全体を通過した値が当たるようにする。
    float vz = isSky ? (gFogFar * 1000.0) : LinearizeViewZ(d);

    float  w   = ViewZToFroxelW(vz);
    float4 fog = g_integrated.SampleLevel(g_linClamp, float3(i.uv, min(w, 1.0)), 0);

    float3 inscatter     = FogSanitize(fog.rgb);
    float  transmittance = saturate(fog.a);

    // froxel ボリュームの外側（distance より遠く）を解析的な指数フォグで延長する。
    // これが無いと「ここでフォグが止まる」帯が遠景に見える。
    if (gExtend.w > 0.0 && vz > gFogFar)
    {
        float extraLen = min(vz - gFogFar, 1.0e5);
        float T2 = exp(-gExtend.w * extraLen);
        inscatter     += gExtend.xyz * (1.0 - T2) * transmittance;
        transmittance *= T2;
    }

    // デバッグ表示（gMisc.z: 1=in-scattering のみ / 2=transmittance のみ / 3=froxel スライスの縞）
    if (gMisc.z > 0.5)
    {
        if (gMisc.z < 1.5) return float4(inscatter, 0.0);          // 背景を消して散乱だけ
        if (gMisc.z < 2.5) return float4(0.0, 0.0, 0.0, transmittance);  // 減衰だけ
        float band = frac(w * FROXEL_Z);
        return float4(band.xxx * 0.5, 0.0);
    }

    return float4(inscatter, transmittance);
}
