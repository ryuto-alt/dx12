// Terrain.hlsl - 地形専用フォワード PS（4 レイヤーのテクスチャスプラット）
//
// ★ルートシグネチャは Forward.hlsl と同じものをそのまま使う（新スロット消費 0）。
//   t0/t1/t2 の「リソース次元」はルートシグネチャに書かれていないので、地形描画時だけ
//   Texture2DArray の SRV を張り、このシェーダが Texture2DArray と宣言すれば合法。
//     t0 = Texture2DArray  RGB=albedo(sRGB) / A=height
//     t1 = Texture2DArray  RG=normal.xy / B=roughness / A=AO
//     t2 = Texture2D       RGBA=レイヤー 0..3 の重み（スプラット）
//   b0 は Forward.hlsl が読んでいない余り 8 float、b2 は 8 DWORD まるごとを
//   「地形の PS しか読まない」ことを利用して読み替えている（C++ 側のバイト数は不変）。
//
// ★フォワード PS を分けた理由: [branch] で飛ばしてもコードがそこに在るだけで
//   レジスタ割当が悪化して PS 全体が遅くなる（00-COORDINATION §2 N24）。
//   地形の 8〜25 タップを Forward.hlsl へ足すと全メッシュが道連れになる。
//
// ★深度プリパスとの整合: VSMain の SV_POSITION は Forward.hlsl と同じ
//   `mul(float4(pos,1), mvp)` なのでプリパスとビット一致する（LESS_EQUAL で欠けない）。
//   SV_Depth は書かない。

#include "Lighting.hlsli"

Texture2DArray g_layerAlbedo  : register(t0);   // RGB=albedo(sRGB) A=height
Texture2DArray g_layerSurface : register(t1);   // RG=normal.xy B=roughness A=AO
Texture2D      g_splat        : register(t2);   // RGBA=レイヤー重み
SamplerState   g_sampler      : register(s0);   // LINEAR WRAP（レイヤーのタイリング用）

Texture2DArray g_shadowMap    : register(t4);

TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);   // LINEAR CLAMP（mip 有）★スプラットもこれで引く
SamplerState g_brdfSampler    : register(s3);

Texture2D<float> g_ssao          : register(t8);
Texture2D<float> g_contactShadow : register(t11);
Texture2D<float4> g_ssr  : register(t16);
Texture2D<float4> g_ssgi : register(t17);

// デカール（t18..t21）。★g_sampler(s0) と Lighting.hlsli より後に include すること。
// 地形は屋外デカール（足跡 / 弾痕 / 血）の最大の受け手なので、レイヤーセットを
// 割り当てた途端にデカールが消えるのを避けるため Forward.hlsl と同じく適用する。
#include "DecalApply.hlsli"

// ---------------------------------------------------------------------------
//  b0（40 DWORD）: Forward.hlsl は先頭 32 しか読んでいない。残り 8 float を地形が使う。
//  C++ 側の PerObjectData { XMMATRIX mvp; XMMATRIX mdl; float effect; XMFLOAT3 _pad; XMFLOAT4 params; }
//  とバイトレイアウトが完全に一致している（合計 160B = 40 DWORD）。
// ---------------------------------------------------------------------------
//  ★★ 4 本の float を並べて書いてはいけない ★★
//  DXC は cbuffer の「使われていないスカラーメンバ」を消して**残りを詰め直す**
//  （実測: float ×4 のうち 3 番目だけ未参照だと 4 番目が offset 140 → 136 へ繰り上がり、
//   C++ が書いた値と 1 個ズレて読まれる。dxc -dumpbin の "hostlayout.*" がその証拠）。
//  float4 として宣言すればベクタ単位でしか消えないのでズレない。
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;                 //   0.. 63
    float4x4 model;               //  64..127
    float4   pomParams;           // 128..143（effectValue + pad(3) の位置）
    //  .x = pomHeightScale  .y = pomFadeStart  .z = pomFadeEnd  .w = normalStrength
    float4   terrainParams;       // 144..159（shaderParams の位置）
    //  .x = 1/uvScale（頂点 UV → 0..1 スプラット UV）
    //  .y = distTilingStart(m)  .z = distTilingFarScale  .w = macroStrength
};
#define pomHeightScale  (pomParams.x)
#define pomFadeStart    (pomParams.y)
#define pomFadeEnd      (pomParams.z)
#define normalStrength  (pomParams.w)

// ---------------------------------------------------------------------------
//  b2（8 DWORD）: PBRMaterial の枠を地形専用の意味に読み替える。
//  ★ゲートは「layerSetPath が空でなければ地形 PSO」の 1 箇所だけ。逆条件
//    （レイヤーセット有りなのに Forward PSO）が起きない書き方にしてある。
// ---------------------------------------------------------------------------
//  b0 と同じ理由でスカラーを並べない（未参照メンバの消去 + 詰め直し対策）。
cbuffer TerrainMaterial : register(b2)
{
    float4 terrainMat0;           //  0..15
    //  .x = heightBlendDepth（Mishkinis の depth。0 に近いほど境界がシャープ）
    //  .y = triplanarSharpness（既定 4.0）
    //  .z = terrainFlags（下の TF_* ビット + layerCount + pomMaxSteps。asuint で読む）
    //  .w = macroScale（マクロバリエーションの周期 m）
    float4 layerTiling;           // 16..31 各レイヤーの「1m あたり何回タイルするか」
};
#define heightBlendDepth   (terrainMat0.x)
#define triplanarSharpness (terrainMat0.y)
#define terrainFlags       (asuint(terrainMat0.z))
#define macroScale         (terrainMat0.w)

#define TF_TRIPLANAR  (1u << 0)
#define TF_POM        (1u << 1)
#define TF_MACRO      (1u << 2)
#define TF_DISTTILE   (1u << 3)
#define TF_LAYERCOUNT(f) (((f) >> 8) & 0xFu)
#define TF_POMSTEPS(f)   (((f) >> 16) & 0xFFu)

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
    float3 worldTangent : TANGENT;
    float  tangentW     : TEXCOORD3;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // ★この式は Forward.hlsl / 深度プリパスと 1 命令もずらさないこと（LESS_EQUAL の前提）
    output.positionSV   = mul(float4(input.position, 1.0f), mvp);

    float4 worldPos4    = mul(float4(input.position, 1.0f), model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(input.normal, (float3x3)model));
    output.worldTangent = normalize(mul(input.tangent.xyz, (float3x3)model));
    output.tangentW     = input.tangent.w;
    output.texCoord     = input.texCoord;

    float4 viewPos4     = mul(worldPos4, view);
    output.viewDepth    = viewPos4.z;
    return output;
}

// ---- CSM（Forward.hlsl と同一。地形は面積が広いので影の見た目を変えないこと）----
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

float SampleCascade(int cascade, float3 worldPos)
{
    float4 lc = mul(float4(worldPos, 1.0f), cascadeViewProj[cascade]);
    float3 proj = lc.xyz / lc.w;
    float2 uv = proj.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1.0f;

    float current = proj.z - shadowParams.y;
    float texel = shadowParams.x;
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
        s += g_shadowMap.SampleCmpLevelZero(g_shadowSampler,
                 float3(uv + float2(x, y) * texel, (float)cascade), current);
    return s / 9.0f;
}

float CalcShadow(float3 worldPos, float viewDepth)
{
    if (cascadeSplitsView.x > 1.0e8) return 1.0f;
    int c = SelectCascade(viewDepth);
    float shadow = SampleCascade(c, worldPos);
    float band = shadowParams.z;
    if (band > 0.0f && c < NUM_CASCADES - 1)
    {
        float edge = cascadeSplitsView[c];
        float t = saturate((edge - viewDepth) / max(band, 1e-4));
        if (t < 1.0f)
            shadow = lerp(SampleCascade(c + 1, worldPos), shadow, t);
    }
    return shadow;
}

// HDR ソース（SSR / SSGI）の Inf 除去。fp16 上限を超えた値をそのまま ACES に通すと
// NaN 化して全ジオメトリが真っ黒になる事故がある（先行実装が実機で踏んだ）。
// 規約は shaders/screenspace/ScreenSpaceCommon.hlsli の SS_Sanitize と同じ（60000 = fp16 上限手前）。
float3 TerrSanitize(float3 c)
{
    c = min(c, 60000.0);
    return (isfinite(c.x) && isfinite(c.y) && isfinite(c.z)) ? max(c, 0.0) : float3(0, 0, 0);
}

// ---------------------------------------------------------------------------
//  レイヤーのサンプル
//  ★分岐の外で導関数を作り SampleGrad を使う。トライプラナー / POM / レイヤー間引きは
//    全部「分岐の中で UV を作る」ので、暗黙の ddx/ddy に頼ると遠景がチラつき黒帯が出る。
// ---------------------------------------------------------------------------
struct LayerSample
{
    float3 albedo;
    float  height;
    float2 normalXY;   // 0..1 のまま（後段でまとめて -1..1 へ）
    float  roughness;
    float  ao;
};

LayerSample SampleLayerPlanar(uint i, float2 uv, float2 dx, float2 dy)
{
    float4 a = g_layerAlbedo .SampleGrad(g_sampler, float3(uv, (float)i), dx, dy);
    float4 s = g_layerSurface.SampleGrad(g_sampler, float3(uv, (float)i), dx, dy);

    LayerSample o;
    o.albedo    = a.rgb;
    o.height    = a.a;
    o.normalXY  = s.rg;
    o.roughness = s.b;
    o.ao        = s.a;
    return o;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // ===== スプラット重み =====
    // 頂点 UV は 0..uvScale なので 1/uvScale を掛けて 0..1 のスプラット UV に戻す。
    // サンプラーは s2（LINEAR CLAMP）。s0 は WRAP なので地形の端で反対側の重みが滲む。
    float2 splatUv = input.texCoord * terrainParams.x;
    float4 w = g_splat.Sample(g_iblSampler, splatUv);

    // レイヤー数が 4 未満なら余ったチャンネルを殺す（未使用スライスを引かない）
    uint layerCount = TF_LAYERCOUNT(terrainFlags);
    if (layerCount < 4u) w.w = 0.0;
    if (layerCount < 3u) w.z = 0.0;
    if (layerCount < 2u) w.y = 0.0;
    // ペイントで崩れた重みを正規化（合計 1 の保証は無い）
    w /= max(w.x + w.y + w.z + w.w, 1e-4);

    // ===== レイヤーのサンプル（平面 Y 投影）=====
    // ワールド XZ をレイヤー UV の素にする（地形は平行移動のみ有効なので歪まない）。
    float3 dWdx = ddx(input.worldPos);
    float3 dWdy = ddy(input.worldPos);

    LayerSample L[4];
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        float  t  = layerTiling[i];
        float2 uv = input.worldPos.xz * t;
        L[i] = SampleLayerPlanar(i, uv, dWdx.xz * t, dWdy.xz * t);
    }

    // ===== 重みの決定（Step 1 は線形ブレンド）=====
    float4 bw = w;

    float3 albedo = L[0].albedo * bw.x + L[1].albedo * bw.y
                  + L[2].albedo * bw.z + L[3].albedo * bw.w;
    float2 nxy01  = L[0].normalXY * bw.x + L[1].normalXY * bw.y
                  + L[2].normalXY * bw.z + L[3].normalXY * bw.w;
    float roughness = L[0].roughness * bw.x + L[1].roughness * bw.y
                    + L[2].roughness * bw.z + L[3].roughness * bw.w;
    float matAO = L[0].ao * bw.x + L[1].ao * bw.y + L[2].ao * bw.z + L[3].ao * bw.w;

    // ===== 法線（XY を加重和 → Z を再構成。PBR.hlsli の PerturbNormal と同じ再構成）=====
    float3 gN = normalize(input.worldNormal);
    float3 N;
    {
        float2 nxy = (nxy01 * 2.0 - 1.0) * normalStrength;
        float3 nTS = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
        float3 T = normalize(input.worldTangent - dot(input.worldTangent, gN) * gN);
        float3 B = cross(gN, T) * input.tangentW;
        N = normalize(mul(nTS, float3x3(T, B, gN)));
    }

    float metallic = 0.0;                    // 地形は非金属
    roughness = clamp(roughness, 0.04, 1.0);

    // ===== デカール（Forward.hlsl と同じ位置・同じ引数）=====
    float3 decalEmissive = 0.0;
    ApplyDecals(input.worldPos, input.positionSV.xy, input.viewDepth,
                albedo, N, metallic, roughness, decalEmissive);

    // ===== ライティング（ここから先は Forward.hlsl と同一）=====
    float3 V = normalize(cameraPos - input.worldPos);
    float3 Ldir = normalize(-lightDir);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float shadow = CalcShadow(input.worldPos, input.viewDepth);
    if (contactShadowEnabled > 0.5)
        shadow = min(shadow, g_contactShadow.Load(int3(input.positionSV.xy, 0)));

    float3 Lo = ShadePunctual(N, V, Ldir, lightColor * shadow, albedo, F0, metallic, roughness);
    Lo += AccumulatePunctualLights(N, V, input.worldPos, albedo, F0, metallic, roughness,
                                   input.positionSV.xy);

    float ao = (aoEnabled > 0.5) ? g_ssao.Load(int3(input.positionSV.xy, 0)) : 1.0;
    ao *= matAO;   // レイヤーのマテリアル AO（岩の隙間など）を掛ける

    float4 ssrRaw  = g_ssr.Load(int3(input.positionSV.xy, 0));
    float4 ssgiRaw = g_ssgi.Load(int3(input.positionSV.xy, 0));
    float3 ssrRgb  = TerrSanitize(ssrRaw.rgb);
    float3 ssgiRgb = TerrSanitize(ssgiRaw.rgb);
    float  ssrConf  = saturate(ssrRaw.a);
    float  ssgiConf = saturate(ssgiRaw.a);

    float3 ambient;
    if (hasIBL != 0u)
    {
        float3 R   = reflect(-V, N);
        float  NoV = max(dot(N, V), 0.0);
        float3 F   = FresnelSchlickRoughness(NoV, F0, roughness);
        float3 kD  = (1.0 - F) * (1.0 - metallic);

        float3 irradiance = g_irradianceMap.SampleLevel(g_iblSampler, N, 0).rgb;
        irradiance = lerp(irradiance, ssgiRgb, ssgiConf);
        float3 diffuseIBL = irradiance * albedo;

        float  mip = roughness * maxPrefilterMip;
        float3 prefiltered = g_prefilteredMap.SampleLevel(g_iblSampler, R, mip).rgb;
        prefiltered = lerp(prefiltered, ssrRgb, ssrConf);
        float2 envBRDF = g_brdfLUT.SampleLevel(g_brdfSampler, float2(NoV, roughness), 0).rg;
        float3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

        float aoDiff = lerp(ao, 1.0, ssgiConf);
        float aoSpec = lerp(ao, 1.0, ssrConf);
        ambient = (kD * diffuseIBL * aoDiff + specularIBL * aoSpec) * iblIntensity;
    }
    else
    {
        float3 ambientDiffuse  = albedo * (1.0 - metallic);
        float3 ambientSpecular = lerp(F0, ssrRgb, ssrConf);
        ambient = ambientStrength * (ambientDiffuse + ambientSpecular) * ao;
        ambient = lerp(ambient,
                       ssgiRgb * ambientDiffuse + ambientStrength * ambientSpecular * ao,
                       ssgiConf);
    }

    float3 color = ambient + Lo + decalEmissive;

    if (shadowParams.w > 0.5f)
    {
        float3 tint[4] = { float3(1,0.4,0.4), float3(0.4,1,0.4), float3(0.4,0.4,1), float3(1,1,0.4) };
        color *= tint[SelectCascade(input.viewDepth)];
    }
    color = ApplyClusterDebug(color, input.positionSV.xy, input.worldPos);

    return float4(color, 1.0);
}
