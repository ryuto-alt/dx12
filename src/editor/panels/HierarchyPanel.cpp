#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/EntityGlyph.h"
#include "editor/UndoSystem.h"
#include "editor/EditorCommands.h"   // 作成メニュー（エンティティ追加と共用）
#include "editor/TransformMath.h"    // ワールド位置を保つ親替え
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー
#include "ecs/Components.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // InnerRect（行の面を窓の左端〜右端いっぱいに塗る）
#pragma warning(pop)

#include <algorithm>
#include <filesystem>
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

} // namespace

// 行の面を窓の左端〜右端いっぱいに塗る（ImGui 標準は窓のパディング内側までしか塗らない）。
//   選択 = アクセント 30% + 左端 2px のアクセントライン / ホバー = Bg3 / 押下 = アクセント 42%
// ImGui 側の Header 色は透明にして呼ぶこと（二重に塗らない）。
static void PaintRowBg(ImDrawList* dl, float y0, float y1, bool selected, bool hovered, bool held)
{
    namespace th = dx12e::theme;
    const ImRect r = ImGui::GetCurrentWindowRead()->InnerRect;
    const ImVec4* col = nullptr;
    if (held)          col = &th::SelectionActive;
    else if (selected) col = hovered ? &th::SelectionActive : &th::Selection;
    else if (hovered)  col = &th::Bg3;
    if (col)
        dl->AddRectFilled(ImVec2(r.Min.x, y0), ImVec2(r.Max.x, y1), ImGui::GetColorU32(*col));
    if (selected)
        dl->AddRectFilled(ImVec2(r.Min.x, y0), ImVec2(r.Min.x + ui::Px(2.0f), y1), ImGui::GetColorU32(th::Accent));
}

// 大文字小文字を無視した部分一致（Hierarchy フィルタ用）
static bool ContainsCI(const std::string& haystack, const char* needle)
{
    if (!needle || !*needle) return true;
    std::string h = haystack, n = needle;
    auto lower = [](std::string& s){ for (char& ch : s) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch))); };
    lower(h); lower(n);
    return h.find(n) != std::string::npos;
}

const std::vector<entt::entity> HierarchyPanel::s_noChildren;

// m_rows（今フレームに実際に並んでいる行）の上で a〜b を範囲選択する。
// 「画面に見えている並び順」で選ぶので、畳んだグループの中身は巻き込まない（Unity と同じ）。
void HierarchyPanel::SelectRange(EditorContext& ctx, entt::entity a, entt::entity b, bool additive)
{
    size_t ia = m_rows.size(), ib = m_rows.size();
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        if (m_rows[i].e == a) ia = i;
        if (m_rows[i].e == b) ib = i;
    }
    if (ia == m_rows.size() || ib == m_rows.size())   // 起点が畳まれた/消えた → 単独選択へ退避
    {
        ctx.Select(b);
        return;
    }
    if (ia > ib) std::swap(ia, ib);

    if (!additive) ctx.ClearSelection();
    for (size_t i = ia; i <= ib; ++i)
        ctx.AddToSelection(m_rows[i].e);
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

void HierarchyPanel::DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e, bool flat)
{
    if (!reg.all_of<NameTag>(e)) return;
    auto& tag = reg.get<NameTag>(e);

    // ID スコープを明示分離 (ImGui 1.92 の TreeNode + D&D + popup 衝突対策)
    ImGui::PushID(static_cast<int>(static_cast<u32>(e)));

    // フィルタ中の行は子の開閉をしない（一致した行だけを平らに並べる）。子を持つ親でも葉として描く。
    auto itKids = flat ? m_childIndex.end() : m_childIndex.find(e);
    const std::vector<entt::entity>& children =
        (itKids == m_childIndex.end()) ? s_noChildren : itKids->second;
    bool hasChildren = !children.empty();
    bool selected = ctx.IsSelected(e);

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

    // ImGui 標準の矢印と文字は Text を透明にして隠し、開閉 chevron・種別アイコン・名前を自前で描く。
    // （標準の矢印は巨大な三角で UE 風に合わせられない。クリック判定・選択・D&D は TreeNodeEx がそのまま担う）
    // ID は PushID で一意化済みなので TreeNodeEx は固定文字列でOK
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));   // 行の面は PaintRowBg が窓幅いっぱいに塗る
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", tag.name.c_str());   // 文字は透明（UI 自動テストが行名で引けるよう渡しておく）
    ImGui::PopStyleColor(4);
    dx12e::vinput_gui::AnchorLastItem("row", tag.name.c_str());   // dx12_imgui_find 用（エンティティ行）
    if (hasChildren && open != wasOpen)
    {
        if (open) m_openNodes.insert(e);
        else      m_openNodes.erase(e);
    }

    {
        namespace th = dx12e::theme;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const float cy = (mn.y + mx.y) * 0.5f;
        const ImGuiStyle& st = ImGui::GetStyle();
        const float fs = ImGui::GetFontSize();
        const bool hov = ImGui::IsItemHovered();

        // 選択行: アクセント 30% の面 + 左端 2px のアクセントライン（窓の左端〜右端いっぱい）
        PaintRowBg(dl, mn.y, mx.y, selected, hov, ImGui::IsItemActive());

        // 開閉 chevron（子がある行だけ。葉は領域だけ確保して名前の桁を揃える）
        if (hasChildren)
            ui::DrawIconCentered(dl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                                 ImVec2(rowX + st.FramePadding.x + fs * 0.5f, cy),
                                 ImGui::GetColorU32(hov || selected ? th::Text : th::TextDim), ui::Px(13.0f));
        // 種別アイコン + 名前
        const float tx = rowX + fs + st.FramePadding.x * 2.0f;
        const EntityGlyph g = PickEntityGlyph(reg, e, hasChildren);
        ui::DrawIconCentered(dl, g.glyph, ImVec2(tx + ui::Px(8.0f), cy), ImGui::GetColorU32(*g.tint), ui::Px(16.0f));
        dl->AddText(ImVec2(tx + ui::Px(22.0f), std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f)),
                    ImGui::GetColorU32(th::Text), tag.name.c_str());
    }

    // 折りたたんだ親は「中に何個いるか」を行の右端に出す（畳んだままでも規模が分かる）。
    // SpanFullWidth なので SameLine は行外へ飛ぶ。DrawList で右寄せに直接描く（数字は等幅）。
    if (hasChildren && !open)
    {
        char cntBuf[16];
        snprintf(cntBuf, sizeof(cntBuf), "%zu", children.size());
        ui::PushMono();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const ImVec2 ts = ImGui::CalcTextSize(cntBuf);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(mx.x - ts.x - ui::Px(10.0f), std::floor((mn.y + mx.y) * 0.5f - ts.y * 0.5f + 0.5f)),
            ImGui::GetColorU32(dx12e::theme::TextFaint), cntBuf);
        ui::PopMono();
    }

    bool itemHov = ImGui::IsItemHovered();

    // ダブルクリックでリネーム開始
    if (itemHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        StartRename(e, tag.name);
    // シングルクリック選択（Ctrl=トグル / Shift=範囲）— ダブルクリック時は選択処理をスキップ
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        const ImGuiIO& io = ImGui::GetIO();
        // 複数選択したうちの1行を「掴んだ」だけの時点で単独選択に潰さない。
        // IsItemClicked は押した瞬間に true なので、ここで Select すると
        // ドラッグ開始前に選択が1件になり、まとめてのドラッグ移動ができなくなる。
        // 離した時にドラッグしていなければ単独選択へ確定する（エクスプローラと同じ規律）。
        if (!io.KeyCtrl && !io.KeyShift && ctx.IsSelected(e) && ctx.selectedEntities.size() > 1)
            m_clickPendingEntity = e;
        else
            HandleRowClick(ctx, e);
    }

    // 掴んだだけで終わった（ドラッグしなかった）場合の選択確定
    if (m_clickPendingEntity == e && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        // GetDragDropPayload() != null が「今ドラッグ中」の公開 API 版（IsDragDropActive は内部）
        if (ImGui::GetDragDropPayload() == nullptr && ImGui::IsItemHovered())
            HandleRowClick(ctx, e);
        m_clickPendingEntity = entt::null;
    }

    // D&D ソース（親子設定用）
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

    // D&D ターゲット（ドロップされたら子にする）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            const entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            // 掴んだ行が選択に含まれていれば選択ぜんぶを一度に移す（1個ずつ引っ張らなくていい）。
            std::vector<entt::entity> moving;
            if (ctx.IsSelected(droppedEntity))
                moving = ctx.selectedEntities;
            else
                moving.push_back(droppedEntity);

            // 祖先も一緒に掴んでいる子は除く（親ごと動くので、ここで付け替えると入れ子が壊れる）
            auto ancestorAlsoMoving = [&](entt::entity d) {
                const auto* t = reg.try_get<Transform>(d);
                entt::entity cur = t ? t->parent : entt::null;
                for (int depth = 0; cur != entt::null && reg.valid(cur) && depth < 4096; ++depth)
                {
                    if (std::find(moving.begin(), moving.end(), cur) != moving.end()) return true;
                    const auto* pt = reg.try_get<Transform>(cur);
                    cur = pt ? pt->parent : entt::null;
                }
                return false;
            };

            // ★ワールド位置（位置・向き・大きさ）を保ったまま親を替える（UE / Unity と同じ既定）。
            //   parent だけ差し替えるとローカル値に新しい親の変換が掛かり直って見た目が飛ぶ。
            //   Shift を押しながら落とすと従来どおり「ローカル値そのまま」で付け替える。
            const bool keepWorld = !ImGui::GetIO().KeyShift;
            auto composite = std::make_unique<CompositeCommand>("Reparent");
            for (entt::entity d : moving)
            {
                if (d == e || !reg.valid(d) || !reg.all_of<Transform>(d)) continue;
                if (ancestorAlsoMoving(d)) continue;
                // 循環（e が d の子孫）は Reparent が拒む（壊れた祖先鎖でも止まる深さ上限つき）。
                Transform before = reg.get<Transform>(d);
                if (!xform::Reparent(reg, d, e, keepWorld)) continue;
                composite->Add(std::make_unique<TransformCommand>(&reg, d, before, reg.get<Transform>(d)));
            }
            if (!composite->Empty())
                ctx.undoSystem.PushCommand(std::move(composite));
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
        if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe5\xa4\x89\xe6\x9b\xb4", cmd::ShortcutText("edit.rename")))  // 名前変更
            StartRename(e, tag.name);
        if (ImGui::MenuItem("選択にフォーカス", cmd::ShortcutText("edit.focus")))
        {
            if (!ctx.IsSelected(e)) ctx.Select(e);
            ctx.pendingFocusSelection = true;
        }
        if (ImGui::MenuItem("\xe8\xa4\x87\xe8\xa3\xbd"))  // 複製
        {
            ctx.pendingDuplications.push_back(e);
        }
        // ★Play 中は無効。消化側（ApplicationRender）が Editor モードでしか回らないので、
        //   押せてしまうと要求が誰にも読まれず、ログにも出ないまま消える。
        if (ImGui::MenuItem("プレハブにする", nullptr, false, !ctx.isPlaying))
        {
            ctx.pendingCreatePrefab = e;
        }
        // 選択をまとめる空の親を作る。親は原点・無回転なので見た目は一切動かない。
        if (ImGui::MenuItem("選択をグループ化", "Ctrl+G", false, ctx.HasSelection()))
        {
            if (!ctx.IsSelected(e)) ctx.Select(e);   // 右クリックした行だけの場合
            ctx.pendingGroupSelection = true;
        }
        if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4"))  // 削除
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

        // 親子解除
        if (reg.all_of<Transform>(e) && reg.get<Transform>(e).parent != entt::null)
        {
            if (ImGui::MenuItem("\xe8\xa6\xaa\xe3\x81\x8b\xe3\x82\x89\xe5\xa4\x96\xe3\x81\x99"))  // 親から外す
            {
                // ワールド位置は保つ（外した瞬間に物が飛ばない）
                Transform before = reg.get<Transform>(e);
                if (xform::Reparent(reg, e, entt::null, /*keepWorld=*/true))
                    ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                        &reg, e, before, reg.get<Transform>(e)));
            }
        }

        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // 子は Render() の行リストが続けて描く（ここでは再帰しない）。
    ImGui::PopID();
}

void HierarchyPanel::Render(entt::registry& reg, EditorContext& ctx)
{
    ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc");  // Hierarchy

    auto nameView = reg.view<const NameTag>();

    // 生成直後の名前入力要求（グループ化など）。作った親は開いた状態にして中身を見せる。
    if (ctx.requestRenameEntity != entt::null)
    {
        if (reg.valid(ctx.requestRenameEntity) && reg.all_of<NameTag>(ctx.requestRenameEntity))
        {
            StartRename(ctx.requestRenameEntity, reg.get<NameTag>(ctx.requestRenameEntity).name);
            m_openNodes.insert(ctx.requestRenameEntity);
            m_scrollToEntity = ctx.requestRenameEntity;   // 画面外なら見える位置へ（F2 で行が見えない事故を避ける）
        }
        ctx.requestRenameEntity = entt::null;
    }

    // 親→子の索引をこのフレームぶん作り直す（1パス）。DrawEntityNode はここだけ見る。
    m_childIndex.clear();
    for (auto [e, t] : reg.view<const Transform>().each())
        if (t.parent != entt::null)
            m_childIndex[t.parent].push_back(e);
    // entt のプール順は生成順とは限らない（グループ化直後は逆順に並んで見えた）。
    // id 昇順＝おおむね生成順にそろえて、ルート行の並びと感覚を合わせる。
    for (auto& kv : m_childIndex)
        std::sort(kv.second.begin(), kv.second.end());

    // 選択が外から変わった（3Dビューでクリック / MCP / F フォーカス）なら、その行を露出する。
    // 祖先グループを開き、次の描画でスクロールして見せる＝畳んだ階層の中でも迷子にならない。
    if (ctx.selectedEntity != m_lastRevealed)
    {
        m_lastRevealed = ctx.selectedEntity;
        if (reg.valid(ctx.selectedEntity) && reg.all_of<Transform>(ctx.selectedEntity))
        {
            entt::entity p = reg.get<Transform>(ctx.selectedEntity).parent;
            for (int d = 0; p != entt::null && reg.valid(p) && d < 4096; ++d)
            {
                m_openNodes.insert(p);
                const auto* pt = reg.try_get<Transform>(p);
                p = pt ? pt->parent : entt::null;
            }
            m_scrollToEntity = ctx.selectedEntity;
        }
    }

    // オブジェクト数（GridPlane は内部用なので除外）
    size_t objCount = 0;
    for (auto [e, tag] : nameView.each())
        if (!reg.all_of<GridPlane>(e)) ++objCount;

    // ---- ヘッダ（件数 + 全展開/全折りたたみ + 検索）----
    // オブジェクトが増えると縦に膨れて目的の行が探せなくなるので、
    // 「一発で全部畳む」と「名前で絞る」を常に手元に置く。
    {
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextFaint);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%zu 個のオブジェクト", objCount);
        ImGui::PopStyleColor();

        // 右端に折りたたむ/展開のアイコンボタン
        const float btn = ui::Px(22.0f);   // DPI
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btn * 2.0f - ui::Px(2.0f));
        if (ui::IconButton("collapseAll", ICON_FOLD_ALL, "すべて折りたたむ", false, nullptr, btn, ui::Px(15.0f)))
            m_openNodes.clear();
        ImGui::SameLine(0.0f, ui::Px(2.0f));
        if (ui::IconButton("expandAll", ICON_UNFOLD_ALL, "すべて展開", false, nullptr, btn, ui::Px(15.0f)))
            for (const auto& kv : m_childIndex) m_openNodes.insert(kv.first);

        ImGui::SetNextItemWidth(-1.0f);
        ui::SearchField("##HierFilter", m_filterBuf, sizeof(m_filterBuf), "名前で検索");
    }
    ImGui::Spacing();

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
    if (m_filterBuf[0] != '\0')
    {
        // フィルタ中はツリーを畳んで、名前一致のフラットリストを表示（こちらも clipper で間引く）。
        // 行は通常のツリー行と同じ DrawEntityNode（flat）で描く＝右クリックメニュー / D&D（親子付け替え）/
        // 改名(F2・ダブルクリック) / 複数選択が、フィルタ中も通常時と同じに効く。
        m_rows.clear();
        for (auto [e, tag] : nameView.each())
        {
            if (reg.all_of<GridPlane>(e)) continue;
            if (!ContainsCI(tag.name, m_filterBuf)) continue;
            m_rows.push_back({e, 0});
        }
        ImGuiListClipper fclip;
        fclip.Begin(static_cast<int>(m_rows.size()));
        while (fclip.Step())
        for (int fi = fclip.DisplayStart; fi < fclip.DisplayEnd; ++fi)
        {
            zebra(fi);
            DrawEntityNode(reg, ctx, m_rows[static_cast<size_t>(fi)].e, /*flat=*/true);
        }
    }
    else
    {
        // ツリーを「見えている行」だけの平坦なリストに畳んでから ImGuiListClipper で
        // 画面外を丸ごと省く。ImGui へ全ノードを積むと 5万体で ~7ms 溶ける（実測）。
        m_rows.clear();
        std::vector<Row> stack;
        for (auto [e, tag] : nameView.each())
        {
            if (reg.all_of<GridPlane>(e)) continue;
            if (const auto* t = reg.try_get<Transform>(e);
                t && t->parent != entt::null && reg.valid(t->parent))
                continue;   // ルートのみ
            stack.push_back({e, 0});
        }
        // 明示スタックの DFS（深い階層でも再帰爆発しない）。開いてる親の子だけ展開。
        while (!stack.empty())
        {
            Row r = stack.back();
            stack.pop_back();
            m_rows.push_back(r);
            if (!m_openNodes.count(r.e)) continue;
            auto it = m_childIndex.find(r.e);
            if (it == m_childIndex.end()) continue;
            for (auto cit = it->second.rbegin(); cit != it->second.rend(); ++cit)
                stack.push_back({*cit, r.depth + 1});
        }

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
                const Row& r = m_rows[static_cast<size_t>(i)];
                zebra(i);
                if (r.depth > 0) ImGui::Indent(indentW * r.depth);
                indentGuides(ImGui::GetCursorScreenPos(), r.depth, indentW);
                if (r.e == m_scrollToEntity)
                {
                    ImGui::SetScrollHereY(0.5f);
                    m_scrollToEntity = entt::null;
                }
                DrawEntityNode(reg, ctx, r.e);
                if (r.depth > 0) ImGui::Unindent(indentW * r.depth);
            }
        }
    }

    ImGui::PopStyleVar(2);     // ItemSpacing / FramePadding
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

    // ---- 空白部分（行の下の残り全部）----
    // 以前の「空白への D&D」は直前の行を的にしていて、空白に落としても効かず、最後の行に落とすと
    // 「子にする」と「親を外す」が同じフレームで両方走りかねなかった。残りの領域を 1 つの項目にして的にする。
    //   ・右クリック: エンティティ作成メニュー（作成位置はエディタカメラの前 / 床との交点）
    //   ・ドロップ: 親を外してルートへ（ワールド位置は保つ。Shift でローカル値そのまま）
    //   ・左クリック: 選択解除
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
                    ctx.undoSystem.PushCommand(std::move(composite));
            }
            ImGui::EndDragDropTarget();
        }

        ui::PushMenuStyle();
        if (ImGui::BeginPopupContextItem("##HierBgCtx", ImGuiPopupFlags_MouseButtonRight))
        {
            ImGui::TextDisabled("エンティティを作成");
            ImGui::Separator();
            cmd::DrawCreateMenu(ctx);
            if (!ctx.clipboard.empty())
            {
                ImGui::Separator();
                if (ImGui::MenuItem("貼り付け", cmd::ShortcutText("edit.paste")))
                    ctx.pendingPastes = ctx.clipboard;
            }
            ImGui::EndPopup();
        }
        ui::PopMenuStyle();
    }

    ImGui::End();
}

} // namespace dx12e
