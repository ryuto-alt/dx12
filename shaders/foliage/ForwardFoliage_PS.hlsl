// ForwardFoliage_PS.hlsl - 植生 F1: Forward.hlsl の PSMain を「距離ディザ（LOD クロスフェード / 間引きフェード）」で包んだ PS。
// -D FOLIAGE_MASK=1 で葉（アルファテスト）版。★Forward.hlsl の ALPHA_TEST は使わず、ここでミップ別カバレッジ保存つきの clip を行う。
//
// ★ライティング / 影 / フォグ / IBL / DDGI は Forward.hlsl（ForwardShade.hlsli）をそのまま使う＝既存の見た目と一致。
// ★ディザの式は全パスで同一（FoliageDitherNoise）。深度プリパス / 速度パスも同じ断片を捨てること。
#include "../forward/Forward.hlsl"
#include "FoliageCommon.hlsli"

struct FoliagePSIn
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;
    nointerpolation float2 fade : TEXCOORD5;
};

float4 FoliagePS(FoliagePSIn i) : SV_TARGET
{
    const float n = FoliageDitherNoise(i.positionSV.xy);
    if (n >= i.fade.x && n <= i.fade.y) discard;
#ifdef FOLIAGE_MASK
    {
        // 葉（アルファテスト）。Forward.hlsl の clip の代わりにここで抜く（ミップ別カバレッジ保存。深度 / 速度パスと同じ式）。
        const float2 uv = i.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;
        const float cutoff = float((pbrFlags >> 8) & 0xFF) / 255.0;
        const float lod = g_albedo.CalculateLevelOfDetail(g_sampler, uv);
        clip(FoliageMaskAlpha(g_albedo.Sample(g_sampler, uv).a, lod) - cutoff);
    }
#endif
    PSInput p;
    p.positionSV   = i.positionSV;
    p.worldPos     = i.worldPos;
    // 両面: 葉のカードは裏から見ることが多い。視線と反対を向いた法線は反転して視線側へ向ける（裏面が空色の環境光だけで暗く青く見えるのを防ぐ）。
    const float3 toEye = cameraPos - i.worldPos;
    const float flipN = (dot(i.worldNormal, toEye) < 0.0) ? -1.0 : 1.0;
    p.worldNormal  = i.worldNormal * flipN;
    p.worldTangent = i.worldTangent * flipN;
    p.tangentW     = i.tangentW;
    p.color        = i.color;
    p.texCoord     = i.texCoord;
    p.viewDepth    = i.viewDepth;
    return PSMain(p);
}
