#pragma once

// シーンファイル形式 v2 の純関数群（GPU 不要）。仕様の正本は docs/SCENE_FORMAT_DESIGN.md §3。
//
//   normalize … float32 で正確に表せる double を最短の float 表記へ（値は 1 ビットも変わらない）
//   strip     … エンティティから「凍結の既定値表と完全一致するフィールド」を消す（保存時）
//   inflate   … 表から欠けたフィールドを足す（読み込み時。補完後は v1 保存と同じ JSON になる）
//   dump      … ルート設定は整形・entities は 1 行 1 体
//
// 既定値表は src/scene/scene_defaults_v2.json（凍結データ。ビルド時にこの .cpp へ埋め込む）。
// 構造体の既定値をあとで変えても表は変えない。変えたいときは v3 を新設する。

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <string>

namespace dx12e::scenefmt
{

constexpr int kSceneVersionV2 = 2;

// 凍結の既定値表（正規化済み）。埋め込みデータを 1 回だけパースして使い回す。
const nlohmann::json& DefaultsV2();

// 数値ノードを再帰で走査し、float32 で正確に表せる double を最短の float 表記へ置き換える。
void NormalizeFloats(nlohmann::json& j);

// 1 つの double に対する同じ処理（テスト用に公開）。float32 で正確でなければそのまま返す。
double NormalizeDouble(double v);

// 保存側: エンティティ 1 体から表と一致するフィールドを消す。ej は normalize 済みであること。
// 全部消えたコンポーネントは {} のまま残す（キーの存在＝コンポーネントがある）。入れ子は再帰しない。
void StripDefaults(nlohmann::json& ej, const nlohmann::json& table);

// 読み込み側: 表にあって欠けているフィールドを足す。
void InflateDefaults(nlohmann::json& ej, const nlohmann::json& table);

// ルート JSON の version が 2 以上か（"version" が無い / 整数でないときは false = v1）。
bool IsV2(const nlohmann::json& root);

// version >= 2 のとき root["entities"] の全エンティティを補完する。戻り値 = 補完したか。
bool InflateScene(nlohmann::json& root);

// BuildSceneJson が作った v1 の完全形ルートを v2 へ変換する（in-place）:
// float 正規化 / 各エンティティの strip / parentGuid があれば parent を消す / version=2。
void ConvertToV2(nlohmann::json& root);

// v2 の整形で文字列化する。version → 他のルート設定（辞書順・dump(2) 相当）→ entities（1 行 1 体）の順。
// 末尾改行あり・改行は \n。root["entities"] が配列でなければ全体を dump(2) で返す。
std::string DumpSceneV2(const nlohmann::json& root);

} // namespace dx12e::scenefmt
