#pragma once
// シーケンサー S1b: バインディング解決(エンジン側)。
//   seq::IBindingResolver(S0 の抽象)を entt::registry の上で実装する。
//   解決順は S0 の規則どおり(上書き → guid → 階層パス → 名前)。ここは「候補を engine の優先順で返す」だけを担う。
//
// ★エンティティの guid は「保存時にしか付かない」(SceneSerializer::BuildSceneJson)。シーケンスが参照するエンティティは
//   EnsureEntityGuid で先に確定させる(エディタのスクラブの元値退避も guid で引くため。設計書 §1.9-1)。
// ★実行時の entt::entity は Play/Stop や open_scene で全部変わる。TargetId は保持せず、シーンが変わったら解決し直す。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "sequencer/SeqBinding.h"

namespace dx12e
{
namespace seqhost
{

// TargetId(0 = 無効)⇔ entt::entity
seq::TargetId ToTarget(entt::entity e);
entt::entity  ToEntity(seq::TargetId t);

// guid が無ければその場で振って返す(0 を返すのは無効なエンティティだけ)。
std::uint64_t EnsureEntityGuid(entt::registry& reg, entt::entity e);

// 階層パス = ルートから自分までの NameTag を '/' で連ねたもの("Rig/CutsceneCam")。名前中の '/' と '%' は %2F / %25 に置換する。
// 親が循環していても落ちない(深さ 64 で打ち切り)。
std::string EntityHierarchyPath(const entt::registry& reg, entt::entity e);

// バインディングの hint(guid / path / name)をエンティティから作る。ensureGuid なら guid を確定させる。
seq::BindingHint MakeBindingHint(entt::registry& reg, entt::entity e, bool ensureGuid = true);

// registry の上の解決器。索引は最初に必要になった時に 1 回だけ作る(この寿命の間はシーンが変わらない前提。毎フレームは作り直さない)。
class EngineBindingResolver final : public seq::IBindingResolver
{
public:
    explicit EngineBindingResolver(const entt::registry& reg) : m_reg(reg) {}

    seq::TargetId FindByGuid(std::string_view guidHex) const override;
    void FindByPath(std::string_view path, std::vector<seq::TargetId>& out) const override;
    void FindByName(std::string_view name, std::vector<seq::TargetId>& out) const override;

private:
    const entt::registry& m_reg;
    mutable bool m_haveGuid = false, m_haveName = false, m_havePath = false;
    mutable std::unordered_map<std::uint64_t, entt::entity> m_byGuid;
    // 候補は Scene::FindEntity と同じ優先順(= view.each() の順 = 最後に作られたものが先頭)
    mutable std::unordered_map<std::string, std::vector<entt::entity>> m_byName;
    mutable std::unordered_map<std::string, std::vector<entt::entity>> m_byPath;
};

} // namespace seqhost
} // namespace dx12e
