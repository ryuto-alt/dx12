#include "editor/panels/ToolbarPanel.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "scripting/ScriptEngine.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "core/Window.h"
#include "core/Logger.h"
#include "project/ProjectManager.h"

#include <commdlg.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace
{
void OpenInVSCode(const std::string& filePath)
{
    static std::string cachedExe;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        char appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData)))
        {
            namespace fs = std::filesystem;
            fs::path candidate = fs::path(appData) / "Programs" / "Microsoft VS Code" / "Code.exe";
            if (fs::exists(candidate))
                cachedExe = candidate.string();
        }
        if (cachedExe.empty())
        {
            namespace fs = std::filesystem;
            const char* dirs[] = {"C:\\Program Files\\Microsoft VS Code\\Code.exe",
                                  "C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe"};
            for (const char* p : dirs)
                if (fs::exists(p)) { cachedExe = p; break; }
        }
    }

    if (!cachedExe.empty())
    {
        std::string cmdLine = "\"" + cachedExe + "\" \"" + filePath + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr,
                           FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return;
        }
    }

    ShellExecuteA(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // anonymous namespace

namespace dx12e
{

void ToolbarPanel::Render(bool isPlaying,
                          EditorContext& ctx,
                          bool& outModeChangeRequested,
                          bool& outPendingPlayMode,
                          ScriptEngine* scriptEngine,
                          GameClock* clock,
                          Scene* scene,
                          Window* window,
                          AudioSystem* /*audioSystem*/,
                          const std::string& assetsDir,
                          f32 toolbarHeight)
{
    f32 displayW = ImGui::GetIO().DisplaySize.x;

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displayW, toolbarHeight), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dx12e::theme::Chrome);  // Nebula のクロム色
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);

    // ===== メニューバー（左上から横並び: ファイル / 編集 / 表示 / ツール / ヘルプ）=====
    bool openShortcutsPopup = false;
    bool openAboutPopup     = false;
    if (ImGui::BeginMenuBar())
    {
        // ---- ファイル ----
        if (ImGui::BeginMenu("ファイル"))
        {
            if (ImGui::MenuItem("新規シーン", "Ctrl+N"))
            {
                ctx.showNewSceneDialog = true;
                ctx.newSceneDialogIsCreate = true;
                std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
                strncpy_s(ctx.newSceneNameBuf, "NewScene", _TRUNCATE);
            }

            if (ImGui::MenuItem("シーンを開く", "Ctrl+O"))
            {
                char loadPath[MAX_PATH] = "";
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = window->GetHwnd();
                ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
                ofn.lpstrFile = loadPath;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                std::string initDir = assetsDir + "scenes";
                std::filesystem::create_directories(initDir);
                ofn.lpstrInitialDir = initDir.c_str();
                if (GetOpenFileNameA(&ofn))
                    ctx.pendingLoadPath = loadPath;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("保存", "Ctrl+S"))
            {
                if (ctx.currentScenePath.empty())
                {
                    char savePath[MAX_PATH] = "";
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = window->GetHwnd();
                    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
                    ofn.lpstrFile = savePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrDefExt = "json";
                    ofn.Flags = OFN_OVERWRITEPROMPT;
                    std::string initDir = assetsDir + "scenes";
                    std::filesystem::create_directories(initDir);
                    ofn.lpstrInitialDir = initDir.c_str();
                    if (GetSaveFileNameA(&ofn))
                        ctx.currentScenePath = savePath;
                }
                if (!ctx.currentScenePath.empty())
                {
                    SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
                    ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                    ctx.hotReloadFlash = 1.5f;
                }
            }

            if (ImGui::MenuItem("名前を付けて保存"))
            {
                char savePath[MAX_PATH] = "";
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = window->GetHwnd();
                ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
                ofn.lpstrFile = savePath;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrDefExt = "json";
                ofn.Flags = OFN_OVERWRITEPROMPT;
                std::string initDir = assetsDir + "scenes";
                std::filesystem::create_directories(initDir);
                ofn.lpstrInitialDir = initDir.c_str();
                if (GetSaveFileNameA(&ofn))
                {
                    ctx.currentScenePath = savePath;
                    SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
                    ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                    ctx.hotReloadFlash = 1.5f;
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("新規スクリプト", "Ctrl+L"))
            {
                ctx.showNewScriptDialog = true;
                std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
                strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
            }

            ImGui::EndMenu();
        }

        // ---- 編集 ----
        if (ImGui::BeginMenu("編集"))
        {
            if (ImGui::MenuItem("元に戻す", "Ctrl+Z", false, ctx.undoSystem.CanUndo()))
                ctx.pendingUndo = true;
            if (ImGui::MenuItem("やり直す", "Ctrl+Y", false, ctx.undoSystem.CanRedo()))
                ctx.pendingRedo = true;

            ImGui::Separator();

            const bool hasSel = ctx.HasSelection();
            if (ImGui::MenuItem("コピー", "Ctrl+C", false, hasSel))
            {
                ctx.clipboard.clear();
                auto& reg = scene->GetRegistry();
                for (auto e : ctx.selectedEntities)
                {
                    if (!reg.valid(e)) continue;
                    std::string snap = SceneSerializer::SerializeEntity(*scene, e, assetsDir);
                    if (!snap.empty())
                        ctx.clipboard.push_back(std::move(snap));
                }
            }
            if (ImGui::MenuItem("貼り付け", "Ctrl+V", false, !ctx.clipboard.empty()))
                ctx.pendingPastes = ctx.clipboard;
            if (ImGui::MenuItem("複製", "Ctrl+D", false, hasSel))
            {
                for (auto e : ctx.selectedEntities)
                    ctx.pendingDuplications.push_back(e);
            }
            if (ImGui::MenuItem("削除", "Del", false, hasSel))
            {
                for (auto e : ctx.selectedEntities)
                    ctx.pendingDeletions.push_back(e);
            }

            ImGui::EndMenu();
        }

        // ---- 表示 ----
        if (ImGui::BeginMenu("表示"))
        {
            if (ImGui::MenuItem("レイアウトをリセット"))
                ctx.resetLayout = true;
            ImGui::Separator();
            ImGui::TextDisabled("ツール窓（右下に開く）");
            ImGui::MenuItem("Post Process",            nullptr, &ctx.showPostProcess);
            ImGui::MenuItem("Post Process パラメータ",  nullptr, &ctx.showPostParams);
            ImGui::MenuItem("Skybox / IBL",            nullptr, &ctx.showSkybox);
            ImGui::MenuItem("SSAO",                    nullptr, &ctx.showSSAO);
            ImGui::MenuItem("エンジン設定",            nullptr, &ctx.showEngineSettings);
            ImGui::MenuItem("Scene Flow",              nullptr, &ctx.showSceneFlow);
            ImGui::MenuItem("Project",                 nullptr, &ctx.showProject);
            ImGui::MenuItem("Git 変更",                nullptr, &ctx.showVersionControl);
            ImGui::EndMenu();
        }

        // ---- ツール ----
        if (ImGui::BeginMenu("ツール"))
        {
            if (ImGui::MenuItem("ゲームをビルド"))
                ctx.pendingBuildGame = true;
            ImGui::MenuItem("MCP / AI Bridge",         nullptr, &ctx.showMcpBridge);
            ImGui::EndMenu();
        }

        // ---- ヘルプ ----
        if (ImGui::BeginMenu("ヘルプ"))
        {
            if (ImGui::MenuItem("ショートカット一覧"))
                openShortcutsPopup = true;
            if (ImGui::MenuItem("バージョン情報"))
                openAboutPopup = true;
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
    if (openShortcutsPopup) ImGui::OpenPopup("ショートカット一覧##ShortcutsPopup");
    if (openAboutPopup)     ImGui::OpenPopup("バージョン情報##AboutPopup");

    // ツールバー共通ヘルパ: 1つのボタンに「アイコン＋ラベル」をまとめて出す。
    // これで「どれがどれか分からない」を解消する（絵と言葉の両方で示す）。
    // tex=0 でアイコン省略、label=nullptr/"" でラベル省略。active=true で青ハイライト。
    const EditorUiIcons* ic = ctx.icons;
    const float kIconSz = 18.0f;
    const ImVec4 kActiveCol(0.26f, 0.42f, 0.78f, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    auto toolButton = [&](u64 tex, const char* label, const char* tip, bool active) -> bool
    {
        ImGui::PushID(tip ? tip : label);
        const ImGuiStyle& st = ImGui::GetStyle();
        ImVec2 labelSz = (label && *label) ? ImGui::CalcTextSize(label) : ImVec2(0, 0);
        float gap      = (tex && label && *label) ? 6.0f : 0.0f;
        float contentW = (tex ? kIconSz : 0.0f) + gap + labelSz.x;
        float contentH = (std::max)(tex ? kIconSz : 0.0f, labelSz.y);
        ImVec2 sz(contentW + st.FramePadding.x * 2.0f, contentH + st.FramePadding.y * 2.0f);

        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        kActiveCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.50f, 0.88f, 1.0f));
        }
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::Button("##tb", sz);
        if (active) ImGui::PopStyleColor(2);
        if (tip && *tip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = p0.x + st.FramePadding.x;
        float cy = p0.y + sz.y * 0.5f;
        if (tex)
        {
            dl->AddImage(static_cast<ImTextureID>(tex),
                         ImVec2(cx, cy - kIconSz * 0.5f),
                         ImVec2(cx + kIconSz, cy + kIconSz * 0.5f));
            cx += kIconSz + gap;
        }
        if (label && *label)
            dl->AddText(ImVec2(cx, cy - labelSz.y * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), label);
        ImGui::PopID();
        return clicked;
    };
    // グループ間の縦区切り（詰め込み感を無くすため各ツール群の間に入れる）。
    auto groupSep = [&]()
    {
        ImGui::SameLine(0, 12);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1, 1, 1, 0.16f), "|");
        ImGui::SameLine(0, 12);
    };

    // ===== ブランド（ロゴ + ワードマーク + バージョンチップ。Nebula の左上に倣う）=====
    {
        if (ic && ic->logo)
        {
            const float kLogoSz = 22.0f;
            ImGui::Image(static_cast<ImTextureID>(ic->logo), ImVec2(kLogoSz, kLogoSz));
            ImGui::SameLine(0, 8);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextHi);
        ImGui::TextUnformatted("DX12 Engine");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_Button,        dx12e::theme::GroupBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dx12e::theme::GroupBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  dx12e::theme::GroupBg);
        ImGui::PushStyleColor(ImGuiCol_Text,          dx12e::theme::TextDim);
        ImGui::SmallButton("v0.9");
        ImGui::PopStyleColor(4);
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, 12);
    }

    // ===== Play/Stop =====（メニューバー下の先頭。緑=再生 / 赤=停止で色分け）
    if (!isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.66f, 0.34f, 1.0f));
        if (toolButton(ic ? ic->play : 0, "再生", "Play  再生（プレイモードへ）", false))
        {
            // game.lua は任意（各エンティティのスクリプトコンポーネントが動くため）。
            // 旧来の「scripts/game.lua が無いと再生不可」警告は廃止し、そのまま再生する。
            outPendingPlayMode = true;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.30f, 0.30f, 1.0f));
        if (toolButton(ic ? ic->stop : 0, "停止", "Stop  停止（エディタへ戻る）", false))
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }

    // ===== Status =====
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    if (isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
        ImGui::Text("\xe2\x97\x8f \xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe4\xb8\xad");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf");
    }

    // Lua error
    if (scriptEngine && !scriptEngine->GetLastError().empty())
    {
        ImGui::SameLine(0, 16);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("\xe2\x9a\xa0 Lua Error");
        ImGui::PopStyleColor();
    }

    // Hot reload flash
    if (ctx.hotReloadFlash > 0.0f)
    {
        ImGui::SameLine(0, 12);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, ctx.hotReloadFlash));
        ImGui::Text("\xe2\x9c\x93 Saved");
        ImGui::PopStyleColor();
        ctx.hotReloadFlash -= clock->GetDeltaTime();
    }

    // Error popup (中央モーダル)
    if (ctx.errorFlash > 0.0f)
    {
        ctx.errorFlash = 0.0f;  // フラグをリセット（トリガー用のみ）
        ImGui::OpenPopup("##ErrorPopup");
    }

    // ポップアップの最小サイズを設定
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(500, 300));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.15f, 0.15f, 0.18f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.35f, 0.35f, 0.6f));

    if (ImGui::BeginPopupModal("##ErrorPopup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar))
    {
        // ウィンドウ中央に配置
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetWindowPos(ImVec2(center.x - ImGui::GetWindowWidth() * 0.5f,
                                    center.y - ImGui::GetWindowHeight() * 0.5f));

        // 警告アイコン（大）
        ImGui::PushFont(nullptr);  // デフォルトフォント
        ImGui::SetWindowFontScale(2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::Text("\xe2\x9a\xa0");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        ImGui::SameLine();

        // タイトル
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::SetWindowFontScale(1.3f);
        ImGui::Text("Play \xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93");  // Playできません
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // メッセージ本文（選択・コピー可能な InputText）
        ImGui::SetWindowFontScale(1.1f);
        static char errorBuf[512] = {};
        strncpy_s(errorBuf, ctx.errorMessage.c_str(), _TRUNCATE);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
        ImGui::InputTextMultiline("##ErrorMsg", errorBuf, sizeof(errorBuf),
            ImVec2(-1, ImGui::GetTextLineHeight() * 3.5f),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Spacing();

        // コピー + OK ボタン
        float totalWidth = 120.0f + 8.0f + 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        // コピーボタン
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.3f, 1.0f));
        if (ImGui::Button("\xf0\x9f\x93\x8b Copy", ImVec2(120.0f, 32.0f)))
        {
            ImGui::SetClipboardText(ctx.errorMessage.c_str());
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8.0f);

        // OK ボタン
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.25f, 0.25f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("OK", ImVec2(120.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::PopStyleVar();

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // ===== ギズモ（移動 / 回転 / 拡縮 / 空間）=====
    groupSep();
    if (toolButton(ic ? ic->gizmoMove : 0, "移動",
                   "移動ギズモ  (W)", ctx.gizmoMode == GizmoMode::Translate))
        ctx.gizmoMode = GizmoMode::Translate;
    ImGui::SameLine();
    if (toolButton(ic ? ic->gizmoRotate : 0, "回転",
                   "回転ギズモ  (E)", ctx.gizmoMode == GizmoMode::Rotate))
        ctx.gizmoMode = GizmoMode::Rotate;
    ImGui::SameLine();
    if (toolButton(ic ? ic->gizmoScale : 0, "拡縮",
                   "拡大縮小ギズモ  (R)", ctx.gizmoMode == GizmoMode::Scale))
        ctx.gizmoMode = GizmoMode::Scale;
    ImGui::SameLine();
    {
        u64 spaceTex = ctx.gizmoLocalSpace ? (ic ? ic->spaceLocal : 0)
                                           : (ic ? ic->spaceWorld : 0);
        if (toolButton(spaceTex, ctx.gizmoLocalSpace ? "ローカル" : "ワールド",
                       ctx.gizmoLocalSpace ? "ローカル空間  (T で切替)"
                                           : "ワールド空間  (T で切替)", false))
            ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace;
    }

    // ===== 2D / 3D ビュー切替（Unity の 2D ボタン相当）=====
    groupSep();
    if (toolButton(0, ctx.view2D ? "2D" : "3D",
                   ctx.view2D
                     ? "2Dビュー（クリックで3D）: 正射・正面固定。WASD/矢印=パン, ホイール=ズーム, 中ドラッグ=パン"
                     : "3Dビュー（クリックで2D）",
                   ctx.view2D))
        ctx.view2D = !ctx.view2D;

    // ===== ゲームビルド =====
    groupSep();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.82f, 1.0f));
    if (toolButton(ic ? ic->build : 0, "ビルド", "ゲームを書き出す（配布用 exe を生成）", false))
        ctx.pendingBuildGame = true;
    ImGui::PopStyleColor(2);
    if (ctx.buildCompleteFlash > 0.0f)
    {
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("\xe2\x9c\x93 ビルド完了");  // ✓
        ImGui::PopStyleColor();
        ctx.buildCompleteFlash -= clock->GetDeltaTime();
    }
    else if (ctx.buildErrorFlash > 0.0f)
    {
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::Text("\xe2\x9c\x97 ビルド失敗 (dx12_engine.log)");  // ✗
        ImGui::PopStyleColor();
        ctx.buildErrorFlash -= clock->GetDeltaTime();
    }

    // ===== ツール窓（「窓 ▾」ドロップダウン1個に集約。以前は8個のボタンで詰め込んでた）=====
    groupSep();
    if (toolButton(ic ? ic->window : 0, "窓 \xe2\x96\xbe",   // ▾
                   "ツール窓の表示/非表示", ctx.AnyToolWindowOpen()))
        ImGui::OpenPopup("##ToolWindowsMenu");
    if (ImGui::BeginPopup("##ToolWindowsMenu"))
    {
        ImGui::TextDisabled("ツール窓（右下にタブで開く）");
        ImGui::Separator();
        ImGui::MenuItem("Post Process",           nullptr, &ctx.showPostProcess);
        ImGui::MenuItem("Post Process パラメータ", nullptr, &ctx.showPostParams);
        ImGui::MenuItem("Skybox / IBL",           nullptr, &ctx.showSkybox);
        ImGui::MenuItem("SSAO",                   nullptr, &ctx.showSSAO);
        ImGui::MenuItem("エンジン設定",           nullptr, &ctx.showEngineSettings);
        ImGui::MenuItem("Scene Flow",             nullptr, &ctx.showSceneFlow);
        ImGui::MenuItem("Project",                nullptr, &ctx.showProject);
        ImGui::MenuItem("Git 変更",               nullptr, &ctx.showVersionControl);
        ImGui::MenuItem("MCP / AI Bridge",        nullptr, &ctx.showMcpBridge);
        ImGui::Separator();
        if (ImGui::MenuItem("すべて閉じる"))
        {
            ctx.showPostProcess = ctx.showPostParams = ctx.showSkybox = ctx.showSSAO =
                ctx.showEngineSettings = ctx.showSceneFlow = ctx.showProject =
                ctx.showVersionControl = ctx.showMcpBridge = false;
        }
        ImGui::EndPopup();
    }

    // ===== シーン名表示 =====
    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);
    if (!ctx.currentScenePath.empty())
    {
        std::string sceneName = std::filesystem::path(ctx.currentScenePath).stem().string();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        ImGui::Text("[%s]", sceneName.c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("[Untitled]");
    }

    // ===== FPS =====
    ImGui::SameLine(displayW - 100);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f FPS", clock->GetFPS());

    ImGui::PopStyleVar(2);  // FramePadding + FrameRounding
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ===== ヘルプ: ショートカット一覧モーダル =====
    {
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("ショートカット一覧##ShortcutsPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            struct KeyRow { const char* key; const char* desc; };
            static const KeyRow rows[] = {
                {"W / E / R",     "ギズモ切替（移動 / 回転 / スケール）"},
                {"T",             "ローカル / ワールド空間の切替"},
                {"左クリック",    "エンティティ選択（Ctrl+クリックで複数選択）"},
                {"右クリック+WASD","フライカメラ移動（Space/Shift で上下）"},
                {"F",             "選択エンティティにフォーカス"},
                {"F11",           "ボーダレスフルスクリーン切替"},
                {"Ctrl+Z / Y",   "元に戻す / やり直す"},
                {"Ctrl+C / V",   "コピー / 貼り付け"},
                {"Ctrl+D",        "複製"},
                {"Del",           "削除"},
                {"Ctrl+S / N",   "シーン保存 / 新規シーン"},
                {"Ctrl+O / L",   "シーンを開く / 新規スクリプト"},
            };
            if (ImGui::BeginTable("##shortcuts", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("動作", ImGuiTableColumnFlags_WidthStretch);
                for (const auto& r : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(r.key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(r.desc);
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            if (ImGui::Button("閉じる", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ===== ヘルプ: バージョン情報モーダル =====
    {
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("バージョン情報##AboutPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("DX12 Engine");
            ImGui::TextDisabled("DirectX 12 ゲームエンジン + エディタ");
            ImGui::Separator();
            ImGui::TextUnformatted("https://github.com/ryuto-alt/dx12");
            ImGui::Spacing();
            if (ImGui::Button("閉じる", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ===== 新規シーン名入力ダイアログ =====
    if (ctx.showNewSceneDialog)
    {
        ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup");
        ctx.showNewSceneDialog = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe5\x90\x8d:");  // シーン名:
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ImGui::InputText("##SceneName", ctx.newSceneNameBuf,
            sizeof(ctx.newSceneNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        // 初回フォーカス
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newSceneNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        const char* btnLabel = ctx.newSceneDialogIsCreate
            ? "\xe4\xbd\x9c\xe6\x88\x90"    // 作成
            : "\xe4\xbf\x9d\xe5\xad\x98";   // 保存
        if (ImGui::Button(btnLabel, ImVec2(120, 0)) || (enterPressed && nameValid))
        {
            std::string scenesDir = assetsDir + "scenes/";
            std::filesystem::create_directories(scenesDir);
            ctx.currentScenePath = scenesDir + ctx.newSceneNameBuf + ".json";
            ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
            if (ctx.newSceneDialogIsCreate)
            {
                ctx.pendingNewScene = true;  // 新規シーン作成
            }
            else
            {
                // 今のシーンをそのまま名前を付けて保存
                SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
                ctx.hotReloadFlash = 1.5f;
            }
            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ===== 新規スクリプト名入力ダイアログ =====
    if (ctx.showNewScriptDialog)
    {
        ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88##NewScriptPopup");
        ctx.showNewScriptDialog = false;
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88##NewScriptPopup",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88\xe5\x90\x8d:");  // スクリプト名:
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ImGui::InputText("##ScriptName", ctx.newScriptNameBuf,
            sizeof(ctx.newScriptNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newScriptNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ImVec2(120, 0)) || (enterPressed && nameValid))  // 作成
        {
            // assets/scripts/ にテンプレート生成（プロジェクト内＝アセットブラウザに表示され、
            // scriptPath="scripts/<name>.lua" で添付できる。旧 assets/../scripts はブラウザ外だった）
            std::string scriptsDir = assetsDir + "scripts/";
            std::filesystem::create_directories(scriptsDir);
            std::string scriptPath = scriptsDir + ctx.newScriptNameBuf + ".lua";

            if (!std::filesystem::exists(scriptPath))
            {
                std::ofstream ofs(scriptPath);
                ofs << "-- " << ctx.newScriptNameBuf << ".lua\n\n";
                ofs << "function OnStart()\n";
                ofs << "    -- Called once when play mode starts\n";
                ofs << "end\n\n";
                ofs << "function OnUpdate(dt)\n";
                ofs << "    -- Called every frame\n";
                ofs << "end\n";
                ofs.close();

                Logger::Info("Created script: {}", scriptPath);
                ctx.hotReloadFlash = 1.5f;
            }
            else
            {
                Logger::Warn("Script already exists: {}", scriptPath);
            }

            // VS Code で開く
            OpenInVSCode(scriptPath);

            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

} // namespace dx12e
