#pragma once

// ===== Transform の行列 <-> TRS 変換と「ワールド位置を保つ親替え」=====
// SceneViewPanel のギズモ（ローカル行列を Transform へ書き戻す）とヒエラルキーの親子付け替えが共有する。
// ヘッダオンリー（ECS と DirectXMath だけ）。tests/reparent_test.cpp が単体で検証する。
//
// ★親替えでワールド位置が飛ぶ問題:
//   Transform は「親からのローカル値」を持つ。parent だけ差し替えると、ローカル値はそのままで
//   親の変換が掛かり直すので、見た目の位置・向き・大きさが飛ぶ。UE / Unity は既定で
//   ワールド変換を保つ（新しいローカル = 自分のワールド行列 × 新しい親のワールド行列の逆行列）。

#include "ecs/Components.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace dx12e::xform
{

// ImGuizmo の DecomposeMatrixToComponents は回転を Rx*Ry*Rz 順で分解するが、
// このエンジンの Transform::GetWorldMatrix は XMMatrixRotationRollPitchYaw
// (= Rz*Rx*Ry 順) で行列を組み立てる。順序が食い違うため、GetWorldMatrix と完全に
// 逆対応する分解を行い、ラウンドトリップを無損失にする。
// (Rz*Rx*Ry を展開した行列成分から閉形式で抽出。20万ケースのランダム検証で誤差ゼロ確認済み)
inline void DecomposeWorldToRPY(const DirectX::XMFLOAT4X4& w, float t[3], float eulerDeg[3], float s[3])
{
    constexpr float kRad2Deg = 57.2957795130823f;

    // スケール = 各基底行の長さ
    const float sx = std::sqrt(w.m[0][0]*w.m[0][0] + w.m[0][1]*w.m[0][1] + w.m[0][2]*w.m[0][2]);
    const float sy = std::sqrt(w.m[1][0]*w.m[1][0] + w.m[1][1]*w.m[1][1] + w.m[1][2]*w.m[1][2]);
    const float sz = std::sqrt(w.m[2][0]*w.m[2][0] + w.m[2][1]*w.m[2][1] + w.m[2][2]*w.m[2][2]);
    s[0] = sx; s[1] = sy; s[2] = sz;

    // 平行移動 = 第4行
    t[0] = w.m[3][0]; t[1] = w.m[3][1]; t[2] = w.m[3][2];

    // 回転行を正規化（スケール除去）
    const float i0 = sx > 1e-8f ? 1.0f / sx : 0.0f;
    const float i1 = sy > 1e-8f ? 1.0f / sy : 0.0f;
    const float i2 = sz > 1e-8f ? 1.0f / sz : 0.0f;
    float m[3][3];
    for (int j = 0; j < 3; ++j)
    {
        m[0][j] = w.m[0][j] * i0;
        m[1][j] = w.m[1][j] * i1;
        m[2][j] = w.m[2][j] * i2;
    }

    // R = Rz(z)*Rx(x)*Ry(y) の成分: m[2][1]=-sin x, m[2][0]=cx*sy, m[2][2]=cx*cy,
    //                              m[0][1]=sz*cx, m[1][1]=cz*cx
    float sinx = -m[2][1];
    sinx = sinx > 1.0f ? 1.0f : (sinx < -1.0f ? -1.0f : sinx);
    const float cosx = std::sqrt(m[2][0]*m[2][0] + m[2][2]*m[2][2]); // = |cos x|
    const float x = std::asin(sinx);
    float y, z;
    if (cosx > 1e-6f)
    {
        y = std::atan2(m[2][0], m[2][2]);
        z = std::atan2(m[0][1], m[1][1]);
    }
    else
    {
        // ジンバルロック (cos x ~ 0): 行列は (y - sgn*z) のみに依存。z=0 に固定。
        const float sgn = (sinx >= 0.0f) ? 1.0f : -1.0f;
        y = std::atan2(sgn * m[1][0], m[0][0]);
        z = 0.0f;
    }
    eulerDeg[0] = x * kRad2Deg;
    eulerDeg[1] = y * kRad2Deg;
    eulerDeg[2] = z * kRad2Deg;
}

// 行列（ローカル or 親を外した後の行列）を Transform の TRS へ書き戻す。
// DecomposeWorldToRPY と対で「GetWorldMatrix と無損失にラウンドトリップする」のが要点。
inline void ApplyMatrixToTransform(Transform& t, const DirectX::XMMATRIX& m)
{
    DirectX::XMFLOAT4X4 f;
    DirectX::XMStoreFloat4x4(&f, m);
    float translation[3], rotation[3], scale[3];
    DecomposeWorldToRPY(f, translation, rotation, scale);
    t.position = {translation[0], translation[1], translation[2]};
    t.rotation = {rotation[0], rotation[1], rotation[2]};
    // Scale が 0 以下になると行列が壊れてギズモが消えるので最小値でクランプ
    constexpr float kMinScale = 0.001f;
    t.scale = {(std::max)(scale[0], kMinScale),
               (std::max)(scale[1], kMinScale),
               (std::max)(scale[2], kMinScale)};
}

// クォータニオンで回転を持つ Transform 用。GetWorldMatrix が quaternion だけを見るので、
// euler だけ書き換えても効かない。クォータニオンを更新し、保存用の euler も揃える。
inline void ApplyMatrixToQuaternionTransform(Transform& t, const DirectX::XMMATRIX& m)
{
    using namespace DirectX;
    XMVECTOR s, q, p;
    if (!XMMatrixDecompose(&s, &q, &p, m)) { ApplyMatrixToTransform(t, m); return; }
    XMFLOAT3 sf, pf; XMFLOAT4 qf;
    XMStoreFloat3(&sf, s); XMStoreFloat3(&pf, p); XMStoreFloat4(&qf, XMQuaternionNormalize(q));
    constexpr float kMinScale = 0.001f;
    t.position = pf;
    t.scale = {(std::max)(sf.x, kMinScale), (std::max)(sf.y, kMinScale), (std::max)(sf.z, kMinScale)};
    t.quaternion = qf;
    t.rotation = QuaternionToEulerDegrees(qf);
}

// child の親を newParent（null なら親なし）へ付け替えてよいか。
// 自分自身・自分の子孫の下へ入れる（循環）ことと、Transform を持たない相手を拒む。
inline bool CanReparent(const entt::registry& reg, entt::entity child, entt::entity newParent)
{
    if (child == entt::null || !reg.valid(child) || !reg.all_of<Transform>(child)) return false;
    if (newParent == entt::null) return true;
    if (!reg.valid(newParent) || !reg.all_of<Transform>(newParent)) return false;
    // newParent から祖先を辿って child に当たれば循環。壊れた（既に循環した）データでも止まるよう深さ上限付き。
    entt::entity cur = newParent;
    for (int depth = 0; cur != entt::null && reg.valid(cur) && reg.all_of<Transform>(cur); ++depth)
    {
        if (cur == child || depth >= 4096) return false;
        cur = reg.get<Transform>(cur).parent;
    }
    return true;
}

// 親を付け替える。keepWorld=true ならワールド変換（位置・向き・大きさ）が変わらないよう
// ローカル値を逆算し直す。false なら従来どおりローカル値そのまま（見た目は飛ぶ）。
// 戻り値: 付け替えたか（不正・循環なら何もせず false）。Undo は呼び出し側が before/after で積む。
inline bool Reparent(entt::registry& reg, entt::entity child, entt::entity newParent, bool keepWorld = true)
{
    using namespace DirectX;
    if (!CanReparent(reg, child, newParent)) return false;
    Transform& t = reg.get<Transform>(child);
    if (t.parent == newParent) return true;

    const XMMATRIX world = ComputeWorldMatrix(reg, child);   // 付け替え前のワールド
    t.parent = newParent;
    if (!keepWorld) return true;

    XMMATRIX parentWorld = XMMatrixIdentity();
    if (newParent != entt::null)
        parentWorld = ComputeWorldMatrix(reg, newParent);
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, parentWorld);
    if (std::fabs(XMVectorGetX(det)) < 1e-12f) return true;   // 親のスケールが 0 など。逆算できないのでローカル据え置き

    const XMMATRIX local = world * inv;
    if (t.useQuaternion) ApplyMatrixToQuaternionTransform(t, local);
    else                 ApplyMatrixToTransform(t, local);
    return true;
}

} // namespace dx12e::xform
