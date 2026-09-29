#pragma once

// ===========================================================================
// プロジェクトランチャー（起動後のプロジェクト選択・作成画面）
// ---------------------------------------------------------------------------
// UE5 のプロジェクトブラウザ風: 左に縦ナビ / 中央にカードのギャラリー / 右に選択中の詳細パネル。
// 世界観はダーク + ネオングロー（起動画面と統一: アクセント #2F8CFF・ガラス風カード・ゆるやかな動き）。
//   最近      … サムネイル付きカード・検索・ピン留め・存在しないパスの警告・右に詳細
//   新規      … テンプレートのギャラリー + プレビュー + アプリ内フォーム（作成先をその場で検証）
//   開く      … .dx12proj / フォルダのパスを入れて開く（OS のダイアログは「参照」だけ）
//   Git       … URL から非同期クローン
//   ニュース  … 更新内容・バージョン・学習リンク
//
// 描画はすべて ImGui（ImDrawList）。純ロジック（検証・並び・検索・サムネイル・テンプレ表・補間）は
// project/LauncherLogic.h と project/LauncherMotion.h に分けてあり、tests/launcher_logic_test.cpp が検査する。
// 設計と手順: docs/LAUNCHER.md
// ===========================================================================

#include "project/ProjectManager.h"

#include <functional>
#include <string>

namespace dx12e
{

// 画像（PNG）を GPU へ上げたもの。id は ImTextureID（=ImU64）。0 = 読み込めなかった（呼び出し側はフォールバックを描く）。
struct LauncherImage
{
    unsigned long long id = 0;
    int w = 0, h = 0;
};

// 画面が Application から受け取る道具。
struct LauncherHost
{
    HWND        hwnd = nullptr;
    unsigned long long logo = 0;            // ロゴ（assets/editor/icons/logo.png）
    std::string assetsDir;                  // エディタ組み込みの assets/（末尾 '/'）
    // 画像ファイル（絶対パス・UTF-8）を読んで GPU へ上げる。同じファイルは呼び出し側でキャッシュする
    // （更新日時が変わったら別物として読み直す）。失敗は id=0。★ImGui のフレーム中に呼ばれる。
    std::function<LauncherImage(const std::string& absPath)> loadImage;
};

// 検証・自動テスト用の内部状態の覗き窓（UI 自動テストとレポート用。画面の動作には影響しない）。
struct LauncherDebugState
{
    int  tab = 0;                  // 0=最近 1=新規 2=開く 3=Git 4=ニュース
    int  templateIndex = 0;
    int  recentCount = 0;
    int  visibleRecentCount = 0;
    int  selectedRecent = -1;      // 表示中の一覧での添字（-1 = なし）
    bool animations = true;
    bool formHasError = false;
    bool formHasWarn = false;
    std::string formName;
    std::string formLocation;
    std::string formFirstIssueCode;
    std::string searchText;
    bool visible = false;          // 直前のフレームで描画された（ランチャーが表示中）
    int  renderedFrames = 0;       // Render が呼ばれた累計回数
};

class LauncherScreen
{
public:
    // 1 フレーム分の描画。OpenExisting / CreateNew / Skip のとき outInfo を埋めて返す。
    static LauncherAction Render(ProjectInfo& outInfo, const LauncherHost& host);

    // ---- 検証 / UI 自動テスト用 ----
    static LauncherDebugState Debug();
    static void DebugSetTab(int tab);                                 // 0..4
    static void DebugSetForm(const std::string& name, const std::string& location);   // 新規フォームへ入力（検証は次フレームで走る）
    static void DebugSelectTemplate(int index);
    static void DebugSetSearch(const std::string& text);
    static void DebugSetAnimations(bool on);                          // 保存はしない（画面の状態だけ）
    static void DebugReloadRecents();
    static void DebugRequestOpen(const std::string& projectPathOrFolder);   // 次のフレームでそのプロジェクトを開く（UI 自動テスト用）
};

} // namespace dx12e
