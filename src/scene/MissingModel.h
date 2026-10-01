#pragma once

#include <string>

namespace dx12e
{

// モデルが読み込めなかったエンティティの「描画まわりの元データ」。
//
// ★以前はモデルの読み込みに失敗するとエンティティごと捨てていた（InstantiateEntityJson が null を返す）。
//   ファイル名を変えた・フォルダを移した・別プロジェクトの文脈で復元された、などで 1 度でも読めないと、
//   次の保存（自動保存を含む）で**そのエンティティがシーンから黙って消えていた**。
//   今はエンティティ本体（名前・Transform・物理・スクリプト等）は作り、描画まわりのキー
//   （meshRenderer / material / shader ...）だけを元の JSON のまま持っておいて、保存時にそのまま書き戻す。
//   モデルが戻れば次に開いたとき普通に描かれる。
struct MissingModel
{
    std::string modelPath;     // assets 相対（Inspector / ログ表示用）
    std::string rendererJson;  // 描画まわりのキーだけを集めた JSON オブジェクト（dump 済み）
};

} // namespace dx12e
