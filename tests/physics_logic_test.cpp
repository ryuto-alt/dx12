// 物理の「判断だけ」の純粋関数（src/physics/PhysicsLogic.h）を固定するテスト。
// 補間の係数/テレポート判定/クォータニオン補間、キャラと動的剛体の押し合いの規約。
// Jolt も entt も GPU も要らない。
//
// 実行: ctest --output-on-failure

#include "physics/PhysicsLogic.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;
bool feq(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b)); }
}

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
    const float step = 1.0f / 60.0f;

    // ---- 補間係数 ----
    CHECK(feq(physlogic::InterpAlpha(0.0f, step), 0.0f));
    CHECK(feq(physlogic::InterpAlpha(step * 0.5f, step), 0.5f));
    CHECK(feq(physlogic::InterpAlpha(step * 3.0f, step), 1.0f));   // 端数が溢れても 1 で止める
    CHECK(feq(physlogic::InterpAlpha(-1.0f, step), 0.0f));
    CHECK(feq(physlogic::InterpAlpha(0.01f, 0.0f), 1.0f));          // step<=0 は補間しない

    // ---- 144fps で描画しても、補間した位置は毎フレーム等間隔に進む ----
    // 物理: 60Hz で x が 1 ステップ 1.0 進む(60 m/s)。描画 144fps の x を並べて 2 階差分を見る。
    {
        const float dt = 1.0f / 144.0f;
        float acc = 0.0f, prev = 0.0f, cur = 0.0f;
        float last = -1.0f, lastStep = -1.0f;
        float maxD2 = 0.0f; int zeroMoves = 0, frames = 0;
        for (int i = 0; i < 600; ++i)
        {
            acc += dt;
            while (acc >= step) { prev = cur; cur += 1.0f; acc -= step; }
            const float a = physlogic::InterpAlpha(acc, step);
            float p[3]; const float A[3] = { prev, 0, 0 }, B[3] = { cur, 0, 0 };
            physlogic::LerpVec3(A, B, a, p);
            if (i > 10)
            {
                const float d = p[0] - last;
                if (std::fabs(d) < 1e-6f) ++zeroMoves;
                if (lastStep >= 0.0f) maxD2 = std::fmax(maxD2, std::fabs(d - lastStep));
                lastStep = d; ++frames;
            }
            last = p[0];
        }
        CHECK(zeroMoves == 0);          // 補間なしだと 58% が 0 移動
        CHECK(maxD2 < 0.05f);           // 1 フレームの移動量(~0.417)の揺らぎは 12% 未満
    }

    // ---- テレポート判定 ----
    CHECK(!physlogic::IsTeleport(1.0f, 0.0f, 0.0f));
    CHECK(!physlogic::IsTeleport(3.0f, 3.0f, 0.0f));               // 4.24m
    CHECK(physlogic::IsTeleport(6.0f, 0.0f, 0.0f));
    CHECK(physlogic::IsTeleport(0.0f, -20.0f, 0.0f));
    CHECK(physlogic::IsTeleport(2.0f, 2.0f, 2.0f, 3.0f));          // 3.46m > 3m

    // ---- クォータニオン補間 ----
    {
        const float a[4] = { 0, 0, 0, 1 };
        const float b[4] = { 0, std::sin(0.5f), 0, std::cos(0.5f) };   // y 軸 1rad
        float o[4];
        physlogic::NlerpQuat(a, b, 0.0f, o); CHECK(feq(o[3], 1.0f) && feq(o[1], 0.0f));
        physlogic::NlerpQuat(a, b, 1.0f, o); CHECK(feq(o[1], b[1]) && feq(o[3], b[3]));
        physlogic::NlerpQuat(a, b, 0.5f, o);
        CHECK(feq(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3], 1.0f));   // 正規化されている
        CHECK(o[1] > 0.0f && o[1] < b[1]);
        // 反対側の半球 (-b) でも最短経路（逆回りしない）
        const float nb[4] = { -b[0], -b[1], -b[2], -b[3] };
        float o2[4];
        physlogic::NlerpQuat(a, nb, 0.5f, o2);
        CHECK(feq(o2[1], o[1]) && feq(o2[3], o[3]));
    }

    // ---- 接地面速度の継承 ----
    CHECK(feq(physlogic::GroundVelocityScale(1.0f, 70.0f), 0.0f));     // 既定 1kg の箱: 継承しない
    CHECK(feq(physlogic::GroundVelocityScale(70.0f, 70.0f), 0.0f));
    CHECK(feq(physlogic::GroundVelocityScale(105.0f, 70.0f), 0.5f));
    CHECK(feq(physlogic::GroundVelocityScale(140.0f, 70.0f), 1.0f));   // 重い筏: 運ぶ
    CHECK(feq(physlogic::GroundVelocityScale(5000.0f, 70.0f), 1.0f));
    CHECK(feq(physlogic::GroundVelocityScale(1.0f, 0.0f), 1.0f));      // 質量 0 の異常値は従来どおり

    // ---- 押し返し ----
    CHECK(!physlogic::CanBodyPushCharacter(1.0f, 70.0f));
    CHECK(!physlogic::CanBodyPushCharacter(34.0f, 70.0f));
    CHECK(physlogic::CanBodyPushCharacter(35.0f, 70.0f));
    CHECK(physlogic::CanBodyPushCharacter(1000.0f, 70.0f));

    // ---- 体重（キャラの実効質量）----
    CHECK(feq(physlogic::StandingMass(70.0f, 14.0f, 1.0f), 15.0f / 14.0f));          // 既定 1kg の箱: 体重を頭打ち
    CHECK(feq(physlogic::StandingMass(70.0f, 14.0f, 10.0f), 150.0f / 14.0f));
    CHECK(feq(physlogic::StandingMass(70.0f, 14.0f, 1000.0f), 70.0f));               // 重い物: 体重そのまま
    CHECK(feq(physlogic::StandingMass(70.0f, 14.0f, 0.0f), 0.0f));
    CHECK(feq(physlogic::StandingMass(-5.0f, 14.0f, 10.0f), 0.0f));                  // 負の質量は 0
    CHECK(feq(physlogic::StandingMass(70.0f, 0.0f, 1.0f), 70.0f));                   // 重力 0 は触らない
    {
        // 頭打ち後の 1 ステップあたりの押し込み速度は 60Hz で 0.25m/s（Jolt の反発の最小速度 1m/s 未満）。
        // 既存シーン（1kg の箱 + 70kg のキャラ）は頭打ち前 16.3m/s だった。
        const float m = physlogic::StandingMass(70.0f, 14.0f, 1.0f);
        CHECK(m * 14.0f / 1.0f * step < 0.3f);
        CHECK(70.0f * 14.0f / 1.0f * step > 16.0f);
        // 65kg 以上の剛体なら体重そのまま
        CHECK(feq(physlogic::StandingMass(70.0f, 14.0f, 66.0f), 70.0f));
        CHECK(physlogic::StandingMass(70.0f, 14.0f, 60.0f) < 70.0f);
    }

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
