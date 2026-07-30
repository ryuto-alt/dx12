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
// ★CLUSTER_CS を define せずに include すること（あちらは define されたときだけ
//   cbuffer b0 を宣言する。define すると DdgiCB の b0 と衝突する）。
//   ここで欲しいのは ClusterLight の構造体定義だけ。Lighting.hlsli / FogScatter.hlsl と同じ作法。
#include "../forward/ClusterCommon.hlsli"

RaytracingAccelerationStructure gTlas : register(t0);

cbuffer DdgiCB : register(b0)
{
    DdgiConstants gDdgi;
    float3        gSunDir;      // 太陽の「進行方向」（PerFrame の lightDir と同じ向き）
    float         gSunIntensity;
    float3        gSunColor;
    float         gPad2;
    float3        gSkyColor;    // ミス時の放射輝度（envMap が無いときのフォールバック）
    // 空キューブ（IBL の irradiance キューブ）の bindless index。0xFFFFFFFF で無効。
    // ★これが無いと「空の見えている面まで真っ暗になる」。段階1 で実機を見て入れた。
    uint          gSkyCubeIndex;
    uint          gLightSrvIndex;  // t13 (StructuredBuffer<ClusterLight>) の bindless index
    uint          gLightCount;     // 有効な灯数。0 なら点光源を 1 灯も評価しない
    // 段階3: 【前フレームの】irradiance / 距離アトラスを SRV として引く bindless index。
    // 0xFFFFFFFF = 履歴が無い（初回フレーム / InvalidateHistory 直後）＝バウンスしない。
    // ★アトラスはクリアしていないので、ガードを外すと未初期化メモリ（NaN あり得る）を読む。
    uint          gPrevIrradianceSrv;
    uint          gPrevDistanceSrv;
};

// レイの結果。x = レイ番号 / y = プローブ番号。rgb = 放射輝度 / a = ヒット距離（ミスは負）
RWTexture2D<float4> gRayData    : register(u0);
// 八面体 irradiance アトラス（ボーダー込みのタイル配置）
RWTexture2D<float4> gIrradiance : register(u1);
// 八面体 距離モーメントアトラス（.r = 平均距離 / .g = 距離の二乗平均）。段階2。
RWTexture2D<float2> gDistance   : register(u2);

SamplerState gLinearWrap : register(s0);

// 空の放射輝度。envMap があるならフォワードの拡散 IBL と【同じ irradiance キューブ】を
// bindless で引く。無い（屋内）なら環境光のスカラーへフォールバック。
float3 DdgiSkyRadiance(float3 dir)
{
    if (gSkyCubeIndex == 0xFFFFFFFFu) return gSkyColor;
    TextureCube<float4> skyCube = ResourceDescriptorHeap[gSkyCubeIndex];
    return skyCube.SampleLevel(gLinearWrap, dir, 0).rgb;
}

// 影レイ 1 本。遮られていれば 0、通れば 1。
// maxT は太陽なら 1e5、点光源ならライトまでの距離（そこまでしか遮蔽を探さない）。
float DdgiShadowRay(float3 posWS, float3 nWS, float3 toLight, float maxT)
{
    RayDesc sr;
    sr.Origin    = posWS + nWS * gDdgi.normalBias;
    sr.Direction = toLight;
    sr.TMin      = 0.0;
    sr.TMax      = maxT;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_CULL_NON_OPAQUE
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
    sq.TraceRayInline(gTlas, 0, 0xFF, sr);
    sq.Proceed();
    return (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// ヒット点が点光源 / スポットから受ける放射照度（拡散のみ）。
//
// ★減衰式・コーン式は Lighting.hlsli の AccumulatePunctualLights と完全同一に保つこと
//   （FogScatter.hlsl に続く 3 つ目の複製。片方だけ直すと絵が食い違う）。
// ★ここは拡散間接光を作るためのバウンス元なので Cook-Torrance は要らない。
//   ラスタ側の kD * albedo/PI * radiance * NdotL に対し、こちらは albedo を後で掛ける前提で
//   radiance * NdotL だけを返す（金属の扱いも含め、プローブは拡散近似で十分）。
// ★影はシャドウマップではなくレイで引く。DDGI の compute ルートシグネチャに t9/t10 も
//   比較サンプラも無いうえ、影マップを持てるのは spot 4 灯 / point 2 灯だけで、
//   屋内の多灯シーンでは大半が shadowIndex=-1 ＝ 壁を貫通してしまうため。
float3 DdgiPunctualIrradiance(float3 posWS, float3 nWS)
{
    if (gLightSrvIndex == 0xFFFFFFFFu || gLightCount == 0) return float3(0.0, 0.0, 0.0);
    StructuredBuffer<ClusterLight> lights = ResourceDescriptorHeap[gLightSrvIndex];

    float3 sum = 0.0;
    [loop]
    for (uint i = 0; i < gLightCount; ++i)
    {
        ClusterLight L = lights[i];

        float3 d    = L.position - posWS;
        float  dist = length(d);
        if (dist >= L.range) continue;              // 減衰 0。影レイを丸ごと省く
        float3 Ldir = d / max(dist, 0.0001);

        float ndotl = dot(nWS, Ldir);
        if (ndotl <= 0.0) continue;                 // 裏面。影レイの前に落とす

        float att = saturate(1.0 - dist / L.range);
        att *= att;
        float3 radiance = L.color * att;            // ★color は intensity 乗算済み

        if (L.type > 0.5)   // spot
        {
            float cd   = dot(L.direction, -Ldir);
            float cone = saturate((cd - L.cosOuter) / max(L.cosInner - L.cosOuter, 0.001));
            cone *= cone;
            if (cone <= 0.0) continue;              // コーン外。ここも影レイの前
            radiance *= cone;
        }

        // 早期棄却を全部抜けた灯だけ影レイを 1 本。
        // ★TMax は「ライトまでの距離」。太陽の 1e5 のままにすると
        //   ライトの向こう側にある壁で遮られたことになり、全部影になる。
        sum += radiance * ndotl * DdgiShadowRay(posWS, nWS, Ldir, dist * 0.999);
    }
    return sum;
}

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

    // ミス = 空。★envMap があるならその irradiance キューブを引く。
    //   フォワードの拡散 IBL と【同じテクスチャ】なので、空が丸見えの面では
    //   DDGI を ON にしても OFF のときと同じ値に落ち着く（＝置き換えが安全になる）。
    //   遮蔽のある所だけがバウンス光で変わる、という本来の差分だけが残る。
    //   ★iblIntensity はフォワード側で ambient 全体に掛かるので、ここでは掛けない（二重になる）。
    float3 radiance = DdgiSkyRadiance(dir);
    // ★ミスは「無限遠」。負の距離は裏面ヒット専用の印にする（下の Blend が見る）。
    //   段階0 は miss も -1 だったので、空が黒いとミスが裏面と区別できず捨てられていた。
    float dist = 1e30;

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
        if (ndotl > 0.0)   // 太陽は無限遠なので TMax は打ち切りなし
            shadow = DdgiShadowRay(h.worldPos, h.worldNormal, -gSunDir, 1e5);
        // ヒット面「自身が受けている環境光」も乗せる（＝空に照らされた床のバウンス）。
        // ★これが無いと囲われた空間の面が全部「太陽の直射ぶんだけ」になり、
        //   DDGI を ON にした瞬間にシーンが暗くなる（実機で踏んだ）。
        //   ラスタ側のフォワードが同じ面へ IBL を掛けているのと同じ扱いに揃えている＝
        //   プローブが返す放射輝度とラスタの見た目が一致する。
        //   ★ラスタ同様この環境項は遮蔽されない。厳密な可視性は段階2 の Chebyshev の仕事。
        // ★屋内はここが本体。灯りが全部 punctual なシーンでは、これが無いと
        //   プローブが太陽と空しか見ず DDGI が丸ごと 0 になる（DDGI を選んだ理由そのもの）。
        // ★段階3: 多重バウンス。ヒット点で【前フレームの】プローブを引いて、
        //   そこへ届いていた間接光を 1 段ぶん足す。これを毎フレーム繰り返すことで
        //   バウンスが積み上がる（1 フレーム 1 段。収束は hysteresis ぶん遅れる）。
        //   ★アルベドは 0.9 で潰す。収束値は E/(1-ρ·b) の幾何級数なので、
        //     白に近い面（ρ→1）を放置すると発散する。
        float3 bounce = 0.0.xxx;
        if (gPrevIrradianceSrv != 0xFFFFFFFF && gDdgi.bounceIntensity > 0.0)
        {
            Texture2D<float4> prevIrr  = ResourceDescriptorHeap[gPrevIrradianceSrv];
            Texture2D<float2> prevDist = ResourceDescriptorHeap[gPrevDistanceSrv];
            float conf = 0.0;
            const float3 irr = DdgiSampleIrradiance(prevIrr, prevDist, gLinearWrap,
                                                    h.worldPos, h.worldNormal,
                                                    gDdgi.originWS, gDdgi.spacing,
                                                    gDdgi.probeCounts, gDdgi.normalBias, conf);
            bounce = gDdgi.bounceIntensity * conf * irr;
        }

        // ★アルベドのクランプは【バウンス項だけ】に掛ける。ループゲインを 1 未満に
        //   抑えるのが目的なので、直接光側に掛けると bounceIntensity=0 でも絵が変わる。
        radiance = albedo * (gSunColor * (gSunIntensity * ndotl * shadow)
                             + DdgiSkyRadiance(h.worldNormal)
                             + DdgiPunctualIrradiance(h.worldPos, h.worldNormal))
                 + min(albedo, 0.9.xxx) * bounce;
    }

    // ★gRayData は RGBA16F ＝ half（最大 65504）。多重バウンスで値が育つので、
    //   書く直前に必ず潰す。溢れると読み戻しが +INF になり、BlendCS の二乗で INF、
    //   分散 r²-r*r が NaN になって PS まで壊れた値が届く（段階2 で実際に踏んだ形）。
    radiance = min(radiance, 1000.0.xxx);
    gRayData[uint2(rayIndex, probeIndex)] = float4(radiance, dist);
}

// ---------------------------------------------------------------------------
//  Blend: 1 スレッド = irradiance アトラスの 1 テクセル（内側のみ）
// ---------------------------------------------------------------------------
// ★numthreads はボーダー込みのタイル全体。内側スレッドが積分して書き、
//   グループ同期のあとでボーダーのスレッドが内側から折り返してコピーする（論文 §4.3）。
//   ボーダーが無いとバイリニアが隣のプローブや未初期化テクセルを舐める。
[numthreads(DDGI_PROBE_TILE, DDGI_PROBE_TILE, 1)]
void BlendCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint probeIndex = gid.x;
    const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (probeIndex >= probeTotal)
        return;

    const uint2 tile = DdgiProbeTileOrigin(probeIndex, gDdgi.probeCounts, DDGI_PROBE_TILE);
    const int2  t    = int2(gtid.xy);                  // 0..DDGI_PROBE_TILE-1
    const bool  interior = all(t >= 1) && all(t <= DDGI_IRRADIANCE_TEXELS);

    if (interior)
    {
        // このテクセルが代表する方向（内側の原点は tile+(1,1)）。
        const float3 texelDir = DdgiTexelDirection(uint2(t - 1), DDGI_IRRADIANCE_TEXELS);

        float3 sum = 0.0;
        float  wsum = 0.0;

        [loop]
        for (uint i = 0; i < DDGI_RAYS_PER_PROBE; ++i)
        {
            const float4 rd = gRayData[uint2(i, probeIndex)];
            if (rd.w < 0.0)
                continue;   // 負の距離 = 裏面ヒット（壁の中）= 無効。ミスは +1e30 なので通る

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

        // 時間ブレンド（ヒステリシス）。★これがあるからデノイザが要らない。
        // 初回（履歴が黒）は hysteresis を無視して即座に埋める。
        const float4 prev = gIrradiance[tile + uint2(t)];
        const float  hyst = (prev.a > 0.0) ? gDdgi.hysteresis : 0.0;
        gIrradiance[tile + uint2(t)] = float4(lerp(irradiance, prev.rgb, hyst), 1.0);
    }

    GroupMemoryBarrierWithGroupSync();

    if (!interior)
    {
        const int2 src = DdgiBorderSource(t, DDGI_IRRADIANCE_TEXELS);
        gIrradiance[tile + uint2(t)] = gIrradiance[tile + uint2(src)];
    }
}

// ---------------------------------------------------------------------------
//  BlendDistance: 距離モーメント（段階2）。1 グループ = 1 プローブ
//
//  Chebyshev 可視性テストのために、方向ごとの「平均距離」と「距離の二乗平均」を持つ。
//  irradiance より高い解像度（14x14）で、コサインの高次乗で重み付けする＝
//  壁の縁をぼかさずに保つ（低次だと半球全体が混ざって縁が消え、リークが残る）。
// ---------------------------------------------------------------------------
[numthreads(DDGI_DISTANCE_TILE, DDGI_DISTANCE_TILE, 1)]
void BlendDistanceCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint probeIndex = gid.x;
    const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (probeIndex >= probeTotal)
        return;

    const uint2 tile = DdgiProbeTileOrigin(probeIndex, gDdgi.probeCounts, DDGI_DISTANCE_TILE);
    const int2  t    = int2(gtid.xy);
    const bool  interior = all(t >= 1) && all(t <= DDGI_DISTANCE_TEXELS);

    if (interior)
    {
        const float3 texelDir = DdgiTexelDirection(uint2(t - 1), DDGI_DISTANCE_TEXELS);
        const float  maxDist  = DdgiMaxProbeDistance(gDdgi.spacing);

        float2 sum  = 0.0;   // (Σ d·w, Σ d²·w)
        float  wsum = 0.0;

        [loop]
        for (uint i = 0; i < DDGI_RAYS_PER_PROBE; ++i)
        {
            const float4 rd = gRayData[uint2(i, probeIndex)];

            // ★abs: 裏面ヒット（負で記録）も「そこに壁がある」ので遮蔽としては数える。
            //   ★min: ミスは +1e30 で記録されるが、RayData は R16G16B16A16_FLOAT なので
            //     読み戻すと +INF。二乗すると INF、分散 mean²-mean で NaN になり
            //     フォワード PS まで壊れた値が届く。ここで必ず潰すこと。
            const float d = min(abs(rd.w), maxDist);

            const float3 rayDir = mul(DdgiRayRotation(gDdgi.frameIndex),
                                      DdgiSphericalFibonacci(i, DDGI_RAYS_PER_PROBE));
            const float c = max(0.0, dot(texelDir, rayDir));
            if (c <= 0.0) continue;
            // 高次のコサイン重み（論文の depthSharpness）。irradiance の 1 乗と違い、
            // ほぼ真正面のレイだけを見るので壁の縁が保たれる。
            const float w = pow(c, 50.0);
            if (w <= 0.0) continue;

            sum  += float2(d, d * d) * w;
            wsum += w;
        }

        const float2 moments = (wsum > 0.0) ? (sum / wsum) : float2(maxDist, maxDist * maxDist);

        // 時間ブレンド。★irradiance と違い .a が無いので、履歴の有無は
        //   「平均距離が 0 より大きいか」で判定する（初期値は 0 クリアされていない可能性が
        //   あるので、格子を作り直したフレームは hysteresis を効かせずに埋める）。
        const float2 prev = gDistance[tile + uint2(t)];
        const float  hyst = (prev.x > 0.0) ? gDdgi.hysteresis : 0.0;
        gDistance[tile + uint2(t)] = lerp(moments, prev, hyst);
    }

    GroupMemoryBarrierWithGroupSync();

    if (!interior)
    {
        const int2 src = DdgiBorderSource(t, DDGI_DISTANCE_TEXELS);
        gDistance[tile + uint2(t)] = gDistance[tile + uint2(src)];
    }
}
