#pragma once
// コンポーネントのシリアライザ登録表（W05-15 / R-00b）。
//
// 新しいコンポーネント（VFX など）は ecs/Components.h（135 TU が読む）を触らず、
// ComponentSerializerList.inc に UNO_COMPONENT_SERIALIZER(Fn) を 1 行足すだけで、
// シーンの保存 / 読み込みと MCP の Undo（McpUndoTrack のフォールバック）に載る。
//
// ★自己登録（静的初期化子）は使わない。静的ライブラリでは .obj ごとリンカに捨てられるため、
//   登録は .inc の明示リストだけで行う。
//   Fn は `void Fn(std::vector<ComponentSerializer>& out)` で、自分の ComponentSerializer を push_back する。
#include <entt/entt.hpp>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace dx12e
{

struct ComponentSerializer
{
    const char* typeName = nullptr;                                         // JSON のキー（エンティティの直下）
    bool (*has)(const entt::registry&, entt::entity) = nullptr;
    void (*save)(const entt::registry&, entt::entity, nlohmann::json& out) = nullptr;
    void (*load)(entt::registry&, entt::entity, const nlohmann::json& in) = nullptr;
    bool (*skipOnSave)(const entt::registry&, entt::entity) = nullptr;      // 保存から外す述語（EditorTransient 用）。null なら外さない
    // Undo で「前は無かった」状態へ戻すときに呼ぶ（McpUndoTrack のフォールバック用）。null ならそのまま残す。
    void (*remove)(entt::registry&, entt::entity) = nullptr;
};

// ComponentSerializerList.inc を展開して out へ並べる（.inc に書いた順）。
void RegisterAllComponentSerializers(std::vector<ComponentSerializer>& out);

// 初回呼び出しで構築する（スレッドセーフな関数内 static）。
const std::vector<ComponentSerializer>& ComponentSerializers();

// typeName で引く。無ければ nullptr。
const ComponentSerializer* FindComponentSerializer(const char* typeName);

// テスト専用: .inc の登録とは別に、一時的にシリアライザを足す / 外す（.inc は変えられないので、
// 未知の型の往復をテストで確かめるための入口）。本体のコードからは呼ばない。
// 要素の参照が変わらないよう、登録表は最初に 256 件ぶん確保してある（超えた分は足さない）。
void TestOnlyAppendComponentSerializer(const ComponentSerializer& cs);
void TestOnlyResetComponentSerializers();   // .inc 由来の分だけに戻す

} // namespace dx12e
