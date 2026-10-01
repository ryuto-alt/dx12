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
    // GI S4（giFlags bit6 = 更新リスト方式）: 更新計画バッファ（StructuredBuffer<uint>）の bindless index。
    uint          gPlanSrv;
    uint          gPad3a;
    uint          gPad3b;
    uint          gPad3c;
};

// レイの結果。x = レイ番号 / y = プローブ番号。rgb = 放射輝度 / a = ヒット距離（ミスは負）
RWTexture2D<float4> gRayData    : register(u0);
// 八面体 irradiance アトラス（ボーダー込みのタイル配置）
RWTexture2D<float4> gIrradiance : register(u1);
// 八面体 距離モーメントアトラス（.r = 平均距離 / .g = 距離の二乗平均）。段階2。
RWTexture2D<float2> gDistance   : register(u2);
// ライトグリッド（GI S4。ヒット点の点光源を絞る）。セルごとに [個数, 灯の添字 × maxPerCell]。LightGridCS が毎フレーム作る。
RWStructuredBuffer<uint> gLightGrid : register(u4);

SamplerState gLinearWrap : register(s0);

// ---------------------------------------------------------------------------
//  更新計画（GI S4）。C++ の DdgiVolume::Update が毎フレーム UPLOAD バッファへ書く。
//   [0]=更新リストの長さ [1]=カスケード数 [2]=1 カスケードのプローブ数 [3]=ライトグリッドの 1 セルの最大灯数
//   [4..6]=カスケード 0 の scroll(int) [7]=間隔0(float) [8..10]=scroll1 [11]=間隔1 [12..14]=origin0 [15]=hyst0 [16..18]=origin1 [19]=hyst1
//   [20..22]=ライトグリッドの原点 [23]=セルの大きさ [24..26]=格子の分割数(uint) [27]=グリッドを使う(1/0) [28]=グリッド外で総当たりする灯数の上限
//   [32..]=更新リスト。要素 = プローブの通し番号 | 0x80000000（最上位ビット = 履歴を捨てて埋め直す）
//  ★更新リスト方式は GI モード New 専用（giFlags bit6）。Legacy は従来どおり dtid をそのままプローブ番号に使う。
// ---------------------------------------------------------------------------
#define DDGI_PLAN_HDR 32u
bool DdgiPlanMode() { return (gGiFlags & 64u) != 0u; }
uint DdgiPlanU(uint i)  { StructuredBuffer<uint> p = ResourceDescriptorHeap[gPlanSrv]; return p[i]; }
float DdgiPlanF(uint i) { return asfloat(DdgiPlanU(i)); }
float3 DdgiPlanF3(uint i) { return float3(DdgiPlanF(i), DdgiPlanF(i + 1), DdgiPlanF(i + 2)); }
int3   DdgiPlanI3(uint i) { return int3(asint(DdgiPlanU(i)), asint(DdgiPlanU(i + 1)), asint(DdgiPlanU(i + 2))); }

// 1 カスケードぶんの窓。
struct DdgiCasc
{
    float3 origin;
    float  spacing;
    int3   scroll;
    uint   baseIdx;
    float  hyst;      // このカスケードの実効ヒステリシス（間引いたぶん h^周期。DdgiSchedule.h の EffectiveHysteresis）
};

DdgiCasc DdgiLoadCasc(uint c)
{
    DdgiCasc k;
    const bool c1 = (c != 0u);
    k.origin  = DdgiPlanF3(c1 ? 16u : 12u);
    k.spacing = DdgiPlanF(c1 ? 11u : 7u);
    k.scroll  = DdgiPlanI3(c1 ? 8u : 4u);
    k.baseIdx = c * DdgiPlanU(2);
    k.hyst    = DdgiPlanF(c1 ? 19u : 15u);
    return k;
}

DdgiVol DdgiVolOf(DdgiCasc k)
{
    return DdgiMakeVol(k.origin, k.spacing.xxx, gDdgi.probeCounts, k.scroll, k.baseIdx);
}

// 通し番号 → 所属カスケード番号
uint DdgiCascadeOfProbe(uint probeIndex)
{
    return min(probeIndex / max(DdgiPlanU(2), 1u), max(DdgiPlanU(1), 1u) - 1u);
}

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
// 1 灯ぶん。減衰・コーン・影レイの式は従来のループ本体と同じ（総当たりとライトグリッドで共有する）。
float3 DdgiShadeLight(ClusterLight L, float3 posWS, float3 nWS)
{
    float3 d    = L.position - posWS;
    float  dist = length(d);
    if (dist >= L.range) return 0.0.xxx;               // 減衰 0。影レイを丸ごと省く
    float3 Ldir = d / max(dist, 0.0001);

    float ndotl = dot(nWS, Ldir);
    if (ndotl <= 0.0) return 0.0.xxx;                  // 裏面。影レイの前に落とす

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
    float3 radiance = L.color * att;                   // ★color は intensity 乗算済み

    if (L.type > 0.5)   // spot
    {
        float cd   = dot(L.direction, -Ldir);
        float cone = saturate((cd - L.cosOuter) / max(L.cosInner - L.cosOuter, 0.001));
        cone *= cone;
        if (cone <= 0.0) return 0.0.xxx;               // コーン外。ここも影レイの前
        radiance *= cone;
    }

    // 早期棄却を全部抜けた灯だけ影レイを 1 本。
    // ★TMax は「ライトまでの距離」。太陽の 1e5 のままにすると
    //   ライトの向こう側にある壁で遮られたことになり、全部影になる。
    return radiance * ndotl * DdgiShadowRay(posWS, nWS, Ldir, dist * 0.999);
}

float3 DdgiPunctualIrradiance(float3 posWS, float3 nWS)
{
    if (gLightSrvIndex == 0xFFFFFFFFu || gLightCount == 0) return float3(0.0, 0.0, 0.0);
    StructuredBuffer<ClusterLight> lights = ResourceDescriptorHeap[gLightSrvIndex];

    float3 sum = 0.0;
    // GI S4: ライトグリッド。ヒット点が入るセルの「影響しうる灯」だけを評価する（総当たりと同じ灯の集合。上限を超えたセルだけ強い順に打ち切り）。
    //   giStage bit4 (16) = 検証用: 総当たりへ戻す（数値比較用）。グリッドの外のヒット点は、灯が少ないシーンだけ総当たり（多いシーンは寄与 0）。
    if (DdgiPlanMode() && DdgiPlanU(27) != 0u && (gGiFlags & 128u) == 0u)
    {
        const float3 go  = DdgiPlanF3(20u);
        const float  cs  = DdgiPlanF(23u);
        const uint3  dims = uint3(DdgiPlanU(24u), DdgiPlanU(25u), DdgiPlanU(26u));
        const float3 g   = (posWS - go) / max(cs, 1.0e-4);
        if (all(g >= 0.0.xxx) && all(g < float3(dims)))
        {
            const uint3 cell = uint3(g);
            const uint  stride = 1u + DdgiPlanU(3);
            const uint  baseI = (cell.x + cell.y * dims.x + cell.z * dims.x * dims.y) * stride;
            const uint  n = min(gLightGrid[baseI], DdgiPlanU(3));
            [loop]
            for (uint k = 0; k < n; ++k)
                sum += DdgiShadeLight(lights[gLightGrid[baseI + 1u + k]], posWS, nWS);
            return sum;
        }
        if (gLightCount > DdgiPlanU(28)) return sum;   // グリッド外 + 灯が多い: 寄与 0（遠すぎて効かない）
    }

    [loop]
    for (uint i = 0; i < gLightCount; ++i)
        sum += DdgiShadeLight(lights[i], posWS, nWS);
    return sum;
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

// 鏡面の方向アルベド（Karis, "Physically Based Shading on Mobile" の EnvBRDFApprox）。GGX + Schlick の半球積分の近似で、
// PT の PtEvalBrdf（GGX NDF / Smith G / Schlick F）の「入射方向で平均した鏡面反射率」に対応する。
// 戻り値 = F0 * A + B（F0 は色つきでよい）。
float3 DdgiEnvBrdfApprox(float3 F0, float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r  = roughness * c0 + c1;
    const float  a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    const float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y.xxx;
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
                // カスケード: 更新計画の窓（固定ボリュームは cb の格子 = カスケード 1 本・scroll 0）
                DdgiVol v0, v1;
                uint nCasc = 1u;
                if (planMode)
                {
                    v0 = DdgiVolOf(DdgiLoadCasc(0u));
                    nCasc = max(DdgiPlanU(1), 1u);
                    v1 = v0;
                    if (nCasc > 1u) v1 = DdgiVolOf(DdgiLoadCasc(1u));
                }
                else
                {
                    v0 = DdgiMakeVol(gDdgi.originWS, gDdgi.spacing, gDdgi.probeCounts, int3(0, 0, 0), 0u);
                    v1 = v0;
                }
                const uint3 atlasCounts = uint3(gDdgi.probeCounts.x, gDdgi.probeCounts.y, gDdgi.probeCounts.z * nCasc);
                const DdgiNewResult sr = DdgiSampleNew(prevIrrN, prevDistN, gLinearWrap, h.worldPos, h.worldNormal, dir,
                                                       h.worldNormal, false, v0, v1, nCasc, atlasCounts,
                                                       gViewBias, gNormalBiasNew);
                bounceNew = gDdgi.bounceIntensity * sr.conf * sr.irrN;
            }
            const float3 skyTerm = oldSky ? DdgiSkyRadiance(h.worldNormal) * gUnitScale : 0.0.xxx;   // 検証用 stage 1/2 のときだけ旧の空の項
            // ★直接光は 1/π を掛ける。フォワードの拡散は albedo/π × radiance × NdotL（ShadePunctual）で、
            //   アトラスの値は「コサイン平均放射輝度＝E/π」の単位。Legacy の式は π 倍明るいが、それは変えない。
            const float kInvPi = 0.31830988618;
            // ★外部の光（太陽・点光源・自己発光・空）だけ gUnitScale を掛ける。bounceNew は前フレームのアトラス＝
            //   すでに内部の単位。物理単位（lux / nit）の値を half の RayData へそのまま入れると溢れるので、
            //   従来単位（太陽 ≈ 3）へ写して持つ。フォワードが読む側で 1/gUnitScale を掛けて戻す。
            // ★A3 修正: PT と同じ材質モデル（PtEvalMaterial / PtEvalBrdf = GGX + Schlick, kD = (1-F)(1-metallic)）の
            //   「方向平均した反射率」で返す。プローブは拡散しか持てないので、鏡面ぶんは「その面が受けた光 × 鏡面の方向アルベド」を
            //   等方に返す近似（サーフェスキャッシュと同じ考え方）。
            //     鏡面の方向アルベド Es = Karis の EnvBRDFApprox(F0, roughness, NdotV)（V = プローブレイの逆向き）
            //     拡散の係数   kD = (1 - F0) × (1 - metallic)  ← PT の kD = (1-F(V·H))(1-metallic) の半球平均。数値積分（GR の材質）で 0.951〜0.960 = 1-F0 に一致（粗さに依らない）。フォワードの拡散と同じ式
            //     反射率 R = kD × albedo + Es（金属は F0 = albedo の色つき反射になる）
            float hitMetal, hitRough;
            RtHitMetalRough(h, gLinearWrap, hitMetal, hitRough);
            const float3 hitF0 = lerp(0.04.xxx, albedo, hitMetal);
            // ★視線方向は平均する（コサイン重みの 4 点: NdotV = sqrt(u), u = 1/8, 3/8, 5/8, 7/8）。V ごとの Es をそのまま使うと、
            //   斜めのプローブレイほど Fresnel で白が増えるが、等方に返す近似では「その方向へ反射する光」は実際には少なく過大になる。
            //   半球平均の F0=0.04 では (1-F) ≈ 0.91 / 鏡面 ≈ 0.03〜0.06 で、A3 の実測フィット（拡散 0.90 + 白 0.03）と一致する。
            const float3 hitEs = 0.25 * (DdgiEnvBrdfApprox(hitF0, hitRough, 0.3536) + DdgiEnvBrdfApprox(hitF0, hitRough, 0.6124)
                                       + DdgiEnvBrdfApprox(hitF0, hitRough, 0.7906) + DdgiEnvBrdfApprox(hitF0, hitRough, 0.9354));
            const float3 albedoE = albedo * ((1.0.xxx - hitF0) * (1.0 - hitMetal)) + hitEs;
            radiance = albedoE * ((gSunColor * (gSunIntensity * ndotl * shadow)
                                   + DdgiPunctualIrradiance(h.worldPos, h.worldNormal)) * (kInvPi * gUnitScale)
                                  + skyTerm)
                     + min(albedoE, 0.9.xxx) * bounceNew
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
