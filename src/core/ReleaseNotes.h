#pragma once

// ===========================================================================
// 更新内容（リリースノート）の「唯一の正」。
//   データは ReleaseNotesData.inc（版ごとの構造化データ。新しい版が先頭）。
//   同じデータから次を作る。文言を直すのはデータ 1 か所だけ:
//     1) 更新後の初回起動の「更新内容」画面（editor/WhatsNewScreen）
//     2) ランチャーのお知らせ欄
//     3) GitHub リリースの本文（DX12Engine.exe --write-release-notes <path>。ToMarkdown）
//     4) 次の更新から古い exe が見る「更新の案内」窓は GitHub 本文を表示するので、3) の書式がそのまま案内になる
//   標準ライブラリだけに依存する（単体テスト tests/update_ux_test.cpp が直接リンクする）。
//
// 新しい版を出すとき: ReleaseNotesData.inc の先頭へ版を足し、Version.cpp の kEngineVersion を同じ値にする
//   （tests の UpdateUxTests が「先頭の版 == kEngineVersion」と整合を検査する）。
// ===========================================================================

#include <string>
#include <vector>

namespace dx12e::relnotes
{

enum class Kind { Feature, Improvement, Fix };

struct Highlight
{
    std::string title;    // カードの題
    std::string body;     // カードの本文（1〜2 行）
    std::string detail;   // 補足（GitHub 本文とカードのツールチップ。空可）
    std::string area;     // 分野 ID（AreaLabel 参照）
};

struct Item
{
    Kind        kind = Kind::Feature;
    std::string title;
    std::string body;     // 空可
};

struct Release
{
    std::string version;   // "2.0.0"（v なし）
    std::string date;      // "2026-10-01"
    std::string headline;  // 1 行の見出し
    std::vector<Highlight> highlights;
    std::vector<Item>      items;
};

// 全リリース。新しい版が先頭。
const std::vector<Release>& All();
const Release* Find(const std::vector<Release>& all, const std::string& version);

// ---- 版 ----
// "v1.2.3" / "1.2.3" を 3 要素へ。数字が 1 つも無ければ false。
bool ParseVersion(const std::string& s, int out[3]);
int  CompareVersions(const std::string& a, const std::string& b);   // a<b:-1 / 等:0 / a>b:+1
bool IsNewer(const std::string& a, const std::string& b);           // a が b より新しい

// 前回表示した版 from → 今の版 to の間（from < v <= to）を新しい順に。
//   from が空 / 解釈できない / to 以上（＝初回インストール・同版・ダウングレード）なら to の版だけ。
std::vector<const Release*> Range(const std::vector<Release>& all, const std::string& from, const std::string& to);
// to 以下の全履歴（新しい順。手動で「更新内容を表示」したとき）。
std::vector<const Release*> History(const std::vector<Release>& all, const std::string& to);

// ---- 表示用 ----
const char* KindLabel(Kind k);                       // 新機能 / 改善 / 修正
int  CountKind(const Release& r, Kind k);
const char* AreaLabel(const std::string& area);      // 分野名（不明は「全般」）
bool IsKnownArea(const std::string& area);

// ---- 整合チェック（空 = 問題なし）----
std::vector<std::string> Validate(const std::vector<Release>& all);

// ---- GitHub リリース本文（Markdown）----
std::string ToMarkdown(const Release& r, const std::string& engineName);

} // namespace dx12e::relnotes
