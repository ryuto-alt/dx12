#pragma once
// バインディングの解決(抽象)。S0 は解決の規則とインターフェース、テスト用の実装まで。
// 実エンジン(EntityGuid / 階層パス / 名前)への接続は S1b の適用層が IBindingResolver を実装して行う。
//
// 解決順(docs/SEQUENCER_DESIGN.md §2.4):
//   ① SequencePlayer 側の上書き(bindingId → guid)  ② hint.guid  ③ hint.path  ④ hint.name
//   scene バインディングは常に kSceneTarget。spawnable は S7 まで未対応(未解決 + 警告)。

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

// 解決先の不透明 ID。エンジン側では entt::entity 等を詰める(0 = 無効)。
// ★実行時のエンティティ ID は Stop で全部変わるので、解決結果を保存/長期保持しないこと。
using TargetId = std::uint64_t;
constexpr TargetId kNoTarget = 0;
constexpr TargetId kSceneTarget = ~TargetId(0);   // scene バインディング(ポスト・時間などグローバル)

// 解決の窓口(エンジン側が実装)。
class IBindingResolver
{
public:
    virtual ~IBindingResolver() = default;
    // EntityGuid(16 桁 hex)で一意に引く。無ければ kNoTarget。
    virtual TargetId FindByGuid(std::string_view guidHex) const = 0;
    // 階層パス("A/B/C")の候補。**エンジンの優先順(先頭 = 採用される方)**で out へ入れる。
    virtual void FindByPath(std::string_view path, std::vector<TargetId>& out) const = 0;
    // 名前の候補。同上。エンジンの Scene::FindEntity は「最後に作られたものが勝つ」ので、その順で返すこと。
    virtual void FindByName(std::string_view name, std::vector<TargetId>& out) const = 0;
};

enum class ResolveVia : std::uint8_t { None = 0, Override, Guid, Path, Name, Scene };
const char* ResolveViaName(ResolveVia v);

struct BindingResolution
{
    ResolveVia via = ResolveVia::None;
    TargetId target = kNoTarget;
    int candidates = 0;   // Path / Name で見つかった候補の数(2 以上 = 曖昧)
    bool Resolved() const { return via != ResolveVia::None; }
};

struct BindingIssue
{
    enum class Code : std::uint8_t
    {
        Unresolved,            // どの手段でも見つからない(トラックは無効化されるがデータは保持)
        FellBack,              // guid では見つからず path / name で見つかった(hint の更新を促す)
        AmbiguousPath,         // パスの候補が複数(先頭を採用)
        AmbiguousName,         // 名前の候補が複数(先頭 = エンジンの優先順を採用)
        SharedTarget,          // 別のバインディングも同じ対象に解決された
        SpawnableUnsupported,  // spawnable は未対応
    };
    Code code = Code::Unresolved;
    std::string bindingId;
    std::vector<std::string> trackIds;   // 影響を受けるトラック(Unresolved のとき = 無効化されるトラック全部)
    std::string message;                 // 日本語
};

struct BindingSet
{
    std::vector<BindingResolution> byBinding;   // seq.bindings と同じ並び
    std::vector<BindingIssue> issues;
    bool AllResolved() const;
    // EvalFilter::bindingEnabled 用(解決できたバインディング = 1)
    std::vector<std::uint8_t> EnabledMask() const;
};

// bindingId → guid(hex) の上書き表(SequencePlayer.bindings)。null 可。
using BindingOverrides = std::map<std::string, std::string>;

BindingResolution ResolveBinding(const Binding& b, const IBindingResolver& resolver,
                                 const BindingOverrides* overrides, std::vector<BindingIssue>* issues);
BindingSet ResolveBindings(const Sequence& seq, const IBindingResolver& resolver,
                           const BindingOverrides* overrides = nullptr);

// ---------------------------------------------------------------------------
// テスト用の解決器(メモリ上の表)。S1a のパネルの単体テストや MCP のドライランにも使える。
// ---------------------------------------------------------------------------
class MapBindingResolver final : public IBindingResolver
{
public:
    struct Entity
    {
        TargetId id = kNoTarget;
        std::string guid;
        std::string path;
        std::string name;
    };
    // 追加順 = 作成順。FindByPath / FindByName は**新しく作られたものが先**で返す(エンジンの実挙動を模す)。
    void Add(Entity e) { m_entities.push_back(std::move(e)); }
    void Clear() { m_entities.clear(); }
    std::size_t Size() const { return m_entities.size(); }

    TargetId FindByGuid(std::string_view guidHex) const override;
    void FindByPath(std::string_view path, std::vector<TargetId>& out) const override;
    void FindByName(std::string_view name, std::vector<TargetId>& out) const override;

private:
    std::vector<Entity> m_entities;
};

} // namespace dx12e::seq
