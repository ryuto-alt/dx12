#pragma once

// ===== Unreal "Details" パネル風 2カラムプロパティグリッド =====
// 左=ラベル / 右=値。列幅は境界ドラッグで調整可、行はゼブラ、Vec3 は XYZ 色分け。
// 使い方:
//   if (pg::Begin("Transform")) {
//       changed |= pg::Float3("Position", &t.position.x, 0.1f, 0, 0, "%.3f", &active);
//       pg::End();
//   }
// 定型に無い行は pg::Label("ラベル") の後に好きなウィジェットを描く（右列・全幅設定済み）。
// ラベルの tip 引数はホバーで説明ツールチップ（従来の SameLine "(?)" の置き換え）。

#include "editor/EditorTheme.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <cfloat>
#include <cstdarg>
#include <algorithm>

namespace dx12e::pg
{

namespace detail
{
inline void Track(bool* active)
{
    if (active) *active |= ImGui::IsItemActive();
}
} // namespace detail

// テーブル開始。false ならクリップ等で非表示（End 不要、内部で後始末済み）。
inline bool Begin(const char* id)
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,    theme::Hex(0xffffff, 0.02f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, theme::Hex(0xffffff, 0.00f));
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_BordersInnerV
                                | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 2, flags))
    {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

inline void End()
{
    ImGui::EndTable();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ラベル行を開始し、値セルへ移動（次のウィジェットは全幅）。
inline void Label(const char* label, const char* tip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMid);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (tip)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        ImGui::TextUnformatted("?");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// グループ見出し（行全体を塗る）。SeparatorText の置き換え。
inline void Group(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(theme::GroupBg));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

// 読み取り専用の情報行
inline void Text(const char* label, const char* fmt, ...)
{
    Label(label);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

inline bool Float(const char* label, float* v, float speed = 0.01f,
                  float mn = 0.0f, float mx = 0.0f, const char* fmt = "%.3f",
                  bool* active = nullptr, const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::DragFloat("##v", v, speed, mn, mx, fmt);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

inline bool SliderFloat(const char* label, float* v, float mn, float mx,
                        const char* fmt = "%.3f", bool* active = nullptr,
                        const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##v", v, mn, mx, fmt);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

inline bool Int(const char* label, int* v, float speed = 1.0f,
                int mn = 0, int mx = 0, bool* active = nullptr,
                const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::DragInt("##v", v, speed, mn, mx);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

inline bool SliderInt(const char* label, int* v, int mn, int mx,
                      bool* active = nullptr, const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::SliderInt("##v", v, mn, mx);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

inline bool Checkbox(const char* label, bool* v, const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::Checkbox("##v", v);
    ImGui::PopID();
    return ch;
}

inline bool Combo(const char* label, int* idx, const char* const items[], int count,
                  const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::Combo("##v", idx, items, count);
    ImGui::PopID();
    return ch;
}

inline bool Color3(const char* label, float* col, ImGuiColorEditFlags flags = 0)
{
    Label(label);
    ImGui::PushID(label);
    bool ch = ImGui::ColorEdit3("##v", col, flags);
    ImGui::PopID();
    return ch;
}

inline bool Color4(const char* label, float* col, ImGuiColorEditFlags flags = 0)
{
    Label(label);
    ImGui::PushID(label);
    bool ch = ImGui::ColorEdit4("##v", col, flags);
    ImGui::PopID();
    return ch;
}

inline bool InputText(const char* label, char* buf, size_t size,
                      ImGuiInputTextFlags flags = 0, bool* active = nullptr,
                      const char* tip = nullptr)
{
    Label(label, tip);
    ImGui::PushID(label);
    bool ch = ImGui::InputText("##v", buf, size, flags);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

// N 軸ドラッグ（左端に軸カラーバー: X=赤 Y=緑 Z=青 W=灰）
inline bool FloatN(const char* label, float* v, int n, float speed,
                   float mn, float mx, const char* fmt, bool* active,
                   const char* tip = nullptr)
{
    static const ImU32 axisCol[4] = {
        IM_COL32(226,  84,  84, 255),   // X
        IM_COL32(126, 204,  88, 255),   // Y
        IM_COL32( 84, 132, 240, 255),   // Z
        IM_COL32(160, 162, 170, 255),   // W
    };
    Label(label, tip);
    ImGui::PushID(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = (ImGui::GetContentRegionAvail().x
                     - st.ItemInnerSpacing.x * static_cast<float>(n - 1))
                    / static_cast<float>(n);
    bool ch = false;
    for (int i = 0; i < n && i < 4; ++i)
    {
        if (i) ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(std::max(w, 32.0f));
        ch |= ImGui::DragFloat("##v", v + i, speed, mn, mx, fmt);
        detail::Track(active);
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            rmin, ImVec2(rmin.x + 3.0f, rmax.y), axisCol[i],
            st.FrameRounding, ImDrawFlags_RoundCornersLeft);
        ImGui::PopID();
    }
    ImGui::PopID();
    return ch;
}

inline bool Float2(const char* label, float* v, float speed = 0.01f,
                   float mn = 0.0f, float mx = 0.0f, const char* fmt = "%.3f",
                   bool* active = nullptr, const char* tip = nullptr)
{
    return FloatN(label, v, 2, speed, mn, mx, fmt, active, tip);
}

inline bool Float3(const char* label, float* v, float speed = 0.01f,
                   float mn = 0.0f, float mx = 0.0f, const char* fmt = "%.3f",
                   bool* active = nullptr, const char* tip = nullptr)
{
    return FloatN(label, v, 3, speed, mn, mx, fmt, active, tip);
}

} // namespace dx12e::pg
