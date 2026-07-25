#pragma once

#include <string>

// ImGuiTestEngine ベースの UI 自動テスト（エンジン診断）。エディタ UI を実際のクリック/
// キー入力として走らせ、クラッシュ・アサート・ハング・エラーログを炙り出す。
//
// 使い方は 2 通り:
//   1) エディタの「ツール > エンジン診断 (UI 自動テスト)」パネル … 誰でもボタン一発で実行できる。
//      配布版でも使えるので、ユーザー環境で不具合が出たときの切り分けに使う。
//      グループ（基本操作 / UIエディタ / パーティクル / マテリアル / 再生 / ビルド …）ごとに
//      個別実行もできる。
//   2) DX12Engine.exe --ui-tests-run-all … 全テストを自動実行し終了コードで結果を返す(0=全パス)。
//      ui_test_results.xml (JUnit) も出る。CI/バッチ検証用。
//      --ui-tests-speed=N で速度 (0=Fast[既定] 1=Normal 2=Cinematic)。
//
// テストエンジン自体はエディタ起動時に常に初期化される（テストを走らせない限り無害）。
//
// 検査はシーンを実際に書き換える（エンティティ生成・削除・Undo 等）ため、
// 実行前に現在のシーンを退避し、完了後に自動で復元する（SaveSceneSnapshot）。

struct ImGuiTestEngine;   // グローバル名前空間の型（imgui_test_engine 提供）

namespace dx12e
{

class Application;

class UiTestHarness
{
public:
    // ImGui コンテキスト生成後・最初の NewFrame より前に呼ぶ。
    // deepOnly=true なら自動実行の対象を超詳細診断だけに絞る（--ui-tests-deep）。
    void Initialize(Application* app, bool runAllAndExit, int speedMode, bool deepOnly = false);

    // 毎フレーム Present の後に呼ぶ（テストのコルーチンを進める）
    void PostRender();
    // 診断パネル（実行ボタン + 結果一覧 + ログ）を描画する。show=false なら何も出さない。
    // hoveredOut … この窓（と子窓）にカーソルがあるフレームは true を書き込む。呼び出し側は
    //              EditorContext::floatingToolWindowHoveredThisFrame を渡し、パネル上の
    //              ホイール操作が背後のシーンビューのカメラ操作へ漏れるのを防ぐ。
    void DrawDiagnosticsPanel(bool* show, bool* hoveredOut = nullptr);

    void Shutdown();

    bool IsActive() const { return m_engine != nullptr; }
    // --ui-tests-run-all 指定時: 全テストが終了したら true（呼び出し側がウィンドウを閉じる）
    bool WantsExit() const { return m_wantsExit; }
    // 全テスト結果（0=全パス）。WantsExit() が true になった後に読む
    int  ExitCode() const { return m_exitCode; }

private:
    void RegisterTests();
    void RefreshSummary();
    // filterCategory=nullptr で全件、"vfx" 等でそのグループだけ、failedOnly で前回失敗分だけ。
    // deepSel: -1=超詳細を問わない / 0=通常の検査だけ / 1=超詳細だけ
    void QueueTests(const char* filterCategory, bool failedOnly, int deepSel = -1);
    void BeginRun();
    std::string BuildReport() const;

    ::ImGuiTestEngine* m_engine = nullptr;
    Application* m_app = nullptr;
    bool m_runAllAndExit = false;
    bool m_deepOnly      = false;   // --ui-tests-deep: 自動実行を超詳細診断だけに絞る
    bool m_started       = false;
    bool m_wantsExit     = false;
    int  m_exitCode      = 0;

    // 診断パネル表示用の直近サマリ
    bool m_running       = false;
    int  m_lastTested    = 0;
    int  m_lastSuccess   = 0;
    int  m_queuedTotal   = 0;       // 今回の実行でキューに積んだ件数（進捗バーの分母）
    bool m_showEngineUi  = false;   // ImGuiTestEngine 純正の詳細窓（上級者向け）
    bool m_showPassedRows = true;   // 成功した項目も一覧に出すか（既定 ON）
    std::string m_statusText;
    std::string m_sceneSnapshot;    // 検査前に退避したシーン（完了後に復元して消す）
    bool m_restorePending = false;  // 復元を要求済み。ロードが消化されるまでファイルを消さない
    std::string m_savedScenePath;   // 検査前に開いていたシーンのパス（復元後に戻す）
    // 検査中に固定する診断パネルの位置（テストエンジンに脇へどかされないように）。
    // ヘッダを imgui 非依存に保つため ImVec2 ではなく float 2 本で持つ。
    float m_panelPosX = 0.0f;
    float m_panelPosY = 0.0f;
};

} // namespace dx12e
