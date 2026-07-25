#include "editor/panels/HierarchyPanel.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <cctype>
#include <Windows.h>

namespace dx12e
{

// エンティティの構成コンポーネントから代表アイコンを選ぶ（Hierarchy のノード頭に表示）
static u64 PickEntityIcon(entt::registry& reg, entt::entity e, const EditorUiIcons& ic)
{
    if (reg.all_of<CameraComponent>(e))                            return ic.entCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e))   return ic.entLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton>(e)) return ic.entUi;
    if (reg.all_of<MeshRenderer>(e))                              return ic.entMesh;
    if (reg.all_of<AudioSource>(e))                              return ic.entAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))      return ic.entPhysics;
    if (reg.all_of<LuaScript>(e))                                 return ic.entScript;
    return ic.entEmpty;
}

// 種別ごとの tint（Nebula のカラフルなアイコンを単色 PNG に着色して再現）
static ImVec4 PickEntityTint(entt::registry& reg, entt::entity e)
{
    using namespace dx12e::theme;
    if (reg.all_of<CameraComponent>(e))                            return TypeCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e))   return TypeLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton>(e)) return TypeUi;
    if (reg.all_of<MeshRenderer>(e))                              return TypeMesh;
    if (reg.all_of<AudioSource>(e))                              return TypeAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))      return TypePhysics;
    if (reg.all_of<LuaScript>(e))                                 return TypeScript;
    return TypeEmpty;
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

void HierarchyPanel::DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    if (!reg.all_of<NameTag>(e)) return;
    auto& tag = reg.get<NameTag>(e);

    // ID スコープを明示分離 (ImGui 1.92 の TreeNode + D&D + popup 衝突対策)
    ImGui::PushID(static_cast<int>(static_cast<u32>(e)));

    auto itKids = m_childIndex.find(e);
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
        bool entered = ImGui::InputText("##Rename", m_renameBuf, sizeof(m_renameBuf),
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
                NameTag before = tag;
                tag.name = m_renameBuf;
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<NameTag>>(
                    &reg, e, before, tag, "Rename"));
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
                    NameTag before = tag;
                    tag.name = m_renameBuf;
                    ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<NameTag>>(
                        &reg, e, before, tag, "Rename"));
                }
                m_renamingEntity = entt::null;
            }
        }

        ImGui::PopID();
        return;  // リネーム中はツリーノード描画しない
    }

    // NoTreePushOnOpen: 子はこの関数から再帰せず、Render() の平坦化行リスト側で描く
    //（ListClipper で画面外の行を丸ごと省くため）。よって TreePop も不要。
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool wasOpen = hasChildren && m_openNodes.count(e) != 0;

    // 種別アイコンをノード頭に表示（クリック判定は直後の TreeNodeEx が担う）
    // 単色 PNG を種別カラーで tint して Nebula のカラフルなアイコンを再現。
    if (ctx.icons)
    {
        u64 typeIcon = PickEntityIcon(reg, e, *ctx.icons);
        if (typeIcon)
        {
            float h = ImGui::GetTextLineHeight();
            ImGui::ImageWithBg(static_cast<ImTextureID>(typeIcon), ImVec2(h, h),
                ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                PickEntityTint(reg, e));
            ImGui::SameLine(0.0f, 6.0f);
        }
    }

    // 開閉状態の指定は「TreeNodeEx の直前」でないと効かない。SetNextItemOpen が積む
    // NextItemData は次に出す 1 アイテムで消費されるので、上のアイコン画像が先に食べてしまい
    // 全展開/全折りたたみや「選択行を露出」が一切効かなくなっていた（ImGui 内部の
    // 保存状態だけが真になり、m_openNodes は見た目に反映されない鏡だった）。
    ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);

    // ID は PushID で一意化済みなので TreeNodeEx は固定文字列でOK
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", tag.name.c_str());
    if (hasChildren && open != wasOpen)
    {
        if (open) m_openNodes.insert(e);
        else      m_openNodes.erase(e);
    }

    // 折りたたんだ親は「中に何個いるか」を行の右端に出す（畳んだままでも規模が分かる）。
    // SpanAvailWidth なので SameLine は行外へ飛ぶ。DrawList で右寄せに直接描く。
    if (hasChildren && !open)
    {
        char cntBuf[16];
        snprintf(cntBuf, sizeof(cntBuf), "%zu", children.size());
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const ImVec2 ts = ImGui::CalcTextSize(cntBuf);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(mx.x - ts.x - 8.0f, mn.y),
            ImGui::GetColorU32(dx12e::theme::TextFaint), cntBuf);
    }

    bool itemHov = ImGui::IsItemHovered();

    // ダブルクリックでリネーム開始
    if (itemHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_renamingEntity = e;
        m_renameWarmup = 3;  // 3フレーム分フォーカス安定を待つ
        std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
        strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
    }
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

            auto composite = std::make_unique<CompositeCommand>("Reparent");
            for (entt::entity d : moving)
            {
                if (d == e || !reg.valid(d) || !reg.all_of<Transform>(d)) continue;
                if (ancestorAlsoMoving(d)) continue;
                // 循環防止: e が d の子孫でないかチェック。
                // 祖先鎖が既にサイクル化した壊れデータでも無限ループしないよう深さ上限付き。
                bool isCyclic = false;
                entt::entity check = e;
                int depth = 0;
                while (check != entt::null && reg.valid(check) && reg.all_of<Transform>(check))
                {
                    if (check == d || ++depth >= 4096) { isCyclic = true; break; }
                    check = reg.get<Transform>(check).parent;
                }
                if (isCyclic) continue;

                auto& dt = reg.get<Transform>(d);
                Transform before = dt;
                dt.parent = e;
                composite->Add(std::make_unique<TransformCommand>(&reg, d, before, dt));
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
    if (ImGui::BeginPopupContextItem("EntityCtx", ImGuiPopupFlags_MouseButtonRight))
    {
        if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe5\xa4\x89\xe6\x9b\xb4"))  // 名前変更
        {
            m_renamingEntity = e;
            m_renameWarmup = 3;  // 3フレーム分フォーカス安定を待つ
            std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
            strncpy_s(m_renameBuf, tag.name.c_str(), _TRUNCATE);
        }
        if (ImGui::MenuItem("\xe8\xa4\x87\xe8\xa3\xbd"))  // 複製
        {
            ctx.pendingDuplications.push_back(e);
        }
        if (ImGui::MenuItem("プレハブにする"))  // Prefab 化（assets/prefabs へ保存）
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
                auto& t = reg.get<Transform>(e);
                Transform before = t;
                t.parent = entt::null;
                ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                    &reg, e, before, t));
            }
        }

        ImGui::EndPopup();
    }

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
            m_renamingEntity = ctx.requestRenameEntity;
            m_renameWarmup   = 3;
            m_openNodes.insert(ctx.requestRenameEntity);
            std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
            strncpy_s(m_renameBuf, reg.get<NameTag>(ctx.requestRenameEntity).name.c_str(), _TRUNCATE);
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

    // ---- ヘッダ（件数 + 全展開/全折りたたみ + フィルタ）----
    // オブジェクトが増えると縦に膨れて目的の行が探せなくなるので、
    // 「一発で全部畳む」と「名前で絞る」を常に手元に置く。
    static char s_filterBuf[64] = {};
    {
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextFaint);
        ImGui::Text("%zu objects", objCount);
        ImGui::PopStyleColor();

        // 右端に畳む/展開ボタン（日本語フォントしか無いので記号ではなく漢字ラベル）
        const float btnW = ImGui::CalcTextSize("閉").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::SmallButton("閉"))
            m_openNodes.clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("全て折りたたむ");
        ImGui::SameLine();
        if (ImGui::SmallButton("開"))
            for (const auto& kv : m_childIndex) m_openNodes.insert(kv.first);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("全て展開");

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##HierFilter", "Filter", s_filterBuf, sizeof(s_filterBuf));
    }
    ImGui::Spacing();

    // ---- 行の見やすさ（縞模様 + 行間 + 階層ガイド線）----
    // 行間を少し広げる: 詰まった行は名前が塊に見えて目的の行を探しにくい。
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(ImGui::GetStyle().ItemSpacing.x, 5.0f));
    // ツリーの矢印スペースは FontSize + FramePadding.x*2。グローバル値(8)のままだと
    // 種別アイコンと名前の間に 30px 以上の空白ができて視線が飛ぶ。詰めて1つの行に見せる。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 2.0f));
    // 選択行をはっきり見せる（既定の淡いヘッダ色だと縞模様に埋もれる）
    ImGui::PushStyleColor(ImGuiCol_Header,        dx12e::theme::AccentDim2);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, dx12e::theme::AccentDim);

    const float rowH = ImGui::GetTextLineHeightWithSpacing();

    // 行の縞模様。長いリストで「どの名前がどの行か」を目で追えるようにする。
    // 行を描く直前に呼び、窓幅いっぱいの帯を1本敷く。
    auto zebra = [rowH](int index)
    {
        if ((index & 1) == 0) return;
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(wp.x, p.y - 2.0f),
            ImVec2(wp.x + ImGui::GetWindowSize().x, p.y + rowH - 3.0f),
            IM_COL32(255, 255, 255, 12));
    };

    // 階層ガイド線。どの行がどの親の下にいるのか、インデント量を数えずに追えるようにする。
    // rowStart = インデント適用後の行頭スクリーン座標。
    auto indentGuides = [rowH](ImVec2 rowStart, int depth, float indentW)
    {
        if (depth <= 0) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = IM_COL32(255, 255, 255, 26);
        for (int j = 1; j <= depth; ++j)
        {
            const float x = rowStart.x - indentW * static_cast<float>(j) + 7.0f;
            dl->AddLine(ImVec2(x, rowStart.y - 2.0f), ImVec2(x, rowStart.y + rowH - 3.0f), col);
        }
        // 一番内側だけ横に伸ばして「この親の子」と分かるようにする
        const float xIn = rowStart.x - indentW + 7.0f;
        const float yMid = rowStart.y + rowH * 0.4f;
        dl->AddLine(ImVec2(xIn, yMid), ImVec2(rowStart.x - 3.0f, yMid), col);
    };

    if (s_filterBuf[0] != '\0')
    {
        // フィルタ中はツリーを畳んで、名前一致のフラットリストを表示（こちらも clipper で間引く）
        m_rows.clear();
        for (auto [e, tag] : nameView.each())
        {
            if (reg.all_of<GridPlane>(e)) continue;
            if (!ContainsCI(tag.name, s_filterBuf)) continue;
            m_rows.push_back({e, 0});
        }
        ImGuiListClipper fclip;
        fclip.Begin(static_cast<int>(m_rows.size()));
        while (fclip.Step())
        for (int fi = fclip.DisplayStart; fi < fclip.DisplayEnd; ++fi)
        {
            const entt::entity e = m_rows[static_cast<size_t>(fi)].e;
            const auto& tag = reg.get<NameTag>(e);

            zebra(fi);
            ImGui::PushID(static_cast<int>(static_cast<u32>(e)));
            if (ctx.icons)
            {
                u64 ico = PickEntityIcon(reg, e, *ctx.icons);
                if (ico)
                {
                    float h = ImGui::GetTextLineHeight();
                    ImGui::ImageWithBg(static_cast<ImTextureID>(ico), ImVec2(h, h),
                        ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                        PickEntityTint(reg, e));
                    ImGui::SameLine(0.0f, 6.0f);
                }
            }
            if (ImGui::Selectable(tag.name.c_str(), ctx.IsSelected(e)))
                HandleRowClick(ctx, e);   // フィルタ中も Shift 範囲 / Ctrl トグルが効く
            ImGui::PopID();
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

    ImGui::PopStyleColor(2);   // Header / HeaderHovered
    ImGui::PopStyleVar(2);     // ItemSpacing / FramePadding

    // ヒエラルキーの空白部分への D&D（親子解除）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
        {
            const entt::entity droppedEntity = *static_cast<const entt::entity*>(payload->Data);
            // 行の上と同じく、掴んだ行が選択に含まれていれば選択ぜんぶをルートへ出す（グループ解除）。
            std::vector<entt::entity> moving;
            if (ctx.IsSelected(droppedEntity)) moving = ctx.selectedEntities;
            else                               moving.push_back(droppedEntity);

            auto composite = std::make_unique<CompositeCommand>("Unparent");
            for (entt::entity d : moving)
            {
                if (!reg.valid(d) || !reg.all_of<Transform>(d)) continue;
                auto& t = reg.get<Transform>(d);
                if (t.parent == entt::null) continue;
                Transform before = t;
                t.parent = entt::null;
                composite->Add(std::make_unique<TransformCommand>(&reg, d, before, t));
            }
            if (!composite->Empty())
                ctx.undoSystem.PushCommand(std::move(composite));
        }
        ImGui::EndDragDropTarget();
    }

    // ヒエラルキーにフォーカスがある状態で Del キー → 選択エンティティを削除。
    // 実際の削除は pendingDeletions 経由でフレーム境界に行われ、Undo も積まれる。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && m_renamingEntity == entt::null
        && ctx.HasSelection()
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        for (auto e : ctx.selectedEntities)
            if (reg.valid(e)) ctx.pendingDeletions.push_back(e);
        ctx.ClearSelection();
    }

    // Ctrl+G: 選択をグループ化（空の親にまとめる）。実処理は Application のフレーム境界。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && m_renamingEntity == entt::null
        && ctx.HasSelection()
        && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        ctx.pendingGroupSelection = true;
    }

    ImGui::Separator();

    // Add entity menu
    if (ImGui::Button("\xe2\x9c\x9a \xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe8\xbf\xbd\xe5\x8a\xa0"))
        ImGui::OpenPopup("AddEntityPopup");

    if (ImGui::BeginPopup("AddEntityPopup"))
    {
        if (ImGui::MenuItem("Box"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_box__";
            req.position = {0.0f, 0.5f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Sphere"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_sphere__";
            req.position = {0.0f, 0.5f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Plane"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__primitive_plane__";
            req.position = {0.0f, 0.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Empty"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__empty__";
            req.position = {0.0f, 0.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        // 地形（ハイトフィールド）。エンティティ生成そのものは解像度/サイズを決めてからなので、
        // ここでは地形ツール窓を開くだけ（窓の「＋ 地形を作成」で作る）。
        if (ImGui::MenuItem("Terrain（地形・山を作る）"))
        {
            ctx.showTerrainEditor = true;
        }
        // スカルプト（任意メッシュの頂点編集）。地形と同じく素体の分割数を決めてからなので、
        // ここではスカルプト窓を開くだけ（窓の「＋ 素体を作成」で作る）。
        if (ImGui::MenuItem("Sculpt（異形・洞窟・アーチ・岩）"))
        {
            ctx.showSculptEditor = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Camera"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__camera__";
            req.position = {0.0f, 2.0f, -5.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Directional Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__directional_light__";
            req.position = {0.0f, 5.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Point Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__point_light__";
            req.position = {0.0f, 3.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Spot Light"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__spot_light__";
            req.position = {0.0f, 5.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Gimmick（ステージ部品）"))
        {
            auto spawnGimmick = [&](const char* marker, float y)
            {
                PendingSpawnRequest req;
                req.modelPath = marker;
                req.position = {0.0f, y, 0.0f};
                ctx.pendingSpawns.push_back(req);
            };
            if (ImGui::MenuItem("Spike Pulse（上下するトゲ）")) spawnGimmick("__gimmick_spike__", 0.7f);
            if (ImGui::MenuItem("Slide Wall（左右に動く壁）")) spawnGimmick("__gimmick_slide__", 0.75f);
            if (ImGui::MenuItem("Static Wall（動かない壁）"))   spawnGimmick("__gimmick_wall__", 0.7f);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Particle Emitter（配置エフェクト）"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__particle_emitter__";
            req.position = {0.0f, 1.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        if (ImGui::MenuItem("Trigger（イベント範囲）"))
        {
            PendingSpawnRequest req;
            req.modelPath = "__trigger__";
            req.position = {0.0f, 1.0f, 0.0f};
            ctx.pendingSpawns.push_back(req);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("UI（ゲーム内UI）"))
        {
            // Image/Text/Button は Application 側で「選択エンティティが UI ツリー内なら
            // その子 → 無ければ最初の UICanvas の子 → Canvas 不在なら自動生成」に配置される
            auto spawnUi = [&](const char* marker)
            {
                PendingSpawnRequest req;
                req.modelPath = marker;
                req.position = {0.0f, 0.0f, 0.0f};
                ctx.pendingSpawns.push_back(req);
            };
            if (ImGui::MenuItem("Canvas（UIルート）"))       spawnUi("__ui_canvas__");
            if (ImGui::MenuItem("Image（画像/単色矩形）"))   spawnUi("__ui_image__");
            if (ImGui::MenuItem("Text（テキスト）"))         spawnUi("__ui_text__");
            if (ImGui::MenuItem("Button（ボタン）"))         spawnUi("__ui_button__");
            if (ImGui::MenuItem("Slider（スライダー）"))     spawnUi("__ui_slider__");
            if (ImGui::MenuItem("Toggle（トグル）"))         spawnUi("__ui_toggle__");
            if (ImGui::MenuItem("ScrollView（スクロール）")) spawnUi("__ui_scrollview__");
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace dx12e
