#pragma once
//
// RuntimeComponentRegistry — 封印ランタイムの「中立コンポーネント機構」（Phase 1 土台）。
//
// 目的:
//   SceneSerializer の if(reg.all_of<T>()) 型ごと連鎖（serialize / instantiate / applyOverride）を、
//   typeName -> ハンドラ表の単一走査へ置き換えるための登録口。
//
// 設計上の不変条件:
//   このヘッダは「具象コンポーネント型（Transform / RigidBody 等）を一切 include しない」中立面に保つ。
//   将来 dx12-runtime（封印 exe）から参照しても、ジャンル語彙にも具象型にも依存しないようにするため。
//   ゲーム/ジャンル語彙（Gimmick 等）は当然ここに現れない。
//
// Phase 0 の段階:
//   まだ SceneSerializer からは参照しない（型と登録口の定義のみ）。
//   コア部品のフィールド反映は entt::meta ベースの ComponentMeta（Phase 1）で行い、
//   その結果をここへ Register する。
//
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

#include <entt/entt.hpp>
#include <nlohmann/json_fwd.hpp>

namespace dx12e
{

// 部品の出自。
//   Core = C++ で実装された固定部品（Transform/Light/RigidBody…）。
//   Lua  = データ駆動で定義された部品（Phase 2 以降の ScriptComponents）。
enum class ComponentSource
{
    Core,
    Lua,
};

// 1 コンポーネント型ぶんの「ランタイム面」ハンドラ。
// エディタ非依存（描画/Inspector UI は別レジストリ EditorComponentRegistry が typeName で後付け登録する）。
struct RuntimeComponentInfo
{
    std::string     typeName;
    ComponentSource source = ComponentSource::Core;

    // entity が当該コンポーネントを持つとき out へ書き出す（持たなければ無操作）。
    std::function<void(const entt::registry&, entt::entity, nlohmann::json& out)> serialize;
    // in から当該コンポーネントを生成/設定する。
    std::function<void(entt::registry&, entt::entity, const nlohmann::json& in)>  deserialize;
    // Play 時のエディタ値の部分上書き（位置/マテリアル等）。未対応なら空でよい。
    std::function<void(entt::registry&, entt::entity, const nlohmann::json& in)>  applyOverride;
    // 依存順がある後処理（例: ConvexHull の Mesh 由来頂点再生成）。未対応なら空でよい。
    std::function<void(entt::registry&, entt::entity)>                            postLoad;
};

// typeName -> RuntimeComponentInfo の単一レジストリ（プロセス内シングルトン）。
// 実装は Phase 1 の ComponentRegistry.cpp で与える（Phase 0 では宣言のみ）。
class RuntimeComponentRegistry
{
public:
    static RuntimeComponentRegistry& Get();

    void Register(RuntimeComponentInfo info);
    const RuntimeComponentInfo* Find(const std::string& typeName) const;
    void ForEach(const std::function<void(const RuntimeComponentInfo&)>& fn) const;

    std::size_t Count() const { return m_infos.size(); }

private:
    std::vector<RuntimeComponentInfo> m_infos;
};

} // namespace dx12e
