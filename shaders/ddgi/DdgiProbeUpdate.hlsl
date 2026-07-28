// DdgiProbeUpdate.hlsl — DDGI のプローブ更新（計画09 Step 6）
//
// 2 パス構成（論文 §4）:
//   TraceCS … プローブごとにレイを飛ばし、ヒット点の放射輝度と距離を RayData へ書く
//   BlendCS … RayData をコサイン重みで積分して irradiance アトラスへ時間ブレンドする
//
// ★compute で RayQuery を使う。DXR 仕様が "RayQuery objects can be used in any shader stage,
//   including compute shaders, pixel shaders etc." と明記している。
//   スクリーンパスと違いプローブ更新は画面空間ではないので compute が自然
//   （PS でやるとアトラスを RT にして 1 プローブ = 1 テクセル群という不自然な配置になる）。
//
// ★ヒット点のシェーディングは計画09 Step 5 のバインドレスをそのまま流用する。
//   RtBindless.hlsli は RtCommon.hlsli を include しない作りにしてあるので、
//   スクリーンパス用のバインド（深度 / SSAO / G-Buffer）と衝突しない。

#define RT_GEOMETRY_REGISTER t1
#include "../raytracing/RtBindless.hlsli"
#include "DdgiCommon.hlsli"

RaytracingAccelerationStructure gTlas : register(t0);

cbuffer DdgiCB : register(b0)
{
    DdgiConstants gDdgi;
    float3        gSunDir;      // 太陽の「進行方向」（PerFrame の lightDir と同じ向き）
    float         gSunIntensity;
    float3        gSunColor;
    float         gPad2;
    float3        gSkyColor;    // ミス時の放射輝度。★屋内は envMap が空なのでここは黒に近い
    float         gPad3;
};

// レイの結果。x = レイ番号 / y = プローブ番号。rgb = 放射輝度 / a = ヒット距離（ミスは負）
RWTexture2D<float4> gRayData    : register(u0);
// 八面体 irradiance アトラス（ボーダー込みのタイル配置）
RWTexture2D<float4> gIrradiance : register(u1);

SamplerState gLinearWrap : register(s0);

// ---------------------------------------------------------------------------
//  Trace: 1 スレッド = 1 レイ
// ---------------------------------------------------------------------------
[numthreads(DDGI_RAYS_PER_PROBE, 1, 1)]
void TraceCS(uint3 dtid : SV_DispatchThreadID)
{
    const uint rayIndex   = dtid.x;
    const uint probeIndex = dtid.y;
    const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (rayIndex >= DDGI_RAYS_PER_PROBE || probeIndex >= probeTotal)
        return;

    const uint3  coord   = DdgiProbeCoord(probeIndex, gDdgi.probeCounts);
    const float3 probeWS = DdgiProbePosition(coord, gDdgi);

    // 球面フィボナッチをフレームごとに回す。レイ 64 本でも時間方向で球面が埋まる。
    const float3 dir = mul(DdgiRayRotation(gDdgi.frameIndex),
                           DdgiSphericalFibonacci(rayIndex, DDGI_RAYS_PER_PROBE));

    RayDesc r;
    r.Origin    = probeWS;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = gDdgi.rayLength;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    q.Proceed();

    float3 radiance = gSkyColor;   // ミス = 空
    float  dist     = -1.0;

    const RtHitInfo h = RtLoadHit(q);
    if (h.valid)
    {
        dist = q.CommittedRayT();

        // 裏面に当たったら「壁の中」なので寄与させない（距離だけ負で記録する）。
        // Chebyshev 可視性（次の段階）を入れるまでのライトリーク対策も兼ねる。
        if (!q.CommittedTriangleFrontFace())
        {
            gRayData[uint2(rayIndex, probeIndex)] = float4(0, 0, 0, -dist);
            return;
        }

        const float3 albedo = RtHitAlbedo(h, gLinearWrap);

        // 1 バウンス目の直接光だけ。影レイを 1 本飛ばす。
        // ★多重バウンス（前フレームの probe irradiance をここでサンプルする）は次の段階。
        //   費用対効果が最も高い拡張だが、まず 1 バウンスの正しさを確認してから入れる。
        const float ndotl = saturate(dot(h.worldNormal, -gSunDir));
        float shadow = 0.0;
        if (ndotl > 0.0)
        {
            RayDesc sr;
            sr.Origin    = h.worldPos + h.worldNormal * gDdgi.normalBias;
            sr.Direction = -gSunDir;
            sr.TMin      = 0.0;
            sr.TMax      = 1e5;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
                   | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                   | RAY_FLAG_CULL_NON_OPAQUE
                   | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
            sq.TraceRayInline(gTlas, 0, 0xFF, sr);
            sq.Proceed();
            shadow = (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
        }
        radiance = albedo * gSunColor * (gSunIntensity * ndotl * shadow);
    }

    gRayData[uint2(rayIndex, probeIndex)] = float4(radiance, dist);
}

// ---------------------------------------------------------------------------
//  Blend: 1 スレッド = irradiance アトラスの 1 テクセル（内側のみ）
// ---------------------------------------------------------------------------
[numthreads(DDGI_IRRADIANCE_TEXELS, DDGI_IRRADIANCE_TEXELS, 1)]
void BlendCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint probeIndex = gid.x;
    const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (probeIndex >= probeTotal)
        return;

    // このテクセルが代表する方向。
    const float3 texelDir = DdgiTexelDirection(gtid.xy);

    float3 sum = 0.0;
    float  wsum = 0.0;

    [loop]
    for (uint i = 0; i < DDGI_RAYS_PER_PROBE; ++i)
    {
        const float4 rd = gRayData[uint2(i, probeIndex)];
        if (rd.w < 0.0 && all(rd.rgb == 0.0))
            continue;   // 裏面ヒット = 無効

        const float3 rayDir = mul(DdgiRayRotation(gDdgi.frameIndex),
                                  DdgiSphericalFibonacci(i, DDGI_RAYS_PER_PROBE));
        // コサイン重み。テクセルの方向から見て裏側のレイは寄与しない。
        const float w = max(0.0, dot(texelDir, rayDir));
        if (w <= 0.0) continue;

        sum  += rd.rgb * w;
        wsum += w;
    }

    float3 irradiance = (wsum > 0.0) ? (sum / wsum) : float3(0, 0, 0);
    irradiance *= gDdgi.intensity;

    // アトラス上の書き込み先（タイルのボーダーを 1 テクセル空ける）。
    const uint2 tile = DdgiProbeTileOrigin(probeIndex, gDdgi.probeCounts);
    const uint2 dst  = tile + uint2(1, 1) + gtid.xy;

    // 時間ブレンド（ヒステリシス）。★これがあるからデノイザが要らない。
    // 初回（履歴が黒）は hysteresis を無視して即座に埋める。
    const float4 prev = gIrradiance[dst];
    const float  hyst = (prev.a > 0.0) ? gDdgi.hysteresis : 0.0;
    gIrradiance[dst] = float4(lerp(irradiance, prev.rgb, hyst), 1.0);
}
