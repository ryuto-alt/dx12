// ============================================================================
// UnoCustom.hlsli — カスタムメッシュシェーダーの共通土台
//
//   assets/shaders/*.hlsl（MeshRenderer::shaderPath に割り当てる自作シェーダー）から
//     #include "UnoCustom.hlsli"
//   と書くだけで、b0/b1 の定数・頂点レイアウト・よく使う数学関数が揃う。
//
// ■ なぜ要るか
//   これまで各シェーダーが cbuffer を手で書き写していた。b0 の先頭数個だけなら
//   それでも動くが、time より後ろ（cameraPos / 光の色 / 影のパラメータ）に届くには
//   448 バイト分の宣言を正しい順序で並べる必要がある。1 つでもズレると
//   **エラーは出ずに値だけが化ける**（cbuffer はオフセットで対応が決まるため）。
//   その結果「時間は使えるがカメラ位置は使えない」のが実質の上限になっていた。
//
// ■ 大原則
//   cbuffer の並びは shaders/forward/Lighting.hlsli と【バイト単位で一致】させること。
//   ここを触るときは必ず両方を同時に直す。片方だけ変えると全カスタムシェーダーが
//   静かに壊れる（絵が出ないのではなく、値がズレた絵が出る）。
//
// ■ 使い方の最小形
//     #include "UnoCustom.hlsli"
//     PSInput VSMain(VSInput i) { return UnoDefaultVS(i); }
//     float4  PSMain(PSInput i) : SV_TARGET { return float4(1,0,0,1); }
// ============================================================================
#ifndef UNO_CUSTOM_HLSLI
#define UNO_CUSTOM_HLSLI

// ---------------------------------------------------------------------------
// b0: オブジェクトごと（ルート定数 40 DWORD）
//   effectValue 以降の 8 float が「作者の自由枠」。Inspector の Shader セクション、
//   Lua の scene:setMeshEffect / setMeshParams、Trigger の SetShaderParam から動かせる。
// ---------------------------------------------------------------------------
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;             //   0
    float4x4 model;           //  64
    float    effectValue;     // 128  汎用の進捗/強度（0..1 で使うことが多い）
    float3   shaderParamsB;   // 132  自由枠その2（旧 _pad）
    float4   shaderParams;    // 144  自由枠その3（意味はシェーダー次第）
};

// ---------------------------------------------------------------------------
// b1: フレームごと
//   ★ Lighting.hlsli と同じ並び。cameraPos(offset 448) まで届かせるために
//     途中の使わない領域も宣言しておく必要がある（省略するとオフセットがズレる）。
// ---------------------------------------------------------------------------
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;                    //   0
    float4x4 proj;                    //  64
    float3   lightDir;                // 128  太陽の向き（正規化済み・光が進む向き）
    float    time;                    // 140  秒。起動からの経過
    float3   lightColor;              // 144
    float    ambientStrength;         // 156
    float4x4 _unoCascadeViewProj[4];  // 160  影のカスケード行列（通常は使わない）
    float4   _unoCascadeSplitsView;   // 416
    float4   _unoShadowParams;        // 432
    float3   cameraPos;               // 448  ワールド空間のカメラ位置
    float    _unoAoEnabled;           // 460
};

// ---------------------------------------------------------------------------
// マテリアルのテクスチャ（Inspector / D&D で割り当てたもの）
// ---------------------------------------------------------------------------
Texture2D    g_albedo        : register(t0);
Texture2D    g_normalMap     : register(t1);
Texture2D    g_metalRoughness: register(t2);
SamplerState g_sampler       : register(s0);

// ---------------------------------------------------------------------------
// 頂点入出力。エンジンが流す頂点レイアウトと一致（変更不可）。
// ---------------------------------------------------------------------------
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
    float4 positionSV  : SV_POSITION;
    float3 worldNormal : NORMAL;
    float3 worldPos    : TEXCOORD1;
    float2 texCoord    : TEXCOORD0;
};

// そのまま通すだけの頂点シェーダー。頂点を動かさないシェーダーはこれで足りる。
PSInput UnoDefaultVS(VSInput i)
{
    PSInput o;
    o.positionSV  = mul(float4(i.position, 1.0), mvp);
    o.worldNormal = normalize(mul(i.normal, (float3x3)model));
    o.worldPos    = mul(float4(i.position, 1.0), model).xyz;
    o.texCoord    = i.texCoord;
    return o;
}

// 頂点を動かした後に使う版（法線は呼び出し側で作って渡す）。
PSInput UnoVSFromLocal(float3 localPos, float3 localNormal, float2 uv)
{
    PSInput o;
    o.positionSV  = mul(float4(localPos, 1.0), mvp);
    o.worldNormal = normalize(mul(localNormal, (float3x3)model));
    o.worldPos    = mul(float4(localPos, 1.0), model).xyz;
    o.texCoord    = uv;
    return o;
}

// ローカル頂点をワールドへ。波などを「ワールド座標で」作るときの入口。
// ワールドで作ると、同じシェーダーを貼った板を並べても継ぎ目が出ない。
float3 UnoLocalToWorld(float3 localPos)
{
    return mul(float4(localPos, 1.0), model).xyz;
}

// ワールド座標をクリップ空間へ。頂点をワールドで動かしたら mvp は使えないので必ずこちら。
//   規約: 行ベクトル × 転置済み行列（エンジンの Forward.hlsl と同じ）
float4 UnoWorldToClip(float3 worldPos)
{
    return mul(mul(float4(worldPos, 1.0), view), proj);
}

// ワールド座標で動かした頂点から PSInput を組む。
PSInput UnoVSFromWorld(float3 worldPos, float3 worldNormal, float2 uv)
{
    PSInput o;
    o.positionSV  = UnoWorldToClip(worldPos);
    o.worldNormal = normalize(worldNormal);
    o.worldPos    = worldPos;
    o.texCoord    = uv;
    return o;
}

// ---------------------------------------------------------------------------
// よく使う数学
// ---------------------------------------------------------------------------
float  UnoHash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float UnoNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = UnoHash21(i),               b = UnoHash21(i + float2(1, 0));
    float c = UnoHash21(i + float2(0, 1)), d = UnoHash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// オクターブを重ねたノイズ。octaves は定数で渡すこと（ループ展開のため）。
float UnoFbm(float2 p, int octaves)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < octaves; ++i) { s += UnoNoise(p) * a; p *= 2.03; a *= 0.5; }
    return s;
}

// 視線と法線のなす角による反射率。水・ガラス・被膜の説得力はほぼこれで決まる。
float UnoFresnel(float3 normal, float3 viewDir, float f0, float power)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - saturate(dot(normal, viewDir))), power);
}

// 太陽のスペキュラ（Blinn-Phong）。水面のギラつき用。
float UnoSunSpecular(float3 normal, float3 viewDir, float shininess)
{
    float3 h = normalize(viewDir - lightDir);
    return pow(saturate(dot(normal, h)), shininess);
}

// 太陽の「色味」だけを取り出す（最大成分で割って 0..1 に正規化）。
//
// ★lightColor は HDR の強度が乗った値。反射色や空の色にそのまま掛けると、
//   太陽の強さを上げただけで一面が白く飛ぶ（実際に踏んだ: 海が真っ白になった）。
//   色として使いたいところは必ずこちらを通すこと。明るさは別途スカラーで足す。
float3 UnoSunTint()
{
    float m = max(max(lightColor.r, lightColor.g), lightColor.b);
    return (m > 1e-4) ? (lightColor / m) : float3(1.0, 1.0, 1.0);
}

// ゲルストナー波。戻り値 .xyz = 変位、.w = 波の峰らしさ(0..1、白波/泡に使う)。
//   dir      : 進行方向（xz 平面。正規化しなくてよい）
//   steepness: 0..1。1 に近いほど尖る（1 を超えると自己交差してループする）
float4 UnoGerstner(float3 worldPos, float2 dir, float wavelength, float steepness, float speed, float t)
{
    float2 d = normalize(dir);
    float  k = 6.28318530718 / max(wavelength, 0.001);
    float  a = steepness / k;
    float  f = k * (dot(d, worldPos.xz) - speed * t);
    float  c = cos(f), s = sin(f);
    // 峰は cos が 1 に近いところ。泡の乗り方を素直にするため 0..1 に均す。
    return float4(d.x * a * c, a * s, d.y * a * c, saturate(c * 0.5 + 0.5));
}

#endif // UNO_CUSTOM_HLSLI
