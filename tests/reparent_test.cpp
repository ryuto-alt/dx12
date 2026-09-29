// ヒエラルキーの親替えで「ワールド変換が保たれる」ことの単体テスト（editor/TransformMath.h）。
//
// ★何を守っているか
//   以前は parent だけ差し替えていたので、ローカル値に新しい親の変換が掛かり直って
//   見た目の位置・向き・大きさが飛んだ（UE / Unity は既定でワールドを保つ）。
//   ここでは「付け替えの前後でワールド行列が一致する」を、親が回転・拡大されている難しい場合まで含めて確かめる。
//   ECS（entt + Transform）と DirectXMath だけで動く（GPU も ImGui も不要）。
#include "editor/TransformMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_checks = 0;
int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

entt::entity Make(entt::registry& reg, XMFLOAT3 pos, XMFLOAT3 rotDeg = {0, 0, 0}, XMFLOAT3 scale = {1, 1, 1},
                  entt::entity parent = entt::null)
{
    const auto e = reg.create();
    Transform t;
    t.position = pos; t.rotation = rotDeg; t.scale = scale; t.parent = parent;
    reg.emplace<Transform>(e, t);
    return e;
}

// 2 つのワールド行列がほぼ同じか（成分の最大差）
float MaxDiff(const XMMATRIX& a, const XMMATRIX& b)
{
    XMFLOAT4X4 fa, fb;
    XMStoreFloat4x4(&fa, a); XMStoreFloat4x4(&fb, b);
    float m = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            m = (std::max)(m, std::fabs(fa.m[i][j] - fb.m[i][j]));
    return m;
}

void ExpectKeepsWorld(entt::registry& reg, entt::entity child, entt::entity newParent, const char* label)
{
    const XMMATRIX before = ComputeWorldMatrix(reg, child);
    const bool ok = xform::Reparent(reg, child, newParent, /*keepWorld=*/true);
    const XMMATRIX after = ComputeWorldMatrix(reg, child);
    const float d = MaxDiff(before, after);
    CHECK(ok, "%s: 付け替えが拒否された", label);
    CHECK(d < 1e-3f, "%s: ワールドが変わった (最大差 %.5f)", label, d);
    CHECK(reg.get<Transform>(child).parent == newParent, "%s: 親が設定されていない", label);
}
} // namespace

int main()
{
    // 1) 単純な平行移動の親
    {
        entt::registry reg;
        const auto parent = Make(reg, {10, 0, 0});
        const auto child  = Make(reg, {1, 2, 3});
        ExpectKeepsWorld(reg, child, parent, "平行移動の親");
        const auto& t = reg.get<Transform>(child);
        CHECK(std::fabs(t.position.x - (-9.0f)) < 1e-4f && std::fabs(t.position.y - 2.0f) < 1e-4f,
              "ローカルは (-9,2,3) になるはず (%.2f,%.2f,%.2f)", t.position.x, t.position.y, t.position.z);
    }
    // 2) 親が回転・拡大されている
    {
        entt::registry reg;
        const auto parent = Make(reg, {5, 1, -2}, {10, 40, 20}, {2, 2, 2});
        const auto child  = Make(reg, {3, 0, 1}, {0, 30, 0}, {1, 1, 1});
        ExpectKeepsWorld(reg, child, parent, "回転+拡大の親");
    }
    // 3) 親の親がいる深い階層
    {
        entt::registry reg;
        const auto gp = Make(reg, {0, 3, 0}, {0, 90, 0}, {1, 1, 1});
        const auto p  = Make(reg, {2, 0, 0}, {15, 0, 0}, {1.5f, 1.5f, 1.5f}, gp);
        const auto c  = Make(reg, {-4, 2, 6}, {5, 25, 70}, {1, 1, 1});
        ExpectKeepsWorld(reg, c, p, "深い階層");
    }
    // 4) 親を外してルートへ（親のワールド変換を子へ焼き込む）
    {
        entt::registry reg;
        const auto parent = Make(reg, {7, 0, 0}, {0, 45, 0}, {2, 2, 2});
        const auto child  = Make(reg, {1, 1, 1}, {0, 0, 0}, {1, 1, 1}, parent);
        ExpectKeepsWorld(reg, child, entt::null, "ルートへ");
        const auto& t = reg.get<Transform>(child);
        CHECK(std::fabs(t.scale.x - 2.0f) < 1e-3f, "親の拡大が子のローカルへ入る (%.3f)", t.scale.x);
    }
    // 5) 親を別の親へ替える（兄弟間の移動）
    {
        entt::registry reg;
        const auto a = Make(reg, {10, 0, 0}, {0, 20, 0}, {1, 1, 1});
        const auto b = Make(reg, {-6, 4, 2}, {30, 0, 0}, {3, 3, 3});
        const auto c = Make(reg, {1, 2, 3}, {0, 10, 0}, {1, 1, 1}, a);
        ExpectKeepsWorld(reg, c, b, "別の親へ");
    }
    // 6) 循環は拒否する（自分自身 / 自分の子孫の下へ）
    {
        entt::registry reg;
        const auto a = Make(reg, {0, 0, 0});
        const auto b = Make(reg, {1, 0, 0}, {0, 0, 0}, {1, 1, 1}, a);
        const auto c = Make(reg, {2, 0, 0}, {0, 0, 0}, {1, 1, 1}, b);
        CHECK(!xform::CanReparent(reg, a, a), "自分自身の子にはできない");
        CHECK(!xform::CanReparent(reg, a, c), "子孫の下へは入れられない");
        CHECK(!xform::Reparent(reg, a, c), "Reparent も拒否");
        CHECK(reg.get<Transform>(a).parent == entt::null, "拒否したら何も変わらない");
        CHECK(xform::CanReparent(reg, c, entt::null), "ルートへは出せる");
        CHECK(xform::CanReparent(reg, c, a), "祖先の直下へは付け替えられる");
    }
    // 7) Shift（keepWorld=false）は従来どおりローカル値そのまま
    {
        entt::registry reg;
        const auto parent = Make(reg, {10, 0, 0});
        const auto child  = Make(reg, {1, 2, 3});
        xform::Reparent(reg, child, parent, /*keepWorld=*/false);
        const auto& t = reg.get<Transform>(child);
        CHECK(t.position.x == 1.0f && t.position.y == 2.0f && t.position.z == 3.0f, "ローカル値は据え置き");
        XMFLOAT3 w; XMStoreFloat3(&w, ComputeWorldMatrix(reg, child).r[3]);
        CHECK(std::fabs(w.x - 11.0f) < 1e-4f, "見た目は親の分だけずれる (%.2f)", w.x);
    }
    // 8) クォータニオンで回転を持つ子（物理同期のエンティティなど）でもワールドが保たれる
    {
        entt::registry reg;
        const auto parent = Make(reg, {3, 0, 0}, {0, 60, 0}, {1, 1, 1});
        const auto child  = Make(reg, {1, 1, 1});
        auto& t = reg.get<Transform>(child);
        t.useQuaternion = true;
        XMStoreFloat4(&t.quaternion, XMQuaternionRotationRollPitchYaw(0.3f, 0.5f, 0.1f));
        ExpectKeepsWorld(reg, child, parent, "クォータニオン");
    }
    // 9) 同じ親への付け替えは何もしない
    {
        entt::registry reg;
        const auto parent = Make(reg, {1, 2, 3});
        const auto child  = Make(reg, {4, 5, 6}, {0, 0, 0}, {1, 1, 1}, parent);
        const Transform before = reg.get<Transform>(child);
        CHECK(xform::Reparent(reg, child, parent), "同じ親でも成功扱い");
        const Transform& after = reg.get<Transform>(child);
        CHECK(after.position.x == before.position.x && after.position.y == before.position.y, "値は変わらない");
    }
    // 10) 親のスケールが 0（逆行列が無い）でも落ちず、親は設定される
    {
        entt::registry reg;
        const auto parent = Make(reg, {0, 0, 0}, {0, 0, 0}, {0, 1, 1});
        const auto child  = Make(reg, {1, 2, 3});
        CHECK(xform::Reparent(reg, child, parent), "逆行列が無くても成功（ローカル据え置き）");
        CHECK(reg.get<Transform>(child).parent == parent, "親は設定される");
    }
    // 11) ランダムな親子で保存則を広く確認（決定的な疑似乱数）
    {
        unsigned s = 987654u;
        const auto rnd = [&s](float lo, float hi) { s = s * 1664525u + 1013904223u; return lo + ((s >> 8) / 16777216.0f) * (hi - lo); };
        for (int i = 0; i < 200; ++i)
        {
            entt::registry reg;
            const float sc = rnd(0.4f, 3.0f);
            const auto p = Make(reg, {rnd(-20, 20), rnd(-20, 20), rnd(-20, 20)},
                                {rnd(-60, 60), rnd(-170, 170), rnd(-60, 60)}, {sc, sc, sc});
            const auto c = Make(reg, {rnd(-5, 5), rnd(-5, 5), rnd(-5, 5)},
                                {rnd(-60, 60), rnd(-170, 170), rnd(-60, 60)}, {rnd(0.5f, 2), rnd(0.5f, 2), rnd(0.5f, 2)});
            const XMMATRIX before = ComputeWorldMatrix(reg, c);
            xform::Reparent(reg, c, p, true);
            const float d = MaxDiff(before, ComputeWorldMatrix(reg, c));
            CHECK(d < 5e-3f, "ランダム #%d: 最大差 %.5f", i, d);
        }
    }

    if (g_failures != 0)
    {
        std::printf("reparent: %d checks, %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    std::printf("reparent: %d checks, all passed\n", g_checks);
    return 0;
}
