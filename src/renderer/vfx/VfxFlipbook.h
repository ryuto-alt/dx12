#pragma once
// ===========================================================================
// UnoFX フリップブックの算術（W1-BD7 / VFX-03）の CPU 参照。shaders/vfx/VfxFlipbook.hlsli / VfxSixWay.hlsli と同じ式
//   （tests/vfx_flipbook_math_test.cpp が CPU 側を、tests/vfx_gpu_test.cpp が GPU との一致を確かめる）。
//   GPU / D3D に依存しない。規約の正は assets/engine/vfx/vfx_bake_manifest.json の `conventions`（NB2 の焼き込み）:
//     * 枠は左上から行優先（frame = row * cols + col）。枠内の uv は左上が (0,0)、v は下向き。ミップ無し。
//     * ビルボードの軸は +x = 画面右、+y = 画面上、+z = カメラ側。
//     * 炎と煙の枠は『根元固定』（下端に根元がある）。置くときは四角形の下端を粒子の位置に合わせる（anchor = bottom）。
//     * モーションベクトル（MV）は R16G16_SNORM。変位 d（枠 uv）= snorm * mvRange。フレーム i の絵が i+1 までに動く量。
//       補間（t = 0..1）: A（フレーム i）は uv - d_A(p)*t の位置から、B（フレーム i+1）は uv + d_B(p)*(1-t) の位置から引き、
//       t で混ぜる。d は『出力 uv で 1 回』ではなく『引く位置で評価し直す』（2 回の不動点反復）。
//       引く uv（MV を引く位置も、絵を引く位置も）は、その枠の矩形に半テクセル内側でクランプしてから、アトラスのオフセットを足す
//       （線形フィルタが隣の枠のテクセルを混ぜない）。MV は色より低解像度（同じ枠 uv で引く）。
//     * 煙の 6-way: RLU（R = 右から、G = 左から、B = 上から、A = 不透明度）と DBF（R = 下から、G = 奥から、B = 手前（カメラ側）から、
//       A = 浸食場）。光 L（ビルボード空間の単位ベクトル。表面から光へ）に対し
//       光 = Lx² * (Lx > 0 ? RLU.R : RLU.G) + Ly² * (Ly > 0 ? RLU.B : DBF.R) + Lz² * (Lz > 0 ? DBF.B : DBF.G)。
//     * 炎族（fireFamily）: R = 温度（0..1。黒体へ）、G = 不透明度、B = 浸食場（小さいほど先に削れる）、A = 自己発光の重み。
// ===========================================================================
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e::vfx::flip
{

struct V2 { float x = 0, y = 0; };
struct V3 { float x = 0, y = 0, z = 0; };
struct V4 { float x = 0, y = 0, z = 0, w = 0; };

inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
inline V4 Lerp(V4 a, V4 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}; }
inline float Saturate(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// ---- アトラス（フレームの格子）----
struct Grid
{
    u32 cols = 1, rows = 1, frames = 1;   // frames <= cols * rows
};

struct FramePick
{
    u32   a = 0, b = 0;   // 補間する 2 枚（a の次が b）
    float t = 0;          // 0..1
};

// 粒子ごとの開始フレーム（fps > 0 のループ再生で、同じ時刻に生まれた粒子の位相をずらす）。HLSL の FbStartFrame と同じ式。
u32 StartFrame(u32 particleId, u32 frames);

// 粒子の age / life から再生するフレームを決める。
//   fps > 0 : f = age * fps + startFrame。ループ再生（最後の次は 0 番）。
//   fps = 0 : 寿命に渡って先頭から最後まで 1 回（f = age / life * (frames - 1)）。最後の次は最後（ループしない）。
FramePick PickFrames(const Grid& g, float age, float life, float fps, u32 startFrame);

// 枠内 uv を、枠の矩形の半テクセル内側へクランプする（frameTexels = 枠の一辺のテクセル数）。
inline V2 ClampInFrame(V2 uv, V2 frameTexels)
{
    const V2 h = {0.5f / frameTexels.x, 0.5f / frameTexels.y};
    return {std::min(std::max(uv.x, h.x), 1.0f - h.x), std::min(std::max(uv.y, h.y), 1.0f - h.y)};
}
// 枠内 uv（クランプ済み）をアトラス uv へ。
inline V2 AtlasUv(V2 uv, const Grid& g, u32 frame)
{
    const u32 c = frame % g.cols, r = frame / g.cols;
    return {(float(c) + uv.x) / float(g.cols), (float(r) + uv.y) / float(g.rows)};
}

// ---- CPU のテクスチャ（DDS から読む。GPU の線形・クランプのサンプリングと同じ重み付け）----
struct CpuTex
{
    u32 w = 0, h = 0;
    u32 dxgi = 0;   // DXGI_FORMAT の数値（28 RGBA8_UNORM, 29 RGBA8_UNORM_SRGB, 37 RG16_SNORM, 13 RGBA16_SNORM, 10 RGBA16_FLOAT）
    u32 bpp = 0;    // 1 テクセルのバイト数
    std::vector<u8> px;   // 行優先、行の隙間なし
    V4 Texel(int x, int y) const;
    V4 Bilinear(V2 uv) const;   // uv は 0..1（アトラス全体）。端はクランプ。テクセル座標 = uv * size - 0.5
};

struct DdsInfo
{
    u32 w = 0, h = 0, dxgi = 0, bpp = 0;
    size_t dataOffset = 0;   // ピクセルの先頭（ファイル先頭からのバイト数）
};
// 'DDS ' + DX10 拡張の 2D・ミップ 1・配列 1 だけを受ける（NB2 の焼き込みの形式）。失敗なら false（err に理由）。
bool ParseDdsHeader(const u8* data, size_t size, DdsInfo& out, std::string& err);
bool LoadDds(const u8* data, size_t size, CpuTex& out, std::string& err);
u32  BytesPerTexel(u32 dxgi);   // 対応外は 0

// 枠 `frame` の、枠内 uv（クランプ前）の色。クランプ・アトラスのオフセットを含む。
V4 SampleFrame(const CpuTex& atlas, const Grid& g, u32 frame, V2 uv);
// 枠 `frame` の MV（枠 uv の変位。snorm * mvRange）。
V2 SampleMv(const CpuTex& mv, const Grid& g, u32 frame, V2 uv, float mvRange);

// MV で補間するときに『絵を引く位置』（枠内 uv。クランプ前）を求める。2 回の不動点反復。MV が無ければ uv のまま。
struct Taps { V2 pa, pb; };
template <class MvFn>   // MvFn: V2(u32 frame, V2 uvInFrame)  … 変位（枠 uv）を返す（引く位置のクランプは MvFn の中）
inline Taps ResolveTaps(V2 uv, float t, u32 a, u32 b, MvFn&& mv)
{
    V2 pa = uv, pb = uv;
    for (int k = 0; k < 2; ++k)
    {
        pa = uv - mv(a, pa) * t;
        pb = uv + mv(b, pb) * (1.0f - t);
    }
    return {pa, pb};
}

// ---- 描画の算術（炎族）----
// 黒体放射の色（線形 sRGB。最大成分 = 1）。Kim ら（2002）の三次スプライン近似の色度（1667K..25000K。範囲外はクランプ）。
V3    BlackbodyColor(float kelvin);
// 浸食: er = 浸食場（小さいほど先に削れる）、th = 閾値（0 で全部残る、1 + w で全部消える）、w = 縁の幅。
float Erosion(float er, float th, float w);
// 寿命の割合 tLife（0..1）から浸食の閾値へ。erodeStart までは削らない。
float ErodeThreshold(float tLife, float erodeStart, float w);

struct FireParams
{
    float tempMin = 1000.0f, tempMax = 2800.0f;   // K（温度チャンネル 0 / 1 に対応）
    float emission = 3.0f;                         // 放射の倍率（HDR）
    float erodeStart = 0.35f, erodeWidth = 0.15f;
};
struct Shaded { V3 rgb; float a; };   // rgb は a を掛ける前（描画側で premultiply）
// s = 炎族のテクセル（R 温度、G 不透明度、B 浸食場、A 自己発光の重み）。tLife = 粒子の寿命の割合。
Shaded ShadeFire(V4 s, float tLife, const FireParams& p);

// ---- 煙（6-way）----
// L = ビルボード空間の光の向き（表面から光へ。単位ベクトル）。戻りは光の量（不透明度で正規化済み。使うときに A を掛ける）。
float SixWayLight(V3 L, V4 rlu, V4 dbf);
// 全方向の平均（環境光の係数）。
float SixWayAmbient(V4 rlu, V4 dbf);

struct SmokeParams
{
    float erodeStart = 0.35f, erodeWidth = 0.15f;
};
// ambient = 環境光（線形 RGB）、sun = 光の色（線形 RGB。放射照度ではなく色 * 強さ）、albedo = 粒子の色。
Shaded ShadeSmoke(V4 rlu, V4 dbf, V3 L, float tLife, V3 albedo, V3 ambient, V3 sun, const SmokeParams& p);

// ビルボード空間への光の変換。lightWorld = 表面から光へ（ワールド）、right / up / towardCam = ビルボードの基底（ワールド）、
// rot = 四角形の回転（rad。ビルボードの +x はカメラ空間で (cos, sin) の向き）。
V3 LightToBillboard(V3 lightWorld, V3 right, V3 up, V3 towardCam, float rot);

} // namespace dx12e::vfx::flip
