// Lighting.hlsli - 共有ライティング定義
// PerFrame 定数バッファ（b1）と、点光源 / スポットライトの Cook-Torrance 累積。
// Forward.hlsl と ForwardSkinned.hlsl の両方から include し、レイアウトを一致させる。
// C++ 側 Application.cpp の FrameConstants と完全一致させること。

#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "PBR.hlsli"
#include "ClusterCommon.hlsli"

#define NUM_CASCADES     4
#define MAX_SHADOW_SPOT  4   // 影を落とせるスポットライトの同時上限（castShadows かつカメラ近い順）
#define MAX_SHADOW_POINT 2   // 影を落とせるポイントライトの同時上限（6面/灯）

// ★MAX_POINT_LIGHTS / MAX_SPOT_LIGHTS（各 8）は撤廃した。ライトは
//   StructuredBuffer<ClusterLight>（t13）に point/spot 統合で最大 CLUSTER_MAX_SCENE_LIGHTS 灯入り、
//   クラスタードライティング（Forward+）で 1 クラスタあたり
//   CLUSTER_MAX_LIGHTS_PER_CLUSTER 灯まで評価される。
//   影マップの本数（MAX_SHADOW_SPOT / MAX_SHADOW_POINT）は据え置き＝
//   クラスタード化してもシャドウパスの描画回数は 1 回も減らないため（別問題として切り離す）。

// スポット/ポイント影サンプリング用（Forward.hlsl / ForwardSkinned.hlsl の両方で s1 を共有するため
// ここに一元化。CSM の g_shadowMap も同サンプラーを使う。
SamplerComparisonState g_shadowSampler       : register(s1);
Texture2DArray         g_spotShadowMap       : register(t9);   // ArraySize=MAX_SHADOW_SPOT
TextureCubeArray       g_pointShadowMap      : register(t10);  // NumCubes=MAX_SHADOW_POINT

// クラスタードライティング（RootSignature::kSlotClusterSRV = slot 11 のテーブル先頭 3 本）。
// ★同じテーブルの続き 4 本（t18..t21）はデカール（計画06）の予約枠。
//   このファイルで宣言していないので DXC は t13..t15 しか要求しない。
StructuredBuffer<ClusterLight> g_clusterLights     : register(t13);
StructuredBuffer<uint>         g_clusterLightIndex : register(t14);  // 固定ストライド 128/クラスタ
StructuredBuffer<uint>         g_clusterLightCount : register(t15);

// DDGI の irradiance アトラス（同じ slot11 テーブルの 8 本目 = デカール予約枠の次）。
// ★無効時は 1x1 黒ダミーが貼られる（ディスクリプタが未初期化だとデバッグレイヤが落とす）。
//   読むかどうかは ddgiOrigin.w > 0 で判断する。s5 は DDGI 専用の LINEAR CLAMP 静的サンプラ
//   （s2 は Forward.hlsl が g_iblSampler として使っているので同じ register を二重宣言できない）。
Texture2D<float4> g_ddgiIrradiance : register(t22);
// 距離モーメント（.r=平均距離 / .g=二乗平均）。Chebyshev 可視性テスト用（段階2）。
Texture2D<float2> g_ddgiDistance   : register(t23);
SamplerState      g_ddgiSampler    : register(s5);
#include "../ddgi/DdgiCommon.hlsli"

// PerFrame constants (b1)
// CSM 対応で lightViewProj(64B) を cascadeViewProj[4](256B) + cascadeSplitsView(16B) + shadowParams(16B) へ拡張。
// IBL 対応で iblParams(16B) を追加。スポット/ポイント影対応で shadowIndex・spotShadowMatrix[]・
// spotShadowTexel/pointShadowNear を追加。コンタクトシャドウ対応で末尾に 16B を追加。
// C++ Application.cpp の FrameConstants(1536B) とバイト単位で一致させること。
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;                               // 64B  (offset   0)
    float4x4 proj;                               // 64B  (offset  64)
    float3   lightDir;        float time;        // 16B  (offset 128)
    float3   lightColor;      float ambientStrength; // 16B (offset 144)
    float4x4 cascadeViewProj[NUM_CASCADES];      // 256B (offset 160)
    float4   cascadeSplitsView;                  // 16B  (offset 416)  各カスケード遠端の view 空間深度(正値) .x..w
    float4   shadowParams;                       // 16B  (offset 432)  .x=1/shadowMapSize .y=depthBias .z=blendBand .w=showCascadeDebug
    float3   cameraPos;       float aoEnabled;   // 16B  (offset 448)  aoEnabled: 1=実AOを読む, 0=AO読まず ao=1
    uint     numPointLights;  uint  numSpotLights;   // ← 統計/デバッグ用に名前だけ残す（シェーダは読まない）
    float    spotShadowTexel; float pointShadowNear;          // 16B (offset 464)
    // ▼ クラスタードライティング 64B (offset 480)。旧 pointLights[8](256B)/spotLights[8](512B) の跡地。
    float4   clusterParams;    // (offset 480) .x=zNear .y=zFar(クラスタ用) .z=sliceScale .w=sliceBias
    float4   clusterGrid;      // (offset 496) .x=gridX .y=gridY .z=gridZ .w=クラスタード有効(1/0)
    float4   clusterViewport;  // (offset 512) .xy=ビューポート原点(RT px) .zw=(gridX/vpW, gridY/vpH)
    float4   clusterExtra;     // (offset 528) .x=総灯数 .y=maxLightsPerCluster .z=デバッグ表示 .w=予約
    // ▼ PCSS（ソフトシャドウ。計画03）16B (offset 544)。_clusterReserved の先頭 1 本を削って作った。
    //   .x = tan(太陽の角半径)。**0 なら PCSS 無効＝従来の 3x3 PCF**（絵はビット一致）
    //   .y = 半影の上限（テクセル数）  .z = 時間ディザの位相（TAA 無効時は 0）
    //   .w = ブロッカー探索半径（テクセル数）
    float4   pcssParams;                          // 16B  (offset 544)
    // ▼ DDGI（world-space 拡散間接光。計画09 Step 6 / 段階1）48B (offset 560)。
    //   _clusterReserved を 3 本削って作った＝総サイズも既存オフセットも 1 バイトも動かない。
    //   ★ddgiOrigin.w（強さ）が 0 なら g_ddgiIrradiance を一切読まず従来経路（絵はビット一致）。
    //     SSR/SSGI と違いワールド座標からの Sample なので、範囲外 Load の自動ゼロが効かない。
    float4   ddgiOrigin;                          // 16B  (offset 560) .xyz=格子の原点 .w=有効(1/0)
    float4   ddgiSpacing;                         // 16B  (offset 576) .xyz=プローブ間隔 .w=法線バイアス(m)
    float4   ddgiCounts;                          // 16B  (offset 592) .xyz=各軸のプローブ数 .w=予約
    float4   _clusterReserved[40];                // 640B (offset 608..1247) 予約（総サイズ 1536B 維持のため）
    float4x4 spotShadowMatrix[MAX_SHADOW_SPOT];   // 256B (offset 1248)
    // ▼ IBL 制御 16B (offset 1504)
    float  iblIntensity;     // IBL 拡散/反射の全体スケール
    float  maxPrefilterMip;  // prefiltered cube の最大 mip index（=4.0）
    uint   hasIBL;           // 1=IBL テクスチャ有効, 0=従来 ambient フォールバック
    float  skyboxIntensity;  // skybox 描画/反射の明るさ
    // ▼ コンタクトシャドウ制御 16B (offset 1520)
    float  contactShadowEnabled;  // 1=実テクスチャ(t11)を読む, 0=読まず 1.0（白ダミー 1x1 の範囲外 Load 対策）
    // ▼ 法線マップフィルタリング（旧 _csPad の 12B をそのまま使う＝レイアウトは 1 バイトも動かない）
    //   .x=強さ(0 で完全に恒等) .y=α に足せる量の上限 .z=幾何法線へ寄せる強さ
    float3 normalFilterParams;
};                                               // total = 1536B

// DDGI の拡散間接光。PerFrame のパラメータで包んだだけの薄いラッパ。
// 戻り値は IBL の irradiance キューブと同じ単位（コサイン加重の平均放射輝度）なので、
// ★足すのではなく「置き換える」こと。足すと同じ光を二重に数える（SSGI と同じ理屈）。
// confidence は格子の境界フェード。無効時は 0 なので lerp が恒等になる。
float3 SampleDdgi(float3 worldPos, float3 N, out float confidence)
{
    confidence = 0.0;
    if (ddgiOrigin.w <= 0.0) return float3(0.0, 0.0, 0.0);   // OFF なら 1 テクセルも読まない
    // ★intensity はここでは掛けない。BlendCS がアトラスへ書く時点で既に掛かっている
    //   （DdgiProbeUpdate.hlsl の `irradiance *= gDdgi.intensity`）。掛けると 2 乗になる。
    return DdgiSampleIrradiance(g_ddgiIrradiance, g_ddgiDistance, g_ddgiSampler, worldPos, N,
                                ddgiOrigin.xyz, ddgiSpacing.xyz,
                                uint3(ddgiCounts.xyz), ddgiSpacing.w, confidence);
}

// スポットライト影: spotShadowMatrix[idx] で射影し 3x3 PCF（CSM の SampleCascade と同じ流儀）。
float SampleSpotShadow(int idx, float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0), spotShadowMatrix[idx]);
    if (clip.w <= 0.0) return 1.0;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    float current = ndc.z - shadowParams.y;
    float texel = spotShadowTexel;
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            s += g_spotShadowMap.SampleCmpLevelZero(g_shadowSampler,
                float3(uv + float2(x, y) * texel, (float)idx), current);
    return s / 9.0;
}

// ポイントライト影: major-axis 距離から比較深度を再構成して SampleCmpLevelZero 1発。
// fromLight = worldPos - lightPos（ライトからピクセルへの方向、正規化不要）。range = ライトのrange(=far)。
float SamplePointShadow(int idx, float3 fromLight, float range)
{
    float dist = max(max(abs(fromLight.x), abs(fromLight.y)), abs(fromLight.z));
    float n = pointShadowNear;
    float f = max(range, n + 0.01);
    // 透視深度の再構成: z = f/(f-n) - (f*n)/(f-n)/dist  (D3D の 0..1 深度規約, LH)
    float current = (f / (f - n)) - (f * n) / (f - n) / max(dist, 0.0001) - shadowParams.y;
    return g_pointShadowMap.SampleCmpLevelZero(g_shadowSampler,
        float4(fromLight, (float)idx), current);
}

// 1 灯ぶんの Cook-Torrance 寄与。L は正規化済みでライトへ向かうベクトル、
// radiance はライト色（減衰・コーン・影を乗じたもの）。
float3 ShadePunctual(float3 N, float3 V, float3 L, float3 radiance,
                     float3 albedo, float3 F0, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float  NdotV = max(dot(N, V), 0.001);
    float  NdotL = max(dot(N, L), 0.0);

    float  NDF = DistributionGGX(N, H, roughness);
    float  G   = GeometrySmith(N, V, L, roughness);
    float3 F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 kD   = (1.0 - F) * (1.0 - metallic);
    float3 spec = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    return (kD * albedo / PI + spec) * radiance * NdotL;
}

// ピクセル座標 + view 空間深度 から、クラスタの線形 index を求める。
// ★svPosXY は SV_Position.xy ＝ レンダーターゲット座標。シーンは m_sceneRT の
//   サブ矩形に描かれる（エディタのビューポート）ので、タイル計算の前に必ず原点を引くこと。
//   ゲームモードは原点 (0,0) なので、ここを忘れるとエディタでだけライトが横にずれる。
uint ClusterIndexFromPixel(float2 svPosXY, float viewZ)
{
    float2 p = svPosXY - clusterViewport.xy;
    uint2  tile = (uint2)clamp(p * clusterViewport.zw,
                               float2(0.0, 0.0),
                               float2(clusterGrid.x - 1.0, clusterGrid.y - 1.0));
    uint slice = ClusterSliceFromViewZ(viewZ, clusterParams.x,
                                       clusterParams.z, clusterParams.w, clusterGrid.z);
    return ClusterLinearIndex(tile.x, tile.y, slice, (uint)clusterGrid.x, (uint)clusterGrid.y);
}

// 点光源 + スポットライトをまとめて累積する（クラスタードライティング / Forward+）。
//
// 減衰式 saturate(1-dist/range)^2 ・コーン式 saturate((cd-cosOuter)/(cosInner-cosOuter))^2 ・
// 影サンプルは旧 8 灯経路と完全に同一。dist >= range の早期 continue だけが新規だが、
// そこは減衰が 0 なので見た目は変わらない（8 灯以下のシーンは従来と同じ絵になる）。
float3 AccumulatePunctualLights(float3 N, float3 V, float3 worldPos,
                                float3 albedo, float3 F0, float metallic, float roughness,
                                float2 svPosXY)
{
    float3 Lo = 0.0;

    uint first, count;
    if (clusterGrid.w > 0.5)
    {
        // クラスタード有効
        float viewZ = mul(float4(worldPos, 1.0), view).z;   // LH: 前方 +Z
        uint  ci    = ClusterIndexFromPixel(svPosXY, viewZ);
        first = ci * (uint)clusterExtra.y;
        count = g_clusterLightCount[ci];
    }
    else
    {
        // フォールバック（正射カメラ / カメラプレビュー / サムネイル / render_clustered=0）。
        // インデックスリストを引かず先頭から総当たり。旧 8 灯より緩い（既定 64 灯）。
        first = 0xFFFFFFFFu;
        count = min((uint)clusterExtra.x, 64u);
    }

    [loop]
    for (uint k = 0; k < count; ++k)
    {
        // ★HLSL の ?: は短絡しない（両辺が評価され得る）ので、フォールバック時に
        //   g_clusterLightIndex[0xFFFFFFFF + k] を踏まないよう添字の方を先に丸める。
        //   構造化バッファは範囲外読みが 0 を返すだけだが、GPU ベース検証が騒ぐので避ける。
        uint listIdx = (first == 0xFFFFFFFFu) ? 0u : (first + k);
        uint li      = (first == 0xFFFFFFFFu) ? k  : g_clusterLightIndex[listIdx];
        ClusterLight L = g_clusterLights[li];

        float3 d    = L.position - worldPos;
        float  dist = length(d);
        if (dist >= L.range) continue;              // 減衰 0。影サンプルを丸ごと省く
        float3 Ldir = d / max(dist, 0.0001);

        float att = saturate(1.0 - dist / L.range);
        att *= att;

        float3 radiance = L.color * att;

        if (L.type > 0.5)   // spot
        {
            // コーン減衰: スポット軸と (-Ldir) の角度を inner..outer でフェード
            float cd   = dot(L.direction, -Ldir);
            float cone = saturate((cd - L.cosOuter) / max(L.cosInner - L.cosOuter, 0.001));
            cone *= cone;
            if (cone <= 0.0) continue;
            radiance *= cone;
            if (L.shadowIndex >= 0.0)
                radiance *= SampleSpotShadow((int)L.shadowIndex, worldPos);
        }
        else                // point
        {
            if (L.shadowIndex >= 0.0)
                radiance *= SamplePointShadow((int)L.shadowIndex, -d, L.range);
        }

        Lo += ShadePunctual(N, V, Ldir, radiance, albedo, F0, metallic, roughness);
    }

    return Lo;
}

// クラスタのデバッグ可視化（clusterExtra.z: 0=off / 1=ライト複雑度ヒートマップ / 2=クラスタ境界）。
// CSM のカスケード可視化（shadowParams.w）と同じ流儀で、最終カラーへ後掛けする。
float3 ApplyClusterDebug(float3 color, float2 svPosXY, float3 worldPos)
{
    if (clusterExtra.z <= 0.5 || clusterGrid.w <= 0.5) return color;

    float viewZ = mul(float4(worldPos, 1.0), view).z;
    uint  ci    = ClusterIndexFromPixel(svPosXY, viewZ);

    if (clusterExtra.z < 1.5)
    {
        // ヒートマップ: 青(0灯) → 緑 → 赤(上限灯)。上限に張り付いたクラスタは白で警告。
        float n = (float)g_clusterLightCount[ci];
        float t = saturate(n / max(clusterExtra.y, 1.0));
        float3 heat = (t < 0.5) ? lerp(float3(0.0, 0.0, 0.4), float3(0.0, 0.9, 0.2), t * 2.0)
                                : lerp(float3(0.0, 0.9, 0.2), float3(1.0, 0.1, 0.0), (t - 0.5) * 2.0);
        if (n >= clusterExtra.y - 0.5) heat = float3(1.0, 1.0, 1.0);   // 128 灯で切り捨て中
        return heat;
    }

    // クラスタ境界の目視用（黄金比で散らした市松）
    return color * (0.4 + 0.6 * frac((float)ci * 0.618034));
}

#endif // LIGHTING_HLSLI
