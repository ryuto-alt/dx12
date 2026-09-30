#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/EntityGlyph.h"
#include "editor/UndoSystem.h"
#include "editor/EditorCommands.h"   // 作成メニュー（エンティティ追加と共用）/ コピー・貼り付け
#include "editor/TransformMath.h"    // ワールド位置を保つ親替え
#include "editor/HierarchyActions.h" // 目 / 鍵 / フォルダ / 並べ替え（Undo つき）
#include "editor/EditorPrefs.h"      // 型チップの選択を永続化（hier.typeFilter）
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー
#include "ecs/Components.h"
#include "ecs/EditorFlags.h"
#include "scene/Scene.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // InnerRect（行の面を窓の左端〜右端いっぱいに塗る）
#pragma warning(pop)

#include <algorithm>
#include <filesystem>
#include <functional>
#include <vector>
#include <cstdio>
#include <cctype>
#include <Windows.h>

namespace dx12e
{

namespace {

// リネームを 1 コマンドとして積む。
// ★NameTag を書き換えるだけだと、名前で結ばれた参照（Lua の entity プロパティ /
//   Trigger の filter・target）が無言で切れる。参照の書き換えと、その巻き戻しを
//   受け持つコマンドを CompositeCommand で束ねて「1 回の Undo で全部戻る」形にする。
void PushRenameCommand(entt::registry& reg, EditorContext& ctx, entt::entity e,
                       NameTag& tag, const char* newName)
{
    NameTag before = tag;
    const std::string oldName = before.name;
    tag.name = newName;
    RewriteEntityNameRefs(reg, oldName, tag.name);

    auto composite = std::make_unique<CompositeCommand>("Rename");
    composite->Add(std::make_unique<ComponentEditCommand<NameTag>>(&reg, e, before, tag, "Rename"));
    composite->Add(std::make_unique<RenameRefsCommand>(&reg, oldName, tag.name));
    ctx.undoSystem.PushCommand(std::move(composite));
}

// 行の面を窓の左端〜右端いっぱいに塗る（ImGui 標準は窓のパディング内側までしか塗らない）。
//   選択 = アクセント 30% + 左端 2px のアクセントライン / ホバー = Bg3 / 押下 = アクセント 42%
// ImGui 側の Header 色は透明にして呼ぶこと（二重に塗らない）。
void PaintRowBg(ImDrawList* dl, float y0, float y1, bool selected, bool hovered, bool held)
{
    // 見た目はテーマ・バリアントの共通ヘルパ（Default は従来と同じ描画）
    const ImRect r = ImGui::GetCurrentWindowRead()->InnerRect;
    ui::deco::RowFace(dl, ImVec2(r.Min.x, y0), ImVec2(r.Max.x, y1), selected, hovered, held);
}

// 型チップの表（アイコンと色。kTypes と同じ順）
struct ChipStyle { const char* glyph; const ImVec4* tint; };
ChipStyle ChipFor(int i)
{
    using namespace theme;
    switch (i)
    {
    case 0: return {ICON_T_MESH,     &TypeMesh};
    case 1: return {ICON_T_LIGHT,    &TypeLight};
    case 2: return {ICON_T_CAMERA,   &TypeCamera};
    case 3: return {ICON_T_UI,       &TypeUi};
    case 4: return {ICON_T_PHYSICS,  &TypePhysics};
    case 5: return {ICON_T_SCRIPT,   &TypeScript};
    case 6: return {ICON_T_PARTICLE, &TypeLight};
    case 7: return {ICON_T_AUDIO,    &TypeAudio};
    default: return {ICON_FOLDER,    &TypeFolder};
    }
}

uint64_t Mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 1099511628211ull;
}

} // namespace

// ------------------------------------------------------------------ 選択 ----

// 見えている並びの上で a〜b を範囲選択する。
// 「画面に見えている並び順」で選ぶので、畳んだグループの中身は巻き込まない（Unity と同じ）。
void HierarchyPanel::SelectRange(EditorContext& ctx, entt::entity a, entt::entity b, bool additive)
{
    const std::vector<hier::Row>& rows = m_filterActive ? m_filterRows : m_rows;
    std::vector<entt::entity> range;
    if (!hier::RangeBetween(rows, a, b, range))   // 起点が畳まれた/消えた → 単独選択へ退避
    {
        ctx.Select(b);
        return;
    }
    if (!additive) ctx.ClearSelection();
    for (entt::entity e : range) ctx.AddToSelection(e);
    ctx.selectedEntity = b;   // プライマリはクリックした行（Inspector にはこれが出る）
}

// 行クリックの選択規則: Shift=範囲 / Ctrl=トグル / それ以外=単独。
// Ctrl+Shift は既存の選択を残したまま範囲を足す。
void HierarchyPanel::HandleRowClick(EditorContext& ctx, entt::entity e)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && m_selectAnchor != entt::null && m_selectAnchor != e)
    {
        SelectRange(ctx, m_selectAnchor, e, io.KeyCtrl);
        return;   // 起点は動かさない（続けて Shift+クリックで範囲を伸ばせる）
    }
    if (io.KeyCtrl) ctx.ToggleSelection(e);
    else            ctx.Select(e);
    m_selectAnchor = e;
}

void HierarchyPanel::StartRename(entt::entity e, const std::string& currentName)
{
    m_renamingEntity = e;
    m_renameWarmup = 3;  // 3フレーム分フォーカス安定を待つ
    std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
    strncpy_s(m_renameBuf, currentName.c_str(), _TRUNCATE);
}

void HierarchyPanel::SetSubtreeOpen(entt::entity root, bool open)
{
    std::vector<entt::entity> st{root};
    while (!st.empty())
    {
        const entt::entity c = st.back(); st.pop_back();
        const auto [p, n] = m_index.Children(c);
        if (n == 0) continue;
        if (open) m_openNodes.insert(c); else m_openNodes.erase(c);
        for (size_t i = 0; i < n; ++i) st.push_back(p[i]);
    }
    ++m_openVersion;
}

// ------------------------------------------------------------------ モデル ----

// 索引 / 表示行 / 検索結果の更新。構造（エンティティ数・親子・順序・名前）が変わっていなければ何もしない。
// ★10 万体で毎フレーム O(N) の走査 + 兄弟ソートをしていたのをやめる。指紋は「Transform / NameTag の数 + Undo の編集通番 +
//   Undo/Redo の深さ + このパネルの操作」。Undo を積まない変更（Lua の Play 中の付け替えなど）は Play 中 6 フレーム、
//   編集中 90 フレームごとの定期更新で拾う（MCP の書き込みは EditSeq を進めるので即時）。
void HierarchyPanel::RefreshModel(entt::registry& reg, EditorContext& ctx)
{
    ++m_frame;
    // ★Undo / Redo は EditSeq を進めず、削除の取り消し / やり直しで「数も深さも同じだがエンティティ番号だけ違う」状態になりうる。
    //   プールの先頭と末尾の番号も指紋へ混ぜる。それでも取りこぼした分は、描画中に無効な行を見つけた時（m_dirty）に作り直す。
    const auto& namePool = reg.storage<NameTag>();
    uint64_t ends = 0;
    if (!namePool.empty())
        ends = (static_cast<uint64_t>(entt::to_integral(namePool.data()[0])) << 32) ^ static_cast<uint64_t>(entt::to_integral(namePool.data()[namePool.size() - 1]));
    const uint64_t key = hier::StructureKey(reg.storage<Transform>().size(), namePool.size(),
                                            ctx.undoSystem.EditSeq(), ctx.undoSystem.UndoDepth(),
                                            ctx.undoSystem.RedoDepth(), Mix(m_localVersion, ends));
    const uint32_t period = ctx.isPlaying ? 6u : 90u;
    if (!m_index.valid || m_index.key != key || m_dirty || (m_frame % period) == 0)
    {
        m_dirty = false;
        m_index.Build(reg);
        m_index.key = key;
        ++m_structVersion;
    }

    // 検索クエリ（入力が変わった時だけ解析し直す）
    if (m_filterPrev != m_filterBuf)
    {
        m_filterPrev = m_filterBuf;
        m_query = hier::ParseQuery(m_filterPrev);
    }
    // 型チップの選択は次回起動でも復元する（初回だけ読み、変わったら書く。同じ値の Set は何もしない）
    if (!m_prefsLoaded)
    {
        m_prefsLoaded = true;
        ctx.hierTypeFilter = static_cast<unsigned>(prefs::GetInt("hier.typeFilter", 0)) & hier::kAllTypes;
    }
    prefs::SetInt("hier.typeFilter", static_cast<int>(ctx.hierTypeFilter));
    const unsigned mask = ctx.hierTypeFilter & hier::kAllTypes;
    m_filterActive = mask != 0 || !m_query.Empty();

    if (!m_filterActive)
    {
        const uint64_t rk = Mix(Mix(m_structVersion, m_openVersion), 0x51);
        if (rk != m_rowsKey)
        {
            hier::BuildRows(m_index, m_openNodes, m_rows);
            m_rowsKey = rk;
        }
    }
    else
    {
        const uint64_t fk = Mix(Mix(Mix(m_structVersion, std::hash<std::string>{}(m_query.Signature())), mask), 0xF1);
        if (fk != m_filterKey)
        {
            m_filterKey = fk;
            hier::BuildRows(m_index, m_openNodes, m_allRows, /*openAll=*/true);
            m_filterRows.clear();
            for (const hier::Row& r : m_allRows)
            {
                if (!reg.valid(r.e) || !reg.all_of<NameTag>(r.e)) continue;
                if (hier::Matches(reg, r.e, reg.get<NameTag>(r.e).name, m_query, mask))
                    m_filterRows.push_back({r.e, 0});
            }
        }
    }
}

// ------------------------------------------------------------------ 行 ----

// 行の右端の目（非表示）と鍵（ロック）。
//   ・行にホバーしている間か、状態が立っている（自分か祖先）間だけ出す＝常時ノイズにしない。
//   ・自分に立っている状態は琥珀色で濃く、祖先から継承しているだけの状態は薄く（押せない）。
//   ・クリック = 切り替え（選択に含まれる行なら選択ぜんぶ）/ Alt+クリック = これだけ表示・これ以外をロック（もう一度で全部戻す）。
void HierarchyPanel::DrawFlagButtons(entt::registry& reg, EditorContext& ctx, entt::entity e,
                                     ImVec2 rowMin, ImVec2 rowMax, bool rowHovered)
{
    const bool anyHidden = eflags::AnyOf<EditorHidden>(reg);
    const bool anyLocked = eflags::AnyOf<EditorLocked>(reg);
    const bool selfHidden = anyHidden && eflags::Self<EditorHidden>(reg, e);
    const bool selfLocked = anyLocked && eflags::Self<EditorLocked>(reg, e);
    const bool effHidden  = selfHidden || (anyHidden && eflags::IsHidden(reg, e));
    const bool effLocked  = selfLocked || (anyLocked && eflags::IsLocked(reg, e));
    const bool showEye  = rowHovered || effHidden;
    const bool showLock = rowHovered || effLocked;
    if (!showEye && !showLock) return;

    namespace th = theme;
    const ImRect ir = ImGui::GetCurrentWindowRead()->InnerRect;
    const float colW = ui::Px(20.0f);
    const float lockX = ir.Max.x - ui::Px(4.0f) - colW;
    const float eyeX  = lockX - colW;
    const float h = rowMax.y - rowMin.y;
    const ImVec2 next = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto button = [&](const char* id, float x, const char* glyph, bool self, bool inherited, bool visible,
                      const char* tip, const char* tipInherited) -> bool
    {
        if (!visible) return false;
        ImGui::SetCursorScreenPos(ImVec2(x, rowMin.y));
        // 継承だけの状態は押せない（親を直す）。押せるのは自分の状態を持てる時
        const bool clickable = !inherited || self;
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(colW, h)) && clickable;
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s", inherited && !self ? tipInherited : tip);
        const ImVec2 c(x + colW * 0.5f, (rowMin.y + rowMax.y) * 0.5f);
        ImVec4 col;
        if (self)             col = th::Warn;
        else if (inherited)   col = th::WithAlpha(th::Warn, 0.45f);
        else                  col = hov ? th::Text : th::TextFaint;
        if (hov && !inherited) dl->AddRectFilled(ImVec2(x + ui::Px(1.0f), rowMin.y + ui::Px(1.0f)),
                                                 ImVec2(x + colW - ui::Px(1.0f), rowMax.y - ui::Px(1.0f)),
                                                 ImGui::GetColorU32(th::Bg4), ui::Px(3.0f));
        ui::DrawIconCentered(dl, glyph, c, ImGui::GetColorU32(col), ui::Px(14.0f));
        return pressed;
    };

    const bool eyePressed = button("##eye", eyeX, effHidden ? ICON_EYE_OFF : ICON_EYE, selfHidden,
                                   effHidden && !selfHidden, showEye,
                                   "表示 / 非表示（エディタのビューポートだけ。ゲームには影響しません）\nAlt+クリック: これだけ表示（もう一度で全部表示）",
                                   "親が非表示のため隠れています");
    const bool lockPressed = button("##lock", lockX, effLocked ? ICON_LOCK : ICON_UNLOCK, selfLocked,
                                    effLocked && !selfLocked, showLock,
                                    "ロック（ビューポートでの選択とギズモを無効にします）\nAlt+クリック: これ以外をロック（もう一度で全部解除）",
                                    "親がロックされているため選べません");
    vinput_gui::AnchorLastItem("row-flag", "flags");
    ImGui::SetCursorScreenPos(next);

    if (!eyePressed && !lockPressed) return;
    const bool alt = ImGui::GetIO().KeyAlt;
    if (eyePressed)
    {
        if (alt) hier::ApplyIsolate<EditorHidden>(reg, ctx, m_index, e, "Isolate");
        else     hier::ToggleFlag<EditorHidden>(reg, ctx, hier::ToggleTargets(ctx.selectedEntities, e), !selfHidden, "Visibility");
    }
    else
    {
        if (alt) hier::ApplyIsolate<EditorLocked>(reg, ctx, m_index, e, "Isolate Lock");
        else     hier::ToggleFlag<EditorLocked>(reg, ctx, hier::ToggleTargets(ctx.selectedEntities, e), !selfLocked, "Lock");
    }
    ++m_localVersion;
}

void HierarchyPanel::DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e, bool flat)
{
    if (!reg.valid(e) || !reg.all_of<NameTag>(e)) { m_dirty = true; return; }   // 古い行（次のフレームで索引を作り直す）
    auto& tag = reg.get<NameTag>(e);

    // ID スコープを明示分離 (ImGui 1.92 の TreeNode + D&D + popup 衝突対策)
    ImGui::PushID(static_cast<int>(static_cast<u32>(e)));

    // フィルタ中の行は子の開閉をしない（一致した行だけを平らに並べる）。子を持つ親でも葉として描く。
    const size_t childCount = flat ? 0 : m_index.ChildCount(e);
    const bool hasChildren = childCount > 0;
    const bool selected = ctx.IsSelected(e);

    // リネーム中はインライン入力を表示
    if (m_renamingEntity == e)
    {
        // ウォームアップ中は SetKeyboardFocusHere を毎フレーム呼ぶ
        // (初回フレームでフォーカスが安定しない問題の回避)
        if (m_renameWarmup > 0)
        {
            ImGui::SetKeyboardFocusHere();
            --m_renameWarmup;
        }

        ImGui::SetNextItemWidth(-1);
        bool entered = ui::InputText("##Rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        // Escape でキャンセル（元の名前に戻す）
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_renamingEntity = entt::null;
            ImGui::PopID();
            return;
        }

        // Enter で確定
        if (entered)
        {
            if (std::strlen(m_renameBuf) > 0 && tag.name != m_renameBuf)
            {
                PushRenameCommand(reg, ctx, e, tag, m_renameBuf);
            }
            m_renamingEntity = entt::null;
            ImGui::PopID();
            return;
        }

        // ウォームアップ完了後のみフォーカスロスで確定判定
        if (m_renameWarmup == 0)
        {
            bool active  = ImGui::IsItemActive();
            bool focused = ImGui::IsItemFocused();
            if (!active && !focused)
            {
                if (std::strlen(m_renameBuf) > 0 && tag.name != m_renameBuf)
                {
                    PushRenameCommand(reg, ctx, e, tag, m_renameBuf);
                }
                m_renamingEntity = entt::null;
            }
        }

        ImGui::PopID();
        return;  // リネーム中はツリーノード描画しない
    }

    // NoTreePushOnOpen: 子はこの関数から再帰せず、Render() の平坦化行リスト側で描く
    //（ListClipper で画面外の行を丸ごと省くため）。よって TreePop も不要。
    // SpanFullWidth: 選択/ホバーの面を行頭（インデントに関係なく左端）から右端まで塗る。
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool wasOpen = hasChildren && m_openNodes.count(e) != 0;

    // 開閉状態の指定は「TreeNodeEx の直前」でないと効かない。SetNextItemOpen が積む
    // NextItemData は次に出す 1 アイテムで消費されるので、間に別アイテムを挟むと
    // 全展開/全折りたたみや「選択行を露出」が一切効かなくなる（ImGui 内部の
    // 保存状態だけが真になり、m_openNodes は見た目に反映されない鏡になる）。
    const float rowX = ImGui::GetCursorScreenPos().x;   // 階層インデント適用後の行頭
    ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
    // 行の項目は右端の目 / 鍵の列（2 ボタン）を避けて終わらせる。重ねると TreeNode が押した瞬間にマウスを占有して、後から出すボタンが押せない。
    ImGuiWindow* hostWin = ImGui::GetCurrentWindow();
    const ImRect innerR = hostWin->InnerRect;
    const float colW = ui::Px(20.0f);
    const float oldWorkMaxX = hostWin->WorkRect.Max.x;
    hostWin->WorkRect.Max.x = innerR.Max.x - ui::Px(4.0f) - colW * 2.0f - ui::Px(4.0f);

    // ImGui 標準の矢印と文字は Text を透明にして隠し、開閉 chevron・種別アイコン・名前を自前で描く。
    // （標準の矢印は巨大な三角で UE 風に合わせられない。クリック判定・選択・D&D は TreeNodeEx がそのまま担う）
    // ID は PushID で一意化済みなので TreeNodeEx は固定文字列でOK
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));   // 行の面は PaintRowBg が窓幅いっぱいに塗る
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", tag.name.c_str());   // 文字は透明（UI 自動テストが行名で引けるよう渡しておく）
    ImGui::PopStyleColor(4);
    hostWin->WorkRect.Max.x = oldWorkMaxX;
    // ★行の項目に対する問い合わせ（ホバー / クリック / D&D / 右クリックメニュー）は、目 / 鍵のボタンを出す前に済ませる
    //   （どれも「直前の項目」に付くため）。
    const bool itemHov = ImGui::IsItemHovered();
    const bool itemClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool itemActive = ImGui::IsItemActive();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    dx12e::vinput_gui::AnchorLastItem("row", tag.name.c_str());   // dx12_imgui_find 用（エンティティ行）
    if (hasChildren && open != wasOpen)
    {
        if (open) m_openNodes.insert(e);
        else      m_openNodes.erase(e);
        ++m_openVersion;
    }

    const bool isFolder = eflags::Self<EditorFolder>(reg, e);
    const bool dimmed = (eflags::AnyOf<EditorHidden>(reg) && eflags::IsHidden(reg, e))
                     || (eflags::AnyOf<EntityDisabled>(reg) && eflags::IsDisabled(reg, e));
    // 行の面のホバーは、目 / 鍵の列の上でも保つ（行の項目は列の手前で終わっているので、項目のホバーだけだと列の上で消える）
    const bool rowHoveredFull = itemHov || (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)   // ★ボタンを押している間（ActiveId がある間）も真にする。でないと押した瞬間にボタンが消えて離した時に反応しない
        && ImGui::IsMouseHoveringRect(ImVec2(innerR.Min.x, rowMin.y), ImVec2(innerR.Max.x, rowMax.y))
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
        && ImGui::GetDragDropPayload() == nullptr);
    {
        namespace th = dx12e::theme;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = (rowMin.y + rowMax.y) * 0.5f;
        const ImGuiStyle& st = ImGui::GetStyle();
        const float fs = ImGui::GetFontSize();
        const bool hov = rowHoveredFull;

        // 選択行: アクセント 30% の面 + 左端 2px のアクセントライン（窓の左端〜右端いっぱい）
        PaintRowBg(dl, rowMin.y, rowMax.y, selected, hov, itemActive);

        // 開閉 chevron（子がある行だけ。葉は領域だけ確保して名前の桁を揃える）
        if (hasChildren)
            ui::DrawIconCentered(dl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                                 ImVec2(rowX + st.FramePadding.x + fs * 0.5f, cy),
                                 ImGui::GetColorU32(hov || selected ? th::Text : th::TextDim), ui::Px(13.0f));
        // 種別アイコン + 名前（非表示 / 無効は薄く）
        const float alpha = dimmed ? 0.42f : 1.0f;
        const float tx = rowX + fs + st.FramePadding.x * 2.0f;
        EntityGlyph g = PickEntityGlyph(reg, e, hasChildren);
        if (isFolder) g = {open && hasChildren ? ICON_FOLDER_OPEN : ICON_FOLDER, &th::TypeFolder};
        ui::DrawIconCentered(dl, g.glyph, ImVec2(tx + ui::Px(8.0f), cy),
                             ImGui::GetColorU32(th::WithAlpha(*g.tint, alpha)), ui::Px(16.0f));

        // 折りたたんだ親（と、フォルダ）は「中に何個いるか」を行の右端に出す（畳んだままでも規模が分かる）。
        // 数字は等幅。目 / 鍵の列のぶん内側へ寄せる。
        float clipRight = rowMax.x - ui::Px(6.0f);
        if (hasChildren && (!open || isFolder))
        {
            char cntBuf[16];
            snprintf(cntBuf, sizeof(cntBuf), "%zu", childCount);
            ui::PushMono();
            const ImVec2 ts = ImGui::CalcTextSize(cntBuf);
            const float bx = rowMax.x - ts.x - ui::Px(6.0f);
            dl->AddText(ImVec2(bx, std::floor(cy - ts.y * 0.5f + 0.5f)),
                        ImGui::GetColorU32(th::TextFaint), cntBuf);
            ui::PopMono();
            clipRight = bx - ui::Px(6.0f);
        }
        dl->PushClipRect(ImVec2(rowMin.x, rowMin.y), ImVec2((std::max)(clipRight, tx + ui::Px(24.0f)), rowMax.y), true);
        if (isFolder) ui::PushBold();
        dl->AddText(ImVec2(tx + ui::Px(22.0f), std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f)),
                    ImGui::GetColorU32(th::WithAlpha(th::Text, alpha)), tag.name.c_str());
        if (isFolder) ui::PopBold();
        dl->PopClipRect();
    }

    // ダブルクリック = ビューポートでフォーカス（UE / Unity と同じ。改名は F2 / 右クリック / ゆっくり 2 回クリック）
    const double now = ImGui::GetTime();
    if (itemHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        if (!ctx.IsSelected(e)) ctx.Select(e);
        ctx.pendingFocusSelection = true;
        m_lastClickTime = -10.0;
    }
    // シングルクリック選択（Ctrl=トグル / Shift=範囲）— ダブルクリック時は選択処理をスキップ
    else if (itemClicked && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool plain = !io.KeyCtrl && !io.KeyShift;
        const bool wasSole = ctx.selectedEntities.size() == 1 && ctx.IsSelected(e);
        // 複数選択したうちの1行を「掴んだ」だけの時点で単独選択に潰さない。
        // IsItemClicked は押した瞬間に true なので、ここで Select すると
        // ドラッグ開始前に選択が1件になり、まとめてのドラッグ移動ができなくなる。
        // 離した時にドラッグしていなければ単独選択へ確定する（エクスプローラと同じ規律）。
        if (plain && ctx.IsSelected(e) && ctx.selectedEntities.size() > 1)
            m_clickPendingEntity = e;
        else
            HandleRowClick(ctx, e);
        // ゆっくり 2 回クリック（選択済みの 1 行を 0.45〜1.6 秒あけて押す）= 改名。
        // ★押した瞬間ではなく「ドラッグせずに離した」時に始める（エクスプローラと同じ。押した瞬間に始めると、選んだ直後の行を掴んで動かせない）。
        if (plain && wasSole && m_lastClickEntity == e && now - m_lastClickTime > 0.45 && now - m_lastClickTime < 1.6)
        {
            m_slowRenameEntity = e;
            m_lastClickTime = -10.0;
        }
        else
        {
            m_slowRenameEntity = entt::null;
            m_lastClickEntity = e;
            m_lastClickTime = now;
        }
    }

    // 掴んだだけで終わった（ドラッグしなかった）場合の選択確定
    if (m_clickPendingEntity == e && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        // GetDragDropPayload() != null が「今ドラッグ中」の公開 API 版（IsDragDropActive は内部）
        if (ImGui::GetDragDropPayload() == nullptr && itemHov)
            HandleRowClick(ctx, e);
        m_clickPendingEntity = entt::null;
    }

    if (m_slowRenameEntity == e && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (ImGui::GetDragDropPayload() == nullptr && itemHov) StartRename(e, tag.name);
        m_slowRenameEntity = entt::null;
    }

    // D&D ソース（親子設定・並べ替え用）
    // SourceNoHoldToOpenOthers: ドラッグ中に他ツリーの自動展開を抑制
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers))
    {
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &e, sizeof(entt::entity));
        // 複数選択を掴んでいるときは件数を出す（ドロップ先は選択ぜんぶを受け取る）
        const size_t n = ctx.IsSelected(e) ? ctx.selectedEntities.size() : 1;
        if (n > 1) ImGui::Text("%s ほか %zu 件", tag.name.c_str(), n - 1);
        else       ImGui::Text("%s", tag.name.c_str());
        ImGui::EndDragDropSource();
    }

    // D&D ターゲット
    //   行の上 25% = 兄弟の前 / 下 25% = 兄弟の後ろ / 中央 = 子にする。ホバー中に挿入位置をライン（子にする時は枠）で見せる。
    //   ★ワールド位置（位置・向き・大きさ）を保ったまま親を替える（UE / Unity と同じ既定）。Shift を押しながら落とすと
    //     従来どおり「ローカル値そのまま」で付け替える。
    if (ImGui::BeginDragDropTarget())
    {
        const ImGuiDragDropFlags acceptFlags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY", acceptFlags))
        {
            const entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            // 掴んだ行が選択に含まれていれば選択ぜんぶを一度に移す（1個ずつ引っ張らなくていい）。
            std::vector<entt::entity> moving;
            if (ctx.IsSelected(droppedEntity)) moving = ctx.selectedEntities;
            else                               moving.push_back(droppedEntity);

            const float t = (rowMax.y > rowMin.y) ? (ImGui::GetIO().MousePos.y - rowMin.y) / (rowMax.y - rowMin.y) : 0.5f;
            // フィルタ中の行は並びが飛び飛びなので「子にする」だけ
            const hier::DropZone zone = flat ? hier::DropZone::Onto : hier::ZoneFromRatio(t);
            const hier::DropPlan plan = hier::PlanDrop(reg, moving, e, zone, m_index);
            if (plan.ok)
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImRect ir = ImGui::GetCurrentWindowRead()->InnerRect;
                const ImU32 col = ImGui::GetColorU32(theme::AccentHover);
                if (zone == hier::DropZone::Onto)
                {
                    dl->AddRectFilled(ImVec2(ir.Min.x, rowMin.y), ImVec2(ir.Max.x, rowMax.y),
                                      ImGui::GetColorU32(theme::WithAlpha(theme::Accent, 0.16f)));
                    dl->AddRect(ImVec2(ir.Min.x + 1.0f, rowMin.y), ImVec2(ir.Max.x - 1.0f, rowMax.y), col, ui::Px(3.0f), 0, ui::Px(1.5f));
                }
                else
                {
                    const float y = std::floor(zone == hier::DropZone::Before ? rowMin.y : rowMax.y) + 0.5f;
                    const float x0 = rowX + ImGui::GetFontSize() + ui::Px(6.0f);
                    dl->AddLine(ImVec2(x0, y), ImVec2(ir.Max.x - ui::Px(4.0f), y), col, ui::Px(2.0f));
                    dl->AddCircleFilled(ImVec2(x0, y), ui::Px(3.5f), col);
                }
            }
            if (payload->IsDelivery() && plan.ok)
            {
                const bool keepWorld = !ImGui::GetIO().KeyShift;
                if (hier::ApplyDrop(reg, ctx, plan, keepWorld))
                {
                    // 子にした親は開いて中身を見せる
                    if (zone == hier::DropZone::Onto) { m_openNodes.insert(e); ++m_openVersion; }
                    ++m_localVersion;
                }
            }
        }
        if (const ImGuiPayload* scriptPayload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
        {
            const char* pathCStr = static_cast<const char*>(scriptPayload->Data);
            std::string absPath(pathCStr);

            // assets 相対パスに変換
            namespace fs = std::filesystem;
            auto abs = fs::path(absPath).lexically_normal().string();
            auto base = fs::path(m_assetsDir).lexically_normal().string();
            std::replace(abs.begin(), abs.end(), '\\', '/');
            std::replace(base.begin(), base.end(), '\\', '/');
            std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

            char dbgBuf[512];
            snprintf(dbgBuf, sizeof(dbgBuf),
                "[Hierarchy DND_SCRIPT] entity=%u abs=%s base=%s rel=%s\n",
                static_cast<u32>(e), abs.c_str(), base.c_str(), rel.c_str());
            OutputDebugStringA(dbgBuf);

            ctx.pendingScriptAttachments.push_back({e, rel});
        }
        ImGui::EndDragDropTarget();
    }

    // 右クリックコンテキストメニュー (明示 ID 必須 ・ D&D の後に置く)
    // PushID スコープ内なので ID は "EntityCtx" だけで十分 unique
    ui::PushMenuStyle();
    if (ImGui::BeginPopupContextItem("EntityCtx", ImGuiPopupFlags_MouseButtonRight))
    {
        DrawEntityContextMenu(reg, ctx, e, tag.name);
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // 目 / 鍵（行の項目の後に出す）
    DrawFlagButtons(reg, ctx, e, rowMin, rowMax, rowHoveredFull);

    // 子は Render() の行リストが続けて描く（ここでは再帰しない）。
    ImGui::PopID();
}

// 行の右クリックメニュー（ImGui::MenuItem の ID = ラベルなので UI テストが名前で引ける）
void HierarchyPanel::DrawEntityContextMenu(entt::registry& reg, EditorContext& ctx, entt::entity e, const std::string& name)
{
    const cmd::Env env{m_scene, m_assetsDir};
    // 右クリックした行が選択に入っていなければ、その行だけを選択する（他アプリと同じ）
    auto ensureSelected = [&]() { if (!ctx.IsSelected(e)) ctx.Select(e); };

    if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe5\xa4\x89\xe6\x9b\xb4", cmd::ShortcutText("edit.rename")))  // 名前変更
        StartRename(e, name);
    if (ImGui::MenuItem("選択にフォーカス", cmd::ShortcutText("edit.focus")))
    {
        ensureSelected();
        ctx.pendingFocusSelection = true;
    }
    ImGui::Separator();

    if (ImGui::MenuItem("\xe8\xa4\x87\xe8\xa3\xbd", cmd::ShortcutText("edit.duplicate")))  // 複製
    {
        ensureSelected();
        for (auto s : ctx.selectedEntities) ctx.pendingDuplications.push_back(s);
    }
    if (ImGui::MenuItem("コピー", cmd::ShortcutText("edit.copy"), false, m_scene != nullptr))
    {
        ensureSelected();
        cmd::Execute(ctx, env, "edit.copy");
    }
    if (ImGui::MenuItem("貼り付け", cmd::ShortcutText("edit.paste"), false, !ctx.clipboard.empty()))
        ctx.pendingPastes = ctx.clipboard;
    // ★Play 中は無効。消化側（ApplicationRender）が Editor モードでしか回らないので、
    //   押せてしまうと要求が誰にも読まれず、ログにも出ないまま消える。
    if (ImGui::MenuItem("プレハブにする", nullptr, false, !ctx.isPlaying))
        ctx.pendingCreatePrefab = e;
    ImGui::Separator();

    // ---- 作成 / 整理 ----
    if (ImGui::BeginMenu("子を作成"))
    {
        if (ImGui::MenuItem("空のエンティティ"))
        {
            const entt::entity c = hier::CreateEmptyChild(reg, ctx, e);
            if (c != entt::null)
            {
                ctx.Select(c);
                ctx.requestRenameEntity = c;
                m_openNodes.insert(e);
                ++m_openVersion; ++m_localVersion;
            }
        }
        if (ImGui::MenuItem("フォルダ"))
        {
            // 空のフォルダを e の子として作る（作成直後は選択物を入れない）
            const entt::entity f = hier::CreateFolder(reg, ctx, {}, "Folder", e);
            if (f != entt::null)
            {
                ctx.Select(f);
                ctx.requestRenameEntity = f;
                m_openNodes.insert(e);
                ++m_openVersion; ++m_localVersion;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("選択をフォルダに入れる"))
    {
        ensureSelected();
        const entt::entity f = hier::CreateFolder(reg, ctx, ctx.selectedEntities);
        if (f != entt::null)
        {
            ctx.Select(f);
            ctx.requestRenameEntity = f;
            m_openNodes.insert(f);
            ++m_openVersion; ++m_localVersion;
        }
    }
    // 選択をまとめる空の親を作る。親は原点・無回転なので見た目は一切動かない。
    if (ImGui::MenuItem("選択をグループ化", "Ctrl+G", false, ctx.HasSelection()))
    {
        ensureSelected();   // 右クリックした行だけの場合
        ctx.pendingGroupSelection = true;
    }
    ImGui::Separator();

    // ---- 表示 / ロック（エディタ専用。ゲームには影響しない）----
    {
        const bool selfHidden = eflags::Self<EditorHidden>(reg, e);
        const bool selfLocked = eflags::Self<EditorLocked>(reg, e);
        if (ImGui::MenuItem(selfHidden ? "表示する" : "非表示にする", cmd::ShortcutText("edit.toggleHidden")))
        {
            hier::ToggleFlag<EditorHidden>(reg, ctx, hier::ToggleTargets(ctx.selectedEntities, e), !selfHidden, "Visibility");
            ++m_localVersion;
        }
        const bool isoH = hier::IsIsolated<EditorHidden>(reg, m_index, e);
        if (ImGui::MenuItem(isoH ? "すべて表示する" : "これだけ表示する"))
        {
            hier::ApplyIsolate<EditorHidden>(reg, ctx, m_index, e, "Isolate");
            ++m_localVersion;
        }
        if (ImGui::MenuItem(selfLocked ? "ロックを解除" : "ロックする", cmd::ShortcutText("edit.toggleLocked")))
        {
            hier::ToggleFlag<EditorLocked>(reg, ctx, hier::ToggleTargets(ctx.selectedEntities, e), !selfLocked, "Lock");
            ++m_localVersion;
        }
        const bool isoL = hier::IsIsolated<EditorLocked>(reg, m_index, e);
        if (ImGui::MenuItem(isoL ? "すべてのロックを解除" : "これ以外をロックする"))
        {
            hier::ApplyIsolate<EditorLocked>(reg, ctx, m_index, e, "Isolate Lock");
            ++m_localVersion;
        }
    }
    ImGui::Separator();

    // ---- 階層 ----
    if (m_index.HasChildren(e))
    {
        if (ImGui::MenuItem("子孫を選択"))
        {
            std::vector<entt::entity> st{e};
            ctx.Select(e);
            while (!st.empty())
            {
                const entt::entity c = st.back(); st.pop_back();
                const auto [p, n] = m_index.Children(c);
                for (size_t i = 0; i < n; ++i) { ctx.AddToSelection(p[i]); st.push_back(p[i]); }
            }
            ctx.selectedEntity = e;
        }
        if (ImGui::MenuItem("子孫をすべて展開")) SetSubtreeOpen(e, true);
        if (ImGui::MenuItem("子孫をすべて折りたたむ")) SetSubtreeOpen(e, false);
    }
    // 親子解除
    if (reg.all_of<Transform>(e) && reg.get<Transform>(e).parent != entt::null)
    {
        if (ImGui::MenuItem("\xe8\xa6\xaa\xe3\x81\x8b\xe3\x82\x89\xe5\xa4\x96\xe3\x81\x99"))  // 親から外す
        {
            // ワールド位置は保つ（外した瞬間に物が飛ばない）
            Transform before = reg.get<Transform>(e);
            if (xform::Reparent(reg, e, entt::null, /*keepWorld=*/true))
            {
                ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                    &reg, e, before, reg.get<Transform>(e)));
                ++m_localVersion;
            }
        }
    }
    // 階層の項目（子孫 / 親から外す）が 1 つも無い行では、区切り線を重ねない
    if (m_index.HasChildren(e) || (reg.all_of<Transform>(e) && reg.get<Transform>(e).parent != entt::null))
        ImGui::Separator();

    if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4", cmd::ShortcutText("edit.delete")))  // 削除
    {
        // Undo コマンドは Application の遅延削除処理で積まれる
        ctx.pendingDeletions.push_back(e);
        // e 本体だけでなく e のサブツリーに含まれる選択も外す（残すと削除後に
        // ダングリング選択ハンドルが selectedEntities に残る）
        auto isSelfOrDescendant = [&](entt::entity s) {
            int depth = 0;
            for (entt::entity cur = s; cur != entt::null && depth < 4096; ++depth)
            {
                if (cur == e) return true;
                auto* t = reg.try_get<Transform>(cur);
                cur = t ? t->parent : entt::null;
            }
            return false;
        };
        auto& sel = ctx.selectedEntities;
        sel.erase(std::remove_if(sel.begin(), sel.end(), isSelfOrDescendant), sel.end());
        if (ctx.selectedEntity == e || (ctx.selectedEntity != entt::null
                                        && isSelfOrDescendant(ctx.selectedEntity)))
            ctx.selectedEntity = sel.empty() ? entt::null : sel.back();
    }
}

// ------------------------------------------------------------------ 上部 ----

// 型チップ（アイコンだけ・複数選択 = OR）。押した型の物だけに絞る。
void HierarchyPanel::DrawTypeChips(EditorContext& ctx)
{
    const float btn = ui::Px(24.0f);
    ImGui::PushID("##hierTypeChips");
    for (int i = 0; i < hier::kTypeCount; ++i)
    {
        const hier::TypeInfo& ti = hier::kTypes[i];
        const ChipStyle cs = ChipFor(i);
        const bool on = (ctx.hierTypeFilter & ti.bit) != 0;
        if (i > 0) ImGui::SameLine(0.0f, ui::Px(2.0f));
        char id[32];
        snprintf(id, sizeof(id), "type_%s", ti.key);
        char tip[64];
        snprintf(tip, sizeof(tip), "%s だけ表示（複数選択で OR）", ti.label);
        if (ui::IconButton(id, cs.glyph, tip, on, cs.tint, btn, ui::Px(15.0f)))
            ctx.hierTypeFilter ^= ti.bit;
    }
    if (ctx.hierTypeFilter != 0)
    {
        ImGui::SameLine(0.0f, ui::Px(6.0f));
        if (ui::IconButton("type_clear", ICON_CLOSE, "型の絞り込みを解除", false, nullptr, btn, ui::Px(13.0f)))
            ctx.hierTypeFilter = 0;
    }
    ImGui::PopID();
}

void HierarchyPanel::DrawHeader(entt::registry& reg, EditorContext& ctx, size_t objCount)
{
    // ---- 件数 + 全展開/全折りたたみ ----
    // オブジェクトが増えると縦に膨れて目的の行が探せなくなるので、
    // 「一発で全部畳む」と「名前で絞る」を常に手元に置く。
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%zu 個のオブジェクト", objCount);
    ImGui::PopStyleColor();

    // 隠れている数 / ロック数（あるときだけ）
    const size_t nHidden = hier::CountSelf<EditorHidden>(reg);
    const size_t nLocked = hier::CountSelf<EditorLocked>(reg);
    if (nHidden > 0)
    {
        ImGui::SameLine(0.0f, ui::Px(8.0f));
        ui::Icon(ICON_EYE_OFF, theme::Warn);
        ImGui::SameLine(0.0f, ui::Px(2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn);
        ImGui::Text("%zu", nHidden);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("エディタで非表示にしている物: %zu 件（ゲームには影響しません）", nHidden);
    }
    if (nLocked > 0)
    {
        ImGui::SameLine(0.0f, ui::Px(8.0f));
        ui::Icon(ICON_LOCK, theme::Warn);
        ImGui::SameLine(0.0f, ui::Px(2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn);
        ImGui::Text("%zu", nLocked);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ロックしている物: %zu 件（ビューポートで選べません）", nLocked);
    }

    // 右端に折りたたむ/展開のアイコンボタン
    const float btn = ui::Px(22.0f);   // DPI
    ImGui::SameLine(ImGui::GetContentRegionMax().x - btn * 2.0f - ui::Px(2.0f));
    if (ui::IconButton("collapseAll", ICON_FOLD_ALL, "すべて折りたたむ", false, nullptr, btn, ui::Px(15.0f)))
    {
        m_openNodes.clear();
        ++m_openVersion;
    }
    ImGui::SameLine(0.0f, ui::Px(2.0f));
    if (ui::IconButton("expandAll", ICON_UNFOLD_ALL, "すべて展開", false, nullptr, btn, ui::Px(15.0f)))
    {
        m_openNodes.insert(m_index.parents.begin(), m_index.parents.end());   // 子を持つ親をすべて開く
        ++m_openVersion;
    }

    // ---- 検索 ----
    ImGui::SetNextItemWidth(-1.0f);
    ui::SearchField("##HierFilter", m_filterBuf, sizeof(m_filterBuf), "名前 / c: / tag: / t: で検索");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("名前・コンポーネント名・タグで検索します\n"
                          "tag:enemy   タグ\nc:light   コンポーネント名\nt:mesh   型（メッシュ / ライト / カメラ / UI / 物理 …）\n"
                          "n:door   名前だけ\nis:hidden  is:locked   状態\n空白で区切ると AND");

    // ---- 型チップ ----
    DrawTypeChips(ctx);
}

// オブジェクトが 0 件のときの案内（パネルの中だけ。ゲームの絵には何も重ねない）
void HierarchyPanel::DrawEmptyState(entt::registry& reg, EditorContext& ctx)
{
    (void)reg;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, (std::max)(ui::Px(16.0f), avail.y * 0.12f)));
    auto centered = [](const char* text, const ImVec4& col) {
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((std::max)(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f + ImGui::GetCursorPosX()));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    };
    centered("シーンは空です", theme::Text);
    centered("アセットをビューポートへドラッグするか、", theme::TextDim);
    centered("下のボタンで作成できます", theme::TextDim);
    ImGui::Dummy(ImVec2(0.0f, ui::Px(8.0f)));
    const float bw = (std::min)(avail.x - ui::Px(24.0f), ui::Px(190.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw) * 0.5f);
    if (ui::PrimaryButton(ICON_PLUS " エンティティを作成", ImVec2(bw, 0.0f)))
        ImGui::OpenPopup("AddEntityPopup");
    (void)ctx;
}

// ---- 空白部分（行の下の残り全部）----
// 以前の「空白への D&D」は直前の行を的にしていて、空白に落としても効かず、最後の行に落とすと
// 「子にする」と「親を外す」が同じフレームで両方走りかねなかった。残りの領域を 1 つの項目にして的にする。
//   ・右クリック: エンティティ作成メニュー（作成位置はエディタカメラの前 / 床との交点）
//   ・ドロップ: 親を外してルートへ（ワールド位置は保つ。Shift でローカル値そのまま）
//   ・左クリック: 選択解除
void HierarchyPanel::DrawBackgroundArea(entt::registry& reg, EditorContext& ctx)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##HierBg", ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, ui::Px(24.0f))));
    dx12e::vinput_gui::AnchorLastItem("hier-bg", "空白");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
        ctx.ClearSelection();

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            const entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            // 行の上と同じく、掴んだ行が選択に含まれていれば選択ぜんぶをルートへ出す（グループ解除）。
            std::vector<entt::entity> moving;
            if (ctx.IsSelected(droppedEntity)) moving = ctx.selectedEntities;
            else                               moving.push_back(droppedEntity);

            const bool keepWorld = !ImGui::GetIO().KeyShift;
            auto composite = std::make_unique<CompositeCommand>("Unparent");
            for (entt::entity d : moving)
            {
                if (!reg.valid(d) || !reg.all_of<Transform>(d)) continue;
                if (reg.get<Transform>(d).parent == entt::null) continue;
                Transform before = reg.get<Transform>(d);
                if (!xform::Reparent(reg, d, entt::null, keepWorld)) continue;
                composite->Add(std::make_unique<TransformCommand>(&reg, d, before, reg.get<Transform>(d)));
            }
            if (!composite->Empty())
            {
                ctx.undoSystem.PushCommand(std::move(composite));
                ++m_localVersion;
            }
        }
        ImGui::EndDragDropTarget();
    }

    ui::PushMenuStyle();
    if (ImGui::BeginPopupContextItem("##HierBgCtx", ImGuiPopupFlags_MouseButtonRight))
    {
        ImGui::TextDisabled("エンティティを作成");
        ImGui::Separator();
        cmd::DrawCreateMenu(ctx);
        ImGui::Separator();
        if (ImGui::MenuItem("フォルダを作成"))
        {
            const entt::entity f = hier::CreateFolder(reg, ctx, ctx.selectedEntities);
            if (f != entt::null)
            {
                ctx.Select(f);
                ctx.requestRenameEntity = f;
                m_openNodes.insert(f);
                ++m_openVersion; ++m_localVersion;
            }
        }
        if (!ctx.clipboard.empty())
        {
            if (ImGui::MenuItem("貼り付け", cmd::ShortcutText("edit.paste")))
                ctx.pendingPastes = ctx.clipboard;
        }
        if (eflags::AnyOf<EditorHidden>(reg) && ImGui::MenuItem("すべて表示する", cmd::ShortcutText("edit.showAll")))
        {
            hier::ToggleFlag<EditorHidden>(reg, ctx, hier::PlanClearAll<EditorHidden>(reg), false, "Visibility");
            ++m_localVersion;
        }
        if (eflags::AnyOf<EditorLocked>(reg) && ImGui::MenuItem("すべてのロックを解除"))
        {
            hier::ToggleFlag<EditorLocked>(reg, ctx, hier::PlanClearAll<EditorLocked>(reg), false, "Lock");
            ++m_localVersion;
        }
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();
}

// ------------------------------------------------------------------ 本体 ----

void HierarchyPanel::Render(entt::registry& reg, EditorContext& ctx)
{
    // CPU 時間の計測（10 万体テスト / 診断用。ctx.hierRenderMs へ書く）
    struct ScopeMs
    {
        float& out; LARGE_INTEGER t0, freq;
        explicit ScopeMs(float& o) : out(o) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); }
        ~ScopeMs() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); out = static_cast<float>(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)); }
    } scopeMs(ctx.hierRenderMs);

    const bool dbgBegin = ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc");  // Hierarchy
    {
        static int lastState = -1;
        ImGuiWindow* dw = ImGui::GetCurrentWindow();
        const int st = (dbgBegin ? 1 : 0) | (dw->SkipItems ? 2 : 0) | (dw->Collapsed ? 4 : 0) | (dw->Hidden ? 8 : 0) | (ImGui::IsWindowFocused() ? 16 : 0);
        if (st != lastState) { lastState = st; Logger::Info("[hier-dbg] Begin={} skip={} collapsed={} hidden={} focused={} size=({:.0f},{:.0f}) clip=({:.0f},{:.0f},{:.0f},{:.0f}) frame={}", dbgBegin, dw->SkipItems, dw->Collapsed, dw->Hidden, ImGui::IsWindowFocused(), dw->Size.x, dw->Size.y, dw->ClipRect.Min.x, dw->ClipRect.Min.y, dw->ClipRect.Max.x, dw->ClipRect.Max.y, ImGui::GetFrameCount()); }
    }

    // 生成直後の名前入力要求（グループ化など）。作った親は開いた状態にして中身を見せる。
    if (ctx.requestRenameEntity != entt::null)
    {
        if (reg.valid(ctx.requestRenameEntity) && reg.all_of<NameTag>(ctx.requestRenameEntity))
        {
            StartRename(ctx.requestRenameEntity, reg.get<NameTag>(ctx.requestRenameEntity).name);
            m_openNodes.insert(ctx.requestRenameEntity);
            ++m_openVersion;
            m_scrollToEntity = ctx.requestRenameEntity;   // 画面外なら見える位置へ（F2 で行が見えない事故を避ける）
        }
        ctx.requestRenameEntity = entt::null;
    }

    // 親→子の索引 / 表示行 / 検索結果（構造が変わった時だけ作り直す）
    RefreshModel(reg, ctx);

    // 選択が外から変わった（3Dビューでクリック / MCP / F フォーカス）なら、その行を露出する。
    // 祖先グループを開き、次の描画でスクロールして見せる＝畳んだ階層の中でも迷子にならない。
    if (ctx.selectedEntity != m_lastRevealed)
    {
        m_lastRevealed = ctx.selectedEntity;
        if (reg.valid(ctx.selectedEntity) && reg.all_of<Transform>(ctx.selectedEntity))
        {
            bool changed = false;
            entt::entity p = reg.get<Transform>(ctx.selectedEntity).parent;
            for (int d = 0; p != entt::null && reg.valid(p) && d < 4096; ++d)
            {
                changed |= m_openNodes.insert(p).second;
                const auto* pt = reg.try_get<Transform>(p);
                p = pt ? pt->parent : entt::null;
            }
            if (changed)
            {
                ++m_openVersion;
                RefreshModel(reg, ctx);   // 開いた分を表示行へ反映（同じフレームでスクロール先を描くため）
            }
            m_scrollToEntity = ctx.selectedEntity;
        }
    }

    // オブジェクト数（GridPlane は内部用なので除外）。プールの大きさだけで数える（O(1)）。
    const size_t objCount = reg.storage<NameTag>().size() - (std::min)(reg.storage<NameTag>().size(), reg.storage<GridPlane>().size());

    DrawHeader(reg, ctx, objCount);
    ImGui::Spacing();

    if (objCount == 0 && !m_filterActive)
    {
        DrawEmptyState(reg, ctx);
    }
    else
    {
    // ---- 行の見やすさ（行高 23 + ごく弱いゼブラ + 階層ガイド線）----
    // 行ピッチ = フレーム高 23 + 1。標準の行間(4)だと選択面の間に隙間ができて縞に見える。
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(ImGui::GetStyle().ItemSpacing.x, ui::Px(1.0f)));
    // ツリー矢印スペースは FontSize + FramePadding.x*2。詰めて（アイコン+名前を 1 つの行として）見せる。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(4.0f, 3.5f));
    // 選択行はアクセント 30%（テーマの Header 色）。ホバーは Bg3。テーマ既定のままで良いので色は上書きしない。

    const float rowH = ImGui::GetFrameHeight();   // 描画される 1 行の高さ（行間を除く）

    // 行の縞模様。ごく弱く（白 3%）: 長いリストで行を目で追える程度に留め、選択面を主役にする。
    // 行を描く直前に呼び、窓幅いっぱいの帯を1本敷く。
    auto zebra = [rowH](int index)
    {
        if ((index & 1) == 0) return;
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(wp.x, p.y),
            ImVec2(wp.x + ImGui::GetWindowSize().x, p.y + rowH),
            IM_COL32(255, 255, 255, 8));
    };

    // 階層ガイド線。どの行がどの親の下にいるのか、インデント量を数えずに追えるようにする。
    // rowStart = インデント適用後の行頭スクリーン座標。線は親の開閉 chevron の真下に引く。
    auto indentGuides = [rowH](ImVec2 rowStart, int depth, float indentW)
    {
        if (depth <= 0) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_TreeLines);
        const float chevX = ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f;
        for (int j = 1; j <= depth; ++j)
        {
            const float x = std::floor(rowStart.x - indentW * static_cast<float>(j) + chevX) + 0.5f;
            dl->AddLine(ImVec2(x, rowStart.y), ImVec2(x, rowStart.y + rowH + ui::Px(1.0f)), col);
        }
        // 一番内側だけ横に伸ばして「この親の子」と分かるようにする
        const float xIn = std::floor(rowStart.x - indentW + chevX) + 0.5f;
        const float yMid = std::floor(rowStart.y + rowH * 0.5f) + 0.5f;
        dl->AddLine(ImVec2(xIn, yMid), ImVec2(rowStart.x + chevX - ui::Px(2.0f), yMid), col);
    };

    if (m_filterActive)
    {
        // フィルタ中はツリーを畳んで、一致のフラットリストを表示（こちらも clipper で間引く）。
        // 行は通常のツリー行と同じ DrawEntityNode（flat）で描く＝右クリックメニュー / D&D（親子付け替え）/
        // 改名(F2・ダブルクリック) / 複数選択 / 目と鍵が、フィルタ中も通常時と同じに効く。
        ImGuiListClipper fclip;
        fclip.Begin(static_cast<int>(m_filterRows.size()));
        while (fclip.Step())
        for (int fi = fclip.DisplayStart; fi < fclip.DisplayEnd; ++fi)
        {
            zebra(fi);
            DrawEntityNode(reg, ctx, m_filterRows[static_cast<size_t>(fi)].e, /*flat=*/true);
        }
        if (m_filterRows.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
            ImGui::TextUnformatted("一致するオブジェクトがありません");
            ImGui::PopStyleColor();
        }
    }
    else
    {
        // ツリーを「見えている行」だけの平坦なリストに畳んでから ImGuiListClipper で
        // 画面外を丸ごと省く。ImGui へ全ノードを積むと 5万体で ~7ms 溶ける（実測）。
        const float indentW = ImGui::GetStyle().IndentSpacing;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        // 画面外の行は clipper が省くので、スクロール先の行だけは強制的に描かせる
        // （でないと SetScrollHereY を呼ぶ機会が来ない）。
        if (m_scrollToEntity != entt::null)
            for (size_t si = 0; si < m_rows.size(); ++si)
                if (m_rows[si].e == m_scrollToEntity)
                {
                    clipper.IncludeItemByIndex(static_cast<int>(si));
                    break;
                }
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const hier::Row r = m_rows[static_cast<size_t>(i)];   // コピー（描画中の操作で m_rows が作り直されても安全）
                zebra(i);
                if (r.depth > 0) ImGui::Indent(indentW * r.depth);
                indentGuides(ImGui::GetCursorScreenPos(), r.depth, indentW);
                if (r.e == m_scrollToEntity)
                {
                    ImGui::SetScrollHereY(0.5f);
                    m_scrollToEntity = entt::null;
                }
                DrawEntityNode(reg, ctx, r.e, /*flat=*/false);
                if (r.depth > 0) ImGui::Unindent(indentW * r.depth);
            }
        }
    }

    ImGui::PopStyleVar(2);     // ItemSpacing / FramePadding
    }   // 空状態でない

    // ヒエラルキーにフォーカスがある状態で Del キー → 選択エンティティを削除。
    // 実際の削除は pendingDeletions 経由でフレーム境界に行われ、Undo も積まれる。
    // ★フォーカスのあるパネルだけが反応する（ビューポート側の Del は focusedPanel==None のときだけ）。
    //   名前フィルタなどテキスト入力中は反応しない（"Del" で文字を消したいだけなのに削除される事故を防ぐ）。
    const bool keyOk = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                    && m_renamingEntity == entt::null
                    && !ImGui::GetIO().WantTextInput;
    if (keyOk && ctx.HasSelection() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        for (auto e : ctx.selectedEntities)
            if (reg.valid(e)) ctx.pendingDeletions.push_back(e);
        ctx.ClearSelection();
    }

    // Ctrl+G: 選択をグループ化（空の親にまとめる）。実処理は Application のフレーム境界。
    if (keyOk && ctx.HasSelection()
        && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        ctx.pendingGroupSelection = true;
    }
    // Ctrl+A: 見えている行をぜんぶ選択（複数選択の入口）
    if (keyOk && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        const std::vector<hier::Row>& rows = m_filterActive ? m_filterRows : m_rows;
        if (!rows.empty())
        {
            ctx.ClearSelection();
            for (const hier::Row& r : rows) ctx.AddToSelection(r.e);
        }
    }

    ImGui::Separator();

    // Add entity menu（作成項目は editor/EditorCreateTable.h の表。空白の右クリックと共用）
    if (ImGui::Button(ICON_PLUS " エンティティ追加"))
        ImGui::OpenPopup("AddEntityPopup");

    ui::PushMenuStyle();
    if (ImGui::BeginPopup("AddEntityPopup"))
    {
        cmd::DrawCreateMenu(ctx);
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    DrawBackgroundArea(reg, ctx);

    ImGui::End();
}

} // namespace dx12e
