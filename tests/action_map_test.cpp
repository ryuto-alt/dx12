// ActionMap（データ駆動入力）の単体テスト — Phase 4 能力カタログ。
//
// キー状態の述語を注入して、移動ベクトル合算・ボタン判定・リバインド・解除を検証する。
// InputSystem / GPU 非依存。
//
// 実行: ctest --output-on-failure

#include "engine/input/ActionMap.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <set>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

// 仮想キーコード
enum { K_W = 1, K_S, K_A, K_D, K_SPACE };
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    ActionMap map;
    map.Bind("move", K_W, XMFLOAT3{0, 0, 1});
    map.Bind("move", K_S, XMFLOAT3{0, 0, -1});
    map.Bind("move", K_A, XMFLOAT3{-1, 0, 0});
    map.Bind("move", K_D, XMFLOAT3{1, 0, 0});
    map.Bind("jump", K_SPACE);
    CHECK(map.BindingCount("move") == 4);
    CHECK(map.BindingCount("jump") == 1);

    auto downSet = [](std::set<int> keys) {
        return [keys](int k) { return keys.count(k) > 0; };
    };

    // W + D → (1,0,1)
    {
        XMFLOAT3 v = map.Evaluate("move", downSet({K_W, K_D}));
        CHECK(feq(v.x, 1.0f) && feq(v.y, 0.0f) && feq(v.z, 1.0f));
    }
    // 押下なし → (0,0,0)
    {
        XMFLOAT3 v = map.Evaluate("move", downSet({}));
        CHECK(feq(v.x, 0.0f) && feq(v.z, 0.0f));
    }
    // 反対キー同時 → 相殺 (0,0,0)
    {
        XMFLOAT3 v = map.Evaluate("move", downSet({K_W, K_S, K_A, K_D}));
        CHECK(feq(v.x, 0.0f) && feq(v.z, 0.0f));
    }
    // Active（ボタン）
    CHECK(map.Active("jump", downSet({K_SPACE})));
    CHECK(!map.Active("jump", downSet({K_W})));
    CHECK(!map.Active("unknown", downSet({K_W})));

    // リバインド: move に矢印キー相当を足す（K_SPACE を前進にも割当）
    map.Bind("move", K_SPACE, XMFLOAT3{0, 0, 1});
    CHECK(map.BindingCount("move") == 5);
    {
        XMFLOAT3 v = map.Evaluate("move", downSet({K_SPACE}));
        CHECK(feq(v.z, 1.0f));
    }

    // 解除
    map.ClearAction("move");
    CHECK(map.BindingCount("move") == 0);
    {
        XMFLOAT3 v = map.Evaluate("move", downSet({K_W}));
        CHECK(feq(v.x, 0.0f) && feq(v.z, 0.0f));
    }
    map.Clear();
    CHECK(map.BindingCount("jump") == 0);

    std::printf("action_map: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
