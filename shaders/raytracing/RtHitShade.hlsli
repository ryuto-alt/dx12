#ifndef RT_HITSHADE_HLSLI
#define RT_HITSHADE_HLSLI
// RtHitShade.hlsli — レイのヒット点の shading（DDGI のプローブ更新と RT 反射が共有する）
//
// もとは shaders/ddgi/DdgiProbeUpdate.hlsl にあったヒット点の照明を、挙動を変えずにここへ移したもの。
//   ・太陽（影レイ）・点光源 / スポット（ライトグリッド or 総当たり + 影レイ）
//   ・DDGI の間接光（GI モード New: 前フレームのアトラスをヒット点で補間）
//   ・自己発光 / アルベド / 金属度・粗さの方向平均反射率（EnvBRDFApprox）
//
// ★includer が【この include より前に】用意するもの:
//     RT_GEOMETRY_REGISTER      … GeometryInfo テーブルのレジスタ（RtBindless.hlsli が使う。例 t1）
//     gTlas                     … RaytracingAccelerationStructure（影レイ用）
//     DDGI_PROBEDATA_LOAD(t)    … プローブデータ（uint2 texel → float4）を読むマクロ
//     RT_LIGHTGRID_AT(i)        … ライトグリッド（uint 配列）の i 番目を読むマクロ
//   ★cbuffer DdgiCB : register(b0) と gLinearWrap : register(s0) はこのファイルが宣言する。
//     C++ 側は DdgiVolume.cpp の DdgiCB とバイト単位で一致させること（RT 反射は同じ構造体を CBV で渡す）。
//
// ★単位（RtHitShadeGiNew の extScale / bounceScale）
//     extScale    … 外部の光（太陽・点光源・自己発光・空）に掛ける倍率。
//                   DDGI = gUnitScale（外部の単位 → 内部の単位。half の溢れ防止）/ RT 反射 = 1（シーンの単位のまま）
//     bounceScale … 前フレームのアトラス（内部の単位）の項に掛ける倍率。DDGI = 1 / RT 反射 = 1 / gUnitScale
//   DDGI は (gUnitScale, 1.0) を渡す＝移設前と同じ式。
#ifndef RT_GEOMETRY_REGISTER
#define RT_GEOMETRY_REGISTER t1
#endif
#include "RtBindless.hlsli"
#include "../ddgi/DdgiCommon.hlsli"
// ★CLUSTER_CS を define せずに include すること（あちらは define されたときだけ
//   cbuffer b0 を宣言する。define すると DdgiCB の b0 と衝突する）。
//   ここで欲しいのは ClusterLight の構造体定義だけ。Lighting.hlsli / FogScatter.hlsl と同じ作法。
#include "../forward/ClusterCommon.hlsli"

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
    // RT2: MASK(葉・柵)は非不透明の BLAS。候補ごとに α を見る（RAY_FLAG_CULL_NON_OPAQUE は付けない）。
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
    sq.TraceRayInline(gTlas, 0, 0xFF, sr);
    RT_PROCEED_WITH_ALPHA(sq, gLinearWrap);
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
            const uint  n = min(RT_LIGHTGRID_AT(baseI), DdgiPlanU(3));
            [loop]
            for (uint k = 0; k < n; ++k)
                sum += DdgiShadeLight(lights[RT_LIGHTGRID_AT(baseI + 1u + k)], posWS, nWS);
            return sum;
        }
        if (gLightCount > DdgiPlanU(28)) return sum;   // グリッド外 + 灯が多い: 寄与 0（遠すぎて効かない）
    }

    [loop]
    for (uint i = 0; i < gLightCount; ++i)
        sum += DdgiShadeLight(lights[i], posWS, nWS);
    return sum;
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
//  GI モード New のヒット点の放射輝度（DDGI のプローブレイ / RT 反射）
// ---------------------------------------------------------------------------
// h = RtLoadHit の結果（valid かつ表面）/ dir = レイ方向 / oldSky = 検証用（旧の空の項）
// reflMode = RT 反射から呼ぶとき true。滑らかな金属・誘電体は「太陽・灯の直接光を鏡面の方向アルベド分だけ白く返す」近似が
//   鏡面反射の中の鏡（クローム球が太陽に照らされた白い球に見える）を作るので、粗さに応じて直接光の鏡面分を落とす。
//   DDGI は false（移設前と同じ式）。
float3 RtHitShadeGiNew(RtHitInfo h, float3 dir, bool oldSky, float extScale, float bounceScale, bool reflMode)
{
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
    float3 radiance;
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
                if (DdgiPlanMode())
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
            const float3 skyTerm = oldSky ? DdgiSkyRadiance(h.worldNormal) * extScale : 0.0.xxx;   // 検証用 stage 1/2 のときだけ旧の空の項
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
            // 直接光に掛ける反射率。reflMode のときだけ、滑らかな面の鏡面分（hitEs）を落とす（DDGI = false は albedoE そのもの）。
            const float3 albedoD = reflMode ? (albedoE - hitEs * (1.0 - smoothstep(0.1, 0.6, hitRough))) : albedoE;
            radiance = albedoD * ((gSunColor * (gSunIntensity * ndotl * shadow)
                                   + DdgiPunctualIrradiance(h.worldPos, h.worldNormal)) * (kInvPi * extScale)
                                  + skyTerm)
                     + min(albedoE, 0.9.xxx) * (bounceNew * bounceScale)
                     + RtHitEmissive(h, gLinearWrap) * extScale;
    }
    return radiance;
}

// ---------------------------------------------------------------------------
//  GI なし（GI モード Legacy / DDGI オフ）の RT 反射用ヒット点の放射輝度
//  GI モード New と同じ材質モデル（albedoE）で、間接光の代わりに空の irradiance（IBL）を足す。
//  skyScale = 空に掛ける倍率（IBL なら iblIntensity、環境光のスカラーなら 1）。単位はシーンのまま。
// ---------------------------------------------------------------------------
float3 RtHitShadeFlat(RtHitInfo h, float extScale, float skyScale)
{
    const float3 albedo = RtHitAlbedo(h, gLinearWrap);
    const float ndotl = saturate(dot(h.worldNormal, -gSunDir));
    float shadow = 0.0;
    if (ndotl > 0.0)
        shadow = DdgiShadowRay(h.worldPos, h.worldNormal, -gSunDir, 1e5);
    float hitMetal, hitRough;
    RtHitMetalRough(h, gLinearWrap, hitMetal, hitRough);
    const float3 hitF0 = lerp(0.04.xxx, albedo, hitMetal);
    const float3 hitEs = 0.25 * (DdgiEnvBrdfApprox(hitF0, hitRough, 0.3536) + DdgiEnvBrdfApprox(hitF0, hitRough, 0.6124)
                               + DdgiEnvBrdfApprox(hitF0, hitRough, 0.7906) + DdgiEnvBrdfApprox(hitF0, hitRough, 0.9354));
    const float3 albedoE = albedo * ((1.0.xxx - hitF0) * (1.0 - hitMetal)) + hitEs;
    const float kInvPi = 0.31830988618;
    const float3 albedoD = albedoE - hitEs * (1.0 - smoothstep(0.1, 0.6, hitRough));   // 反射の中の鏡: 直接光の鏡面分を落とす
    return albedoD * ((gSunColor * (gSunIntensity * ndotl * shadow)
                       + DdgiPunctualIrradiance(h.worldPos, h.worldNormal)) * (kInvPi * extScale)
                      + DdgiSkyRadiance(h.worldNormal) * (skyScale * extScale))
         + RtHitEmissive(h, gLinearWrap) * extScale;
}

#endif // RT_HITSHADE_HLSLI
