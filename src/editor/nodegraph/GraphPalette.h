#pragma once

// ===== ノード検索パレットの検索ロジック（純ロジック。既存 editor/FuzzyMatch.h を流用）=====
// 絞り込み: 文字列（あいまい）・カテゴリ・「ドラッグ元ピンに繋がるノードだけ」。

#include "editor/nodegraph/IGraphModel.h"

#include <string>
#include <vector>

namespace dx12e::ng
{

struct PaletteFilter
{
    std::string query;       // 空白区切りの複数語（全語が一致）
    std::string category;    // 空 = 全部
    // ドラッグ元ピン（無ければ hasPin = false）。pinIsOutput = 出力ピンからドラッグ → 入力を持つノードだけ。
    bool     hasPin = false;
    bool     pinIsOutput = false;
    PinType  pinType = 0;
};

struct PaletteItem
{
    int      typeIndex = 0;          // IGraphModel::NodeTypes() の添字
    int      score = 0;
    int      autoPin = -1;           // 作ったあと自動で繋ぐ、新ノード側のピン添字（無ければ -1）
    std::vector<uint32_t> titleHighlight;   // タイトルの一致バイト位置（強調表示用）
};

// 検索。空クエリは全件（カテゴリ順 → タイトル順）。結果は score 降順。
std::vector<PaletteItem> SearchPalette(const IGraphModel& model, const PaletteFilter& f, size_t maxResults = 500);

// カテゴリ一覧（NodeTypes() に最初に現れた順・重複なし）。
std::vector<std::string> PaletteCategories(const IGraphModel& model);

} // namespace dx12e::ng
