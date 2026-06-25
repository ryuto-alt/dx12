// EventBus（汎用イベントバス）の単体テスト — Phase 3 中核。
//
// 即時/遅延発火・複数購読・解除・全消去・ペイロード取得・ハンドラ内 On 安全性 を検証する。
//
// 実行: ctest --output-on-failure

#include "engine/core/EventBus.h"

#include <cstdio>
#include <string>

#include <entt/entity/entity.hpp>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;
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
    EventBus bus;

    // 即時発火 + ペイロード取得
    int hits = 0;
    double lastDmg = 0.0;
    std::string lastWho;
    bus.On("hit", [&](const EngineEvent& e) {
        ++hits;
        lastDmg = e.num("dmg");
        lastWho = e.str("who");
    });

    {
        EngineEvent e;
        e.name = "hit";
        e.set("dmg", 12.5);
        e.set("who", std::string("enemy"));
        bus.Emit(e);
    }
    CHECK(hits == 1);
    CHECK(lastDmg == 12.5);
    CHECK(lastWho == "enemy");

    // 購読者の居ない name は無害
    {
        EngineEvent e; e.name = "nobody";
        bus.Emit(e);
        CHECK(hits == 1);
    }

    // 複数購読は両方呼ばれる
    int second = 0;
    std::uint32_t id2 = bus.On("hit", [&](const EngineEvent&) { ++second; });
    CHECK(bus.HandlerCount("hit") == 2);
    {
        EngineEvent e; e.name = "hit";
        bus.Emit(e);
    }
    CHECK(hits == 2);
    CHECK(second == 1);

    // Off で解除
    bus.Off(id2);
    CHECK(bus.HandlerCount("hit") == 1);
    {
        EngineEvent e; e.name = "hit";
        bus.Emit(e);
    }
    CHECK(hits == 3);
    CHECK(second == 1);   // 解除済みなので増えない

    // 遅延 Post → Flush
    int deferred = 0;
    bus.On("turn", [&](const EngineEvent& e) { deferred += static_cast<int>(e.num("n")); });
    {
        EngineEvent a; a.name = "turn"; a.set("n", 1.0); bus.Post(a);
        EngineEvent b; b.name = "turn"; b.set("n", 2.0); bus.Post(b);
    }
    CHECK(deferred == 0);   // Flush 前は発火しない
    bus.Flush();
    CHECK(deferred == 3);

    // ハンドラ内で On しても落ちない（コピー反復）
    int reentrant = 0;
    bus.On("chain", [&](const EngineEvent&) {
        ++reentrant;
        bus.On("chain", [&](const EngineEvent&) { ++reentrant; });
    });
    {
        EngineEvent e; e.name = "chain";
        bus.Emit(e);   // 既存1つだけ呼ばれる（追加分は次回から）
    }
    CHECK(reentrant == 1);

    // --- entity スコープ購読（接触イベント連携で使う）---
    // scope エンティティに束ねた購読は、OnEntityDestroyed で一括解除される。
    {
        EventBus sbus;
        const auto eA = static_cast<entt::entity>(100);
        const auto eB = static_cast<entt::entity>(200);

        int aHits = 0, bHits = 0, globalHits = 0;
        sbus.On("contact", eA, [&](const EngineEvent&) { ++aHits; });
        sbus.On("contact", eB, [&](const EngineEvent&) { ++bHits; });
        std::uint32_t gid = sbus.On("contact", [&](const EngineEvent&) { ++globalHits; });

        // HandlerCount は通常 + scoped の合算
        CHECK(sbus.HandlerCount("contact") == 3);

        // Emit は通常 + scoped の両方を呼ぶ
        { EngineEvent e; e.name = "contact"; sbus.Emit(e); }
        CHECK(aHits == 1);
        CHECK(bHits == 1);
        CHECK(globalHits == 1);

        // eA を破棄 → eA の scoped 購読だけ消える
        sbus.OnEntityDestroyed(eA);
        CHECK(sbus.HandlerCount("contact") == 2);
        { EngineEvent e; e.name = "contact"; sbus.Emit(e); }
        CHECK(aHits == 1);   // 解除済み
        CHECK(bHits == 2);
        CHECK(globalHits == 2);

        // 通常購読も Off で消える（scoped と独立）
        sbus.Off(gid);
        CHECK(sbus.HandlerCount("contact") == 1);
        { EngineEvent e; e.name = "contact"; sbus.Emit(e); }
        CHECK(bHits == 3);
        CHECK(globalHits == 2);

        // Post→Flush は scoped にも届く
        { EngineEvent e; e.name = "contact"; sbus.Post(e); }
        CHECK(bHits == 3);   // Flush 前
        sbus.Flush();
        CHECK(bHits == 4);

        // Clear は scoped も全消去
        sbus.Clear();
        CHECK(sbus.HandlerCount("contact") == 0);
    }

    // Clear で全消去
    bus.Clear();
    CHECK(bus.HandlerCount("hit") == 0);
    CHECK(bus.HandlerCount("turn") == 0);

    std::printf("event_bus: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
