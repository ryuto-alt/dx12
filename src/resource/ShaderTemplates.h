#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dx12e::shadertemplates
{

// カスタムシェーダーの雛形カタログ。
//
// ■ なぜ要るか
//   カスタムシェーダーは「何が使えるか」を知らないと書けない。cbuffer はオフセットで
//   対応が決まるので、宣言の順序を 1 つ間違えると **エラーは出ずに値だけが化ける**。
//   人間はドキュメントを読めばよいが、MCP 越しの AI には読ませる先が要る。
//   そこで
//     list_shader_templates    … 動く雛形の一覧（白紙から書かせない）
//     describe_shader_contract … 使える定数/テクスチャ/入出力の表
//   の 2 つで「土台」を配る。実体は shaders-src/templates/*.hlsl なので、
//   雛形を足したいときは .hlsl を置いて kBuiltin に 1 行足すだけでよい。
struct Info
{
    std::string name;      // create_shader の template に渡す識別子
    std::string title;     // 人が読む名前
    std::string summary;   // 何ができるか（1 行）
    std::string file;      // shaders-src/templates/ 配下のファイル名
};

// 同梱テンプレートの一覧。
const std::vector<Info>& List();

// name の雛形を読んで outCode に入れる。見つからなければ false。
bool Load(const std::string& name, std::string& outCode);

// kind = "mesh" / "sprite" / "screen"。
// そのシェーダー種別で使える定数・テクスチャ・入出力・注意点を JSON で返す。
nlohmann::json DescribeContract(const std::string& kind);

} // namespace dx12e::shadertemplates
