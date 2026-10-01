#pragma once
// ===========================================================================
// インスタンス群（InstanceGroup）の編集操作: 変換（個別エンティティ → 群）/ 展開（群 → 個別エンティティ）/
// インスタンスの追加・削除・変更。MCP `instance_group`・エディタの「インスタンス群にまとめる」・Inspector の「展開」が共通で使う。
// 設計: docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1（変換の条件・展開・MCP）。
//
//   変換の条件（満たさないものは変換せず、理由を集計して返す）:
//     同じ modelPath・同じ材質設定・同じコンポーネント構成（Transform・名前・guid 以外が JSON として完全一致）・同じ親・
//     子なし・スクリプト / トリガー / 音 / ライト等なし（許可するキーの表に無い）・剛体は静的のみ（MeshCollider と対）・
//     guid（または名前）で他から参照されていない（Trigger の相手 / Lua の entity プロパティ）・uvTiling なし（頂点を焼くので共有メッシュと衝突）。
//   群は「最初のメンバー（guid 昇順）をその場で群に変える」（モデルを読み直さない）。インスタンスは guid 昇順に並べる
//   （＝描画の同点順が変換前と同じになり、決定論スクショが保たれる）。群の Transform は恒等（インスタンスは元のローカル TRS のまま）。
// ===========================================================================
#include <map>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "ecs/InstanceGroup.h"

namespace dx12e
{
class Scene;
}

namespace dx12e::instgroup
{

// Undo / Redo 用の記録（スナップショットの JSON は SceneSerializer::SerializeEntity。群の JSON の実体は台帳の "mem" で共有される）。
struct GroupUndoRecord
{
    std::vector<std::string> memberJson;   // 個別エンティティ側（変換前 / 展開後）
    std::vector<u64>         memberGuids;
    std::string              groupJson;    // 群側
    u64                      groupGuid = 0;
    u64                      parentGuid = 0;   // 共通の親（0 = ルート）
};

struct ConvertOptions
{
    std::vector<entt::entity> targets;   // 空 = シーン全体から自動。指定ありはその中だけ（個数の下限は 2）
    u32  minCount = 16;                  // 自動: 同じ条件の配置がこの個数以上あるものだけまとめる
    bool dryRun   = false;               // true = 何も変えず、まとめられる群の数と理由だけ返す
};

struct GroupSummary
{
    entt::entity entity = entt::null;    // 作った群（dryRun では null）
    std::string  name;
    std::string  modelPath;
    u32          count = 0;
};

struct ConvertResult
{
    bool        ok = true;
    std::string error;
    u32 candidates     = 0;    // 描画物（MeshRenderer）を持つ候補の数
    u32 converted      = 0;    // 群にまとめた元エンティティの数
    u32 groupsCreated  = 0;
    u32 entitiesBefore = 0;    // 名前を持つエンティティの総数
    u32 entitiesAfter  = 0;
    std::vector<GroupSummary> groups;
    std::map<std::string, u32> skipped;   // 変換しなかった理由 → 数
    std::vector<GroupUndoRecord> undo;    // dryRun でなければ Undo 用の記録（群ごと）
};

// 変換。assetsDir は SerializeEntity に渡すもの。Editor モード（Play 中でない）で呼ぶこと。
ConvertResult ConvertToGroups(Scene& scene, const std::string& assetsDir, const ConvertOptions& opt);

struct ExplodeResult
{
    bool        ok = true;
    std::string error;
    u32         created = 0;
    std::vector<entt::entity> entities;
    GroupUndoRecord undo;
};
// 展開（群 → 個別エンティティ）。群のエンティティは消え、同じ親の下に N 個のエンティティができる（名前は「<モデル名>_0001」…）。
ExplodeResult ExplodeGroup(Scene& scene, entt::entity group, const std::string& assetsDir);

// Undo / Redo: 記録の「変換前 ⇄ 群」を入れ替える。toGroup=true なら メンバー → 群、false なら 群 → メンバー。
// モデルの読み込みを伴うので、呼び出し側は cmdList が有効なフレーム境界で実行すること（他の Undo コマンドと同じ）。
void ApplyUndoRecord(Scene& scene, const std::string& assetsDir, const GroupUndoRecord& rec, bool toGroup);

// ---- インスタンスの編集（コピーオンライト。群の _set を新しい実体へ差し替える）----
struct InstanceEdit
{
    bool ok = true;
    std::string error;
    u32 count = 0;
};
InstanceEdit AddInstances(Scene& scene, entt::entity group, const std::vector<InstanceTRS>& items);
InstanceEdit RemoveInstances(Scene& scene, entt::entity group, const std::vector<u32>& indices);   // 重複・範囲外は無視。残りの番号は詰まる
InstanceEdit SetInstance(Scene& scene, entt::entity group, u32 index, const InstanceTRS& value);

} // namespace dx12e::instgroup
