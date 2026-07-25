// 精密ピッキングの土台（editor/RayGeometry.h）の単体テスト。
// ヘッダオンリー・純 DirectXMath（GPU も entt も ImGui も要らない）。実行: ctest --output-on-failure
//
// ここで守りたい退行は 3 つ:
//   1) レイ-三角形（Möller–Trumbore）が「当たる/外れる/裏面も当たる/平行は外れる」を正しく返すこと。
//      これが崩れると密集シーンで手前の別モデルが選ばれる（元の不具合そのもの）。
//   2) dir を正規化せずに渡すと t がスケールされる ＝ ローカル空間へ逆変換したレイの t が
//      ワールドの t とそのまま一致する、という前提。ナローフェーズの距離比較の根拠。
//   3) ヒット列のソートが t 昇順で安定であること。「AABB の t 順 ≠ 三角形ヒットの t 順」なので、
//      三角形ヒットを集めてから並べ直さないと奥のものが選ばれる。

#include "editor/RayGeometry.h"

#include <DirectXMath.h>
#include <cstdio>
#include <vector>

using namespace DirectX;
using namespace dx12e::raygeo;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

namespace
{
    // ScenePickHit と同じ形（distance を持つ）のダミー。SortHitsByDistance はこの形だけを要求する。
    struct Hit
    {
        int   id       = 0;
        float distance = 0.0f;
    };
}

int main()
{
    const XMVECTOR orig = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR dirZ = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    // z=5 に置いた、原点正面をカバーする三角形（(-1,-1) (3,-1) (-1,3)）。
    const XMVECTOR v0 = XMVectorSet(-1.0f, -1.0f, 5.0f, 1.0f);
    const XMVECTOR v1 = XMVectorSet( 3.0f, -1.0f, 5.0f, 1.0f);
    const XMVECTOR v2 = XMVectorSet(-1.0f,  3.0f, 5.0f, 1.0f);

    // ---- 1) レイ-三角形 ----
    // 正面ヒット: t = 5
    CHECK_NEAR(RayTriangle(orig, dirZ, v0, v1, v2), 5.0f, 1e-4f);

    // 巻き順を逆にしても当たる（両面判定。裏から見たメッシュも掴めることの担保）
    CHECK_NEAR(RayTriangle(orig, dirZ, v0, v2, v1), 5.0f, 1e-4f);

    // 三角形の外（+X へ大きく外れる方向）は外れる
    CHECK(RayTriangle(orig, XMVectorSet(1.0f, 0.0f, 0.1f, 0.0f), v0, v1, v2) < 0.0f);

    // 背後（-Z 方向）は外れる（t <= 0 は不採用）
    CHECK(RayTriangle(orig, XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), v0, v1, v2) < 0.0f);

    // 三角形と平行なレイは外れる
    CHECK(RayTriangle(orig, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), v0, v1, v2) < 0.0f);

    // 頂点から少し外へ出た点（u+v > 1 側）は外れる
    CHECK(RayTriangle(orig, XMVector3Normalize(XMVectorSet(2.0f, 2.0f, 5.0f, 0.0f)),
                      v0, v1, v2) < 0.0f);

    // ---- 2) dir を正規化しないと t がその比率でスケールされる ----
    // ＝「ワールドのレイをメッシュのローカル空間へ落として（方向は正規化しない）投げれば、
    //     返る t はワールドの t と一致する」というナローフェーズの前提そのもの。
    // 方向を 2 倍にすると同じ交点に対する t は半分になる。
    CHECK_NEAR(RayTriangle(orig, XMVectorSet(0.0f, 0.0f, 2.0f, 0.0f), v0, v1, v2), 2.5f, 1e-4f);

    // ---- 3) レイ-AABB（ナローフェーズの前段）----
    const XMFLOAT3 o{0.0f, 0.0f, 0.0f}, d{0.0f, 0.0f, 1.0f};
    CHECK_NEAR(RayAabb(o, d, XMFLOAT3(-1.0f, -1.0f, 4.0f), XMFLOAT3(1.0f, 1.0f, 6.0f)), 4.0f, 1e-4f);
    // 原点が箱の内側なら出口 t を返す（掴んだ物の中にカメラが入っていても拾える）
    CHECK_NEAR(RayAabb(o, d, XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 1.0f, 2.0f)), 2.0f, 1e-4f);
    // min/max が入れ替わっていても同じ結果（負スケールをローカルへ持ち込んだ時の保険）
    CHECK_NEAR(RayAabb(o, d, XMFLOAT3(1.0f, 1.0f, 6.0f), XMFLOAT3(-1.0f, -1.0f, 4.0f)), 4.0f, 1e-4f);
    // 外れ
    CHECK(RayAabb(o, d, XMFLOAT3(5.0f, 5.0f, 4.0f), XMFLOAT3(6.0f, 6.0f, 6.0f)) < 0.0f);
    // 背後だけの箱は外れ
    CHECK(RayAabb(o, d, XMFLOAT3(-1.0f, -1.0f, -6.0f), XMFLOAT3(1.0f, 1.0f, -4.0f)) < 0.0f);

    // ---- 4) レイ-球（ブロードフェーズ）----
    CHECK_NEAR(RaySphere(o, d, XMFLOAT3(0.0f, 0.0f, 5.0f), 1.0f), 4.0f, 1e-4f);
    CHECK(RaySphere(o, d, XMFLOAT3(0.0f, 0.0f, 5.0f), 0.0f) > 0.0f);      // 半径 0 でも中心を通れば当たる
    CHECK(RaySphere(o, d, XMFLOAT3(0.0f, 0.0f, 5.0f), 2.0f) >= 0.0f);
    const float insideT = RaySphere(o, d, XMFLOAT3(0.0f, 0.0f, 0.0f), 1.0f);
    CHECK(insideT >= 0.0f && insideT <= 1e-6f);                   // 原点が内側 → t=0
    CHECK(RaySphere(o, d, XMFLOAT3(0.0f, 0.0f, -5.0f), 1.0f) < 0.0f);     // 完全に後方
    CHECK(RaySphere(o, d, XMFLOAT3(10.0f, 0.0f, 5.0f), 1.0f) < 0.0f);     // 横に外れ

    // ---- 5) ヒット順ソート ----
    // 「AABB の t 順（= ブロードフェーズで候補を並べた順）と、実際の三角形ヒットの t 順は
    //   一致しない」を再現した並び。大きな箱に囲まれた奥のオブジェクトの AABB が
    //   手前に来ていても、三角形の t で並べ直せば正しい手前が先頭に来ること。
    std::vector<Hit> hits = {
        {10, 7.5f},   // 大きな箱（AABB は手前だが実体は奥）
        {20, 3.0f},   // 実際に一番手前
        {30, 5.25f},
    };
    SortHitsByDistance(hits);
    CHECK(hits.size() == 3u);
    CHECK(hits[0].id == 20);
    CHECK(hits[1].id == 30);
    CHECK(hits[2].id == 10);
    CHECK(hits[0].distance <= hits[1].distance);
    CHECK(hits[1].distance <= hits[2].distance);

    // 同距離は入力順を保つ（stable）。循環選択の順番が毎クリック入れ替わらないための保証。
    std::vector<Hit> tied = {{1, 2.0f}, {2, 2.0f}, {3, 1.0f}, {4, 2.0f}};
    SortHitsByDistance(tied);
    CHECK(tied[0].id == 3);
    CHECK(tied[1].id == 1);
    CHECK(tied[2].id == 2);
    CHECK(tied[3].id == 4);

    // 空でも落ちない
    std::vector<Hit> empty;
    SortHitsByDistance(empty);
    CHECK(empty.empty());

    std::printf("ray_pick: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
