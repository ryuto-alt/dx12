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
//
// ── フェーズ 1b の拡張（既存の呼び出しは無変更で同じ見た目・同じ挙動）──
//   ・行の右クリック: 既定値へリセット / 値をコピー / 値を貼り付け（Float3 などは軸ごとのリセットも）。
//     数値 / 真偽 / 選択肢の行が対象（文字列行は対象外）。コピー先は型タグ付きテキスト（"dx12v:f:1,2,3"）。
//   ・変更あり印（●）: 既定値と違う行のラベルの左に薄いアクセントの点。
//   ・Mixed 表示: 複数選択で値がバラバラの行は「—」。編集した瞬間に全員へ書かれて揃う。
//   ・プロパティ検索: pg::SetFilter("pos") の間、ラベルが一致しない行と、一致行の無い Group を出さない。
//   これらは「コンポーネント文脈」（pg::SetComponent）があるときだけ働く。文脈は
//     ・base / size = 描いている（プライマリの）コンポーネント実体
//     ・def          = 既定値の実体（T{}。null なら印もリセットも出さない）
//     ・others       = 同じ選択の他のエンティティの同じコンポーネント実体（Mixed 判定用）
//   行のウィジェットへ渡した値ポインタが base の中にあれば、そのオフセットで def / others とバイト比較する
//   （フィールドごとの追加コードは要らない）。文脈が無い行は従来どおり。

#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"     // 見た目部品（スライダー/チェック/コンボ等の自前描画）
#include "editor/InspectorLogic.h"   // 行の右クリック / Mixed / 検索の純ロジック（フェーズ 1b）
#include "gui/VirtualInputImGui.h"   // find 用アンカー登録（仮想入力モード ON の間だけ働く）

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstring>
#include <algorithm>

namespace dx12e::pg
{

namespace detail
{
inline void Track(bool* active)
{
    if (active) *active |= ImGui::IsItemActive();
}

// 値が「バラバラ」の行に出す文字（em ダッシュ）。DragFloat / DragInt / スライダーの書式にそのまま渡す。
inline constexpr const char* kMixedText = "\xE2\x80\x94";

// ---- 共有状態（inline 関数の static = プログラム全体で 1 個）----
struct State
{
    // プロパティ検索
    std::string filter;            // 空 = 検索なし
    bool        bypass = false;    // true = コンポーネント名が一致した＝全行を出す
    std::string pendingGroup;      // 検索中は Group を遅延させ、その下に出す行が出た時に初めて描く
    bool        hasPendingGroup = false;
    int         rowsShown   = 0;   // ResetRowStats 以降に出した行数（ヘッダの「一致なし」判定用）
    int         tablesBegun = 0;   // 同じく Begin した表の数

    // コンポーネント文脈（SetComponent）
    const unsigned char* base = nullptr;
    size_t               size = 0;
    const unsigned char* def  = nullptr;
    std::vector<const unsigned char*> others;
    bool                 mixedOk = false;

    // 直近のラベル位置（変更あり印・右クリック範囲に使う）
    ImVec2 labelMin{0, 0}, labelMax{0, 0};

    // 値コピーの内部バッファ（仮想入力中 / UI 自動テスト中は OS のクリップボードへ書かない。テストもこれを見る）
    std::string clip;
    bool        internalOnly = false;
};
inline State& S()
{
    static State s;
    return s;
}

inline void DrawGroupRow(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(theme::GroupBg));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

inline void FlushGroup()
{
    State& s = S();
    if (!s.hasPendingGroup) return;
    s.hasPendingGroup = false;
    DrawGroupRow(s.pendingGroup.c_str());
}

// ラベルがフィルタに合うか（検索なし / バイパス中は常に true）。
inline bool Passes(const char* label)
{
    const State& s = S();
    if (s.filter.empty() || s.bypass) return true;
    return insp::PropertyMatches(s.filter, label);
}

// ---- 行の判定（値ポインタがコンポーネント文脈の中にあるか / 既定値と違うか / バラバラか）----
struct Probe
{
    bool inCtx    = false;
    bool modified = false;
    bool mixed    = false;
    long off      = -1;
};
inline Probe ProbeRange(const void* p, size_t n)
{
    Probe r;
    const State& s = S();
    const long off = insp::OffsetIn(s.base, s.size, p, n);
    if (off < 0) return r;
    r.inCtx = true;
    r.off = off;
    const auto* cur = static_cast<const unsigned char*>(p);
    r.modified = insp::BytesDiffer(cur, s.def ? s.def + off : nullptr, n);
    if (s.mixedOk)
        for (const unsigned char* o : s.others)
            if (o && std::memcmp(cur, o + off, n) != 0) { r.mixed = true; break; }
    return r;
}

enum class Kind { Float, Int, Bool };
struct RowSpec
{
    Kind  kind = Kind::Float;
    void* p    = nullptr;   // 値の先頭
    int   n    = 1;         // 要素数（Float3 = 3）
    float mn = 0.0f, mx = 0.0f;   // mx > mn なら貼り付けをクランプ
    const char* const* axisNames = nullptr;   // 軸ごとのリセット項目名（Float2〜4 のみ）
};

inline size_t ElemSize(Kind k) { return k == Kind::Bool ? sizeof(bool) : sizeof(float); }

inline void CopyText(const std::string& text)
{
    State& s = S();
    s.clip = text;
    if (!vinput::Enabled() && !s.internalOnly) ImGui::SetClipboardText(text.c_str());
}
inline std::string PasteText()
{
    const State& s = S();
    if (vinput::Enabled() || s.internalOnly) return s.clip;
    const char* c = ImGui::GetClipboardText();
    std::string t = c ? c : "";
    double tmp[4];
    if (insp::DecodeNumbers(t, 1, tmp) || insp::DecodeNumbers(t, 2, tmp) || insp::DecodeNumbers(t, 3, tmp) || insp::DecodeNumbers(t, 4, tmp))
        return t;
    return s.clip;   // OS のクリップボードに型タグ付きの値が無ければ内部バッファ
}

inline std::string EncodeRow(const RowSpec& r)
{
    switch (r.kind)
    {
    case Kind::Float: return insp::EncodeFloats(static_cast<const float*>(r.p), r.n);
    case Kind::Int:   return insp::EncodeInt(*static_cast<const int*>(r.p));
    case Kind::Bool:  return insp::EncodeBool(*static_cast<const bool*>(r.p));
    }
    return {};
}

inline bool ApplyDecoded(const RowSpec& r, const double* d)
{
    bool ch = false;
    for (int i = 0; i < r.n; ++i)
    {
        if (r.kind == Kind::Float)
        {
            float v = static_cast<float>(d[i]);
            if (r.mx > r.mn) v = (std::min)((std::max)(v, r.mn), r.mx);
            float& dst = static_cast<float*>(r.p)[i];
            if (dst != v) { dst = v; ch = true; }
        }
        else if (r.kind == Kind::Int)
        {
            int v = static_cast<int>(d[i] < 0 ? d[i] - 0.5 : d[i] + 0.5);
            if (r.mx > r.mn) v = (std::min)((std::max)(v, static_cast<int>(r.mn)), static_cast<int>(r.mx));
            int& dst = *static_cast<int*>(r.p);
            if (dst != v) { dst = v; ch = true; }
        }
        else
        {
            bool v = d[i] != 0.0;
            bool& dst = *static_cast<bool*>(r.p);
            if (dst != v) { dst = v; ch = true; }
        }
    }
    return ch;
}

// 行の後始末。変更あり印の描画と、右クリックメニュー。値を書き換えたら true（呼び出し側の changed へ OR する）。
// ★必ず PushID(label) のスコープ内 & 値ウィジェットの直後に呼ぶ（IsItemHovered ではなく行の矩形で判定する）。
inline bool RowFooter(const RowSpec& r, const Probe& whole)
{
    State& s = S();
    bool changed = false;
    const float rowTop    = s.labelMin.y - theme::Px(2.0f);
    const float rowBottom = (std::max)(s.labelMax.y, ImGui::GetItemRectMax().y) + theme::Px(2.0f);

    // 変更あり印（ラベルの左の余白。ラベルの高さの中央）
    if (whole.inCtx && whole.modified)
    {
        const float cy = std::floor((s.labelMin.y + s.labelMax.y) * 0.5f) + 0.5f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(s.labelMin.x - theme::Px(3.5f), cy), theme::Px(2.2f),
            ImGui::GetColorU32(theme::WithAlpha(theme::AccentHover, 0.85f)));
    }

    // 右クリック: ラベル〜値欄の行全体
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 rmin(wp.x, rowTop), rmax(wp.x + ImGui::GetWindowWidth(), rowBottom);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rmin, rmax) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##pgctx");

    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##pgctx"))
    {
        const bool canReset = whole.inCtx && s.def != nullptr;
        if (ImGui::MenuItem("既定値へリセット", nullptr, false, canReset && whole.modified))
        {
            std::memcpy(r.p, s.def + whole.off, static_cast<size_t>(r.n) * ElemSize(r.kind));
            changed = true;
        }
        if (canReset && r.n > 1 && r.kind == Kind::Float && r.axisNames)
        {
            for (int i = 0; i < r.n; ++i)
            {
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), "%s を既定値へ", r.axisNames[i]);
                const long off = whole.off + static_cast<long>(i * sizeof(float));
                const bool differs = std::memcmp(static_cast<float*>(r.p) + i, s.def + off, sizeof(float)) != 0;
                if (ImGui::MenuItem(lbl, nullptr, false, differs))
                {
                    std::memcpy(static_cast<float*>(r.p) + i, s.def + off, sizeof(float));
                    changed = true;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("値をコピー"))
            CopyText(EncodeRow(r));
        double nums[4] = {};
        const std::string clipText = PasteText();
        const bool canPaste = insp::DecodeNumbers(clipText, r.n, nums);
        if (ImGui::MenuItem("値を貼り付け", nullptr, false, canPaste))
            changed |= ApplyDecoded(r, nums);
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();
    return changed;
}

// 型付きの行の入口。検索で除外された行は false（何も描かない）。
inline void LabelCore(const char* label, const char* tip, bool counted = true);
inline bool BeginRow(const char* label, const char* tip)
{
    if (!Passes(label)) return false;
    LabelCore(label, tip);
    return true;
}
} // namespace detail

// ---- 検索 ----
// text が空なら検索なし。SetFilter は毎フレーム（コンポーネントを描く前に）呼んでよい。
inline void SetFilter(const char* text)
{
    detail::S().filter = text ? text : "";
    detail::S().bypass = false;
}
inline bool FilterActive() { return !detail::S().filter.empty(); }
// コンポーネント名などが既に一致している間は true にして、配下の全行を出す（SetFilter の後・Begin の前）。
inline void SetFilterBypass(bool on) { detail::S().bypass = on; }
inline void ResetRowStats() { detail::S().rowsShown = 0; detail::S().tablesBegun = 0; }
inline int  RowsShown()   { return detail::S().rowsShown; }
inline int  TablesBegun() { return detail::S().tablesBegun; }

// ---- コンポーネント文脈 ----
// base = 描くコンポーネント実体（reg.get<T>(primary) の参照先）、def = 既定値の実体（nullptr 可）、
// others = 同じ選択の他のエンティティの同じ型の実体。mixedOk = 値の違いを「—」で出してよいか
// （編集が全員へ伝わる型 = trivially copyable のときだけ true にすること）。
inline void SetComponent(const void* base, size_t size, const void* def,
                         const std::vector<const void*>& others, bool mixedOk)
{
    detail::State& s = detail::S();
    s.base = static_cast<const unsigned char*>(base);
    s.size = size;
    s.def  = static_cast<const unsigned char*>(def);
    s.others.clear();
    for (const void* o : others) s.others.push_back(static_cast<const unsigned char*>(o));
    s.mixedOk = mixedOk && !s.others.empty();
}
inline void ClearComponent()
{
    detail::State& s = detail::S();
    s.base = nullptr; s.size = 0; s.def = nullptr; s.others.clear(); s.mixedOk = false;
}
// 内部クリップボード（UI 自動テストが値のコピー / 貼り付けを確かめる）
inline const std::string& InternalClipboard() { return detail::S().clip; }
inline void SetInternalClipboard(const std::string& t) { detail::S().clip = t; }
// true の間は OS のクリップボードを触らず内部バッファだけを使う（UI 自動テスト / 仮想入力。呼び出し側が毎フレーム設定してよい）
inline void SetInternalClipboardOnly(bool on) { detail::S().internalOnly = on; }

// テーブル開始。false ならクリップ等で非表示（End 不要、内部で後始末済み）。
inline bool Begin(const char* id)
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, theme::Px(6.0f, 3.0f));
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
    detail::S().hasPendingGroup = false;
    ++detail::S().tablesBegun;
    return true;
}

inline void End()
{
    detail::S().hasPendingGroup = false;   // 一致する行が無かった Group は出さない
    ImGui::EndTable();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

namespace detail
{
inline void LabelCore(const char* label, const char* tip, bool counted)
{
    State& s = S();
    FlushGroup();
    if (counted) ++s.rowsShown;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    // 検索中は一致した行のラベルをアクセントに（バイパス中は全行が一致なので色を変えない）
    const bool hl = !s.filter.empty() && !s.bypass;
    ImGui::PushStyleColor(ImGuiCol_Text, hl ? theme::AccentHover : theme::TextMid);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const ImVec2 anchorLabelMin = ImGui::GetItemRectMin();   // dx12_imgui_find 用（ラベル文字の矩形）
    const ImVec2 anchorLabelMax = ImGui::GetItemRectMax();
    s.labelMin = anchorLabelMin;
    s.labelMax = anchorLabelMax;
    if (tip)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0.0f, theme::Px(4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
        ImGui::TextUnformatted(ICON_HELP);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
    }
    ImGui::TableSetColumnIndex(1);
    if (vinput::Enabled())
    {
        // 値欄の矩形（次に描くウィジェットの位置と幅）。クリック対象はこちら。
        const ImVec2 vmin = ImGui::GetCursorScreenPos();
        const ImVec2 vmax(vmin.x + ImGui::GetContentRegionAvail().x, vmin.y + ImGui::GetFrameHeight());
        vinput_gui::AnchorProperty(label, anchorLabelMin, anchorLabelMax, vmin, vmax);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
}
} // namespace detail

// ラベル行を開始し、値セルへ移動（次のウィジェットは全幅）。
// ★raw の Label は検索で隠さない（後続の生ウィジェットを描く場所が要るため）。ただし検索に一致しない行は
//   「表示した行数」に数えない（一致する行が 1 つも無い見出しを隠せるように）。
inline void Label(const char* label, const char* tip = nullptr)
{
    detail::LabelCore(label, tip, detail::Passes(label));
}

// グループ見出し（行全体を塗る）。SeparatorText の置き換え。
// 検索中は、その下に出す行が 1 つでもあった時だけ描く（一致する行が無い Group は出さない）。
inline void Group(const char* label)
{
    detail::State& s = detail::S();
    if (!s.filter.empty())
    {
        s.pendingGroup = label;
        s.hasPendingGroup = true;
        return;
    }
    detail::DrawGroupRow(label);
}

// 読み取り専用の情報行
inline void Text(const char* label, const char* fmt, ...)
{
    if (!detail::BeginRow(label, nullptr)) return;
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

inline bool Float(const char* label, float* v, float speed = 0.01f,
                  float mn = 0.0f, float mx = 0.0f, const char* fmt = "%.3f",
                  bool* active = nullptr, const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(v, sizeof(float));
    bool ch = ui::DragFloat("##v", v, speed, mn, mx, pr.mixed ? detail::kMixedText : fmt);
    detail::Track(active);
    ch |= detail::RowFooter({detail::Kind::Float, v, 1, mn, mx, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool SliderFloat(const char* label, float* v, float mn, float mx,
                        const char* fmt = "%.3f", bool* active = nullptr,
                        const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(v, sizeof(float));
    bool ch = ui::SliderFloat("##v", v, mn, mx, pr.mixed ? detail::kMixedText : fmt);
    detail::Track(active);
    ch |= detail::RowFooter({detail::Kind::Float, v, 1, mn, mx, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool Int(const char* label, int* v, float speed = 1.0f,
                int mn = 0, int mx = 0, bool* active = nullptr,
                const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(v, sizeof(int));
    bool ch = pr.mixed ? ui::DragInt("##v", v, speed, mn, mx, detail::kMixedText)
                       : ui::DragInt("##v", v, speed, mn, mx);
    detail::Track(active);
    ch |= detail::RowFooter({detail::Kind::Int, v, 1, static_cast<float>(mn), static_cast<float>(mx), nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool SliderInt(const char* label, int* v, int mn, int mx,
                      bool* active = nullptr, const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(v, sizeof(int));
    bool ch = pr.mixed ? ui::SliderInt("##v", v, mn, mx, detail::kMixedText)
                       : ui::SliderInt("##v", v, mn, mx);
    detail::Track(active);
    ch |= detail::RowFooter({detail::Kind::Int, v, 1, static_cast<float>(mn), static_cast<float>(mx), nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool Checkbox(const char* label, bool* v, const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(v, sizeof(bool));
    bool ch = ui::Checkbox("##v", v);
    if (pr.mixed && !ch)
    {
        // 値がバラバラ: 箱の中に横棒（「—」相当）を重ねる
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float box = theme::Px(theme::size::kCheckBox);
        const float top = mn.y + (ImGui::GetFrameHeight() - box) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(mn.x, top), ImVec2(mn.x + box, top + box), ImGui::GetColorU32(theme::InputBg), theme::Px(2.0f));
        dl->AddRect(ImVec2(mn.x, top), ImVec2(mn.x + box, top + box), ImGui::GetColorU32(theme::InputBorderHover), theme::Px(2.0f));
        dl->AddRectFilled(ImVec2(mn.x + box * 0.25f, top + box * 0.5f - theme::Px(1.0f)),
                          ImVec2(mn.x + box * 0.75f, top + box * 0.5f + theme::Px(1.0f)),
                          ImGui::GetColorU32(theme::TextMid));
    }
    ch |= detail::RowFooter({detail::Kind::Bool, v, 1, 0.0f, 0.0f, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool Combo(const char* label, int* idx, const char* const items[], int count,
                  const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(idx, sizeof(int));
    bool ch = false;
    if (pr.mixed)
    {
        // 値がバラバラ: プレビューを「—」にして、選んだ項目で全員を揃える
        if (ui::BeginCombo("##v", detail::kMixedText))
        {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], false)) { *idx = i; ch = true; }
            ImGui::EndCombo();
        }
    }
    else
    {
        ch = ui::Combo("##v", idx, items, count);
    }
    ch |= detail::RowFooter({detail::Kind::Int, idx, 1, 0.0f, 0.0f, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool Color3(const char* label, float* col, ImGuiColorEditFlags flags = 0)
{
    if (!detail::BeginRow(label, nullptr)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(col, sizeof(float) * 3);
    bool ch = ui::ColorEdit3("##v", col, flags);
    ch |= detail::RowFooter({detail::Kind::Float, col, 3, 0.0f, 0.0f, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool Color4(const char* label, float* col, ImGuiColorEditFlags flags = 0)
{
    if (!detail::BeginRow(label, nullptr)) return false;
    ImGui::PushID(label);
    const detail::Probe pr = detail::ProbeRange(col, sizeof(float) * 4);
    bool ch = ui::ColorEdit4("##v", col, flags);
    ch |= detail::RowFooter({detail::Kind::Float, col, 4, 0.0f, 0.0f, nullptr}, pr);
    ImGui::PopID();
    return ch;
}

inline bool InputText(const char* label, char* buf, size_t size,
                      ImGuiInputTextFlags flags = 0, bool* active = nullptr,
                      const char* tip = nullptr)
{
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    bool ch = ui::InputText("##v", buf, size, flags);
    detail::Track(active);
    ImGui::PopID();
    return ch;
}

// std::string 版。呼び出し側で毎回 char バッファへ copy する定型を畳んだもの。
// 260 文字を超えるパスは扱わない（Windows の MAX_PATH 相当で十分）。
inline bool InputTextStr(const char* label, std::string& v, bool* active = nullptr,
                         const char* tip = nullptr)
{
    char buf[260] = {};
    const size_t n = v.copy(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    if (!InputText(label, buf, sizeof(buf), 0, active, tip)) return false;
    v = buf;
    return true;
}

// N 軸ドラッグ（左端に軸カラーバー: X=赤 Y=緑 Z=青 W=灰）
inline bool FloatN(const char* label, float* v, int n, float speed,
                   float mn, float mx, const char* fmt, bool* active,
                   const char* tip = nullptr)
{
    // XYZ の色帯。C（インク・アンド・シグナル）だけは彩度を落とす（色は状態のためにとっておく）。
    static const ImU32 axisColDefault[4] = {
        IM_COL32(226,  84,  84, 255),   // X
        IM_COL32(126, 204,  88, 255),   // Y
        IM_COL32( 84, 132, 240, 255),   // Z
        IM_COL32(160, 162, 170, 255),   // W
    };
    static const ImU32 axisColMuted[4] = {
        IM_COL32(184, 118, 114, 255),
        IM_COL32(148, 176, 128, 255),
        IM_COL32(124, 148, 194, 255),
        IM_COL32(150, 150, 150, 255),
    };
    static const char* const axisNames[4] = {"X", "Y", "Z", "W"};
    const ImU32* axisCol = (theme::CurrentVariant() == theme::Variant::C) ? axisColMuted : axisColDefault;
    if (!detail::BeginRow(label, tip)) return false;
    ImGui::PushID(label);
    const detail::Probe whole = detail::ProbeRange(v, sizeof(float) * static_cast<size_t>((std::min)(n, 4)));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = (ImGui::GetContentRegionAvail().x
                     - st.ItemInnerSpacing.x * static_cast<float>(n - 1))
                    / static_cast<float>(n);
    bool ch = false;
    for (int i = 0; i < n && i < 4; ++i)
    {
        if (i) ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(std::max(w, theme::Px(32.0f)));
        const detail::Probe axis = detail::ProbeRange(v + i, sizeof(float));
        ch |= ui::DragFloat("##v", v + i, speed, mn, mx, axis.mixed ? detail::kMixedText : fmt);
        detail::Track(active);
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        // 細い色帯（枠の内側 2px）。X/Y/Z の見分けだけを担い、入力欄の面を塗らない。
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(rmin.x + theme::Px(1.0f), rmin.y + theme::Px(1.0f)), ImVec2(rmin.x + theme::Px(3.0f), rmax.y - theme::Px(1.0f)), axisCol[i],
            st.FrameRounding, ImDrawFlags_RoundCornersLeft);
        ImGui::PopID();
    }
    ch |= detail::RowFooter({detail::Kind::Float, v, (std::min)(n, 4), mn, mx, axisNames}, whole);
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

inline bool Float4(const char* label, float* v, float speed = 0.01f,
                   float mn = 0.0f, float mx = 0.0f, const char* fmt = "%.3f",
                   bool* active = nullptr, const char* tip = nullptr)
{
    return FloatN(label, v, 4, speed, mn, mx, fmt, active, tip);
}

} // namespace dx12e::pg
