// 仮想ジオメトリ P2: VgLodMath.h（LOD 選択・錐台・コーン・インスタンス変換の CPU 純関数）のテスト。GPU 不要。
//   ・画面空間誤差の数値・+INF・近距離クランプ / 錐台の内外 = 点を実際に投影した結果と一致 / 変換 = DirectXMath と一致
//   ・法線コーン: 定義どおり / cutoff = 1 は無効 / ミラー・非一様スケールでもオブジェクト空間の判定が保たれる
//   ・★親の射影誤差 >= 子（単調性）を、実 DAG（cook した blob）の全クラスタ x ランダムなカメラ / インスタンス変換で確かめる
//   ・カメラが遠ざかると描く三角形数が増えない（LOD の単調性）
#include "CookBench.h"
#include "VgeoCook.h"

#include "renderer/vg/VgLodMath.h"
#include "renderer/vg/VgGpuTypes.h"
#include "renderer/vg/VgeoFormat.h"

#include <DirectXMath.h>

#include <cstdio>
#include <random>
#include <vector>

using namespace dx12e::vg;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{
using namespace DirectX;

void TestProjectedError()
{
    const float cam[3] = {0, 0, 0};
    const float sph[4] = {0, 0, 100, 0};
    CHECK(std::fabs(ProjectedErrorPx(1.0f, sph, cam, 1.0f, 1000.0f, 0.1f) - 10.0f) < 1e-4f);           // 1 m / 100 m x 1000 = 10 px
    CHECK(std::fabs(ProjectedErrorPx(1.0f, sph, cam, 2.0f, 1000.0f, 0.1f) - 20.0f) < 1e-4f);           // スケールに比例
    const float sph2[4] = {0, 0, 100, 40};
    CHECK(std::fabs(ProjectedErrorPx(1.0f, sph2, cam, 1.0f, 1000.0f, 0.1f) - 1000.0f / 60.0f) < 1e-3f); // 球の最近点（半径ぶん近づく）
    const float inside[4] = {0, 0, 1, 50};
    CHECK(std::fabs(ProjectedErrorPx(0.001f, inside, cam, 1.0f, 1000.0f, 0.1f) - 0.001f * 1000.0f / 0.1f) < 1e-3f);   // 近距離は zNear で頭打ち
    CHECK(ProjectedErrorPx(std::numeric_limits<float>::infinity(), sph, cam, 1.0f, 1000.0f, 0.1f) > 1e30f);    // +INF は常に粗い
    CHECK(ProjectedErrorPx(0.0f, sph, cam, 1.0f, 1000.0f, 0.1f) == 0.0f);
    // τ は出力解像度の px: 内部 720p → 出力 1080p なら 1080 で測る / 出力が内部以下なら内部
    CHECK(std::fabs(LodProjScale(720.0f, 1080.0f, 1.7320508f) - 0.5f * 1080.0f * 1.7320508f) < 1e-3f);
    CHECK(std::fabs(LodProjScale(1080.0f, 1080.0f, 1.7320508f) - 0.5f * 1080.0f * 1.7320508f) < 1e-3f);
    CHECK(std::fabs(LodProjScale(1080.0f, 0.0f, 2.0f) - 1080.0f) < 1e-3f);
    // 影（正射のカスケード）: テクセル / m = 解像度 / 幅、誤差のテクセル数は距離に依らない、+INF は +INF、半径 0 は 0
    CHECK(std::fabs(CascadeTexelsPerMeter(2048.0f, 10.0f) - 102.4f) < 1e-3f);
    CHECK(CascadeTexelsPerMeter(2048.0f, 0.0f) == 0.0f);
    CHECK(std::fabs(ProjectedErrorOrtho(0.01f, 2.0f, 102.4f) - 2.048f) < 1e-4f);
    CHECK(ProjectedErrorOrtho(std::numeric_limits<float>::infinity(), 1.0f, 100.0f) > 1e30f);
    // プロキシで足りるか: 偏差 1 cm・テクセル 2 cm（50 テクセル / m）・τ 0.5 → 0.5 テクセル = ちょうど足りる / 偏差 1.1 cm は足りない
    CHECK(ShadowProxySuffices(0.01f, 50.0f, 0.5f));
    CHECK(!ShadowProxySuffices(0.011f, 50.0f, 0.5f));
    CHECK(ShadowProxySuffices(0.0f, 50.0f, 0.5f));             // プロキシ = LOD0（小さいメッシュ）なら常にプロキシ
    // 単調: 親の誤差 >= 子なら正射でも親の射影 >= 子（スケール・解像度に依らない）
    CHECK(ProjectedErrorOrtho(0.02f, 1.5f, 80.0f) >= ProjectedErrorOrtho(0.01f, 1.5f, 80.0f));
    // 最遠点版は近点版以下（遠いほど小さく見える）
    CHECK(ProjectedErrorPxFar(1.0f, sph2, cam, 1.0f, 1000.0f, 0.1f) <= ProjectedErrorPx(1.0f, sph2, cam, 1.0f, 1000.0f, 0.1f));
}

void TestFrustum()
{
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    for (int t = 0; t < 40; ++t)
    {
        const XMVECTOR eye = XMVectorSet(U(rng) * 10, U(rng) * 10, U(rng) * 10, 1);
        const XMVECTOR at = XMVectorSet(U(rng) * 10, U(rng) * 10, U(rng) * 10, 1);
        if (XMVectorGetX(XMVector3Length(at - eye)) < 1.0f) continue;
        const XMMATRIX vp = XMMatrixLookAtLH(eye, at, XMVectorSet(0.1f, 1, 0.05f, 0)) * XMMatrixPerspectiveFovLH(1.0f + 0.3f * U(rng), 1.6f, 0.2f, 300.0f);
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, vp);
        FrustumPlanes fp;
        ExtractFrustumPlanes(&m._11, fp);
        int agree = 0, total = 0;
        for (int i = 0; i < 400; ++i)
        {
            const float p[3] = {U(rng) * 80, U(rng) * 80, U(rng) * 80};
            const XMVECTOR clip = XMVector4Transform(XMVectorSet(p[0], p[1], p[2], 1), vp);
            const float w = XMVectorGetW(clip), x = XMVectorGetX(clip), y = XMVectorGetY(clip), z = XMVectorGetZ(clip);
            const bool inside = (w > 0) && (x >= -w) && (x <= w) && (y >= -w) && (y <= w) && (z >= 0) && (z <= w);
            const bool outside = SphereOutsideFrustum(fp, p, 0.0f);
            // 境界の float 誤差は許容（内側の点が外と判定されない / 外側が内と判定されないのを厳密に）
            const float margin = 1e-3f * std::max(1.0f, std::fabs(w));
            const bool clearlyInside = (w > margin) && (x > -w + margin) && (x < w - margin) && (y > -w + margin) && (y < w - margin) && (z > margin) && (z < w - margin);
            const bool clearlyOutside = !((w > -margin) && (x >= -w - margin) && (x <= w + margin) && (y >= -w - margin) && (y <= w + margin) && (z >= -margin) && (z <= w + margin));
            if (clearlyInside) { CHECK(!outside); ++total; agree += !outside; }
            if (clearlyOutside) { CHECK(outside); ++total; agree += outside; }
            (void)inside;
        }
        (void)agree; (void)total;
        // 半径を持つ球: 中心が外でも半径が届けば「外側ではない」
        const float far0[3] = {0, 0, 0};
        (void)far0;
    }
    // 単純な構成: カメラ原点・+Z 向き。近傍の球
    {
        const XMMATRIX vp = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1), XMVectorSet(0, 1, 0, 0)) * XMMatrixPerspectiveFovLH(1.0f, 1.0f, 0.1f, 100.0f);
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, vp);
        FrustumPlanes fp;
        ExtractFrustumPlanes(&m._11, fp);
        const float front[3] = {0, 0, 10}, behind[3] = {0, 0, -10}, side[3] = {50, 0, 10}, farp[3] = {0, 0, 200};
        CHECK(!SphereOutsideFrustum(fp, front, 1.0f));
        CHECK(SphereOutsideFrustum(fp, behind, 1.0f));
        CHECK(SphereOutsideFrustum(fp, side, 1.0f));
        CHECK(!SphereOutsideFrustum(fp, side, 60.0f));      // 半径が届く
        CHECK(SphereOutsideFrustum(fp, farp, 1.0f));
        CHECK(!SphereOutsideFrustum(fp, behind, 11.0f));    // 後ろでも近平面を跨ぐ半径なら外側ではない（保守的）
    }
}

void TestTransforms()
{
    std::mt19937 rng(2);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    for (int t = 0; t < 200; ++t)
    {
        const float sx = 0.2f + std::fabs(U(rng)) * 3, sy = 0.2f + std::fabs(U(rng)) * 3, sz = 0.2f + std::fabs(U(rng)) * 3;
        const float mir = (t % 4 == 3) ? -1.0f : 1.0f;
        const XMMATRIX m = XMMatrixScaling(sx * mir, sy, sz) * XMMatrixRotationRollPitchYaw(U(rng) * 3, U(rng) * 3, U(rng) * 3) * XMMatrixTranslation(U(rng) * 20, U(rng) * 20, U(rng) * 20);
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, m);
        float rows[12];
        PackWorld34(&f._11, rows);
        const float p[3] = {U(rng) * 5, U(rng) * 5, U(rng) * 5};
        float o[3];
        TransformPoint34(rows, p, o);
        const XMVECTOR ref = XMVector3Transform(XMVectorSet(p[0], p[1], p[2], 1), m);
        CHECK(std::fabs(o[0] - XMVectorGetX(ref)) < 1e-3f && std::fabs(o[1] - XMVectorGetY(ref)) < 1e-3f && std::fabs(o[2] - XMVectorGetZ(ref)) < 1e-3f);
        CHECK((Det3(&f._11) < 0.0f) == (mir < 0.0f));
        CHECK(std::fabs(MaxAxisScale(&f._11) - std::max({sx, sy, sz})) < 1e-4f);
        // カメラのオブジェクト空間座標: world 行列で戻すと元のカメラ位置
        const float cam[3] = {U(rng) * 30, U(rng) * 30, U(rng) * 30};
        float co[3];
        CHECK(CameraToObjectSpace(&f._11, cam, co));
        const XMVECTOR back = XMVector3Transform(XMVectorSet(co[0], co[1], co[2], 1), m);
        CHECK(std::fabs(XMVectorGetX(back) - cam[0]) < 5e-3f && std::fabs(XMVectorGetY(back) - cam[1]) < 5e-3f && std::fabs(XMVectorGetZ(back) - cam[2]) < 5e-3f);
    }
    // 特異な行列（スケール 0）
    XMFLOAT4X4 z;
    XMStoreFloat4x4(&z, XMMatrixScaling(0, 1, 1));
    const float cam[3] = {1, 2, 3};
    float co[3];
    CHECK(!CameraToObjectSpace(&z._11, cam, co));
}

void TestCone()
{
    const float center[3] = {0, 0, 10}, axis[3] = {0, 0, 1};   // 軸が +Z（カメラの向き＝奥向き）: 背面
    const float cam[3] = {0, 0, 0};
    CHECK(ConeCulls(center, 1.0f, axis, 0.5f, cam));            // 面がそっぽを向いている（dot=10 >= 0.5*10+1）
    const float axisToCam[3] = {0, 0, -1};
    CHECK(!ConeCulls(center, 1.0f, axisToCam, 0.5f, cam));      // カメラを向いている
    CHECK(!ConeCulls(center, 1.0f, axis, 1.0f, cam));           // cutoff = 1 は無効
    CHECK(!ConeCulls(center, 20.0f, axis, 0.5f, cam));          // 半径が大きくカメラが球の内側 → 棄却しない
    CHECK(std::fabs(ConeAxisComponent(0x7F000000u >> 24, 0) - 127.0f / 127.0f) < 1e-6f);
    CHECK(ConeAxisComponent(0x0000FF00u, 1) < 0.0f);           // s8 の符号拡張
}

// 実 DAG の全クラスタで、親の射影誤差 >= 子（どの視点・どのインスタンス変換でも）
void TestMonotone()
{
    dx12e::vg::cook::BenchOptions bo;
    bo.kind = "blob"; bo.tris = 40000; bo.seed = 5; bo.threads = 2;
    VgsrcData d;
    CHECK(dx12e::vg::cook::GenerateBench(bo, d).ok());
    dx12e::vg::cook::CookOptions co;
    co.threads = 2; co.collectStats = false; co.proxy = false;
    VgeoContent c;
    CHECK(dx12e::vg::cook::Cook(std::move(d), co, c).ok());
    std::vector<u8> bytes;
    CHECK(WriteVgeoToMemory(c, bytes).ok());
    MemorySource ms(bytes);
    VgeoMeta meta;
    CHECK(LoadMeta(ms, meta).ok());
    std::vector<u8> pages(static_cast<size_t>(meta.header.pageCount) * kPageSize);
    for (u32 p = 0; p < meta.header.pageCount; ++p) CHECK(ReadPage(ms, meta, p, pages.data() + static_cast<size_t>(p) * kPageSize).ok());

    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    size_t checked = 0, violations = 0;
    for (int t = 0; t < 30; ++t)
    {
        const float sx = 0.3f + std::fabs(U(rng)) * 3, sy = 0.3f + std::fabs(U(rng)) * 3, sz = 0.3f + std::fabs(U(rng)) * 3;
        const XMMATRIX m = XMMatrixScaling(sx, sy, sz) * XMMatrixRotationRollPitchYaw(U(rng) * 3, U(rng) * 3, 0) * XMMatrixTranslation(U(rng) * 8, U(rng) * 8, U(rng) * 8);
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, m);
        float rows[12];
        PackWorld34(&f._11, rows);
        const float s = MaxAxisScale(&f._11);
        const float dist = std::pow(10.0f, -0.5f + 3.0f * (0.5f + 0.5f * U(rng)));   // 0.3 .. 300 m
        float dir[3] = {U(rng), U(rng), U(rng)};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-6f;
        const float cam[3] = {f._41 + dir[0] / dl * dist, f._42 + dir[1] / dl * dist, f._43 + dir[2] / dl * dist};
        for (u32 p = 0; p < meta.header.pageCount; ++p)
        {
            const PageHeader ph = ReadPageHeader(pages.data() + static_cast<size_t>(p) * kPageSize);
            for (u32 k = 0; k < ph.clusterCount; ++k)
            {
                const ClusterHeader ch = ReadClusterHeader(pages.data() + static_cast<size_t>(p) * kPageSize, k);
                float lc[3], pc[3];
                TransformPoint34(rows, ch.lodSphere, lc);
                TransformPoint34(rows, ch.parentLodSphere, pc);
                const float lsW[4] = {lc[0], lc[1], lc[2], ch.lodSphere[3] * s};
                const float psW[4] = {pc[0], pc[1], pc[2], ch.parentLodSphere[3] * s};
                const float child = ProjectedErrorPx(ch.lodError, lsW, cam, s, 1000.0f, 0.1f);
                const float parent = ProjectedErrorPx(ch.parentLodError, psW, cam, s, 1000.0f, 0.1f);
                ++checked;
                if (!(parent * 1.001f >= child)) ++violations;
            }
        }
    }
    std::printf("単調性: %zu クラスタ x 視点 / 違反 %zu\n", checked, violations);
    CHECK(violations == 0);

    // 距離の LOD 単調性: 中心から遠ざかるほど、描くクラスタの三角形数は増えない（1% の余裕）
    const XMFLOAT4X4 I = [] { XMFLOAT4X4 x; XMStoreFloat4x4(&x, XMMatrixIdentity()); return x; }();
    float rowsI[12];
    PackWorld34(&I._11, rowsI);
    u64 prev = ~0ull;
    for (float dist = 2.0f; dist < 2000.0f; dist *= 1.6f)
    {
        const float cam[3] = {0, 0, -dist};
        u64 tris = 0;
        for (u32 p = 0; p < meta.header.pageCount; ++p)
        {
            const PageHeader ph = ReadPageHeader(pages.data() + static_cast<size_t>(p) * kPageSize);
            for (u32 k = 0; k < ph.clusterCount; ++k)
            {
                const ClusterHeader ch = ReadClusterHeader(pages.data() + static_cast<size_t>(p) * kPageSize, k);
                if (ProjectedErrorPx(ch.parentLodError, ch.parentLodSphere, cam, 1.0f, 1000.0f, 0.1f) > 1.0f &&
                    !(ProjectedErrorPx(ch.lodError, ch.lodSphere, cam, 1.0f, 1000.0f, 0.1f) > 1.0f))
                    tris += ClusterTriangleCount(ch);
            }
        }
        CHECK(prev == ~0ull || static_cast<double>(tris) <= static_cast<double>(prev) * 1.01);
        prev = tris;
    }
    CHECK(prev < 5000);   // 遠距離では大きく減る
}
// 球の正確な画面矩形（HZB 判定用）: 球面上の点を密に投影した矩形を必ず含み、ほぼ一致する（ジッタ込みの VP・画面の端・遠くの小さい球）。
//   最近点の深度は球面上の点の最小 NDC 深度以下。カメラが球に掛かる / 正射は valid = false / ortho = true。
void TestSphereRect()
{
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    const float W = 1280.0f, H = 720.0f;
    int tested = 0, loose = 0;
    for (int t = 0; t < 400; ++t)
    {
        const XMVECTOR eye = XMVectorSet(U(rng) * 5, 1 + U(rng), U(rng) * 5, 1);
        const XMVECTOR at = XMVectorSet(U(rng) * 2, 1, 20, 1);
        const XMMATRIX v = XMMatrixLookAtLH(eye, at, XMVectorSet(0, 1, 0, 0));
        const XMMATRIX p = XMMatrixPerspectiveFovLH(1.0472f, W / H, 0.1f, 2000.0f) * XMMatrixTranslation(U(rng) * 0.002f, U(rng) * 0.002f, 0.0f);   // ジッタ
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, v * p);
        // 球: 視線の周り（画面の端も含む）、距離 2〜1500 m、半径 1 mm〜3 m
        const float dist = std::pow(10.0f, 0.3f + 2.9f * (0.5f + 0.5f * U(rng)));
        const XMVECTOR fwd = XMVector3Normalize(XMVectorSubtract(at, eye));
        const XMVECTOR side = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), fwd));
        const XMVECTOR up = XMVector3Cross(fwd, side);
        const XMVECTOR cen = XMVectorAdd(eye, XMVectorAdd(XMVectorScale(fwd, dist), XMVectorAdd(XMVectorScale(side, U(rng) * dist * 0.75f), XMVectorScale(up, U(rng) * dist * 0.45f))));
        const float r = std::pow(10.0f, -3.0f + 3.5f * (0.5f + 0.5f * U(rng)));
        const float c[3] = {XMVectorGetX(cen), XMVectorGetY(cen), XMVectorGetZ(cen)};
        const SphereScreenRect s = ProjectSphereToScreen(&vp._11, c, r, 0, 0, W, H);
        CHECK(!s.ortho);
        if (!s.valid) continue;
        // 球面の点を密に投影（画面内にクランプ）
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, mnz = 1e30f;
        const int N = 96;
        for (int i = 0; i <= N; ++i)
            for (int j = 0; j < 2 * N; ++j)
            {
                const float th = 3.14159265f * static_cast<float>(i) / N, ph = 3.14159265f * static_cast<float>(j) / N;
                const XMVECTOR q = XMVectorSet(c[0] + r * std::sin(th) * std::cos(ph), c[1] + r * std::cos(th), c[2] + r * std::sin(th) * std::sin(ph), 1);
                const XMVECTOR cl = XMVector4Transform(q, XMLoadFloat4x4(&vp));
                const float w = XMVectorGetW(cl);
                const float x = (XMVectorGetX(cl) / w * 0.5f + 0.5f) * W, y = (1.0f - (XMVectorGetY(cl) / w * 0.5f + 0.5f)) * H;
                mnx = std::min(mnx, std::clamp(x, 0.0f, W)); mxx = std::max(mxx, std::clamp(x, 0.0f, W));
                mny = std::min(mny, std::clamp(y, 0.0f, H)); mxy = std::max(mxy, std::clamp(y, 0.0f, H));
                mnz = std::min(mnz, XMVectorGetZ(cl) / w);
            }
        const float tol = 1e-3f + 1e-4f * std::max(mxx - mnx, mxy - mny);
        CHECK(s.minX <= mnx + tol && s.minY <= mny + tol && s.maxX >= mxx - tol && s.maxY >= mxy - tol);   // 球を必ず含む
        CHECK(s.minZ <= mnz + 1e-6f);
        // きつさ: サンプルの矩形より 2% + 0.05 px 以上広くない（サンプルの粗さぶんの余裕）
        const float slack = 0.05f + 0.02f * std::max(mxx - mnx, mxy - mny);
        if (s.minX < mnx - slack || s.minY < mny - slack || s.maxX > mxx + slack || s.maxY > mxy + slack) ++loose;
        ++tested;
    }
    std::printf("球の画面矩形: %d ケース / ゆるい %d\n", tested, loose);
    CHECK(tested > 200);
    CHECK(loose == 0);
    // カメラが球の中 → valid = false。正射 → ortho
    {
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1), XMVectorSet(0, 1, 0, 0)) * XMMatrixPerspectiveFovLH(1.0f, 1.0f, 0.1f, 100.0f));
        const float c0[3] = {0, 0, 0.5f};
        CHECK(!ProjectSphereToScreen(&vp._11, c0, 1.0f, 0, 0, W, H).valid);
        XMStoreFloat4x4(&vp, XMMatrixOrthographicLH(10, 10, 0.1f, 100.0f));
        const float c1[3] = {0, 0, 5};
        CHECK(ProjectSphereToScreen(&vp._11, c1, 1.0f, 0, 0, W, H).ortho);
    }
}
} // namespace

int main()
{
    TestProjectedError();
    TestFrustum();
    TestTransforms();
    TestCone();
    TestSphereRect();
    TestMonotone();
    std::printf("VgLodMathTests: %d チェック / 失敗 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
