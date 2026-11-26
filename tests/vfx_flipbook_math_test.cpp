// UnoFX フリップブックの算術の試験（W1-BD7 / VFX-03。vfx_flipbook_math_test）。GPU 不要。
//   規約（NB2 の焼き込みの manifest conventions）に沿っていることを、合成のアトラスと解析式で確かめる:
//     * フレームの選び方・枠 → アトラスの座標（左上から行優先）
//     * 引く uv は枠の矩形の半テクセル内側へクランプ（隣の枠を引かない）
//     * MV は『引く位置で 2 回評価し直す』（解析的な MV 場で式どおりになる）。平行移動する斑点を MV 補間すると斑点が正しい位置へ来る
//     * 6-way の向き（RLU / DBF の各チャンネルと軸）、ビルボード空間への光の変換
//     * 黒体の色、浸食、炎族の陰影
//   第 1 引数に assets/engine/vfx を渡すと、焼き込み済みの実データ（DDS）でも確かめる（形式・大きさ・根元固定・枠の端）。
#include "renderer/vfx/VfxFlipbook.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::vfx::flip;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                                  \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        const double va_ = (a), vb_ = (b);                                                                     \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                                                \
            std::printf("FAIL %s:%d: %s = %.9g, 期待 %s = %.9g (許容 %g)\n", __FILE__, __LINE__, #a, va_, #b, vb_, double(eps)); \
            ++g_failures;                                                                                      \
        }                                                                                                      \
    } while (0)

namespace
{
const float kPi = 3.14159265358979f;

CpuTex MakeTex(u32 w, u32 h, u32 dxgi)
{
    CpuTex t; t.w = w; t.h = h; t.dxgi = dxgi; t.bpp = BytesPerTexel(dxgi);
    t.px.assign(size_t(w) * h * t.bpp, 0);
    return t;
}
void SetRgba8(CpuTex& t, int x, int y, u8 r, u8 g = 0, u8 b = 0, u8 a = 255)
{
    u8* p = &t.px[(size_t(y) * t.w + size_t(x)) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
void SetSnorm2(CpuTex& t, int x, int y, float u, float v)
{
    i16 s[2] = {static_cast<i16>(std::lround(u * 32767.0f)), static_cast<i16>(std::lround(v * 32767.0f))};
    std::memcpy(&t.px[(size_t(y) * t.w + size_t(x)) * 4], s, 4);
}

void TestFramePick()
{
    Grid g{6, 6, 36};
    // fps > 0: f = age * fps + start。ループ（最後の次は 0 番）
    FramePick p = PickFrames(g, 0.5f, 1.0f, 12.0f, 0);
    CHECK(p.a == 6 && p.b == 7); CHECK_NEAR(p.t, 0.0, 1e-6);
    p = PickFrames(g, 35.5f / 12.0f, 10.0f, 12.0f, 0);
    CHECK(p.a == 35 && p.b == 0); CHECK_NEAR(p.t, 0.5, 1e-4);
    p = PickFrames(g, 1.0f, 10.0f, 12.0f, 30);   // 12 + 30 = 42 -> 6
    CHECK(p.a == 6 && p.b == 7);
    p = PickFrames(g, 1000.0f, 10.0f, 12.0f, 5);  // 大きい f でも桁あふれしない
    CHECK(p.a < 36 && p.b < 36 && p.t >= 0.0f && p.t < 1.0f);
    // fps = 0: 寿命に渡って先頭から最後まで 1 回
    Grid g5{5, 1, 5};
    p = PickFrames(g5, 0.0f, 2.0f, 0.0f, 0);   CHECK(p.a == 0 && p.b == 1 && p.t == 0.0f);
    p = PickFrames(g5, 1.0f, 2.0f, 0.0f, 0);   CHECK(p.a == 2 && p.b == 3);
    p = PickFrames(g5, 2.0f, 2.0f, 0.0f, 0);   CHECK(p.a == 4 && p.b == 4 && p.t == 0.0f);   // 最後の次は最後（ループしない）
    p = PickFrames(g5, 5.0f, 2.0f, 0.0f, 0);   CHECK(p.a == 4 && p.b == 4);
    // 開始フレームは (id) だけで決まる決定論
    CHECK(StartFrame(7, 36) == StartFrame(7, 36) && StartFrame(7, 36) < 36);
    int distinct = 0; u32 seen[36] = {};
    for (u32 i = 0; i < 256; ++i) { const u32 s = StartFrame(i, 36); if (!seen[s]++) ++distinct; }
    CHECK(distinct > 24);   // 位相が散る
}

void TestClampAndAtlas()
{
    const V2 c = ClampInFrame({0.0f, 1.0f}, {64.0f, 64.0f});
    CHECK_NEAR(c.x, 0.5 / 64.0, 1e-7); CHECK_NEAR(c.y, 1.0 - 0.5 / 64.0, 1e-7);
    const V2 m = ClampInFrame({0.3f, 0.6f}, {64.0f, 64.0f});
    CHECK(m.x == 0.3f && m.y == 0.6f);
    Grid g{6, 6, 36};
    const V2 a = AtlasUv({0.25f, 0.5f}, g, 7);   // 7 = row 1, col 1
    CHECK_NEAR(a.x, (1.0 + 0.25) / 6.0, 1e-7); CHECK_NEAR(a.y, (1.0 + 0.5) / 6.0, 1e-7);
    const V2 z = AtlasUv({0.0f, 0.0f}, g, 0);
    CHECK(z.x == 0.0f && z.y == 0.0f);
    const V2 l = AtlasUv({1.0f, 1.0f}, g, 35);
    CHECK_NEAR(l.x, 1.0, 1e-7); CHECK_NEAR(l.y, 1.0, 1e-7);
}

// 枠の端で隣の枠を引かない。フレーム 0 は R = 0 で、フレーム 1 は R = 255。クランプ無しなら端が 1 テクセル分混ざる（負の対照）。
void TestNoLeak()
{
    const Grid g{2, 2, 4};
    CpuTex t = MakeTex(32, 32, 28);   // 枠 16x16
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
    {
        const int fx = x / 16, fy = y / 16;
        SetRgba8(t, x, y, (fx + fy * 2) == 0 ? 0 : 255);
    }
    const V2 pts[] = {{0, 0}, {1, 1}, {1, 0.5f}, {0.5f, 1}, {1, 0}, {0, 1}, {0.999f, 0.999f}};
    for (V2 p : pts) CHECK(SampleFrame(t, g, 0, p).x == 0.0f);
    // 負の対照: クランプ無し（枠の右端そのもの）で引くと隣の枠が混ざる
    const V4 leaked = t.Bilinear(AtlasUv({1.0f, 0.5f}, g, 0));
    CHECK(leaked.x > 0.4f);
    // 他の枠も同様（フレーム 3 の左上の角は R=255 のまま）
    CHECK(SampleFrame(t, g, 3, {0, 0}).x == 1.0f);
}

// 平行移動する斑点を MV で補間する: 補間は斑点を正しい位置へ運ぶ（単純な線形補間は 2 つに割れて暗い）
void TestMvTranslation()
{
    const Grid g{2, 1, 2};
    const u32 FS = 64;
    CpuTex col = MakeTex(FS * 2, FS, 28);
    const float sigma = 0.08f;
    auto blob = [&](float cx, int x, int y)
    {
        const float u = (float(x) + 0.5f) / float(FS), v = (float(y) + 0.5f) / float(FS);
        return std::exp(-((u - cx) * (u - cx) + (v - 0.5f) * (v - 0.5f)) / (2.0f * sigma * sigma));
    };
    for (u32 f = 0; f < 2; ++f)
        for (u32 y = 0; y < FS; ++y) for (u32 x = 0; x < FS; ++x)
            SetRgba8(col, int(f * FS + x), int(y), static_cast<u8>(std::lround(blob(f == 0 ? 0.3f : 0.5f, int(x), int(y)) * 255.0f)));
    const float mvRange = 0.5f;
    const u32 MS = 32;   // MV は色より低解像度
    CpuTex mv = MakeTex(MS * 2, MS, 37);
    for (u32 f = 0; f < 2; ++f)
        for (u32 y = 0; y < MS; ++y) for (u32 x = 0; x < MS; ++x)
            SetSnorm2(mv, int(f * MS + x), int(y), 0.2f / mvRange, 0.0f);   // 変位 (0.2, 0)

    const float t = 0.5f;
    const V2 uv = {0.4f, 0.5f};   // 中間の斑点の中心
    const Taps taps = ResolveTaps(uv, t, 0, 1, [&](u32 f, V2 p) { return SampleMv(mv, g, f, p, mvRange); });
    CHECK_NEAR(taps.pa.x, 0.3, 2e-3);   // A はフレーム 0 の位置（0.4 - 0.2 * 0.5 = 0.3）から
    CHECK_NEAR(taps.pb.x, 0.5, 2e-3);   // B はフレーム 1 の位置（0.4 + 0.2 * 0.5 = 0.5）から
    const V4 a = SampleFrame(col, g, 0, taps.pa), b = SampleFrame(col, g, 1, taps.pb);
    const float interp = (a.x + (b.x - a.x) * t);
    const V4 na = SampleFrame(col, g, 0, uv), nb = SampleFrame(col, g, 1, uv);
    const float naive = na.x + (nb.x - na.x) * t;
    std::printf("  MV 補間 %.3f / 単純な線形補間 %.3f（斑点の中心）\n", interp, naive);
    CHECK(interp > 0.93f);
    CHECK(naive < 0.55f);
    // t = 0 / 1 では端の絵そのもの
    const Taps t0 = ResolveTaps(uv, 0.0f, 0, 1, [&](u32 f, V2 p) { return SampleMv(mv, g, f, p, mvRange); });
    CHECK(t0.pa.x == uv.x && t0.pa.y == uv.y);
    const Taps t1 = ResolveTaps(uv, 1.0f, 0, 1, [&](u32 f, V2 p) { return SampleMv(mv, g, f, p, mvRange); });
    CHECK(t1.pb.x == uv.x && t1.pb.y == uv.y);
}

// 『出力 uv で 1 回』ではなく『引く位置で 2 回評価し直す』ことを、解析的な MV 場で確かめる
void TestMvFixedPoint()
{
    const float k = 0.3f, t = 0.6f;
    auto mv = [&](u32 f, V2 p) { return f == 0 ? V2{k * p.x, 0.0f} : V2{-k * p.x, 0.1f * p.y}; };
    const V2 uv = {0.7f, 0.4f};
    const Taps tp = ResolveTaps(uv, t, 0, 1, mv);
    // A: p1 = uv - d(uv) t, p2 = uv - d(p1) t
    const float p1 = uv.x - k * uv.x * t, p2 = uv.x - k * p1 * t;
    CHECK_NEAR(tp.pa.x, p2, 1e-6);
    CHECK(std::fabs(tp.pa.x - p1) > 1e-3f);        // 1 回だけの評価とは違う
    CHECK_NEAR(tp.pa.y, uv.y, 1e-7);
    // B: p1 = uv + dB(uv)(1-t), p2 = uv + dB(p1)(1-t)（x と y それぞれ）
    const float bx1 = uv.x + (-k * uv.x) * (1.0f - t), bx2 = uv.x + (-k * bx1) * (1.0f - t);
    const float by1 = uv.y + 0.1f * uv.y * (1.0f - t), by2 = uv.y + 0.1f * by1 * (1.0f - t);
    CHECK_NEAR(tp.pb.x, bx2, 1e-6);
    CHECK_NEAR(tp.pb.y, by2, 1e-6);
    // 2 回目の反復の誤差は 1 回目より小さい（不動点 uv/(1+k t) へ近づく）
    const float fix = uv.x / (1.0f + k * t);
    CHECK(std::fabs(p2 - fix) < std::fabs(p1 - fix));
}

void TestSixWay()
{
    const V4 rlu = {1.0f, 0.2f, 0.3f, 1.0f}, dbf = {0.4f, 0.5f, 0.6f, 0.0f};
    CHECK_NEAR(SixWayLight({1, 0, 0}, rlu, dbf), 1.0, 1e-6);    // 右から = RLU.R
    CHECK_NEAR(SixWayLight({-1, 0, 0}, rlu, dbf), 0.2, 1e-6);   // 左から = RLU.G
    CHECK_NEAR(SixWayLight({0, 1, 0}, rlu, dbf), 0.3, 1e-6);    // 上から = RLU.B
    CHECK_NEAR(SixWayLight({0, -1, 0}, rlu, dbf), 0.4, 1e-6);   // 下から = DBF.R
    CHECK_NEAR(SixWayLight({0, 0, 1}, rlu, dbf), 0.6, 1e-6);    // 手前（カメラ側）から = DBF.B
    CHECK_NEAR(SixWayLight({0, 0, -1}, rlu, dbf), 0.5, 1e-6);   // 奥（背面）から = DBF.G
    const float s = 1.0f / std::sqrt(3.0f);
    CHECK_NEAR(SixWayLight({s, s, s}, rlu, dbf), (1.0 + 0.3 + 0.6) / 3.0, 1e-6);
    CHECK_NEAR(SixWayAmbient(rlu, dbf), (1.0 + 0.2 + 0.3 + 0.4 + 0.5 + 0.6) / 6.0, 1e-6);
    // 光量が一様なら、どの向きでも同じ（二乗の重みの和が 1）
    const V4 u = {0.7f, 0.7f, 0.7f, 1.0f};
    CHECK_NEAR(SixWayLight({0.36f, 0.48f, 0.8f}, u, u), 0.7, 1e-6);

    // ビルボード空間への変換: +x = 画面右、+y = 画面上、+z = カメラ側
    const V3 R = {1, 0, 0}, U = {0, 1, 0}, C = {0, 0, -1};   // 例: 左手系のカメラは +z を見るので、カメラ側は -z
    V3 l = LightToBillboard({1, 0, 0}, R, U, C, 0.0f);   CHECK_NEAR(l.x, 1, 1e-6); CHECK_NEAR(l.y, 0, 1e-6); CHECK_NEAR(l.z, 0, 1e-6);
    l = LightToBillboard({0, 0, -1}, R, U, C, 0.0f);     CHECK_NEAR(l.z, 1, 1e-6);   // カメラ側の光は +z
    l = LightToBillboard({0, 1, 0}, R, U, C, 0.0f);      CHECK_NEAR(l.y, 1, 1e-6);
    // 四角形を 90 度回すと、画面右からの光はテクスチャ空間では -y（下）から来る
    l = LightToBillboard({1, 0, 0}, R, U, C, kPi * 0.5f); CHECK_NEAR(l.x, 0, 1e-5); CHECK_NEAR(l.y, -1, 1e-5);
    // 回転しても長さは保たれる
    l = LightToBillboard({0.3f, 0.5f, 0.2f}, R, U, C, 1.234f);
    CHECK_NEAR(std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z), std::sqrt(0.3 * 0.3 + 0.5 * 0.5 + 0.2 * 0.2), 1e-6);
}

void TestBlackbodyAndErosion()
{
    for (float T : {1000.0f, 1700.0f, 2200.0f, 2800.0f, 4000.0f, 6500.0f, 12000.0f})
    {
        const V3 c = BlackbodyColor(T);
        CHECK_NEAR(std::max(c.x, std::max(c.y, c.z)), 1.0, 1e-5);
        CHECK(c.x >= 0.0f && c.y >= 0.0f && c.z >= 0.0f);
    }
    const V3 c1700 = BlackbodyColor(1700.0f), c2800 = BlackbodyColor(2800.0f), c6500 = BlackbodyColor(6500.0f);
    CHECK(c1700.x >= c1700.y && c1700.y > c1700.z);      // 低温は赤〜橙
    CHECK(c1700.z < 0.1f);
    CHECK(c2800.y > c1700.y && c2800.z > c1700.z);       // 温度が上がると緑・青が増える
    CHECK(c6500.z > 0.85f && c6500.y > 0.85f);           // 6500K はほぼ白
    // 範囲外はクランプ（NaN にならない）
    const V3 lo = BlackbodyColor(100.0f), hi = BlackbodyColor(1.0e6f);
    CHECK(std::isfinite(lo.x) && std::isfinite(hi.z));

    // 浸食: 閾値 0 で全部残り、1 + w で全部消え、浸食場が大きいほど残る
    const float w = 0.15f;
    for (float er : {0.0f, 0.3f, 0.7f, 1.0f})
    {
        CHECK_NEAR(Erosion(er, 0.0f, w), 1.0, 1e-6);
        CHECK_NEAR(Erosion(er, 1.0f + w, w), 0.0, 1e-6);
    }
    float prev = -1.0f;
    for (float er = 0.0f; er <= 1.0f; er += 0.05f)
    {
        const float e = Erosion(er, 0.5f, w);
        CHECK(e >= prev - 1e-6f);
        prev = e;
    }
    CHECK(Erosion(0.1f, 0.5f, w) < Erosion(0.9f, 0.5f, w));   // 小さいほど先に削れる
    CHECK(ErodeThreshold(0.2f, 0.35f, w) == 0.0f);            // 開始前は削らない
    CHECK_NEAR(ErodeThreshold(1.0f, 0.35f, w), 1.0 + w, 1e-6);
    CHECK(ErodeThreshold(0.7f, 0.35f, w) > ErodeThreshold(0.5f, 0.35f, w));
}

void TestShade()
{
    FireParams fp;
    // 温度が高いほど明るい。自己発光 0 なら光らない。α = 不透明度 * 浸食
    const V4 cold = {0.1f, 0.8f, 1.0f, 1.0f}, hot = {0.9f, 0.8f, 1.0f, 1.0f}, dark = {0.9f, 0.8f, 1.0f, 0.0f};
    const Shaded sc = ShadeFire(cold, 0.0f, fp), sh = ShadeFire(hot, 0.0f, fp), sd = ShadeFire(dark, 0.0f, fp);
    CHECK(sh.rgb.x + sh.rgb.y + sh.rgb.z > 3.0f * (sc.rgb.x + sc.rgb.y + sc.rgb.z));
    CHECK(sd.rgb.x == 0.0f && sd.rgb.y == 0.0f && sd.rgb.z == 0.0f);
    CHECK_NEAR(sh.a, 0.8, 1e-6);
    CHECK(sh.rgb.x >= sh.rgb.y && sh.rgb.y >= sh.rgb.z);   // 炎の色味は赤 > 緑 > 青
    // 寿命の終わりでは、浸食場の小さい所から消える
    const Shaded late = ShadeFire({0.9f, 0.8f, 0.2f, 1.0f}, 1.0f, fp);
    CHECK(late.a < 1e-4f);
    // 煙: 光が来る向きで明るさが変わる。α は RLU.A * 浸食（DBF.A）
    const V4 rlu = {1.0f, 0.1f, 0.5f, 0.9f}, dbf = {0.2f, 0.2f, 0.4f, 1.0f};
    SmokeParams sp;
    const V3 amb = {0.1f, 0.1f, 0.1f}, sun = {2.0f, 2.0f, 2.0f}, alb = {0.5f, 0.5f, 0.5f};
    const Shaded fromRight = ShadeSmoke(rlu, dbf, {1, 0, 0}, 0.0f, alb, amb, sun, sp);
    const Shaded fromLeft = ShadeSmoke(rlu, dbf, {-1, 0, 0}, 0.0f, alb, amb, sun, sp);
    CHECK(fromRight.rgb.x > fromLeft.rgb.x * 3.0f);
    CHECK_NEAR(fromRight.a, 0.9, 1e-6);
    CHECK_NEAR(fromRight.rgb.x, 0.5 * (0.1 * ((1.0 + 0.1 + 0.5 + 0.2 + 0.2 + 0.4) / 6.0) + 2.0 * 1.0), 1e-5);
}

std::vector<u8> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 焼き込み済みの実データ（NB2）。形式・大きさ・枠の端・根元固定
void TestRealAssets(const std::string& dir)
{
    struct Spec { const char* file; u32 w, h, dxgi; };
    const Spec specs[] = {
        {"fire_flipbook.dds", 768, 768, 28}, {"fire_mv.dds", 384, 384, 37}, {"smoke_6way_rlu.dds", 768, 768, 28},
        {"smoke_6way_dbf.dds", 768, 768, 28}, {"smoke_mv.dds", 384, 384, 37}, {"embers.dds", 256, 256, 28}, {"sparks.dds", 256, 128, 28},
    };
    for (const Spec& s : specs)
    {
        const std::vector<u8> bytes = ReadFile(dir + "/" + s.file);
        CpuTex t; std::string err;
        const bool ok = !bytes.empty() && LoadDds(bytes.data(), bytes.size(), t, err);
        if (!ok) std::printf("  %s: %s\n", s.file, err.c_str());
        CHECK(ok);
        if (!ok) continue;
        CHECK(t.w == s.w && t.h == s.h && t.dxgi == s.dxgi);
    }
    // 壊れた入力は拒否する（落ちない）
    CpuTex junk; std::string err;
    const u8 bad[8] = {'D', 'D', 'S', ' ', 0, 0, 0, 0};
    CHECK(!LoadDds(bad, sizeof(bad), junk, err));
    CHECK(!LoadDds(nullptr, 0, junk, err));

    const Grid g{6, 6, 36};
    const std::vector<u8> fb = ReadFile(dir + "/fire_flipbook.dds"), mvb = ReadFile(dir + "/fire_mv.dds");
    CpuTex fire, mv;
    if (!LoadDds(fb.data(), fb.size(), fire, err) || !LoadDds(mvb.data(), mvb.size(), mv, err)) { CHECK(false); return; }

    // 枠の端: フレーム f の右下の角（uv = (1,1)）は、枠の最後のテクセルそのもの（隣の枠が混ざらない）。MV の枠も同じ
    for (u32 f : {0u, 1u, 7u, 20u, 35u})
    {
        const u32 cx = (f % 6) * 128, cy = (f / 6) * 128;
        const V4 got = SampleFrame(fire, g, f, {1.0f, 1.0f}), ref = fire.Texel(int(cx + 127), int(cy + 127));
        // アトラスの割り算（/ 6）の丸めで重みが 1e-5 程度ずれるので、厳密一致ではなく近似で見る（隣の枠が混ざれば 0.01 以上動く）
        CHECK_NEAR(got.x, ref.x, 2e-3); CHECK_NEAR(got.y, ref.y, 2e-3); CHECK_NEAR(got.z, ref.z, 2e-3); CHECK_NEAR(got.w, ref.w, 2e-3);
        const V4 got0 = SampleFrame(fire, g, f, {0.0f, 0.0f}), ref0 = fire.Texel(int(cx), int(cy));
        CHECK_NEAR(got0.x, ref0.x, 2e-3); CHECK_NEAR(got0.y, ref0.y, 2e-3);
        const u32 mx = (f % 6) * 64, my = (f / 6) * 64;
        const V4 mg = SampleFrame(mv, g, f, {1.0f, 1.0f}), mr = mv.Texel(int(mx + 63), int(my + 63));
        CHECK_NEAR(mg.x, mr.x, 2e-3); CHECK_NEAR(mg.y, mr.y, 2e-3);
    }

    // 根元固定（manifest anchoring）: 枠の下端の行に不透明度が残る（最大約 0.6）。上端は先端で薄い。v は下向き（画像の下 = 枠の下）
    float bottomMax = 0.0f, topMax = 0.0f;
    for (u32 f = 0; f < 36; ++f)
    {
        const u32 cx = (f % 6) * 128, cy = (f / 6) * 128;
        for (u32 x = 0; x < 128; ++x)
        {
            bottomMax = std::max(bottomMax, fire.Texel(int(cx + x), int(cy + 127)).y);
            topMax = std::max(topMax, fire.Texel(int(cx + x), int(cy)).y);
        }
    }
    std::printf("  炎の不透明度: 下端の行の最大 %.3f / 上端の行の最大 %.3f\n", bottomMax, topMax);
    CHECK(bottomMax > 0.3f);
    CHECK(topMax < bottomMax * 0.5f);

    // 実データの MV 補間: t = 0 / 1 は端の絵そのもの。中間でも [0,1] に収まり有限
    const float mvRange = 0.5f;
    for (u32 f : {0u, 10u, 35u})
    {
        const FramePick p = {f, (f + 1) % 36, 0.0f};
        auto mvfn = [&](u32 fr, V2 q) { return SampleMv(mv, g, fr, q, mvRange); };
        const V2 uv = {0.5f, 0.6f};
        const Taps t0 = ResolveTaps(uv, 0.0f, p.a, p.b, mvfn);
        CHECK(t0.pa.x == uv.x && t0.pa.y == uv.y);
        const Taps th = ResolveTaps(uv, 0.5f, p.a, p.b, mvfn);
        CHECK(std::isfinite(th.pa.x) && std::isfinite(th.pb.y));
        CHECK(std::fabs(th.pa.x - uv.x) < mvRange && std::fabs(th.pb.y - uv.y) < mvRange);
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestFramePick();
    TestClampAndAtlas();
    TestNoLeak();
    TestMvTranslation();
    TestMvFixedPoint();
    TestSixWay();
    TestBlackbodyAndErosion();
    TestShade();
    if (argc >= 2) TestRealAssets(argv[1]);
    else std::printf("  （実データの検査は省略。第 1 引数に assets/engine/vfx を渡す）\n");
    std::printf("vfx_flipbook_math_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
