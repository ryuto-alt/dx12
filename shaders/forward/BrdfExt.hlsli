// BrdfExt.hlsli - 拡張シェーディング（マルチスキャッタ補償 / クリアコート / 異方性 GGX / シーン(布)）の純関数。
//
//   リソースも cbuffer も宣言しない（フォワード・VG resolve・パストレーサーの 3 経路から include する。
//   パストレーサーは「フォワードと同じ式」を使うことで、ラスタと PT の比較が「アルゴリズム差だけ」になる）。
//   式の出どころ（公開文献から自前で実装。他エンジンのコードは写していない）:
//     ・異方性 GGX の D と height-correlated の可視項 … Burley 2012 / Heitz 2014
//     ・異方性の α … glTF KHR_materials_anisotropy（αt = mix(α, 1, s²), αb = α）
//     ・シーン … Charlie 分布と可視項（Estevez & Kulla 2017 "Production Friendly Microfacet Sheen BRDF"）
//     ・クリアコート … glTF KHR_materials_clearcoat の層モデル（下層を 1 - coat·Fc(N_c·V) で減衰、F0 = 0.04 の GGX を足す）
//     ・マルチスキャッタ … Kulla & Conty 2017 / Fdez-Agüera 2019 の「方向アルベド E で割って持ち上げる」近似
//       spec *= 1 + F0·(1/E − 1)（E は split-sum LUT の A + B ＝ F0 = 1 の方向アルベド）
//   ★CPU 側の写し: tests/shading_brdf_test.cpp（相反性・エネルギー上限の数値テスト）。式を変えたら両方を直すこと。

#ifndef UNO_BRDF_EXT_HLSLI
#define UNO_BRDF_EXT_HLSLI

static const float UNO_BX_PI = 3.14159265359;

// 機能ビット（src/renderer/ShadingExt.h の kShadingExt* と同じ値）
#define UNO_EXT_COAT     1u
#define UNO_EXT_ANISO    2u
#define UNO_EXT_SHEEN    4u
#define UNO_EXT_SPECULAR 8u
#define UNO_EXT_MAGIC      0xA5000000u
#define UNO_EXT_MAGIC_MASK 0xFF000000u

float UnoBxMax3(float3 v) { return max(v.x, max(v.y, v.z)); }

// Schlick（F90 付き）。F90 = 1 なら PBR.hlsli の FresnelSchlick と同じ値。
float3 UnoBxFresnel(float3 F0, float F90, float cosTheta)
{
    return F0 + (F90 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// 等方 GGX（PBR.hlsli の DistributionGGX と同じ式。NoH を直接受ける版）
float UnoBxD_GGX(float NoH, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    d = UNO_BX_PI * d * d;
    return a2 / max(d, 0.0000001);
}

// Smith（Schlick-GGX、直接光の k = (r+1)^2/8。PBR.hlsli の GeometrySchlickGGX と同じ）
float UnoBxG1(float NoX, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return NoX / (NoX * (1.0 - k) + k);
}

// ---- 異方性 GGX ----------------------------------------------------------------------------
// 知覚的ラフネスと強さ（-1..1）から (αt, αb)。負の強さは呼び出し側で接線と従法線を入れ替えて |s| を渡す。
float2 UnoBxAnisoAlpha(float roughness, float strength)
{
    const float a = roughness * roughness;
    const float s = saturate(abs(strength));
    return float2(lerp(a, 1.0, s * s), a);
}

// D(h) = 1 / (π αt αb (ToH²/αt² + BoH²/αb² + NoH²)²)
float UnoBxD_GGXAniso(float NoH, float ToH, float BoH, float at, float ab)
{
    const float a2 = at * ab;
    const float3 v = float3(ab * ToH, at * BoH, a2 * NoH);
    const float v2 = dot(v, v);
    const float w2 = a2 / max(v2, 1e-20);
    return a2 * w2 * w2 / UNO_BX_PI;
}

// 異方性の遮蔽 G1。方向 X の方位で投影した α_o = sqrt(αt²cos²φ + αb²sin²φ)（Smith の異方版の作法）を
//   知覚的ラフネス sqrt(α_o) に戻し、等方の直接光と同じ Schlick-GGX（k = (r+1)²/8）で評価する。
//   ★等方（αt = αb）では UnoBxG1 と完全に同じ値＝強さ 0 へ連続（異方の葉だけ別の G に飛ばない）。
//     マルチスキャッタの補償（LUT .a = この G の方向アルベド）とも同じ遮蔽モデルになる。
float UnoBxG1Aniso(float NoX, float ToX, float BoX, float at, float ab)
{
    const float s2 = max(1.0 - NoX * NoX, 0.0);
    const float a2 = (s2 > 1e-6) ? (at * at * ToX * ToX + ab * ab * BoX * BoX) / s2 : at * ab;
    const float r  = sqrt(sqrt(max(a2, 1e-12)));
    return UnoBxG1(NoX, r);
}

// 異方性のマルチスキャッタ補償の LUT を引く座標: 粗さ = 等価ラフネス sqrt(sqrt(αt·αb))、N·V = 異方なほど 0.5 へ寄せる。
//   異方の葉のエネルギー損失は視線の角度にほとんど依らない（CPU 写しで 0.5〜0.68）ので、等方の E の強い N·V 依存を
//   持ち込まない。強さ 0（αt = αb）では元の (N·V, 粗さ) と同じ＝連続。白い炉で 0.92〜1.22（tests/shading_brdf_test.cpp）。
float UnoBxAnisoEqRoughness(float at, float ab) { return sqrt(sqrt(at * ab)); }
float UnoBxAnisoMsNoV(float NoV, float at, float ab) { return lerp(NoV, 0.5, 1.0 - ab / max(at, 1e-6)); }

// ---- シーン（Charlie）------------------------------------------------------------------------
float UnoBxD_Charlie(float sheenRoughness, float NoH)
{
    const float r    = max(sheenRoughness, 0.000001);
    const float aG   = r * r;
    const float invR = 1.0 / aG;
    const float sin2h = max(1.0 - NoH * NoH, 0.0);
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * UNO_BX_PI);
}

// Estevez & Kulla の λ のフィット（5 係数を (1-αg)² で補間）
float UnoBxSheenL(float x, float aG)
{
    const float t = (1.0 - aG) * (1.0 - aG);
    const float a = lerp(21.5473, 25.3245, t);
    const float b = lerp(3.82987, 3.32435, t);
    const float c = lerp(0.19823, 0.16801, t);
    const float d = lerp(-1.97760, -1.27393, t);
    const float e = lerp(-4.32054, -4.85967, t);
    return a / (1.0 + b * pow(max(x, 1e-6), c)) + d * x + e;
}
float UnoBxSheenLambda(float cosTheta, float aG)
{
    const float c = abs(cosTheta);
    return (c < 0.5) ? exp(UnoBxSheenL(c, aG))
                     : exp(2.0 * UnoBxSheenL(0.5, aG) - UnoBxSheenL(1.0 - c, aG));
}
// V = 1 / ((1 + Λ(V) + Λ(L)) · 4 NoV NoL)（上限 1。グレージングの発散を止める）
float UnoBxV_Charlie(float NoL, float NoV, float sheenRoughness)
{
    const float r  = max(sheenRoughness, 0.000001);
    const float aG = r * r;
    const float den = (1.0 + UnoBxSheenLambda(NoV, aG) + UnoBxSheenLambda(NoL, aG)) * (4.0 * NoV * NoL);
    return saturate(1.0 / max(den, 1e-7));
}

// ---- マルチスキャッタの補償係数 ----------------------------------------------------------------
// E = F0 = 1 の方向アルベド。1 + F0·(1/E − 1)。E の下限で発散を止める。
//   ★E は「そのスペキュラが使う遮蔽 G で積分したもの」を渡すこと（IntegrateBRDF.hlsl の LUT）:
//     直接光（k = (r+1)²/8 の Schlick-GGX）= LUT .a / IBL の split-sum（k = α/2）= LUT .r + .g
float3 UnoBxMultiScatterComp(float3 F0, float E)
{
    return 1.0 + F0 * (1.0 / max(E, 0.05) - 1.0);
}

// ---- 異方性の IBL 用の曲げた反射ベクトル（反射が伸びる向きへ法線を寄せる近似）----------------------
// anisoT = 伸ばす向き（接線を回したもの）, anisoB = cross(N, anisoT)
float3 UnoBxAnisoBentReflect(float3 N, float3 V, float3 anisoB, float strength, float roughness)
{
    float3 bent = cross(anisoB, V);
    bent = cross(bent, anisoB);
    const float l2 = dot(bent, bent);
    if (!(l2 > 1e-12)) return reflect(-V, N);
    bent = bent * rsqrt(l2);
    float a = 1.0 - saturate(abs(strength)) * (1.0 - roughness);
    a = a * a; a = a * a;
    bent = normalize(lerp(bent, N, a));
    return reflect(-V, bent);
}

// 接線 T（0 / 壊れていてもよい）と回転から、異方性の向き（anisoT, anisoB）。N は単位ベクトル。
//   T が使えないときは N から任意の直交基底を作る（向きは任意だが NaN にはしない）。
void UnoBxAnisoFrame(float3 N, float3 T, float tangentW, float rotation, float strength,
                     out float3 anisoT, out float3 anisoB)
{
    float3 Tp = T - dot(T, N) * N;
    const float l2 = dot(Tp, Tp);
    if (l2 > 1e-12)
    {
        Tp *= rsqrt(l2);
    }
    else
    {
        // Duff et al. 2017 の正規直交基底
        const float s = (N.z >= 0.0) ? 1.0 : -1.0;
        const float a = -1.0 / (s + N.z);
        const float b = N.x * N.y * a;
        Tp = float3(1.0 + s * N.x * N.x * a, s * b, -s * N.x);
    }
    const float3 Bp = cross(N, Tp) * ((tangentW < 0.0) ? -1.0 : 1.0);
    float sn, cs;
    sincos(rotation, sn, cs);
    anisoT = Tp * cs + Bp * sn;
    if (strength < 0.0) anisoT = Bp * cs - Tp * sn;   // 負の強さ = 90° 回した向きへ伸ばす
    anisoB = cross(N, anisoT);
}

#endif // UNO_BRDF_EXT_HLSLI
