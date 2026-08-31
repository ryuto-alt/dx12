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
// PCSS / 3x3 PCF の共有実装（g_shadowMap と Lighting.hlsli の後で include すること）
#include "ShadowPcss.hlsli"

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

// ★頂点タンジェントは持たない。地形のタンジェントは常に +X 固定（TerrainMeshBuilder）で、
//   レイヤー UV はワールド投影だから、タンジェント基底は法線とワールド軸だけで作れる
//   （Whiteout の Y 投影項がそのまま TBN になっている）。
struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldPos     : TEXCOORD2;
    float3 worldNormal  : NORMAL;
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

float SampleCascade(int cascade, float3 worldPos, float2 svPos)
{
    // ★実体は ShadowPcss.hlsli（PCSS / 3x3 PCF の切替込み。4 つの PS で共有）
    return SampleShadowCascadeCommon(g_shadowMap, cascade, worldPos, svPos, shadowParams.y);
}

float CalcShadow(float3 worldPos, float viewDepth, float2 svPos)
{
    if (cascadeSplitsView.x > 1.0e8) return 1.0f;
    int c = SelectCascade(viewDepth);
    float shadow = SampleCascade(c, worldPos, svPos);
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

// 高さブレンド（Mishkinis "Advanced Terrain Texture Splatting" の 4 層一般化）。
//   出典: https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
// h = 各レイヤーのハイトマップ / a = スプラットの重み / depth = 遷移帯の厚み。
// depth を 0 に近づけるほど「高い方が総取り」で境界がシャープ、大きくすると線形ブレンドに近づく。
//
// ★重み 0 のレイヤーが高さだけで勝って滲み出さないよう、a≈0 の層は競争から外す
//   （2 層版の原文は a1+a2=1 前提なのでこの保護が要らないだけ）。
float4 HeightBlend4(float4 h, float4 a, float depth)
{
    float4 hw = lerp(-1.0, h + a, step(1e-4, a));
    float  ma = max(max(hw.x, hw.y), max(hw.z, hw.w)) - depth;
    float4 b  = max(hw - ma, 0.0);
    return b / max(b.x + b.y + b.z + b.w, 1e-5);
}

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

// 1 投影ぶんの合成結果（4 レイヤーを高さブレンドで混ぜたもの）。
struct Surface
{
    float3 albedo;
    float2 nxy01;      // 0..1 のタンジェント空間 XY
    float  roughness;
    float  ao;
};

// uvBase / dBase は「タイリング 1.0 のときの投影座標（メートル）とその導関数」。
// tileMul で距離タイリング（粗い側）へ切り替える。
// ★導関数は必ず呼び出し側（分岐の外）で作ったものを渡すこと。
//   分岐の中で ddx/ddy を取ると遠景でミップが暴れて黒帯が出る。
Surface CompositeLayers(float2 uvBase, float2 dxBase, float2 dyBase, float4 w, float tileMul)
{
    LayerSample L[4];
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        float t = layerTiling[i] * tileMul;
        L[i] = SampleLayerPlanar(i, uvBase * t, dxBase * t, dyBase * t);
    }

    float4 h  = float4(L[0].height, L[1].height, L[2].height, L[3].height);
    float4 bw = HeightBlend4(h, w, heightBlendDepth);

    Surface s;
    s.albedo    = L[0].albedo   * bw.x + L[1].albedo   * bw.y
                + L[2].albedo   * bw.z + L[3].albedo   * bw.w;
    s.nxy01     = L[0].normalXY * bw.x + L[1].normalXY * bw.y
                + L[2].normalXY * bw.z + L[3].normalXY * bw.w;
    s.roughness = L[0].roughness * bw.x + L[1].roughness * bw.y
                + L[2].roughness * bw.z + L[3].roughness * bw.w;
    s.ao        = L[0].ao * bw.x + L[1].ao * bw.y + L[2].ao * bw.z + L[3].ao * bw.w;
    return s;
}

// 0..1 の RG からタンジェント空間法線を復元する（Z は XY から再構成＝BC5/2ch 互換。
// PBR.hlsli の PerturbNormal と同じ再構成）。
float3 UnpackTangentNormal(float2 nxy01, float strength)
{
    float2 nxy = (nxy01 * 2.0 - 1.0) * strength;
    return float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
}

// ---------------------------------------------------------------------------
//  パララックスオクルージョンマッピング（POM・既定 OFF）
//
//  視線を面へ投影し、高さを N スライスに割って進みながらハイトマップを引く。
//  レイの深さがハイト値を下回った時点で交差 → 直前 2 ステップ間を線形補間して交点を出す
//  （Steep Parallax にこの補間を足したものが POM）。
//    出典: https://gamedev.net/tutorials/programming/graphics/a-closer-look-at-parallax-occlusion-mapping-r3262
//
//  ★4 層ぶんの合成高さを毎ステップ作るとステップあたり 4 タップになるので、
//    「スプラットの重みが最大のレイヤー 1 枚」の高さだけでレイマーチする（1 タップ/ステップ）。
//  ★SV_Depth は書かない。書くと深度プリパスの平らな深度と食い違って LEQUAL で全部落ちる。
//    したがってシルエットは平ら＝仕様として受け入れる。
//  ★セルフシャドウは入れない（コストがほぼ倍で、屋外の地形では寄与が小さい）。
// ---------------------------------------------------------------------------
float SampleLayerHeight(uint dom, float2 uv, float2 dx, float2 dy)
{
    return g_layerAlbedo.SampleGrad(g_sampler, float3(uv, (float)dom), dx, dy).a;
}

// 戻り値は「支配レイヤーの UV 空間でのオフセット」。呼び出し側でタイリングを割ってメートルへ戻す。
float2 ParallaxOcclusionOffset(uint dom, float2 uv, float2 dx, float2 dy,
                               float3 viewTS, float heightScaleUv, int steps)
{
    const float layerDepth = 1.0 / max((float)steps, 1.0);
    // 視線が寝るほど大きくずらす。z のクランプで斜め見のときの発散を抑える。
    const float2 deltaUV = (viewTS.xy / max(viewTS.z, 0.25)) * heightScaleUv * layerDepth;

    float2 curUV        = uv;
    float  curLayer     = 0.0;
    float  curDepthMap  = 1.0 - SampleLayerHeight(dom, curUV, dx, dy);

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        if (curLayer >= curDepthMap) break;
        curUV      -= deltaUV;
        curLayer   += layerDepth;
        curDepthMap = 1.0 - SampleLayerHeight(dom, curUV, dx, dy);
    }

    // 直前 2 ステップの線形補間で交点を出す（ここが「POM」の定義）
    const float2 prevUV = curUV + deltaUV;
    const float  after  = curDepthMap - curLayer;
    const float  before = (1.0 - SampleLayerHeight(dom, prevUV, dx, dy)) - (curLayer - layerDepth);
    const float  wgt    = after / max(after - before, 1e-5);
    return lerp(curUV, prevUV, saturate(wgt)) - uv;
}

uint ArgMax4(float4 v)
{
    uint  i = 0;
    float m = v.x;
    if (v.y > m) { m = v.y; i = 1; }
    if (v.z > m) { m = v.z; i = 2; }
    if (v.w > m) {          i = 3; }
    return i;
}

// ---------------------------------------------------------------------------
//  マクロバリエーション（タップ 0 の算術 value noise）
//  t0/t1/t2 が埋まっていてノイズテクスチャ用のディスクリプタが無いので算術で作る。
//  50〜200m スケールの低周波を 1 枚アルベドに掛けるだけで、広い面の「同じ色がずっと続く」
//  作り物感がかなり消える（UE 界隈で最初にやる定番）。
//  出典: https://www.worldofleveldesign.com/categories/ue4/landscape-macro-tiling-variation.php
// ---------------------------------------------------------------------------
float TerrHash21(float2 p)
{
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float TerrValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = TerrHash21(i);
    float b = TerrHash21(i + float2(1.0, 0.0));
    float c = TerrHash21(i + float2(0.0, 1.0));
    float d = TerrHash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float MacroNoise(float2 p)
{
    float n = 0.0, a = 0.5;
    [unroll]
    for (int i = 0; i < 3; ++i) { n += a * TerrValueNoise(p); p *= 2.03; a *= 0.5; }
    return n / 0.875;   // 3 オクターブ（0.5+0.25+0.125）で 0..1 に正規化
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

    // ===== レイヤーのサンプル =====
    // ワールド座標を投影して UV にする（地形は平行移動のみ有効なので歪まない）。
    // ★導関数は分岐に入る前にここで 1 回だけ作る（SampleGrad へ渡す）。
    float3 dWdx = ddx(input.worldPos);
    float3 dWdy = ddy(input.worldPos);

    float3 gN = normalize(input.worldNormal);

    // トライプラナーの重み（bgolus。ハードコード 4 乗が最速かつ十分だが、
    // 鋭さは triplanarSharpness で調整できるようにしてある）。
    //   出典: https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
    float3 tb = pow(abs(gN), triplanarSharpness);
    tb /= max(tb.x + tb.y + tb.z, 1e-5);

    // UV のミラー防止（軸の符号で U を反転する。bgolus の axisSign）。
    // ★HLSL 2021 の ?: はスカラー条件しか取らない。sign() は 0 を返し得るので使わない。
    float3 axisSign = float3(gN.x < 0.0 ? -1.0 : 1.0,
                             gN.y < 0.0 ? -1.0 : 1.0,
                             gN.z < 0.0 ? -1.0 : 1.0);

    const float dist = length(cameraPos - input.worldPos);

    // ===== POM（既定 OFF）=====
    // Y 投影の UV（ワールド XZ、メートル）をずらす量を先に決めてから全レイヤーへ適用する。
    // ★トライプラナー分岐に入るピクセルでは切る（3 投影ぶん回すと 3 倍になるため）。
    float2 uvY = input.worldPos.xz * float2(axisSign.y, 1.0);
    float2 dxY = dWdx.xz * float2(axisSign.y, 1.0);
    float2 dyY = dWdy.xz * float2(axisSign.y, 1.0);
    {
        // 距離フェード: pomFadeEnd を跨いでスケールが 0 に落ちるのでポップしない
        const float fade  = 1.0 - saturate((dist - pomFadeStart)
                                         / max(pomFadeEnd - pomFadeStart, 1e-3));
        const float scale = pomHeightScale * fade;
        const bool  flat  = !(terrainFlags & TF_TRIPLANAR) || tb.y >= 0.98;

        [branch]
        if ((terrainFlags & TF_POM) && scale > 1e-4 && flat)
        {
            const uint  dom = ArgMax4(w);
            const float t   = layerTiling[dom];
            // 接空間の視線（Y 投影の基底は T=+X / B=+Z / N=+Y。axisSign で U のミラーも合わせる）
            const float3 V0 = normalize(cameraPos - input.worldPos);
            const float3 viewTS = normalize(float3(V0.x * axisSign.y, V0.z, max(V0.y, 1e-3)));
            const float  ndv    = saturate(viewTS.z);
            const int    steps  = (int)max(lerp(2.0, (float)TF_POMSTEPS(terrainFlags),
                                                (1.0 - ndv) * fade), 2.0);

            const float2 off = ParallaxOcclusionOffset(dom, uvY * t, dxY * t, dyY * t,
                                                       viewTS, scale * t, steps);
            const float2 offM = off / max(t, 1e-4);   // レイヤー UV → メートルへ戻す
            uvY += offM;
            // ★導関数はずらさない（ずらすとミップが暴れる。ずれ量は連続なので問題ない）
        }
    }

    // ★高さブレンドは CompositeLayers の中（4 レイヤーぶんの合成もそこ）
    Surface S = CompositeLayers(uvY, dxY, dyY, w, 1.0);

    // ===== 距離タイリング =====
    // 同じテクスチャを「遠距離用の粗いタイリング」でもう一度引いて距離で lerp する。
    // 遠景の格子模様（同じ絵が等間隔に並んで見える現象）が消える。マクロと併用すると
    // タイリングはほぼ完全に見えなくなる、というのが UE 界隈の定説。
    //   出典: https://80.lv/articles/tutorial-fixing-landscape-texture-tiling-in-ue4
    {
        float distBlend = saturate((dist - terrainParams.y) / max(terrainParams.y, 1.0));
        [branch]
        if ((terrainFlags & TF_DISTTILE) && distBlend > 0.001)
        {
            float far = 1.0 / max(terrainParams.z, 1.0);
            Surface F = CompositeLayers(uvY, dxY, dyY, w, far);
            S.albedo    = lerp(S.albedo,    F.albedo,    distBlend);
            S.nxy01     = lerp(S.nxy01,     F.nxy01,     distBlend);
            S.roughness = lerp(S.roughness, F.roughness, distBlend);
            S.ao        = lerp(S.ao,        F.ao,        distBlend);
        }
    }

    // ===== トライプラナー投影（Whiteout ブレンド・急斜面だけ）=====
    // 平坦地は tb.y ≈ 1 なので Y 投影だけで足りる。急斜面（テクスチャが縦に引き伸ばされる所）
    // だけ 3 投影に落とす。しきい値 0.98 で「引き伸ばしが目に見える斜面」をほぼ拾える。
    // 遠景は法線を弱める（ミップで潰れた法線がノイズにしか見えなくなるのを防ぐ）。
    const float nStr = normalStrength * (1.0 - 0.5 * saturate((dist - 60.0) / 120.0));

    float3 albedo;
    float  roughness;
    float  matAO;
    float3 N;

    [branch]
    if ((terrainFlags & TF_TRIPLANAR) && tb.y < 0.98)
    {
        // X 投影 = ZY 平面 / Z 投影 = XY 平面（U 側だけ軸の符号でミラーを解く）
        float2 sX = float2(axisSign.x, 1.0);
        float2 sZ = float2(-axisSign.z, 1.0);
        Surface SX = CompositeLayers(input.worldPos.zy * sX, dWdx.zy * sX, dWdy.zy * sX, w, 1.0);
        Surface SZ = CompositeLayers(input.worldPos.xy * sZ, dWdx.xy * sZ, dWdy.xy * sZ, w, 1.0);

        albedo    = SX.albedo    * tb.x + S.albedo    * tb.y + SZ.albedo    * tb.z;
        roughness = SX.roughness * tb.x + S.roughness * tb.y + SZ.roughness * tb.z;
        matAO     = SX.ao        * tb.x + S.ao        * tb.y + SZ.ao        * tb.z;

        // 3 投影はそれぞれ別のタンジェント空間にあるので素直に混ぜてはいけない。
        // Whiteout ブレンド（bgolus / Barré-Brisebois & Hill "Blending in Detail"）。
        float3 tnX = UnpackTangentNormal(SX.nxy01, nStr);
        float3 tnY = UnpackTangentNormal(S.nxy01,  nStr);
        float3 tnZ = UnpackTangentNormal(SZ.nxy01, nStr);
        tnX.x *= axisSign.x;
        tnY.x *= axisSign.y;
        tnZ.x *= -axisSign.z;
        tnX = float3(tnX.xy + gN.zy, abs(tnX.z) * gN.x);
        tnY = float3(tnY.xy + gN.xz, abs(tnY.z) * gN.y);
        tnZ = float3(tnZ.xy + gN.xy, abs(tnZ.z) * gN.z);
        N = normalize(tnX.zyx * tb.x + tnY.xzy * tb.y + tnZ.xyz * tb.z);
    }
    else
    {
        albedo    = S.albedo;
        roughness = S.roughness;
        matAO     = S.ao;

        // Y 投影だけ。上のトライプラナー式に tb=(0,1,0) を入れたものと厳密に一致させる
        // （しきい値を跨いだときに法線が飛ばないように）。
        float3 tnY = UnpackTangentNormal(S.nxy01, nStr);
        tnY.x *= axisSign.y;
        tnY = float3(tnY.xy + gN.xz, abs(tnY.z) * gN.y);
        N = normalize(tnY.xzy);
    }

    // ===== マクロバリエーション =====
    // 低周波ノイズ 1 系統をアルベドに掛ける（テクスチャを引かないのでタップ 0）。
    [branch]
    if (terrainFlags & TF_MACRO)
    {
        float m = MacroNoise(input.worldPos.xz / max(macroScale, 1e-3));
        albedo *= lerp(1.0, m * 2.0, saturate(terrainParams.w));
    }

    float metallic = 0.0;                    // 地形は非金属
    roughness = clamp(roughness, 0.04, 1.0);

    // ===== デカール（Forward.hlsl と同じ位置・同じ引数）=====
    float3 decalEmissive = 0.0;
    ApplyDecals(input.worldPos, input.positionSV.xy, input.viewDepth,
                albedo, N, metallic, roughness, decalEmissive);

    // ===== 法線マップフィルタリング（Forward.hlsl と同一。分散 → ラフネス / 平均法線の復元）=====
    FilterShadingNormal(N, roughness, normalize(input.worldNormal), normalFilterParams);

    // ===== ライティング（ここから先は Forward.hlsl と同一）=====
    float3 V = normalize(cameraPos - input.worldPos);
    float3 Ldir = normalize(-lightDir);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float shadow = CalcShadow(input.worldPos, input.viewDepth, input.positionSV.xy);
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

    // DDGI（Forward.hlsl と同じ扱い。OFF なら ddgiConf=0 で以降の lerp が恒等）
    float  ddgiConf = 0.0;
    float3 ddgiIrr  = TerrSanitize(SampleDdgi(input.worldPos, N, ddgiConf));

    float3 ambient;
    if (hasIBL != 0u)
    {
        float3 R   = reflect(-V, N);
        float  NoV = max(dot(N, V), 0.0);
        float3 F   = FresnelSchlickRoughness(NoV, F0, roughness);
        float3 kD  = (1.0 - F) * (1.0 - metallic);

        float3 irradiance = g_irradianceMap.SampleLevel(g_iblSampler, N, 0).rgb;
        irradiance = lerp(irradiance, ddgiIrr, ddgiConf);
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
                       (ddgiIrr * ambientDiffuse + ambientStrength * ambientSpecular) * ao,
                       ddgiConf);
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
    // デカール枚数ヒートマップ（clusterExtra.z == 3。dx12_render_debug decalCount）
    color = ApplyDecalDebug(color, input.positionSV.xy, input.worldPos);

    return float4(color, 1.0);
}
