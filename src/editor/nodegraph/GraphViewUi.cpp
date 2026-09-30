// ノードグラフ ImGui ビュー: ポップアップ（検索パレット・コンテキストメニュー・値の編集・コメント名変更）。

#include "editor/nodegraph/GraphView.h"

#include "editor/EditorCommandTable.h"
#include "editor/EditorIcons.h"
#include "editor/UiWidgets.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>   // ClearActiveID（外部から検索語を差し替えるとき、入力欄の内部バッファを捨てる）
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dx12e::ng
{

namespace
{
ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

const char* ShortcutFor(const char* id)
{
    static std::string cache[32];
    static const char* keys[32];
    static int n = 0;
    for (int i = 0; i < n; ++i) if (keys[i] == id) return cache[i].empty() ? nullptr : cache[i].c_str();
    const cmd::Def* d = cmd::FindCommand(id);
    std::string s = d ? cmd::ChordLabel(*d) : std::string();
    if (n < 32) { keys[n] = id; cache[n] = s; ++n; return s.empty() ? nullptr : cache[n - 1].c_str(); }
    return nullptr;
}

const char* const kCommentColorNames[kCommentColors] = {"青", "ティール", "緑", "琥珀", "橙", "マゼンタ"};

// 見出し文字列を「一致した文字だけ強調」して描く（byteOffsets は一致した文字の先頭バイト位置。昇順）。
float DrawHighlighted(ImDrawList* dl, ImVec2 pos, const std::string& text, const std::vector<uint32_t>& offs, ImU32 col, ImU32 hi)
{
    float x = pos.x;
    size_t i = 0, oi = 0;
    while (i < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        const bool h = oi < offs.size() && offs[oi] == i;
        if (h) ++oi;
        const std::string ch = text.substr(i, len);
        dl->AddText(ImVec2(x, pos.y), h ? hi : col, ch.c_str());
        x += ImGui::CalcTextSize(ch.c_str()).x;
        i += len;
    }
    return x - pos.x;
}
} // namespace

// ---------------------------------------------------------------------------
// 検索パレット
// ---------------------------------------------------------------------------
void GraphView::OpenPaletteAt(Vec2 g)
{
    m_palGraphPos = g;
    m_palScreenPos = ImVec2(m_vp.ToScreen(g).x, m_vp.ToScreen(g).y);
    m_palHasPin = false;
    m_palFilter = PaletteFilter{};
    m_palQuery[0] = '\0';
    m_palCategory.clear();
    m_paletteRequest = true;
}

void GraphView::SetPaletteQuery(const std::string& q)
{
    std::snprintf(m_palQuery, sizeof(m_palQuery), "%s", q.c_str());
    m_palRefresh = true;
    m_palSel = 0;
    m_palQueryDirty = true;
}

bool GraphView::ConfirmPalette(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_palItems.size()) return false;
    CreateFromPalette(m_palItems[static_cast<size_t>(index)]);
    m_palCloseRequest = true;   // 閉じるのは DrawPalette（ポップアップの中）で行う
    return true;
}

void GraphView::CreateFromPalette(const PaletteItem& item)
{
    const NodeTypeDesc& t = m_doc->Model().NodeTypes()[static_cast<size_t>(item.typeIndex)];
    const TypeLayout& L = m_doc->Layout().For(t);
    Vec2 pos = m_palGraphPos;
    const bool connect = m_palHasPin && item.autoPin >= 0;
    if (connect)
    {
        // 新ノードの接続ピンが、ドロップした位置に来るように置く
        if (m_palPinOutput) pos = pos - Vec2(0.0f, L.inY[static_cast<size_t>(item.autoPin)]);
        else                pos = pos - Vec2(L.size.x, L.outY[static_cast<size_t>(item.autoPin)]);
    }
    else if (L.reroute) pos = pos - L.size * 0.5f;
    pos = SnapPos(pos);
    m_doc->History().BeginGroup("ノードを追加");
    const NodeId id = m_doc->AddNode(t.id, pos);
    if (id && connect)
    {
        if (m_palPinOutput) m_doc->Connect(m_palPin, PinRef{id, static_cast<uint16_t>(item.autoPin), false});
        else                m_doc->Connect(PinRef{id, static_cast<uint16_t>(item.autoPin), true}, m_palPin);
    }
    m_doc->History().EndGroup();
    if (id) { m_sel.Clear(); m_sel.nodes.insert(id); ++m_selRev; ShowBringToFront(m_sel); }
    m_palHasPin = false;
}

void GraphView::DrawPalette()
{
    if (m_paletteRequest)
    {
        ImGui::OpenPopup("##ngPalette");
        m_paletteRequest = false;
        m_palFocus = true;
        m_palRefresh = true;
        m_palSel = 0;
        m_palCats = PaletteCategories(m_doc->Model());
    }
    const float w = ui::Px(372.0f);
    ImVec2 pos = m_palScreenPos;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    pos.x = std::min(std::max(pos.x, vp->Pos.x + 4.0f), vp->Pos.x + vp->Size.x - w - 4.0f);
    pos.y = std::min(std::max(pos.y, vp->Pos.y + 4.0f), vp->Pos.y + vp->Size.y - ui::Px(470.0f));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
    ui::PushMenuStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ui::Px(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(6.0f, 6.0f));
    const bool open = ImGui::BeginPopup("##ngPalette", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (open)
    {
        m_paletteOpen = true;
        IGraphModel& model = m_doc->Model();

        // 見出し
        {
            ui::PushBold();
            ImGui::TextUnformatted("ノードを追加");
            ui::PopBold();
            ImGui::SameLine();
            if (m_palHasPin)
            {
                const PinTypeDesc& ti = model.PinTypeInfo(m_palFilter.pinType);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                ImGui::Text(" - %s %s", ti.name.c_str(), m_palPinOutput ? "の出力に繋がるノード" : "を出力できるノード");
                ImGui::PopStyleColor();
            }
            if (!m_palHasPin)   // ピンの絞り込み説明が出ているときは幅が足りないのでキー案内を省く
            {
                const char* hint = "↑↓ 選択   Enter 決定   Esc 閉じる";
                const float hw = ImGui::CalcTextSize(hint).x;
                ImGui::SameLine(ImGui::GetWindowWidth() - hw - ImGui::GetStyle().WindowPadding.x);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::TextUnformatted(hint);
                ImGui::PopStyleColor();
            }
        }

        // 検索欄（外部から語を差し替えたときは、入力欄の内部バッファ（アクティブ中は buf を上書きする）を捨てて読み直させる）
        if (m_palQueryDirty) { ImGui::ClearActiveID(); m_palQueryDirty = false; m_palFocus = true; }
        if (m_palFocus) { ImGui::SetKeyboardFocusHere(0); m_palFocus = false; }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ui::SearchField("##ngq", m_palQuery, sizeof(m_palQuery), "ノードを検索（名前・カテゴリ・日本語の別名）")) { m_palRefresh = true; m_palSel = 0; }

        // カテゴリのチップ
        {
            auto chip = [&](const char* label, bool active) -> bool
            {
                const ImVec2 ts = ImGui::CalcTextSize(label);
                const ImVec2 sz(ts.x + ui::Px(18.0f), ui::Px(22.0f));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(label);
                const bool clicked = ImGui::InvisibleButton("##chip", sz);
                ImGui::PopID();
                const bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mx(p.x + sz.x, p.y + sz.y);
                dl->AddRectFilled(p, mx, Col(active ? theme::Selection : (hov ? theme::Bg4 : theme::Bg3)), sz.y * 0.5f);
                if (active) dl->AddRect(p, mx, Col(theme::AccentHover), sz.y * 0.5f, 0, ui::PxF(1.0f));
                dl->AddText(ImVec2(p.x + (sz.x - ts.x) * 0.5f, p.y + (sz.y - ts.y) * 0.5f), Col(active ? theme::Text : theme::TextDim), label);
                return clicked;
            };
            const float avail = ImGui::GetContentRegionAvail().x;
            float used = 0.0f;
            auto place = [&](const char* label, bool active, const std::string& value)
            {
                const float cw = ImGui::CalcTextSize(label).x + ui::Px(18.0f) + ImGui::GetStyle().ItemSpacing.x;
                if (used > 0.0f && used + cw > avail) used = 0.0f; else if (used > 0.0f) ImGui::SameLine();
                used += cw;
                if (chip(label, active)) { m_palCategory = value; m_palRefresh = true; m_palSel = 0; }
            };
            place("すべて", m_palCategory.empty(), std::string());
            for (const std::string& c : m_palCats) place(c.c_str(), m_palCategory == c, c);
        }

        if (m_palRefresh)
        {
            PaletteFilter f = m_palFilter;
            f.query = m_palQuery;
            f.category = m_palCategory;
            m_palItems = SearchPalette(model, f);
            m_palRefresh = false;
            if (m_palSel >= static_cast<int>(m_palItems.size())) m_palSel = 0;
        }

        // キー操作
        const int n = static_cast<int>(m_palItems.size());
        bool scroll = false;
        if (n > 0)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { m_palSel = (m_palSel + 1) % n; scroll = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   { m_palSel = (m_palSel + n - 1) % n; scroll = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))  { m_palSel = std::min(n - 1, m_palSel + 8); scroll = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))    { m_palSel = std::max(0, m_palSel - 8); scroll = true; }
        }

        // 結果リスト
        const float rowH = ui::Px(26.0f);
        int confirm = -1;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##ngList", ImVec2(0.0f, std::min(ui::Px(300.0f), std::max(rowH * 2.0f, static_cast<float>(n) * rowH + ui::Px(4.0f)))), ImGuiChildFlags_None, 0);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (n == 0)
            {
                ImGui::Dummy(ui::Px(4.0f, 8.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::TextUnformatted("  一致するノードがありません");
                ImGui::PopStyleColor();
            }
            for (int i = 0; i < n; ++i)
            {
                const PaletteItem& it = m_palItems[static_cast<size_t>(i)];
                const NodeTypeDesc& t = model.NodeTypes()[static_cast<size_t>(it.typeIndex)];
                ImGui::PushID(i);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float rw = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##row", ImVec2(rw, rowH));
                const bool hov = ImGui::IsItemHovered();
                const bool held = ImGui::IsItemActive();
                if (hov && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) m_palSel = i;
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) confirm = i;
                const bool sel = (i == m_palSel);
                const ImVec2 mx(p.x + rw, p.y + rowH);
                ui::deco::RowFace(dl, p, mx, sel, hov, held);
                if (sel && scroll) ImGui::SetScrollHereY(0.5f);

                // カテゴリ色の小片・ノード名・カテゴリ・自動接続ピン
                const int cs = std::min(std::max(t.categorySlot, 0), kCategorySlots - 1);
                const GraphTokens& tok = m_tok;
                const float cy = p.y + rowH * 0.5f;
                dl->AddRectFilled(ImVec2(p.x + ui::Px(8.0f), cy - ui::Px(7.0f)), ImVec2(p.x + ui::Px(11.0f), cy + ui::Px(7.0f)), tok.headerCatBright[cs], ui::Px(1.5f));
                const float th = ImGui::GetTextLineHeight();
                DrawHighlighted(dl, ImVec2(p.x + ui::Px(18.0f), cy - th * 0.5f), t.title, it.titleHighlight, Col(theme::Text), Col(theme::AccentHover));
                const float cw = ImGui::CalcTextSize(t.category.c_str()).x;
                dl->AddText(ImVec2(mx.x - cw - ui::Px(10.0f), cy - th * 0.5f), Col(theme::TextFaint), t.category.c_str());
                if (t.hotkey)
                {
                    char hk[4] = {t.hotkey, 0, 0, 0};
                    const float hw = ImGui::CalcTextSize(hk).x;
                    const ImVec2 hp(mx.x - cw - hw - ui::Px(34.0f), cy - ui::Px(8.0f));
                    dl->AddRect(ImVec2(hp.x - ui::Px(4.0f), hp.y), ImVec2(hp.x + hw + ui::Px(4.0f), hp.y + ui::Px(16.0f)), Col(theme::InputBorder), ui::Px(3.0f), 0, ui::PxF(1.0f));
                    dl->AddText(ImVec2(hp.x, cy - th * 0.5f), Col(theme::TextDim), hk);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (m_palCloseRequest) { m_palCloseRequest = false; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();   // 入力欄が Esc を先に食っても 1 回で閉じる

        // Enter で決定（検索欄にフォーカスがあっても効く）
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) confirm = m_palSel;
        if (confirm >= 0 && confirm < n) { CreateFromPalette(m_palItems[static_cast<size_t>(confirm)]); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    else m_paletteOpen = false;
    ImGui::PopStyleVar(3);
    ui::PopMenuStyle();
}

// ---------------------------------------------------------------------------
// コンテキストメニュー
// ---------------------------------------------------------------------------
void GraphView::DrawContextMenus()
{
    if (m_ctxRequest)
    {
        ImGui::OpenPopup("##ngCtx");
        m_ctxRequest = false;
    }
    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##ngCtx", ImGuiWindowFlags_NoSavedSettings))
    {
        m_ctxOpen = true;
        auto item = [&](const char* icon, const char* label, const char* cmdId, bool enabled = true) -> bool
        {
            if (ui::MenuItem(icon, label, cmdId ? ShortcutFor(cmdId) : nullptr, false, enabled))
            {
                if (cmdId) Execute(cmdId);
                return true;
            }
            return false;
        };
        const bool multi = m_sel.nodes.size() >= 2;
        switch (m_ctxKind)
        {
        case CtxKind::Node:
        case CtxKind::Selection:
            item(ICON_T_LAYERS, "複製", "edit.duplicate");
            item(ICON_COPY, "コピー", "edit.copy");
            item(ICON_PASTE, "貼り付け", "edit.paste");
            item(ICON_TRASH, "削除", "edit.delete");
            ImGui::Separator();
            item(ICON_ARROW_RIGHT, "上流のノードを選択", "graph.selectUpstream");
            item(ICON_ARROW_RIGHT, "下流のノードを選択", "graph.selectDownstream");
            ImGui::Separator();
            item(ICON_SQUARE_PLUS, "コメントで囲む", "graph.comment");
            item(ICON_LINK, "ワイヤをすべて切断", "graph.disconnectSelected");
            if (ImGui::BeginMenuEx("整列", ICON_T_SLIDERS, multi))
            {
                item(ICON_BLANK, "左揃え", "graph.alignLeft");
                item(ICON_BLANK, "右揃え", "graph.alignRight");
                item(ICON_BLANK, "上揃え", "graph.alignTop");
                item(ICON_BLANK, "下揃え", "graph.alignBottom");
                item(ICON_BLANK, "水平中央揃え", "graph.alignCenterH");
                item(ICON_BLANK, "垂直中央揃え", "graph.alignCenterV");
                item(ICON_BLANK, "水平に等間隔", "graph.distributeH", m_sel.nodes.size() >= 3);
                item(ICON_BLANK, "垂直に等間隔", "graph.distributeV", m_sel.nodes.size() >= 3);
                ImGui::EndMenu();
            }
            break;
        case CtxKind::Pin:
            if (ui::MenuItem(ICON_LINK, "このピンのワイヤをすべて切断", "Alt+クリック", false, true)) m_doc->DisconnectPin(m_ctxHit.pin);
            break;
        case CtxKind::Edge:
            if (ui::MenuItem(ICON_TRASH, "ワイヤを削除", nullptr, false, true)) m_doc->Disconnect(m_ctxHit.edge.to);
            if (ui::MenuItem(ICON_PLUS, "リルート点を挿入", "ダブルクリック", false, m_doc->Model().RerouteTypeId() != nullptr))
                m_doc->InsertReroute(m_ctxHit.edge, SnapPos(m_ctxG));
            break;
        case CtxKind::Comment:
        {
            if (ui::MenuItem(ICON_FILE_TEXT, "名前を変更", "ダブルクリック", false, true)) StartRename(m_ctxHit.comment);
            if (ImGui::BeginMenuEx("色", ICON_T_MATERIAL, true))
            {
                const Comment* cur = m_doc->FindComment(m_ctxHit.comment);
                const GraphTokens& tok = m_tok;
                for (int i = 0; i < kCommentColors; ++i)
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float sw = ui::Px(12.0f);
                    if (ui::MenuItem(ICON_BLANK, kCommentColorNames[i], nullptr, cur && cur->color == i, true) && cur)
                    {
                        Comment n = *cur; n.color = i; m_doc->EditComment(cur->id, n);
                    }
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + ui::Px(5.0f), p.y + ui::Px(5.0f)), ImVec2(p.x + ui::Px(5.0f) + sw, p.y + ui::Px(5.0f) + sw),
                                                              tok.commentBorder[i] | 0xFF000000u, ui::Px(3.0f));
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ui::MenuItem(ICON_TRASH, "コメントを削除", "Del", false, true))
            {
                Selection s; s.comments.insert(m_ctxHit.comment);
                m_doc->RemoveItems(s);
                m_sel.comments.erase(m_ctxHit.comment);
            }
            break;
        }
        case CtxKind::None: break;
        }
        ImGui::EndPopup();
    }
    else m_ctxOpen = false;
    ui::PopMenuStyle();
}

// ---------------------------------------------------------------------------
// 値の編集・コメント名変更（等倍の ImGui ポップアップ）
// ---------------------------------------------------------------------------
void GraphView::DrawEditPopups()
{
    IGraphModel& model = m_doc->Model();

    // ---- 値欄 ----
    if (m_editRequest)
    {
        ImGui::OpenPopup("##ngEdit");
        m_editRequest = false;
    }
    if (m_editKind != EditKind::None)
    {
        const NodeData* n = model.FindNode(m_editPin.node);
        if (!n || m_editPin.index >= n->desc->inputs.size()) { ImGui::CloseCurrentPopup(); m_editKind = EditKind::None; }
        else
        {
            const PinDesc& pd = n->desc->inputs[m_editPin.index];
            // ポップアップは値欄のすぐ下へ
            const Rect sr = m_vp.ToScreen(m_doc->Layout().SlotRect(*n, m_editPin.index));
            ImGui::SetNextWindowPos(ImVec2(sr.min.x, sr.max.y + ui::Px(4.0f)), ImGuiCond_Appearing);
            ui::PushMenuStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(8.0f, 8.0f));
            const bool open = ImGui::BeginPopup("##ngEdit", ImGuiWindowFlags_NoSavedSettings);
            if (open)
            {
                if (m_editKind == EditKind::Text)
                {
                    ImGui::SetNextItemWidth(ui::Px(120.0f));
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    const bool enter = ImGui::InputText("##v", m_editBuf, sizeof(m_editBuf),
                                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsScientific);
                    if (enter)
                    {
                        char* end = nullptr;
                        const float v = std::strtof(m_editBuf, &end);
                        if (end != m_editBuf)
                        {
                            PinValue nv = m_editOld;
                            nv.f[std::min(m_editComp, 3)] = std::min(std::max(v, pd.rangeMin), pd.rangeMax);
                            m_doc->SetPinValue(m_editPin, nv);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }
                else if (m_editKind == EditKind::Enum)
                {
                    const int cur = static_cast<int>(m_editOld.f[0]);
                    for (size_t i = 0; i < pd.enumOptions.size(); ++i)
                    {
                        if (ImGui::Selectable(pd.enumOptions[i].c_str(), static_cast<int>(i) == cur, 0, ImVec2(ui::Px(150.0f), 0.0f)))
                        {
                            PinValue nv = m_editOld;
                            nv.f[0] = static_cast<float>(i);
                            m_doc->SetPinValue(m_editPin, nv);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                else   // Color
                {
                    float col[4] = {n->values[m_editPin.index].f[0], n->values[m_editPin.index].f[1], n->values[m_editPin.index].f[2], n->values[m_editPin.index].f[3]};
                    if (ImGui::ColorPicker4("##c", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_PickerHueBar))
                    {
                        PinValue nv = m_editOld;
                        for (int i = 0; i < 4; ++i) nv.f[i] = col[i];
                        m_doc->SetPinValueLive(m_editPin, nv);
                    }
                }
                ImGui::EndPopup();
            }
            else
            {
                // 閉じた: 色はライブ編集を 1 コマンドに確定
                if (m_editKind == EditKind::Color) m_doc->CommitPinValue(m_editPin, m_editOld);
                m_editKind = EditKind::None;
            }
            ImGui::PopStyleVar();
            ui::PopMenuStyle();
        }
    }

    // ---- コメント名変更 ----
    if (m_renameRequest)
    {
        ImGui::OpenPopup("##ngRename");
        m_renameRequest = false;
    }
    if (m_renameComment != 0)
    {
        const Comment* c = m_doc->FindComment(m_renameComment);
        if (!c) { m_renameComment = 0; }
        else
        {
            const Rect rs = m_vp.ToScreen(c->rect);
            ImGui::SetNextWindowPos(ImVec2(rs.min.x, rs.min.y), ImGuiCond_Appearing);
            ui::PushMenuStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(8.0f, 8.0f));
            if (ImGui::BeginPopup("##ngRename", ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::SetNextItemWidth(std::max(ui::Px(220.0f), rs.max.x - rs.min.x - ui::Px(16.0f)));
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##t", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                {
                    Comment n = *c;
                    n.title = m_renameBuf;
                    m_doc->EditComment(c->id, n);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            else m_renameComment = 0;
            ImGui::PopStyleVar();
            ui::PopMenuStyle();
        }
    }
}

} // namespace dx12e::ng
