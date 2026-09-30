// 植生 F1 の純ロジックテスト（GPU 不要）。
//   ・32B パック規約の往復 / チャンク構築の不変条件と決定論 / .dxfoliage の往復と破損の拒否
//   ・ポアソンディスク散布: 最小間隔・決定論・傾斜 / 高度 / スプラットの制限・既存との間隔・上限
//   ・GPU カリングの CPU 参照: 距離 / LOD 選択 / 間引き密度 / クロスフェードが補完的 / 影パス / 錐台 = Frustum::SphereVisible
//   ・風: 決定論 / 無風で 0 / 曲げで長さが保たれる / 時間に連続 / 位相がインスタンスごとに違う
//   ・性能の目安: 100 万インスタンスのチャンク構築（時間は表示するだけ。判定は「終わること」）
#include "renderer/Frustum.h"
#include "renderer/foliage/FoliageIO.h"
#include "renderer/foliage/FoliageLayerOps.h"
#include "renderer/foliage/FoliageMath.h"
#include "renderer/foliage/FoliageScatter.h"
#include "renderer/foliage/FoliageTypes.h"
#include "renderer/foliage/SceneWind.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

using namespace dx12e;
using namespace dx12e::foliage;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        const double _a = (a), _b = (b);                                                               \
        if (!(std::fabs(_a - _b) <= (tol))) {                                                          \
            std::printf("FAIL %s:%d: %s ≈ %s  (%.9g vs %.9g, tol %.3g)\n", __FILE__, __LINE__, #a, #b, _a, _b, static_cast<double>(tol)); \
            ++g_failures;                                                                              \
        }                                                                                              \
    } while (0)

namespace
{
FoliageInstance MakeInstance(f32 x, f32 y, f32 z, u32 seed, u32 type = 0, f32 yaw = 0.0f, f32 scale = 1.0f)
{
    FoliageInstance in;
    in.px = x; in.py = y; in.pz = z;
    in.rotScale = PackYawScale(yaw, scale);
    in.tilt = PackTilt(0, 0);
    in.color = PackColor8(1, 1, 1, 1);
    in.seedType = PackSeedType(seed, type);
    in.params = 0;
    return in;
}

std::vector<FoliageInstance> RandomInstances(u32 n, f32 extent, u32 seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-extent, extent);
    std::vector<FoliageInstance> v;
    v.reserve(n);
    for (u32 i = 0; i < n; ++i) v.push_back(MakeInstance(u(rng), u(rng) * 0.05f, u(rng), rng() & 0xFFFFFF, rng() & 3));
    return v;
}

void Test_Pack()
{
    for (f32 yaw : {0.0f, 0.5f, 1.0f, 3.14159f, 6.0f})
    {
        const u32 p = PackYawScale(yaw, 1.25f);
        CHECK_NEAR(UnpackYaw(p), yaw, kTwoPi / 65536.0 * 1.01);
        CHECK_NEAR(UnpackScale(p), 1.25, 1e-3);
    }
    CHECK_NEAR(UnpackYaw(PackYawScale(-1.0f, 1.0f)), kTwoPi - 1.0, 1e-3);   // 負の角は折り返す
    for (f32 t : {-0.7f, -0.1f, 0.0f, 0.3f, 0.78f})
    {
        const u32 p = PackTilt(t, -t);
        CHECK_NEAR(UnpackTiltX(p), t, kTiltMax / 32767.0 * 1.5);
        CHECK_NEAR(UnpackTiltZ(p), -t, kTiltMax / 32767.0 * 1.5);
    }
    CHECK_NEAR(UnpackTiltX(PackTilt(10.0f, 0)), kTiltMax, 1e-4);              // ±45° へクランプ
    const u32 st = PackSeedType(0x123456, 3);
    CHECK(SeedOf(st) == 0x123456u && TypeOf(st) == 3u);
    CHECK_NEAR(WindScaleOf(0), 1.0, 1e-6);
    CHECK_NEAR(WindScaleOf(128), 1.0, 1e-6);
    CHECK_NEAR(WindScaleOf(64), 0.5, 1e-6);
    // ハッシュは決定論で一様
    CHECK(HashU32(12345u) == HashU32(12345u));
    f64 sum = 0; const u32 n = 100000;
    for (u32 i = 0; i < n; ++i) sum += Hash01(i);
    CHECK_NEAR(sum / n, 0.5, 0.005);
    CHECK(sizeof(FoliageInstance) == 32 && sizeof(FoliageChunk) == 32);
}

void Test_Chunks()
{
    for (u32 n : {0u, 1u, 127u, 128u, 129u, 1000u, 50000u})
    {
        FoliageInstanceSet s;
        s.instances = RandomInstances(n, 500.0f, 7 + n);
        s.Rebuild();
        u32 covered = 0;
        bool ok = true;
        for (const FoliageChunk& c : s.chunks)
        {
            if (c.first != covered || c.count == 0 || c.count > kChunkMaxInstances) ok = false;
            for (u32 i = c.first; i < c.first + c.count && ok; ++i)
            {
                const FoliageInstance& in = s.instances[i];
                if (in.px < c.mn[0] || in.px > c.mx[0] || in.py < c.mn[1] || in.py > c.mx[1] || in.pz < c.mn[2] || in.pz > c.mx[2]) ok = false;
            }
            covered += c.count;
        }
        CHECK(ok);
        CHECK(covered == n);
        // 決定論: 同じ入力（順序込み）→ 同じ並び
        FoliageInstanceSet s2;
        s2.instances = RandomInstances(n, 500.0f, 7 + n);
        s2.Rebuild();
        CHECK(s2.chunks.size() == s.chunks.size());
        bool same = true;
        for (u32 i = 0; i < n; ++i) if (std::memcmp(&s.instances[i], &s2.instances[i], sizeof(FoliageInstance)) != 0) { same = false; break; }
        CHECK(same);
    }
    // 空間的なまとまり: 50000 個の平均チャンク体積が全体の小さな割合（kd 木が効いている）
    FoliageInstanceSet s;
    s.instances = RandomInstances(50000, 500.0f, 3);
    s.Rebuild();
    f64 areaSum = 0;
    for (const auto& c : s.chunks) areaSum += static_cast<f64>(c.mx[0] - c.mn[0]) * (c.mx[2] - c.mn[2]);
    CHECK(areaSum < 1000.0 * 1000.0 * 3.0);   // 全面積の 3 倍未満（重なりが小さい）
}

void Test_IO()
{
    FoliageInstanceSet s;
    s.instances = RandomInstances(3000, 100.0f, 11);
    s.Rebuild();
    const std::vector<u8> bytes = EncodeFoliage(s);
    FoliageInstanceSet t;
    std::string err;
    CHECK(DecodeFoliage(bytes, t, &err));
    CHECK(t.instances.size() == s.instances.size() && t.chunks.size() == s.chunks.size());
    CHECK(std::memcmp(t.instances.data(), s.instances.data(), s.instances.size() * 32) == 0);
    CHECK(std::memcmp(t.chunks.data(), s.chunks.data(), s.chunks.size() * 32) == 0);
    CHECK(std::memcmp(t.boundsMin, s.boundsMin, 12) == 0 && std::memcmp(t.boundsMax, s.boundsMax, 12) == 0);
    // 空
    FoliageInstanceSet e;
    e.Rebuild();
    FoliageInstanceSet e2;
    CHECK(DecodeFoliage(EncodeFoliage(e), e2));
    CHECK(e2.Count() == 0);
    // 破損の拒否
    {
        std::vector<u8> b = bytes;
        b[100] ^= 0x55;   // ペイロードの 1 バイト
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(b, x, &err));
    }
    {
        std::vector<u8> b(bytes.begin(), bytes.end() - 5);   // 途中で切れている
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(b, x, &err));
    }
    {
        std::vector<u8> b = bytes;
        b[0] = 'X';   // マジック
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(b, x, &err));
    }
    {
        std::vector<u8> b(10, 0);
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(b, x, &err));
        CHECK(!DecodeFoliage(nullptr, 0, x, &err));
    }
    {
        // NaN の位置は CRC が合っていても拒否（描画側へ渡さない）
        FoliageInstanceSet bad = s;
        bad.instances[5].px = std::nanf("");
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(EncodeFoliage(bad), x, &err));
    }
    {
        // 巨大件数（ヘッダだけ改ざんして CRC を合わせ直しても、サイズ不一致 / 上限で拒否）
        std::vector<u8> b = bytes;
        const u32 huge = 0xFFFFFFF0u;
        std::memcpy(b.data() + 8, &huge, 4);   // instanceCount
        FoliageInstanceSet x;
        CHECK(!DecodeFoliage(b, x, &err));
    }
    // 名前 → パス
    CHECK(MakeFoliageRelPath("Grass 1/x") == "foliage/Grass_1_x.dxfoliage");
    CHECK(MakeFoliageRelPath("") == "foliage/Foliage.dxfoliage");
    CHECK(Crc32(reinterpret_cast<const u8*>("123456789"), 9) == 0xCBF43926u);   // 標準の CRC32 テストベクタ
}

// 表面: 傾いた面 / 高さの帯 / スプラット
void Test_Scatter()
{
    // 平面・一様
    {
        ScatterParams p; p.seed = 5; p.density = 0.3f; p.minSpacing = 1.5f;
        ScatterRegion r; r.x0 = -50; r.z0 = -50; r.x1 = 50; r.z1 = 50;
        SurfaceFn flat = [](f32, f32, SurfaceSample& s) { s.y = 2.0f; return true; };
        std::vector<FoliageInstance> out;
        const ScatterStats st = ScatterPoisson(p, r, flat, out);
        CHECK(out.size() > 500);
        CHECK(st.accepted == out.size());
        // 最小間隔（ポアソンディスク）
        const f32 md = MinPairDistanceXZ(out, 2.0f);
        CHECK(md >= 1.5f - 1e-3f);
        // 密度: 0.3/m² × 10000 m² = 3000 に対し ±25%（最大充填から間引く）
        CHECK(out.size() > 2200 && out.size() < 3800);
        // 領域内・高さ
        bool inside = true;
        for (const auto& in : out) if (in.px < -50 || in.px > 50 || in.pz < -50 || in.pz > 50 || in.py != 2.0f) inside = false;
        CHECK(inside);
        // 決定論
        std::vector<FoliageInstance> out2;
        ScatterPoisson(p, r, flat, out2);
        CHECK(out2.size() == out.size() && std::memcmp(out.data(), out2.data(), out.size() * 32) == 0);
        // seed が違えば違う
        ScatterParams p2 = p; p2.seed = 6;
        std::vector<FoliageInstance> out3;
        ScatterPoisson(p2, r, flat, out3);
        CHECK(out3.size() != out.size() || std::memcmp(out.data(), out3.data(), std::min(out.size(), out3.size()) * 32) != 0);
    }
    // 最大充填（density を大きく）でも最小間隔は守られる
    {
        ScatterParams p; p.seed = 1; p.density = 100.0f; p.minSpacing = 2.0f;
        ScatterRegion r; r.x0 = 0; r.z0 = 0; r.x1 = 40; r.z1 = 40;
        SurfaceFn flat = [](f32, f32, SurfaceSample& s) { s.y = 0; return true; };
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, flat, out);
        CHECK(out.size() > 100);
        CHECK(MinPairDistanceXZ(out, 3.0f) >= 2.0f - 1e-3f);
    }
    // 一様ランダム（minSpacing = 0）: 密度に忠実
    {
        ScatterParams p; p.seed = 9; p.density = 1.0f; p.minSpacing = 0.0f;
        ScatterRegion r; r.x0 = 0; r.z0 = 0; r.x1 = 100; r.z1 = 100;
        SurfaceFn flat = [](f32, f32, SurfaceSample& s) { s.y = 0; return true; };
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, flat, out);
        CHECK(out.size() > 9000 && out.size() < 11000);
    }
    // 傾斜: x > 0 は 60° の斜面、x <= 0 は平ら。maxSlope 30° なら x <= 0 だけ
    {
        SurfaceFn slope = [](f32 x, f32, SurfaceSample& s)
        {
            s.y = 0;
            if (x > 0) { const f32 a = 60.0f * kPi / 180.0f; s.nx = std::sin(a); s.ny = std::cos(a); s.nz = 0; }
            else { s.nx = 0; s.ny = 1; s.nz = 0; }
            return true;
        };
        ScatterParams p; p.seed = 2; p.density = 0.5f; p.maxSlopeDeg = 30.0f;
        ScatterRegion r; r.x0 = -30; r.z0 = -30; r.x1 = 30; r.z1 = 30;
        std::vector<FoliageInstance> out;
        const ScatterStats st = ScatterPoisson(p, r, slope, out);
        bool allFlat = true;
        for (const auto& in : out) if (in.px > 0) allFlat = false;
        CHECK(allFlat);
        CHECK(st.rejectedSlope > 100);
        // minSlope 45° なら逆に x > 0 だけ
        p.maxSlopeDeg = 90.0f; p.minSlopeDeg = 45.0f;
        out.clear();
        ScatterPoisson(p, r, slope, out);
        bool allSlope = !out.empty();
        for (const auto& in : out) if (in.px <= 0) allSlope = false;
        CHECK(allSlope);
    }
    // 高度の帯
    {
        SurfaceFn ramp = [](f32 x, f32, SurfaceSample& s) { s.y = x; return true; };
        ScatterParams p; p.seed = 3; p.density = 0.5f; p.minHeight = -5.0f; p.maxHeight = 5.0f;
        ScatterRegion r; r.x0 = -30; r.z0 = -30; r.x1 = 30; r.z1 = 30;
        std::vector<FoliageInstance> out;
        const ScatterStats st = ScatterPoisson(p, r, ramp, out);
        bool ok = !out.empty();
        for (const auto& in : out) if (in.py < -5.0f || in.py > 5.0f) ok = false;
        CHECK(ok);
        CHECK(st.rejectedHeight > 500);
    }
    // スプラット: layer 1 の重みが x > 0 で 1、x <= 0 で 0
    {
        SurfaceFn sp = [](f32 x, f32, SurfaceSample& s) { s.y = 0; s.splat[1] = (x > 0) ? 1.0f : 0.0f; return true; };
        ScatterParams p; p.seed = 4; p.density = 0.5f; p.splatLayer = 1; p.splatThreshold = 0.5f;
        ScatterRegion r; r.x0 = -30; r.z0 = -30; r.x1 = 30; r.z1 = 30;
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, sp, out);
        bool ok = !out.empty();
        for (const auto& in : out) if (in.px <= 0) ok = false;
        CHECK(ok);
    }
    // 表面が無い所（穴）には置かない
    {
        SurfaceFn hole = [](f32 x, f32 z, SurfaceSample& s) { if (x * x + z * z < 100.0f) return false; s.y = 0; return true; };
        ScatterParams p; p.seed = 4; p.density = 0.5f;
        ScatterRegion r; r.x0 = -30; r.z0 = -30; r.x1 = 30; r.z1 = 30;
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, hole, out);
        bool ok = !out.empty();
        for (const auto& in : out) if (in.px * in.px + in.pz * in.pz < 100.0f) ok = false;
        CHECK(ok);
    }
    // 円領域 / 上限 / 既存との最小間隔
    {
        SurfaceFn flat = [](f32, f32, SurfaceSample& s) { s.y = 0; return true; };
        ScatterParams p; p.seed = 8; p.density = 1.0f; p.minSpacing = 1.0f; p.maxCount = 50;
        ScatterRegion r; r.circle = true; r.cx = 10; r.cz = -5; r.radius = 20;
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, flat, out);
        CHECK(out.size() == 50);
        bool inC = true;
        for (const auto& in : out) if ((in.px - 10) * (in.px - 10) + (in.pz + 5) * (in.pz + 5) > 400.01f) inC = false;
        CHECK(inC);
        // 追加: 既存との間隔も守る
        ScatterParams p2 = p; p2.maxCount = 100000; p2.seed = 99; p2.density = 1.0f;
        std::vector<FoliageInstance> all = out;
        ScatterPoisson(p2, r, flat, all, &out);
        CHECK(all.size() > out.size());
        CHECK(MinPairDistanceXZ(all, 2.0f) >= 1.0f - 1e-3f);
    }
    // 種別の重み
    {
        SurfaceFn flat = [](f32, f32, SurfaceSample& s) { s.y = 0; return true; };
        ScatterParams p; p.seed = 12; p.density = 2.0f; p.variantCount = 3; p.variantWeight[0] = 1; p.variantWeight[1] = 2; p.variantWeight[2] = 0;
        ScatterRegion r; r.x0 = 0; r.z0 = 0; r.x1 = 50; r.z1 = 50;
        std::vector<FoliageInstance> out;
        ScatterPoisson(p, r, flat, out);
        u32 c[3] = {};
        for (const auto& in : out) c[std::min(TypeOf(in.seedType), 2u)]++;
        CHECK(c[2] == 0);
        CHECK(c[1] > c[0] && c[0] > 0);
    }
    // 削除（決定論・割合）
    {
        std::vector<FoliageInstance> v = RandomInstances(20000, 50.0f, 5);
        auto w = v;
        const u32 r1 = EraseInCircle(v, 0, 0, 20.0f, 0.5f, 1);
        const u32 r2 = EraseInCircle(w, 0, 0, 20.0f, 0.5f, 1);
        CHECK(r1 == r2 && v.size() == w.size());
        u32 inside = 0;
        for (const auto& in : RandomInstances(20000, 50.0f, 5)) if (in.px * in.px + in.pz * in.pz <= 400.0f) ++inside;
        CHECK(r1 > inside * 0.4 && r1 < inside * 0.6);
        auto all = RandomInstances(20000, 50.0f, 5);
        CHECK(EraseInCircle(all, 0, 0, 1000.0f, 1.0f, 1) == 20000u && all.empty());
    }
}

CullParams BaseParams()
{
    CullParams p;
    p.cullDist = 200.0f; p.lodDist[0] = 20.0f; p.lodDist[1] = 50.0f; p.lodDist[2] = 100.0f;
    p.thinStart = 0.6f; p.lodFade = 0.0f; p.boundRadius = 2.0f; p.boundCenterY = 1.0f;
    p.variantCount = 1;
    p.lodCount[0] = 4;
    // 全方向を含む大きな平面（錐台の代わり）
    for (int i = 0; i < 6; ++i) { p.planes[i][0] = 0; p.planes[i][1] = 1; p.planes[i][2] = 0; p.planes[i][3] = 1e6f; }
    p.camPos[0] = p.camPos[1] = p.camPos[2] = 0;
    return p;
}

void Test_CullReference()
{
    const Affine34 I;
    // 距離 / LOD（クロスフェード無し・間引き無し）
    {
        CullParams p = BaseParams();
        p.thinStart = 0.999f;
        u32 lodAt[6] = {};
        const f32 d[6] = {5, 19.9f, 20.1f, 60, 101, 150};
        for (int i = 0; i < 6; ++i)
        {
            FoliageInstance in = MakeInstance(d[i], 0, 0, 12345);
            const EmitInfo e = EvaluateInstance(in, I, p);
            CHECK(e.count == 1);
            lodAt[i] = e.lod[0];
        }
        CHECK(lodAt[0] == 0 && lodAt[1] == 0 && lodAt[2] == 1 && lodAt[3] == 2 && lodAt[4] == 3 && lodAt[5] == 3);
        // 遠すぎ
        CHECK(EvaluateInstance(MakeInstance(200.5f, 0, 0, 1), I, p).count == 0);
        // LOD が 2 段しかない種別は最後の LOD に丸まる
        CullParams p2 = p; p2.lodCount[0] = 2;
        CHECK(EvaluateInstance(MakeInstance(150, 0, 0, 1), I, p2).lod[0] == 1);
    }
    // 間引き: 距離が伸びるほど残る割合が単調に減る。cullDist に近づくと 0
    {
        CullParams p = BaseParams();
        p.thinStart = 0.5f;
        f64 prev = 2.0;
        for (f32 dist : {50.0f, 100.0f, 120.0f, 150.0f, 180.0f, 199.0f})
        {
            u32 kept = 0; const u32 n = 20000;
            for (u32 i = 0; i < n; ++i) if (EvaluateInstance(MakeInstance(dist, 0, 0, i * 2654435u & 0xFFFFFF), I, p).count > 0) ++kept;
            const f64 frac = static_cast<f64>(kept) / n;
            CHECK(frac <= prev + 0.02);
            prev = frac;
            const f32 keepProb = std::min(1.0f, ThinKeepProb(dist, p.cullDist, p.thinStart));
            // 期待値: h < keepProb（帯の外側では b>0 ⇔ h < keep）
            CHECK_NEAR(frac, std::min(1.0, static_cast<f64>(keepProb)), 0.03);
        }
        CHECK(prev < 0.1);
        // 密度 1（thinStart 手前）は全部残る
        u32 kept = 0;
        for (u32 i = 0; i < 5000; ++i) if (EvaluateInstance(MakeInstance(60, 0, 0, i), I, p).count > 0) ++kept;
        CHECK(kept == 5000u);
    }
    // クロスフェード: 帯の中では隣り合う 2 LOD に出て、ディザが補完的（合計 = 1）
    {
        CullParams p = BaseParams();
        p.lodFade = 0.1f; p.thinStart = 0.999f;
        // 帯 [18, 22]
        for (f32 dist : {18.5f, 19.5f, 20.0f, 21.0f, 21.9f})
        {
            const EmitInfo e = EvaluateInstance(MakeInstance(dist, 0, 0, 777), I, p);
            CHECK(e.count == 2 && e.lod[0] == 0 && e.lod[1] == 1);
            u32 both = 0, neither = 0;
            const u32 n = 2000;
            for (u32 i = 0; i < n; ++i)
            {
                const f32 noise = (static_cast<f32>(i) + 0.5f) / n;
                const bool k0 = !(noise >= e.lo[0] && noise <= e.hi[0]);
                const bool k1 = !(noise >= e.lo[1] && noise <= e.hi[1]);
                if (k0 && k1) ++both;
                if (!k0 && !k1) ++neither;
            }
            CHECK(both == 0 && neither <= 1);   // 排他かつ全被覆（境界 1 個の誤差を許す）
        }
        // 帯の外は 1 LOD
        CHECK(EvaluateInstance(MakeInstance(15, 0, 0, 1), I, p).count == 1);
        CHECK(EvaluateInstance(MakeInstance(30, 0, 0, 1), I, p).count == 1);
        // 距離とともに LOD0 の占める割合は 1 → 0（単調）
        f64 prevFrac = 1.1;
        for (int k = 1; k < 20; ++k)
        {
            const f32 dist = 18.0f + k * 0.2f;
            const EmitInfo e = EvaluateInstance(MakeInstance(dist, 0, 0, 5), I, p);
            const f64 frac = e.lo[0];   // = (1-t)*b, b=1
            CHECK(frac <= prevFrac + 1e-6);
            prevFrac = frac;
        }
    }
    // 影パス: LOD 1 つ・shadowMaxLod 超えは棄却・ディザ無し
    {
        CullParams p = BaseParams();
        p.shadowPass = true; p.shadowMaxLod = 1; p.lodFade = 0.1f; p.thinStart = 0.999f;
        const EmitInfo a = EvaluateInstance(MakeInstance(21, 0, 0, 1), I, p);   // 帯の中でも 1 つだけ
        CHECK(a.count == 1 && a.lod[0] == 1 && a.lo[0] >= 1.0f);
        CHECK(EvaluateInstance(MakeInstance(60, 0, 0, 1), I, p).count == 0);    // LOD2 は影を落とさない
        CHECK(EvaluateInstance(MakeInstance(10, 0, 0, 1), I, p).lod[0] == 0);
    }
    // 錐台: 実際のカメラで Frustum::SphereVisible と一致
    {
        using namespace DirectX;
        const XMMATRIX vp = XMMatrixLookAtLH(XMVectorSet(0, 5, -20, 1), XMVectorSet(0, 2, 0, 1), XMVectorSet(0, 1, 0, 0))
                          * XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 300.0f);
        XMFLOAT4X4 vpf; XMStoreFloat4x4(&vpf, vp);
        CullParams p = BaseParams();
        p.cullDist = 1000.0f; p.thinStart = 0.999f;
        PlanesFromViewProj(vpf, p.planes);
        p.camPos[0] = 0; p.camPos[1] = 5; p.camPos[2] = -20;
        p.boundCenterY = 0.0f; p.boundRadius = 1.5f;
        const Frustum fr = Frustum::FromViewProj(vp);
        std::mt19937 rng(3);
        std::uniform_real_distribution<f32> u(-60.0f, 60.0f);
        u32 agree = 0, total = 3000, inside = 0;
        for (u32 i = 0; i < total; ++i)
        {
            const f32 x = u(rng), y = u(rng) * 0.3f, z = u(rng);
            const FoliageInstance in = MakeInstance(x, y, z, i);
            f32 c[3]; f32 r;
            EvaluateInstance(in, I, p, c, &r);
            const bool cpu = SphereInPlanes(p.planes, c, r);
            const bool eng = fr.SphereVisible(XMVectorSet(c[0], c[1], c[2], 1), r);
            if (cpu == eng) ++agree;
            if (eng) ++inside;
        }
        CHECK(agree >= total - 3);
        CHECK(inside > 200 && inside < total - 200);
    }
    // レイヤーの Transform（回転 90° + 平行移動 + スケール 2）が中心 / 半径に反映される
    {
        using namespace DirectX;
        XMFLOAT4X4 w;
        XMStoreFloat4x4(&w, XMMatrixScaling(2, 2, 2) * XMMatrixRotationY(XM_PIDIV2) * XMMatrixTranslation(10, 0, 0));
        const Affine34 layer = AffineFromWorld(w);
        const FoliageInstance in = MakeInstance(1, 0, 0, 1, 0, 0.0f, 1.0f);
        const Affine34 m = ComposeInstance(in, layer);
        // ローカル (1,0,0) をワールドへ: 回転 90°(Y) + スケール 2 + 平行移動。XMMatrixRotationY(+90°) は行ベクトルで x→-z
        f32 p[3] = {1, 0, 0}, o[3];
        layer.Apply(p, o);
        CHECK_NEAR(m.m[0][3], o[0], 1e-4); CHECK_NEAR(m.m[1][3], o[1], 1e-4); CHECK_NEAR(m.m[2][3], o[2], 1e-4);
        CHECK_NEAR(MaxColumnLength(layer), 2.0, 1e-4);
    }
    // ComposeInstance: 傾き 0・yaw 0・スケール s は単位行列 × s
    {
        const Affine34 m = ComposeInstance(MakeInstance(3, 4, 5, 1, 0, 0.0f, 2.0f), I);
        CHECK_NEAR(m.m[0][0], 2.0, 2e-3); CHECK_NEAR(m.m[1][1], 2.0, 2e-3); CHECK_NEAR(m.m[2][2], 2.0, 2e-3);
        CHECK_NEAR(m.m[0][3], 3.0, 1e-6); CHECK_NEAR(m.m[1][3], 4.0, 1e-6); CHECK_NEAR(m.m[2][3], 5.0, 1e-6);
        // 直交（回転 + 一様スケール）: 列の内積 0・長さ = スケール
        const Affine34 g = ComposeInstance(MakeInstance(0, 0, 0, 1, 0, 1.3f, 1.5f), I);
        // 傾きも付ける
        FoliageInstance t = MakeInstance(0, 0, 0, 1, 0, 1.3f, 1.5f);
        t.tilt = PackTilt(0.3f, -0.2f);
        const Affine34 h = ComposeInstance(t, I);
        for (const Affine34* a : {&g, &h})
        {
            f32 col[3][3];
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) col[c][r] = a->m[r][c];
            for (int c = 0; c < 3; ++c)
                CHECK_NEAR(std::sqrt(col[c][0] * col[c][0] + col[c][1] * col[c][1] + col[c][2] * col[c][2]), 1.5, 2e-3);
            CHECK_NEAR(col[0][0] * col[1][0] + col[0][1] * col[1][1] + col[0][2] * col[1][2], 0.0, 1e-3);
        }
    }
}

void Test_Wind()
{
    WindFrame f;
    f.dirX = 1.0f; f.dirZ = 0.0f; f.speed = 6.0f; f.gustStrength = 0.6f; f.gustFreq = 0.4f; f.turbulence = 0.3f; f.time = 12.5f; f.prevTime = 12.4f;
    WindLayer l; l.bend = 1.0f; l.flutter = 0.1f; l.bendExp = 2.0f; l.heightLocal = 7.0f;
    const f32 origin[3] = {10, 0, 20};
    const f32 nrm[3] = {0, 0, 1};
    const f32 vc[3] = {0.8f, 0.3f, 1.0f};
    auto delta = [&](f32 y, f32 t, f32 phase, const WindFrame& fr, const WindLayer& ly, f32 out[3])
    {
        const f32 wp[3] = {10.5f, y, 20.2f};
        WindDelta(wp, origin, nrm, y, vc, phase, 1.0f, 1.0f, fr, ly, t, out);
    };
    // 決定論
    f32 a[3], b[3];
    delta(5.0f, 12.5f, 0.37f, f, l, a);
    delta(5.0f, 12.5f, 0.37f, f, l, b);
    CHECK(std::memcmp(a, b, 12) == 0);
    // 無風で 0
    WindFrame calm = f; calm.speed = 0.0f;
    delta(5.0f, 12.5f, 0.37f, calm, l, a);
    CHECK(a[0] == 0 && a[1] == 0 && a[2] == 0);
    // 根元（高さ 0）は動かない・高いほど大きい（曲げのみ）
    WindLayer bendOnly = l; bendOnly.flutter = 0.0f;
    f32 d0[3], d3[3], d7[3];
    delta(0.0f, 12.5f, 0.37f, f, bendOnly, d0);
    delta(3.5f, 12.5f, 0.37f, f, bendOnly, d3);
    delta(7.0f, 12.5f, 0.37f, f, bendOnly, d7);
    auto len = [](const f32 v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    CHECK(len(d0) < 1e-4f);
    CHECK(len(d3) < len(d7));
    CHECK(len(d7) > 0.05f && len(d7) < 3.0f);   // 7m の木・風 6m/s で 5cm〜3m
    // 曲げは長さを保つ: |wp + delta - origin| ≈ |wp - origin|
    for (f32 y : {1.0f, 3.5f, 7.0f})
    {
        f32 d[3]; delta(y, 12.5f, 0.37f, f, bendOnly, d);
        const f32 wp[3] = {10.5f, y, 20.2f};
        const f32 r1 = std::sqrt((wp[0] - origin[0]) * (wp[0] - origin[0]) + (wp[1] - origin[1]) * (wp[1] - origin[1]) + (wp[2] - origin[2]) * (wp[2] - origin[2]));
        const f32 r2 = std::sqrt((wp[0] + d[0] - origin[0]) * (wp[0] + d[0] - origin[0]) + (wp[1] + d[1] - origin[1]) * (wp[1] + d[1] - origin[1])
                               + (wp[2] + d[2] - origin[2]) * (wp[2] + d[2] - origin[2]));
        CHECK_NEAR(r2, r1, 1e-3);
    }
    // 時間に連続（10ms で大きく飛ばない）
    f32 prev[3]; delta(7.0f, 10.0f, 0.2f, f, l, prev);
    f32 maxJump = 0;
    for (int i = 1; i <= 400; ++i)
    {
        f32 cur[3]; delta(7.0f, 10.0f + i * 0.01f, 0.2f, f, l, cur);
        maxJump = std::max(maxJump, std::sqrt((cur[0] - prev[0]) * (cur[0] - prev[0]) + (cur[1] - prev[1]) * (cur[1] - prev[1]) + (cur[2] - prev[2]) * (cur[2] - prev[2])));
        std::memcpy(prev, cur, 12);
    }
    CHECK(maxJump < 0.25f);
    // 位相がインスタンスごとに違う
    std::set<u32> distinct;
    for (u32 s = 0; s < 32; ++s)
    {
        f32 d[3]; delta(7.0f, 12.5f, Hash01(s * 977u), f, l, d);
        distinct.insert(static_cast<u32>(d[0] * 1000.0f) * 31u + static_cast<u32>(d[2] * 1000.0f));
    }
    CHECK(distinct.size() > 20);
    // 風下方向へ倒れる（dir = +X なら平均 +X）
    f64 sumX = 0;
    for (int i = 0; i < 200; ++i) { f32 d[3]; delta(7.0f, 5.0f + i * 0.13f, 0.5f, f, bendOnly, d); sumX += d[0]; }
    CHECK(sumX > 0);
    // はばたき: 頂点色 R = 0 では出ない・R > 0 で出る
    {
        WindLayer flutterOnly = l; flutterOnly.bend = 0.0f; flutterOnly.flutter = 0.2f;
        const f32 vc0[3] = {0.0f, 0.3f, 1.0f}, vc1[3] = {1.0f, 0.3f, 1.0f};
        const f32 wp[3] = {10, 3, 20};
        f32 d0v[3], d1v[3];
        WindDelta(wp, origin, nrm, 3.0f, vc0, 0.4f, 1.0f, 1.0f, f, flutterOnly, 5.0f, d0v);
        WindDelta(wp, origin, nrm, 3.0f, vc1, 0.4f, 1.0f, 1.0f, f, flutterOnly, 5.0f, d1v);
        CHECK(len(d0v) < 1e-6f);
        CHECK(len(d1v) > 1e-3f && len(d1v) <= 0.2f + 1e-4f);
    }
    // 速度用: prevTime と time が同じなら差は 0（一時停止・決定論キャプチャで速度が 0 になる）
    {
        f32 x0[3], x1[3];
        delta(5.0f, 3.0f, 0.1f, f, l, x0);
        delta(5.0f, 3.0f, 0.1f, f, l, x1);
        CHECK(std::memcmp(x0, x1, 12) == 0);
    }
    // SceneWind → WindFrame（向き / 無効 / phaseOffset）
    {
        SceneWind w; w.directionDeg = 90.0f; w.speed = 4.0f; w.phaseOffset = 2.0f;
        const WindFrame fr = MakeWindFrame(w, 10.0f, 9.9f);
        CHECK_NEAR(fr.dirX, 0.0, 1e-6); CHECK_NEAR(fr.dirZ, 1.0, 1e-6);
        CHECK_NEAR(fr.time, 12.0, 1e-6); CHECK_NEAR(fr.prevTime, 11.9, 1e-5);
        w.enabled = false;
        CHECK(MakeWindFrame(w, 1, 1).speed == 0.0f);
        CHECK(SceneWind{} == SceneWind{});
    }
}

void Test_LayerOps()
{
    auto v = ParseLodList(" a.glb ; b.glb;;c.glb ; ");
    CHECK(v.size() == 3 && v[0] == "a.glb" && v[2] == "c.glb");
    CHECK(ParseLodList("").empty());
    CHECK(ParseLodList("a;b;c;d;e;f").size() == kMaxLods);
    CHECK(JoinLodList({"x", "y"}) == "x;y");
    FoliageLayer l;
    CHECK(CountVariants(l) == 0);
    l.variant0 = "a;b"; l.variant1 = "c";
    CHECK(CountVariants(l) == 2);
    l.variant1.clear(); l.variant2 = "z";
    CHECK(CountVariants(l) == 1);   // 途中に空があればそこまで
    // カリング定数: クロスフェード帯が重ならないよう縮む
    l.lodDist0 = 10; l.lodDist1 = 11; l.lodDist2 = 12; l.lodFade = 0.4f;
    const u32 lc[4] = {3, 3, 3, 3};
    const CullParams p = MakeLayerCullParams(l, 1, lc);
    for (int k = 0; k < 2; ++k)
        CHECK(p.lodDist[k] * (1.0f + p.lodFade) <= p.lodDist[k + 1] * (1.0f - p.lodFade) + 1e-4f);
    // 複製は _set を共有する（コピーオンライト前提）: 片方の差し替えがもう片方に漏れない
    auto s1 = std::make_shared<FoliageInstanceSet>();
    s1->instances = RandomInstances(10, 5, 1); s1->Rebuild();
    FoliageLayer a, b;
    a._set = s1; b = a;
    CHECK(b._set.get() == a._set.get());
    auto s2 = std::make_shared<FoliageInstanceSet>();
    ReplaceSet(b, s2);
    CHECK(a._set.get() == s1.get() && b._set.get() == s2.get() && b._needsSave && InstanceCount(b) == 0 && InstanceCount(a) == 10);
}

void Test_Scale1M()
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    FoliageInstanceSet s;
    s.instances = RandomInstances(1000000, 500.0f, 21);
    const auto t1 = std::chrono::high_resolution_clock::now();
    s.Rebuild();
    const auto t2 = std::chrono::high_resolution_clock::now();
    const std::vector<u8> bytes = EncodeFoliage(s);
    const auto t3 = std::chrono::high_resolution_clock::now();
    FoliageInstanceSet back;
    const bool ok = DecodeFoliage(bytes, back);
    const auto t4 = std::chrono::high_resolution_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("  [1M] generate %.0f ms / Rebuild(chunks=%zu) %.0f ms / Encode %.0f ms (%.1f MB) / Decode+CRC %.0f ms\n",
                ms(t0, t1), s.chunks.size(), ms(t1, t2), ms(t2, t3), static_cast<double>(bytes.size()) / 1048576.0, ms(t3, t4));
    CHECK(ok);
    CHECK(back.Count() == 1000000u);
    CHECK(bytes.size() == 64 + s.chunks.size() * 32 + 32ull * 1000000);
    CHECK(ms(t3, t4) < 2000.0);   // 100 万の読み込み（CRC 込み）が 2 秒未満（実測は 100ms 台）
}
} // namespace

int main()
{
    Test_Pack();
    Test_Chunks();
    Test_IO();
    Test_Scatter();
    Test_CullReference();
    Test_Wind();
    Test_LayerOps();
    Test_Scale1M();
    std::printf("foliage_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
