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
// プローブデータ（GI モード New: 再配置オフセット xyz + 状態 w）。1 テクセル = 1 プローブ。
// ★DdgiCommon.hlsli の New 用の補間（DdgiComputeTaps）がこのマクロで読むので RtHitShade.hlsli の include より前に置く。
RWTexture2D<float4> gProbeData : register(u3);
#define DDGI_PROBEDATA_LOAD(t) gProbeData[(t)]
RaytracingAccelerationStructure gTlas : register(t0);
// ライトグリッド（GI S4。ヒット点の点光源を絞る）。セルごとに [個数, 灯の添字 × maxPerCell]。LightGridCS が毎フレーム作る。
RWStructuredBuffer<uint> gLightGrid : register(u4);
#define RT_LIGHTGRID_AT(i) gLightGrid[(i)]
// ヒット点の shading（太陽・点光源・DDGI の間接光・emissive）は RT 反射と共有。cbuffer DdgiCB もこの中。
#include "../raytracing/RtHitShade.hlsli"


// レイの結果。x = レイ番号 / y = プローブ番号。rgb = 放射輝度 / a = ヒット距離（ミスは負）
RWTexture2D<float4> gRayData    : register(u0);
// 八面体 irradiance アトラス（ボーダー込みのタイル配置）
RWTexture2D<float4> gIrradiance : register(u1);
// 八面体 距離モーメントアトラス（.r = 平均距離 / .g = 距離の二乗平均）。段階2。
RWTexture2D<float2> gDistance   : register(u2);

// 更新リストの 1 要素。戻り値 false = リスト外（何もしない）。
bool DdgiListEntry(uint slot, out uint probeIndex, out bool resetProbe)
{
    probeIndex = slot; resetProbe = false;
    if (slot >= DdgiPlanU(0)) return false;
    const uint e = DdgiPlanU(DDGI_PLAN_HDR + slot);
    probeIndex = e & 0x7FFFFFFFu;
    resetProbe = (e & 0x80000000u) != 0u;
    return true;
}

// ---------------------------------------------------------------------------
//  LightGrid: ワールド空間の粗いライトグリッドを作る（GI S4）。1 スレッド = 1 セル
//  セルの AABB と灯の球（位置・影響半径）が重なる灯を、灯の添字の昇順で並べる（総当たりと足す順序が同じ）。
//  上限（maxPerCell）を超えるセルは、セル中心での寄与（輝度 / 距離²）の大きい順に上限まで残す。
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void LightGridCS(uint3 dtid : SV_DispatchThreadID)
{
    const uint3 dims = uint3(DdgiPlanU(24u), DdgiPlanU(25u), DdgiPlanU(26u));
    const uint cells = dims.x * dims.y * dims.z;
    const uint ci = dtid.x;
    if (ci >= cells) return;
    const uint maxPer = DdgiPlanU(3);
    const uint stride = 1u + maxPer;
    const uint baseI  = ci * stride;
    if (gLightSrvIndex == 0xFFFFFFFFu || gLightCount == 0u) { gLightGrid[baseI] = 0u; return; }
    StructuredBuffer<ClusterLight> lights = ResourceDescriptorHeap[gLightSrvIndex];

    const uint3  cell = uint3(ci % dims.x, (ci / dims.x) % dims.y, ci / (dims.x * dims.y));
    const float  cs   = DdgiPlanF(23u);
    const float3 cmin = DdgiPlanF3(20u) + float3(cell) * cs;
    const float3 cmax = cmin + cs.xxx;
    const float3 cctr = cmin + 0.5 * cs;

    uint n = 0;
    [loop]
    for (uint i = 0; i < gLightCount; ++i)
    {
        const ClusterLight L = lights[i];
        const float3 q = clamp(L.position, cmin, cmax);   // 球の中心に最も近いセル内の点
        const float3 dv = L.position - q;
        if (dot(dv, dv) >= L.range * L.range) continue;    // 重ならない（DdgiShadeLight の dist >= range と同じ向き）
        if (n < maxPer)
        {
            gLightGrid[baseI + 1u + n] = i;
            ++n;
        }
        else
        {
            // 上限超え: 今の最弱と比べて強ければ入れ替える（足す順序は崩れるが、上限超えのセルだけ）。
            const float3 dc = L.position - cctr;
            const float  sNew = dot(L.color, float3(0.3, 0.6, 0.1)) / max(dot(dc, dc), 0.25);
            uint worst = 0; float sWorst = 1.0e30;
            [loop]
            for (uint k = 0; k < maxPer; ++k)
            {
                const ClusterLight M = lights[gLightGrid[baseI + 1u + k]];
                const float3 dm = M.position - cctr;
                const float  sM = dot(M.color, float3(0.3, 0.6, 0.1)) / max(dot(dm, dm), 0.25);
                if (sM < sWorst) { sWorst = sM; worst = k; }
            }
            if (sNew > sWorst) gLightGrid[baseI + 1u + worst] = i;
        }
    }
    gLightGrid[baseI] = n;
}

// ---------------------------------------------------------------------------
//  Ground: 窓の置き場所を決める地面レイ（GI S4 のカメラ追従。DdgiSchedule.h の AnchorForCascade）
//  スレッド 0 = カメラの真下 / 1 = 視線の中心。結果は xyz = ヒット点 / w = 距離（ミスは -1）。CPU が読み戻す。
//  ★α抜きの面（葉カードなど）は無視する（RAY_FLAG_CULL_NON_OPAQUE）。木の下では梢でなく地面を拾いたい。
// ---------------------------------------------------------------------------
cbuffer DdgiGroundCB : register(b1)
{
    float3 gGroundCam; float gGroundPad0;
    float3 gGroundFwd; float gGroundPad1;
};
RWStructuredBuffer<float4> gGroundOut : register(u5);

[numthreads(2, 1, 1)]
void GroundCS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= 2u) return;
    const float3 fwd = (dot(gGroundFwd, gGroundFwd) > 1.0e-8) ? normalize(gGroundFwd) : float3(0.0, 0.0, 1.0);
    RayDesc r;
    r.Origin    = gGroundCam;
    r.Direction = (dtid.x == 0u) ? float3(0.0, -1.0, 0.0) : fwd;
    r.TMin      = 0.0;
    r.TMax      = 1.0e5;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    q.Proceed();
    float4 o = float4(0.0, 0.0, 0.0, -1.0);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const float t = q.CommittedRayT();
        o = float4(r.Origin + r.Direction * t, t);
    }
    gGroundOut[dtid.x] = o;
}

// ---------------------------------------------------------------------------
//  Trace: 1 スレッド = 1 レイ
// ---------------------------------------------------------------------------
[numthreads(DDGI_RAYS_PER_PROBE, 1, 1)]
void TraceCS(uint3 dtid : SV_DispatchThreadID)
{
    const uint rayIndex   = dtid.x;
    const bool giNew      = (gGiFlags & 1u) != 0u;
    const bool planMode   = DdgiPlanMode();
    uint probeIndex = dtid.y;
    bool resetProbe = (gGiFlags & 2u) != 0u;
    if (rayIndex >= DDGI_RAYS_PER_PROBE) return;
    float3 probeWS;
    if (planMode)
    {
        // GI S4: dtid.y は更新リストの番号。プローブの通し番号 = カスケード * N + 記憶領域の番号。
        if (!DdgiListEntry(dtid.y, probeIndex, resetProbe)) return;
        const DdgiCasc kc = DdgiLoadCasc(DdgiCascadeOfProbe(probeIndex));
        const uint3 sc    = DdgiProbeCoord(probeIndex - kc.baseIdx, gDdgi.probeCounts);       // 記憶領域の座標
        const uint3 local = uint3((int3(sc) - kc.scroll + int3(gDdgi.probeCounts)) % int3(gDdgi.probeCounts));
        probeWS = kc.origin + float3(local) * kc.spacing;
    }
    else
    {
        const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
        if (probeIndex >= probeTotal) return;
        const uint3 coord = DdgiProbeCoord(probeIndex, gDdgi.probeCounts);
        probeWS = DdgiProbePosition(coord, gDdgi);
    }
    // GI モード New: 再配置オフセット。履歴リセットのプローブは probeData が未初期化（または前の位置のもの）なので読まない（0 とみなす）。
    if (giNew && !resetProbe)
        probeWS += gProbeData[DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts)].xyz;

    // 球面フィボナッチをフレームごとに回す。レイ 64 本でも時間方向で球面が埋まる。
    const float3 dir = mul(DdgiRayRotation(gDdgi.frameIndex),
                           DdgiSphericalFibonacci(rayIndex, DDGI_RAYS_PER_PROBE));

    RayDesc r;
    r.Origin    = probeWS;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = gDdgi.rayLength;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, 0, 0xFF, r);
    RT_PROCEED_WITH_ALPHA(q, gLinearWrap);   // RT2: MASK は α を見て抜く

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

        if (giNew)
        {
            // ★GI モード New のヒット点の照明は RtHitShade.hlsli の RtHitShadeGiNew へ移した（RT 反射と共有。式は移設前と同じ）。
            //   gUnitScale = 外部の光に掛ける倍率 / 1.0 = 前フレームのアトラス（内部の単位）はそのまま。
            radiance = RtHitShadeGiNew(h, dir, oldSky, gUnitScale, 1.0, false);
        }
        else
        {
            const float3 albedo = RtHitAlbedo(h, gLinearWrap);
            const float ndotl = saturate(dot(h.worldNormal, -gSunDir));
            float shadow = 0.0;
            if (ndotl > 0.0)   // 太陽は無限遠なので TMax は打ち切りなし
                shadow = DdgiShadowRay(h.worldPos, h.worldNormal, -gSunDir, 1e5);
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
    uint probeIndex = dtid.x;
    bool reset = (gGiFlags & 2u) != 0u;
    float spacingC = gDdgi.spacing.x;     // このプローブのカスケードの間隔
    float hystCap  = gDdgi.hysteresis;    // このカスケードの実効ヒステリシス（若いプローブの立ち上げの上限）
    float minFront = gMinFrontDist;
    if (DdgiPlanMode())
    {
        // GI S4: dtid.x は更新リストの番号。
        if (!DdgiListEntry(dtid.x, probeIndex, reset)) return;
        const DdgiCasc kc = DdgiLoadCasc(DdgiCascadeOfProbe(probeIndex));
        spacingC = kc.spacing;
        hystCap  = kc.hyst;
        minFront = clamp(gMinFrontDist * spacingC / max(gDdgi.spacing.x, 1.0e-4), 0.03, 0.5);
    }
    else
    {
        const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
        if (probeIndex >= probeTotal) return;
    }

    const uint2  pt    = DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts);
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
    float3 fullOffset = curOffset;
    if (backCount > 0 && backFrac > 0.25 && closestBack < 1.0e29)
    {
        // 壁の中: 最も近い裏面ヒットの向きへ、その面を越えて少し先まで。
        fullOffset = curOffset + closestBackDir * (closestBack + minFront * 0.5);
    }
    else if (closestFront < minFront && farthestFront > 0.0)
    {
        // 表面に近すぎる: 最も遠い表面ヒットの方向へ離す（1 フレームの移動量は間隔の 1/4 まで）。
        const float stepMax = 0.25 * spacingC;
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
    const float3 nrm = fullOffset / max(spacingC, 1.0e-4);
    if (dot(nrm, nrm) < gRelocLimit * gRelocLimit && (gGiFlags & 8u) == 0u)   // bit3 = 検証用: 再配置しない
        newOffset = fullOffset;

    // ---- 状態 ----
    // 0 = 無効（壁の中）/ 1 = 有効（収束済み）/ 2 + k = 有効になって k フレーム（若いプローブ。Blend が k/(k+1) のヒステリシスで平均する
    //   ＝ 最初の数十フレームは「それまでのサンプルの単純平均」になり、窓のスクロールで入ってきた列が速く落ち着く）。
    float state = 0.0;
    if (!inside || (gGiFlags & 16u) != 0u)   // bit4 = 診断: 全プローブ有効
    {
        if (!prevActive) state = 2.0;
        else if (prev.w < 1.5) state = 1.0;
        else
        {
            const float k = prev.w - 2.0 + 1.0;
            state = (k / (k + 1.0) >= hystCap) ? 1.0 : 2.0 + k;
        }
    }
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
    uint probeIndex = gid.x;
    bool resetProbe = false;
    float hystCap = gDdgi.hysteresis;
    if (DdgiPlanMode())
    {
        // GI S4: gid.x は更新リストの番号。
        if (!DdgiListEntry(gid.x, probeIndex, resetProbe)) return;
        hystCap = DdgiLoadCasc(DdgiCascadeOfProbe(probeIndex)).hyst;
    }
    else
    {
        const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
        if (probeIndex >= probeTotal)
            return;
    }

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
            // 若いプローブ（probeData.w = 2 + k）は k/(k+1) で平均する（k=0 は hysteresis 0 ＝ そのまま書く）。上限はこのカスケードの実効値。
            float hystN = hystCap;
            if (probeDat.w > 1.5) { const float k = probeDat.w - 2.0; hystN = min(hystCap, k / (k + 1.0)); }
            if (resetProbe) hystN = 0.0;
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
    uint probeIndex = gid.x;
    bool resetProbe = false;
    float hystCap = gDdgi.hysteresis;
    float3 spacingC = gDdgi.spacing;
    if (DdgiPlanMode())
    {
        // GI S4: gid.x は更新リストの番号。
        if (!DdgiListEntry(gid.x, probeIndex, resetProbe)) return;
        const DdgiCasc kc = DdgiLoadCasc(DdgiCascadeOfProbe(probeIndex));
        hystCap = kc.hyst;
        spacingC = kc.spacing.xxx;
    }
    else
    {
        const uint probeTotal = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
        if (probeIndex >= probeTotal)
            return;
    }

    const uint2 tile = DdgiProbeTileOrigin(probeIndex, gDdgi.probeCounts, DDGI_DISTANCE_TILE);
    const int2  t    = int2(gtid.xy);
    const bool  interior = all(t >= 1) && all(t <= DDGI_DISTANCE_TEXELS);

    if (interior)
    {
        const float3 texelDir = DdgiTexelDirection(uint2(t - 1), DDGI_DISTANCE_TEXELS);
        const float  maxDist  = DdgiMaxProbeDistance(spacingC);

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
            // GI モード New: 履歴リセット（全体のリセットのフレーム = hysteresis 0 / スクロールで入ってきたプローブ = resetProbe）は
            //   未初期化メモリ（または前の位置の値）を読まず、そのまま書く。若いプローブは irradiance と同じ k/(k+1) の立ち上げ。
            float hystN = hystCap;
            const float4 pdv = gProbeData[DdgiProbeDataTexel(probeIndex, gDdgi.probeCounts)];
            if (pdv.w > 1.5) { const float k = pdv.w - 2.0; hystN = min(hystCap, k / (k + 1.0)); }
            if (resetProbe) hystN = 0.0;
            if (hystN <= 0.0) gDistance[tile + uint2(t)] = moments;
            else              gDistance[tile + uint2(t)] = lerp(moments, prev, hystN);
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
