#pragma once

// ===== グラフ文書の JSON 保存/復元とクリップボード（純ロジック）=====
// 保存形式は 1 ノード / 1 接続 / 1 コメントが 1 行（git の差分が取りやすい）。ID は安定（保存・読込・Undo を通して不変）。
// 浮動小数は位置 0.01・値 0.0001 に丸めて書く（往復で同じ文字列になる = 正準）。
//
//   {
//    "kind": "nodegraph",
//    "version": 1,
//    "view": {"pan": [0, 0], "zoom": 1},
//    "nodes": [
//     {"id":1,"type":"add","pos":[100,200],"values":{"1":[0.5]}},
//    ],
//    "edges": [
//     [1,0,2,1]          ← [出力ノード, 出力ピン, 入力ノード, 入力ピン]
//    ],
//    "comments": [
//     {"id":1,"rect":[0,0,300,200],"title":"...","color":2}
//    ]
//   }

#include "editor/nodegraph/GraphDocument.h"

#include <string>

namespace dx12e::ng
{

struct ViewState
{
    Vec2  pan;
    float zoom = 1.0f;
};

// 文書全体を JSON にする。view が null なら "view" を書かない（ファズ・正準比較用）。
std::string SaveGraphJson(const GraphDocument& doc, const ViewState* view);

// 読み込む。文書（モデル・コメント・履歴）は先に空にする。パース失敗は false（文書は変えない）。
// 未知のノード型・不正な接続は読み飛ばし、warnings に日本語で積む（読み込み自体は成功）。
bool LoadGraphJson(GraphDocument& doc, const std::string& json, ViewState* outView, std::string* warnings);

// 選択範囲（ノード + コメント + 選択内で完結する接続）をクリップボード用 JSON にする。
std::string CopySelectionJson(const GraphDocument& doc, const Selection& sel);

struct PasteResult
{
    Selection selection;   // 貼り付けた新しいノード / コメント
    int nodes = 0, edges = 0, comments = 0;
};
// クリップボード JSON を offset だけずらして貼り付ける。ID は再採番。1 回の Undo で全部戻る。
bool PasteJson(GraphDocument& doc, const std::string& json, Vec2 offset, PasteResult* out, std::string* error);
// クリップボード JSON の外接矩形（貼り付け位置の計算用）。中身が無ければ false。
bool ClipboardBounds(const GraphDocument& doc, const std::string& json, Rect& out);

} // namespace dx12e::ng
