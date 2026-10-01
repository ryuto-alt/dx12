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
// プローブデータ（GI モード New: 再配置オフセット xyz + 状態 w）。1 テクセル = 1 プローブ。
// ★DdgiCommon.hlsli の New 用の補間（DdgiComputeTaps）がこのマクロで読むので include より前に置く。
RWTexture2D<float4> gProbeData : register(u3);
#define DDGI_PROBEDATA_LOAD(t) gProbeData[(t)]
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
    // ---- GI モード New だけが読む（Legacy は C++ が 0 埋め。giFlags bit0 が 0 なら従来経路）----
    uint          gGiFlags;         // bit0 = New / bit1 = 履歴リセット（今フレームは probeData を「オフセット 0・有効」とみなす）
    uint          gEnvCubeIndex;    // 空の放射輝度キューブ（0xFFFFFFFF = 無し）
    float         gSkyScale;        // ミスの放射輝度に掛ける（iblIntensity）
    float         gMinFrontDist;    // 再配置: 面からこれ未満に近いプローブは離す(m)
    float         gViewBias;        // 補間の surface bias（視線方向。m）
    float         gNormalBiasNew;   // 補間の surface bias（法線方向。m）
    float         gRelocLimit;      // 再配置オフセットの上限（間隔比）
    float         gUnitScale;       // 物理ライティング単位 → DDGI の内部単位（従来単位）への倍率。従来単位なら 1
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

// GI モード New: ミスしたレイが拾う空の【放射輝度】。パストレーサーと同じ envCube を引く
// （Legacy の DdgiSkyRadiance は irradiance キューブで、放射輝度として使うと二重にぼかしている）。
// 無ければ irradiance キュー → 環境光スカラーの順に落とす。
float3 DdgiMissRadiance(float3 dir)
{
    if (gEnvCubeIndex != 0xFFFFFFFFu)
    {
        TextureCube<float4> envCube = ResourceDescriptorHeap[gEnvCubeIndex];
        return envCube.SampleLevel(gLinearWrap, dir, 0).rgb * gSkyScale;
    }
    return DdgiSkyRadiance(dir);   // 環境光スカラー / irradiance キューブのフォールバックは iblIntensity を掛けない
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
        if ((gGiFlags & 32u) != 0u)
        {
            // GI モード New + 物理ライティング単位: フォワードの UNO_PHYSICAL_LIGHTS と同じ逆二乗 + 影響半径の窓。
            const float srcR = max(L._pad, 0.01);
            const float d2   = max(dist * dist, srcR * srcR);
            const float xr   = dist / max(L.range, 1.0e-4);
            const float win  = saturate(1.0 - xr * xr * xr * xr);
            att = win * win / d2;
        }
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

    const bool   giNew   = (gGiFlags & 1u) != 0u;
    const uint3  coord   = DdgiProbeCoord(probeIndex, gDdgi.probeCounts);
    float3       probeWS = DdgiProbePosition(coord, gDdgi);
    // GI モード New: 再配置オフセット。履歴リセットのフレームは probeData が未初期化なので読まない（0 とみなす）。
    if (giNew && (gGiFlags & 2u) == 0u)
        probeWS += gProbeData[DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts)].xyz;

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
    // bit2 = 検証用: 旧の空の項のまま（S2 手順 1〜3 の測定）
    const bool oldSky = (gGiFlags & 4u) != 0u;
    float3 radiance;
    if (giNew && !oldSky) radiance = DdgiMissRadiance(dir) * gUnitScale;   // New: 空はミスしたレイからだけ入る（外部の単位 → 内部の単位）
    else                  radiance = DdgiSkyRadiance(dir);
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
        //
        // ★★実測で却下した方針（憶測でやり直さないこと。2026-07-30）★★
        //   「この加算を lerp(DdgiSkyRadiance(n), bounceIntensity*irr, conf) へ置き換えれば、
        //     密閉部屋のプローブは壁しか見ないので空成分が入らず、
        //     『窓の無い部屋でも環境光ぶんだけ床が光る』が自動的に消える」という案を実装して測った。
        //   結果は**逆**だった。ddgi_leak.json の暗い側が段階2 比で **+2.39EV 明るくなり**、
        //   黒潰れ率が 39.7% → 0.4% まで浮いた（＝リークが悪化した）。
        //   500 フレーム追加しても -0.11EV でほぼ動かないので発散ではなく、平衡点がそこにある。
        //   理由: 多重バウンスは**漏れた光も一緒に増幅する**。薄い仕切りをプローブ格子が
        //   またいでいると、Chebyshev をすり抜けた分（`DdgiSampleIrradiance` の重み
        //   `wrap*wrap + 0.2` で真後ろのプローブが 17% 残るのが主因と思われる。未確認）が
        //   毎フレーム再注入され、定数の環境光 0.25 より高い所で釣り合ってしまう。
        //   ＝**環境項の遮蔽は多重バウンスでは解けない**。別の手（プローブごとに空可視性を
        //   蓄積する等）が要る。ちなみに Chebyshev を環境光へ流用する案も筋が悪い:
        //   あれが持つのは「プローブ→点」の可視性で「点→空」ではないうえ、距離モーメントは
        //   maxDist で飽和するので広い屋内ホールを「空が見えている」と誤判定する。
        // ★屋内はここが本体。灯りが全部 punctual なシーンでは、これが無いと
        //   プローブが太陽と空しか見ず DDGI が丸ごと 0 になる（DDGI を選んだ理由そのもの）。
        if (giNew)
        {
            // ★GI モード New: ヒット面の照明 = albedo × (直接光 + 前フレームのプローブ irradiance(ヒット点)) + emissive。
            //   空の項（DdgiSkyRadiance(h.worldNormal)）は入れない。空はミスしたレイからだけ入る
            //   （その光は irradiance アトラス経由でここの bounce に乗る）。
            //   2026-07-30 に空の項を再帰へ置き換えて失敗した原因は、プローブの分類・再配置が無く
            //   薄い壁をまたぐプローブの漏れが増幅されたこと。New は分類・再配置（ProbeDataCS）と
            //   RTXGI 流の補間（DdgiComputeTaps）を前提にしている。
            //   ★アルベドの 0.9 クランプは維持（ループゲインを 1 未満に保つ）。
            float3 bounceNew = 0.0.xxx;
            if (gPrevIrradianceSrv != 0xFFFFFFFF && gDdgi.bounceIntensity > 0.0)
            {
                Texture2D<float4> prevIrrN  = ResourceDescriptorHeap[gPrevIrradianceSrv];
                Texture2D<float2> prevDistN = ResourceDescriptorHeap[gPrevDistanceSrv];
                const DdgiTaps taps = DdgiComputeTaps(prevDistN, gLinearWrap, h.worldPos, h.worldNormal, dir,
                                                      gDdgi.originWS, gDdgi.spacing, gDdgi.probeCounts,
                                                      gViewBias, gNormalBiasNew);
                const float4 irrN = DdgiFetchTaps(prevIrrN, gLinearWrap, taps, gDdgi.probeCounts, h.worldNormal);
                bounceNew = gDdgi.bounceIntensity * taps.conf * irrN.rgb;
            }
            const float3 skyTerm = oldSky ? DdgiSkyRadiance(h.worldNormal) * gUnitScale : 0.0.xxx;   // 検証用 stage 1/2 のときだけ旧の空の項
            // ★直接光は 1/π を掛ける。フォワードの拡散は albedo/π × radiance × NdotL（ShadePunctual）で、
            //   アトラスの値は「コサイン平均放射輝度＝E/π」の単位。Legacy の式は π 倍明るいが、それは変えない。
            const float kInvPi = 0.31830988618;
            // ★外部の光（太陽・点光源・自己発光・空）だけ gUnitScale を掛ける。bounceNew は前フレームのアトラス＝
            //   すでに内部の単位。物理単位（lux / nit）の値を half の RayData へそのまま入れると溢れるので、
            //   従来単位（太陽 ≈ 3）へ写して持つ。フォワードが読む側で 1/gUnitScale を掛けて戻す。
            radiance = albedo * ((gSunColor * (gSunIntensity * ndotl * shadow)
                                  + DdgiPunctualIrradiance(h.worldPos, h.worldNormal)) * (kInvPi * gUnitScale)
                                 + skyTerm)
                     + min(albedo, 0.9.xxx) * bounceNew
                     + RtHitEmissive(h, gLinearWrap) * gUnitScale;
        }
        else
        {
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
                     + min(albedo, 0.9.xxx) * bounce
                     + RtHitEmissive(h, gLinearWrap);   // S0b: 自己発光（色 × 強度 × 発光テクスチャ）。表面が出す光なので albedo は掛けない
        }
    }

    // ★gRayData は RGBA16F ＝ half（最大 65504）。多重バウンスで値が育つので、
    //   書く直前に必ず潰す。溢れると読み戻しが +INF になり、BlendCS の二乗で INF、
    //   分散 r²-r*r が NaN になって PS まで壊れた値が届く（段階2 で実際に踏んだ形）。
    radiance = min(radiance, 1000.0.xxx);
    gRayData[uint2(rayIndex, probeIndex)] = float4(radiance, dist);
}

// ---------------------------------------------------------------------------
//  ProbeData: プローブの分類と再配置（GI モード New）。1 スレッド = 1 プローブ
//
//  RTXGI の classification / relocation と同じ考え方。TraceCS が書いた RayData（x = レイ番号 / y = プローブ）を読み、
//  probeData（xyz = 再配置オフセット / w = 状態）を更新する。TraceCS は旧オフセットで撃ち、ここが新オフセットを書く。
//    分類: 裏面ヒットの率が閾値（既定 25%。RTXGI と同じ）を超えたプローブは「壁の中」＝無効（w=0）。
//          判定のふらつきを避けるため、いま無効のプローブが有効に戻る閾値は低く（15%）してある。
//    再配置: 壁の中 → 最も近い裏面ヒットの先（面の向こう側）へ押し出す。
//            表面に近すぎる → 最も遠い表面ヒットの方向へ離す。
//            遠い（十分離れている）→ 格子点へ少しずつ戻す。
//          オフセットは格子間隔の gRelocLimit（0.45）まで。超える更新は捨てる。
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void ProbeDataCS(uint3 dtid : SV_DispatchThreadID)
{
    const uint probeIndex = dtid.x;
    const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (probeIndex >= probeTotal) return;

    const uint2  pt    = DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts);
    const bool   reset = (gGiFlags & 2u) != 0u;
    const float4 prev  = reset ? float4(0.0, 0.0, 0.0, 0.0) : gProbeData[pt];
    const float3 curOffset = prev.xyz;
    const bool   prevActive = prev.w > 0.5;

    uint   backCount = 0;
    float  closestBack = 1.0e30;   float3 closestBackDir = 0.0.xxx;
    float  closestFront = 1.0e30;
    float  farthestFront = 0.0;    float3 farthestFrontDir = 0.0.xxx;

    [loop]
    for (uint i = 0; i < DDGI_RAYS_PER_PROBE; ++i)
    {
        const float d = gRayData[uint2(i, probeIndex)].w;
        const float3 rayDir = mul(DdgiRayRotation(gDdgi.frameIndex),
                                  DdgiSphericalFibonacci(i, DDGI_RAYS_PER_PROBE));
        if (d < 0.0)
        {
            ++backCount;
            const float ad = -d;
            if (ad < closestBack) { closestBack = ad; closestBackDir = rayDir; }
        }
        else if (d < 1.0e8)   // 表面ヒット（ミスは +INF）
        {
            if (d < closestFront) closestFront = d;
            if (d > farthestFront) { farthestFront = d; farthestFrontDir = rayDir; }
        }
    }

    const float backFrac = float(backCount) / float(DDGI_RAYS_PER_PROBE);
    const bool  inside   = backFrac > (prevActive ? 0.25 : 0.15);

    // ---- 再配置 ----
    const float  minFront = gMinFrontDist;
    float3 fullOffset = curOffset;
    if (backCount > 0 && backFrac > 0.25 && closestBack < 1.0e29)
    {
        // 壁の中: 最も近い裏面ヒットの向きへ、その面を越えて少し先まで。
        fullOffset = curOffset + closestBackDir * (closestBack + minFront * 0.5);
    }
    else if (closestFront < minFront && farthestFront > 0.0)
    {
        // 表面に近すぎる: 最も遠い表面ヒットの方向へ離す（1 フレームの移動量は間隔の 1/4 まで）。
        const float stepMax = 0.25 * gDdgi.spacing.x;
        const float moveBack = min(min(farthestFront, stepMax), abs(closestFront - minFront));
        fullOffset = curOffset + farthestFrontDir * moveBack;
    }
    else if (closestFront > minFront)
    {
        // 十分離れている: 格子点へ少しずつ戻す。
        const float len = length(curOffset);
        if (len > 1.0e-5)
        {
            const float moveBack = min(closestFront - minFront, len);
            fullOffset = curOffset - (curOffset / len) * moveBack;
        }
    }
    float3 newOffset = curOffset;
    const float3 nrm = fullOffset / max(gDdgi.spacing, 1.0e-4);
    if (dot(nrm, nrm) < gRelocLimit * gRelocLimit && (gGiFlags & 8u) == 0u)   // bit3 = 検証用: 再配置しない
        newOffset = fullOffset;

    // ---- 状態 ----
    float state = 0.0;
    if (!inside || (gGiFlags & 16u) != 0u) state = (prevActive) ? 1.0 : 2.0;   // bit4 = 診断: 全プローブ有効   // 2 = 有効になった直後（Blend が hysteresis 0 で埋める）
    gProbeData[pt] = float4(newOffset, state);
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

    // GI モード New: 壁の中のプローブ（状態 0）は更新しない（フォワードも重み 0 で読まない）。
    const bool giNew = (gGiFlags & 1u) != 0u;
    float4 probeDat = float4(0.0, 0.0, 0.0, 1.0);
    if (giNew) probeDat = gProbeData[DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts)];
    const bool probeActive = probeDat.w > 0.5;

    if (interior && probeActive)
    {
        // このテクセルが代表する方向（内側の原点は tile+(1,1)）。
        const float3 texelDir = DdgiTexelDirection(uint2(t - 1), DDGI_IRRADIANCE_TEXELS);

        float3 sum = 0.0;
        float  wsum = 0.0;
        float  skySum = 0.0;   // New: 空の可視率 = このテクセル方向のコサイン重み半球で「ミス」だった割合

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
            // ミスは 1e30 で記録され、half に落ちて +INF で返る。有限の距離は rayLength(≤1e4) 以下。
            if (rd.w > 1.0e8) skySum += w;
        }

        float3 irradiance = (wsum > 0.0) ? (sum / wsum) : float3(0, 0, 0);
        irradiance *= gDdgi.intensity;

        // 時間ブレンド（ヒステリシス）。★これがあるからデノイザが要らない。
        // 初回（履歴が黒）は hysteresis を無視して即座に埋める。
        const float4 prev = gIrradiance[tile + uint2(t)];
        if (giNew)
        {
            // New: .a は空の可視率（0 になりうる）なので「履歴の有無」の目印には使えない。
            //   履歴リセットのフレームは C++ が hysteresis=0 を渡し、壁から出て有効になった直後のプローブは
            //   probeData.w == 2 で知らせる。
            const float skyVis = (wsum > 0.0) ? (skySum / wsum) : 0.0;
            const float hystN  = (probeDat.w > 1.5) ? 0.0 : gDdgi.hysteresis;
            // ★hystN == 0 は lerp にしない: 作りたてのアトラスは未初期化メモリで、NaN / Inf があると 0 * NaN = NaN で残る
            //   （起動ごとに結果が変わる原因だった）。
            if (hystN <= 0.0)
                gIrradiance[tile + uint2(t)] = float4(irradiance, skyVis);
            else
                gIrradiance[tile + uint2(t)] = float4(lerp(irradiance, prev.rgb, hystN),
                                                      lerp(skyVis, prev.a, hystN));
        }
        else
        {
            const float  hyst = (prev.a > 0.0) ? gDdgi.hysteresis : 0.0;
            gIrradiance[tile + uint2(t)] = float4(lerp(irradiance, prev.rgb, hyst), 1.0);
        }
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
        if ((gGiFlags & 1u) != 0u)
        {
            // GI モード New: 履歴リセットのフレーム（hysteresis=0）は未初期化メモリを読まず、そのまま書く。
            if (gDdgi.hysteresis <= 0.0) gDistance[tile + uint2(t)] = moments;
            else                         gDistance[tile + uint2(t)] = lerp(moments, prev, gDdgi.hysteresis);
        }
        else
        {
            const float  hyst = (prev.x > 0.0) ? gDdgi.hysteresis : 0.0;
            gDistance[tile + uint2(t)] = lerp(moments, prev, hyst);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (!interior)
    {
        const int2 src = DdgiBorderSource(t, DDGI_DISTANCE_TEXELS);
        gDistance[tile + uint2(t)] = gDistance[tile + uint2(src)];
    }
}
