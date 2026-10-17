// Forward.hlsl - PBR Forward Rendering (Cook-Torrance BRDF)
// ★仮想ジオメトリ P4: shaders/vg/VgResolve.hlsl がこの PS の「材質の評価」（アルベド × ティント・法線マップ・MR・自己発光）を
//   同じ手順で書き写している（ライティング本体は ForwardShade.hlsli を共有）。材質の評価を変えたら VgResolve.hlsl も揃え、
//   VG のパリティ（docs/VIRTUAL_GEOMETRY_DESIGN.md §5.4）を撮り直すこと。
#include "Lighting.hlsli"

// Textures
Texture2D    g_albedo         : register(t0);
Texture2D    g_normalMap      : register(t1);
Texture2D    g_metalRoughness : register(t2);
SamplerState g_sampler        : register(s0);

// 自己発光（emissive、t24）。★材質 SRV ブロックの 4 枚目。
//   レジスタは t0,t1,t2 から t24 へ飛んでいるが、ルートシグネチャの材質テーブルは
//   OFFSET_APPEND なので**ヒープ上は t2 の直後**（graphics/RootSignature.cpp 参照）。
//   t3..t23 は骨/影/IBL/SSAO/クラスタ/デカール/DDGI で埋まっていて、空きが t24 だけだった。
//   ★pbrFlags bit3 が立っていないときは絶対にサンプルしないこと（材質ブロックを持たない
//     フォールバック描画では 4 枚目が別物のディスクリプタになる）。
Texture2D    g_emissive       : register(t24);

// 高さ（視差オクルージョン、t25、R チャンネル、白 = 高い）。★材質 SRV ブロックの 5 枚目（ヒープ上は +4）。
//   pbrFlags bit5（kPbrFlagParallax）が立っていないときは絶対にサンプルしないこと
//   （材質ブロックを持たないフォールバック描画では 5 枚目が別物のディスクリプタになる）。
Texture2D    g_height         : register(t25);

// Shadow (CSM: Texture2DArray, 1スライス=1カスケード)。g_shadowSampler(s1)は Lighting.hlsli で共有宣言。
Texture2DArray         g_shadowMap     : register(t4);
// PCSS / 3x3 PCF の共有実装（g_shadowMap と Lighting.hlsli の後で include すること）
#include "ShadowPcss.hlsli"

// IBL (t5,t6,t7 / s2=linear-clamp(mip有), s3=linear-clamp(mipなし))
TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);  // LINEAR CLAMP（mip有, irradiance/prefiltered 用）
SamplerState g_brdfSampler    : register(s3);  // LINEAR CLAMP（mipなし, LUT 用）

// SSAO（スクリーン空間 AO。フル解像度・同一ビューポート前提でピクセル直読み）
Texture2D<float> g_ssao        : register(t8);
SamplerState     g_ssaoSampler : register(s4);  // POINT CLAMP（未使用だが RootSig 整合のため宣言）

// コンタクトシャドウ（深度バッファのスクリーン空間レイマーチ。1=遮蔽なし）。
// CSM が解像度不足で落とせない接地部の細かい影を補うため、太陽の寄与へ乗算する。
Texture2D<float> g_contactShadow : register(t11);

// SSR（スクリーン空間反射。rgb=反射放射輝度 / a=confidence）と
// SSGI（スクリーン空間GI。rgb=間接放射照度＝albedo を掛ける前）。
// どちらもフル解像度・同一ビューポートなのでピクセル直読み。
// ★無効時は 1x1 黒ダミーが貼られる → Load(画面座標) が範囲外で 0 を返し、
//   SSR は confidence=0（＝完全に IBL）、SSGI は寄与 0 になる。分岐フラグは不要。
Texture2D<float4> g_ssr  : register(t16);
Texture2D<float4> g_ssgi : register(t17);

// デカール（t18..t21）。★g_sampler(s0) と Lighting.hlsli より後に include すること。
#include "DecalApply.hlsli"

// PerObject constants (b0)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

// PBR Material constants (b2)
cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    // bit0=hasNormalMap, bit1=hasMetalRoughness, bit2=アルファテスト有効, bit3=emissive テクスチャ有り,
    // bit4=MR の R をマテリアル AO として読む, bit16..23=AO 強度(8bit),
    // bit8..15=alphaCutoff(8bit 量子化)。詳細は renderer/Material.h。
    uint  pbrFlags;
    // ★エンティティ毎の一律色ティント（RGB888。0xFFFFFF = 白 = 影響なし）。
    //   共有 Mesh の頂点バッファを塗り替える旧実装だと、同じモデルを使う他のエンティティまで
    //   同じ色になり、シーン読み込みでは最後に読まれた 1 体の色が全員に残っていた。
    //   ★インスタンス描画では白が入り、色は per-instance の input.color 側で掛かる。
    // ★上位 8bit(bit24..31) は「不透明度(opacity)」を 8bit 量子化したもの。255 = 不透明。
    //   ルート定数の予算が 61/64 で埋まっていて float を足せないため、空きバイトへ詰めてある
    //   （b2 のバイトレイアウトは 1 バイトも変わっていない）。詳細は renderer/Material.h。
    uint  packedTint;
    // UV 変換 (xy=スケール, zw=オフセット)。MeshRenderer の UV スクロール/連番アニメ用。
    // 無効時は (1,1,0,0) が入るので従来と同じ結果になる。
    float4 uvScaleOffset;
    // 自己発光（9 本目のルート定数）。bit0..23=色 RGB888（リニア）/ bit24..31=強度。
    // 強度は二乗曲線で 8bit 量子化してある（復元は (v/255)^2 * 64）。0 = 無発光。
    // 詰め方の正は renderer/Material.h の PackEmissive()。
    uint  packedEmissive;
};

// packedEmissive（b2）を放射輝度へ戻す。CPU 側 renderer/Material.h の PackEmissive と対。
// ★0 なら丸ごと 0 を返す＝ emissive を持たないマテリアルは 1 ピクセルも変わらない。
float3 UnpackEmissive(uint packed, float2 uv)
{
    if (packed == 0u) return 0.0;
    const float3 c = float3((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF) / 255.0;
    // 強度は二乗で伸長（0 付近を細かく、ブルームに要る 1 超の領域まで 8bit で届かせるため）
    const float n = float((packed >> 24) & 0xFF) / 255.0;
    float3 e = c * (n * n * 64.0);
    if (pbrFlags & 8u) e *= g_emissive.Sample(g_sampler, uv).rgb;
    return e;
}

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
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float  viewDepth    : TEXCOORD4;  // view 空間深度(正値) → カスケード選択用
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV   = mul(float4(input.position, 1.0f), mvp);

    float4 worldPos4    = mul(float4(input.position, 1.0f), model);
    output.worldPos     = worldPos4.xyz;
    output.worldNormal  = normalize(mul(input.normal, (float3x3)model));
    output.worldTangent = normalize(mul(input.tangent.xyz, (float3x3)model));
    output.tangentW     = input.tangent.w;
    output.color        = input.color;
    output.texCoord     = input.texCoord;

    float4 viewPos4     = mul(worldPos4, view);  // LH: 前方 +z
    output.viewDepth    = viewPos4.z;

    return output;
}

// ===== 視差オクルージョンマッピング（POM。pbrFlags bit5 が立った材質だけ。既定 OFF）=====
//  Terrain.hlsl の ParallaxOcclusionOffset と同じ作法: 視線を面へ投影し、深さを N スライスに割って線形に進み、
//  交差した区間を 1 回だけ線形補間して交点を出す。高さは「白 = 面の最上面、黒 = 最上面から parallaxScale 下」。
//  ★SV_Depth は書かない（深度プリパス・Hi-Z と一致のまま。シルエットは平ら＝仕様）。自己影も入れない。
//  ★接線ベクトルに頼らず、ワールド座標と UV の画面空間微分から「UV の勾配（面内）」を直接作る。
//    → メッシュの接線の有無・ミラー・非一様スケール・uvTiling（uvScaleOffset）がそのまま正しく効く。
//  ★距離とグレージング角でフェード（遠方・かすめ角のちらつきと負荷を抑える）。段数も視線が寝るほど増え、遠いほど減る。
//  戻り値は「UV に足すオフセット」（最上面の交点が 0）。効果 0 のときは 0 を返す。
static const float kPomScaleMax  = 0.25;    // renderer/Material.h kParallaxScaleMax と一致
static const float kPomFadeStart = 14.0;    // viewDepth(m): ここから効果が薄れ始め
static const float kPomFadeEnd   = 36.0;    // ここで 0

float2 ParallaxOffset(uint flags, float2 uv, float2 gx, float2 gy, float3 worldPos, float3 worldNormal, float viewDepth)
{
    // ★導関数は早期 return より前に作る（ピクセルごとに分かれる分岐の中で ddx すると 4 画素組の値が不定になる）
    const float3 dp1 = ddx(worldPos);
    const float3 dp2 = ddy(worldPos);
    const float fade = 1.0 - saturate((viewDepth - kPomFadeStart) / (kPomFadeEnd - kPomFadeStart));
    float scale = (float(flags >> 24) / 255.0) * kPomScaleMax * fade;
    if (scale < 1e-4) return float2(0.0, 0.0);

    const float3 N   = normalize(worldNormal);
    const float3 V   = normalize(cameraPos - worldPos);
    const float  ndv = dot(N, V);
    // かすめ角（約 2.5°〜6°）で効果を 0 へ。裏向き（ndv<=0）は何もしない。
    scale *= saturate((ndv - 0.04) / 0.10);
    if (scale < 1e-4) return float2(0.0, 0.0);

    // UV の面内勾配（Schüler の cotangent frame と同じ導出。gradU·dp1 = gx.x, gradU·dp2 = gy.x）
    const float3 dp2perp = cross(dp2, N);
    const float3 dp1perp = cross(N, dp1);
    const float  det = dot(dp1, dp2perp);
    if (abs(det) < 1e-12) return float2(0.0, 0.0);
    const float3 gradU = (dp2perp * gx.x + dp1perp * gy.x) / det;
    const float3 gradV = (dp2perp * gx.y + dp1perp * gy.y) / det;

    // 最上面→最下面まで視線が横へずれる UV 量（scale は m）。ndv の下限で発散を抑える。
    const float3 Vt = V - N * ndv;
    const float2 deltaTotal = float2(dot(Vt, gradU), dot(Vt, gradV)) * (scale / max(ndv, 0.12));

    const int maxSteps = (int)(8u << ((flags >> 6) & 3u));
    // 真上でも最低 maxSteps/4（8 未満にしない）。少なすぎると層が段になって見える（真上の斜め縁で顕著）。
    const float minSteps = max(0.25 * (float)maxSteps, 8.0);
    const int steps = (int)max(lerp(minSteps, (float)maxSteps, (1.0 - ndv) * fade), minSteps);
    const float layerDepth = 1.0 / (float)steps;
    const float2 deltaUV = deltaTotal * layerDepth;

    float2 curUV       = uv;
    float  curLayer    = 0.0;
    float  curDepthMap = 1.0 - g_height.SampleGrad(g_sampler, curUV, gx, gy).r;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        if (curLayer >= curDepthMap) break;
        curUV      -= deltaUV;
        curLayer   += layerDepth;
        curDepthMap = 1.0 - g_height.SampleGrad(g_sampler, curUV, gx, gy).r;
    }
    // 交差した区間 [prevUV(面の上), curUV(面の下)] を詰める。
    // 標準の POM は区間を 1 回線形補間するだけだが、かすめ角では 1 ステップが数 mm 〜 cm になって石の縁が層状の段差に
    // なる。そこで 4 回の二分探索（高さタップ 4 回。区間を 1/16 に絞る）に置き換え、最後に 1 回線形補間する。
    float2 loUV = curUV + deltaUV;                 // 面の上側
    float2 hiUV = curUV;                           // 面の下側
    float  loL  = curLayer - layerDepth;
    float  hiL  = curLayer;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const float2 midUV = 0.5 * (loUV + hiUV);
        const float  midL  = 0.5 * (loL + hiL);
        const float  midD  = 1.0 - g_height.SampleGrad(g_sampler, midUV, gx, gy).r;
        if (midL >= midD) { hiUV = midUV; hiL = midL; }
        else              { loUV = midUV; loL = midL; }
    }
    const float after  = (1.0 - g_height.SampleGrad(g_sampler, hiUV, gx, gy).r) - hiL;   // <= 0 付近（面の下）
    const float before = (1.0 - g_height.SampleGrad(g_sampler, loUV, gx, gy).r) - loL;   // >= 0 付近（面の上）
    const float wgt    = after / min(after - before, -1e-5);
    return lerp(hiUV, loUV, saturate(wgt)) - uv;
}

// 通常は Sample、POM のときだけ元の UV の微分で SampleGrad（ずらした UV の微分は段差でミップが暴れるため）。
// pom は b2 由来＝ドロー内で一様なので Sample の導関数は壊れない。
float4 SampleMat(Texture2D t, float2 uv, bool pom, float2 gx, float2 gy)
{
    [branch]
    if (pom) return t.SampleGrad(g_sampler, uv, gx, gy);
    return t.Sample(g_sampler, uv);
}

// ===== シェーディング尾部（デカール → 影 → ライティング → IBL → 自己発光 → デバッグ）=====
// ★実体は ForwardShade.hlsli（Forward / ForwardSkinned / Terrain / マテリアルグラフで共有）。
//   上のリソース宣言・Lighting.hlsli・ShadowPcss.hlsli・DecalApply.hlsli より後で include すること。
//   SelectCascade / CalcShadow もそこにある。
#include "ForwardShade.hlsli"

float4 PSMain(PSInput input) : SV_TARGET
{
    // UV スクロール/連番アニメ (b2)。無効時は uvScaleOffset=(1,1,0,0) なので恒等変換
    float2 uv = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;

    // ===== 視差オクルージョン（pbrFlags bit5。ALPHA_TEST バリアントでは使わない = 深度プリパスの clip と食い違うため）=====
    // ★無効（既定）のときは uv も後段のサンプルも従来と同一（[branch] で丸ごと飛ぶ）。
#ifdef ALPHA_TEST
    const bool pom = false;
#else
    const bool pom = (pbrFlags & 32u) != 0u;
#endif
    const float2 uvGx = ddx(uv);
    const float2 uvGy = ddy(uv);
    [branch]
    if (pom)
        uv += ParallaxOffset(pbrFlags, uv, uvGx, uvGy, input.worldPos, input.worldNormal, input.viewDepth);

    // ===== マテリアル評価（旧経路: 4 枚のテクスチャ + b2）=====
    // ここより後ろ（デカール以降）は UnoShadeLighting → UnoShadeFinish。グラフ材質は UnoMatEval でここを置き換える。

    // Albedo
    const float3 objectTint = float3((packedTint >> 16) & 0xFF,
                                     (packedTint >>  8) & 0xFF,
                                     (packedTint      ) & 0xFF) / 255.0;
    float4 albedo4 = SampleMat(g_albedo, uv, pom, uvGx, uvGy) * input.color * float4(objectTint, 1.0);
    float3 albedo = albedo4.rgb;

    // ===== 透明（アルファクリップ / アルファブレンド）=====
    // ALPHA_TEST は -D ALPHA_TEST=1 でコンパイルした **専用バリアント**でのみ有効
    // （clip を含む PS は early-Z が効かなくなるので、不透明パスの PS には絶対に混ぜない）。
    // cutoff は b2 の pbrFlags bit8..15 に 8bit 量子化して載っている。
#ifdef ALPHA_TEST
    {
        const float cutoff = float((pbrFlags >> 8) & 0xFF) / 255.0;
        clip(albedo4.a - cutoff);
    }
#endif
    // 不透明度。BLEND 用 PSO でのみ意味を持つ（不透明 PSO はブレンド無効なので無視される）。
    // ★不透明時は 255 が入る。0 が入ると全部消えるので CPU 側は必ず書くこと。
    const float gOpacity = float((packedTint >> 24) & 0xFF) / 255.0;

    // Normal
    float3 N;
    if (pbrFlags & 1u)
    {
        float3 normalSample = SampleMat(g_normalMap, uv, pom, uvGx, uvGy).rgb;
        N = PerturbNormal(input.worldNormal, input.worldTangent, input.tangentW, normalSample);
    }
    else
    {
        N = normalize(input.worldNormal);
    }
#ifdef ALPHA_TEST
    // ★両面の薄い面（葉カード・柵・金網）は、裏から見たら法線を視線側へ返す。
    //   フォワードは全部カリング無しで描くが法線は返していなかった。閉じた形では裏面が隠れるので
    //   問題にならないが、カードは裏面が見える → N·V < 0 で Fresnel が 1 に張り付き、葉が全部
    //   空を映して白い霜のように光っていた（Bistro の木。2026-10-14、パストレーサーと比べて確認）。
    //   判定は SV_IsFrontFace ではなく幾何法線と視線の向きで行う（鏡映 Transform で巻き方向が逆でも正しい。
    //   アルファテストの面だけなので、滑らかな閉じた形の輪郭でひっくり返る心配は無い）。
    if (dot(input.worldNormal, cameraPos - input.worldPos) < 0.0) N = -N;
#endif

    // Metallic / Roughness（テクスチャ × スライダー値でスケーリング）
    // ★ラフネスの下限 0.04 は UnoShadeLighting の中で掛かる。
    float metallic, roughness;
    // マテリアル AO（bit4 = MR の R が AO。bit16..23 = 強度）。1.0 = 遮蔽なし＝AO を持たないモデルは従来と同じ。
    float materialAo = 1.0;
    if (pbrFlags & 2u)
    {
        float4 mr = SampleMat(g_metalRoughness, uv, pom, uvGx, uvGy);
        roughness = mr.g * defaultRoughness;
        metallic  = mr.b * defaultMetallic;
        if (pbrFlags & 16u)
            materialAo = 1.0 + (float((pbrFlags >> 16) & 0xFF) / 255.0) * (mr.r - 1.0);
    }
    else
    {
        metallic  = defaultMetallic;
        roughness = defaultRoughness;
    }

    UnoSurface s = UnoSurfaceDefault();
    s.baseColor = albedo;
    s.metallic  = metallic;
    s.roughness = roughness;
    s.ao        = materialAo;   // 間接光だけに掛かる（ForwardShade.hlsli: ao *= s.ao）

    const UnoShadeInput si = UnoMakeShadeInput(input.worldPos, input.worldNormal,
                                               input.positionSV, input.viewDepth);
    const float3 lit = UnoShadeLighting(s, N, si);

    // ===== 自己発光（emissive）=====
    // ★影・AO・ライティングを一切通さずそのまま足す（デカールの emissive と同じ扱い）。
    //   リニア HDR のまま出すので、1 を超えた分はブルームがそのまま拾う。
    //   ★サンプルはライティングの後ろに置く（外出し前の命令順を保つため。UnoShadeForward と結果は同じ）。
    const float3 color = lit + UnpackEmissive(packedEmissive, uv);

    return float4(UnoShadeFinish(color, si), albedo4.a * gOpacity);
}
