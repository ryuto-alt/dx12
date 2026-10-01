#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cmath>

// ===== 物理まわりの「判断だけ」の純粋関数（Jolt にも entt にも依存しない）=====
//
// PhysicsSystem.cpp の中で数値の規約が散らばらないよう、ここに置いてテストで固定する
// （tests/physics_logic_test.cpp）。Jolt 側は結果を受けて動かすだけ。
//
// 背景（2026-10）:
//   ・動的剛体は 60Hz 固定ステップで進むが描画は 144fps 等 → 補間しないと
//     「同じ位置のフレーム」と「飛ぶフレーム」が混ざって、がくがく動いて見える。
//   ・CharacterVirtual（質量 70kg）が既定質量 1kg の箱の上に載ると、Jolt が体重ぶんの
//     インパルス(980N*dt)を箱へ与え、反発係数 0.4 で箱が十数 m/s で跳ね飛ぶ。
//     箱の速度を接地面速度としてキャラが拾うと、さらにキャラが吹き飛ぶ（フィードバック）。
namespace dx12e::physlogic
{

// ---- 描画補間 ----

// accumulator の端数 → 補間係数（0..1）。step<=0 は 1（補間しない）。
inline f32 InterpAlpha(f32 accumulator, f32 step)
{
    if (!(step > 0.0f)) return 1.0f;
    return std::clamp(accumulator / step, 0.0f, 1.0f);
}

// 1 固定ステップで maxDist(m) を超えて動いたものは「移動」ではなく「テレポート」とみなし、
// 補間せず現在の姿勢をそのまま出す（旧位置から新位置を舐めて見えるのを防ぐ）。
inline bool IsTeleport(f32 dx, f32 dy, f32 dz, f32 maxDist = 5.0f)
{
    return dx * dx + dy * dy + dz * dz > maxDist * maxDist;
}

inline void LerpVec3(const f32 a[3], const f32 b[3], f32 t, f32 out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

// クォータニオン (x,y,z,w) の最短経路 nlerp。1 ステップぶんの小さな角度なので slerp と実質同じ。
inline void NlerpQuat(const f32 a[4], const f32 b[4], f32 t, f32 out[4])
{
    const f32 dot  = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const f32 sign = dot < 0.0f ? -1.0f : 1.0f;   // 反対側の半球なら符号を反転して短い方へ
    f32 len2 = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        out[i] = a[i] + (b[i] * sign - a[i]) * t;
        len2 += out[i] * out[i];
    }
    if (len2 > 1.0e-12f)
    {
        const f32 inv = 1.0f / std::sqrt(len2);
        for (int i = 0; i < 4; ++i) out[i] *= inv;
    }
    else
    {
        for (int i = 0; i < 4; ++i) out[i] = b[i];
    }
}

// ---- キャラ（CharacterVirtual）と動的剛体 ----

// 動的剛体を接地面にしたとき、その速度をキャラへどれだけ継承するか（0..1）。
//   軽い箱（キャラ質量以下）は 0 ＝継承しない（箱の揺れをキャラが拾うフィードバックを断つ）。
//   キャラの 2 倍以上の重い物（筏・トロッコ等）は 1 ＝従来どおり運ばれる。間は線形。
// 静的/キネマティックは呼ぶ側で 1 にする（動く床・エレベーターは従来どおり運ぶ）。
inline f32 GroundVelocityScale(f32 bodyMass, f32 charMass)
{
    if (!(charMass > 0.0f)) return 1.0f;
    return std::clamp((bodyMass - charMass) / charMass, 0.0f, 1.0f);
}

// 動的剛体がキャラを押し返せるか（mCanPushCharacter）。
// キャラの半分未満の軽い物が 70kg のキャラを弾き飛ばしたり、足元の軽い箱に押し出されたりしない。
inline bool CanBodyPushCharacter(f32 bodyMass, f32 charMass)
{
    return bodyMass >= 0.5f * charMass;
}

// 足元の動的剛体へ掛かるキャラの体重を決めるための、キャラの実効質量(kg)。
// Jolt の CharacterVirtual は、動的剛体の上に立つと mMass×重力 のインパルスを毎ステップその剛体へ与える。
// 体重そのままだと、軽い箱（既定 1kg）には 980N*dt = 16m/s の押し込み速度になり、反発係数ぶん
// 跳ね返って箱が飛ぶ（実測: 箱 17〜27 m/s）。そこで「剛体の質量×maxAccel」を超えない質量に頭打ちする。
//   maxAccel = 15 m/s²（≒1g）: 60Hz で 1 ステップあたりの押し込みは 0.25 m/s。Jolt の反発の最小速度(1 m/s)より十分小さい。
//   キャラの 70kg に対し、剛体が約 65kg 以上なら頭打ちにならず体重がそのまま掛かる（シーソー・筏）。
inline f32 StandingMass(f32 charMass, f32 gravityAccel, f32 bodyMass, f32 maxAccel = 15.0f)
{
    charMass = std::max(0.0f, charMass);
    if (!(gravityAccel > 1.0e-6f)) return charMass;
    return std::min(charMass, std::max(0.0f, bodyMass) * maxAccel / gravityAccel);
}

} // namespace dx12e::physlogic
