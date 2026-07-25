// 2 ボーン解析 IK（animation/TwoBoneIK.h）のテスト。
//
// 出典: Daniel Holden "Simple Two Joint IK"（theorangeduck.com/page/simple-two-joint）
// を DirectXMath へ移植したもの。移植で一番壊しやすいのは
//   ・XMQuaternionMultiply の引数順が逆（Q1,Q2 で Q2*Q1 を返す）
//   ・quat_mul(quat_inv(q), axis) は「四元数×ベクトル」であって四元数積ではない
// の 2 点。どちらを間違えても脚が裏返るので、
// 「解いたあと FK し直すと足首が目標に乗る」ことを数値で確認して固定する。
//
// ヘッダオンリー・純 DirectXMath（GPU も entt も要らない）。
#include "animation/TwoBoneIK.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) < eps;
}

static bool Finite3(FXMVECTOR v)
{
    XMFLOAT3 f;
    XMStoreFloat3(&f, v);
    return std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z);
}

// ---------------------------------------------------------------------------
// 2 ボーンチェーンの簡易 FK。
//   a はワールド固定。ローカル回転 aLocal / bLocal から b と c を求める。
//   ボーンはローカル +Y 方向に伸びているものとする（長さ lab / lcb）。
// ---------------------------------------------------------------------------
struct Chain
{
    XMVECTOR a, b, c;
    XMVECTOR aGlobal, bGlobal;   // グローバル回転
    XMVECTOR aLocal, bLocal;     // ローカル回転
    float lab, lcb;
};

static void ForwardKinematics(Chain& ch, FXMVECTOR parentRot)
{
    ch.aGlobal = XMQuaternionNormalize(XMQuaternionMultiply(ch.aLocal, parentRot));
    ch.b = XMVectorAdd(ch.a, XMVectorScale(
        XMVector3Rotate(XMVectorSet(0, 1, 0, 0), ch.aGlobal), ch.lab));

    ch.bGlobal = XMQuaternionNormalize(XMQuaternionMultiply(ch.bLocal, ch.aGlobal));
    ch.c = XMVectorAdd(ch.b, XMVectorScale(
        XMVector3Rotate(XMVectorSet(0, 1, 0, 0), ch.bGlobal), ch.lcb));
}

// 少し曲げた初期姿勢のチェーンを作る（完全に伸び切っていると平面が決まらないため）
static Chain MakeChain(float lab = 1.0f, float lcb = 1.0f, float bend = 0.35f)
{
    Chain ch;
    ch.a    = XMVectorSet(0, 0, 0, 1);
    ch.lab  = lab;
    ch.lcb  = lcb;
    ch.aLocal = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), -bend);
    ch.bLocal = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0),  bend * 2.0f);
    ForwardKinematics(ch, XMQuaternionIdentity());
    return ch;
}

static void SolveAndRefresh(Chain& ch, FXMVECTOR target, FXMVECTOR hint)
{
    // 解く前のグローバル回転を渡すのが原文の仕様（元ポーズ 1 回ぶんで計算する）
    SolveTwoBoneIK(ch.a, ch.b, ch.c, target, hint, ch.aGlobal, ch.bGlobal,
                   ch.aLocal, ch.bLocal);
    ForwardKinematics(ch, XMQuaternionIdentity());
}

// ★本命★ 到達可能な目標なら、解いたあと足首が目標に乗る
static void TestReachableTarget()
{
    const XMVECTOR hint = XMVectorSet(0, 0, 1, 0);

    const XMVECTOR targets[] = {
        XMVectorSet( 0.0f,  1.5f,  0.4f, 1),
        XMVectorSet( 0.7f,  1.2f, -0.5f, 1),
        XMVectorSet(-0.9f,  0.8f,  0.6f, 1),
        XMVectorSet( 0.2f,  0.5f,  0.9f, 1),
        XMVectorSet( 1.2f,  1.1f,  0.1f, 1),
    };

    float worst = 0.0f;
    for (const XMVECTOR& t : targets)
    {
        Chain ch = MakeChain();
        SolveAndRefresh(ch, t, hint);

        const float err = XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, t)));
        worst = (std::max)(worst, err);
        CHECK(err < 1e-4f);

        // ボーン長が保存されている
        CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), ch.lab, 1e-5f));
        CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.b))), ch.lcb, 1e-5f));
    }
    std::printf("  [IK] worst reachable-target error = %.9f\n", worst);
}

// 長さの違う 2 ボーンでも解ける
static void TestUnequalBoneLengths()
{
    const XMVECTOR hint = XMVectorSet(0, 0, 1, 0);
    Chain ch = MakeChain(1.4f, 0.6f);
    const XMVECTOR t = XMVectorSet(0.5f, 1.3f, 0.3f, 1);
    SolveAndRefresh(ch, t, hint);

    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, t))) < 1e-4f);
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), 1.4f, 1e-5f));
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.b))), 0.6f, 1e-5f));
}

// 到達不能（遠すぎ）: 脚が伸び切り、a→c の向きが a→target の向きと一致する
static void TestUnreachableFar()
{
    const XMVECTOR hint = XMVectorSet(0, 0, 1, 0);
    Chain ch = MakeChain();
    const XMVECTOR t = XMVectorSet(0.0f, 8.0f, 3.0f, 1);   // |t-a| >> lab+lcb
    SolveAndRefresh(ch, t, hint);

    const XMVECTOR dirAC = XMVector3Normalize(XMVectorSubtract(ch.c, ch.a));
    const XMVECTOR dirAT = XMVector3Normalize(XMVectorSubtract(t, ch.a));
    const float dot = XMVectorGetX(XMVector3Dot(dirAC, dirAT));
    CHECK(dot > 1.0f - 1e-4f);

    // 伸び切っている（ほぼ lab + lcb）。eps ぶんだけ手前で止まる
    const float reach = XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.a)));
    CHECK(reach > ch.lab + ch.lcb - 0.05f);
    CHECK(reach <= ch.lab + ch.lcb + 1e-4f);

    // ボーン長は保存される
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), ch.lab, 1e-5f));
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.b))), ch.lcb, 1e-5f));
}

// 到達不能（近すぎ）: 長さの違う 2 ボーンは |lab - lcb| より内側へは畳めない。
// NaN を出さず、ボーン長を保ったまま最大限畳むこと。
static void TestUnreachableNear()
{
    const XMVECTOR hint = XMVectorSet(0, 0, 1, 0);
    Chain ch = MakeChain(1.5f, 0.5f);
    const XMVECTOR t = XMVectorSet(0.0f, 0.05f, 0.0f, 1);   // ほぼ a と同じ位置
    SolveAndRefresh(ch, t, hint);

    CHECK(Finite3(ch.b) && Finite3(ch.c));
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), 1.5f, 1e-5f));
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.b))), 0.5f, 1e-5f));

    // 内側の限界 |1.5 - 0.5| = 1.0 より近づけないので、そこで止まっている
    const float reach = XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, ch.a)));
    CHECK(reach > 0.9f);
}

// ★伸び切った脚★ではヒント（極ベクトル）が膝の出る側を決める。
// これが原文 Bonus 節の狙い（cross(c-a, b-a) が退化して膝がパタつくのを防ぐ）。
static void TestHintControlsKneeWhenStraight()
{
    const XMVECTOR t = XMVectorSet(0.0f, 1.6f, 0.0f, 1);

    Chain front = MakeChain(1.0f, 1.0f, 0.0f);   // bend=0 → 完全に伸び切った初期姿勢
    SolveAndRefresh(front, t, XMVectorSet(0, 0, 1, 0));
    Chain back = MakeChain(1.0f, 1.0f, 0.0f);
    SolveAndRefresh(back, t, XMVectorSet(0, 0, -1, 0));

    const float zFront = XMVectorGetZ(front.b);
    const float zBack  = XMVectorGetZ(back.b);

    std::printf("  [IK] straight limb: knee Z with hint +Z = %+.4f, with hint -Z = %+.4f\n",
                zFront, zBack);
    CHECK(std::fabs(zFront) > 0.05f);
    CHECK(std::fabs(zBack)  > 0.05f);
    CHECK(zFront * zBack < 0.0f);   // ヒントの符号で反対側を向く

    // どちらも足首は目標に乗る
    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(front.c, t))) < 1e-4f);
    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(back.c, t)))  < 1e-4f);
}

// ★既に曲がっている脚★は、ヒントの符号がどちらでも「今曲がっている側」を保つ。
// これをやらないと、アニメが決めた膝の向きがヒント次第で毎フレーム裏返る
// （フット IK は「接地の高さだけ直す」道具なので、膝の向きはアニメに従うのが正しい）。
static void TestBentLimbKeepsItsSide()
{
    const XMVECTOR t = XMVectorSet(0.0f, 1.5f, -0.3f, 1);

    Chain a = MakeChain();   // 既定の bend=0.35 で膝は -Z 側へ出ている
    SolveAndRefresh(a, t, XMVectorSet(0, 0, 1, 0));
    Chain b = MakeChain();
    SolveAndRefresh(b, t, XMVectorSet(0, 0, -1, 0));

    std::printf("  [IK] bent limb: knee Z with hint +Z = %+.4f, with hint -Z = %+.4f (same side)\n",
                XMVectorGetZ(a.b), XMVectorGetZ(b.b));

    // 同じ側（符号が一致する）
    CHECK(XMVectorGetZ(a.b) * XMVectorGetZ(b.b) > 0.0f);
    // どちらも目標に乗る
    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(a.c, t))) < 1e-4f);
    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(b.c, t))) < 1e-4f);
}

// 退化ケースで NaN / Inf を出さない
static void TestDegenerate()
{
    const XMVECTOR hint = XMVectorSet(0, 0, 1, 0);

    // 目標が股関節と同一点
    {
        Chain ch = MakeChain();
        SolveAndRefresh(ch, ch.a, hint);
        CHECK(Finite3(ch.b) && Finite3(ch.c));
    }
    // ボーン長 0
    {
        Chain ch = MakeChain(0.0f, 0.0f);
        SolveAndRefresh(ch, XMVectorSet(1, 1, 1, 1), hint);
        CHECK(Finite3(ch.b) && Finite3(ch.c));
    }
    // 完全に伸び切った初期姿勢 + ヒント無し（cross が退化する）
    {
        Chain ch = MakeChain(1.0f, 1.0f, 0.0f);
        SolveAndRefresh(ch, XMVectorSet(0.3f, 1.7f, 0.2f, 1), XMVectorZero());
        CHECK(Finite3(ch.b) && Finite3(ch.c));
        CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), 1.0f, 1e-5f));
    }
    // 完全に伸び切った初期姿勢 + ヒントあり → ちゃんと解ける
    {
        Chain ch = MakeChain(1.0f, 1.0f, 0.0f);
        const XMVECTOR t = XMVectorSet(0.3f, 1.7f, 0.2f, 1);
        SolveAndRefresh(ch, t, XMVectorSet(0, 0, 1, 0));
        CHECK(Finite3(ch.c));
        CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, t))) < 1e-3f);
    }
}

// 親に回転が乗っていても解ける（ローカル回転を書き換える実装であることの確認）
static void TestWithRotatedParent()
{
    const XMVECTOR parent = XMQuaternionRotationRollPitchYaw(0.3f, 1.1f, -0.4f);
    Chain ch = MakeChain();
    ForwardKinematics(ch, parent);

    const XMVECTOR t = XMVectorAdd(ch.a, XMVectorSet(0.4f, 1.3f, 0.2f, 0));
    SolveTwoBoneIK(ch.a, ch.b, ch.c, t, XMVectorSet(0, 0, 1, 0),
                   ch.aGlobal, ch.bGlobal, ch.aLocal, ch.bLocal);
    ForwardKinematics(ch, parent);

    CHECK(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.c, t))) < 1e-4f);
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(ch.b, ch.a))), ch.lab, 1e-5f));
}

// ---------------------------------------------------------------------------
// 補助関数
// ---------------------------------------------------------------------------
static void TestQuaternionFromTo()
{
    const XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    // 45 度傾いた斜面の法線へ up を向ける
    const XMVECTOR n = XMVector3Normalize(XMVectorSet(1, 1, 0, 0));
    const XMVECTOR q = QuaternionFromTo(up, n);
    const XMVECTOR r = XMVector3Rotate(up, q);
    CHECK(Near(XMVectorGetX(XMVector3Length(XMVectorSubtract(r, n))), 0.0f, 1e-5f));

    // 同一方向 → 単位四元数
    const XMVECTOR qi = QuaternionFromTo(up, up);
    CHECK(Near(XMVectorGetW(qi), 1.0f, 1e-5f));

    // 真逆 → 180 度（NaN を出さない）
    const XMVECTOR qd = QuaternionFromTo(up, XMVectorNegate(up));
    const XMVECTOR rd = XMVector3Rotate(up, qd);
    CHECK(Near(XMVectorGetY(rd), -1.0f, 1e-4f));

    // ゼロベクトル → 単位四元数
    const XMVECTOR qz = QuaternionFromTo(XMVectorZero(), up);
    CHECK(Near(XMVectorGetW(qz), 1.0f, 1e-5f));
}

static void TestClampQuaternionAngle()
{
    const XMVECTOR axis = XMVector3Normalize(XMVectorSet(0, 0, 1, 0));
    const XMVECTOR q60 = XMQuaternionRotationAxis(axis, XMConvertToRadians(60.0f));

    // 45 度に制限
    const XMVECTOR c = ClampQuaternionAngle(q60, XMConvertToRadians(45.0f));
    XMVECTOR outAxis; float outAngle = 0.0f;
    XMQuaternionToAxisAngle(&outAxis, &outAngle, c);
    CHECK(Near(std::fabs(outAngle), XMConvertToRadians(45.0f), 1e-3f));

    // 制限より小さい回転はそのまま
    const XMVECTOR q10 = XMQuaternionRotationAxis(axis, XMConvertToRadians(10.0f));
    const XMVECTOR c2 = ClampQuaternionAngle(q10, XMConvertToRadians(45.0f));
    XMQuaternionToAxisAngle(&outAxis, &outAngle, c2);
    CHECK(Near(std::fabs(outAngle), XMConvertToRadians(10.0f), 1e-3f));

    // 単位四元数でも NaN を出さない
    const XMVECTOR ci = ClampQuaternionAngle(XMQuaternionIdentity(), XMConvertToRadians(45.0f));
    CHECK(std::isfinite(XMVectorGetW(ci)));
}

// 指数平滑はフレームレート非依存（同じ実時間なら同じ結果に収束する）
static void TestExpSmooth()
{
    const float tau = 0.1f;

    float a = 0.0f;
    for (int i = 0; i < 60; ++i) a = ExpSmooth(a, 1.0f, tau, 1.0f / 60.0f);

    float b = 0.0f;
    for (int i = 0; i < 240; ++i) b = ExpSmooth(b, 1.0f, tau, 1.0f / 240.0f);

    std::printf("  [smooth] 60fps -> %.6f, 240fps -> %.6f (1 sec, tau=0.1)\n", a, b);
    CHECK(Near(a, b, 1e-3f));
    CHECK(a > 0.99f);

    // tau=0 は即座に target
    CHECK(Near(ExpSmooth(0.0f, 5.0f, 0.0f, 0.016f), 5.0f));
    // dt=0 でも NaN を出さない
    CHECK(std::isfinite(ExpSmooth(1.0f, 2.0f, 0.1f, 0.0f)));
}

int main()
{
    TestReachableTarget();
    TestUnequalBoneLengths();
    TestUnreachableFar();
    TestUnreachableNear();
    TestHintControlsKneeWhenStraight();
    TestBentLimbKeepsItsSide();
    TestDegenerate();
    TestWithRotatedParent();
    TestQuaternionFromTo();
    TestClampQuaternionAngle();
    TestExpSmooth();

    std::printf("TwoBoneIKTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
