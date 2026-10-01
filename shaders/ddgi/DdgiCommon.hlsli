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

// 距離モーメント（平均距離 / 距離の二乗平均）のタイル。★irradiance より高解像度にする。
// irradiance は低周波でよいが、可視性は「壁の縁」を表現しないといけないので 6x6 では粗すぎる
// （論文も距離側だけ 14x14 を使う）。
#define DDGI_DISTANCE_TEXELS  14
#define DDGI_DISTANCE_TILE    (DDGI_DISTANCE_TEXELS + 2)     // 16

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
    // 多重バウンス（段階3）の強さ。0 = 1 バウンスのみ。収束値は E/(1-ρ·b) なので 1 で打ち止め。
    float  bounceIntensity;
    float  pad1;
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
// ★tile はアトラスごとに違う（irradiance=8 / distance=16）ので引数で受ける。
uint2 DdgiProbeTileOrigin(uint index, uint3 counts, uint tile)
{
    const uint perRow = counts.x * counts.y;
    return uint2((index % perRow) * tile, (index / perRow) * tile);
}

// ボーダー（周囲 1 テクセル）が写し取るべき内側テクセルの座標。
// 八面体は「縁で反対側の縁へ折り返す」形なので、単純なクランプでは繋がらない。
// t はタイル内座標 0..texels+1、戻り値も同じ座標系（内側は 1..texels）。
int2 DdgiBorderSource(int2 t, int texels)
{
    const int N = texels;
    // 角は対角の内側テクセル
    if ((t.x == 0 || t.x == N + 1) && (t.y == 0 || t.y == N + 1))
        return int2(t.x == 0 ? N : 1, t.y == 0 ? N : 1);
    if (t.x == 0)     return int2(1,         N + 1 - t.y);
    if (t.x == N + 1) return int2(N,         N + 1 - t.y);
    if (t.y == 0)     return int2(N + 1 - t.x, 1);
    return                 int2(N + 1 - t.x, N);   // t.y == N+1
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

// タイル内のテクセル座標(0..texels-1) → 方向
float3 DdgiTexelDirection(uint2 texel, uint texels)
{
    // テクセル中心を [-1,1] へ。
    const float2 uv = (float2(texel) + 0.5) / float(texels);
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
float2 DdgiAtlasSize(uint3 counts, uint tile)
{
    return float2(counts.x * counts.y * tile, counts.z * tile);
}

// 八面体タイル内のサンプル UV。★段階2 でボーダーを埋めたのでクランプは要らなくなった。
//   oct01*texels ∈ [0, texels] → タイル内座標 [1, texels+1]、バイリニアの足 [0.5, texels+1.5]
//   ＝ ボーダー込みのタイル [0, texels+2] に完全に収まる。継ぎ目は折り返しで正しく繋がる。
float2 DdgiProbeUv(uint probeIndex, uint3 counts, float3 dir, uint texels, uint tile)
{
    const uint2  origin = DdgiProbeTileOrigin(probeIndex, counts, tile);
    const float2 oct01  = DdgiOctEncode(dir) * 0.5 + 0.5;                  // [-1,1] -> [0,1]
    return (float2(origin) + 1.0 + oct01 * float(texels)) / DdgiAtlasSize(counts, tile);
}

// 1 プローブの八面体タイルから dir 方向の irradiance を引く。
float3 DdgiFetchProbe(Texture2D<float4> atlas, SamplerState samp,
                      uint probeIndex, uint3 counts, float3 dir)
{
    return atlas.SampleLevel(
        samp, DdgiProbeUv(probeIndex, counts, dir, DDGI_IRRADIANCE_TEXELS, DDGI_PROBE_TILE),
        0).rgb;
}

// 距離モーメント（.x = 平均距離 / .y = 距離の二乗平均）。
float2 DdgiFetchProbeDistance(Texture2D<float2> atlas, SamplerState samp,
                              uint probeIndex, uint3 counts, float3 dir)
{
    return atlas.SampleLevel(
        samp, DdgiProbeUv(probeIndex, counts, dir, DDGI_DISTANCE_TEXELS, DDGI_DISTANCE_TILE),
        0).rg;
}

// 距離モーメントを詰めるときの上限（＝Chebyshev の効く範囲）。
// ★spacing の 2 倍。8 近傍プローブまでの最大距離は spacing*sqrt(3)≒1.73 倍なので、
//   これを下回る値でクランプすると「何も無い空間なのに遮蔽扱い」になる。
//   同時に、half(最大 65504) で二乗平均が溢れないための上限でもある
//   （rayLength は 10000 まで許されているので、生の距離をそのまま二乗すると壊れる）。
float DdgiMaxProbeDistance(float3 spacing)
{
    return max(spacing.x, max(spacing.y, spacing.z)) * 2.0;
}

// worldPos / N の位置で周囲 8 プローブをトライリニア + 法線重みで混ぜる（論文 §4.4）。
// 戻り値はコサイン加重の平均放射輝度で、IBL の irradiance キューブと同じ単位。
//   ＝ albedo を掛ければそのまま拡散間接光になる（π の補正は不要）。
// confidence は格子の境界フェード（外側は 0）。呼び出し側はこれで lerp する。
float3 DdgiSampleIrradiance(Texture2D<float4> atlas, Texture2D<float2> distAtlas,
                            SamplerState samp,
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

        // ---- Chebyshev 可視性テスト（段階2 / 論文 §4.4。Variance Shadow Maps と同型）----
        // プローブから見た「この点の方向」の平均距離 r と二乗平均 r2 を引く。
        // 点がプローブより手前なら遮蔽なし。奥なら、分散が小さい（＝その方向の壁が
        // はっきりしている）ほど強く遮蔽と判定して重みを落とす。
        // ★これが壁の裏へ光が回り込む「ライトリーク」を潰す本体。
        {
            const float3 toPoint = p - probePos;
            const float  distToProbe = length(toPoint);
            const float2 m = DdgiFetchProbeDistance(distAtlas, samp,
                                 DdgiProbeIndex(uint3(c), counts), counts,
                                 toPoint / max(distToProbe, 1e-6));
            if (distToProbe > m.x)
            {
                // 分散。abs は数値誤差で負に振れるのを潰すため。
                const float variance = abs(m.x * m.x - m.y);
                const float diff = distToProbe - m.x;
                float cheb = variance / (variance + diff * diff);
                // 3 乗して裾を切る（論文と同じ。中途半端な半遮蔽が滲むのを防ぐ）。
                cheb = max(cheb * cheb * cheb, 0.0);
                w *= cheb;
            }
        }

        if (w < 1e-6) continue;

        sum  += DdgiFetchProbe(atlas, samp, DdgiProbeIndex(uint3(c), counts), counts, N) * w;
        wsum += w;
    }
    if (wsum <= 0.0) return 0.0;

    confidence = fade;
    return sum / wsum;
}

// ============================================================================
// GI モード New（GiMode::New）の補間。RTXGI の GetDDGIVolumeIrradiance と同じ考え方:
//   - プローブの位置 = 格子点 + 再配置オフセット（probeData.xyz）。状態 0（壁の中）は重み 0
//   - surface bias（法線 + 視線方向。プローブ間隔に比例）で補間位置をずらしてから重みを作る
//   - wrap 重み（wrap*wrap + 0.2）は【押し出す前の位置】から見た方向で作る（RTXGI と同じ）。
//     Chebyshev は押し出した位置で。★旧実装は両方とも押し出し後の位置を使っていた。
//   - 重みが crush 閾値（0.2）未満なら 3 乗でさらに潰す（RTXGI の crushThreshold）
//   - trilinear は最後に掛ける
// 呼び出し側は【include より前に】DDGI_PROBEDATA_LOAD(uint2 texel) を定義すること
// （フォワード PS は t30 の SRV の Load、プローブ更新は u3 の UAV 読み）。未定義なら何も宣言しない。
// ============================================================================
#ifdef DDGI_PROBEDATA_LOAD

// プローブデータの 1 テクセル = 1 プローブ。xyz = 再配置オフセット(m) / w = 状態（0 無効・1 有効・2 有効になった直後）
uint2 DdgiProbeDataTexel(uint probeIndex, uint3 counts)
{
    const uint perRow = counts.x * counts.y;
    return uint2(probeIndex % perRow, probeIndex / perRow);
}

float4 DdgiLoadProbeData(uint probeIndex, uint3 counts)
{
    return DDGI_PROBEDATA_LOAD(DdgiProbeDataTexel(probeIndex, counts));
}

/// カスケード 1 本ぶんの格子（GI S4: カメラ追従のスクロール格子 + 2 カスケード）。
//   origin  : 窓の最小コーナー（ワールド）。ローカル座標 c のプローブ = origin + c * spacing（+ 再配置オフセット）
//   scroll  : プローブの記憶領域のずらし。ローカル座標 c の記憶領域 = (c + scroll) mod counts（固定格子は 0）
//   baseIdx : このカスケードの先頭プローブの通し番号（カスケード番号 * 1 カスケードのプローブ数）
// ★アトラスの行はカスケードごとに積む（atlasCounts = (cx, cy, cz * カスケード数)）。両カスケードの counts は同じ。
struct DdgiVol
{
    float3 origin;
    float3 spacing;
    uint3  counts;
    int3   scroll;
    uint   baseIdx;
};

DdgiVol DdgiMakeVol(float3 origin, float3 spacing, uint3 counts, int3 scroll, uint baseIdx)
{
    DdgiVol v;
    v.origin = origin; v.spacing = spacing; v.counts = counts; v.scroll = scroll; v.baseIdx = baseIdx;
    return v;
}

// 近いカスケードの外縁で遠いカスケードへ混ぜる幅（セル数）。DdgiSchedule.h の kCascadeBlendCells と同じ値にすること。
#define DDGI_CASCADE_BLEND_CELLS 1.0

// 補間の結果（8 個の重みと添字）。同じ重みで複数の方向のアトラスを引くために分けてある。
struct DdgiTaps
{
    uint  idx[8];     // プローブの通し番号（baseIdx 込み）
    float w[8];
    float wsum;
    float conf;       // 窓の外側のフェード。重みが全部 0（8 個とも壁の中）なら 0
    float cover;      // 窓の内側のフェード（外縁 coverBand セルで 0 へ。カスケードの切り替え用。coverBand=0 なら 1）
};

// camDir: カメラ → 点の向き（プローブ更新の中では「レイの進行方向」）。
// coverBand: 窓の外縁から内側へ何セルかけて cover を 0→1 にするか（0 で cover=1）。
DdgiTaps DdgiComputeTaps(Texture2D<float2> distAtlas, SamplerState samp,
                         float3 worldPos, float3 N, float3 camDir,
                         DdgiVol vol, uint3 atlasCounts,
                         float viewBias, float normalBias, float coverBand)
{
    const float3 originWS = vol.origin;
    const float3 spacing  = vol.spacing;
    const uint3  counts   = vol.counts;

    DdgiTaps t;
    [unroll] for (uint z = 0; z < 8; ++z) { t.idx[z] = 0; t.w[z] = 0.0; }
    t.wsum = 0.0;
    t.conf = 0.0;
    t.cover = 1.0;

    const float3 gridMax = originWS + float3(counts - 1) * spacing;
    const float3 outside  = max(originWS - worldPos, worldPos - gridMax);
    const float  worstOut = max(outside.x, max(outside.y, outside.z));
    // ★旧実装は最外プローブから半セルで 0 まで落としていたが、部屋の内寸に格子を合わせると
    //   壁・床・天井（最外プローブの外側 0.5〜1 セル）が全部 conf=0 になり、IBL（空）が戻ってきた。
    //   RTXGI は最外プローブへ clamp して外挿する。ここも「最外プローブから 0.75 セルまでは満額、
    //   そこから 0.5 セルかけて IBL へ戻す」にする（自動ボリューム S4 は壁の外まで覆うので、この外挿はほぼ使われない）。
    const float  cell     = max(max(spacing.x, max(spacing.y, spacing.z)), 1e-4);
    const float  fade     = saturate(1.0 - max(worstOut - 0.75 * cell, 0.0) / (0.5 * cell));
    if (fade <= 0.0) return t;
    if (coverBand > 0.0)
    {
        // 窓の内側へ向かう距離（最外プローブの面からの深さ）。外側は負 → 0。
        const float3 inner = min(worldPos - originWS, gridMax - worldPos);
        t.cover = saturate(min(inner.x, min(inner.y, inner.z)) / (coverBand * cell));
        if (t.cover <= 0.0) return t;
    }

    // surface bias。法線方向は壁際で裏側のプローブを引くのを減らし、視線方向は
    // 「カメラ側へ寄せる」ことで、壁に張り付いた画素が壁の向こうのプローブを拾うのを減らす。
    const float3 pb = worldPos + N * normalBias - camDir * viewBias;
    const float3 g  = (pb - originWS) / max(spacing, 1e-4);
    const int3   lo = clamp(int3(floor(g)), int3(0, 0, 0), int3(counts) - 1);
    const float3 f  = saturate(g - float3(lo));

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int3   off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3   c   = clamp(lo + off, int3(0, 0, 0), int3(counts) - 1);   // ローカル座標
        const uint3  sc  = (uint3(c) + uint3(vol.scroll)) % counts;             // 記憶領域の座標（リングバッファ）
        const uint   pIdx = vol.baseIdx + DdgiProbeIndex(sc, counts);
        t.idx[i] = pIdx;

        const float4 pd = DdgiLoadProbeData(pIdx, atlasCounts);
        if (pd.w < 0.5) continue;   // 壁の中（無効）= 重み 0

        const float3 tri = lerp(1.0 - f, f, float3(off));
        const float  triW = tri.x * tri.y * tri.z;
        const float3 probePos = originWS + float3(c) * spacing + pd.xyz;

        const float3 toProbe  = probePos - worldPos;     // 押し出す前
        const float3 toProbeB = probePos - pb;           // 押し出した後
        const float  distB    = length(toProbeB);
        const float3 dirW = toProbe / max(length(toProbe), 1e-6);

        float w = 1.0;
        // wrap（RTXGI: weight *= (wrap*wrap) + 0.2。0.2 は 8 個すべてが背面でも真っ黒にならないための下駄）
        const float wrap = (dot(dirW, N) + 1.0) * 0.5;
        w *= wrap * wrap + 0.2;

        // Chebyshev 可視性（押し出した位置で）。方向は「プローブ → 点」。
        {
            const float2 m = DdgiFetchProbeDistance(distAtlas, samp, pIdx, atlasCounts,
                                                    -toProbeB / max(distB, 1e-6));
            if (distB > m.x)
            {
                const float variance = abs(m.x * m.x - m.y);
                const float diff = distB - m.x;
                float cheb = variance / (variance + diff * diff);
                cheb = max(cheb * cheb * cheb, 0.0);
                w *= cheb;
            }
        }

        w = max(w, 1e-6);
        // 小さい重みをさらに潰す（RTXGI の crushThreshold。中途半端に残った漏れを消す）
        const float crush = 0.2;
        if (w < crush) w *= w * w * (1.0 / (crush * crush));
        w *= triW;

        t.w[i] = w;
        t.wsum += w;
    }
    t.conf = (t.wsum > 0.0) ? fade : 0.0;
    return t;
}

// 重み t でアトラスを dir 方向に引いた平均。rgb = irradiance / a = 空の可視率（BlendCS が書く）。
float4 DdgiFetchTaps(Texture2D<float4> atlas, SamplerState samp, DdgiTaps t, uint3 atlasCounts, float3 dir)
{
    float4 sum = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        if (t.w[i] <= 0.0) continue;
        sum += atlas.SampleLevel(
            samp, DdgiProbeUv(t.idx[i], atlasCounts, dir, DDGI_IRRADIANCE_TEXELS, DDGI_PROBE_TILE), 0) * t.w[i];
    }
    return (t.wsum > 0.0) ? (sum / t.wsum) : float4(0.0, 0.0, 0.0, 0.0);
}

// ---- GI モード New のサンプル（フォワード / SSGI / プローブ更新のバウンス共通）。カスケード 1〜2 本 ----
//   カスケード 0（近景）の外縁 kCascadeBlendCells セルの幅で、カスケード 1（遠景）へ滑らかに切り替える。
//   カスケード 0 の外縁のプローブ（窓がずれて入ってきた直後＝まだ収束していない）は重みが 0 に近いので見えない。
//   cascadeCount=1（固定ボリューム）なら従来と同じ（cover=1・遠景は引かない）。
//   irrN : 法線 N 方向の irradiance / irrR : 反射方向 R の irradiance（wantR のときだけ）/ skyVisR : R 方向の空の可視率
//   conf : 範囲外 0（呼び出し側がこの割合で IBL へ戻す）
struct DdgiNewResult
{
    float3 irrN;
    float3 irrR;
    float  skyVisR;
    float  conf;
};

DdgiNewResult DdgiSampleNew(Texture2D<float4> atlas, Texture2D<float2> distAtlas, SamplerState samp,
                            float3 worldPos, float3 N, float3 camDir, float3 R, bool wantR,
                            DdgiVol v0, DdgiVol v1, uint cascadeCount, uint3 atlasCounts,
                            float viewBias, float normalBias)
{
    DdgiNewResult o;
    o.irrN = 0.0.xxx; o.irrR = 0.0.xxx; o.skyVisR = 1.0; o.conf = 0.0;

    const bool two = cascadeCount > 1;
    const DdgiTaps t0 = DdgiComputeTaps(distAtlas, samp, worldPos, N, camDir, v0, atlasCounts,
                                        viewBias, normalBias, two ? DDGI_CASCADE_BLEND_CELLS : 0.0);
    const float a0 = t0.conf * t0.cover;
    float3 n0 = 0.0.xxx, r0 = 0.0.xxx; float s0 = 1.0;
    if (a0 > 0.0)
    {
        n0 = DdgiFetchTaps(atlas, samp, t0, atlasCounts, N).rgb;
        if (wantR) { const float4 r = DdgiFetchTaps(atlas, samp, t0, atlasCounts, R); r0 = r.rgb; s0 = r.a; }
    }
    float a1 = 0.0;
    float3 n1 = 0.0.xxx, r1 = 0.0.xxx; float s1 = 1.0;
    if (two && a0 < 1.0)
    {
        const float ratio = v1.spacing.x / max(v0.spacing.x, 1e-4);
        const DdgiTaps t1 = DdgiComputeTaps(distAtlas, samp, worldPos, N, camDir, v1, atlasCounts,
                                            viewBias * ratio, normalBias * ratio, 0.0);
        a1 = (1.0 - a0) * t1.conf;
        if (a1 > 0.0)
        {
            n1 = DdgiFetchTaps(atlas, samp, t1, atlasCounts, N).rgb;
            if (wantR) { const float4 r = DdgiFetchTaps(atlas, samp, t1, atlasCounts, R); r1 = r.rgb; s1 = r.a; }
        }
    }
    const float tot = a0 + a1;
    if (tot <= 0.0) return o;
    if (a1 <= 0.0)   // 近景だけ（固定ボリュームもここ。従来の式と同じ値）
    {
        o.irrN = n0; o.irrR = r0; o.skyVisR = saturate(s0);
    }
    else
    {
        const float inv = 1.0 / tot;
        o.irrN = (a0 * n0 + a1 * n1) * inv;
        o.irrR = (a0 * r0 + a1 * r1) * inv;
        o.skyVisR = saturate((a0 * s0 + a1 * s1) * inv);
    }
    if (!wantR) o.skyVisR = 1.0;
    o.conf = tot;
    return o;
}

#endif // DDGI_PROBEDATA_LOAD

#endif // DDGI_COMMON_HLSLI
