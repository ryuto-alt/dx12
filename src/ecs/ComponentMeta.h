#pragma once
//
// コア部品のフィールド反映（entt::meta）— Phase 2 土台。
//
// RegisterCoreComponentMeta() を一度呼ぶと、コア部品の型とフィールドが entt::meta に登録され、
//   entt::resolve<T>().data() で各フィールドの name()/type()/get()/set() を取得できるようになる。
// これを Inspector の自動 UI 生成（型ごとの描画コードを撤廃）と、将来の汎用シリアライズが消費する。
//
// このヘッダは宣言のみ（具象型に依存しない）。登録の実体は ComponentMeta.cpp。
//
namespace dx12e
{
// コア部品の型/フィールドを entt::meta へ登録する（冪等：2回目以降は無視）。
void RegisterCoreComponentMeta();
}
