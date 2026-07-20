#include "gui/UiTestHarness.h"
#include "core/Logger.h"
#include "core/Version.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#include <imgui_te_ui.h>
#include <imgui_te_utils.h>
#include <imgui_te_exporters.h>
#pragma warning(pop)

#include <cstring>

namespace dx12e
{
namespace
{

// パネル名（UTF-8）。ImGui のウィンドウ名と一致させる必要がある
const char* kWinHierarchy = "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc";        // ヒエラルキー
const char* kWinInspector = "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc"; // インスペクター
const char* kBtnAddEntity = "\xe2\x9c\x9a \xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe8\xbf\xbd\xe5\x8a\xa0"; // ✚ エンティティ追加
const char* kBtnAddComponent = "\xe2\x9c\x9a \xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe8\xbf\xbd\xe5\x8a\xa0"; // ✚ コンポーネント追加
const char* kMenuScript = "\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88";  // スクリプト

// 集めた項目のうち、最後に生成されたエンティティ行（=リスト末尾側）を返す。
// ✚ ボタン等の非エンティティ項目は飛ばす。見つからなければ nullptr。
const ImGuiTestItemInfo* SelectLastItem(ImGuiTestItemList& items)
{
    for (int i = items.GetSize() - 1; i >= 0; --i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0 || item->Window == nullptr) continue;
        if (std::strstr(item->DebugLabel, "\xe2\x9c\x9a") != nullptr) continue;
        return item;
    }
    return nullptr;
}

// ヒエラルキーに並ぶエンティティ行を集めて、順にクリックする。
// 落ちる/固まるのはたいてい「選択が変わった次のフレームで Inspector が描く」瞬間なので、
// 1 件ごとに数フレーム回して Inspector 側の描画まで到達させる。
void ClickEveryHierarchyItem(ImGuiTestContext* ctx, int maxItems)
{
    ctx->SetRef(kWinHierarchy);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 3);

    int clicked = 0;
    for (int i = 0; i < items.GetSize(); ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (clicked >= maxItems) break;
        if (item == nullptr || item->ID == 0 || item->Window == nullptr) continue;
        // ✚ ボタン（エンティティ追加）はここでは押さない
        if (std::strstr(item->DebugLabel, "\xe2\x9c\x9a") != nullptr) continue;

        ctx->MouseMove(item->ID);
        ctx->MouseClick(0);
        ctx->Yield(3);          // Inspector の再描画まで進める
        ++clicked;
    }
    ctx->LogInfo("clicked %d hierarchy items", clicked);
}

// 現在フォーカス中のポップアップから、ラベルに needle を含む項目をクリックする。
// ItemClick はラベル中の '/' をパス区切りとして解釈するため、"UI Layout (VBox/HBox/Grid)"
// のようなスラッシュ入りラベルは名前指定では引けない。ID 直指定で回避する。
bool ClickPopupItemContaining(ImGuiTestContext* ctx, const char* needle)
{
    ctx->SetRef("//$FOCUSED");
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "", 2);
    for (int i = 0; i < items.GetSize(); ++i)
    {
        const ImGuiTestItemInfo* item = items[i];
        if (item == nullptr || item->ID == 0) continue;
        if (std::strstr(item->DebugLabel, needle) == nullptr) continue;
        ctx->MouseMove(item->ID);
        ctx->MouseClick(0);
        return true;
    }
    ctx->LogWarning("popup item not found: %s", needle);
    return false;
}

// ヒエラルキーの「✚ エンティティ追加」から種別を1つ足す
void AddEntity(ImGuiTestContext* ctx, const char* type)
{
    ctx->SetRef(kWinHierarchy);
    ctx->ItemClick(kBtnAddEntity);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    ctx->ItemClick(type);
    ctx->Yield(6);   // spawn はフレーム境界の遅延処理なので余分に回す
}

} // namespace

void UiTestHarness::Initialize(Application* app, bool runAllAndExit, int speedMode)
{
    m_app           = app;
    m_runAllAndExit = runAllAndExit;

    m_engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(m_engine);
    io.ConfigRunSpeed = (speedMode == 2) ? ImGuiTestRunSpeed_Cinematic
                      : (speedMode == 1) ? ImGuiTestRunSpeed_Normal
                                         : ImGuiTestRunSpeed_Fast;
    io.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    io.ConfigLogToTTY            = true;   // 失敗内容を標準出力へ（バッチ実行の解析用）
    io.ConfigLogToDebugger       = true;
    if (runAllAndExit)
    {
        // 実行結果(失敗テスト名+理由)を JUnit XML で残す。バッチ/CI から読む用
        io.ExportResultsFilename = "ui_test_results.xml";
        io.ExportResultsFormat   = ImGuiTestEngineExportFormat_JUnitXml;
    }
    io.ConfigSavedSettings       = false;   // テストで .ini を汚さない
    io.ConfigStopOnError         = false;   // 1件落ちても残りを走らせる（網羅優先）
    io.ConfigNoThrottle          = true;

    RegisterTests();
    ImGuiTestEngine_Start(m_engine, ImGui::GetCurrentContext());

    Logger::Info("UI テストエンジンを起動しました (run-all={})", runAllAndExit ? "yes" : "no");
}

void UiTestHarness::RefreshSummary()
{
    ImGuiTestEngineResultSummary summary{};
    ImGuiTestEngine_GetResultSummary(m_engine, &summary);
    m_lastTested  = summary.CountTested;
    m_lastSuccess = summary.CountSuccess;
}

void UiTestHarness::PostRender()
{
    if (!m_engine) return;
    ImGuiTestEngine_PostSwap(m_engine);

    // 診断パネルから走らせたテストの完了検知（--ui-tests-run-all でない通常起動時）
    if (m_running && ImGuiTestEngine_IsTestQueueEmpty(m_engine))
    {
        m_running = false;
        RefreshSummary();
        const bool allOk = (m_lastSuccess == m_lastTested);
        m_statusText = allOk
            ? "すべて成功しました。クラッシュ・異常は検出されませんでした。"
            : "失敗したテストがあります。下の一覧で赤い項目を確認してください。";
        Logger::Info("エンジン診断: {}/{} 成功", m_lastSuccess, m_lastTested);
    }

    if (!m_runAllAndExit) return;

    // 起動直後の 1 フレーム目でキューに積むと、まだパネルが出ておらず失敗するので数フレーム待つ
    static int warmup = 0;
    if (!m_started)
    {
        if (++warmup < 60) return;
        ImGuiTestEngine_QueueTests(m_engine, ImGuiTestGroup_Tests, nullptr);
        m_started = true;
        Logger::Info("UI テスト: 全テストをキューへ投入しました");
        return;
    }

    if (ImGuiTestEngine_IsTestQueueEmpty(m_engine))
    {
        ImGuiTestEngineResultSummary summary{};
        ImGuiTestEngine_GetResultSummary(m_engine, &summary);
        Logger::Info("UI テスト完了: {}/{} 成功", summary.CountSuccess, summary.CountTested);

        // 詳細(失敗テスト名と理由)は ExportResultsFilename の JUnit XML に出力される
        m_exitCode = (summary.CountSuccess == summary.CountTested) ? 0 : 1;
        m_wantsExit = true;
    }
}

void UiTestHarness::DrawDiagnosticsPanel(bool* show)
{
    if (!m_engine) return;

    // 上級者向け: ImGuiTestEngine 純正の詳細窓（個別実行・ログ・キャプチャ）
    if (m_showEngineUi)
        ImGuiTestEngine_ShowTestEngineWindows(m_engine, &m_showEngineUi);

    if (show == nullptr || !*show) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("エンジン診断", show))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "エディタの UI を自動操作して、クラッシュやフリーズが起きないか検査します。"
        "不具合が出たときは、この検査を実行して結果を開発者に伝えてください。");
    ImGui::Spacing();
    ImGui::TextDisabled("検査中はマウス/キーボードが自動で動きます（数十秒）。触らずに待ってください。");
    ImGui::Separator();

    ImGui::BeginDisabled(m_running);
    if (ImGui::Button("検査を実行", ImVec2(150, 34)))
    {
        ImGuiTestEngine_QueueTests(m_engine, ImGuiTestGroup_Tests, nullptr);
        m_running    = true;
        m_statusText = "検査を実行しています...";
        Logger::Info("エンジン診断: 検査を開始しました");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("結果をコピー", ImVec2(130, 34)))
    {
        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(m_engine, &tests);
        std::string report = "DX12 Engine 診断結果\n";
        report += "エンジン版: v" + std::string(kEngineVersion) + "\n";
        report += "結果: " + std::to_string(m_lastSuccess) + "/" + std::to_string(m_lastTested) + " 成功\n";
        for (int i = 0; i < tests.Size; ++i)
        {
            ImGuiTest* test = tests[i];
            if (test == nullptr) continue;
            const char* st = (test->Output.Status == ImGuiTestStatus_Success) ? "OK  "
                           : (test->Output.Status == ImGuiTestStatus_Error)   ? "NG  "
                                                                             : "--  ";
            report += std::string(st) + test->Name + "\n";
        }
        ImGui::SetClipboardText(report.c_str());
        m_statusText = "結果をクリップボードにコピーしました。";
    }

    ImGui::SameLine();
    ImGui::Checkbox("詳細ウィンドウ", &m_showEngineUi);

    if (m_running)
    {
        ImGui::Spacing();
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "検査中...");
    }

    if (!m_statusText.empty())
    {
        ImGui::Spacing();
        const bool bad = (m_lastTested > 0 && m_lastSuccess != m_lastTested);
        ImGui::TextColored(bad ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.45f, 0.9f, 0.5f, 1.0f),
                           "%s", m_statusText.c_str());
    }

    ImGui::Separator();
    ImGui::Text("検査項目");
    if (ImGui::BeginTable("##diagtests", 2,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("項目", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("結果", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(m_engine, &tests);
        for (int i = 0; i < tests.Size; ++i)
        {
            ImGuiTest* test = tests[i];
            if (test == nullptr) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(test->Name);
            ImGui::TableNextColumn();
            switch (test->Output.Status)
            {
            case ImGuiTestStatus_Success:
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f), "OK");
                break;
            case ImGuiTestStatus_Error:
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "失敗");
                break;
            case ImGuiTestStatus_Running:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "実行中");
                break;
            default:
                ImGui::TextDisabled("未実行");
                break;
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void UiTestHarness::Shutdown()
{
    if (!m_engine) return;
    ImGuiTestEngine_Stop(m_engine);
    // DestroyContext は ImGui::DestroyContext() の後に呼ぶ必要があるが、
    // 本エンジンでは ImGuiManager::Shutdown より先にここが呼ばれるため、
    // エンジン破棄はプロセス終了に任せる（リークしても即終了なので実害なし）。
    m_engine = nullptr;
}

void UiTestHarness::RegisterTests()
{
    ImGuiTestEngine* e = m_engine;

    // --- 1. ヒエラルキーの全エンティティを順にクリック ---
    // 報告のあった「ヒエラルキーのオブジェクトを押すとクラッシュ」の再現枠
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "hierarchy_click_all");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            ClickEveryHierarchyItem(ctx, 40);
        };
    }

    // --- 2. Empty を足して選択 → スクリプトを付ける ---
    // 「新規 Empty にスクリプト追加ボタンでクラッシュ」の再現枠
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "empty_attach_script");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            AddEntity(ctx, "Empty");

            // 生成された Empty を選択（末尾に追加される）
            ctx->SetRef(kWinHierarchy);
            ImGuiTestItemList items;
            ctx->GatherItems(&items, "", 3);
            if (const ImGuiTestItemInfo* last = SelectLastItem(items))
            {
                ctx->MouseMove(last->ID);
                ctx->MouseClick(0);
                ctx->Yield(4);
            }

            // Inspector → ✚ コンポーネント追加 → スクリプト サブメニューを開く
            ctx->SetRef(kWinInspector);
            ctx->ItemClick(kBtnAddComponent);
            ctx->Yield(2);
            ctx->SetRef("//$FOCUSED");
            ctx->ItemClick(kMenuScript);
            ctx->Yield(4);           // ScanScriptComponents + ParsePropertySchema が走る
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield(2);
        };
    }

    // --- 3. Empty に全コンポーネントを片端から足す ---
    // 「コンポーネント追加でクラッシュ」の再現枠。追加ごとに Inspector が
    // そのコンポーネントの編集 UI を描くので、レイアウト不整合があれば即死する
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "empty_add_all_components");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            AddEntity(ctx, "Empty");
            ctx->SetRef(kWinHierarchy);
            ImGuiTestItemList items;
            ctx->GatherItems(&items, "", 3);
            if (const ImGuiTestItemInfo* last = SelectLastItem(items))
            {
                ctx->MouseMove(last->ID);
                ctx->MouseClick(0);
                ctx->Yield(4);
            }

            const char* comps[] = {
                "Point Light", "Directional Light", "Spot Light", "Camera",
                "Audio Source", "Gimmick", "Particle Emitter", "Trail Renderer", "Trigger",
                "UI Canvas", "UI Rect", "UI Image", "UI Text", "UI Button",
                "UI Slider", "UI Toggle", "UI Scroll View", "UI Layout", "UI Animator",
                "RigidBody", "Box Collider", "Sphere Collider", "Capsule Collider",
                "Character Controller", "Network Identity", "Network Transform",
            };
            for (const char* comp : comps)
            {
                ctx->SetRef(kWinInspector);
                ctx->ItemClick(kBtnAddComponent);
                ctx->Yield(2);
                // ラベルに '/' を含むものがあるため、名前ではなくラベル一致 → ID でクリックする
                if (!ClickPopupItemContaining(ctx, comp))
                    ctx->KeyPress(ImGuiKey_Escape);
                ctx->Yield(4);   // 追加直後の Inspector 再描画まで進める
            }
        };
    }

    // --- 4. Inspector の全セクションを開閉してウィジェットを総なめ ---
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "inspector_open_all_sections");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            ctx->SetRef(kWinHierarchy);
            ImGuiTestItemList items;
            ctx->GatherItems(&items, "", 3);
            for (int i = 0; i < items.GetSize() && i < 12; ++i)
            {
                const ImGuiTestItemInfo* item = items[i];
                if (item == nullptr || item->ID == 0) continue;
                ctx->MouseMove(item->ID);
                ctx->MouseClick(0);
                ctx->Yield(3);
                ctx->SetRef(kWinInspector);
                ctx->ItemOpenAll("", 2);   // 折りたたみを全部開く＝全編集 UI を描かせる
                ctx->Yield(3);
                ctx->SetRef(kWinHierarchy);
            }
        };
    }

    // --- 5. エンティティ種別を一通り生成して、都度選択する ---
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "spawn_all_entity_types");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            const char* types[] = { "Box", "Sphere", "Plane", "Empty", "Camera",
                                    "Directional Light", "Point Light", "Spot Light" };
            for (const char* type : types)
            {
                AddEntity(ctx, type);
                ctx->SetRef(kWinHierarchy);
                ImGuiTestItemList items;
                ctx->GatherItems(&items, "", 3);
                if (const ImGuiTestItemInfo* last = SelectLastItem(items))
                {
                    ctx->MouseMove(last->ID);
                    ctx->MouseClick(0);
                    ctx->Yield(4);
                }
            }
        };
    }

    // --- 6. 生成 → Undo/Redo を往復（遅延処理とスナップショット復元の耐性） ---
    {
        ImGuiTest* t = IM_REGISTER_TEST(e, "editor", "undo_redo_stress");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            AddEntity(ctx, "Box");
            AddEntity(ctx, "Empty");
            ctx->SetRef(kWinHierarchy);
            for (int i = 0; i < 6; ++i)
            {
                ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
                ctx->Yield(3);
            }
            for (int i = 0; i < 6; ++i)
            {
                ctx->KeyPress(ImGuiKey_Y | ImGuiMod_Ctrl);
                ctx->Yield(3);
            }
        };
    }
}

} // namespace dx12e
