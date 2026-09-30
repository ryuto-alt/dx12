// マテリアルグラフ窓: 下部（診断リスト・生成 HLSL ビューア）と、診断 → ノード表示（赤縁・黄縁・減光）。

#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/panels/MaterialGraphPanelInternal.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace dx12e::mgpanel
{

namespace
{
ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 ColA(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }

ImU32 TokColor(mg::HlslTok k)
{
    switch (k)
    {
    case mg::HlslTok::Keyword:   return Col(theme::TypeScript);
    case mg::HlslTok::Type:      return Col(theme::TypeCamera);
    case mg::HlslTok::Intrinsic: return Col(theme::TypeAudio);
    case mg::HlslTok::Engine:    return Col(theme::TypeLight);
    case mg::HlslTok::Number:    return Col(theme::TypeScene);
    case mg::HlslTok::Comment:   return Col(theme::TextFaint);
    case mg::HlslTok::Preproc:   return ColA(theme::TypeUi, 0.75f);
    case mg::HlslTok::Punct:     return Col(theme::TextDim);
    default:                     return Col(theme::Text);
    }
}

void RebuildHlsl(State& s)
{
    const mat::CompileResult& r = s.ed.Result();
    s.hlslCompileSeen = s.ed.CompileCount();
    if (r.ok && !r.hlsl.empty())
    {
        s.hlsl = r.hlsl;
        s.hlslStale = false;
    }
    else s.hlslStale = !s.hlsl.empty();
    s.hlslLines.clear();
    s.hlslSpans.clear();
    s.hlslLineNode.clear();
    std::istringstream ss(s.hlsl);
    std::string line;
    bool inBlock = false;
    while (std::getline(ss, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        s.hlslSpans.push_back(mg::ColorizeHlslLine(line, &inBlock));
        s.hlslLines.push_back(std::move(line));
    }
    s.hlslLineNode.assign(s.hlslLines.size(), 0);
    if (!s.hlslStale)
        for (const mat::SourceMapEntry& e : r.sourceMap)
        {
            const ng::NodeId id = s.ed.Model().IntOf(e.nodeId);
            for (int ln = e.firstLine; ln <= e.lastLine; ++ln)
                if (ln >= 1 && static_cast<size_t>(ln) <= s.hlslLineNode.size()) s.hlslLineNode[static_cast<size_t>(ln - 1)] = id;
        }
}

bool IsAscii(const std::string& s)
{
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 診断リスト
// ---------------------------------------------------------------------------
void DrawDiagnostics(State& s, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::vector<mg::DiagItem> all = s.ed.Diagnostics(true);
    int nErr = 0, nWarn = 0, nInfo = 0;
    for (const mg::DiagItem& d : all)
    {
        if (d.severity == mat::Severity::Error) ++nErr;
        else if (d.severity == mat::Severity::Warning) ++nWarn;
        else ++nInfo;
    }
    (void)size;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##mgDiagList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    int shown = 0;
    for (size_t i = 0; i < all.size(); ++i)
    {
        const mg::DiagItem& d = all[i];
        if (d.severity == mat::Severity::Error && !s.showErr) continue;
        if (d.severity == mat::Severity::Warning && !s.showWarn) continue;
        if (d.severity == mat::Severity::Info && !s.showInfo) continue;
        ++shown;
        const bool hasHint = !d.hint.empty();
        const float h = ui::Px(hasHint ? 46.0f : 30.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##diag", ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const bool sel = static_cast<int>(i) == s.diagCursor;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            s.diagCursor = static_cast<int>(i);
            const ng::NodeId target = s.ed.FocusNodeFor(d);
            if (target) s.view.FocusNode(target, true);
        }
        Anchor("row", "mg:diag:" + d.code + ":" + std::to_string(i));
        ImGui::PopID();
        const ImVec2 mx(p.x + w, p.y + h);
        ui::deco::RowFace(dl, p, mx, sel, hov, held);

        const ImVec4 sc = d.severity == mat::Severity::Error ? theme::Bad : d.severity == mat::Severity::Warning ? theme::Warn : theme::TextDim;
        const float cy = p.y + ui::Px(15.0f);
        ui::DrawIconCentered(dl, d.severity == mat::Severity::Info ? ICON_INFO : d.severity == mat::Severity::Warning ? ICON_WARN : ICON_ERROR,
                             ImVec2(p.x + ui::Px(20.0f), cy), Col(sc), ui::Px(15.0f));
        const float th = ImGui::GetTextLineHeight();
        // 右端: ノード名（・ピン）
        float rx = mx.x - ui::Px(12.0f);
        std::string where = d.nodeTitle.empty() ? std::string("グラフ全体") : d.nodeTitle;
        if (!d.pin.empty()) where += " · " + d.pin;
        const ImVec2 ws = ImGui::CalcTextSize(where.c_str());
        const float chipW = ws.x + ui::Px(16.0f);
        const ImVec2 cmn(rx - chipW, cy - ui::Px(10.0f)), cmx(rx, cy + ui::Px(10.0f));
        dl->AddRectFilled(cmn, cmx, ColA(theme::Bg3, 0.9f), ui::Px(10.0f));
        dl->AddText(ImVec2(cmn.x + ui::Px(8.0f), cy - th * 0.5f), Col(d.node ? theme::TextMid : theme::TextFaint), where.c_str());
        // 本文（右のチップにかからないよう切る）
        const ImVec4 clip(p.x + ui::Px(36.0f), p.y, cmn.x - ui::Px(10.0f), p.y + h);
        const ImU32 tc = d.reachable ? Col(theme::Text) : Col(theme::TextDim);
        dl->AddText(nullptr, 0.0f, ImVec2(p.x + ui::Px(36.0f), cy - th * 0.5f), tc, d.message.c_str(), nullptr, 0.0f, &clip);
        if (hasHint)
            dl->AddText(nullptr, 0.0f, ImVec2(p.x + ui::Px(36.0f), cy - th * 0.5f + th + ui::Px(3.0f)), Col(theme::TextFaint), d.hint.c_str(), nullptr, 0.0f, &clip);
        if (hov && ImGui::CalcTextSize(d.message.c_str()).x > clip.z - clip.x) ImGui::SetTooltip("%s%s%s", d.message.c_str(), hasHint ? "\n" : "", d.hint.c_str());
        // 区切り
        dl->AddLine(ImVec2(p.x + ui::Px(8.0f), mx.y - 0.5f), ImVec2(mx.x - ui::Px(8.0f), mx.y - 0.5f), ColA(theme::Border, 0.5f), 1.0f);
    }
    if (shown == 0)
    {
        const mat::CompileResult& r = s.ed.Result();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = std::max(ui::Px(80.0f), ImGui::GetContentRegionAvail().y);
        ImGui::Dummy(ImVec2(w, h));
        const bool ok = s.ed.HasResult() && r.ok;
        const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.42f);
        ui::DrawIconCentered(dl, ok ? ICON_CHECK : ICON_INFO, ImVec2(c.x, c.y - ui::Px(14.0f)), Col(ok ? theme::Good : theme::TextDim), ui::Px(26.0f));
        char buf[200];
        std::snprintf(buf, sizeof(buf), all.empty() ? "診断はありません — 生成 OK" : "表示中の診断はありません（フィルタで隠れています）");
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + ui::Px(6.0f)), Col(theme::TextMid), buf);
        if (ok)
        {
            std::snprintf(buf, sizeof(buf), "ノード %d  Op %d  スロット %d  HLSL %d 行  %.1f ms", r.stats.nodesReachable, r.stats.ops, r.slotCount, r.stats.hlslLines, s.ed.LastCompileMs());
            const ImVec2 ts2 = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(c.x - ts2.x * 0.5f, c.y + ui::Px(28.0f)), Col(theme::TextFaint), buf);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    (void)nInfo;
}

// ---------------------------------------------------------------------------
// HLSL ビューア
// ---------------------------------------------------------------------------
void DrawHlsl(State& s)
{
    if (s.hlslCompileSeen != s.ed.CompileCount()) RebuildHlsl(s);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (s.hlslStale)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = ui::Px(24.0f);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ColA(theme::Warn, 0.14f));
        ui::DrawIconCentered(dl, ICON_WARN, ImVec2(p.x + ui::Px(16.0f), p.y + h * 0.5f), Col(theme::Warn), ui::Px(13.0f));
        dl->AddText(ImVec2(p.x + ui::Px(30.0f), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), Col(theme::Warn),
                    "現在のグラフにはエラーがあります。直前に生成できた HLSL を薄く表示しています。");
        ImGui::Dummy(ImVec2(w, h));
    }

    // 選択ノードの行を強調・スクロール
    const ng::Selection& sel = s.view.GetSelection();
    const ng::NodeId selNode = sel.nodes.size() == 1 ? *sel.nodes.begin() : 0;
    if (selNode != s.hlslSelSeen)
    {
        s.hlslSelSeen = selNode;
        if (selNode && !s.hlslStale)
            for (size_t i = 0; i < s.hlslLineNode.size(); ++i)
                if (s.hlslLineNode[i] == selNode) { s.hlslScrollToLine = static_cast<int>(i); break; }
    }

    ui::PushMono();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##mgHlsl", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
    const float lh = ImGui::GetTextLineHeight() + ui::Px(3.0f);
    const float cw = ImGui::CalcTextSize("0").x;
    const int n = static_cast<int>(s.hlslLines.size());
    const float gutter = s.hlslLineNums ? cw * 5.0f : ui::Px(8.0f);
    if (s.hlslScrollToLine >= 0)
    {
        const float target = static_cast<float>(s.hlslScrollToLine) * lh - ImGui::GetWindowHeight() * 0.3f;
        ImGui::SetScrollY(std::max(0.0f, target));
        s.hlslScrollToLine = -1;
    }
    ImGuiListClipper clip;
    clip.Begin(n, lh);
    const float alpha = s.hlslStale ? 0.5f : 1.0f;
    float maxW = 0.0f;
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetWindowWidth());
            ImGui::PushID(i);
            ImGui::InvisibleButton("##ln", ImVec2(w, lh));
            const bool hov = ImGui::IsItemHovered();
            const ng::NodeId ln = s.hlslLineNode[static_cast<size_t>(i)];
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ln) s.view.FocusNode(ln, true);
            ImGui::PopID();
            const bool mine = selNode != 0 && ln == selNode;
            if (mine) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh), ColA(theme::Accent, 0.16f));
            else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh), ColA(theme::Text, 0.05f));
            if (mine) dl->AddRectFilled(p, ImVec2(p.x + ui::Px(2.0f), p.y + lh), Col(theme::Accent));
            const float ty = p.y + (lh - ImGui::GetTextLineHeight()) * 0.5f;
            if (s.hlslLineNums)
            {
                char nb[16];
                std::snprintf(nb, sizeof(nb), "%4d", i + 1);
                dl->AddText(ImVec2(p.x + ui::Px(6.0f), ty), Col(mine ? theme::AccentHover : theme::TextFaint), nb);
            }
            const std::string& line = s.hlslLines[static_cast<size_t>(i)];
            const bool ascii = IsAscii(line);
            const float x0 = p.x + gutter + ui::Px(6.0f);
            for (const mg::HlslSpan& sp : s.hlslSpans[static_cast<size_t>(i)])
            {
                float x = x0;
                if (ascii) x += cw * static_cast<float>(sp.begin);
                else x += ImGui::CalcTextSize(line.c_str(), line.c_str() + sp.begin).x;
                ImU32 col = TokColor(sp.kind);
                if (alpha < 1.0f) col = (col & 0x00FFFFFFu) | (static_cast<ImU32>(255.0f * alpha) << 24);
                dl->AddText(ImVec2(x, ty), col, line.c_str() + sp.begin, line.c_str() + sp.end);
            }
            maxW = std::max(maxW, ascii ? cw * static_cast<float>(line.size()) : ImGui::CalcTextSize(line.c_str()).x);
        }
    }
    clip.End();
    ImGui::Dummy(ImVec2(gutter + maxW + ui::Px(24.0f), 0.0f));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ui::PopMono();
}

} // namespace

// ---------------------------------------------------------------------------
// 公開
// ---------------------------------------------------------------------------
void CopyHlsl(State& s)
{
    if (s.hlslCompileSeen != s.ed.CompileCount()) RebuildHlsl(s);
    if (s.hlsl.empty()) { Say(s, "コピーできる HLSL がありません（エラーを直してください）", true); return; }
    ImGui::SetClipboardText(s.hlsl.c_str());
    Say(s, s.hlslStale ? "直前に生成できた HLSL をコピーしました" : "生成 HLSL をクリップボードへコピーしました");
}

void NextDiagnostic(State& s)
{
    const std::vector<mg::DiagItem> all = s.ed.Diagnostics(false);
    if (all.empty()) { Say(s, "診断はありません"); return; }
    s.showBottom = true;
    s.bottomTab = 0;
    s.diagCursor = (s.diagCursor + 1) % static_cast<int>(all.size());
    const mg::DiagItem& d = all[static_cast<size_t>(s.diagCursor)];
    const ng::NodeId target = s.ed.FocusNodeFor(d);
    if (target) s.view.FocusNode(target, true);
}

void ApplyNodeStates(State& s)
{
    const bool flash = s.now < s.compileFlashUntil;
    const uint64_t key = static_cast<uint64_t>(s.ed.CompileCount()) * 1000003ull + s.ed.Model().AnalysisTick() * 31ull + (flash ? 1ull : 0ull) + (s.ed.HasResult() ? 7ull : 0ull) +
                         s.ed.ExternalDiagRev() * 977ull + (s.gpuCompiling ? 3ull : 0ull);
    if (key == s.nodeStateKey) return;
    s.nodeStateKey = key;
    s.view.ClearNodeStates();
    struct Agg { mat::Severity sev = mat::Severity::Info; bool live = false; bool dead = false; std::string msg; };
    std::map<ng::NodeId, Agg> per;
    for (const mg::DiagItem& d : s.ed.Diagnostics(true))
    {
        if (!d.node) continue;
        Agg& a = per[d.node];
        if (!d.reachable) { a.dead = true; continue; }
        if (d.severity == mat::Severity::Info) continue;
        a.live = true;
        if (static_cast<int>(d.severity) > static_cast<int>(a.sev)) a.sev = d.severity;
        if (!a.msg.empty()) a.msg += "\n";
        a.msg += std::string(d.severity == mat::Severity::Error ? "エラー: " : "警告: ") + d.message;
        if (!d.hint.empty()) a.msg += "  → " + d.hint;
    }
    for (const auto& kv : per)
    {
        const Agg& a = kv.second;
        if (a.live && a.sev == mat::Severity::Error) s.view.SetNodeState(kv.first, ng::GraphView::NodeState::Error, a.msg);
        else if (a.live && a.sev == mat::Severity::Warning) s.view.SetNodeState(kv.first, ng::GraphView::NodeState::Warning, a.msg);
        else if (a.dead) s.view.SetNodeState(kv.first, ng::GraphView::NodeState::Dimmed, "出力に繋がっていません（生成には使われません）");
    }
    // 生成した直後 / GPU でコンパイル中（DXC + PSO）は、出力ノードのヘッダに光を流す（エラーが無いときだけ）
    if (flash || s.gpuCompiling)
    {
        const ng::NodeId out = s.ed.OutputNode();
        auto it = per.find(out);
        if (out && (it == per.end() || !it->second.live))
            s.view.SetNodeState(out, ng::GraphView::NodeState::Compiling, "コンパイル中…");
    }
}

void DrawBottomPane(State& s, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float hh = ui::Px(34.0f);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + hh), Col(theme::Bg2));
    dl->AddLine(ImVec2(p.x, p.y + hh - 0.5f), ImVec2(p.x + size.x, p.y + hh - 0.5f), Col(theme::Border), std::max(1.0f, ui::PxF(1.0f)));

    const std::vector<mg::DiagItem> all = s.ed.Diagnostics(true);
    int nErr = 0, nWarn = 0, nInfo = 0;
    for (const mg::DiagItem& d : all)
    {
        if (d.severity == mat::Severity::Error) ++nErr;
        else if (d.severity == mat::Severity::Warning) ++nWarn;
        else ++nInfo;
    }

    // ---- タブ ----
    ImGui::SetCursorScreenPos(ImVec2(p.x + ui::Px(8.0f), p.y + (hh - ui::Px(24.0f)) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(6.0f, 0.0f));
    char label[64];
    std::snprintf(label, sizeof(label), "診断  %d", nErr + nWarn);
    if (ui::Chip("##mgTabDiag", label, s.bottomTab == 0)) s.bottomTab = 0;
    Anchor("tab", "mg:tab:diag");
    ImGui::SameLine();
    std::snprintf(label, sizeof(label), "生成 HLSL  %d 行", s.ed.HasResult() ? s.ed.Result().stats.hlslLines : 0);
    if (ui::Chip("##mgTabHlsl", label, s.bottomTab == 1)) s.bottomTab = 1;
    Anchor("tab", "mg:tab:hlsl");
    ImGui::SameLine(0.0f, ui::Px(16.0f));

    if (s.bottomTab == 0)
    {
        // フィルタ
        std::snprintf(label, sizeof(label), "エラー %d", nErr);
        if (ui::Chip("##mgFErr", label, s.showErr)) s.showErr = !s.showErr;
        Anchor("chip", "mg:filter:error");
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "警告 %d", nWarn);
        if (ui::Chip("##mgFWarn", label, s.showWarn)) s.showWarn = !s.showWarn;
        Anchor("chip", "mg:filter:warning");
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "情報 %d", nInfo);
        if (ui::Chip("##mgFInfo", label, s.showInfo)) s.showInfo = !s.showInfo;
        Anchor("chip", "mg:filter:info");
    }
    else
    {
        if (ui::Chip("##mgHlslNums", "行番号", s.hlslLineNums)) s.hlslLineNums = !s.hlslLineNums;
        Anchor("chip", "mg:hlsl:lineNumbers");
        ImGui::SameLine();
        if (ui::Chip("##mgHlslCopy", "コピー", false)) CopyHlsl(s);
        Anchor("button", "mg:btn:copyHlsl");
        if (s.ed.HasResult() && s.ed.Result().ok)
        {
            char hb[64];
            std::snprintf(hb, sizeof(hb), "hash %016llx", static_cast<unsigned long long>(s.ed.Result().hash));
            ui::PushMono();
            const ImVec2 hs = ImGui::CalcTextSize(hb);
            dl->AddText(ImVec2(p.x + size.x - hs.x - ui::Px(12.0f), p.y + (hh - hs.y) * 0.5f), Col(theme::TextFaint), hb);
            ui::PopMono();
        }
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
    ImGui::BeginChild("##mgBottomBody", ImVec2(size.x, size.y - hh), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    if (s.bottomTab == 0) DrawDiagnostics(s, ImVec2(size.x, size.y - hh));
    else DrawHlsl(s);
    ImGui::EndChild();
}

} // namespace dx12e::mgpanel
