#pragma once

#include "project/Project.h"
#include "project/LauncherLogic.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace dx12e
{

// ランチャーの結果。どのアクションが選ばれたか + 選ばれたプロジェクト情報。
enum class LauncherAction { None, OpenExisting, CreateNew, Skip };

// ランチャーの画面そのもの（ImGui）は editor/LauncherScreen.{h,cpp}。ここは「データと OS ダイアログ」の層。
class ProjectManager
{
public:
    // ---- 保存先 ----
    // エディタのユーザーデータ（最近のプロジェクト / editor_state.json）のフォルダ。
    // 既定は %APPDATA%\DX12Engine\。環境変数 DX12E_DATA_DIR があればそちらを使う
    // （検証・自動テスト用: 実ユーザーの最近のプロジェクト一覧を触らずに済む）。UTF-8。
    static std::string DataDir();

    // ---- 最近のプロジェクト ----
    // recent.json: {"recents":[{"name","path","lastOpened"(epoch秒),"pinned"}]}（旧形式は name/path だけ。互換）。
    static std::vector<launcher::RecentRecord> LoadRecents();          // 並び済み（ピン留め優先 → 新しい順）
    static void AddToRecents(const ProjectInfo& info);                  // 開いた/作った: 先頭へ・時刻更新（ピン状態は保つ）
    static bool RemoveFromRecents(const std::string& projectRoot);      // 一覧から外すだけ（ディスクは触らない）
    static bool SetRecentPinned(const std::string& projectRoot, bool pinned);

    // 旧 API（ProjectInfo の配列）。既存の呼び出し元向け。
    static std::vector<ProjectInfo> GetRecents();

    // ---- 開く ----
    // Open project via file dialog（既存プロジェクトを読み込む。即ロード）
    static bool OpenProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // 新規プロジェクトの作成先フォルダをモダンダイアログで選ぶ（旧: ランチャーのテンプレ選択後に使っていた）。
    // ★ディスクへの書き込みはしない（非同期で作成するため info を埋めるだけ）。
    static bool NewProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // モダンなフォルダ選択ダイアログ（IFileOpenDialog / FOS_PICKFOLDERS）。
    static bool PickFolder(HWND hwnd, std::string& outPath, const wchar_t* title);
    // .dx12proj を選ぶダイアログ（パスだけ返す。読み込みはしない）。UTF-8 の完全パス。
    static bool PickProjectFile(HWND hwnd, std::string& outPath);

    // フォルダから ProjectInfo を組み立てる（.dx12proj があれば読む、無ければ合成）。
    // --net-client の自動プロジェクトオープン等、ダイアログ無しで開きたい時用。
    static bool ProjectFromFolder(const std::string& dir, ProjectInfo& out);

    // 入力欄に貼られたパス（.dx12proj ファイル / プロジェクトのフォルダ）から ProjectInfo を組み立てる。
    // 失敗時は err に画面用の理由（標準語）を入れる。
    static bool ResolveProjectPath(const std::string& utf8Path, ProjectInfo& out, std::string& err);

    // ---- エディタ状態（editor_state.json。読んでマージして書く）----
    static void SaveLastOpenedScene(const std::string& scenePath);
    static std::string LoadLastOpenedScene();
    static bool        GetEditorBool(const char* key, bool def);
    static void        SetEditorBool(const char* key, bool value);
    static std::string GetEditorString(const char* key, const std::string& def);
    static void        SetEditorString(const char* key, const std::string& value);
    // 任意のトップレベルキー(オブジェクト)を JSON 文字列で読み書きする（他のキーは保つ。editor/EditorPrefs が "prefs" に使う）。
    static std::string LoadEditorSection(const char* key);                          // 無い/オブジェクトでなければ空文字
    static void        SaveEditorSection(const char* key, const std::string& jsonText);   // jsonText が壊れていれば何もしない

    // 現在時刻（epoch 秒）。
    static int64_t NowEpoch();

private:
    static std::string GetRecentsFilePath();
    static std::string GetEditorStatePath();
};

} // namespace dx12e
