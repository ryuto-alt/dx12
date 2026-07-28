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
    uint2         _pad45;
};

// レイの結果。x = レイ番号 / y = プローブ番号。rgb = 放射輝度 / a = ヒット距離（ミスは負）
RWTexture2D<float4> gRayData    : register(u0);
// 八面体 irradiance アトラス（ボーダー込みのタイル配置）
RWTexture2D<float4> gIrradiance : register(u1);

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
        radiance = albedo * (gSunColor * (gSunIntensity * ndotl * shadow)
                             + DdgiSkyRadiance(h.worldNormal)
                             + DdgiPunctualIrradiance(h.worldPos, h.worldNormal));
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

    // アトラス上の書き込み先（タイルのボーダーを 1 テクセル空ける）。
    const uint2 tile = DdgiProbeTileOrigin(probeIndex, gDdgi.probeCounts);
    const uint2 dst  = tile + uint2(1, 1) + gtid.xy;

    // 時間ブレンド（ヒステリシス）。★これがあるからデノイザが要らない。
    // 初回（履歴が黒）は hysteresis を無視して即座に埋める。
    const float4 prev = gIrradiance[dst];
    const float  hyst = (prev.a > 0.0) ? gDdgi.hysteresis : 0.0;
    gIrradiance[dst] = float4(lerp(irradiance, prev.rgb, hyst), 1.0);
}
