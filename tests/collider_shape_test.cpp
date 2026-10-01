// 当たり判定の「実効サイズ」の規約を固定するテスト（src/physics/ColliderShape.h）。
//
// なに:
//   コライダー/トリガーの大きさは、コンポーネントに書いた値そのままではなく
//   Transform のワールドスケールを掛けたものが実際の判定になる。掛け方は形ごとに違う。
//   その規則を数値で固定する。
//
// なぜ:
//   この規則は【判定を作る側】(PhysicsSystem / ScriptEngine) と
//   【線で描く側】(PhysicsDebugRenderer) の両方が使う。片方だけ変えると
//   「見えている線と実際に当たる場所が違う」デバッグ表示になり、
//   しかも症状は目で見ても分からない（線は出ているので壊れて見えない）。
//   実際、この共通化の前は描画側がスケールもオフセットも無視していて、
//   拡大した床の判定が線と一致していなかった。
//
//   ヘッダオンリーの純粋関数なので、GPU も Jolt も entt も要らない。
//
// 実行: ctest --output-on-failure （失敗があれば終了コード 1）

#include "physics/ColliderShape.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b)
{
    return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(a) + std::fabs(b));
}
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_F(a, b) CHECK(feq((a), (b)))

int main()
{
    using DirectX::XMFLOAT3;

    const XMFLOAT3 one { 1.0f, 1.0f, 1.0f };
    const XMFLOAT3 anis{ 2.0f, 3.0f, 4.0f };   // 非等方スケール

    // ---- 箱: 成分ごとに掛ける ----
    {
        const XMFLOAT3 he = collider::BoxHalfExtents({0.5f, 1.0f, 2.0f}, anis);
        CHECK_F(he.x, 1.0f);    // 0.5 * 2
        CHECK_F(he.y, 3.0f);    // 1.0 * 3
        CHECK_F(he.z, 8.0f);    // 2.0 * 4
    }
    // スケール 1 は素通し（親なしの大多数がここを通る）
    {
        const XMFLOAT3 he = collider::BoxHalfExtents({0.5f, 0.5f, 0.5f}, one);
        CHECK_F(he.x, 0.5f); CHECK_F(he.y, 0.5f); CHECK_F(he.z, 0.5f);
    }

    // ---- 球: 最大成分だけを使う（潰れた球は作れないので大きい方へ倒す）----
    CHECK_F(collider::SphereRadius(1.0f, anis), 4.0f);
    CHECK_F(collider::SphereRadius(2.0f, one),  2.0f);
    CHECK_F(collider::SphereRadius(1.0f, XMFLOAT3{5.0f, 1.0f, 1.0f}), 5.0f);
    CHECK_F(collider::SphereRadius(1.0f, XMFLOAT3{1.0f, 1.0f, 7.0f}), 7.0f);

    // ---- カプセル: 半径は XZ の最大 / 高さは Y（軸は Y 固定）----
    CHECK_F(collider::CapsuleRadius(0.5f, anis), 2.0f);       // 0.5 * max(2, 4)
    CHECK_F(collider::CapsuleHalfHeight(1.0f, anis), 3.0f);   // 1.0 * 3
    // ★Y をいくら伸ばしても半径は太らない。ここを max(x,y,z) にすると
    //   背を伸ばしたキャラが横にも太って壁にめり込む。
    CHECK_F(collider::CapsuleRadius(1.0f, XMFLOAT3{1.0f, 10.0f, 1.0f}), 1.0f);

    // ---- コライダー部品が無いとき: スケールそのものが箱になる ----
    {
        const XMFLOAT3 he = collider::FallbackHalfExtents(anis);
        CHECK_F(he.x, 1.0f); CHECK_F(he.y, 1.5f); CHECK_F(he.z, 2.0f);
    }

    // ---- トリガーはコライダーと同じ規約（別々に育てない）----
    {
        const XMFLOAT3 a = collider::TriggerBoxHalfExtents({1.5f, 1.5f, 0.5f}, anis);
        const XMFLOAT3 b = collider::BoxHalfExtents({1.5f, 1.5f, 0.5f}, anis);
        CHECK_F(a.x, b.x); CHECK_F(a.y, b.y); CHECK_F(a.z, b.z);
        CHECK_F(collider::TriggerSphereRadius(2.0f, anis), collider::SphereRadius(2.0f, anis));
    }

    // ---- 0 スケールでも落ちない（分解不能な Transform の受け皿）----
    {
        const XMFLOAT3 zero{0.0f, 0.0f, 0.0f};
        const XMFLOAT3 he = collider::BoxHalfExtents({1.0f, 1.0f, 1.0f}, zero);
        CHECK_F(he.x, 0.0f);
        CHECK_F(collider::SphereRadius(1.0f, zero), 0.0f);
    }

    // ---- 凸包の頂点の間引き: 極値（外形）を残す ----
    {
        std::vector<XMFLOAT3> pts;
        for (int i = 0; i < 5000; ++i)
        {
            const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 5000.0f;
            const float r = std::sqrt(1.0f - y * y), phi = static_cast<float>(i) * 2.3999632f;
            pts.push_back({ std::cos(phi) * r * 1.5f, y * 1.5f, std::sin(phi) * r * 1.5f });
        }
        pts.push_back({ 0.1f, 3.0f, 0.2f });     // 末尾の 1 点だけが遠い（step おきの間引きだと落ちる）
        const std::vector<XMFLOAT3> red = collider::ReduceHullPoints(pts, 256);
        CHECK(red.size() <= 256 && red.size() >= 128);
        float maxY = -1e9f, minY = 1e9f, maxX = -1e9f;
        for (const auto& p : red) { maxY = (std::max)(maxY, p.y); minY = (std::min)(minY, p.y); maxX = (std::max)(maxX, p.x); }
        CHECK_F(maxY, 3.0f);                     // 遠い 1 点は残る
        CHECK(minY < -1.45f);                    // 反対側の極値も外形を保つ（球の底）
        CHECK(maxX > 1.4f);
        // 決定論・256 以下はそのまま
        CHECK(collider::ReduceHullPoints(pts, 256).size() == red.size());
        std::vector<XMFLOAT3> small(pts.begin(), pts.begin() + 100);
        CHECK(collider::ReduceHullPoints(small, 256).size() == 100);
    }

    // ---- 物理が使う「安全な」サイズ（負・0・NaN）----
    {
        const float nan = std::nanf("");
        CHECK_F(collider::SafeSize(-2.0f), 2.0f);
        CHECK_F(collider::SafeSize(0.0f), collider::kMinColliderSize);
        CHECK_F(collider::SafeSize(nan), collider::kMinColliderSize);
        CHECK_F(collider::EffectiveCapsuleHalfHeight(1.0f, XMFLOAT3{1.0f, -3.0f, 1.0f}), 3.0f);
        CHECK_F(collider::EffectiveCapsuleRadius(0.5f, XMFLOAT3{-4.0f, 1.0f, 2.0f}), 2.0f);
        // BoxShape の丸め半径は最小ハーフサイズ以下（薄い板で Jolt の前提 half >= radius を守る）
        CHECK_F(collider::BoxConvexRadius(XMFLOAT3{0.5f, 0.01f, 0.5f}), 0.01f);
        CHECK_F(collider::BoxConvexRadius(XMFLOAT3{0.5f, 0.5f, 0.5f}), 0.05f);
        // offset はスケールを掛ける（符号つき）
        const XMFLOAT3 so = collider::ScaledOffset({2.0f, 1.0f, 0.0f}, {-2.0f, 3.0f, 5.0f});
        CHECK_F(so.x, -4.0f); CHECK_F(so.y, 3.0f); CHECK_F(so.z, 0.0f);
        const XMFLOAT3 ms = collider::SafeMeshScale({0.0f, -1.0f, nan});
        CHECK_F(ms.x, collider::kMinColliderSize); CHECK_F(ms.y, -1.0f); CHECK_F(ms.z, collider::kMinColliderSize);
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
