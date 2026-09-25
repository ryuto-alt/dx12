#pragma once

// ============================================================================
// 通路（パス・コリドー）— Detour の dtPathCorridor と同じ考え方
// ============================================================================
// 経路を「折れ線」ではなく「通るポリゴンの列」で持ち回る。
//   - 毎フレームの角（コーナー）は、現在地からこの列の中をファネルで引き直して得る
//     （＝角を回り終えるとその角は自然に出てこなくなる。到達判定が要らない）。
//   - 位置の更新は MovePosition 一箇所。滑り移動の結果で列の先頭を差し替えるので、
//     少し押し出されたり戻されたりしても A* をやり直さずに済む。
//   - 目標が少し動いただけなら MoveTargetPosition で列の末尾を伸ばす（A* をやり直さない）。
//   - ナビメッシュが焼き直されたら（世代が変わったら）IsValid が false になる。
//
// MikuChase.lua が Lua で作り直していた「頻繁に張り直す」「角を押し出して狙う」
// 「縁から押し戻す」は、この通路 + 群衆（NavCrowd）がエンジン側で引き取る。
// ============================================================================

#include <vector>

#include "nav/NavTypes.h"

namespace dx12e
{
namespace nav
{

class NavCorridor
{
public:
    // 位置だけの通路にする（poly は pos を含むポリゴン）
    void Reset(i32 poly, const f32 pos[3], u32 generation);
    void Clear();

    // 現在地から target までの通路を A* で張る。target はナビメッシュへ落としてから使う。
    // 到達できなければ「一番近いところまで」の通路になり Partial を返す。
    NavPathStatus Plan(const NavMesh& nav, const f32 target[3], const f32 ext[3]);

    // 現在地から先の角を最大 maxCorners 個（最後は目標点）。margin > 0 なら角を
    // 壁から margin だけ離して返す（通路の幅が足りない所では幅の半分まで）。
    // 戻り値 = 角の数。flags（任意）に kStraightPath* が入る。
    i32 FindCorners(const NavMesh& nav, std::vector<f32>& outCorners, std::vector<u8>* outFlags,
                    i32 maxCorners, f32 margin = 0.0f) const;

    // 現在地を npos へ滑らせて動かす（通路の先頭を差し替える）。戻り値 = 動けたか。
    bool MovePosition(const NavMesh& nav, const f32 npos[3]);
    // 目標を npos へ滑らせて動かす（通路の末尾を伸ばす / 縮める）。
    // 戻り値 = 目標が npos に届いたか（壁で止まったら false → 呼び側が Plan し直す）
    bool MoveTargetPosition(const NavMesh& nav, const f32 npos[3]);
    // 見通せる所まで通路を近道する（Detour の optimizePathVisibility）
    void OptimizePathVisibility(const NavMesh& nav, const f32 next[3], f32 range);

    // 通路が今のナビメッシュでまだ使えるか（世代が同じで、全ポリゴンが有効）
    bool IsValid(const NavMesh& nav) const;
    bool Empty() const { return m_path.empty(); }

    const f32* Pos() const { return m_pos; }
    const f32* Target() const { return m_target; }
    i32  FirstPoly() const { return m_path.empty() ? -1 : m_path.front(); }
    i32  LastPoly() const { return m_path.empty() ? -1 : m_path.back(); }
    const std::vector<i32>& Path() const { return m_path; }
    NavPathStatus Status() const { return m_status; }
    bool HasTarget() const { return m_hasTarget; }
    // 通路に沿った現在地→目標の長さ（角をたどった距離）
    f32  PathLength(const NavMesh& nav) const;

private:
    f32 m_pos[3]{ 0, 0, 0 };
    f32 m_target[3]{ 0, 0, 0 };
    std::vector<i32> m_path;
    u32 m_generation = 0;
    NavPathStatus m_status = NavPathStatus::Failed;
    bool m_hasTarget = false;
};

// ---- 通路のマージ（Detour の dtMergeCorridor* と同じ規則。テストから直接叩く）----
// 滑り移動で通ったポリゴン visited（始点→着地点）で通路の先頭を差し替える
void MergeCorridorStartMoved(std::vector<i32>& path, const std::vector<i32>& visited);
// 目標を滑らせて通ったポリゴン visited（旧目標→新目標）で通路の末尾を差し替える
void MergeCorridorEndMoved(std::vector<i32>& path, const std::vector<i32>& visited);
// レイで見通せたポリゴン visited（先頭→見通せた所）で通路の先頭を近道する
void MergeCorridorStartShortcut(std::vector<i32>& path, const std::vector<i32>& visited);

} // namespace nav
} // namespace dx12e
