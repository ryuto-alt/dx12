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

// ============================================================================
// シェーディング側のサンプル（段階1）。プローブ更新は使わないので、
// フォワード PS からも同じ配置ルールで引けるようここに置く。
// ============================================================================

// アトラスのテクセル数（C++ の DdgiVolume::AtlasSize と同じ式）
float2 DdgiAtlasSize(uint3 counts)
{
    return float2(counts.x * counts.y * DDGI_PROBE_TILE, counts.z * DDGI_PROBE_TILE);
}

// 1 プローブの八面体タイルから dir 方向の irradiance を引く。
// ★ボーダー（周囲 1 テクセル）はまだ埋めていないので、バイリニアの 2x2 が内側 6x6 から
//   はみ出さないよう [0.5, 5.5] にクランプする。八面体の継ぎ目がわずかに引き伸ばされるが、
//   6x6 の低周波なので目視では出ない。ボーダーコピーは段階2で BlendCS 側に入れる。
float3 DdgiFetchProbe(Texture2D<float4> atlas, SamplerState samp,
                      uint probeIndex, uint3 counts, float3 dir)
{
    const uint2  tile  = DdgiProbeTileOrigin(probeIndex, counts);
    const float2 oct01 = DdgiOctEncode(dir) * 0.5 + 0.5;                  // [-1,1] -> [0,1]
    const float2 inner = clamp(oct01 * float(DDGI_IRRADIANCE_TEXELS),
                               0.5, float(DDGI_IRRADIANCE_TEXELS) - 0.5);
    // BlendCS は tile+(1,1) を内側の原点として書いている（DdgiProbeUpdate.hlsl）。
    const float2 uv = (float2(tile) + 1.0 + inner) / DdgiAtlasSize(counts);
    return atlas.SampleLevel(samp, uv, 0).rgb;
}

// worldPos / N の位置で周囲 8 プローブをトライリニア + 法線重みで混ぜる（論文 §4.4）。
// 戻り値はコサイン加重の平均放射輝度で、IBL の irradiance キューブと同じ単位。
//   ＝ albedo を掛ければそのまま拡散間接光になる（π の補正は不要）。
// confidence は格子の境界フェード（外側は 0）。呼び出し側はこれで lerp する。
float3 DdgiSampleIrradiance(Texture2D<float4> atlas, SamplerState samp,
                            float3 worldPos, float3 N,
                            float3 originWS, float3 spacing, uint3 counts,
                            float normalBias, out float confidence)
{
    confidence = 0.0;
    const float3 gridMax = originWS + float3(counts - 1) * spacing;

    // 格子の外は寄与させない。半セルかけて滑らかに落とす（硬い境界線を出さないため）。
    const float3 outside  = max(originWS - worldPos, worldPos - gridMax);
    const float  worstOut = max(outside.x, max(outside.y, outside.z));
    const float  falloff  = max(max(spacing.x, max(spacing.y, spacing.z)) * 0.5, 1e-4);
    const float  fade     = saturate(1.0 - max(worstOut, 0.0) / falloff);
    if (fade <= 0.0) return 0.0;

    // サンプル位置を法線方向へ押し出す（壁際で裏側のプローブを引くのを減らす）。
    // ★段階2 の Chebyshev 可視性テストが入るまでは、これがリーク対策の主な手段。
    const float3 p = worldPos + N * normalBias;
    const float3 g = (p - originWS) / max(spacing, 1e-4);
    const int3   lo = clamp(int3(floor(g)), int3(0, 0, 0), int3(counts) - 1);
    const float3 f  = saturate(g - float3(lo));

    float3 sum  = 0.0;
    float  wsum = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int3   off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3   c   = clamp(lo + off, int3(0, 0, 0), int3(counts) - 1);
        const float3 t   = lerp(1.0 - f, f, float3(off));
        float w = t.x * t.y * t.z;

        // 法線重み（"wrap shading"）。真後ろのプローブをほぼ捨てる。+0.2 は
        // 8 個すべてが背面になった時に真っ黒へ落ちないための下駄（論文と同じ）。
        const float3 probePos = originWS + float3(c) * spacing;
        const float3 dir  = normalize(probePos - p);
        const float  wrap = dot(dir, N) * 0.5 + 0.5;
        w *= wrap * wrap + 0.2;
        if (w < 1e-6) continue;

        sum  += DdgiFetchProbe(atlas, samp, DdgiProbeIndex(uint3(c), counts), counts, N) * w;
        wsum += w;
    }
    if (wsum <= 0.0) return 0.0;

    confidence = fade;
    return sum / wsum;
}

#endif // DDGI_COMMON_HLSLI
