#ifndef DDGI_COMMON_HLSLI
#define DDGI_COMMON_HLSLI
// DdgiCommon.hlsli — DDGI（Dynamic Diffuse Global Illumination）の共通定義
//
// 出典: Majercik, Guertin, Nowrouzezahrai, McGuire,
//       "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields",
//       JCGT Vol.8 No.2 Article 1 (2019)  https://jcgt.org/published/0008/02/01/
//
// ★NVIDIA の RTXGI SDK は使わない。v2.0 で DDGI は削除済み（SHaRC に置き換え）、
//   v1.x は 2023-05 で休眠、しかも独占ライセンス。そして SDK が提供するのは
//   probe blending / relocation / classification だけで、**レイトレはもともとアプリ側の責任**
//   （DDGIVolume.md: "The application is responsible for tracing rays for DDGIVolume probes"）。
//   このエンジンは inline RayQuery とバインドレスのヒット読み取りが既にあるので、
//   論文から自前実装するほうが速いし、規約も自前のものに合わせられる。
//
// ★なぜ DDGI か（他候補を落とした理由）
//   - ReSTIR GI / SHaRC / Surfel は「1spp のノイズ + 専用デノイザ」が前提。
//     デノイザの自作は GI 本体と同規模の別プロジェクトになる。DDGI の出力は
//     時間平滑化された低周波なのでデノイザが要らない。
//   - SHaRC は「既にパストレーサがある」前提の加速機構であって、
//     ラスタライズエンジンに間接光を足す道具ではない。
//   - Radiance Cascades は著者自身が 3D では "dealbreaker" と書いており、
//     実用実装はスクリーンスペース限定＝既存の SSGI と同じ土俵。

// 1 プローブあたりのレイ本数。論文の既定は 256 だが、まずは 64 で回して質を見る。
#define DDGI_RAYS_PER_PROBE 64

// irradiance アトラスの 1 プローブぶんのテクセル数（内側）。周囲 1 テクセルはボーダー。
// ボーダーはバイリニア補間が隣のプローブを舐めないようにするために必須（論文 §4.3）。
#define DDGI_IRRADIANCE_TEXELS 6
#define DDGI_PROBE_TILE       (DDGI_IRRADIANCE_TEXELS + 2)   // 8

struct DdgiConstants
{
    float3 originWS;      // プローブ格子の原点（最小コーナー）
    float  rayLength;     // プローブレイの最大距離(m)
    float3 spacing;       // プローブ間隔(m)
    float  hysteresis;    // 履歴の保持率（0.97 前後）
    uint3  probeCounts;   // 各軸のプローブ数
    uint   frameIndex;    // レイ方向を回すための連番
    float  intensity;     // 出力の強さ
    float  normalBias;    // レイ始点の法線オフセット(m)
    float  pad0, pad1;
};

// プローブの 3D 添字 → 通し番号
uint DdgiProbeIndex(uint3 c, uint3 counts)
{
    return c.x + c.y * counts.x + c.z * counts.x * counts.y;
}

uint3 DdgiProbeCoord(uint index, uint3 counts)
{
    uint3 c;
    c.x = index % counts.x;
    c.y = (index / counts.x) % counts.y;
    c.z = index / (counts.x * counts.y);
    return c;
}

float3 DdgiProbePosition(uint3 c, DdgiConstants k)
{
    return k.originWS + float3(c) * k.spacing;
}

// アトラス上のプローブタイルの左上テクセル（ボーダー込み）。
// プローブは (counts.x * counts.y) 列 × counts.z 行 に並べる。
uint2 DdgiProbeTileOrigin(uint index, uint3 counts)
{
    const uint perRow = counts.x * counts.y;
    return uint2((index % perRow) * DDGI_PROBE_TILE, (index / perRow) * DDGI_PROBE_TILE);
}

// ---- 八面体マッピング（Cigolle et al. / 論文 §4.2）----
// [-1,1]^2 の正方形と単位球面の全方向を 1 対 1 で対応させる。
// キューブマップと違い継ぎ目が 1 本で済み、面ごとのテクスチャも要らない。
float2 DdgiOctEncode(float3 n)
{
    const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    float2 o = n.xy * (1.0 / max(l1, 1e-8));
    if (n.z < 0.0)
        o = (1.0 - abs(o.yx)) * float2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    return o;
}

float3 DdgiOctDecode(float2 f)
{
    float3 n = float3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// タイル内のテクセル座標(0..DDGI_IRRADIANCE_TEXELS-1) → 方向
float3 DdgiTexelDirection(uint2 texel)
{
    // テクセル中心を [-1,1] へ。
    const float2 uv = (float2(texel) + 0.5) / float(DDGI_IRRADIANCE_TEXELS);
    return DdgiOctDecode(uv * 2.0 - 1.0);
}

// ---- プローブレイの方向（球面フィボナッチ）----
// 論文は確率的に回転させた球面フィボナッチを使う。フレームごとに回すことで
// レイ本数が少なくても時間方向で埋まる。
float3 DdgiSphericalFibonacci(uint i, uint n)
{
    const float PHI = 1.6180339887498948482;
    const float phi = 6.2831853071795864769 * frac(float(i) * (PHI - 1.0));
    const float z   = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float r   = sqrt(saturate(1.0 - z * z));
    return float3(cos(phi) * r, sin(phi) * r, z);
}

// フレームごとの回転（軸角。低食い違い列で回す）
float3x3 DdgiRayRotation(uint frameIndex)
{
    // 黄金比で回す 3 つの角度。厳密なランダム回転でなくてよく、
    // 「毎フレーム同じ方向にならない」ことだけが要件。
    const float a = frac(float(frameIndex) * 0.6180339887) * 6.2831853;
    const float b = frac(float(frameIndex) * 0.7548776662) * 6.2831853;
    const float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
    const float3x3 ry = float3x3( ca, 0, sa,   0, 1, 0,  -sa, 0, ca);
    const float3x3 rx = float3x3( 1, 0, 0,   0, cb, -sb,   0, sb, cb);
    return mul(ry, rx);
}

#endif // DDGI_COMMON_HLSLI
