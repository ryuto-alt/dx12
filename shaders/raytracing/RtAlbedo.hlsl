// RtAlbedo.hlsl — レイのヒット点のアルベドをそのまま出す（計画09 Step 5 の検証用）
//
// ★これが「バインドレスの配線が全部正しいこと」の証明になる。
//   ラスタライズした絵と色が一致すれば、InstanceID → GeometryInfo → VB/IB/テクスチャ の
//   経路とバリセントリック補間が全部合っているということ。ここが合っていないと
//   DDGI（Step 6 以降）のヒットシェーディングは絶対に正しくならない。
//
// 比較時に踏みやすい罠（一次情報で確認済み）:
//   - mip: Sample() は quad 内で添字が発散すると LOD が未定義。SampleLevel(...,0) で比較する
//   - sRGB: 既存の SRV をそのまま使うので自動でリニアへ変換される（作り直すと合わなくなる）
//   - LOD: BLAS は LOD0 固定、ラスタは LOD を切り替える。比較は近距離で行うこと
//   - 裏面: ラスタは背面カリング、RT は当たる。CommittedTriangleFrontFace() で判定できる
//   - 半透明は TLAS に入らない仕様なのでミスになる

#include "RtCommon.hlsli"     // 深度 / TLAS / RtParams（スクリーンパス用のバインド一式）
#include "RtBindless.hlsli"   // ヒット点の属性読み取り（gGeometry は t6）

SamplerState gLinearWrap : register(s0);

float4 PSMain(FSQuadVSOut i) : SV_TARGET
{
    const float2 px = i.pos.xy;
    if (!RtInsideViewport(px)) return float4(0, 0, 0, 1);

    // プライマリレイをカメラから飛ばす（RtDebug.hlsl と同じ作り方）。
    const float2 ndc = RtPixelToNdc(px);
    float4 far = mul(float4(ndc, 1.0, 1.0), gInvViewProj);
    const float3 dir = normalize(far.xyz / far.w - gCameraPos);

    RayDesc r;
    r.Origin    = gCameraPos;
    r.Direction = dir;
    r.TMin      = max(gZNear, 1e-3);
    r.TMax      = RT_TMAX;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    q.Proceed();

    const RtHitInfo h = RtLoadHit(q);
    if (!h.valid)
        return float4(0, 0, 0, 1);   // ミス = 黒（空・半透明・TLAS 外）

    return float4(RtHitAlbedo(h, gLinearWrap), 1);
}
