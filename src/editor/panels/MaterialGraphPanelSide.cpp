// マテリアルグラフ窓: 左（ノードパレット）と右（詳細）、Custom / テクスチャのモーダル。

#include "core/PathResolver.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/FuzzyMatch.h"
#include "editor/PropertyGrid.h"
#include "editor/UiWidgets.h"
#include "editor/nodegraph/GraphPalette.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "editor/panels/MaterialGraphPanelInternal.h"
#include "gui/VirtualInputImGui.h"
#include "input/VirtualInput.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>   // GetCurrentTable（テーブルの行全体を当たりにする）
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace dx12e::mgpanel
{

namespace
{
namespace fs = std::filesystem;

ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 ColA(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string AssetsRoot()
{
    std::string a = PathResolver::AssetsDir();
    if (a.empty()) a = (fs::temp_directory_path() / "uno_matgraph/").string();
    return a;
}

// ---------------------------------------------------------------------------
// パレット
// ---------------------------------------------------------------------------
void AddAtCenter(State& s, const std::string& typeId)
{
    const ImVec2 mn = s.view.CanvasMin(), mx = s.view.CanvasMax();
    ng::Vec2 g = s.view.ToGraph(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));
    const ng::NodeTypeDesc* t = s.ed.Model().FindNodeType(typeId);
    if (!t) return;
    const ng::TypeLayout& L = s.ed.Doc().Layout().For(*t);
    g = g + ng::Vec2(24.0f, 24.0f) * static_cast<float>(s.paletteCascade % 8);
    ++s.paletteCascade;
    g = g - (L.reroute ? L.size * 0.5f : ng::Vec2(L.size.x * 0.5f, L.size.y * 0.5f));
    if (s.view.Settings().snap) g = ng::SnapVec(g, s.view.Settings().snapStep);
    const ng::NodeId id = s.ed.Doc().AddNode(typeId, g);
    if (id) s.view.FocusNode(id, false);
}

// 1 行（ノード型）。戻り値 = クリックされたか
void PaletteRow(State& s, const ng::NodeTypeDesc& t, int typeIndex, const std::vector<uint32_t>* hl, bool showCategory)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ui::Px(26.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(typeIndex);
    ImGui::InvisibleButton("##row", ImVec2(w, rowH));
    const bool hov = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 mx(p.x + w, p.y + rowH);
    ui::deco::RowFace(dl, p, mx, s.palHover == typeIndex && false, hov, held);
    const float cy = p.y + rowH * 0.5f;
    dl->AddRectFilled(ImVec2(p.x + ui::Px(10.0f), cy - ui::Px(7.0f)), ImVec2(p.x + ui::Px(13.0f), cy + ui::Px(7.0f)), CategoryColor(t.categorySlot), ui::Px(1.5f));
    const float th = ImGui::GetTextLineHeight();
    float x = p.x + ui::Px(21.0f);
    if (hl && !hl->empty())
    {
        // 一致した文字だけ強調
        size_t i = 0, oi = 0;
        while (i < t.title.size())
        {
            const unsigned char c = static_cast<unsigned char>(t.title[i]);
            const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            const bool h = oi < hl->size() && (*hl)[oi] == i;
            if (h) ++oi;
            const std::string ch = t.title.substr(i, len);
            dl->AddText(ImVec2(x, cy - th * 0.5f), h ? Col(theme::AccentHover) : Col(theme::Text), ch.c_str());
            x += ImGui::CalcTextSize(ch.c_str()).x;
            i += len;
        }
    }
    else dl->AddText(ImVec2(x, cy - th * 0.5f), Col(theme::Text), t.title.c_str());
    float rx = mx.x - ui::Px(10.0f);
    if (t.hotkey)
    {
        const char hk[2] = {t.hotkey, 0};
        const ImVec2 ks = ImGui::CalcTextSize(hk);
        const float bw = std::max(ks.x + ui::Px(10.0f), ui::Px(18.0f)), bh = ui::Px(17.0f);
        const ImVec2 bmn(rx - bw, cy - bh * 0.5f), bmx(rx, cy + bh * 0.5f);
        dl->AddRect(bmn, bmx, Col(theme::InputBorder), ui::Px(3.0f), 0, std::max(1.0f, ui::PxF(1.0f)));
        dl->AddText(ImVec2(bmn.x + (bw - ks.x) * 0.5f, cy - th * 0.5f), Col(theme::TextDim), hk);
        rx -= bw + ui::Px(8.0f);
    }
    if (showCategory)
    {
        const ImVec2 cs = ImGui::CalcTextSize(t.category.c_str());
        dl->AddText(ImVec2(rx - cs.x, cy - th * 0.5f), Col(theme::TextFaint), t.category.c_str());
    }
    // 操作: ダブルクリック = 中央に追加 / ドラッグ = キャンバスへドロップ
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AddAtCenter(s, t.id);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        ImGui::SetDragDropPayload("MG_NODE_TYPE", t.id.c_str(), t.id.size() + 1);
        ui::PushBold();
        ImGui::TextUnformatted(t.title.c_str());
        ui::PopBold();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::TextUnformatted("キャンバスへドロップして追加");
        ImGui::PopStyleColor();
        ImGui::EndDragDropSource();
    }
    else if (hov && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsMouseDown(0))
    {
        ImGui::BeginTooltip();
        ui::PushBold();
        ImGui::TextUnformatted(t.title.c_str());
        ui::PopBold();
        ImGui::PushTextWrapPos(ui::Px(320.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMid);
        ImGui::TextUnformatted(t.description.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        ImGui::Text("ダブルクリック = 中央に追加   ドラッグ = 配置%s", t.hotkey ? "   キー + クリック = その場に配置" : "");
        ImGui::PopStyleColor();
        ImGui::EndTooltip();
    }
    Anchor("row", "mg:pal:" + t.id);
    ImGui::PopID();
}

} // namespace

void DrawPalettePane(State& s, ImVec2 size)
{
    const ng::IGraphModel& model = s.ed.Model();
    char right[32];
    std::snprintf(right, sizeof(right), "%zu 種", model.NodeTypes().size());
    const float hh = PaneHeader("ノード", right);

    const ImVec2 base = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(base.x + ui::Px(8.0f), base.y + ui::Px(8.0f)));
    ImGui::SetNextItemWidth(size.x - ui::Px(16.0f));
    ui::SearchField("##mgpsearch", s.palQuery, sizeof(s.palQuery), "ノードを検索（名前・日本語の別名）");
    Anchor("input", "mg:pal:search");
    const float top = ui::Px(8.0f) + ImGui::GetFrameHeight() + ui::Px(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + top));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##mgPalList", ImVec2(size.x, size.y - hh - top), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (!s.palOpenInit)
    {
        for (const char* c : {"パラメータ", "テクスチャ", "数学"}) s.palOpen.insert(c);
        s.palOpenInit = true;
    }
    const std::vector<ng::NodeTypeDesc>& types = model.NodeTypes();
    if (s.palQuery[0])
    {
        ng::PaletteFilter f;
        f.query = s.palQuery;
        const std::vector<ng::PaletteItem> items = ng::SearchPalette(model, f, 200);
        if (items.empty())
        {
            ImGui::Dummy(ui::Px(4.0f, 10.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
            ImGui::Indent(ui::Px(12.0f));
            ImGui::TextUnformatted("一致するノードがありません");
            ImGui::Unindent(ui::Px(12.0f));
            ImGui::PopStyleColor();
        }
        for (const ng::PaletteItem& it : items)
            PaletteRow(s, types[static_cast<size_t>(it.typeIndex)], it.typeIndex, &it.titleHighlight, true);
    }
    else
    {
        for (const std::string& cat : ng::PaletteCategories(model))
        {
            int count = 0, slot = 0;
            for (const ng::NodeTypeDesc& t : types) if (t.category == cat) { ++count; slot = t.categorySlot; }
            // カテゴリ見出し（クリックで開閉）
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = ui::Px(26.0f);
            ImGui::PushID(cat.c_str());
            ImGui::InvisibleButton("##cat", ImVec2(w, h));
            const bool hov = ImGui::IsItemHovered();
            const bool open = s.palOpen.count(cat) != 0;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { if (open) s.palOpen.erase(cat); else s.palOpen.insert(cat); }
            Anchor("header", "mg:palcat:" + cat);
            ImGui::PopID();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? Col(theme::Bg3) : Col(theme::Bg2));
            const float cy = p.y + h * 0.5f, th = ImGui::GetTextLineHeight();
            ui::DrawIconCentered(dl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(p.x + ui::Px(14.0f), cy), Col(theme::TextDim), ui::Px(12.0f));
            dl->AddRectFilled(ImVec2(p.x + ui::Px(26.0f), cy - ui::Px(6.0f)), ImVec2(p.x + ui::Px(29.0f), cy + ui::Px(6.0f)), CategoryColor(slot), ui::Px(1.5f));
            ui::PushBold();
            dl->AddText(ImVec2(p.x + ui::Px(36.0f), cy - th * 0.5f), Col(theme::Text), cat.c_str());
            ui::PopBold();
            char cbuf[16];
            std::snprintf(cbuf, sizeof(cbuf), "%d", count);
            const ImVec2 cs = ImGui::CalcTextSize(cbuf);
            dl->AddText(ImVec2(p.x + w - cs.x - ui::Px(12.0f), cy - th * 0.5f), Col(theme::TextFaint), cbuf);
            if (!open) continue;
            for (size_t i = 0; i < types.size(); ++i)
                if (types[i].category == cat) PaletteRow(s, types[i], static_cast<int>(i), nullptr, false);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// 連続編集
// ---------------------------------------------------------------------------
namespace
{
void BeginLive(State& s, ng::NodeId id, const std::string& key, const mat::Json& cur)
{
    if (s.pe.live && s.pe.node == id && s.pe.key == key) return;
    FlushLiveEdits(s);
    // FlushLiveEdits は「操作中」なら確定しない。別の欄へ移ったときは強制的に確定する
    if (s.pe.live) { s.ed.CommitProp(s.pe.node, s.pe.key, s.pe.old); s.pe.live = false; }
    s.pe.live = true; s.pe.node = id; s.pe.key = key; s.pe.old = cur;
}

std::string LabelOf(const mat::PropDecl& pd)
{
    if (pd.name == "samplerState") return "Sampler";
    if (pd.name == "outputType") return "出力型";
    if (pd.name == "srgb") return "sRGB";
    if (pd.name == "inline") return "inline";
    std::string s = pd.name;
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

void DrawPropRow(State& s, ng::NodeId id, const mat::NodeDef& def, const mat::PropDecl& pd)
{
    mat::Json cur = s.ed.Model().GetNodeProp(id, pd.name);
    if (cur.is_null()) cur = pd.defaultValue;
    const std::string label = LabelOf(pd);
    const char* tip = pd.desc.empty() ? nullptr : pd.desc.c_str();
    bool changed = false;
    mat::Json nv;
    bool active = false;
    switch (pd.type)
    {
    case mat::PropType::Float:
    {
        float v = cur.is_number() ? cur.get<float>() : 0.0f;
        const bool ranged = pd.minValue != 0.0f || pd.maxValue != 0.0f;
        changed = pg::Float(label.c_str(), &v, 0.01f, ranged ? pd.minValue : 0.0f, ranged ? pd.maxValue : 0.0f, "%.3f", &active, tip);
        nv = static_cast<double>(v);
        break;
    }
    case mat::PropType::Int:
    {
        int v = cur.is_number() ? cur.get<int>() : 0;
        changed = pg::Int(label.c_str(), &v, 1.0f, 0, 0, &active, tip);
        nv = static_cast<int64_t>(v);
        break;
    }
    case mat::PropType::Bool:
    {
        bool v = cur.is_boolean() && cur.get<bool>();
        changed = pg::Checkbox(label.c_str(), &v, tip);
        nv = v;
        break;
    }
    case mat::PropType::Vec2: case mat::PropType::Vec3: case mat::PropType::Vec4: case mat::PropType::Color:
    {
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const int n = pd.type == mat::PropType::Vec2 ? 2 : pd.type == mat::PropType::Vec3 ? 3 : 4;
        if (cur.is_array()) for (size_t k = 0; k < cur.size() && k < 4; ++k) if (cur[k].is_number()) v[k] = cur[k].get<float>();
        const bool asColor = pd.type == mat::PropType::Color || (pd.type == mat::PropType::Vec3 && def.type == "Float3");
        if (asColor)
        {
            pg::Label(label.c_str(), tip);
            ImGui::PushID(label.c_str());
            changed = pd.type == mat::PropType::Vec3 ? ui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float) : ui::ColorEdit4("##v", v, ImGuiColorEditFlags_Float);
            ImGui::PopID();
        }
        else changed = pg::FloatN(label.c_str(), v, n, 0.01f, 0.0f, 0.0f, "%.3f", &active, tip);
        nv = mat::Json::array();
        for (int k = 0; k < n; ++k) nv.push_back(static_cast<double>(v[k]));
        break;
    }
    case mat::PropType::Enum:
    {
        std::vector<const char*> items;
        int idx = 0;
        const std::string curS = cur.is_string() ? cur.get<std::string>() : std::string();
        for (size_t k = 0; k < pd.enumValues.size(); ++k)
        {
            items.push_back(pd.enumValues[k].c_str());
            if (pd.enumValues[k] == curS) idx = static_cast<int>(k);
        }
        changed = pg::Combo(label.c_str(), &idx, items.data(), static_cast<int>(items.size()), tip);
        if (changed) nv = pd.enumValues[static_cast<size_t>(idx)];
        break;
    }
    case mat::PropType::String:
    {
        std::string v = cur.is_string() ? cur.get<std::string>() : std::string();
        changed = pg::InputTextStr(label.c_str(), v, &active, tip);
        nv = v;
        break;
    }
    case mat::PropType::Text:
    {
        pg::Label(label.c_str(), tip);
        ImGui::PushID(label.c_str());
        std::string first = cur.is_string() ? cur.get<std::string>() : std::string();
        const size_t nl = first.find_first_of("\r\n");
        if (nl != std::string::npos) first.resize(nl);
        if (ui::PrimaryButton("編集…", ui::Px(70.0f, 0.0f))) OpenCustomEditor(s, id);
        Anchor("button", "mg:btn:editCode");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ui::PushMono();
        ImGui::TextUnformatted(first.c_str());
        ui::PopMono();
        ImGui::PopStyleColor();
        ImGui::PopID();
        break;
    }
    case mat::PropType::Texture:
    {
        std::string v = cur.is_string() ? cur.get<std::string>() : std::string();
        pg::Label(label.c_str(), tip);
        ImGui::PushID(label.c_str());
        const float btnW = ui::Px(28.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - ui::Px(4.0f));
        char buf[260] = {};
        v.copy(buf, sizeof(buf) - 1);
        if (ui::InputTextWithHint("##v", "テクスチャのパス（assets 相対）", buf, sizeof(buf))) { changed = true; nv = std::string(buf); }
        active = ImGui::IsItemActive();
        Anchor("input", "mg:prop:" + pd.name);
        ImGui::SameLine(0.0f, ui::Px(4.0f));
        if (ImGui::Button("...", ImVec2(btnW, 0.0f))) OpenTexturePicker(s, id, pd.name.c_str());
        ImGui::PopID();
        break;
    }
    }
    (void)active;
    if (changed)
    {
        BeginLive(s, id, pd.name, cur);
        s.ed.SetPropLive(id, pd.name, nv);
    }
}

// テーブルの「今の行」（ImGui::TableNextRow 直後・最初の列の位置で呼ぶ）を、行全体で押せる当たりにする。
// 列をまたぐ Selectable は、後ろの列の項目（色見本など）に押しを奪われて反応しないので、行の矩形を自前で取る。
// ホバー背景も塗る。戻り値 = 左クリックされた。anchor があれば仮想入力の名前つき要素として行の矩形を登録する。
bool ClickableRow(const char* anchor, bool* outHovered = nullptr)
{
    ImGuiTable* tb = ImGui::GetCurrentTable();
    if (!tb) return false;
    const float padY = ImGui::GetStyle().CellPadding.y;
    const float y0 = ImGui::GetCursorScreenPos().y - padY;
    const ImVec2 mn(tb->OuterRect.Min.x, y0), mx(tb->OuterRect.Max.x, y0 + ImGui::GetTextLineHeight() + padY * 2.0f);
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_None) && ImGui::IsMouseHoveringRect(mn, mx) && !ImGui::IsAnyItemActive();
    if (hov) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ColA(theme::Accent, 0.13f));
    if (anchor && vinput::Enabled() && ImGui::IsRectVisible(mn, mx))
    {
        vinput::Anchor a;
        a.kind = "row";
        a.label = anchor;
        a.window = ImGui::GetCurrentWindow()->RootWindow->Name;
        a.x0 = mn.x; a.y0 = mn.y; a.x1 = mx.x; a.y1 = mx.y;
        vinput::Global().AddAnchor(std::move(a));
    }
    if (outHovered) *outHovered = hov;
    return hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// 入力ピンの接続状況（ノードの入力 / 出力ノードのピン）
bool HasPinRows(const ng::NodeData& n)
{
    for (const ng::PinDesc& p : n.desc->inputs) if (!p.propertyOnly) return true;
    return false;
}

void DrawPinTable(State& s, ng::NodeId id, const char* tableId)
{
    const ng::IGraphModel& m = s.ed.Model();
    const ng::NodeData* n = m.FindNode(id);
    if (!n || !n->desc) return;
    const std::string sid = s.ed.Model().StrOf(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ui::Px(6.0f, 3.0f));
    if (!ImGui::BeginTable(tableId, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn("##pin", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::TableSetupColumn("##src", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    for (size_t i = 0; i < n->desc->inputs.size(); ++i)
    {
        const ng::PinDesc& pd = n->desc->inputs[i];
        if (pd.propertyOnly) continue;
        const ng::PinRef pin{id, static_cast<uint16_t>(i), false};
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMid);
        ImGui::TextUnformatted(pd.name.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        const ng::PinType rt = m.ResolvedPinType(pin);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        ImGui::TextUnformatted(m.PinTypeInfo(rt).name.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        if (const ng::Edge* e = m.FindEdgeTo(pin))
        {
            const ng::NodeData* src = m.FindNode(e->from.node);
            // 名前つき（パラメータ）は名前を、それ以外はノード名（複数出力なら .ピン名）を出す
            std::string t = src && src->desc ? src->desc->title : std::string("?");
            if (src && src->desc && e->from.index < src->desc->outputs.size() && src->desc->outputs.size() > 1)
                t += "." + src->desc->outputs[e->from.index].name;
            const mat::NodeDef* srcDef = s.ed.Model().DefOf(e->from.node);
            if (src && !src->label.empty() && srcDef && srcDef->isParameter) t = src->label + (src->desc->outputs.size() > 1 && e->from.index < src->desc->outputs.size() ? "." + src->desc->outputs[e->from.index].name : std::string());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(t.c_str(), false, ImGuiSelectableFlags_None)) s.view.FocusNode(e->from.node, true);
            Anchor("row", "mg:pinsrc:" + pd.name);
            ImGui::PopID();
        }
        else
        {
            const mat::InputBinding* b = s.ed.Graph().Binding(sid, s.ed.Model().NameOf(pin).pin);
            const mat::PinDecl* decl = s.ed.Model().InputDecl(*n->desc, i);
            char buf[64] = {};
            if (b && b->literal)
            {
                const mat::Value& v = *b->literal;
                if (v.type == mat::ValueType::Bool) std::snprintf(buf, sizeof(buf), "%s", v.b ? "true" : "false");
                else if (v.type == mat::ValueType::Int) std::snprintf(buf, sizeof(buf), "%d", v.i);
                else if (mat::Dim(v.type) == 1) std::snprintf(buf, sizeof(buf), "%.3g", static_cast<double>(v.f[0]));
                else if (mat::Dim(v.type) == 2) std::snprintf(buf, sizeof(buf), "(%.2g, %.2g)", static_cast<double>(v.f[0]), static_cast<double>(v.f[1]));
                else if (mat::Dim(v.type) == 3) std::snprintf(buf, sizeof(buf), "(%.2g, %.2g, %.2g)", static_cast<double>(v.f[0]), static_cast<double>(v.f[1]), static_cast<double>(v.f[2]));
                else std::snprintf(buf, sizeof(buf), "(%.2g, %.2g, %.2g, %.2g)", static_cast<double>(v.f[0]), static_cast<double>(v.f[1]), static_cast<double>(v.f[2]), static_cast<double>(v.f[3]));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::Text);
                ImGui::Text("%s", buf);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::TextUnformatted("値");
                ImGui::PopStyleColor();
            }
            else if (decl && !decl->defaultBuiltin.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                ImGui::Text("既定: %s", decl->defaultBuiltin == "uv" ? "メッシュの UV" : decl->defaultBuiltin.c_str());
                ImGui::PopStyleColor();
            }
            else if (decl && decl->implicitTexture)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                ImGui::TextUnformatted("ノードのテクスチャ");
                ImGui::PopStyleColor();
            }
            else if (pd.valueKind != ng::ValueKind::None)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                ImGui::TextUnformatted("既定値");
                ImGui::PopStyleColor();
            }
            else
            {
                const bool required = decl && decl->required;
                ImGui::PushStyleColor(ImGuiCol_Text, required ? theme::Bad : theme::TextFaint);
                ImGui::TextUnformatted(required ? "未接続（必須）" : "未接続");
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

const char* const kBlendModes[] = {"Opaque", "Masked", "Translucent", "Additive"};
const char* const kShadingModels[] = {"DefaultLit", "Unlit"};

int IndexOf(const char* const* list, int n, const std::string& v)
{
    for (int i = 0; i < n; ++i) if (v == list[i]) return i;
    return 0;
}

} // namespace

void FlushLiveEdits(State& s)
{
    // どの入力欄も操作されていない = 連続編集が終わった → 1 コマンドに確定する
    if (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (s.pe.live)
    {
        s.ed.CommitProp(s.pe.node, s.pe.key, s.pe.old);
        s.pe.live = false;
    }
    if (s.settingsLive)
    {
        s.ed.CommitSettings(s.settingsOld);
        s.settingsLive = false;
    }
}

void OpenCustomEditor(State& s, ng::NodeId id)
{
    const mat::Json code = s.ed.Model().GetNodeProp(id, "code");
    const mat::Json ot = s.ed.Model().GetNodeProp(id, "outputType");
    s.customNode = id;
    s.codeBuf.assign(16384, '\0');
    const std::string c = code.is_string() ? code.get<std::string>() : std::string();
    std::snprintf(s.codeBuf.data(), s.codeBuf.size(), "%s", c.c_str());
    static const char* const kTypes[] = {"float", "float2", "float3", "float4"};
    s.customOutType = ot.is_string() ? IndexOf(kTypes, 4, ot.get<std::string>()) : 0;
    s.customRequest = true;
}

void OpenTexturePicker(State& s, ng::NodeId id, const char* key)
{
    s.texNode = id;
    s.texKey = key;
    s.texScanned = false;
    s.texFilter[0] = '\0';
    s.texRequest = true;
}

// ---------------------------------------------------------------------------
// 詳細
// ---------------------------------------------------------------------------
void DrawDetailsPane(State& s, EditorContext& ctx, ImVec2 size)
{
    (void)ctx;
    const ng::Selection& sel = s.view.GetSelection();
    char right[48] = {};
    if (sel.nodes.size() == 1) std::snprintf(right, sizeof(right), "選択 1");
    else if (sel.nodes.size() > 1) std::snprintf(right, sizeof(right), "選択 %zu", sel.nodes.size());
    const float hh = PaneHeader("詳細", right);

    ImGui::BeginChild("##mgDetScroll", ImVec2(size.x, size.y - hh), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float pad = ui::Px(8.0f);
    ImGui::Dummy(ImVec2(0.0f, ui::Px(2.0f)));

    // ---- プレビュー領域（G2c: 実レンダラで描くライブプレビュー。GPU が使えない環境は従来の空き枠）----
    const bool previewOpen = ui::SectionHeader("プレビュー##mgsecPrev", ICON_T_MATERIAL, nullptr, ImGuiTreeNodeFlags_DefaultOpen);
    if (previewOpen && MaterialGraphPanel::GpuReady())
    {
        DrawPreviewPane(s, pad);
        ImGui::Dummy(ui::Px(0.0f, 6.0f));
        Anchor("area", "mg:preview");
    }
    else if (previewOpen)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x - pad * 2.0f;
        const float h = std::min(w * 0.72f, ui::Px(210.0f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 mn(p.x + pad, p.y + pad), mx(mn.x + w, mn.y + h);
        dl->AddRectFilled(mn, mx, Col(theme::Bg0), ui::Px(6.0f));
        // チェッカー（背景の目印）
        dl->PushClipRect(mn, mx, true);
        const float cell = ui::Px(14.0f);
        for (float y = mn.y, iy = 0; y < mx.y; y += cell, iy += 1.0f)
            for (float x = mn.x, ix = 0; x < mx.x; x += cell, ix += 1.0f)
                if ((static_cast<int>(ix) + static_cast<int>(iy)) % 2 == 0) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), IM_COL32(255, 255, 255, 6));
        dl->PopClipRect();
        dl->AddRect(mn, mx, Col(theme::Border), ui::Px(6.0f), 0, std::max(1.0f, ui::PxF(1.0f)));
        bool drew = false;
        if (g_previewDrawer)
        {
            dl->PushClipRect(mn, mx, true);
            drew = g_previewDrawer(mn.x, mn.y, mx.x, mx.y, s.ed);
            dl->PopClipRect();
        }
        if (!drew)
        {
            const float cr = std::min(w, h) * 0.30f;
            const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f - ui::Px(8.0f));
            dl->AddCircle(c, cr, ColA(theme::TextFaint, 0.5f), 48, std::max(1.0f, ui::PxF(1.5f)));
            dl->AddCircle(c, cr * 0.72f, ColA(theme::TextFaint, 0.22f), 40, std::max(1.0f, ui::PxF(1.0f)));
            const char* t1 = "プレビュー";
            const char* t2 = "球 / 平面 / 円柱 / 立方体（プレビュー担当が接続します）";
            const ImVec2 s1 = ImGui::CalcTextSize(t1), s2 = ImGui::CalcTextSize(t2);
            dl->AddText(ImVec2(c.x - s1.x * 0.5f, mx.y - ui::Px(40.0f)), Col(theme::TextDim), t1);
            dl->AddText(ImVec2(c.x - s2.x * 0.5f, mx.y - ui::Px(22.0f)), Col(theme::TextFaint), t2);
        }
        ImGui::Dummy(ImVec2(w + pad * 2.0f, h + pad * 2.0f));
        Anchor("area", "mg:preview");
    }

    // ---- 選択ノード / 出力ノードの接続状況 ----
    const std::vector<mg::MatGraphModel::PropInfo> noProps;
    if (sel.nodes.size() == 1)
    {
        const ng::NodeId id = *sel.nodes.begin();
        const ng::NodeData* n = s.ed.Model().FindNode(id);
        const mat::NodeDef* def = s.ed.Model().DefOf(id);
        if (n && n->desc)
        {
            const bool open = ui::SectionHeader("選択ノード##mgsecNode", ICON_T_SLIDERS, nullptr, ImGuiTreeNodeFlags_DefaultOpen);
            if (open)
            {
                ImGui::Indent(pad);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(ImVec2(p.x, p.y + ui::Px(2.0f)), ImVec2(p.x + ui::Px(3.0f), p.y + ui::Px(34.0f)), CategoryColor(n->desc->categorySlot), ui::Px(1.5f));
                ImGui::SetCursorScreenPos(ImVec2(p.x + ui::Px(10.0f), p.y));
                ImGui::BeginGroup();
                ui::PushBold();
                ImGui::TextUnformatted(n->desc->title.c_str());
                ui::PopBold();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::Text("%s  ·  %s", n->desc->category.c_str(), s.ed.Model().StrOf(id).c_str());
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                Anchor("header", "mg:sel:" + n->type);
                ImGui::Dummy(ui::Px(0.0f, 4.0f));
                if (!n->desc->description.empty())
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pad);
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                    ImGui::TextUnformatted(n->desc->description.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ui::Px(0.0f, 4.0f));
                }
                ImGui::Unindent(pad);

                if (def && !def->props.empty() && pg::Begin("##mgprops"))
                {
                    for (const mat::PropDecl& pd : def->props) DrawPropRow(s, id, *def, pd);
                    pg::End();
                }
                ImGui::Indent(pad);
                ImGui::Dummy(ui::Px(0.0f, 4.0f));
                // 操作ボタン
                std::string why;
                const bool canPromote = s.ed.CanPromote(id, &why);
                if (def && !def->isParameter)
                {
                    if (!canPromote) ImGui::BeginDisabled();
                    if (ImGui::Button("パラメータに昇格", ImVec2(0.0f, 0.0f))) s.pending.push_back("matgraph.promote");
                    if (!canPromote) ImGui::EndDisabled();
                    Anchor("button", "mg:btn:promoteSel");
                    if (!canPromote && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !why.empty()) ImGui::SetTooltip("%s", why.c_str());
                    ImGui::SameLine();
                }
                if (ImGui::Button("複製")) s.pending.push_back("edit.duplicate");
                Anchor("button", "mg:btn:dupSel");
                ImGui::SameLine();
                if (ImGui::Button("削除")) s.pending.push_back("edit.delete");
                Anchor("button", "mg:btn:delSel");
                ImGui::Dummy(ui::Px(0.0f, 4.0f));
                const bool pinRows = HasPinRows(*n);
                if (pinRows)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                    ImGui::TextUnformatted("入力の接続");
                    ImGui::PopStyleColor();
                }
                ImGui::Unindent(pad);
                if (pinRows) DrawPinTable(s, id, "##mgpins");
            }
        }
    }
    else if (sel.nodes.size() > 1)
    {
        if (ui::SectionHeader("選択ノード##mgsecNode", ICON_T_SLIDERS, nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(pad);
            ImGui::Text("%zu 個のノードを選択中", sel.nodes.size());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
            ImGui::TextUnformatted("Ctrl+C / Ctrl+D で複製、Del で削除、C でコメントで囲みます。");
            ImGui::PopStyleColor();
            ImGui::Dummy(ui::Px(0.0f, 2.0f));
            if (ImGui::Button("複製")) s.pending.push_back("edit.duplicate");
            ImGui::SameLine();
            if (ImGui::Button("コメントで囲む")) s.pending.push_back("graph.comment");
            ImGui::SameLine();
            if (ImGui::Button("削除")) s.pending.push_back("edit.delete");
            ImGui::Unindent(pad);
        }
    }
    else
    {
        const ng::NodeId out = s.ed.OutputNode();
        if (ui::SectionHeader("出力ノードの接続状況##mgsecOut", ICON_T_SPLINE, nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (out) DrawPinTable(s, out, "##mgoutpins");
            else
            {
                ImGui::Indent(pad);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad);
                ImGui::TextUnformatted("出力ノード（MaterialOutput）がありません。");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                ImGui::TextUnformatted("パレットの「出力」から追加してください。");
                ImGui::PopStyleColor();
                ImGui::Unindent(pad);
            }
        }
    }

    // ---- グラフ設定 ----
    if (ui::SectionHeader("グラフ設定##mgsecGraph", ICON_SETTINGS, nullptr, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##mggraph"))
        {
            mat::GraphSettings st = s.ed.Graph().Settings();
            std::string name = s.ed.Graph().Name();
            bool active = false;
            if (pg::InputTextStr("名前", name, &active, "グラフの名前（.dxmg の name）")) s.ed.Graph().SetName(name);
            int bm = IndexOf(kBlendModes, 4, st.blendMode);
            if (pg::Combo("ブレンド", &bm, kBlendModes, 4, "Opaque / Masked（抜き）/ Translucent / Additive"))
            {
                st.blendMode = kBlendModes[bm];
                s.ed.SetSettings(st);
            }
            int sm = IndexOf(kShadingModels, 2, st.shadingModel);
            if (pg::Combo("シェーディング", &sm, kShadingModels, 2, "DefaultLit / Unlit"))
            {
                st.shadingModel = kShadingModels[sm];
                s.ed.SetSettings(st);
            }
            bool two = st.twoSided;
            if (pg::Checkbox("両面", &two, "裏面も描く"))
            {
                st.twoSided = two;
                s.ed.SetSettings(st);
            }
            if (st.blendMode == "Masked")
            {
                float mc = st.maskClip;
                bool act = false;
                if (pg::SliderFloat("マスクしきい値", &mc, 0.0f, 1.0f, "%.2f", &act, "OpacityMask がこの値未満のピクセルは描かない"))
                {
                    if (!s.settingsLive) { s.settingsOld = st; s.settingsLive = true; }
                    st.maskClip = mc;
                    s.ed.SetSettingsLive(st);
                }
            }
            pg::End();
        }
    }

    // ---- パラメータ一覧 ----
    {
        const std::vector<mat::ParamDecl> params = s.ed.Parameters();
        char label[64];
        std::snprintf(label, sizeof(label), "パラメータ (%zu)##mgsecParams", params.size());
        if (ui::SectionHeader(label, ICON_T_SLIDERS, nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (params.empty())
            {
                ImGui::Indent(pad);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                ImGui::TextUnformatted("パラメータはありません（ScalarParameter / VectorParameter / TextureParameter を置くと並びます）");
                ImGui::PopStyleColor();
                ImGui::Unindent(pad);
            }
            else
            {
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ui::Px(6.0f, 3.0f));
                if (ImGui::BeginTable("##mgparams", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                    ImGui::TableSetupColumn("型", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                    ImGui::TableSetupColumn("既定値", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                    ImGui::TableSetupColumn("グループ", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < params.size(); ++i)
                    {
                        const mat::ParamDecl& p = params[i];
                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableSetColumnIndex(0);
                        bool rowHov = false;
                        if (ClickableRow(("mg:param:" + p.name).c_str(), &rowHov)) s.view.FocusNode(s.ed.Model().IntOf(p.nodeId), true);
                        ImGui::PushStyleColor(ImGuiCol_Text, p.reachable ? theme::Text : theme::TextFaint);
                        ImGui::TextUnformatted(p.name.c_str());
                        ImGui::PopStyleColor();
                        if (!p.reachable && rowHov) ImGui::SetTooltip("出力に繋がっていません（スロットを持ちません）");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
                        ImGui::TextUnformatted(p.type == mat::ValueType::Tex2D ? "Texture" : p.type == mat::ValueType::F1 ? "Scalar" : "Vector");
                        ImGui::PopStyleColor();
                        ImGui::TableSetColumnIndex(2);
                        if (p.type == mat::ValueType::F4 && p.defaultValue.is_array() && p.defaultValue.size() >= 3)
                        {
                            const float c[4] = {p.defaultValue[0].get<float>(), p.defaultValue[1].get<float>(), p.defaultValue[2].get<float>(), 1.0f};
                            const float sz = ImGui::GetTextLineHeight();
                            const ImVec2 cp = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(cp, ImVec2(cp.x + sz * 1.6f, cp.y + sz), ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 1.0f)), 3.0f);
                            ImGui::GetWindowDrawList()->AddRect(cp, ImVec2(cp.x + sz * 1.6f, cp.y + sz), Col(theme::InputBorder), 3.0f);
                            ImGui::Dummy(ImVec2(sz * 1.6f, sz));
                        }
                        else if (p.type == mat::ValueType::Tex2D)
                        {
                            const std::string path = p.defaultValue.is_string() ? p.defaultValue.get<std::string>() : std::string();
                            ImGui::PushStyleColor(ImGuiCol_Text, path.empty() ? theme::TextFaint : theme::TextMid);
                            ImGui::TextUnformatted(path.empty() ? "(なし)" : PathLeaf(path).c_str());
                            ImGui::PopStyleColor();
                        }
                        else if (p.defaultValue.is_number())
                        {
                            ui::PushMono();
                            ImGui::Text("%.3g", p.defaultValue.get<double>());
                            ui::PopMono();
                        }
                        ImGui::TableSetColumnIndex(3);
                        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
                        ImGui::TextUnformatted(p.group.empty() ? "-" : p.group.c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Px(12.0f)));
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// モーダル: Custom の HLSL・テクスチャ選択
// ---------------------------------------------------------------------------
void DrawEditorDialogs(State& s)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
    ui::PushMenuStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(8.0f, 8.0f));

    // ---- Custom ----
    if (s.customRequest) { ImGui::OpenPopup("HLSL 式の編集###mgCustom"); s.customRequest = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(680.0f, 500.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("HLSL 式の編集###mgCustom", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted("入力は In0〜In7（接続したものだけが引数になります）。本文に return があれば関数本体、無ければ式として扱います。"
                               "テクスチャは Texture2D として渡されます。");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        const float footer = ui::Px(84.0f);
        ui::PushMono();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##mgcode", s.codeBuf.data(), s.codeBuf.size(), ImVec2(-FLT_MIN, -footer), ImGuiInputTextFlags_AllowTabInput);
        Anchor("input", "mg:input:customCode");
        ui::PopMono();
        static const char* const kTypes[] = {"float", "float2", "float3", "float4"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("出力型");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Px(120.0f));
        ui::Combo("##mgouttype", &s.customOutType, kTypes, 4);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        int lines = 1;
        for (const char* c = s.codeBuf.data(); *c; ++c) if (*c == '\n') ++lines;
        ImGui::Text("   %d 行", lines);
        ImGui::PopStyleColor();
        if (ui::PrimaryButton("適用", ui::Px(120.0f, 0.0f)))
        {
            s.ed.Doc().History().BeginGroup("HLSL 式を変更");
            s.ed.SetNodeProp(s.customNode, "code", std::string(s.codeBuf.data()));
            s.ed.SetNodeProp(s.customNode, "outputType", kTypes[std::min(std::max(s.customOutType, 0), 3)]);
            s.ed.Doc().History().EndGroup();
            ImGui::CloseCurrentPopup();
        }
        Anchor("button", "mg:btn:customApply");
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- テクスチャ選択 ----
    if (s.texRequest) { ImGui::OpenPopup("テクスチャを選択###mgTex"); s.texRequest = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(560.0f, 480.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("テクスチャを選択###mgTex", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        if (!s.texScanned)
        {
            s.texList.clear();
            std::error_code ec;
            const fs::path root(AssetsRoot());
            if (fs::exists(root, ec))
            {
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                for (; !ec && it != end; it.increment(ec))
                {
                    std::error_code fec;
                    if (!it->is_regular_file(fec) || fec) continue;
                    if (AssetBrowserPanel::ClassifyExtension(Lower(it->path().extension().string())) != AssetBrowserPanel::AssetType::Texture) continue;
                    const fs::path rel = fs::relative(it->path(), root, fec);
                    if (!fec) s.texList.push_back(rel.generic_string());
                }
            }
            std::sort(s.texList.begin(), s.texList.end());
            s.texScanned = true;
        }
        const mat::Json curJ = s.ed.Model().GetNodeProp(s.texNode, s.texKey);
        const std::string cur = curJ.is_string() ? curJ.get<std::string>() : std::string();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ui::SearchField("##mgtexfilter", s.texFilter, sizeof(s.texFilter), "テクスチャを絞り込み…");
        ImGui::BeginChild("##mgtexlist", ImVec2(0.0f, -ui::Px(48.0f)), ImGuiChildFlags_Borders);
        const std::string f = Lower(s.texFilter);
        if (ImGui::Selectable("(なし)", cur.empty()))
        {
            s.ed.SetNodeProp(s.texNode, s.texKey, std::string());
            ImGui::CloseCurrentPopup();
        }
        int shown = 0;
        for (const std::string& rel : s.texList)
        {
            if (!f.empty() && Lower(rel).find(f) == std::string::npos) continue;
            ++shown;
            if (ImGui::Selectable(rel.c_str(), rel == cur))
            {
                s.ed.SetNodeProp(s.texNode, s.texKey, rel);
                ImGui::CloseCurrentPopup();
            }
            Anchor("row", "mg:tex:" + rel);
        }
        if (shown == 0 && !s.texList.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
            ImGui::TextUnformatted("一致するテクスチャがありません");
            ImGui::PopStyleColor();
        }
        if (s.texList.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
            ImGui::TextUnformatted("assets にテクスチャがありません（パスは手入力もできます）");
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        if (ImGui::Button("閉じる", ui::Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ui::PopMenuStyle();
}

} // namespace dx12e::mgpanel
