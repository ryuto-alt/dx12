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

    // Bindings() の列挙。設定ファイルへの保存と設定画面の一覧表示がこれに依存する。
    // ここが壊れると「保存したのに次回起動で戻っていない」になるが、保存経路は
    // Application 側なのでユニットテストからは見えない。最低限ここで守る。
    {
        ActionMap m2;
        m2.Bind("move", 'W', XMFLOAT3{0.0f, 0.0f, 1.0f});
        m2.Bind("move", 'S', XMFLOAT3{0.0f, 0.0f, -1.0f});
        m2.Bind("jump", 0x20);   // VK_SPACE

        const auto& all = m2.Bindings();
        CHECK(all.size() == 2);                       // move / jump
        auto itMove = all.find("move");
        CHECK(itMove != all.end());
        if (itMove != all.end())
        {
            CHECK(itMove->second.size() == 2);
            // Bind した順に並ぶ（保存 → 読み込みで順序が変わらないこと）
            CHECK(itMove->second[0].key == 'W');
            CHECK(feq(itMove->second[0].c.z, 1.0f));
            CHECK(itMove->second[1].key == 'S');
            CHECK(feq(itMove->second[1].c.z, -1.0f));
        }
        auto itJump = all.find("jump");
        CHECK(itJump != all.end());
        // キーだけの Bind は寄与 (1,0,0)
        if (itJump != all.end() && itJump->second.size() == 1)
            CHECK(feq(itJump->second[0].c.x, 1.0f));
    }

    // ---- 同じ (action, key) を積み増さない ------------------------------------
    // m_actionMap はアプリ寿命なのに、Play / Stop / ランタイムのシーン切替のたびに
    // スクリプトが読み直されて bind が再実行される。重複を許すと寄与が合算され、
    // ★2 回目の Play で移動速度が 2 倍、3 回目で 3 倍になる（エラーもログも無し）。
    {
        ActionMap m;
        m.Bind("move", 'W', {0.0f, 0.0f, 1.0f});
        m.Bind("move", 'W', {0.0f, 0.0f, 1.0f});   // 同じ割り当てを 2 回
        m.Bind("move", 'W', {0.0f, 0.0f, 1.0f});   // 3 回

        const auto& all2 = m.Bindings();
        auto it = all2.find("move");
        CHECK(it != all2.end());
        if (it != all2.end())
        {
            CHECK(it->second.size() == 1);          // 積み増さない
            CHECK(it->second[0].key == 'W');
        }
        // 押されている扱いにして合算値を見る（ここが 3.0 になると 3 倍速）
        CHECK(feq(m.Evaluate("move", [](int) { return true; }).z, 1.0f));

        // 同じキーへ別の寄与を割り当て直したら「上書き」（重複ではない）
        m.Bind("move", 'W', {0.0f, 0.0f, 2.0f});
        CHECK(all2.find("move")->second.size() == 1);
        CHECK(feq(m.Evaluate("move", [](int) { return true; }).z, 2.0f));

        // 別キーはちゃんと増える
        m.Bind("move", 'S', {0.0f, 0.0f, -1.0f});
        CHECK(all2.find("move")->second.size() == 2);

        // HasAction（「既定値は上書きしない」判定用）
        CHECK(m.HasAction("move"));
        CHECK(!m.HasAction("fire"));
        m.ClearAction("move");
        CHECK(!m.HasAction("move"));
    }

    std::printf("action_map: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
