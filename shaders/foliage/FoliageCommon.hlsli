// FoliageCommon.hlsli - 植生 F1: インスタンス 32B の読み出し・パック規約・ハッシュ・3x4 行列の合成。
//
// ★src/renderer/foliage/FoliageTypes.h / FoliageMath.h と一対一。片方だけ直すと GPU が別の値を読むので
//   変えるときは必ず両方 + tests/foliage_test.cpp / foliage_gpu_test.cpp を更新すること。
#ifndef FOLIAGE_COMMON_HLSLI
#define FOLIAGE_COMMON_HLSLI

static const float kFoliagePi    = 3.14159265358979;
static const float kFoliageTwoPi = 6.28318530717959;
static const float kFoliageTiltMax = 0.78539816339745;   // π/4（tilt の snorm 1.0）
static const float kFoliageThinBand = 0.15;              // FoliageMath.h の kThinBand

// ---- 決定論ハッシュ（FoliageTypes.h の HashU32 / Hash01 と同一）----
uint FoliageHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float FoliageHash01(uint x) { return float(FoliageHash(x) >> 8) * (1.0 / 16777216.0); }

// ---- インスタンス（32B）と チャンク（32B）----
struct FoliageInstance
{
    float3 pos;
    uint   rotScale;
    uint   tilt;
    uint   color;
    uint   seedType;
    uint   params;
};
struct FoliageChunk
{
    float3 mn; uint first;
    float3 mx; uint count;
};

FoliageInstance FoliageLoadInstance(ByteAddressBuffer buf, uint index)
{
    const uint off = index * 32u;
    const uint4 a = buf.Load4(off);
    const uint4 b = buf.Load4(off + 16u);
    FoliageInstance o;
    o.pos      = asfloat(a.xyz);
    o.rotScale = a.w;
    o.tilt     = b.x;
    o.color    = b.y;
    o.seedType = b.z;
    o.params   = b.w;
    return o;
}
FoliageChunk FoliageLoadChunk(ByteAddressBuffer buf, uint index)
{
    const uint off = index * 32u;
    const uint4 a = buf.Load4(off);
    const uint4 b = buf.Load4(off + 16u);
    FoliageChunk c;
    c.mn = asfloat(a.xyz); c.first = a.w;
    c.mx = asfloat(b.xyz); c.count = b.w;
    return c;
}

float FoliageUnpackYaw(uint rotScale) { return float(rotScale & 0xFFFFu) * (kFoliageTwoPi / 65536.0); }
float FoliageUnpackScale(uint rotScale) { return f16tof32(rotScale >> 16); }
float FoliageSnorm16(uint bits16)
{
    const int s = ((int)(bits16 << 16)) >> 16;
    return max(float(s) / 32767.0, -1.0);
}
float FoliageTiltX(uint t) { return FoliageSnorm16(t & 0xFFFFu) * kFoliageTiltMax; }
float FoliageTiltZ(uint t) { return FoliageSnorm16(t >> 16) * kFoliageTiltMax; }
uint  FoliageSeed(uint seedType) { return seedType & 0xFFFFFFu; }
uint  FoliageTypeOf(uint seedType) { return seedType >> 24; }

float4 FoliageUnpackColor8(uint c)
{
    return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float(c >> 24)) * (1.0 / 255.0);
}

// インスタンス → ワールドの 3x4（列ベクトル規約。行 r0..r2 = (基底の行, 平行移動)）。
// B = Rz(tz) * Rx(tx) * Ry(yaw) * s。world = layer * [B | p]。FoliageMath.h の ComposeInstance と同一。
void FoliageCompose(FoliageInstance i, float4 L0, float4 L1, float4 L2, out float4 r0, out float4 r1, out float4 r2)
{
    const float yaw = FoliageUnpackYaw(i.rotScale);
    const float s   = FoliageUnpackScale(i.rotScale);
    const float tx  = FoliageTiltX(i.tilt), tz = FoliageTiltZ(i.tilt);
    const float cy = cos(yaw), sy = sin(yaw);
    const float cx = cos(tx),  sx = sin(tx);
    const float cz = cos(tz),  sz = sin(tz);
    // Rx * Ry
    const float3x3 Ry = float3x3(cy, 0, sy,   0, 1, 0,   -sy, 0, cy);
    const float3x3 Rx = float3x3(1, 0, 0,   0, cx, -sx,   0, sx, cx);
    const float3x3 Rz = float3x3(cz, -sz, 0,   sz, cz, 0,   0, 0, 1);
    // ★HLSL の float3x3(...) は行優先で埋まる。列ベクトル規約の数式どおり mul(A, B) = A*B。
    const float3x3 B = mul(Rz, mul(Rx, Ry)) * s;
    const float3 p = float3(i.pos);
    // layer(3x4) * [B | p]
    const float3x3 A = float3x3(L0.xyz, L1.xyz, L2.xyz);
    const float3x3 W = mul(A, B);
    const float3 wp = float3(dot(L0.xyz, p) + L0.w, dot(L1.xyz, p) + L1.w, dot(L2.xyz, p) + L2.w);
    r0 = float4(W[0], wp.x);
    r1 = float4(W[1], wp.y);
    r2 = float4(W[2], wp.z);
}

// ディザ用ノイズ（SV_Position のピクセル座標）。FoliageMath.h の InterleavedGradientNoise と同一。
// ★全パス（本体 / 深度 / 速度）で同じ式を使うこと。ずれると深度プリパスと本体が食い違って穴が開く。
float FoliageDitherNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// アルファテスト用の「ミップ別カバレッジ保存」。ミップが粗くなるほど葉のアルファは薄まって（平均化されて）カードが痩せる。
// 粗いミップほどアルファを持ち上げてから cutoff と比べる（lod = CalculateLevelOfDetail。近距離の lod <= 0 では恒等）。
// ★本体 / 深度 / 速度の全パスで同じ式を使うこと（ずれると深度と本体の抜け方が食い違って穴や黒縁が出る）。
float FoliageMaskAlpha(float a, float lod)
{
    return a * (1.0 + 0.5 * clamp(lod, 0.0, 4.0));
}

#endif // FOLIAGE_COMMON_HLSLI
