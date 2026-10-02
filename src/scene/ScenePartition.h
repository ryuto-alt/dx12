#pragma once

// シーンの分割保存（docs/SCENE_FORMAT_DESIGN.md §4.3）。GPU 不要の純関数＋ファイル入出力。
//
//   foo.json        … ルート設定 + "partition":{"cellSize":64} + "parts" 一覧 + どのセルにも属さないエンティティ
//   foo.parts/cell_<x>_<z>.json … 中身は v2 と同じ（正しい JSON・entities は 1 行 1 体）+ "seq"（全体の並び順）
//
// 割り当て: ルートエンティティ（親なし）のサブツリー単位で、ルートのワールド位置（XZ）が入るセルへ。
//   名前・guid・transform だけの「入れ物」（親だけの空エンティティ）は foo.json に置き、子を個別に割り当てる
//   （Dead Mall のように 1 つの空の親に 1.4 万体がぶら下がる形でも分かれるように）。
//   ライト・カメラ・UI・スクリプト付き・"partition":"root" の印のあるものは foo.json 側。
// 並び順: エンティティの配列の並び（= 生成順 = ヒエラルキーの並び・同点の描画順）は保存前後で変えない。
//   各セルファイルの "seq"（"0-2,10,15-16" の形）が、そのセルの各エンティティの全体での位置を持つ。
//   foo.json のエンティティは「どのセルも使っていない位置」を順に埋める。seq が壊れていたら
//   foo.json → セル（parts の並び）の順につなぐ（手書きのセルでも読める）。

#include <string>
#include <vector>
#include <functional>

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

namespace dx12e::scenepart
{

using json = nlohmann::json;

constexpr double kDefaultCellSize     = 64.0;
constexpr size_t kAdviceEntityThreshold = 5000;   // これ以上の体数で分割していないシーンを保存するとき、分割を勧める

// "cell_<x>_<z>.json"（負の値は "-" 付き）
std::string CellFileName(int x, int z);

// <シーンの絶対パス> → "<フォルダ>/<stem>.parts"
std::string PartsDirFor(const std::string& scenePath);

// ---- 割り当て（純関数）----
struct CellKey
{
    int x = 0, z = 0;
    bool operator<(const CellKey& o) const { return x != o.x ? x < o.x : z < o.z; }
    bool operator==(const CellKey& o) const { return x == o.x && z == o.z; }
};

struct Assignment
{
    std::vector<int> rootIdx;                                  // foo.json に残るエンティティの添字（昇順）
    std::vector<std::pair<CellKey, std::vector<int>>> cells;   // セルごとの添字（セルは (x,z) 昇順・添字は昇順）
};

// entities は BuildSceneJson / ConvertToV2 後の配列（guid / parentGuid / transform を見る）。cellSize <= 0 なら全部 root。
Assignment AssignCells(const json& entities, double cellSize);

// ---- 並び順（seq）----
std::string EncodeSeq(const std::vector<int>& sortedAscending);                 // 例 "0-2,10,15-16"
bool DecodeSeq(const std::string& s, size_t expectedCount, size_t total, std::vector<int>& out);

// ---- 保存 ----
struct PartText
{
    std::string name;     // "cell_0_0.json"
    CellKey     cell;
    size_t      count = 0;
    std::string text;
};
struct SplitOutput
{
    std::string           rootText;   // foo.json の本文（parts 一覧つき）
    std::vector<PartText> parts;
};

// root は ConvertToV2 済み（in-place に壊してよい）。entities を割り当てて文字列まで作る（セルの文字列化は並列）。
// cellSize <= 0 や 0 体のときは parts 無し（rootText は通常の v2 と同じ）。
void SplitAndDump(json&& root, double cellSize, SplitOutput& out);

struct WriteStats
{
    int    files = 0;        // セルファイルの数
    int    written = 0;      // 内容が変わって書いたファイル（foo.json 含む）
    int    unchanged = 0;    // 内容が同じなので触らなかったファイル
    int    removed = 0;      // 空になったセルとして消したファイル
    size_t maxBytes = 0;     // 最大のファイル（foo.json 含む）
    size_t totalBytes = 0;
    bool   ok = true;
};
// セルファイルを先に、最後に foo.json を書く（foo.json が「無いファイル」を指す瞬間を作らない）。内容が同じものには触らない。
// 今回のセルに無い cell_*.json は消す（フォルダが空になれば消す）。
WriteStats WriteSplit(const std::string& scenePath, const SplitOutput& out);

// 分割していないシーンの保存後の掃除: <stem>.parts/ の cell_*.json を消す（フォルダが空なら消す）。戻り値 = 消した数。
int RemoveParts(const std::string& scenePath);

// ---- 読み込み ----
// ファイル名の安全確認（フラットな名前だけ許す。".." / 区切り / ドライブ指定は不可）
bool IsSafePartName(const std::string& name);

// root（foo.json のパース結果）の "parts" が指すファイルの名前一覧（不正な項目は含めない）。
std::vector<std::string> PartNames(const json& root);

struct MergeStats
{
    size_t files = 0;
    unsigned threads = 1;
    double readMs = 0, parseMs = 0, mergeMs = 0;
    bool seqUsed = false;    // false = 並び順の印が無い / 壊れていて、つなぎ順にした
};
// 読み込み関数: パーツ名 → 本文。false で失敗。
using ReadPartFn = std::function<bool(const std::string& partName, std::string& bytes)>;

// root に "parts" があれば、全パーツを読んで（パースは並列）root["entities"] へ並び順どおりに統合し "parts" を消す。
// 補完（InflateScene）はしない（呼び出し側が全体に 1 回かける）。
// 戻り値 false = 読めない / 壊れたパーツがある（err に日本語の理由）。保存で黙って消えるのを避けるため部分読みはしない。
bool MergeParts(json& root, const ReadPartFn& read, MergeStats& stats, std::string& err);

// パース済みの root（絶対パス scenePath のシーン）に "parts" があれば、ディスクからセルを読んで統合する（pak は使わない）。
bool MergePartsFromDisk(json& root, const std::string& scenePath, std::string& err);

// 絶対パスのシーンファイル（ディスク）を読み、パーツもつなげた 1 つの JSON にする。補完はしない。
// --validate / ApplyOverrides / 外部ツール用。pak（vfs）は使わない。
bool ReadSceneFileMerged(const std::string& absPath, json& root, std::string& err);

// 渡した threads 数の上限（論理コアの 1/4 程度。最低 1）。
unsigned ThreadBudget();

} // namespace dx12e::scenepart
