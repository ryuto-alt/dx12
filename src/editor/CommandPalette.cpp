#include "editor/CommandPalette.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/EntityGlyph.h"
#include "editor/FuzzyMatch.h"
#include "editor/ToolWindows.h"
#include "editor/UiWidgets.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "gui/VirtualInputImGui.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace dx12e
{

namespace
{
constexpr float kWidth  = 640.0f;
constexpr float kRowH   = 30.0f;
constexpr int   kMaxVisibleRows = 9;
constexpr size_t kMaxResults = 80;
constexpr size_t kMaxRecent  = 24;
constexpr size_t kMaxAssetIndex = 40000;

const char* kAssetBrowserWindow =
    "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6";   // アセットブラウザ

ImVec4 TintColor(int t)
{
    namespace th = theme;
    switch (t)
    {
    case 1: return th::TypeLight;
    case 2: return th::TypeCamera;
    case 3: return th::TypeScript;
    case 4: return th::TypeAudio;
    case 5: return th::TypeFolder;
    case 6: return th::TypeScene;
    case 7: return th::TypeUi;
    case 8: return th::TypePhysics;
    default: return th::TextDim;
    }
}

int TintOfEntityGlyph(const ImVec4* tint)
{
    namespace th = theme;
    if (tint == &th::TypeLight)   return 1;
    if (tint == &th::TypeCamera)  return 2;
    if (tint == &th::TypeScript)  return 3;
    if (tint == &th::TypeAudio)   return 4;
    if (tint == &th::TypeFolder)  return 5;
    if (tint == &th::TypeUi)      return 7;
    if (tint == &th::TypePhysics) return 8;
    return 0;
}

const char* AssetIcon(int type, int& tint)
{
    using T = AssetBrowserPanel::AssetType;
    tint = 0;
    switch (static_cast<T>(type))
    {
    case T::Folder:      tint = 5; return ICON_FOLDER;
    case T::Model:       return ICON_T_MESH;
    case T::Texture:     return ICON_IMAGE;
    case T::Scene:       tint = 6; return ICON_FILM;
    case T::Script:      tint = 3; return ICON_FILE_CODE;
    case T::Audio:       tint = 4; return ICON_MUSIC;
    case T::Prefab:      return ICON_T_PREFAB;
    case T::Shader:      tint = 3; return ICON_T_SHADER;
    case T::Material:    return ICON_T_MATERIAL;
    case T::UiAnim:      tint = 7; return ICON_T_ANIM;
    case T::SpriteSheet: tint = 7; return ICON_T_SPRITE;
    default:             return ICON_FILE;
    }
}

// label を一致箇所だけ色を変えて描く。maxW を超える分は切る（クリップ）。
void DrawHighlighted(ImDrawList* dl, ImVec2 pos, const std::string& text, const std::vector<uint32_t>& hl,
                     ImU32 base, ImU32 hi, float maxW)
{
    const char* p = text.c_str();
    const char* end = p + text.size();
    dl->PushClipRect(ImVec2(pos.x, pos.y - 2.0f), ImVec2(pos.x + maxW, pos.y + ImGui::GetTextLineHeight() + 2.0f), true);
    float x = pos.x;
    while (p < end)
    {
        const size_t off = static_cast<size_t>(p - text.c_str());
        const bool isHl = std::binary_search(hl.begin(), hl.end(), static_cast<uint32_t>(off));
        // 同じ色が続く間をまとめて 1 回で描く（UTF-8 のコードポイント境界で進める）
        const char* q = p;
        while (q < end)
        {
            const size_t o2 = static_cast<size_t>(q - text.c_str());
            if (std::binary_search(hl.begin(), hl.end(), static_cast<uint32_t>(o2)) != isHl) break;
            const unsigned char c = static_cast<unsigned char>(*q);
            q += (c < 0x80) ? 1 : ((c >> 5) == 0x6 ? 2 : ((c >> 4) == 0xE ? 3 : ((c >> 3) == 0x1E ? 4 : 1)));
            if (q > end) q = end;
        }
        dl->AddText(ImVec2(x, pos.y), isHl ? hi : base, p, q);
        x += ImGui::CalcTextSize(p, q).x;
        if (x > pos.x + maxW) break;
        p = q;
    }
    dl->PopClipRect();
}

std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

// ---------------------------------------------------------------------------

void CommandPalette::Touch(std::vector<std::string>& mru, const std::string& key)
{
    mru.erase(std::remove(mru.begin(), mru.end(), key), mru.end());
    mru.insert(mru.begin(), key);
    if (mru.size() > kMaxRecent) mru.resize(kMaxRecent);
}

int CommandPalette::RecentRank(const std::vector<std::string>& mru, const std::string& key) const
{
    for (size_t i = 0; i < mru.size(); ++i)
        if (mru[i] == key) return static_cast<int>(i);
    return -1;
}

void CommandPalette::Open(Mode mode, EditorContext& ctx)
{
    m_open = true;
    m_justOpened = true;
    m_mode = mode;
    m_buf[0] = '\0';
    m_lastQuery = "\x01";
    m_sel = 0;
    ctx.paletteOpen = true;
}

void CommandPalette::Close(EditorContext& ctx)
{
    m_open = false;
    ctx.paletteOpen = false;
}

// アセット索引（プロジェクトのアセット + スクリプト）。開いた時に作り、15 秒以内なら使い回す。
void CommandPalette::EnsureAssetIndex(AssetBrowserPanel* assets)
{
    if (!assets) return;
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = m_assetIndexBuilt && (now - m_assetIndexTime) < std::chrono::seconds(15)
        && m_assetRoots.size() == 2 && m_assetRoots[0] == assets->AssetsRoot() && m_assetRoots[1] == assets->ScriptsRoot();
    if (fresh) return;

    namespace fs = std::filesystem;
    m_assetIndex.clear();
    m_assetRoots = {assets->AssetsRoot(), assets->ScriptsRoot()};
    for (const fs::path& root : m_assetRoots)
    {
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && m_assetIndex.size() < kMaxAssetIndex; it.increment(ec))
        {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (!name.empty() && name[0] == '.')
            {
                if (it->is_directory(ec)) it.disable_recursion_pending();   // .thumbcache / .autosave は覗かない
                continue;
            }
            const bool isDir = it->is_directory(ec);
            AssetRec r;
            r.abs = it->path().string();
            r.name = name;
            std::string rel = fs::relative(it->path(), root, ec).generic_string();
            r.rel = (root == assets->ScriptsRoot() ? "scripts/" : "") + rel;
            std::string ext = LowerAscii(it->path().extension().string());
            r.type = static_cast<int>(isDir ? AssetBrowserPanel::AssetType::Folder
                                            : AssetBrowserPanel::ClassifyExtension(ext));
            r.isScene = !isDir && r.type == static_cast<int>(AssetBrowserPanel::AssetType::Scene);
            m_assetIndex.push_back(std::move(r));
        }
    }
    m_assetIndexBuilt = true;
    m_assetIndexTime = now;
}

void CommandPalette::BuildCommandItems(EditorContext& ctx, const std::string& q, std::vector<Item>& out)
{
    auto add = [&](const std::string& id, const std::string& label, const std::string& en,
                   const std::string& category, const std::string& chord)
    {
        Item it;
        it.kind = Item::Kind::Command;
        it.id = id;
        it.label = label;
        it.sub = category;
        it.chord = chord;
        it.icon = cmd::IconFor(id);
        it.enabled = cmd::IsEnabled(ctx, id);
        if (q.empty())
        {
            it.score = 0;
        }
        else
        {
            int best = fuzzy::Score(q, label, &it.hl);
            if (best >= 0) best += 30;                       // 表示ラベルの一致を優先
            std::vector<uint32_t> tmp;
            const int s2 = fuzzy::Score(q, en, &tmp);
            const int s3 = fuzzy::Score(q, category, &tmp) - 40;   // カテゴリ名だけの一致は弱く
            const int s3c = fuzzy::Score(q, category);
            if (s2 > best) { best = s2; it.hl.clear(); }
            if (s3c >= 0 && s3 > best) { best = s3; it.hl.clear(); }
            if (best < 0) return;
            it.score = best;
        }
        out.push_back(std::move(it));
    };

    for (const cmd::Def& d : cmd::kCommands)
    {
        // パレット自身の起動コマンドは出さない（開いているのに「パレットを開く」は無意味）
        if (std::string_view(d.id).rfind("palette.", 0) == 0) continue;
        add(d.id, d.label, d.labelEn, d.category, cmd::ChordLabel(d));
    }
    for (const tools::Desc& t : tools::kAll)
    {
        const std::string id = std::string("window.") + t.id;
        std::string label = std::string(t.title);
        add(id, label, t.keywords ? std::string("window ") + t.keywords : std::string("window"), "ウィンドウ", "");
    }
    for (const cmd::CreateItem& c : cmd::kCreateItems)
        add(c.id, std::string("作成: ") + c.label, std::string("create add spawn ") + c.labelEn, "作成", "");
}

void CommandPalette::BuildEntityItems(entt::registry& reg, const std::string& q, std::vector<Item>& out)
{
    if (q.empty()) return;   // 全エンティティを並べても探せない。名前を入れてもらう
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        if (reg.all_of<GridPlane>(e)) continue;
        std::vector<uint32_t> hl;
        const int s = fuzzy::Score(q, tag.name, &hl);
        if (s < 0) continue;
        Item it;
        it.kind = Item::Kind::Entity;
        it.entity = e;
        it.label = tag.name;
        it.score = s;
        it.hl = std::move(hl);
        const EntityGlyph g = PickEntityGlyph(reg, e, false);
        it.icon = g.glyph;
        it.tint = TintOfEntityGlyph(g.tint);
        // 補足: 親階層（近い親から 2 段）
        std::string path;
        entt::entity p = entt::null;
        if (const auto* t = reg.try_get<Transform>(e)) p = t->parent;
        for (int d = 0; d < 2 && p != entt::null && reg.valid(p); ++d)
        {
            if (const auto* pn = reg.try_get<NameTag>(p)) path = pn->name + (path.empty() ? "" : " / " + path);
            const auto* pt = reg.try_get<Transform>(p);
            p = pt ? pt->parent : entt::null;
        }
        it.sub = path.empty() ? "エンティティ" : path;
        out.push_back(std::move(it));
    }
}

void CommandPalette::BuildAssetItems(const std::string& q, std::vector<Item>& out)
{
    // 空欄のときは最近開いたアセット
    if (q.empty())
    {
        for (const std::string& abs : m_recentAssets)
        {
            for (const AssetRec& r : m_assetIndex)
            {
                if (r.abs != abs) continue;
                Item it;
                it.kind = Item::Kind::Asset;
                it.id = r.abs;
                it.label = r.name;
                it.sub = r.rel;
                int tint = 0;
                it.icon = AssetIcon(r.type, tint);
                it.tint = tint;
                out.push_back(std::move(it));
                break;
            }
        }
        return;
    }
    for (const AssetRec& r : m_assetIndex)
    {
        std::vector<uint32_t> hl;
        int s = fuzzy::Score(q, r.name, &hl);
        if (s >= 0) s += 20;   // ファイル名の一致はパスの一致より上
        else
        {
            s = fuzzy::Score(q, r.rel);
            if (s < 0) continue;
            hl.clear();
        }
        Item it;
        it.kind = Item::Kind::Asset;
        it.id = r.abs;
        it.label = r.name;
        it.sub = r.rel;
        it.score = s;
        it.hl = std::move(hl);
        int tint = 0;
        it.icon = AssetIcon(r.type, tint);
        it.tint = tint;
        out.push_back(std::move(it));
    }
}

void CommandPalette::Rebuild(EditorContext& ctx, entt::registry& reg)
{
    // 先頭の記号で対象を切り替える
    std::string raw = m_buf;
    Source src = (m_mode == Mode::Commands) ? Source::Commands : Source::All;
    if (!raw.empty() && (raw[0] == '>' || raw[0] == '@' || raw[0] == '#'))
    {
        src = raw[0] == '>' ? Source::Commands : (raw[0] == '@' ? Source::Entities : Source::Assets);
        raw.erase(0, 1);
    }
    // 先頭の空白は無視
    while (!raw.empty() && (raw[0] == ' ' || raw[0] == '\t')) raw.erase(0, 1);

    m_items.clear();
    if (src == Source::Commands) BuildCommandItems(ctx, raw, m_items);
    if (src == Source::Entities || src == Source::All) BuildEntityItems(reg, raw, m_items);
    if (src == Source::Assets || src == Source::All) BuildAssetItems(raw, m_items);

    // 並べ替え: 有効 > スコア > 最近使った順 > 表の順（stable_sort で最後を保つ）
    auto rank = [&](const Item& it) -> int
    {
        if (it.kind == Item::Kind::Command) return RecentRank(m_recentCommands, it.id);
        if (it.kind == Item::Kind::Asset)   return RecentRank(m_recentAssets, it.id);
        return -1;
    };
    if (raw.empty())
    {
        // 空欄: 最近使ったコマンドを先頭に、あとは表の順
        std::stable_sort(m_items.begin(), m_items.end(), [&](const Item& a, const Item& b)
        {
            const int ra = rank(a), rb = rank(b);
            const bool ha = ra >= 0, hb = rb >= 0;
            if (ha != hb) return ha;
            if (ha && hb) return ra < rb;
            return false;
        });
    }
    else
    {
        std::stable_sort(m_items.begin(), m_items.end(), [&](const Item& a, const Item& b)
        {
            if (a.enabled != b.enabled) return a.enabled;
            if (a.score != b.score) return a.score > b.score;
            const int ra = rank(a), rb = rank(b);
            if ((ra >= 0) != (rb >= 0)) return ra >= 0;
            if (ra >= 0 && rb >= 0) return ra < rb;
            return false;
        });
    }
    if (m_items.size() > kMaxResults) m_items.resize(kMaxResults);
    m_sel = 0;
    m_scrollToSel = true;
    m_lastSource = src;
}

void CommandPalette::Activate(EditorContext& ctx, const cmd::Env& env, entt::registry& reg,
                              AssetBrowserPanel* assets, const Item& it)
{
    switch (it.kind)
    {
    case Item::Kind::Command:
    {
        if (!it.enabled)
        {
            ctx.Notify(ui::ToastKind::Warn, "今は実行できません: " + it.label);
            return;
        }
        Touch(m_recentCommands, it.id);
        Close(ctx);
        ImGui::CloseCurrentPopup();
        cmd::Execute(ctx, env, it.id);
        return;
    }
    case Item::Kind::Entity:
    {
        Close(ctx);
        ImGui::CloseCurrentPopup();
        if (reg.valid(it.entity))
        {
            ctx.Select(it.entity);
            ctx.pendingFocusSelection = true;   // Application が選択へカメラを寄せる
        }
        return;
    }
    case Item::Kind::Asset:
    {
        Touch(m_recentAssets, it.id);
        Close(ctx);
        ImGui::CloseCurrentPopup();
        namespace fs = std::filesystem;
        const fs::path p(it.id);
        std::string ext = LowerAscii(p.extension().string());
        const bool isScene = AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Scene
                          && !fs::is_directory(p);
        if (isScene)
            ctx.pendingLoadPath = p.string();   // メニュー「シーンを開く」と同じ（未保存なら確認が出る）
        else if (assets)
        {
            assets->RevealAsset(p);
            ImGui::SetWindowFocus(kAssetBrowserWindow);   // アセットブラウザのタブを前面へ
        }
        return;
    }
    }
}

void CommandPalette::Render(EditorContext& ctx, const cmd::Env& env, entt::registry& reg, AssetBrowserPanel* assets)
{
    namespace th = theme;

    // ---- 開く要求 ----
    if (ctx.paletteRequest != 0)
    {
        const Mode want = ctx.paletteRequest == 1 ? Mode::Commands : Mode::QuickOpen;
        ctx.paletteRequest = 0;
        if (m_open && m_mode == want)
            Close(ctx);          // 同じキーをもう一度 = 閉じる
        else
        {
            const bool wasOpen = m_open;
            Open(want, ctx);
            (void)wasOpen;
            if (want == Mode::QuickOpen) EnsureAssetIndex(assets);
        }
    }
    if (!m_open) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // 開いたフレームは、開いた原因のキー（Ctrl+K など）が同じフレームのままここからも見える。
    // その同じ押下でトグル判定して即座に閉じないよう、開いたフレームはキー操作を受け付けない。
    const bool openedThisFrame = m_justOpened;
    if (m_justOpened) ImGui::OpenPopup("##CommandPalette");

    // クエリが変わったら作り直す
    {
        const std::string cur = m_buf;
        if (cur != m_lastQuery)
        {
            m_lastQuery = cur;
            // 記号でアセット検索へ入ったら索引を用意
            if (!cur.empty() && cur[0] == '#') EnsureAssetIndex(assets);
            Rebuild(ctx, reg);
        }
    }

    const int rows = static_cast<int>((std::min)(m_items.size(), static_cast<size_t>(kMaxVisibleRows)));
    const float listH = (m_items.empty() ? 1 : rows) * kRowH;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 84.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, th::Bg2);
    ImGui::PushStyleColor(ImGuiCol_Border, th::BorderStrong);

    bool closeRequested = false;
    if (ImGui::BeginPopupModal("##CommandPalette", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar))
    {
        m_justOpened = false;
        ImGuiIO& io = ImGui::GetIO();

        // ---- 入力欄 ----
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
            ImGui::SetKeyboardFocusHere();
        const char* hint = (m_mode == Mode::Commands)
            ? "コマンドを検索（> コマンド  @ エンティティ  # アセット）"
            : "エンティティ / アセットを検索（> コマンド  @ エンティティ  # アセット）";
        ui::InputTextWithHint("##palq", hint, m_buf, sizeof(m_buf));
        vinput_gui::AnchorLastItem("palette", "検索");
        ImGui::PopStyleVar();

        // ---- 上下キー / Enter / Esc ----
        const int n = static_cast<int>(m_items.size());
        auto move = [&](int d)
        {
            if (n <= 0) return;
            m_sel = ((m_sel + d) % n + n) % n;
            m_scrollToSel = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) move(+1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))   move(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))  { if (n > 0) { m_sel = (std::min)(n - 1, m_sel + kMaxVisibleRows); m_scrollToSel = true; } }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))    { if (n > 0) { m_sel = (std::max)(0, m_sel - kMaxVisibleRows); m_scrollToSel = true; } }
        bool activate = (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) closeRequested = true;

        // Ctrl+K / Ctrl+P で開いたまま切り替え・閉じる
        if (!openedThisFrame && io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_K, false))
            {
                if (m_mode == Mode::Commands) closeRequested = true;
                else { m_mode = Mode::Commands; m_lastQuery = "\x01"; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_P, false))
            {
                if (m_mode == Mode::QuickOpen) closeRequested = true;
                else { m_mode = Mode::QuickOpen; m_lastQuery = "\x01"; EnsureAssetIndex(assets); }
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // ---- 結果リスト ----
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##palList", ImVec2(0.0f, listH), false, ImGuiWindowFlags_NoNav);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (m_items.empty())
            {
                const bool needQuery = std::string(m_buf).empty() ||
                    (m_buf[0] != '\0' && m_buf[1] == '\0' && (m_buf[0] == '>' || m_buf[0] == '@' || m_buf[0] == '#'));
                ImGui::SetCursorPosY(6.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, th::TextFaint);
                ImGui::TextUnformatted(needQuery
                    ? "名前を入力すると検索します（最近使った項目はここに出ます）"
                    : "一致する項目がありません");
                ImGui::PopStyleColor();
            }
            for (int i = 0; i < n; ++i)
            {
                const Item& it = m_items[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
                const bool clicked = ImGui::Selectable("##row", m_sel == i, 0, ImVec2(0.0f, kRowH));
                ImGui::PopStyleColor(3);
                vinput_gui::AnchorLastItem("palette-item", it.label.c_str());
                if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) m_sel = i;
                if (m_sel == i && m_scrollToSel) { ImGui::SetScrollHereY(0.5f); m_scrollToSel = false; }

                const bool sel = (m_sel == i);
                const ImVec2 p1(p0.x + w, p0.y + kRowH);
                if (sel)
                {
                    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(th::Selection), 3.0f);
                    dl->AddRectFilled(p0, ImVec2(p0.x + 2.0f, p1.y), ImGui::GetColorU32(th::Accent), 2.0f);
                }
                const float cy = (p0.y + p1.y) * 0.5f;
                const float alpha = it.enabled ? 1.0f : 0.45f;
                ImVec4 iconCol = TintColor(it.tint);
                iconCol.w *= alpha;
                ui::DrawIconCentered(dl, it.icon, ImVec2(p0.x + 20.0f, cy), ImGui::GetColorU32(iconCol), 16.0f);

                // 右端: キー表記（等幅・dim）
                float rightW = 0.0f;
                if (!it.chord.empty())
                {
                    ui::PushMono();
                    const ImVec2 cs = ImGui::CalcTextSize(it.chord.c_str());
                    dl->AddText(ImVec2(p1.x - 12.0f - cs.x, std::floor(cy - cs.y * 0.5f + 0.5f)),
                                ImGui::GetColorU32(th::TextDim), it.chord.c_str());
                    ui::PopMono();
                    rightW = cs.x + 16.0f;
                }
                // 補足（カテゴリ / 親階層 / パス）は dim。ラベルの右に続ける
                const float lx = p0.x + 38.0f;
                const float ly = std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f);
                const float labelMax = (p1.x - rightW) - lx - 8.0f;
                ImVec4 tc = th::Text; tc.w *= alpha;
                ImVec4 hc = th::AccentHover; hc.w *= alpha;
                DrawHighlighted(dl, ImVec2(lx, ly), it.label, it.hl, ImGui::GetColorU32(tc), ImGui::GetColorU32(hc), labelMax);
                if (!it.sub.empty())
                {
                    const float lw = ImGui::CalcTextSize(it.label.c_str()).x;
                    const float sx = lx + lw + 10.0f;
                    if (sx < p1.x - rightW - 24.0f)
                    {
                        dl->PushClipRect(ImVec2(sx, p0.y), ImVec2(p1.x - rightW - 8.0f, p1.y), true);
                        dl->AddText(ImVec2(sx, ly), ImGui::GetColorU32(th::TextFaint), it.sub.c_str());
                        dl->PopClipRect();
                    }
                }

                if (clicked) { m_sel = i; activate = true; }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // ---- フッタ（操作の案内）----
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, th::TextFaint);
        ImGui::TextUnformatted("↑↓ 選択    Enter 実行    Esc 閉じる");
        if (n > 0)
        {
            char cnt[32];
            std::snprintf(cnt, sizeof(cnt), "%d 件", n);
            const float cw = ImGui::CalcTextSize(cnt).x;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - cw);
            ImGui::TextUnformatted(cnt);
        }
        ImGui::PopStyleColor();

        if (activate && m_sel >= 0 && m_sel < n)
        {
            const Item chosen = m_items[static_cast<size_t>(m_sel)];   // Activate 内で m_items を触らないがコピーで安全に
            Activate(ctx, env, reg, assets, chosen);
            if (!m_open)
            {
                ImGui::EndPopup();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(3);
                return;
            }
        }

        // モーダルの外をクリックしたら閉じる
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
            closeRequested = true;

        if (closeRequested)
        {
            Close(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    else if (!m_justOpened)
    {
        // ポップアップが（何かの拍子に）閉じていたらこちらも閉じる
        Close(ctx);
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

} // namespace dx12e
