#include "editor/panels/MaterialGraphPanel.h"

#include "core/PathResolver.h"
#include "editor/EditorCommandTable.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "editor/panels/MaterialGraphPanelInternal.h"
#include "gui/VirtualInputImGui.h"
#include "input/VirtualInput.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>   // BeginDragDropTargetCustom（キャンバスへのノードのドロップ）
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace dx12e
{
namespace mgpanel
{

namespace
{
namespace fs = std::filesystem;

MaterialGraphPanel::ThumbnailProvider g_thumb;

ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 ColA(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }

std::string AssetsRoot()
{
    std::string a = PathResolver::AssetsDir();
    if (a.empty()) a = (fs::temp_directory_path() / "uno_matgraph/").string();
    return a;
}

bool IsAbsolutePath(const std::string& p)
{
    return p.size() >= 2 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
}

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void ScanDxmg(State& s)
{
    s.fileList.clear();
    std::error_code ec;
    const fs::path root(AssetsRoot());
    if (fs::exists(root, ec))
    {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            if (Lower(it->path().extension().string()) != ".dxmg") continue;
            const fs::path rel = fs::relative(it->path(), root, fec);
            if (!fec) s.fileList.push_back(rel.generic_string());
        }
    }
    std::sort(s.fileList.begin(), s.fileList.end());
    s.fileListScanned = true;
}

// ".dxmg" を補って絶対パスへ
std::string ResolveSavePath(const std::string& typed)
{
    std::string p = typed;
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
    if (p.empty()) return {};
    if (Lower(fs::path(p).extension().string()) != ".dxmg") p += ".dxmg";
    if (!IsAbsolutePath(p)) p = AssetsRoot() + p;
    return p;
}

std::string ResolveOpenPath(const std::string& typed)
{
    if (typed.empty()) return {};
    if (IsAbsolutePath(typed)) return typed;
    return AssetsRoot() + typed;
}

void AfterLoad(State& s, bool restoreView)
{
    s.view.SetDocument(&s.ed.Doc());
    s.view.ClearNodeStates();
    ng::ViewState vs;
    if (restoreView && s.ed.TakeLoadedView(&vs)) s.view.SetViewState(vs, false);
    else s.view.FrameAll(false);
    s.nodeStateKey = ~0ull;
    s.hlslCompileSeen = -1;
    s.pe = PropEdit{};
    s.settingsLive = false;
    s.diagCursor = -1;
    PreviewAfterLoad(s);   // G2c: サムネイルと GPU 側のプレビューを作り直す
}

// 次のフレームで（キャンバスの大きさが分かってから）全体表示する
bool g_needFrame = false;

void DoNew(State& s, mg::NewTemplate t, const char* what)
{
    s.ed.NewGraph(t);
    AfterLoad(s, false);
    g_needFrame = true;
    Say(s, std::string(what) + "を作りました");
}

bool DoOpen(State& s, const std::string& path)
{
    std::string err;
    if (!s.ed.Open(path, &err))
    {
        Say(s, "開けませんでした: " + err, true);
        return false;
    }
    AfterLoad(s, true);
    g_needFrame = !s.ed.Graph().View().valid;
    Say(s, "開きました: " + PathLeaf(path));
    return true;
}

bool DoSave(State& s, bool forceAs)
{
    if (!s.ed.HasPath() || forceAs)
    {
        s.dialog = Dialog::SaveAs;
        s.dialogRequest = true;
        s.dialogError.clear();
        s.overwriteAsk = false;
        std::string def = "materials/" + (s.ed.Graph().Name().empty() ? std::string("NewMaterial") : s.ed.Graph().Name()) + ".dxmg";
        if (s.ed.HasPath())
        {
            // すでにパスがあるときは、assets 相対（下にあれば）を初期値にする
            std::error_code ec;
            const fs::path rel = fs::relative(fs::path(s.ed.Path()), fs::path(AssetsRoot()), ec);
            def = (!ec && !rel.empty() && rel.native().find(L"..") == std::wstring::npos) ? rel.generic_string() : s.ed.Path();
        }
        std::snprintf(s.pathBuf, sizeof(s.pathBuf), "%s", def.c_str());
        return false;
    }
    std::string err;
    const ng::ViewState vs = s.view.GetViewState();
    if (!s.ed.Save(&err, &vs))
    {
        Say(s, "保存できませんでした: " + err, true);
        return false;
    }
    Say(s, "保存しました: " + PathLeaf(s.ed.Path()));
    return true;
}

// 未保存の変更があるなら確認を挟んでから action を実行する
void GuardDiscard(State& s, const std::string& action)
{
    if (s.ed.IsDirty())
    {
        s.confirmAction = action;
        s.dialog = Dialog::ConfirmDiscard;
        s.dialogRequest = true;
        return;
    }
    s.pending.push_back("!" + action);   // 確認なしで実行（"!" 付き）
}

void RunAction(State& s, const std::string& action)
{
    if (action == "matgraph.new") DoNew(s, mg::NewTemplate::Empty, "空のグラフ");
    else if (action == "matgraph.newPbr") DoNew(s, mg::NewTemplate::StandardPbr, "標準 PBR のグラフ");
    else if (action == "matgraph.sample") DoNew(s, mg::NewTemplate::Sample, "見本のグラフ");
    else if (action.rfind("open:", 0) == 0) DoOpen(s, action.substr(5));
    else if (action == "matgraph.open")
    {
        s.dialog = Dialog::Open;
        s.dialogRequest = true;
        s.fileListScanned = false;
        s.fileFilter[0] = '\0';
        s.dialogError.clear();
        s.pathBuf[0] = '\0';
    }
}

void PromoteSelected(State& s)
{
    const ng::Selection& sel = s.view.GetSelection();
    if (sel.nodes.size() != 1) { Say(s, "パラメータにするノードを 1 個選んでください", true); return; }
    const ng::NodeId id = *sel.nodes.begin();
    std::string why;
    if (!s.ed.CanPromote(id, &why)) { Say(s, why, true); return; }
    ng::NodeId np = 0;
    if (s.ed.PromoteToParameter(id, &np))
    {
        s.view.FocusNode(np, true);
        Say(s, "パラメータに昇格しました（Ctrl+Z で戻せます）");
    }
    else Say(s, "昇格できませんでした", true);
}

void ProcessCommand(State& s, EditorContext& ctx, const std::string& id)
{
    (void)ctx;
    if (!id.empty() && id[0] == '!') { RunAction(s, id.substr(1)); return; }
    if (id == "matgraph.new" || id == "matgraph.newPbr" || id == "matgraph.sample" || id == "matgraph.open") { GuardDiscard(s, id); return; }
    if (id == "matgraph.save") { DoSave(s, false); return; }
    if (id == "matgraph.saveAs") { DoSave(s, true); return; }
    if (id == "matgraph.recompile") { s.ed.Recompile(); s.compileFlashUntil = s.now + 0.5; Say(s, "HLSL を再生成しました"); return; }
    if (id == "matgraph.nextDiag") { NextDiagnostic(s); return; }
    if (id == "matgraph.copyHlsl") { CopyHlsl(s); return; }
    if (id == "matgraph.promote") { PromoteSelected(s); return; }
    if (id.rfind("graph.", 0) == 0 || id.rfind("edit.", 0) == 0) { s.view.Execute(id); return; }
}

// ---------------------------------------------------------------------------
// ダイアログ
// ---------------------------------------------------------------------------
void DrawFileDialogs(State& s)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    if (s.dialogRequest)
    {
        const char* name = s.dialog == Dialog::ConfirmDiscard ? "未保存の変更###mgConfirm"
                         : s.dialog == Dialog::Open ? "マテリアルグラフを開く###mgOpen"
                         : s.dialog == Dialog::SaveAs ? "名前を付けて保存###mgSaveAs" : nullptr;
        if (name) ImGui::OpenPopup(name);
        s.dialogRequest = false;
    }

    ui::PushMenuStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(8.0f, 8.0f));

    // ---- 未保存の確認 ----
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("未保存の変更###mgConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("「%s」に保存していない変更があります。", s.ed.DisplayName().c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::TextUnformatted("続けると、この変更は失われます（Undo 履歴も消えます）。");
        ImGui::PopStyleColor();
        ImGui::Dummy(ui::Px(0.0f, 4.0f));
        if (ui::PrimaryButton("保存して続ける", ui::Px(140.0f, 0.0f)))
        {
            if (DoSave(s, false)) { s.pending.push_back("!" + s.confirmAction); ImGui::CloseCurrentPopup(); }
            else ImGui::CloseCurrentPopup();   // パス未設定 → 保存ダイアログが開く（続きは行わない）
        }
        Anchor("button", "mg:btn:confirmSave");
        ImGui::SameLine();
        if (ui::DangerButton("破棄して続ける", ui::Px(140.0f, 0.0f)))
        {
            s.pending.push_back("!" + s.confirmAction);
            ImGui::CloseCurrentPopup();
        }
        Anchor("button", "mg:btn:confirmDiscard");
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        Anchor("button", "mg:btn:confirmCancel");
        ImGui::EndPopup();
    }

    // ---- 開く ----
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(560.0f, 480.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("マテリアルグラフを開く###mgOpen", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        if (!s.fileListScanned) ScanDxmg(s);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::Text("assets 以下の .dxmg（%zu 個）", s.fileList.size());
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ui::SearchField("##mgOpenFilter", s.fileFilter, sizeof(s.fileFilter), "ファイルを絞り込み…");
        const float footer = ui::Px(78.0f);
        ImGui::BeginChild("##mgOpenList", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
        {
            const std::string f = Lower(s.fileFilter);
            int shown = 0;
            for (const std::string& rel : s.fileList)
            {
                if (!f.empty() && Lower(rel).find(f) == std::string::npos) continue;
                ++shown;
                const bool sel = std::string(s.pathBuf) == rel;
                if (ImGui::Selectable(rel.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    std::snprintf(s.pathBuf, sizeof(s.pathBuf), "%s", rel.c_str());
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        s.pending.push_back("!open:" + ResolveOpenPath(rel));
                        ImGui::CloseCurrentPopup();
                    }
                }
                Anchor("row", "mg:file:" + rel);
            }
            if (shown == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::TextUnformatted(s.fileList.empty() ? "この assets に .dxmg はまだありません" : "一致するファイルがありません");
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ui::InputTextWithHint("##mgOpenPath", "パスを直接入力（assets 相対 / 絶対）", s.pathBuf, sizeof(s.pathBuf));
        if (!s.dialogError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad);
            ImGui::TextUnformatted(s.dialogError.c_str());
            ImGui::PopStyleColor();
        }
        if (ui::PrimaryButton("開く", ui::Px(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            const std::string p = ResolveOpenPath(s.pathBuf);
            std::error_code ec;
            if (p.empty() || !fs::exists(fs::path(p), ec)) s.dialogError = "ファイルが見つかりません";
            else { s.pending.push_back("!open:" + p); ImGui::CloseCurrentPopup(); }
        }
        Anchor("button", "mg:btn:openOk");
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 名前を付けて保存 ----
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(560.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("名前を付けて保存###mgSaveAs", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::Text("保存先（assets からの相対パス。.dxmg は自動で付きます）");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ui::InputText("##mgSavePath", s.pathBuf, sizeof(s.pathBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        Anchor("input", "mg:input:savePath");
        const std::string full = ResolveSavePath(s.pathBuf);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(full.empty() ? "" : full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        std::error_code ec;
        const bool exists = !full.empty() && fs::exists(fs::path(full), ec) && full != s.ed.Path();
        if (exists)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn);
            ImGui::TextUnformatted("同名のファイルがあります。保存すると上書きされます。");
            ImGui::PopStyleColor();
        }
        if (!s.dialogError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad);
            ImGui::TextUnformatted(s.dialogError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ui::Px(0.0f, 2.0f));
        if (ui::PrimaryButton(exists ? "上書き保存" : "保存", ui::Px(120.0f, 0.0f)) || enter)
        {
            if (full.empty()) s.dialogError = "保存先を入力してください";
            else
            {
                std::string err;
                const ng::ViewState vs = s.view.GetViewState();
                if (s.ed.SaveAs(full, &err, &vs))
                {
                    Say(s, "保存しました: " + PathLeaf(full));
                    ImGui::CloseCurrentPopup();
                }
                else s.dialogError = err;
            }
        }
        Anchor("button", "mg:btn:saveOk");
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ui::PopMenuStyle();
}

// ---------------------------------------------------------------------------
// ツールバー
// ---------------------------------------------------------------------------
struct ChipInfo { std::string text; ImU32 dot = 0; bool ok = true; };

ChipInfo CompileChip(State& s)
{
    ChipInfo c;
    const mg::MatGraphEditor& ed = s.ed;
    if (!ed.HasResult()) { c.text = "…"; c.dot = Col(theme::TextDim); return c; }
    char buf[160];
    if (ed.ErrorCount() == 0)
    {
        // G2c: GPU のコンパイル状態（変更を検出 / 生成中… DXC・PSO / 高速版で表示中 / シェーダーエラー）
        std::string t, tip;
        ImU32 dot = 0;
        bool ok = true;
        if (GpuChip(s, t, dot, ok, tip)) { c.text = t; c.dot = dot; c.ok = ok; return c; }
    }
    if (ed.ErrorCount() > 0)
    {
        if (ed.WarningCount() > 0) std::snprintf(buf, sizeof(buf), "エラー %d 件・警告 %d 件", ed.ErrorCount(), ed.WarningCount());
        else std::snprintf(buf, sizeof(buf), "エラー %d 件", ed.ErrorCount());
        c.text = buf; c.dot = Col(theme::Bad); c.ok = false;
    }
    else if (ed.WarningCount() > 0)
    {
        std::snprintf(buf, sizeof(buf), "警告 %d 件・生成 OK", ed.WarningCount());
        c.text = buf; c.dot = Col(theme::Warn);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "生成 OK  %.1f ms", ed.LastCompileMs());
        c.text = buf; c.dot = Col(theme::Good);
    }
    return c;
}

// ツールバーのアイコンボタン（仮想入力の名前つき）
bool ToolIcon(const char* id, const char* glyph, const char* tip, bool active = false, bool enabled = true)
{
    if (!enabled) ImGui::BeginDisabled();
    const bool r = ui::IconButton(id, glyph, tip, active);
    if (!enabled) ImGui::EndDisabled();
    Anchor("button", std::string("mg:btn:") + (id[0] == '#' ? id + 2 : id));
    return r;
}

void DrawToolbar(State& s, EditorContext& ctx, ImVec2 origin, float width, float height)
{
    (void)ctx;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Col(theme::Bg1));
    dl->AddLine(ImVec2(origin.x, origin.y + height - b1 * 0.5f), ImVec2(origin.x + width, origin.y + height - b1 * 0.5f), Col(theme::Border), b1);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + ui::Px(8.0f), origin.y + (height - ui::Px(26.0f)) * 0.5f));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(3.0f, 0.0f));
    auto sep = [&]()
    {
        ImGui::SameLine(0.0f, ui::Px(8.0f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, origin.y + ui::Px(9.0f)), ImVec2(p.x, origin.y + height - ui::Px(9.0f)), Col(theme::Border), b1);
        ImGui::SameLine(0.0f, ui::Px(9.0f));
    };

    if (ToolIcon("##mgNew", ICON_FILE_PLUS, "新規（空 / 標準 PBR / 見本）  Ctrl+N")) ImGui::OpenPopup("##mgNewMenu");
    if (ImGui::BeginPopup("##mgNewMenu", ImGuiWindowFlags_NoSavedSettings))
    {
        ui::PushMenuStyle();
        if (ui::MenuItem(ICON_FILE, "空のグラフ", "Ctrl+N")) s.pending.push_back("matgraph.new");
        if (ui::MenuItem(ICON_T_MATERIAL, "標準 PBR（テクスチャ + パラメータ）")) s.pending.push_back("matgraph.newPbr");
        if (ui::MenuItem(ICON_T_SPLINE, "見本（テクスチャ x 色 / ノイズ / フレネル）")) s.pending.push_back("matgraph.sample");
        ui::PopMenuStyle();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ToolIcon("##mgOpenBtn", ICON_FOLDER_OPEN, "開く…  Ctrl+O")) s.pending.push_back("matgraph.open");
    ImGui::SameLine();
    if (ToolIcon("##mgSave", ICON_SAVE, "保存  Ctrl+S", s.ed.IsDirty())) s.pending.push_back("matgraph.save");
    ImGui::SameLine();
    if (ToolIcon("##mgSaveAs", ICON_COPY, "名前を付けて保存…  Ctrl+Shift+S")) s.pending.push_back("matgraph.saveAs");
    sep();
    if (ToolIcon("##mgUndo", ICON_UNDO, "元に戻す  Ctrl+Z", false, s.ed.Doc().History().CanUndo())) s.pending.push_back("edit.undo");
    ImGui::SameLine();
    if (ToolIcon("##mgRedo", ICON_REDO, "やり直す  Ctrl+Y", false, s.ed.Doc().History().CanRedo())) s.pending.push_back("edit.redo");
    sep();
    if (ToolIcon("##mgFrameAll", ICON_SCAN_EYE, "全体を表示  Home")) s.pending.push_back("graph.frameAll");
    ImGui::SameLine();
    if (ToolIcon("##mgFrameSel", ICON_EYE, "選択へ移動  F")) s.pending.push_back("edit.focus");
    ImGui::SameLine();
    if (ToolIcon("##mgZoom100", ICON_T_GRID, "表示を 100% に  Ctrl+0")) s.pending.push_back("graph.resetZoom");
    sep();
    if (ToolIcon("##mgPromote", ICON_T_MATERIAL, "選択をパラメータに昇格（定数 / テクスチャ）")) s.pending.push_back("matgraph.promote");
    ImGui::SameLine();
    if (ToolIcon("##mgRecompile", ICON_REFRESH, "HLSL を再生成  Ctrl+Enter")) s.pending.push_back("matgraph.recompile");
    sep();
    if (ToolIcon("##mgTogLeft", ICON_T_LAYERS, "ノードパレットの表示", s.showLeft)) s.showLeft = !s.showLeft;
    ImGui::SameLine();
    if (ToolIcon("##mgTogRight", ICON_T_SLIDERS, "詳細パネルの表示", s.showRight)) s.showRight = !s.showRight;
    ImGui::SameLine();
    if (ToolIcon("##mgTogBottom", ICON_FILE_CODE, "診断 / HLSL の表示", s.showBottom)) s.showBottom = !s.showBottom;
    ImGui::SameLine();
    {
        ng::GraphViewSettings& gs = s.view.Settings();
        if (ToolIcon("##mgSnap", ICON_T_GRID, "グリッドスナップ", gs.snap)) gs.snap = !gs.snap;
        ImGui::SameLine();
        if (ToolIcon("##mgMinimap", ICON_T_MONITOR, "ミニマップ", gs.showMinimap)) gs.showMinimap = !gs.showMinimap;
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const float usedRight = ImGui::GetItemRectMax().x;

    // ---- 右寄せ: コンパイル状態 ----
    const ChipInfo chip = CompileChip(s);
    const float ph = ui::Px(24.0f);
    const float padX = ui::Px(11.0f);
    const float dotR = ui::PxF(3.6f);
    const ImVec2 ts = ImGui::CalcTextSize(chip.text.c_str());
    float w = padX + dotR * 2.0f + ui::Px(7.0f) + ts.x + padX;
    // 窓が狭くてツールバーのアイコンにかかるときは、文字を省いて点だけにする（詳細はツールチップ）
    const bool compact = origin.x + width - w - ui::Px(10.0f) < usedRight + ui::Px(10.0f);
    if (compact) w = padX * 2.0f + dotR * 2.0f;
    float x = origin.x + width - w - ui::Px(10.0f);
    const float y = origin.y + (height - ph) * 0.5f;

    // 「値のみの変更」の表示（HLSL は変わらず、再コンパイルされない）: 編集の直後だけ淡く出す
    if (s.ed.ValueOnlyEditSeq() != s.valueSeqSeen)
    {
        s.valueSeqSeen = s.ed.ValueOnlyEditSeq();
        s.valueBadgeUntil = s.now + 3.2;
    }
    const float badge = static_cast<float>(std::min(1.0, std::max(0.0, (s.valueBadgeUntil - s.now) / 0.8)));
    if (badge > 0.01f && x - usedRight > ui::Px(230.0f))
    {
        const char* t = "値のみの変更  再コンパイル不要（HLSL 不変）";
        const ImVec2 bs = ImGui::CalcTextSize(t);
        const float bw = padX + bs.x + padX;
        const ImVec2 bmn(x - bw - ui::Px(8.0f), y), bmx(bmn.x + bw, y + ph);
        const ImVec4 acc = theme::Accent;
        dl->AddRectFilled(bmn, bmx, ColA(acc, 0.16f * badge), ph * 0.5f);
        dl->AddRect(bmn, bmx, ColA(theme::AccentHover, 0.55f * badge), ph * 0.5f, 0, b1);
        dl->AddText(ImVec2(bmn.x + padX, bmn.y + (ph - bs.y) * 0.5f), ColA(theme::AccentHover, badge), t);
        ImGui::SetCursorScreenPos(bmn);
        ImGui::InvisibleButton("##mgValueBadge", ImVec2(bw, ph));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("編集した値はパラメータプールのスロットへ入るだけで、\n生成される HLSL は 1 バイトも変わりません。\nシェーダーの再コンパイルも要りません。");
        Anchor("status", "mg:valueOnly");
    }
    if (compact)
    {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ph), chip.ok ? Col(theme::Bg3) : ColA(theme::Bad, 0.16f), ph * 0.5f);
        dl->AddCircleFilled(ImVec2(x + w * 0.5f, y + ph * 0.5f), dotR, chip.dot, 14);
    }
    else StatusChip(dl, ImVec2(x, y), chip.text.c_str(), chip.dot, chip.ok ? Col(theme::Bg3) : ColA(theme::Bad, 0.16f), Col(theme::Text));
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##mgCompileChip", ImVec2(w, ph));
    if (ImGui::IsItemHovered())
    {
        const mat::CompileResult& r = s.ed.Result();
        std::string gtext, gtip;
        ImU32 gdot = 0;
        bool gok = true;
        GpuChip(s, gtext, gdot, gok, gtip);
        ImGui::SetTooltip("%s\nノード %d（出力に繋がる %d）/ Op %d / スロット %d / HLSL %d 行\n生成 %d 回（値のみの変更は数えない）  直近 %.2f ms%s\nクリックで診断を開く",
                          chip.text.c_str(), r.stats.nodesTotal, r.stats.nodesReachable, r.stats.ops, r.slotCount, r.stats.hlslLines, s.ed.CompileCount(), s.ed.LastCompileMs(), gtip.c_str());
    }
    if (ImGui::IsItemClicked()) { s.showBottom = true; s.bottomTab = 0; }
    Anchor("status", "mg:compile " + chip.text);
}

// ---------------------------------------------------------------------------
// ステータスバー
// ---------------------------------------------------------------------------
void DrawStatusBar(State& s, ImVec2 mn, float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mn, ImVec2(mn.x + width, mn.y + height), Col(theme::Bg1));
    dl->AddLine(ImVec2(mn.x, mn.y + 0.5f), ImVec2(mn.x + width, mn.y + 0.5f), Col(theme::Border), std::max(1.0f, ui::PxF(1.0f)));
    const ng::GraphViewStats& st = s.view.Stats();
    const ng::Selection& sel = s.view.GetSelection();
    char buf[320];
    std::snprintf(buf, sizeof(buf), "ノード %d   ワイヤ %d   |   Zoom %.0f%%   |   選択 %d   |   Undo %zu / Redo %zu   |   生成 %d 回",
                  st.nodes, st.edges, static_cast<double>(st.zoom) * 100.0, static_cast<int>(sel.nodes.size() + sel.comments.size()),
                  s.ed.Doc().History().UndoDepth(), s.ed.Doc().History().RedoDepth(), s.ed.CompileCount());
    ui::PushMono();
    const float ty = mn.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(mn.x + ui::Px(10.0f), ty), Col(theme::TextDim), buf);
    ui::PopMono();
    if (s.now < s.messageUntil)
    {
        const ImVec2 ts = ImGui::CalcTextSize(s.message.c_str());
        dl->AddText(ImVec2(mn.x + width - ts.x - ui::Px(12.0f), ty), Col(s.messageBad ? theme::Bad : theme::Good), s.message.c_str());
    }
    else if (s.ed.HasPath())
    {
        const std::string p = s.ed.Path();
        const ImVec2 ts = ImGui::CalcTextSize(p.c_str());
        if (ts.x < width * 0.4f) dl->AddText(ImVec2(mn.x + width - ts.x - ui::Px(12.0f), ty), Col(theme::TextFaint), p.c_str());
    }
    if (vinput::Enabled())
    {
        vinput::Anchor a;
        a.kind = "status";
        a.label = "mg:status " + s.view.StatusLine() + " dirty=" + (s.ed.IsDirty() ? "1" : "0") + " path=" + s.ed.Path() +
                  " compiles=" + std::to_string(s.ed.CompileCount()) + " errors=" + std::to_string(s.ed.ErrorCount()) +
                  " warnings=" + std::to_string(s.ed.WarningCount());
        a.window = ImGui::GetCurrentWindow()->Name;
        a.x0 = mn.x; a.y0 = mn.y; a.x1 = mn.x + width; a.y1 = mn.y + height;
        vinput::Global().AddAnchor(std::move(a));
    }
}

// スプリッタ（見た目は細い線。ホバー / ドラッグでアクセント）。ドラッグ量（px）を返す。
float Splitter(const char* id, ImVec2 mn, ImVec2 mx, bool vertical)
{
    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton(id, ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hov) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b = std::max(1.0f, ui::PxF(1.0f));
    ImU32 col = hov ? Col(theme::Accent) : Col(theme::Border);
    if (vertical)
    {
        const float x = std::floor((mn.x + mx.x) * 0.5f);
        dl->AddRectFilled(ImVec2(x, mn.y), ImVec2(x + (hov ? b * 2.0f : b), mx.y), col);
    }
    else
    {
        const float y = std::floor((mn.y + mx.y) * 0.5f);
        dl->AddRectFilled(ImVec2(mn.x, y), ImVec2(mx.x, y + (hov ? b * 2.0f : b)), col);
    }
    Anchor("splitter", std::string("mg:split:") + (id[0] == '#' ? id + 2 : id));
    if (ImGui::IsItemActive()) return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    return 0.0f;
}

void Init(State& s)
{
    s.codeBuf.assign(16384, '\0');
    s.view.SetDocument(&s.ed.Doc());
    s.view.Settings().hotkeyHoldClick = true;   // UE 風: キーを押しながらクリックでその場に置く
    s.view.SetAnchorSink([](const char* kind, const std::string& label, ImVec2 mn, ImVec2 mx)
    {
        if (!vinput::Enabled()) return;
        vinput::Anchor a;
        a.kind = kind;
        a.label = label;
        a.window = ImGui::GetCurrentWindow() ? ImGui::GetCurrentWindow()->RootWindow->Name : "";
        a.x0 = mn.x; a.y0 = mn.y; a.x1 = mx.x; a.y1 = mx.y;
        vinput::Global().AddAnchor(std::move(a));
    }, 80);
    // ノードの右クリックメニューへ、マテリアルグラフ固有の項目を足す
    s.view.SetNodeMenuExtender([&s](ng::NodeId id, const ng::Selection& sel)
    {
        const mat::NodeDef* def = s.ed.Model().DefOf(id);
        if (!def) return;
        if (ui::MenuItem(ICON_T_SLIDERS, "プロパティ", nullptr))
        {
            s.showRight = true;
            s.detailPropsOpen = true;
        }
        if (def->isCustom && ui::MenuItem(ICON_FILE_CODE, "カスタム HLSL 式を編集…", nullptr)) OpenCustomEditor(s, id);
        const bool hasTex = def->type == "TextureSample" || def->type == "NormalMap" || def->type == "TextureParameter";
        if (hasTex && ui::MenuItem(ICON_IMAGE, "テクスチャを選択…", nullptr))
            OpenTexturePicker(s, id, def->type == "TextureParameter" ? "default" : "texture");
        std::string why;
        const bool can = sel.nodes.size() == 1 && s.ed.CanPromote(id, &why);
        if (def->isParameter) ui::MenuItem(ICON_CHECK, "パラメータ（インスタンスで値を上書きできます）", nullptr, false, false);
        else if (ui::MenuItem(ICON_T_MATERIAL, "パラメータに昇格", nullptr, false, can)) PromoteSelected(s);
        if (!can && !def->isParameter && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !why.empty()) ImGui::SetTooltip("%s", why.c_str());
    });
    s.view.SetNodeActivateHandler([&s](ng::NodeId id)
    {
        const mat::NodeDef* def = s.ed.Model().DefOf(id);
        if (!def) return;
        if (def->isCustom) OpenCustomEditor(s, id);
        else { s.showRight = true; s.detailPropsOpen = true; }
    });
    PreviewInit(s);   // G2c: 設定の読み込み・サムネイル枠の予約（GPU が使えるときだけ）
    s.ed.NewGraph(mg::NewTemplate::StandardPbr);
    AfterLoad(s, false);
    g_needFrame = true;
    s.init = true;
}

} // namespace

MaterialGraphPanel::PreviewDrawer g_previewDrawer;

// ---------------------------------------------------------------------------
// 共通の小物
// ---------------------------------------------------------------------------
State& S()
{
    static State s;
    return s;
}

void Say(State& s, const std::string& msg, bool bad)
{
    s.message = msg;
    s.messageBad = bad;
    s.messageUntil = ImGui::GetTime() + 4.5;
}

std::string PathLeaf(const std::string& p)
{
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

void Anchor(const char* kind, const std::string& label)
{
    vinput_gui::AnchorLastItem(kind, label.c_str());
}

ImU32 CategoryColor(int slot)
{
    const ng::GraphTokens tok = ng::MakeGraphTokens();
    return tok.headerCatBright[std::min(std::max(slot, 0), ng::kCategorySlots - 1)];
}

float PaneHeader(const char* title, const char* right)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ui::Px(28.0f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(theme::Bg2));
    dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), Col(theme::Border), std::max(1.0f, ui::PxF(1.0f)));
    ui::PushBold();
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(p.x + ui::Px(10.0f), p.y + (h - ts.y) * 0.5f), Col(theme::Text), title);
    ui::PopBold();
    if (right && right[0])
    {
        const ImVec2 rs = ImGui::CalcTextSize(right);
        dl->AddText(ImVec2(p.x + w - rs.x - ui::Px(10.0f), p.y + (h - rs.y) * 0.5f), Col(theme::TextFaint), right);
    }
    ImGui::Dummy(ImVec2(w, h));
    return h;
}

float StatusChip(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 dot, ImU32 face, ImU32 textCol)
{
    const float h = ui::Px(24.0f), padX = ui::Px(11.0f), dotR = ui::PxF(3.6f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = padX + dotR * 2.0f + ui::Px(7.0f) + ts.x + padX;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), face, h * 0.5f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), Col(theme::Border), h * 0.5f, 0, std::max(1.0f, ui::PxF(1.0f)));
    dl->AddCircleFilled(ImVec2(pos.x + padX + dotR, pos.y + h * 0.5f), dotR, dot, 14);
    dl->AddText(ImVec2(pos.x + padX + dotR * 2.0f + ui::Px(7.0f), pos.y + (h - ts.y) * 0.5f), textCol, text);
    return w;
}

// MaterialGraphPanelPreview.cpp（MCP の DebugCall）から使う入口
void InitState(State& s) { Init(s); }
void AfterLoadExternal(State& s)
{
    AfterLoad(s, false);
    g_needFrame = true;
}

} // namespace mgpanel

// ===========================================================================
// 公開 API
// ===========================================================================
namespace MaterialGraphPanel
{

bool ExecuteCommand(EditorContext& ctx, const char* commandId)
{
    if (!ctx.showMaterialGraph || !commandId) return false;
    mgpanel::S().pending.emplace_back(commandId);
    return true;
}

bool OpenFileNow(EditorContext& ctx, const std::string& path, std::string* error)
{
    mgpanel::State& s = mgpanel::S();
    ctx.showMaterialGraph = true;
    if (!s.init) mgpanel::Init(s);
    std::string err;
    if (!s.ed.Open(path, &err))
    {
        if (error) *error = err;
        return false;
    }
    mgpanel::AfterLoad(s, true);
    mgpanel::g_needFrame = !s.ed.Graph().View().valid;
    return true;
}

void SetThumbnailProvider(ThumbnailProvider fn) { mgpanel::g_thumb = std::move(fn); }
void SetPreviewDrawer(PreviewDrawer fn) { mgpanel::g_previewDrawer = std::move(fn); }

void Render(EditorContext& ctx)
{
    using namespace mgpanel;
    State& s = S();
    if (!ctx.showMaterialGraph)
    {
        ctx.materialGraphKeyFocus = false;
        return;
    }
    if (!s.init) Init(s);
    s.now = ImGui::GetTime();

    // ---- 外からの依頼（アセットブラウザのダブルクリック・マテリアルエディタのボタン）----
    if (!ctx.pendingOpenMatGraphPath.empty())
    {
        const std::string p = ctx.pendingOpenMatGraphPath;
        ctx.pendingOpenMatGraphPath.clear();
        GuardDiscard(s, "open:" + p);
    }
    if (ctx.pendingNewMatGraph)
    {
        ctx.pendingNewMatGraph = false;
        GuardDiscard(s, "matgraph.newPbr");
    }

    // ---- 窓 ----
    std::string title = "マテリアルグラフ - " + s.ed.DisplayName() + (s.ed.IsDirty() ? " *" : "") + "###MaterialGraph";
    const ImGuiViewport* mvp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mvp->Pos.x + std::min(ui::Px(80.0f), mvp->Size.x * 0.05f), mvp->Pos.y + std::min(ui::Px(70.0f), mvp->Size.y * 0.06f)), ImGuiCond_FirstUseEver);
    // 既定の大きさは 1560x940（論理 px）。ただし表示領域の 86% までに収める（DPI が大きい・画面が小さいときに窓がはみ出さないように）
    ImGui::SetNextWindowSize(ImVec2(std::min(ui::Px(1560.0f), mvp->Size.x * 0.86f), std::min(ui::Px(940.0f), mvp->Size.y * 0.86f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ui::Px(520.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(title.c_str(), &ctx.showMaterialGraph, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open)
    {
        ImGui::End();
        ctx.materialGraphKeyFocus = false;
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        ctx.floatingToolWindowHoveredThisFrame = true;
    ctx.materialGraphKeyFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // ---- コマンド ----
    {
        std::vector<std::string> cmds;
        cmds.swap(s.pending);
        for (const std::string& c : cmds) ProcessCommand(s, ctx, c);
    }

    // ---- 文書の更新（コメント同期・再コンパイル・診断）----
    s.ed.Update();
    if (s.ed.CompileCount() != s.compileSeen)
    {
        if (s.compileSeen >= 0) s.compileFlashUntil = s.now + 0.35;
        s.compileSeen = s.ed.CompileCount();
    }
    PreviewUpdate(s);   // G2c: 変更の検出・デバウンス・GPU へ送信・状態・DXC エラーのノード逆引き
    ApplyNodeStates(s);

    // ---- レイアウト ----
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = avail.x, H = avail.y;
    const float tbH = ui::Px(40.0f), stH = ui::Px(24.0f), sp = ui::Px(5.0f);
    DrawToolbar(s, ctx, origin, W, tbH);

    const bool wideLeft = W >= ui::Px(640.0f), wideRight = W >= ui::Px(940.0f);
    const bool showL = s.showLeft && wideLeft, showR = s.showRight && wideRight;
    const float bodyY0 = origin.y + tbH, bodyY1 = origin.y + H - stH;
    const float bodyH = std::max(ui::Px(120.0f), bodyY1 - bodyY0);
    const float minCenter = ui::Px(260.0f);
    float leftPx = showL ? ui::Px(s.leftW) : 0.0f, rightPx = showR ? ui::Px(s.rightW) : 0.0f;
    // 中央が細くなりすぎないように両側を縮める
    const float sideMax = std::max(0.0f, W - minCenter - (showL ? sp : 0.0f) - (showR ? sp : 0.0f));
    if (leftPx + rightPx > sideMax)
    {
        const float k = sideMax / std::max(1.0f, leftPx + rightPx);
        leftPx *= k; rightPx *= k;
    }
    const float cx0 = origin.x + leftPx + (showL ? sp : 0.0f);
    const float cx1 = origin.x + W - rightPx - (showR ? sp : 0.0f);
    float bottomPx = s.showBottom ? std::min(ui::Px(s.bottomH), bodyH * 0.65f) : 0.0f;
    const float canvasH = bodyH - bottomPx - (s.showBottom ? sp : 0.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 paneBg = Col(theme::Bg1);
    // ---- 左: ノードパレット ----
    if (showL)
    {
        const ImVec2 mn(origin.x, bodyY0), mx(origin.x + leftPx, bodyY1);
        dl->AddRectFilled(mn, mx, paneBg);
        ImGui::SetCursorScreenPos(mn);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##mgLeft", ImVec2(leftPx, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        DrawPalettePane(s, ImVec2(leftPx, bodyH));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        const float d = Splitter("##mgSplitL", ImVec2(mx.x, bodyY0), ImVec2(mx.x + sp, bodyY1), true);
        if (d != 0.0f) s.leftW = std::min(std::max(s.leftW + d / ui::Scale(), 180.0f), 480.0f);
    }
    // ---- 右: 詳細 ----
    if (showR)
    {
        const ImVec2 mn(origin.x + W - rightPx, bodyY0), mx(origin.x + W, bodyY1);
        dl->AddRectFilled(mn, mx, paneBg);
        ImGui::SetCursorScreenPos(mn);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##mgRight", ImVec2(rightPx, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        DrawDetailsPane(s, ctx, ImVec2(rightPx, bodyH));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        const float d = Splitter("##mgSplitR", ImVec2(mn.x - sp, bodyY0), ImVec2(mn.x, bodyY1), true);
        if (d != 0.0f) s.rightW = std::min(std::max(s.rightW - d / ui::Scale(), 240.0f), 560.0f);
    }
    // ---- 中央: キャンバス ----
    {
        const ImVec2 mn(cx0, bodyY0);
        const ImVec2 size(std::max(ui::Px(64.0f), cx1 - cx0), std::max(ui::Px(64.0f), canvasH));
        ImGui::SetCursorScreenPos(mn);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##mgCanvasHost", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        s.view.Draw("##mgcanvas", size);
        s.view.ProcessKeys();
        // パレットからのノードのドロップ
        if (ImGui::BeginDragDropTargetCustom(ImRect(s.view.CanvasMin(), s.view.CanvasMax()), ImGui::GetID("##mgdrop")))
        {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MG_NODE_TYPE"))
            {
                const std::string type(static_cast<const char*>(pl->Data));
                ng::Vec2 g = s.view.ToGraph(ImGui::GetIO().MousePos);
                const ng::NodeTypeDesc* t = s.ed.Model().FindNodeType(type);
                if (t)
                {
                    const ng::TypeLayout& L = s.ed.Doc().Layout().For(*t);
                    if (L.reroute) g = g - L.size * 0.5f;
                    if (s.view.Settings().snap) g = ng::SnapVec(g, s.view.Settings().snapStep);
                    const ng::NodeId id = s.ed.Doc().AddNode(type, g);
                    if (id) s.view.FocusNode(id, false);
                }
            }
            // アセットブラウザからテクスチャをドロップ = その位置に TextureSample（texture プロパティにパス）を置く
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
            {
                const std::string abs(static_cast<const char*>(pl->Data));
                if (AssetBrowserPanel::ClassifyExtension(Lower(fs::path(abs).extension().string())) == AssetBrowserPanel::AssetType::Texture)
                {
                    std::error_code ec;
                    std::string rel = fs::relative(fs::path(abs), fs::path(AssetsRoot()), ec).generic_string();
                    if (ec || rel.empty() || rel.rfind("..", 0) == 0) rel = fs::path(abs).generic_string();
                    ng::Vec2 g = s.view.ToGraph(ImGui::GetIO().MousePos);
                    if (s.view.Settings().snap) g = ng::SnapVec(g, s.view.Settings().snapStep);
                    s.ed.Doc().History().BeginGroup("テクスチャを配置");
                    const ng::NodeId id = s.ed.Doc().AddNode("TextureSample", g);
                    if (id) s.ed.SetNodeProp(id, "texture", rel);
                    s.ed.Doc().History().EndGroup();
                    if (id) s.view.FocusNode(id, false);
                    Say(s, "TextureSample を置きました: " + PathLeaf(rel));
                }
                else Say(s, "テクスチャ（png / jpg / dds / tga / bmp）だけ置けます", true);
            }
            ImGui::EndDragDropTarget();
        }
        if (g_needFrame && (s.view.CanvasMax().x - s.view.CanvasMin().x) > 100.0f)
        {
            s.view.FrameAll(false);
            g_needFrame = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ctx.nodeGraphKeyFocus = ctx.nodeGraphKeyFocus || s.view.WantsKeyboard();   // シーン向けの Typing 系キー（Ctrl+Z / W E R T / F / Esc）を窓へ渡す

        if (s.showBottom)
        {
            const float sy0 = bodyY0 + canvasH;
            const float d = Splitter("##mgSplitB", ImVec2(cx0, sy0), ImVec2(cx1, sy0 + sp), false);
            if (d != 0.0f) s.bottomH = std::min(std::max(s.bottomH - d / ui::Scale(), 110.0f), 600.0f);
            const ImVec2 bmn(cx0, sy0 + sp);
            const ImVec2 bsize(cx1 - cx0, bottomPx);
            dl->AddRectFilled(bmn, ImVec2(bmn.x + bsize.x, bmn.y + bsize.y), paneBg);
            ImGui::SetCursorScreenPos(bmn);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##mgBottom", bsize, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
            DrawBottomPane(s, bsize);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }
    FlushLiveEdits(s);

    // ---- ステータスバー ----
    DrawStatusBar(s, ImVec2(origin.x, origin.y + H - stH), W, stH);

    // ---- ダイアログ ----
    DrawEditorDialogs(s);
    DrawFileDialogs(s);
    ImGui::End();
}

} // namespace MaterialGraphPanel

} // namespace dx12e
