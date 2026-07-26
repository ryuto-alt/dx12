// クラスタードライティング（Forward+）の数式テスト。
//
// 守りたい不変条件:
//   (A) Z の指数分割は「スライス境界の逆写像」と往復で一致する。
//       slice(zSlice(k)) == k。ここが崩れるとライトがひとつ手前/奥のクラスタへ落ちて
//       「近づくと急にライトが消える」という再現しにくいバグになる。
//   (B) スライスは常に 0..gridZ-1 にクランプされる。特に viewZ<=0（カメラ背面）で
//       log2 が NaN 化しないこと。
//   (C) 球 vs AABB / 円錐 vs 球のカリング判定が、明らかに当たる/外れるケースで正しい。
//       保守的（false positive は許すが false negative は許さない）であること。
//
// GPU 不要（純粋な C++ + ヘッダオンリーの ClusterMath.h）。
// 実行: ctest --output-on-failure
//
// ★HLSL 側（shaders/forward/ClusterCommon.hlsli）は同じ式を手で写している。
//   片方だけ直すとこのテストは通ったまま画面だけ壊れるので、必ず両方直すこと。

#include "renderer/ClusterMath.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

bool feq(float a, float b, float tol = 1e-4f)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
}
} // namespace

int main()
{
    std::printf("cluster_math_test\n");

    const float zNear = 0.1f;
    const float zFar  = 500.0f;

    // ---- (A) 指数分割の往復 ----
    {
        for (u32 k = 0; k < cluster::kGridZ; ++k)
        {
            const float z0 = cluster::SliceNearZ(k, zNear, zFar);
            // スライスの内側（境界のちょうど上は丸め次第なので少し内側を突く）
            const float z1 = cluster::SliceNearZ(k + 1, zNear, zFar);
            const float mid = std::sqrt(z0 * z1);   // 対数空間の中点
            Check(cluster::SliceFromViewZ(mid, zNear, zFar) == k,
                  "SliceFromViewZ(対数中点) == k");
        }
        // 境界そのもの: zSlice(k) は slice k に入る（floor なので下側の境界は自スライス）
        for (u32 k = 0; k < cluster::kGridZ; ++k)
        {
            const float z = cluster::SliceNearZ(k, zNear, zFar);
            Check(cluster::SliceFromViewZ(z, zNear, zFar) == k, "SliceFromViewZ(スライス手前端) == k");
        }
        // 先頭と末尾
        Check(feq(cluster::SliceNearZ(0, zNear, zFar), zNear), "スライス0の手前端 == zNear");
        Check(feq(cluster::SliceNearZ(cluster::kGridZ, zNear, zFar), zFar), "最終スライスの奥端 == zFar");
    }

    // ---- (B) クランプ ----
    {
        Check(cluster::SliceFromViewZ(0.0f, zNear, zFar) == 0, "viewZ=0 は slice 0");
        Check(cluster::SliceFromViewZ(-1000.0f, zNear, zFar) == 0, "カメラ背面は slice 0");
        Check(cluster::SliceFromViewZ(zNear * 0.001f, zNear, zFar) == 0, "zNear より手前は slice 0");
        Check(cluster::SliceFromViewZ(zFar, zNear, zFar) == cluster::kGridZ - 1,
              "viewZ==zFar は最終スライス");
        Check(cluster::SliceFromViewZ(1.0e9f, zNear, zFar) == cluster::kGridZ - 1,
              "zFar より奥は最終スライスへクランプ");
        // scale/bias の符号（DOOM 2016 の式: scale>0, bias は zNear<1 のとき正）
        Check(cluster::SliceScale(zNear, zFar) > 0.0f, "sliceScale > 0");
        Check(feq(std::log2(zNear) * cluster::SliceScale(zNear, zFar)
                      + cluster::SliceBias(zNear, zFar), 0.0f, 1e-3f),
              "log2(zNear)*scale + bias == 0");
    }

    // ---- (C1) 球 vs AABB ----
    {
        const float bmin[3] = {-1.0f, -1.0f, 2.0f};
        const float bmax[3] = { 1.0f,  1.0f, 4.0f};

        const float inside[3] = {0.0f, 0.0f, 3.0f};
        Check(cluster::SphereVsAabb(inside, 0.01f, bmin, bmax), "AABB 内部の点光源は当たる");

        const float near1[3] = {2.0f, 0.0f, 3.0f};   // 面から 1.0 離れている
        Check(cluster::SphereVsAabb(near1, 1.5f, bmin, bmax), "range 1.5 は届く");
        Check(!cluster::SphereVsAabb(near1, 0.5f, bmin, bmax), "range 0.5 は届かない");

        // 角からの距離 = sqrt(3)。半径 1.7 では届かず 1.8 なら届く
        const float corner[3] = {2.0f, 2.0f, 5.0f};
        Check(!cluster::SphereVsAabb(corner, 1.7f, bmin, bmax), "角: range 1.7 は届かない");
        Check(cluster::SphereVsAabb(corner, 1.8f, bmin, bmax), "角: range 1.8 は届く");
    }

    // ---- (C2) 円錐 vs 球（Bart Wronski） ----
    {
        // 原点から +Z を向く半角 30 度・高さ 10 の円錐
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float fwd[3]    = {0.0f, 0.0f, 1.0f};
        const float half      = 30.0f * 3.14159265358979f / 180.0f;
        const float cosH = std::cos(half), sinH = std::sin(half);

        const float onAxis[3] = {0.0f, 0.0f, 5.0f};
        Check(cluster::ConeVsSphere(origin, fwd, 10.0f, cosH, sinH, onAxis, 0.1f),
              "軸上の球は当たる");

        // 軸から 45 度ずれた位置（円錐の外）。小さい球なら外れる
        const float side[3] = {5.0f, 0.0f, 5.0f};
        Check(!cluster::ConeVsSphere(origin, fwd, 10.0f, cosH, sinH, side, 0.1f),
              "円錐の外の小球は外れる");
        // 同じ位置でも球が大きければ円錐に触れる（保守的に true）
        Check(cluster::ConeVsSphere(origin, fwd, 10.0f, cosH, sinH, side, 4.0f),
              "大きい球は円錐に触れる");

        // 円錐の背面
        const float behind[3] = {0.0f, 0.0f, -5.0f};
        Check(!cluster::ConeVsSphere(origin, fwd, 10.0f, cosH, sinH, behind, 0.1f),
              "背面の球は外れる");
        // 高さ(range)より遠い
        const float tooFar[3] = {0.0f, 0.0f, 20.0f};
        Check(!cluster::ConeVsSphere(origin, fwd, 10.0f, cosH, sinH, tooFar, 0.1f),
              "range より遠い球は外れる");
    }

    // ---- グリッド定数の自明な整合（HLSL 側と手で揃える対象）----
    {
        Check(cluster::kClusterCount == cluster::kGridX * cluster::kGridY * cluster::kGridZ,
              "kClusterCount == gridX*gridY*gridZ");
        Check(cluster::kClusterCount == 3456u, "クラスタ数は 3456");
        Check(cluster::LinearIndex(0, 0, 0) == 0u, "LinearIndex(0,0,0) == 0");
        Check(cluster::LinearIndex(cluster::kGridX - 1, cluster::kGridY - 1, cluster::kGridZ - 1)
                  == cluster::kClusterCount - 1,
              "LinearIndex(最終) == kClusterCount-1");
        // 全クラスタが一意な index を持つ
        bool unique = true;
        for (u32 z = 0; z < cluster::kGridZ && unique; ++z)
            for (u32 y = 0; y < cluster::kGridY && unique; ++y)
                for (u32 x = 0; x < cluster::kGridX && unique; ++x)
                    if (cluster::LinearIndex(x, y, z) >= cluster::kClusterCount) unique = false;
        Check(unique, "LinearIndex は常に範囲内");
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
