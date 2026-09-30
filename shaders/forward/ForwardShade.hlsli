// ForwardShade.hlsli - フォワードのシェーディング尾部（マテリアル評価より後ろの全部）。
//
//   Forward.hlsl / ForwardSkinned.hlsl / Terrain.hlsl が 3 か所に複製していた
//     デカール → 法線フィルタ → CSM + コンタクト影 → 平行光 → クラスタライト
//     → SSAO / SSR / SSGI / DDGI / IBL → 自己発光 → デバッグ可視化 → (LDR)
//   を 1 本の関数 UnoShadeForward に集約したもの。マテリアルグラフ（G2b の ForwardGraph.hlsl）と
//   VG resolve（VG 設計書 §2.2.6）も同じ関数を呼ぶ＝複製が増えない。
//
// ★★ 絵が 1 ビットも変わらないこと（G2a の合否）★★
//   式・演算順は元の Forward.hlsl:174-364 と同一。tools/shader_dxil_equiv.ps1 が
//   外出し前後の DXIL を比較する（Forward_PS / ForwardSkinned_PS / Terrain_PS）。
//   ここを直すときは同じスクリプトで「意図した差だけ」になっていることを確かめること。
//
// ★include 側の前提（リソース宣言は各シェーダの責務。t/s 番号は RootSignature.cpp が正。
//   宣言順を変えると DXIL のリソース ID が動くので、シェーダ側の宣言はこのファイルへ移さない）:
//     Lighting.hlsli（b1 / PBR / クラスタ / DDGI）
//     g_shadowMap(t4) → ShadowPcss.hlsli
//     g_irradianceMap(t5) g_prefilteredMap(t6) g_brdfLUT(t7) g_iblSampler(s2) g_brdfSampler(s3)
//     g_ssao(t8) g_contactShadow(t11) g_ssr(t16) g_ssgi(t17)
//     DecalApply.hlsli
//   このファイルは VS からは使われない（PS 専用の関数だけ）。
//
// ★差し替え点（既定は Forward / ForwardSkinned の挙動。地形などが include の前に #define する）:
//     UNO_SHADE_SANITIZE(x)        SSR / SSGI / DDGI の放射輝度に掛ける前処理（既定: 恒等）
//     UNO_SHADE_ROUGHNESS(r)       ラフネスの下限処理（既定: max(r, 0.04)）
//     UNO_SHADE_ROUGHNESS_BEFORE_NORMAL  定義すると局所変数の宣言順を地形の旧 PSMain に合わせる（DXIL 同値のためだけ。値は不変）
//
// ★UNO_SHADE_LITE（マテリアルグラフ G2c のプレビュー専用。既定は未定義 = 旧経路の DXIL は 1 バイトも変わらない）:
//     影（CSM / コンタクト）・クラスタライト・デカール・SSAO・SSR・SSGI・DDGI を【コンパイル時に】外す。
//     プレビューはこれらを実行時定数で全部無効にして描くので、LITE の絵は「全部無効にした通常経路」と同じ（式は同じ関数 / 同じ順）。
//     コンパイルする HLSL が小さくなるので、編集 → 見た目更新の DXC + PSO が速い。

#ifndef FORWARD_SHADE_HLSLI
#define FORWARD_SHADE_HLSLI

#include "UnoSurface.hlsli"

#ifndef UNO_SHADE_SANITIZE
#define UNO_SHADE_SANITIZE(x) (x)
#endif
#ifndef UNO_SHADE_ROUGHNESS
#define UNO_SHADE_ROUGHNESS(r) max((r), 0.04)
#endif

// ---- CSM（view 空間深度でカスケードを選ぶ。Forward / Skinned / Terrain で同一だった）----
int SelectCascade(float viewDepth)
{
    int c = NUM_CASCADES - 1;
    [unroll]
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (viewDepth <= cascadeSplitsView[i]) { c = i; break; }
    }
    return c;
}

float SampleCascade(int cascade, float3 worldPos, float2 svPos)
{
    // ★実体は ShadowPcss.hlsli（PCSS / 3x3 PCF の切替込み。フォワード系 PS で共有）
    return SampleShadowCascadeCommon(g_shadowMap, cascade, worldPos, svPos, shadowParams.y);
}

float CalcShadow(float3 worldPos, float viewDepth, float2 svPos)
{
    // 正射カメラでは CSM 無効（ComputeCascades が cascadeSplitsView=1e9・identity を設定）。
    // identity フォールバックは原点付近(uv∈[0,1])でクリップ済みシャドウマップ(クリア1.0)と
    // 比較してゴミ影を落とすため、明示的に無影(1.0)を返す。
    if (cascadeSplitsView.x > 1.0e8) return 1.0f;
    int c = SelectCascade(viewDepth);
    float shadow = SampleCascade(c, worldPos, svPos);
    // カスケード境界ブレンド(任意): 次カスケードと線形混合
    float band = shadowParams.z;
    if (band > 0.0f && c < NUM_CASCADES - 1)
    {
        float edge = cascadeSplitsView[c];
        float t = saturate((edge - viewDepth) / max(band, 1e-4));
        if (t < 1.0f)
            shadow = lerp(SampleCascade(c + 1, worldPos, svPos), shadow, t);
    }
    return shadow;
}

// ---- ピクセルの幾何入力（PSInput の型はシェーダごとに違うので、尾部が読む分だけを束ねる）----
struct UnoShadeInput
{
    float3 worldPos;
    float3 worldNormal;   // 補間された頂点法線（未正規化）。法線フィルタの幾何法線 Ng に使う
    float2 svPos;         // SV_Position.xy（クラスタ / SSAO / SSR / SSGI / デカールの画面座標）
    float  viewDepth;     // view 空間深度（正値）。カスケード選択・デカール
};

UnoShadeInput UnoMakeShadeInput(float3 worldPos, float3 worldNormal, float4 positionSV, float viewDepth)
{
    UnoShadeInput si;
    si.worldPos    = worldPos;
    si.worldNormal = worldNormal;
    si.svPos       = positionSV.xy;
    si.viewDepth   = viewDepth;
    return si;
}

// ---- 本体（3 段）------------------------------------------------------------------------
//   UnoShadeLighting : デカール → 法線フィルタ → 影 → 直接光 → 環境光。戻り値 = ambient + Lo + デカールの自己発光。
//   UnoShadeFinish   : デバッグ可視化 → (LDR)。色だけ返す（alpha は呼び出し側が最後に付ける）。
//   UnoShadeForward  : 上の 2 つを s.emissive でつないだもの。**グラフ材質はこれを呼ぶ**。
//   旧経路（Forward / ForwardSkinned）は自己発光を「ライティングの後ろで」サンプルしていた。
//   その命令順を 1 命令も動かさないために（DXIL 同値の検証）、2 段を直接呼んで間に自分で
//   emissive を足す。結果は UnoShadeForward と同じ（加算の結合順も同じ）。
//
// s : マテリアル評価の結果（baseColor / metallic / roughness / ao を使う。emissive / opacity は
//     UnoShadeForward だけが読む。予約欄と normalTS / hasNormal は無視＝法線は N で受ける）
// N : 解決済みのワールド空間シェーディング法線（デカール適用前）
float3 UnoShadeLighting(UnoSurface s, float3 normalWS, UnoShadeInput si)
{
    // ★局所変数の宣言順は旧 PSMain に合わせてある（Forward: albedo → N → metallic → roughness /
    //   地形: albedo → roughness → N → metallic）。デカールのループの phi の並びが宣言順で決まるので、
    //   入れ替えても値は変わらないが DXIL のテキストが動く（tools/shader_dxil_equiv.ps1 が「同一」と言えなくなる）。
#ifdef UNO_SHADE_ROUGHNESS_BEFORE_NORMAL
    float3 albedo    = s.baseColor;
    float  roughness = UNO_SHADE_ROUGHNESS(s.roughness);
    float3 N         = normalWS;
    float  metallic  = s.metallic;
#else
    float3 albedo    = s.baseColor;
    float3 N         = normalWS;
    float  metallic  = s.metallic;
    float  roughness = UNO_SHADE_ROUGHNESS(s.roughness);
#endif

    // ===== デカール（クラスタードフォワードデカール）=====
    // ★マテリアル確定直後・ライティング開始前に適用する。こうすると後続の
    //   ShadePunctual / AccumulatePunctualLights / IBL / 影が一切の変更なしに
    //   デカールの法線とマテリアルで動く。
    // ★デカールが 0 個のシーンでは [branch] で丸ごと飛ぶ（ddx/ddy も関数の中）。
    float3 decalEmissive = 0.0;
#ifndef UNO_SHADE_LITE
    ApplyDecals(si.worldPos, si.svPos, si.viewDepth,
                albedo, N, metallic, roughness, decalEmissive);
#endif

    // ===== 法線マップフィルタリング（分散 → ラフネス / 平均法線の復元）=====
    // ★デカールの後・ライティングの前。normalFilterParams.x=0 なら完全に恒等。
    FilterShadingNormal(N, roughness, normalize(si.worldNormal), normalFilterParams);

    // View & Light
    float3 V = normalize(cameraPos - si.worldPos);
    float3 L = normalize(-lightDir);

    // PBR Cook-Torrance BRDF
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

#ifdef UNO_SHADE_LITE
    float shadow = 1.0;
#else
    // Shadow (CSM カスケード選択 PCF)
    float shadow = CalcShadow(si.worldPos, si.viewDepth, si.svPos);

    // コンタクトシャドウ（スクリーン空間・フル解像度・同一ビューポートなのでピクセル直読み）。
    // contactShadowEnabled=0（無効/正射/フォールバック）のときは読まず 1.0。
    // 白ダミーは 1x1 のため Load(画面座標) が範囲外で 0 を返し、太陽光を全消しするのを防ぐ。
    if (contactShadowEnabled > 0.5)
        shadow = min(shadow, g_contactShadow.Load(int3(si.svPos, 0)));
#endif

    // Directional Light（影付き）
    float3 Lo = ShadePunctual(N, V, L, lightColor * shadow, albedo, F0, metallic, roughness);

    // Point Lights + Spot Lights（クラスタードライティング / Forward+。灯数上限なし）
#ifndef UNO_SHADE_LITE
    Lo += AccumulatePunctualLights(N, V, si.worldPos, albedo, F0, metallic, roughness,
                                   si.svPos);
#endif

    // SSAO（スクリーン空間 AO）: フル解像度・同一ビューポートなのでピクセル直読み。
    // aoEnabled=0（SSAO無効/正射カメラ/編集2Dビュー）のときは AO を読まず 1.0。
    // 白ダミーは 1x1 のため Load(画面座標) が範囲外で 0 を返し、環境光を黒く潰してしまうのを防ぐ。
#ifdef UNO_SHADE_LITE
    float ao = 1.0;
#else
    float ao = (aoEnabled > 0.5) ? g_ssao.Load(int3(si.svPos, 0)) : 1.0;
#endif
    ao *= s.ao;   // マテリアル AO（既定 1.0 = 恒等。地形の岩の隙間 / グラフの AmbientOcclusion 出力）

    // SSR / SSGI（無効時は 1x1 黒ダミー → Load が範囲外で 0 = 寄与ゼロ）
    //   ssr .rgb=反射放射輝度 / .a=confidence
    //   ssgi.rgb=半球の間接放射照度（IBL ミスフォールバック込み） / .a=有効度
#ifdef UNO_SHADE_LITE
    float3 ssrRgb   = 0.0;
    float3 ssgiRgb  = 0.0;
    float  ssrConf  = 0.0;
    float  ssgiConf = 0.0;
    float  ddgiConf = 0.0;
    float3 ddgiIrr  = 0.0;
#else
    float4 ssr  = g_ssr.Load(int3(si.svPos, 0));
    float4 ssgi = g_ssgi.Load(int3(si.svPos, 0));
    float3 ssrRgb  = UNO_SHADE_SANITIZE(ssr.rgb);
    float3 ssgiRgb = UNO_SHADE_SANITIZE(ssgi.rgb);
    float  ssrConf  = saturate(ssr.a);
    float  ssgiConf = saturate(ssgi.a);

    // DDGI（world-space の拡散間接光）。OFF なら ddgiConf=0 で以降の lerp が全部恒等になる。
    float  ddgiConf = 0.0;
    float3 ddgiIrr  = UNO_SHADE_SANITIZE(SampleDdgi(si.worldPos, N, ddgiConf));
#endif

    // ===== Ambient / IBL =====
    float3 ambient;
    if (hasIBL != 0u)
    {
        float3 R   = reflect(-V, N);
        float  NoV = max(dot(N, V), 0.0);
        float3 F   = FresnelSchlickRoughness(NoV, F0, roughness);
        float3 kD  = (1.0 - F) * (1.0 - metallic);

        // 拡散 IBL（irradiance）
        // ★SSGI は irradiance を「置き換える」（足さない）。SSGI はミスしたレイに
        //   IBL の irradiance を積んでいるので、足すと同じ光を二重に数えて全体が倍明るくなる。
        //   SSGI が無効/無効ピクセルでは ssgiConf=0 で完全に従来どおり。
        float3 irradiance = g_irradianceMap.SampleLevel(g_iblSampler, N, 0).rgb;
        // ★役割分担は Lumen と同じ「スクリーントレース優先 → 外したら world-space プローブ」。
        //   DDGI が envMap を置き換え、そのうえで SSGI が当てたピクセルは SSGI が勝つ。
        irradiance = lerp(irradiance, ddgiIrr, ddgiConf);
        irradiance = lerp(irradiance, ssgiRgb, ssgiConf);
        float3 diffuseIBL = irradiance * albedo;

        // 鏡面 IBL（split-sum: prefiltered * (F*scale + bias)）
        // ★SSR がヒットしていれば prefiltered をその放射輝度で置き換える（confidence で連続に）。
        float  mip = roughness * maxPrefilterMip;
        float3 prefiltered = g_prefilteredMap.SampleLevel(g_iblSampler, R, mip).rgb;
        prefiltered = lerp(prefiltered, ssrRgb, ssrConf);
        float2 envBRDF = g_brdfLUT.SampleLevel(g_brdfSampler, float2(NoV, roughness), 0).rg;
        float3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

        // ★AO の掛け方（二重計上の解消）:
        //   SSGI/SSR が担当していない分にだけ AO を掛ける。どちらも「実際に届いた光」を
        //   画面空間で積分した結果なので遮蔽は織り込み済み。そこへ AO を重ねると
        //   窪みが二重に暗くなる。
        float aoDiff = lerp(ao, 1.0, ssgiConf);
        float aoSpec = lerp(ao, 1.0, ssrConf);
        ambient = (kD * diffuseIBL * aoDiff + specularIBL * aoSpec) * iblIntensity;
    }
    else
    {
        // 従来フォールバック（ライト ambient のみ）
        // metallic は拡散反射を持たない → (1-metallic) でスケール。F0 項は環境反射の簡易近似。
        float3 ambientDiffuse  = albedo * (1.0 - metallic);
        float3 ambientSpecular = lerp(F0, ssrRgb, ssrConf);
        // 環境光へ AO を乗算（直接光は遮蔽しない）。
        ambient = ambientStrength * (ambientDiffuse + ambientSpecular) * ao;
        // ★屋内は envMap が空＝hasIBL=0 なので、ここが DDGI の本命の経路。
        //   固定 ambient（部屋のどこでも同じ明るさ）をプローブの irradiance で置き換える。
        //   AO は掛けたまま: プローブ間隔 2m は皺や隅の小さな遮蔽を持っていないので、
        //   そこは従来どおり SSAO の担当（SSGI と違い二重計上にならない）。
        ambient = lerp(ambient,
                       (ddgiIrr * ambientDiffuse + ambientStrength * ambientSpecular) * ao,
                       ddgiConf);
        // SSGI があるなら拡散側をその放射照度で置き換える（AO は掛けない）。
        ambient = lerp(ambient,
                       ssgiRgb * ambientDiffuse + ambientStrength * ambientSpecular * ao,
                       ssgiConf);
    }

    return ambient + Lo + decalEmissive;
}

// color : 自己発光まで足し終えたリニア HDR の色。
// ★alpha をここへ渡さない理由: 旧 PSMain は alpha（albedo.a * opacity）を return の位置で計算していた。
//   引数にすると呼び出しの前に評価され、命令が上へ動く（DXIL のテキストが変わる。意味は同じ）。
float3 UnoShadeFinish(float3 color, UnoShadeInput si)
{
#ifndef UNO_SHADE_LITE
    // カスケード可視化デバッグ（shadowParams.w>0.5）
    if (shadowParams.w > 0.5f)
    {
        float3 tint[4] = { float3(1,0.4,0.4), float3(0.4,1,0.4), float3(0.4,0.4,1), float3(1,1,0.4) };
        color *= tint[SelectCascade(si.viewDepth)];
    }

    // クラスタ可視化デバッグ（clusterExtra.z: 1=ライト複雑度ヒートマップ / 2=クラスタ境界）
    color = ApplyClusterDebug(color, si.svPos, si.worldPos);
    // デカール枚数ヒートマップ（clusterExtra.z == 3。dx12_render_debug decalCount）
    color = ApplyDecalDebug(color, si.svPos, si.worldPos);
#endif

#ifdef UNO_PHYSICAL_LIGHTS
    // 物理ライティング単位（Q2）: シーン RT の 1.0 = 1 nit。R16G16B16A16_FLOAT の上限（65504）を超えると inf になり、
    // ポスト（ブルーム / TAA）が NaN を撒く。鏡面のピークなど極端な値はここで頭打ちにする（表示の白は EV100 で決まる）。
    color = min(color, 6.0e4);
#endif

#ifdef LDR_OUTPUT
    // LDR 直出力バリアント（サムネイル等、ポストプロセスを通らない R8G8B8A8 RT 用）
    color = ACESFilm(color);
    color = pow(color, 1.0 / 2.2);
#endif
    // 通常経路はリニア HDR のまま scene RT(R16G16B16A16_FLOAT) へ出力する。
    // トーンマップ(ACES)+ガンマは PostProcess の最終段で一括適用（bloom が
    // トーンマップ前の正しい輝度エネルギーを拾えるようにするため）。

    return color;
}

// 戻り値: rgb = リニア HDR の最終色 / a = s.opacity（BLEND PSO でだけ意味を持つ。不透明 PSO は無視）
float4 UnoShadeForward(UnoSurface s, float3 N, UnoShadeInput si)
{
    const float3 lit = UnoShadeLighting(s, N, si);
    // ===== 自己発光（emissive）=====
    // ★影・AO・ライティングを一切通さずそのまま足す（デカールの emissive と同じ扱い）。
    //   リニア HDR のまま出すので、1 を超えた分はブルームがそのまま拾う。
    const float3 color = lit + s.emissive;
    return float4(UnoShadeFinish(color, si), s.opacity);
}

#endif // FORWARD_SHADE_HLSLI
