// ============================================================================
// GraphAnalysis.h — 型推論 + 検証（診断）
//
//   MaterialGraph 全体を 1 パスで解決する。UI（ピン色・エラー縁）とコンパイラ（Compiler）が同じ結果を使う。
//   診断は「どのノードのどの入力が何故失敗か」を nodeId / pin / code / message で返す。
// ============================================================================
#pragma once

#include "renderer/matgraph/GraphModel.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

// 入力ピンの値の出どころ
struct PinSource
{
    enum class Kind : uint8_t
    {
        None,             // 未接続で既定も無い（Free の任意入力 / エラー）
        Link,             // 他ノードの出力
        Literal,          // ピンに直書きされた値
        Default,          // ピン宣言の既定値
        Builtin,          // 組み込み入力（uv など）
        ImplicitTexture,  // Tex 未接続 → ノードの texture プロパティ
    };
    Kind        kind = Kind::None;
    NodeId      node;                 // Link
    std::string pin;                  // Link: 接続元の出力ピン名
    int         outIndex = -1;        // Link: 接続元 NodeDef の出力ピン番号
    Value       value;                // Literal / Default
    std::string builtin;              // Builtin
    ValueType   srcType = ValueType::Invalid;   // 接続元の型（キャスト前）
    bool        broken = false;       // 診断済みの不正（以降のキャスト検査を飛ばす）
};

struct NodeTypeInfo
{
    const NodeDef*          def = nullptr;
    bool                    resolved = false;
    std::vector<PinSource>  inSource;   // 入力ピン順
    std::vector<ValueType>  inTypes;    // 入力ピン順。キャスト後の型（Invalid = 使えない / 無い）
    std::vector<ValueType>  outTypes;   // 出力ピン順
    ValueType               result = ValueType::Invalid;   // 出力変数の型
    int                     polyDim[4] = {1, 1, 1, 1};
    bool                    reachable = false;             // 出力ノードから到達できる
};

struct GraphAnalysis
{
    std::map<NodeId, NodeTypeInfo> nodes;
    std::vector<Diagnostic>        diags;
    std::vector<NodeId>            order;        // 出力ノードから到達できるノードの後順（トポロジカル順。出力ノードが最後）
    NodeId                         outputNode;   // 出力ノード（無い / 複数のときは先頭。無ければ空）
    bool                           hasErrors = false;   // 到達可能なノードに Error がある

    const NodeTypeInfo* Find(const NodeId& id) const
    {
        auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : &it->second;
    }
};

// パラメータ宣言（ScalarParameter / VectorParameter / TextureParameter ノードから作る一覧）。
// インスペクタのパラメータ表や、マテリアルインスタンスの上書き検証に使う。出力に繋がっていないものも含む
// （reachable で見分ける。スロットを持つのは reachable のものだけ）。
struct ParamDecl
{
    NodeId      nodeId;
    std::string name;
    std::string nodeType;     // "ScalarParameter" / "VectorParameter" / "TextureParameter"
    ValueType   type = ValueType::Invalid;   // F1 / F4 / Tex2D
    Json        defaultValue;                // 数値 / 配列 / テクスチャパス
    std::string group;
    int         priority = 0;
    std::string usage;                       // TextureParameter: "Color" / "LinearColor" / "Normal" / "Mask"
    bool        reachable = false;
};
std::vector<ParamDecl> CollectParameters(const MaterialGraph& g);   // (priority, group, name) 順

// 組み込み入力（Pin の defaultBuiltin）の型。未知なら Invalid。
ValueType BuiltinType(const std::string& name);

GraphAnalysis AnalyzeGraph(const MaterialGraph& g);

} // namespace dx12e::matgraph
