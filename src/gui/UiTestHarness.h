#pragma once

#include <string>

// ImGuiTestEngine ベースの UI 自動テスト（エンジン診断）。エディタ UI を実際のクリック/
// キー入力として走らせ、クラッシュ・アサート・ハングを炙り出す。
//
// 使い方は 2 通り:
//   1) エディタの「ツール > エンジン診断 (UI 自動テスト)」パネル … 誰でもボタン一発で実行できる。
//      配布版でも使えるので、ユーザー環境で不具合が出たときの切り分けに使う。
//   2) DX12Engine.exe --ui-tests-run-all … 全テストを自動実行し終了コードで結果を返す(0=全パス)。
//      ui_test_results.xml (JUnit) も出る。CI/バッチ検証用。
//      --ui-tests-speed=N で速度 (0=Fast[既定] 1=Normal 2=Cinematic)。
//
// テストエンジン自体はエディタ起動時に常に初期化される（テストを走らせない限り無害）。

struct ImGuiTestEngine;   // グローバル名前空間の型（imgui_test_engine 提供）

namespace dx12e
{

class Application;

class UiTestHarness
{
public:
    // ImGui コンテキスト生成後・最初の NewFrame より前に呼ぶ
    void Initialize(Application* app, bool runAllAndExit, int speedMode);

    // 毎フレーム Present の後に呼ぶ（テストのコルーチンを進める）
    void PostRender();
    // 診断パネル（実行ボタン + 結果一覧 + ログ）を描画する。show=false なら何も出さない
    void DrawDiagnosticsPanel(bool* show);

    void Shutdown();

    bool IsActive() const { return m_engine != nullptr; }
    // --ui-tests-run-all 指定時: 全テストが終了したら true（呼び出し側がウィンドウを閉じる）
    bool WantsExit() const { return m_wantsExit; }
    // 全テスト結果（0=全パス）。WantsExit() が true になった後に読む
    int  ExitCode() const { return m_exitCode; }

private:
    void RegisterTests();
    void RefreshSummary();

    ::ImGuiTestEngine* m_engine = nullptr;
    Application* m_app = nullptr;
    bool m_runAllAndExit = false;
    bool m_started       = false;
    bool m_wantsExit     = false;
    int  m_exitCode      = 0;

    // 診断パネル表示用の直近サマリ
    bool m_running       = false;
    int  m_lastTested    = 0;
    int  m_lastSuccess   = 0;
    bool m_showEngineUi  = false;   // ImGuiTestEngine 純正の詳細窓（上級者向け）
    std::string m_statusText;
};

} // namespace dx12e
