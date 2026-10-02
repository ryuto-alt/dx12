#include "editor/panels/AssetBrowserPanel.h"
#include "editor/UiWidgets.h"
#include "core/VirtualGuard.h"   // 仮想入力モード中は ShellExecute / ダイアログを実行しない
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/EditorPrefs.h"
#include "editor/ModelThumbnailRenderer.h"
#include "editor/panels/MaterialPreviewRenderer.h"
#include "resource/ResourceManager.h"
#include "resource/MaterialAssetIO.h"
#include "core/Logger.h"
#include "core/AtomicFileJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <commdlg.h>

namespace
{
// アセットをごみ箱へ送る（完全削除しない）。
// アセットの参照元を調べる仕組みが弱い以上、「消す前に確認」だけでは足りず「消した後に戻せる」が要る。
// 失敗時は false を返し、呼び出し側がログに出す。
bool MoveToRecycleBin(const std::filesystem::path& path)
{
    // pFrom は二重 NUL 終端が要る（複数パスを NUL 区切りで並べる仕様のため）。
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    // ALLOWUNDO = ごみ箱行き。NOCONFIRMATION/NOERRORUI/SILENT はエディタ側で既に
    // 確認ダイアログを出しているので、OS の UI を二重に出さないため。
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int rc = ::SHFileOperationW(&op);
    return rc == 0 && !op.fAnyOperationsAborted;
}

void OpenInVSCode(const std::string& filePath)
{
    if (dx12e::guard::Blocked("Open in VS Code")) return;   // 仮想入力モード: 人の画面に窓を出さない
    // Code.exe を SHGetFolderPathA (LOCALAPPDATA) 経由で探す
    static std::string cachedExe;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        char appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData)))
        {
            namespace fs = std::filesystem;
            fs::path candidate = fs::path(appData) / "Programs" / "Microsoft VS Code" / "Code.exe";
            if (fs::exists(candidate))
                cachedExe = candidate.string();
        }
        if (cachedExe.empty())
        {
            // Program Files もチェック
            namespace fs = std::filesystem;
            const char* dirs[] = {"C:\\Program Files\\Microsoft VS Code\\Code.exe",
                                  "C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe"};
            for (const char* p : dirs)
                if (fs::exists(p)) { cachedExe = p; break; }
        }
    }

    if (!cachedExe.empty())
    {
        // ShellExecute の "open" で Code.exe にファイルを渡して起動する。
        // CreateProcess で「エディタの子プロセス」として直接起動すると、VSCode を閉じた後に
        // 単一インスタンス（ロックファイル＋名前付きパイプ）の後始末が正しく行われず、
        // 再度ダブルクリックしてもロックに転送されて窓が出ない、という症状が起きやすい。
        std::string args = "\"" + filePath + "\"";
        HINSTANCE r = dx12e::guard::ShellExecuteGuarded(nullptr, "open", cachedExe.c_str(), args.c_str(),
                                    nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32)
            return;  // 起動成功（> 32 が成功の規約）
    }

    // フォールバック: 拡張子の関連付けで開く
    dx12e::guard::ShellExecuteGuarded(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// file_time_type の ticks（FILETIME と同じ 100ns 単位）→ "2026-09-30 14:05"（ローカル時刻）。0 なら "-"。
std::string FormatTicks(int64_t ticks)
{
    if (ticks <= 0) return "-";
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(static_cast<uint64_t>(ticks) & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32);
    FILETIME lt;
    SYSTEMTIME st;
    if (!::FileTimeToLocalFileTime(&ft, &lt) || !::FileTimeToSystemTime(&lt, &st)) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

bool ValidFileName(const std::string& n)
{
    if (n.empty() || n == "." || n == "..") return false;
    if (n.back() == ' ' || n.back() == '.') return false;
    for (char c : n)
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    return true;
}
} // anonymous namespace

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

namespace th = dx12e::theme;
using abl::Kind;

// ============================================================ 種別の色 / グリフ（テーマのトークンだけを引く）

static ImVec4 KindColor(Kind k)
{
    switch (k)
    {
    case Kind::Folder:        return th::TypeFolder;
    case Kind::Model:         return th::TypeCamera;    // 青
    case Kind::Texture:       return th::TypePhysics;   // 緑
    case Kind::Scene:         return th::TypeScene;     // 橙
    case Kind::Script:        return th::TypeScript;    // 紫
    case Kind::Audio:         return th::TypeAudio;     // ティール
    case Kind::Prefab:        return th::TypePrefab;
    case Kind::Shader:        return th::TypeScript;    // コード系はスクリプトと同じ紫
    case Kind::Material:      return th::TypeLight;     // 琥珀
    case Kind::UiAnim:        return th::TypeUi;        // アニメ系はマゼンタ寄りで統一
    case Kind::SpriteSheet:   return th::TypeUi;
    case Kind::MaterialGraph: return th::TypeLight;
    default:                  return th::TypeEmpty;
    }
}

static const char* KindGlyph(Kind k)
{
    switch (k)
    {
    case Kind::Folder:        return ICON_FOLDER;
    case Kind::Model:         return ICON_T_MESH;
    case Kind::Texture:       return ICON_IMAGE;
    case Kind::Scene:         return ICON_FILM;
    case Kind::Script:        return ICON_FILE_CODE;
    case Kind::Audio:         return ICON_MUSIC;
    case Kind::Prefab:        return ICON_PACKAGE;
    case Kind::Shader:        return ICON_T_SHADER;
    case Kind::Material:      return ICON_T_MATERIAL;
    case Kind::UiAnim:        return ICON_T_ANIM;
    case Kind::SpriteSheet:   return ICON_T_GRID;
    case Kind::MaterialGraph: return ICON_T_SPLINE;   // .dxmg（ノードのつながり）
    default:                  return ICON_FILE;
    }
}

static bool KindIsDraggableToScene(Kind k)
{
    return k == Kind::Model || k == Kind::Texture || k == Kind::Script || k == Kind::Prefab || k == Kind::Material
        || k == Kind::UiAnim || k == Kind::SpriteSheet || k == Kind::Shader;
}

// ============================================================ 生成 / 初期化

AssetBrowserPanel::AssetBrowserPanel() = default;

AssetBrowserPanel::~AssetBrowserPanel()
{
    m_index.Stop();
    m_thumbs.Shutdown();
}

static std::filesystem::path StripTrailingSep(const std::string& s)
{
    std::filesystem::path p(s);
    if (!p.empty() && !p.has_filename()) p = p.parent_path();
    return p;
}

void AssetBrowserPanel::Initialize(const std::string& assetsDir,
                                   const std::string& scriptsDir,
                                   ResourceManager* resourceManager,
                                   DescriptorHeap* srvHeap)
{
    m_resourceManager = resourceManager;
    m_srvHeap         = srvHeap;
    if (resourceManager && srvHeap)
        m_thumbs.Initialize(resourceManager->GetDevice(), srvHeap);
    SetRoots(assetsDir, scriptsDir);
}

void AssetBrowserPanel::SetRoots(const std::string& assetsDir, const std::string& scriptsDir)
{
    m_assetsRoot  = StripTrailingSep(assetsDir);
    m_scriptsRoot = StripTrailingSep(scriptsDir);
    m_thumbs.SetAssetsDir(assetsDir);
    if (m_thumbRenderer) m_thumbRenderer->SetCacheRoot(assetsDir);
    OnRootsChanged();
}

void AssetBrowserPanel::SetThumbnailRenderer(ModelThumbnailRenderer* r)
{
    m_thumbRenderer = r;
    if (r && !m_assetsRoot.empty()) r->SetCacheRoot(m_assetsRoot.string());
}

void AssetBrowserPanel::OnRootsChanged()
{
    m_index.SetWatchRoots({m_assetsRoot, m_scriptsRoot});
    m_currentDir = m_assetsRoot;
    m_entries.clear();
    m_view.clear();
    m_order.clear();
    m_matCache.clear();
    m_sel.Clear();
    m_cursor = -1;
    m_searchBuf[0] = '\0';
    m_appliedQuery.clear();
    m_renamePath.clear();
    m_scrollToPath.clear();
    m_viewDirty = true;
    RequestScan();
}

void AssetBrowserPanel::LoadPrefs()
{
    m_prefsLoaded = true;
    m_listView   = prefs::GetBool("asset.listView", false);
    m_cellSize   = (std::max)(56.0f, (std::min)(192.0f, prefs::GetFloat("asset.cellSize", 96.0f)));
    m_treeWidth  = (std::max)(90.0f, (std::min)(420.0f, prefs::GetFloat("asset.treeWidth", 170.0f)));
    m_sortKey    = static_cast<abl::SortKey>((std::max)(0, (std::min)(abl::kSortKeyCount - 1, prefs::GetInt("asset.sortKey", 0))));
    m_sortAsc    = prefs::GetBool("asset.sortAsc", true);
    m_kindMask   = static_cast<uint32_t>(prefs::GetInt("asset.kindMask", 0));
    m_viewDirty  = true;
}

void AssetBrowserPanel::SavePrefs()
{
    prefs::SetBool("asset.listView", m_listView);
    prefs::SetFloat("asset.cellSize", m_cellSize);
    prefs::SetFloat("asset.treeWidth", m_treeWidth);
    prefs::SetInt("asset.sortKey", static_cast<int>(m_sortKey));
    prefs::SetBool("asset.sortAsc", m_sortAsc);
    prefs::SetInt("asset.kindMask", static_cast<int>(m_kindMask));
}

void AssetBrowserPanel::ForceRefresh()
{
    m_index.ForceRescan();
}

void AssetBrowserPanel::RevealAsset(const std::filesystem::path& absPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(absPath, ec)) return;
    m_searchBuf[0] = '\0';
    m_appliedQuery.clear();
    m_kindMask = 0;
    const bool isDir = std::filesystem::is_directory(absPath, ec);
    m_currentDir = isDir ? absPath : absPath.parent_path();
    m_scrollToPath = absPath.string();
    m_sel.Clear();
    m_sel.set.insert(m_scrollToPath);
    m_sel.primary = m_sel.anchor = m_scrollToPath;
    m_entries.clear();
    m_viewDirty = true;
    RequestScan();
}

void AssetBrowserPanel::NavigateTo(const std::filesystem::path& dir)
{
    m_searchBuf[0] = '\0';
    m_appliedQuery.clear();
    m_currentDir = dir;
    m_sel.Clear();
    m_cursor = -1;
    m_renamePath.clear();
    m_entries.clear();
    m_view.clear();
    m_order.clear();
    m_viewDirty = true;
    m_setScrollY = 0.0f;
    // ツリーでこのフォルダが見えるよう祖先を開く
    {
        std::error_code ec;
        std::filesystem::path p = dir;
        for (int i = 0; i < 64 && !p.empty() && p != p.parent_path(); ++i)
        {
            m_treeOpen[p.parent_path().string()] = true;
            p = p.parent_path();
        }
    }
    RequestScan();
}

void AssetBrowserPanel::SetSearch(const std::string& q)
{
    std::snprintf(m_searchBuf, sizeof(m_searchBuf), "%s", q.c_str());
    m_appliedQuery = m_searchBuf;
    m_entries.clear();
    m_viewDirty = true;
    RequestScan();
}

// ============================================================ 走査 / 表示リスト

void AssetBrowserPanel::RequestScan()
{
    abl::ScanOptions o;
    o.dir = m_currentDir;
    o.query = m_appliedQuery;
    m_index.Request(o);
    m_scanPending = true;
    m_scannedDir = o.dir;
    m_scannedQuery = o.query;
}

void AssetBrowserPanel::PollScan()
{
    abl::ScanResult r;
    if (!m_index.Poll(r)) return;
    m_scanPending = false;
    if (r.dirMissing)
    {
        // 現在のフォルダが消えた（削除・移動）→ 親をたどって存在する所まで戻る
        std::filesystem::path p = m_currentDir;
        std::error_code ec;
        while (!p.empty() && !std::filesystem::is_directory(p, ec) && p != m_assetsRoot && p != m_scriptsRoot)
            p = p.parent_path();
        if (p.empty() || !std::filesystem::is_directory(p, ec)) p = m_assetsRoot;
        if (p != m_currentDir) { m_currentDir = p; m_entries.clear(); m_viewDirty = true; RequestScan(); }
        return;
    }
    m_entries = std::move(r.entries);
    m_searchTruncated = r.truncated;
    m_viewDirty = true;
    // 名前を検索語で絞った結果が返るまでの間に検索語が変わっていたら、次の要求はデバウンスが出す
}

bool AssetBrowserPanel::HasUpEntry() const
{
    return m_appliedQuery.empty() && m_currentDir != m_assetsRoot && m_currentDir != m_scriptsRoot
        && !m_currentDir.empty() && m_currentDir.has_parent_path();
}

void AssetBrowserPanel::RebuildView()
{
    m_viewDirty = false;
    m_view.clear();
    m_order.clear();

    std::vector<int> idx;
    idx.reserve(m_entries.size());
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
    {
        const Entry& e = m_entries[static_cast<size_t>(i)];
        if (abl::PassesKind(e.kind, e.isDir, m_kindMask)) idx.push_back(i);
    }
    const abl::SortKey key = m_sortKey;
    const bool asc = m_sortAsc;
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        return abl::EntryLess(m_entries[static_cast<size_t>(a)], m_entries[static_cast<size_t>(b)], key, asc);
    });

    if (HasUpEntry())
    {
        m_upEntry = Entry{};
        m_upEntry.path = m_currentDir.parent_path();
        m_upEntry.name = "..";
        m_upEntry.kind = Kind::Folder;
        m_upEntry.isDir = true;
        m_view.push_back(-1);
    }
    m_order.reserve(idx.size());
    for (int i : idx)
    {
        m_view.push_back(i);
        m_order.push_back(m_entries[static_cast<size_t>(i)].path.string());
    }
    abl::PruneSelection(m_sel, m_order);
    if (m_cursor >= static_cast<int>(m_order.size())) m_cursor = static_cast<int>(m_order.size()) - 1;

    // 走査後にこの項目を見える位置へ（クイックオープン / 新規作成 / 取り込み）
    if (!m_scrollToPath.empty() && !m_scanPending)
    {
        for (size_t i = 0; i < m_order.size(); ++i)
            if (m_order[i] == m_scrollToPath)
            {
                m_sel.Clear();
                m_sel.set.insert(m_scrollToPath);
                m_sel.primary = m_sel.anchor = m_scrollToPath;
                m_cursor = static_cast<int>(i);
                break;
            }
    }
}

const abl::Entry& AssetBrowserPanel::EntryAt(int viewIndex) const
{
    const int e = m_view[static_cast<size_t>(viewIndex)];
    return e < 0 ? m_upEntry : m_entries[static_cast<size_t>(e)];
}

std::vector<std::filesystem::path> AssetBrowserPanel::SelectedPaths() const
{
    // 表示順で返す（複数選択の操作の順序を安定させる）
    std::vector<std::filesystem::path> out;
    for (const std::string& p : m_order)
        if (m_sel.set.count(p)) out.emplace_back(p);
    return out;
}

std::string AssetBrowserPanel::AssetRelPath(const std::filesystem::path& p) const
{
    std::error_code ec;
    if (!m_assetsRoot.empty() && abl::IsInside(p, m_assetsRoot))
        return std::filesystem::relative(p, m_assetsRoot, ec).generic_string();
    if (!m_scriptsRoot.empty() && abl::IsInside(p, m_scriptsRoot))
        return "scripts/" + std::filesystem::relative(p, m_scriptsRoot, ec).generic_string();
    return {};
}

// ============================================================ サムネイル

void AssetBrowserPanel::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    m_thumbs.Pump(cmdList);
}

u64 AssetBrowserPanel::GetOrQueueThumbnail(const std::string& absPath)
{
    std::error_code ec;
    const auto mt = std::filesystem::last_write_time(absPath, ec);
    if (ec) return 0;
    const auto sz = std::filesystem::file_size(absPath, ec);
    return m_thumbs.Get(absPath, abl::ToTicks(mt), ec ? 0 : static_cast<uint64_t>(sz));
}

// .dxmat の albedo（サムネイルの代用元）。更新時刻つきでキャッシュ＝毎フレームのパース / 全件同期パースをやめた。
// 1 フレームに解決する数を絞る（初めて可視になった 100 個のマテリアルで 100 回ファイルを読まない）。
const AssetBrowserPanel::MatInfo* AssetBrowserPanel::ResolveMaterial(const Entry& e)
{
    const std::string key = e.path.string();
    auto it = m_matCache.find(key);
    if (it != m_matCache.end() && it->second.mtime == e.mtime) return &it->second;
    if (m_matResolvesThisFrame >= 16) return it != m_matCache.end() ? &it->second : nullptr;
    ++m_matResolvesThisFrame;

    MatInfo mi;
    mi.mtime = e.mtime;
    std::ifstream ifs(e.path, std::ios::binary);
    if (ifs)
    {
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        MaterialAssetData data;
        if (ParseMaterialAsset(bytes, data) && !data.albedoPath.empty())
        {
            std::filesystem::path abs = m_assetsRoot / data.albedoPath;
            std::error_code ec;
            if (std::filesystem::is_regular_file(abs, ec))
            {
                mi.albedoAbs = abs.string();
                const auto mt = std::filesystem::last_write_time(abs, ec);
                mi.albedoMtime = ec ? 0 : abl::ToTicks(mt);
                const auto sz = std::filesystem::file_size(abs, ec);
                mi.albedoSize = ec ? 0 : static_cast<uint64_t>(sz);
            }
        }
    }
    return &(m_matCache[key] = std::move(mi));
}

// 可視セルからだけ呼ぶ。GPU ハンドルを返す（0 = まだ / 種別にサムネイルが無い）。
u64 AssetBrowserPanel::ThumbHandleFor(const Entry& e)
{
    switch (e.kind)
    {
    case Kind::Texture:
        return m_thumbs.Get(e.path.string(), e.mtime, e.size);
    case Kind::Model:
    {
        if (!m_thumbRenderer) return 0;
        const std::string key = e.path.string();
        const u64 h = m_thumbRenderer->GetCachedHandle(key);
        if (h == 0) m_thumbRenderer->Request(key, e.mtime, e.size);
        return h;
    }
    case Kind::Material:
    {
        // 球体サムネイル（MaterialPreviewRenderer）を優先。生成待ちの間・レンダラー不在時は albedo の平面サムネで代用。
        if (m_materialPreview)
        {
            const u64 sphere = m_materialPreview->GetOrQueueThumbnail(e.path.string());
            if (sphere != 0) return sphere;
        }
        if (const MatInfo* mi = ResolveMaterial(e); mi && !mi->albedoAbs.empty())
            return m_thumbs.Get(mi->albedoAbs, mi->albedoMtime, mi->albedoSize);
        return 0;
    }
    default:
        return 0;
    }
}

// カード 1 枚の見た目（サムネイル or 種別アイコン）。座標は左上 (x, y)・1 辺 size。
void AssetBrowserPanel::DrawThumbOrGlyph(ImDrawList* dl, const Entry& e, float x, float y, float size, bool hovered)
{
    const ImVec2 mn(x, y), mx(x + size, y + size);
    const ImVec4 typeColor = KindColor(e.kind);
    const float rounding = ui::PxF(3.0f);
    const u64 h = e.isDir ? 0 : ThumbHandleFor(e);

    if (h != 0)
    {
        // 面（画像が透けたときの下地）→ 画像（縦横比を保って中に収める）→ 枠 → 下辺の種別カラー帯
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(th::Bg2), rounding);
        float iw = size, ih = size;
        if (e.kind == Kind::Texture)
        {
            u32 tw = 0, tht = 0;
            if (m_thumbs.SizeOf(e.path.string(), tw, tht) && tw > 0 && tht > 0)
            {
                const float ar = static_cast<float>(tw) / static_cast<float>(tht);
                if (ar >= 1.0f) ih = size / ar; else iw = size * ar;
            }
        }
        const ImVec2 imn(x + (size - iw) * 0.5f, y + (size - ih) * 0.5f);
        dl->AddImage(static_cast<ImTextureID>(h), imn, ImVec2(imn.x + iw, imn.y + ih));
        dl->AddRect(mn, mx, ImGui::GetColorU32(hovered ? th::BorderStrong : th::Border), rounding, 0, ui::Px(1.0f));
        dl->AddRectFilled(ImVec2(mn.x + ui::Px(1.0f), mx.y - ui::Px(3.0f)), ImVec2(mx.x - ui::Px(1.0f), mx.y - ui::Px(1.0f)),
                          ImGui::GetColorU32(th::WithAlpha(typeColor, 0.9f)));
        return;
    }

    // カード背景（Bg2。ホバーで Bg3）+ 1px の枠。種別は下辺の細い色帯で示す。
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(hovered ? th::Bg3 : th::Bg2), rounding);
    dl->AddRect(mn, mx, ImGui::GetColorU32(hovered ? th::BorderStrong : th::Border), rounding, 0, ui::Px(1.0f));
    if (!e.isDir)
        dl->AddRectFilled(ImVec2(mn.x + ui::Px(1.0f), mx.y - ui::Px(3.0f)), ImVec2(mx.x - ui::Px(1.0f), mx.y - ui::Px(1.0f)),
                          ImGui::GetColorU32(th::WithAlpha(typeColor, 0.85f)));

    const bool isUp = (e.name == "..");
    const ImVec2 c(x + size * 0.5f, y + size * 0.5f - size * 0.03f);
    const float px = (std::max)(ui::Px(16.0f), size * 0.62f);
    ui::DrawIconCentered(dl, isUp ? ICON_ARROW_UP : KindGlyph(e.kind), c, ImGui::GetColorU32(typeColor), px);

    // 拡張子（下部。控えめな中性ピル）
    if (!e.isDir && size >= ui::Px(48.0f))
    {
        const std::string ext = e.path.extension().string();
        if (!ext.empty())
        {
            ui::PushMono();
            const ImVec2 ts = ImGui::CalcTextSize(ext.c_str());
            const ImVec2 bMin(mn.x + (size - ts.x) * 0.5f - ui::Px(4.0f), mx.y - ts.y - ui::Px(8.0f));
            const ImVec2 bMax(bMin.x + ts.x + ui::Px(8.0f), bMin.y + ts.y + ui::Px(2.0f));
            dl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(th::WithAlpha(th::Bg0, 0.75f)), ui::PxF(3.0f));
            dl->AddText(ImVec2(bMin.x + ui::Px(4.0f), bMin.y + ui::Px(1.0f)), ImGui::GetColorU32(th::TextDim), ext.c_str());
            ui::PopMono();
        }
    }
}

// ============================================================ Render

void AssetBrowserPanel::Render(EditorContext& ctx, f32 dt)
{
    LARGE_INTEGER qpcT0{}, qpcT1{}, qpcFreq{};
    ::QueryPerformanceCounter(&qpcT0);
    ctx.assetBrowser = this;
    m_dropCtx = &ctx;
    if (!m_prefsLoaded) LoadPrefs();
    m_matResolvesThisFrame = 0;
    m_drawnCells = 0;
    if (m_thumbRenderer) m_thumbRenderer->BeginFrameRequests();   // 見えているセルだけを要求し直す（スクロールで外れた要求は捨てる）

    ProcessOsDrops(ctx);
    PollImport(ctx);

    // 検索語のデバウンス: 入力が止まって 0.15 秒たったら走査を要求する（1 文字ごとに再帰走査を走らせない）
    if (m_appliedQuery != m_searchBuf)
    {
        m_searchDebounce += dt;
        if (m_searchDebounce >= 0.15f)
        {
            m_searchDebounce = 0.0f;
            m_appliedQuery = m_searchBuf;
            m_scrollToPath.clear();
            m_setScrollY = 0.0f;
            RequestScan();
        }
    }
    else m_searchDebounce = 0.0f;

    PollScan();
    // ツリーのキャッシュは変更検知（ReadDirectoryChangesW）で古くなる。表示側は SubDirs が裏で更新する。
    if (m_viewDirty) RebuildView();

    ImGui::Begin("\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6");  // アセットブラウザ

    DrawToolbar(ctx);
    ImGui::Separator();

    // ===== 2 カラム: 左=フォルダツリー | スプリッタ | 右=ファイル一覧 =====
    const float treeW = ui::Px(m_treeWidth);
    bool navigated = false;
    ImGui::BeginChild("##FolderTree", ImVec2(treeW, 0), ImGuiChildFlags_Borders);
    DrawTree(navigated);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    {
        // スプリッタ（ドラッグでツリーの幅を変える）
        const float w = ui::Px(6.0f);
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##TreeSplit", ImVec2(w, (std::max)(ImGui::GetContentRegionAvail().y, 1.0f)));
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
        {
            m_treeWidth += ImGui::GetIO().MouseDelta.x / ui::Scale();
            m_treeWidth = (std::max)(90.0f, (std::min)(420.0f, m_treeWidth));
        }
        if (ImGui::IsItemDeactivated()) SavePrefs();
        if (hov)
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sp.x + w * 0.5f - ui::Px(1.0f), sp.y),
                ImVec2(sp.x + w * 0.5f + ui::Px(1.0f), sp.y + ImGui::GetItemRectSize().y),
                ImGui::GetColorU32(ImGui::IsItemActive() ? th::Accent : th::BorderStrong));
    }
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::BeginChild("##FileGrid", ImVec2(0, 0));
    DrawContent(ctx);
    ImGui::EndChild();

    DrawModals(ctx);
    ImGui::End();
    m_dropCtx = nullptr;
    if (m_hasNav) { m_hasNav = false; NavigateTo(m_navTarget); }   // フォルダ移動は描画が終わってから

    ::QueryPerformanceCounter(&qpcT1);
    ::QueryPerformanceFrequency(&qpcFreq);
    m_lastRenderMs = static_cast<f32>(static_cast<double>(qpcT1.QuadPart - qpcT0.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart));
}

// ============================================================ ツールバー

void AssetBrowserPanel::DrawToolbar(EditorContext& ctx)
{
    // ---- 作成 / インポート ----
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui::PxF(3.0f));
    if (ImGui::Button(ICON_PLUS " 作成")) ImGui::OpenPopup("##CreateMenu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("このフォルダに新しいフォルダ / マテリアル / シーン / スクリプト / シェーダーを作る");
    ImGui::SameLine(0, ui::Px(4.0f));
    if (ImGui::Button(ICON_UPLOAD " インポート")) OpenImportDialog(ctx);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ファイルを選んでこのフォルダへ取り込む（エクスプローラーからこの窓へドロップしても取り込めます）");
    ImGui::PopStyleVar();

    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##CreateMenu"))
    {
        DrawBackgroundMenu(ctx);
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // ---- 右側のまとまり（並び順 / 表示切替 / サイズ）の幅を先に求め、検索欄は残りを使う ----
    const float sliderW = ui::Px(96.0f);
    const float iconBtn = ui::Px(24.0f);
    char sortLabel[64];
    std::snprintf(sortLabel, sizeof(sortLabel), "%s", abl::SortKeyLabel(m_sortKey));
    const float sortW = ImGui::CalcTextSize(sortLabel).x + ui::Px(58.0f);
    const float kindW = ui::Px(96.0f);
    const float rightW = sortW + iconBtn * 2.0f + sliderW + ui::Px(4.0f) * 3.0f + ui::Px(8.0f);

    ImGui::SameLine(0, ui::Px(10.0f));
    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth((std::max)(ui::Px(90.0f), (std::min)(ui::Px(260.0f), availW - rightW - kindW - ui::Px(20.0f))));
    if (ui::SearchField("##Search", m_searchBuf, sizeof(m_searchBuf), "検索（このフォルダ以下）"))
        m_searchDebounce = 0.0f;   // 入力のたびにデバウンスを延ばす（RequestScan は Render 側）

    // ---- 種別フィルタ（複数トグル）----
    ImGui::SameLine(0, ui::Px(6.0f));
    {
        int active = 0;
        for (int i = 0; i < abl::kChipCount; ++i)
            if (m_kindMask & abl::KindBit(abl::kChipKinds[i])) ++active;
        char lbl[32];
        if (active > 0) std::snprintf(lbl, sizeof(lbl), "種別 %d", active); else std::snprintf(lbl, sizeof(lbl), "種別");
        if (ui::LabelDropdownButton("##KindFilter", ICON_FILTER, lbl, "種別で絞り込む（複数選べます）", active > 0, 0.0f, nullptr))
            ImGui::OpenPopup("##KindPopup");
    }
    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##KindPopup"))
    {
        ImGui::TextDisabled("表示する種別");
        ImGui::Spacing();
        for (int i = 0; i < abl::kChipCount; ++i)
        {
            const abl::Kind k = abl::kChipKinds[i];
            const uint32_t bits = abl::ChipMask(k);
            bool on = (m_kindMask & abl::KindBit(k)) != 0;
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, KindColor(k));
            ImGui::TextUnformatted(KindGlyph(k));
            ImGui::PopStyleColor();
            ImGui::SameLine(0, ui::Px(6.0f));
            if (ui::Checkbox(abl::KindLabel(k), &on))
            {
                if (on) m_kindMask |= bits; else m_kindMask &= ~bits;
                m_viewDirty = true;
                SavePrefs();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("すべて表示に戻す", nullptr, false, m_kindMask != 0))
        {
            m_kindMask = 0; m_viewDirty = true; SavePrefs();
        }
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // ---- 右寄せ: 並び順 / グリッド・リスト / サイズ ----
    ImGui::SameLine(ImGui::GetContentRegionMax().x - rightW + ImGui::GetStyle().WindowPadding.x * 0.0f);
    {
        const char* dirGlyph = m_sortAsc ? ICON_ARROW_UP : ICON_ARROW_DOWN;
        if (ui::LabelDropdownButton("##SortBtn", dirGlyph, sortLabel, "並び順（フォルダは常に先頭）", false, 0.0f, nullptr))
            ImGui::OpenPopup("##SortPopup");
    }
    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##SortPopup"))
    {
        for (int i = 0; i < abl::kSortKeyCount; ++i)
        {
            const auto k = static_cast<abl::SortKey>(i);
            if (ImGui::MenuItem(abl::SortKeyLabel(k), nullptr, m_sortKey == k)) { m_sortKey = k; m_viewDirty = true; SavePrefs(); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("昇順", nullptr, m_sortAsc))  { m_sortAsc = true;  m_viewDirty = true; SavePrefs(); }
        if (ImGui::MenuItem("降順", nullptr, !m_sortAsc)) { m_sortAsc = false; m_viewDirty = true; SavePrefs(); }
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    ImGui::SameLine(0, ui::Px(4.0f));
    if (ui::IconButton("##ViewGrid", ICON_T_GRID, "グリッド表示", !m_listView, nullptr, iconBtn, ui::Px(15.0f)))
    { m_listView = false; SavePrefs(); }
    ImGui::SameLine(0, ui::Px(2.0f));
    if (ui::IconButton("##ViewList", ICON_LIST, "リスト表示", m_listView, nullptr, iconBtn, ui::Px(15.0f)))
    { m_listView = true; SavePrefs(); }

    // サイズはグリッド専用（リスト表示では薄くして触れなくする。項目自体は残す＝配置がずれない）
    ImGui::SameLine(0, ui::Px(4.0f));
    ImGui::SetNextItemWidth(sliderW);
    ImGui::BeginDisabled(m_listView);
    ui::SliderFloat("##Size", &m_cellSize, 56.0f, 192.0f, "%.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) SavePrefs();
    ImGui::EndDisabled();
}

// ============================================================ フォルダツリー

void AssetBrowserPanel::DrawTree(bool& navigated)
{
    DrawTreeNode(m_assetsRoot, "assets", 0, navigated);
    if (!m_scriptsRoot.empty()) DrawTreeNode(m_scriptsRoot, "scripts", 0, navigated);
}

void AssetBrowserPanel::DrawTreeNode(const std::filesystem::path& dir, const std::string& label, int depth, bool& navigated)
{
    std::vector<abl::DirInfo> kids;
    const bool have = m_index.SubDirs(dir, kids);
    const bool hasSub = !have || !kids.empty();   // 未取得の間は「あるかも」として矢印を出す（開くと確定）

    const std::string key = dir.string();
    const bool isSelected = (m_currentDir == dir);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasSub) flags |= ImGuiTreeNodeFlags_Leaf;
    if (depth == 0 && !m_treeOpen.count(key)) m_treeOpen[key] = true;   // ルートは初回に開く

    ImGui::PushID(key.c_str());
    const float rowX = ImGui::GetCursorScreenPos().x;
    ImGui::SetNextItemOpen(m_treeOpen[key], ImGuiCond_Always);   // TreeNodeEx の直前でないと効かない（ImGui の罠）
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(4.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));   // 標準の矢印と文字は隠して自前で描く
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (hasSub) m_treeOpen[key] = open;

    {
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const float cy = (mn.y + mx.y) * 0.5f;
        const bool hov = ImGui::IsItemHovered();
        // 行の面（窓の左端〜右端いっぱい）
        const ImRect r = ImGui::GetCurrentWindowRead()->InnerRect;
        ui::deco::RowFace(tdl, ImVec2(r.Min.x, mn.y), ImVec2(r.Max.x, mx.y), isSelected, hov, ImGui::IsItemActive());
        if (hasSub)
            ui::DrawIconCentered(tdl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                                 ImVec2(rowX + ui::Px(4.0f) + ImGui::GetFontSize() * 0.5f, cy),
                                 ImGui::GetColorU32(hov || isSelected ? th::Text : th::TextDim), ui::Px(13.0f));
        const float tx = rowX + ImGui::GetFontSize() + ui::Px(8.0f);
        ui::DrawIconCentered(tdl, open && hasSub ? ICON_FOLDER_OPEN : ICON_FOLDER,
                             ImVec2(tx + ui::Px(8.0f), cy), ImGui::GetColorU32(th::TypeFolder), ui::Px(16.0f));
        tdl->AddText(ImVec2(tx + ui::Px(22.0f), std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f)),
                     ImGui::GetColorU32(th::Text), label.c_str());
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() && !isSelected)
    {
        RequestNavigate(dir);
        navigated = true;
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (m_dropCtx) AcceptDropOnFolder(*m_dropCtx, dir);
        ImGui::EndDragDropTarget();
    }

    if (open)
    {
        if (hasSub && have)
            for (const abl::DirInfo& k : kids)
                DrawTreeNode(k.path, k.name, depth + 1, navigated);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// ============================================================ 右ペイン（パンくず + 一覧 + 詳細帯）

void AssetBrowserPanel::DrawContent(EditorContext& ctx)
{
    DrawBreadcrumb();
    if (m_listView) DrawListHeader();

    const float stripH = ImGui::GetTextLineHeight() + ui::Px(10.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##Items", ImVec2(0, -stripH), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    {
        // キーボード（このパネルにフォーカスがあるとき）
        HandleKeyboard(ctx);

        const bool onlyUp = (m_view.size() == 1 && HasUpEntry());
        if (!m_view.empty())
        {
            if (m_listView) DrawListRows(ctx); else DrawGrid(ctx);
        }
        if (m_view.empty() || onlyUp) DrawEmptyState();

        // 空白の左クリック = 選択解除 / 右クリック = 作成メニュー
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()
            && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
        {
            m_sel.Clear();
            m_cursor = -1;
        }
        ui::PushMenuStyle();
        if (ImGui::BeginPopupContextWindow("##BgCtx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            DrawBackgroundMenu(ctx);
            ImGui::EndPopup();
        }
        ui::PopMenuStyle();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    DrawDetailStrip();
}

void AssetBrowserPanel::DrawBreadcrumb()
{
    // 現在のフォルダが assets 配下か scripts 配下か
    const bool inScripts = !m_scriptsRoot.empty() && abl::IsInside(m_currentDir, m_scriptsRoot);
    const std::filesystem::path& root = inScripts ? m_scriptsRoot : m_assetsRoot;
    const std::vector<abl::Crumb> crumbs = abl::Breadcrumbs(root, inScripts ? "scripts" : "assets", m_currentDir);

    // パンくずは平たい文字ボタン（面はホバーだけ）。区切りは薄い chevron。各階層はフォルダの移動先（D&D の的）にもなる。
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, th::TextMid);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    for (size_t i = 0; i < crumbs.size(); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine(0, ui::Px(2.0f));
            ImGui::TextDisabled("%s", ICON_CHEVRON_RIGHT);
            ImGui::SameLine(0, ui::Px(2.0f));
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(crumbs[i].label.c_str()) && crumbs[i].path != m_currentDir)
            RequestNavigate(crumbs[i].path);
        if (ImGui::BeginDragDropTarget())
        {
            if (m_dropCtx) AcceptDropOnFolder(*m_dropCtx, crumbs[i].path);
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    // 検索中はそれを明示（今見ているのがフォルダの中身ではないと分かるように）
    if (!m_appliedQuery.empty())
    {
        ImGui::SameLine(0, ui::Px(10.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, th::AccentLight);
        ImGui::Text("\"%s\" の検索結果 %zu 件%s", m_appliedQuery.c_str(), m_view.size(), m_searchTruncated ? "+" : "");
        ImGui::PopStyleColor();
    }
    // 種別フィルタが効いているときは、外せるピルで示す
    if (m_kindMask != 0)
    {
        ImGui::SameLine(0, ui::Px(10.0f));
        for (int i = 0; i < abl::kChipCount; ++i)
        {
            const abl::Kind k = abl::kChipKinds[i];
            if (!(m_kindMask & abl::KindBit(k))) continue;
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), "%s ×", abl::KindLabel(k));
            ImGui::PushID(1000 + i);
            if (ui::Chip("##pill", lbl, true))
            {
                m_kindMask &= ~abl::ChipMask(k);
                m_viewDirty = true;
                SavePrefs();
            }
            ImGui::PopID();
            ImGui::SameLine(0, ui::Px(3.0f));
        }
    }
    if (m_scanPending && m_entries.empty())
    {
        ImGui::SameLine(0, ui::Px(10.0f));
        ImGui::TextDisabled("読み込み中…");
    }
}

void AssetBrowserPanel::DrawEmptyState()
{
    if (m_scanPending) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const char* msg = !m_appliedQuery.empty() ? "一致するアセットがありません"
                    : (m_kindMask != 0 && !m_entries.empty()) ? "この種別のアセットはありません"
                    : "このフォルダは空です";
    const char* sub = (m_appliedQuery.empty() && m_kindMask == 0) ? "ファイルをこの窓へドロップするか、右クリックから作成できます" : "";
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cx = p.x + avail.x * 0.5f;
    float y = p.y + (std::min)(avail.y * 0.3f, ui::Px(40.0f));
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(cx - ts.x * 0.5f, y), ImGui::GetColorU32(th::TextDim), msg);
    y += ts.y + ui::Px(4.0f);
    if (sub[0])
    {
        const ImVec2 ss = ImGui::CalcTextSize(sub);
        dl->AddText(ImVec2(cx - ss.x * 0.5f, y), ImGui::GetColorU32(th::TextFaint), sub);
    }
}

// ============================================================ グリッド（行クリッピング）

void AssetBrowserPanel::DrawGrid(EditorContext& ctx)
{
    const float ts = (std::max)(ui::Px(16.0f), ui::Px((std::max)(m_cellSize - 24.0f, 0.0f)));   // サムネイルの 1 辺（m_cellSize は論理 px）
    const float gap = ui::Px(6.0f);
    const float cellW = ts + ui::Px(16.0f);
    const float rowH = ts + ImGui::GetTextLineHeight() + ui::Px(18.0f);
    const int count = static_cast<int>(m_view.size());
    const abl::GridMetrics g = abl::ComputeGrid(count, ImGui::GetContentRegionAvail().x - ui::Px(4.0f), cellW);
    m_lastColumns = g.columns;

    // スクロール: 新規作成 / クイックオープン / キーボード移動で「この項目を見せる」
    auto rowOfView = [&](int viewIdx) { return viewIdx / g.columns; };
    auto ensureVisibleRow = [&](int row) {
        const float sy = ImGui::GetScrollY();
        const float vh = ImGui::GetWindowHeight();
        if (row * rowH < sy) ImGui::SetScrollY(row * rowH);
        else if ((row + 1) * rowH > sy + vh) ImGui::SetScrollY((row + 1) * rowH - vh);
    };
    if (m_setScrollY >= 0.0f) { ImGui::SetScrollY(m_setScrollY); m_setScrollY = -1.0f; }
    if (!m_scrollToPath.empty())
    {
        bool found = false;
        for (int i = 0; i < count; ++i)
            if (m_view[static_cast<size_t>(i)] >= 0 && EntryAt(i).path.string() == m_scrollToPath)
            {
                ImGui::SetScrollY((std::max)(0.0f, static_cast<float>(rowOfView(i)) * rowH - rowH));
                found = true;
                break;
            }
        if (found || (!m_scanPending && ++m_scrollTries > 90)) { m_scrollToPath.clear(); m_scrollTries = 0; }
    }
    if (m_cursorMoved && m_cursor >= 0)
    {
        ensureVisibleRow(rowOfView(m_cursor + (HasUpEntry() ? 1 : 0)));
        m_cursorMoved = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGuiListClipper clipper;
    clipper.Begin(g.rows, rowH);
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            for (int c = 0; c < g.columns; ++c)
            {
                const int viewIdx = r * g.columns + c;
                if (viewIdx >= count) break;
                DrawGridCell(ctx, viewIdx, rowPos.x + c * cellW + ui::Px(2.0f), rowPos.y, ts, cellW - gap * 0.5f, rowH - gap * 0.5f);
            }
            ImGui::SetCursorScreenPos(rowPos);
            ImGui::Dummy(ImVec2(1.0f, rowH));
        }
    }
    ImGui::PopStyleVar();
}

void AssetBrowserPanel::DrawGridCell(EditorContext& ctx, int viewIdx, float x, float y, float ts, float cw, float ch)
{
    const Entry& e = EntryAt(viewIdx);
    const bool isUp = m_view[static_cast<size_t>(viewIdx)] < 0;
    ++m_drawnCells;
    const std::string key = e.path.string();
    const bool renaming = !isUp && !m_renamePath.empty() && e.path == m_renamePath;
    const bool selected = !isUp && m_sel.Has(key);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushID(viewIdx);
    const ImVec2 mn(x, y), mx(x + cw, y + ch);
    bool hovered = false;
    if (!renaming)
    {
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton("##cell", ImVec2(cw, ch));
        hovered = ImGui::IsItemHovered();
    }

    // 選択 / ホバーの面（セル全体）。光るのは選択・ドラッグ中だけ（1a の規則）。
    if (selected)
    {
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(th::Selection), ui::PxF(4.0f));
        ui::deco::CardSelected(dl, mn, mx, ui::PxF(4.0f));
    }
    else if (hovered)
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(th::WithAlpha(th::Bg3, 0.55f)), ui::PxF(4.0f));

    const float thumbX = x + (cw - ts) * 0.5f;
    DrawThumbOrGlyph(dl, e, thumbX, y + ui::Px(4.0f), ts, hovered);

    // ファイル名（必ず 1 行。全文はツールチップ）
    const float nameY = y + ui::Px(4.0f) + ts + ui::Px(4.0f);
    if (renaming)
    {
        ImGui::SetCursorScreenPos(ImVec2(x, nameY - ui::Px(2.0f)));
        ImGui::SetNextItemWidth(cw);
        DrawRenameField(ctx);
    }
    else
    {
        std::string name = e.name;
        const float maxW = cw - ui::Px(6.0f);
        if (ImGui::CalcTextSize(name.c_str()).x > maxW)
        {
            while (name.size() > 2 && ImGui::CalcTextSize((name + "…").c_str()).x > maxW) name.pop_back();
            name += "…";
        }
        const float nameW = ImGui::CalcTextSize(name.c_str()).x;
        // フォルダは白、ファイルは少し落として「入れ物」と「中身」を見分けやすくする
        dl->AddText(ImVec2(x + (cw - nameW) * 0.5f, nameY),
                    ImGui::GetColorU32(e.isDir || selected ? th::Text : th::TextMid), name.c_str());
    }

    if (!renaming) HandleItemInteraction(ctx, e, viewIdx, isUp);
    ImGui::PopID();
}

// ============================================================ リスト表示

void AssetBrowserPanel::DrawListHeader()
{
    // 列見出し（クリックで並び順を切り替える）。固定（スクロールしない）。
    const float rowH = ImGui::GetFrameHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(th::Bg2));

    const float xType = w - ui::Px(300.0f), xMod = w - ui::Px(200.0f), xSize = w - ui::Px(70.0f);
    struct Col { const char* label; float x; abl::SortKey key; };
    const Col cols[] = {
        {"名前", ui::Px(30.0f), abl::SortKey::Name}, {"種別", xType, abl::SortKey::Type},
        {"更新日", xMod, abl::SortKey::Modified}, {"サイズ", xSize, abl::SortKey::Size},
    };
    for (int i = 0; i < 4; ++i)
    {
        const float x0 = p.x + cols[i].x;
        const float x1 = (i == 3) ? p.x + w : p.x + cols[i + 1].x;
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##col", ImVec2((std::max)(x1 - x0, 1.0f), rowH)))
        {
            if (m_sortKey == cols[i].key) m_sortAsc = !m_sortAsc; else { m_sortKey = cols[i].key; m_sortAsc = true; }
            m_viewDirty = true;
            SavePrefs();
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool cur = (m_sortKey == cols[i].key);
        dl->AddText(ImVec2(x0 + ui::Px(4.0f), p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(cur || hov ? th::Text : th::TextDim), cols[i].label);
        if (cur)
        {
            const ImVec2 ts = ImGui::CalcTextSize(cols[i].label);
            ui::DrawIconCentered(dl, m_sortAsc ? ICON_ARROW_UP : ICON_ARROW_DOWN,
                                 ImVec2(x0 + ui::Px(4.0f) + ts.x + ui::Px(10.0f), p.y + rowH * 0.5f), ImGui::GetColorU32(th::AccentHover), ui::Px(11.0f));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(1.0f, ui::Px(1.0f)));
}

void AssetBrowserPanel::DrawListRows(EditorContext& ctx)
{
    const float rowH = (std::max)(ImGui::GetFrameHeight(), ui::Px(24.0f));
    const int count = static_cast<int>(m_view.size());
    m_lastColumns = 1;

    if (m_setScrollY >= 0.0f) { ImGui::SetScrollY(m_setScrollY); m_setScrollY = -1.0f; }
    if (!m_scrollToPath.empty())
    {
        bool found = false;
        for (int i = 0; i < count; ++i)
            if (m_view[static_cast<size_t>(i)] >= 0 && EntryAt(i).path.string() == m_scrollToPath)
            { ImGui::SetScrollY((std::max)(0.0f, static_cast<float>(i) * rowH - rowH * 2.0f)); found = true; break; }
        if (found || (!m_scanPending && ++m_scrollTries > 90)) { m_scrollToPath.clear(); m_scrollTries = 0; }
    }
    if (m_cursorMoved && m_cursor >= 0)
    {
        const int vi = m_cursor + (HasUpEntry() ? 1 : 0);
        const float sy = ImGui::GetScrollY(), vh = ImGui::GetWindowHeight();
        if (vi * rowH < sy) ImGui::SetScrollY(vi * rowH);
        else if ((vi + 1) * rowH > sy + vh) ImGui::SetScrollY((vi + 1) * rowH - vh);
        m_cursorMoved = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGuiListClipper clipper;
    clipper.Begin(count, rowH);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Entry& e = EntryAt(i);
            const bool isUp = m_view[static_cast<size_t>(i)] < 0;
            ++m_drawnCells;
            const std::string key = e.path.string();
            const bool renaming = !isUp && !m_renamePath.empty() && e.path == m_renamePath;
            const bool selected = !isUp && m_sel.Has(key);
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            bool hovered = false, held = false;
            if (!renaming)
            {
                ImGui::InvisibleButton("##row", ImVec2(w, rowH));
                hovered = ImGui::IsItemHovered();
                held = ImGui::IsItemActive();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImRect ir = ImGui::GetCurrentWindowRead()->InnerRect;
            ui::deco::RowFace(dl, ImVec2(ir.Min.x, p.y), ImVec2(ir.Max.x, p.y + rowH), selected, hovered, held);
            if (!selected && !hovered && (i & 1)) dl->AddRectFilled(ImVec2(ir.Min.x, p.y), ImVec2(ir.Max.x, p.y + rowH), IM_COL32(255, 255, 255, 6));

            // アイコン or 小サムネイル
            const float ic = rowH - ui::Px(6.0f);
            const u64 th_ = e.isDir ? 0 : ThumbHandleFor(e);
            if (th_ != 0)
                dl->AddImage(static_cast<ImTextureID>(th_), ImVec2(p.x + ui::Px(6.0f), p.y + ui::Px(3.0f)), ImVec2(p.x + ui::Px(6.0f) + ic, p.y + ui::Px(3.0f) + ic));
            else
                ui::DrawIconCentered(dl, isUp ? ICON_ARROW_UP : KindGlyph(e.kind), ImVec2(p.x + ui::Px(6.0f) + ic * 0.5f, p.y + rowH * 0.5f),
                                     ImGui::GetColorU32(KindColor(e.kind)), ui::Px(16.0f));

            const float textY = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            const float xName = p.x + ui::Px(30.0f) + ui::Px(4.0f);
            const float xType = p.x + w - ui::Px(300.0f) + ui::Px(4.0f);
            const float xMod  = p.x + w - ui::Px(200.0f) + ui::Px(4.0f);
            const float xSize = p.x + w - ui::Px(70.0f) + ui::Px(4.0f);
            if (renaming)
            {
                ImGui::SetCursorScreenPos(ImVec2(xName - ui::Px(2.0f), p.y + ui::Px(1.0f)));
                ImGui::SetNextItemWidth((std::max)(xType - xName - ui::Px(8.0f), ui::Px(80.0f)));
                DrawRenameField(ctx);
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            }
            else
            {
                dl->PushClipRect(ImVec2(xName, p.y), ImVec2(xType - ui::Px(6.0f), p.y + rowH), true);
                dl->AddText(ImVec2(xName, textY), ImGui::GetColorU32(e.isDir || selected ? th::Text : th::TextMid), e.name.c_str());
                dl->PopClipRect();
                if (!isUp)
                {
                    dl->AddText(ImVec2(xType, textY), ImGui::GetColorU32(th::TextDim), abl::KindLabel(e.kind));
                    ui::PushMono();
                    dl->AddText(ImVec2(xMod, textY), ImGui::GetColorU32(th::TextDim), FormatTicks(e.mtime).c_str());
                    if (!e.isDir)
                        dl->AddText(ImVec2(xSize, textY), ImGui::GetColorU32(th::TextDim), abl::FormatSize(e.size).c_str());
                    ui::PopMono();
                }
                HandleItemInteraction(ctx, e, i, isUp);
            }
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
}

// ============================================================ 項目の操作（クリック・D&D・メニュー）

void AssetBrowserPanel::HandleItemInteraction(EditorContext& ctx, const Entry& e, int viewIdx, bool isUp)
{
    const ImGuiIO& io = ImGui::GetIO();
    const std::string key = e.path.string();
    const int orderIdx = viewIdx - (HasUpEntry() ? 1 : 0);

    if (!isUp)
    {
        // 押した瞬間に選択（複数選択の 1 つを「掴んだだけ」の時は、離した時に確定＝まとめてドラッグできる）
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (!io.KeyCtrl && !io.KeyShift && m_sel.Has(key) && m_sel.set.size() > 1)
                m_clickPending = key;
            else
            {
                abl::ApplyClick(m_sel, m_order, orderIdx, io.KeyCtrl, io.KeyShift);
                m_cursor = orderIdx;
            }
        }
        if (m_clickPending == key && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (ImGui::GetDragDropPayload() == nullptr && ImGui::IsItemHovered())
            {
                abl::ApplyClick(m_sel, m_order, orderIdx, false, false);
                m_cursor = orderIdx;
            }
            m_clickPending.clear();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_sel.Has(key))
        {
            abl::ApplyClick(m_sel, m_order, orderIdx, false, false);
            m_cursor = orderIdx;
        }
    }

    // ダブルクリック = 開く（フォルダは移動 / シーンは読み込み / モデルは配置 …）
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenEntry(ctx, e);

    // ドラッグ元
    if (!isUp && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        SetDragPayload(e);
        ImGui::EndDragDropSource();
    }
    // フォルダへのドロップ（移動）
    if (e.isDir && ImGui::BeginDragDropTarget())
    {
        AcceptDropOnFolder(ctx, e.path);
        ImGui::EndDragDropTarget();
    }

    // 右クリックメニュー
    if (!isUp)
    {
        ui::PushMenuStyle();
        if (ImGui::BeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight))
        {
            DrawItemMenu(ctx, e);
            ImGui::EndPopup();
        }
        ui::PopMenuStyle();
    }

    // ツールチップ
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, KindColor(e.kind));
        ImGui::Text("[%s]", abl::KindShortName(e.kind));
        ImGui::PopStyleColor();
        if (!isUp)
        {
            const std::string rel = AssetRelPath(e.path);
            if (!rel.empty()) ImGui::TextDisabled("%s", rel.c_str());
            if (!e.isDir) ImGui::TextDisabled("%s  ·  %s", abl::FormatSize(e.size).c_str(), FormatTicks(e.mtime).c_str());
        }
        if (e.kind == Kind::Model)       ImGui::TextDisabled("ダブルクリック: シーンに配置 / ドラッグ: シーンへドロップ");
        else if (e.kind == Kind::Scene)  ImGui::TextDisabled("ダブルクリック: シーンを開く");
        else if (e.kind == Kind::Script || e.kind == Kind::Shader) ImGui::TextDisabled("ダブルクリック: VS Code で開く");
        else if (e.isDir)                ImGui::TextDisabled("ダブルクリック: 開く / ここへドロップ: 移動");
        ImGui::EndTooltip();
    }
}

void AssetBrowserPanel::SetDragPayload(const Entry& e)
{
    const std::string key = e.path.string();
    if (!m_sel.Has(key))
    {
        m_sel.Clear();
        m_sel.set.insert(key);
        m_sel.primary = m_sel.anchor = key;
    }
    const std::vector<std::filesystem::path> sel = SelectedPaths();
    if (sel.size() <= 1 && KindIsDraggableToScene(e.kind))
    {
        // 従来どおりの単体ペイロード（シーン / インスペクタ / ヒエラルキーが受け取る）。フォルダへも落とせる。
        const char* payloadId = (e.kind == Kind::Script) ? "DND_SCRIPT" : kDragDropPayloadType;
        ImGui::SetDragDropPayload(payloadId, key.c_str(), key.size() + 1);
        ImGui::Text("%s", e.name.c_str());
        if (e.kind == Kind::Model || e.kind == Kind::Prefab) ImGui::TextDisabled("シーンへドロップで配置");
        else if (e.kind == Kind::Shader) ImGui::TextDisabled("MeshRenderer / Sprite2D / Camera へ");
        return;
    }
    // 移動専用（フォルダ・シーン・複数選択）。フォルダのツリー / パンくず / セルが受け取る。
    std::string data;
    for (const auto& p : sel) { if (!data.empty()) data.push_back('\n'); data += p.string(); }
    ImGui::SetDragDropPayload(kMovePayloadType, data.c_str(), data.size() + 1);
    if (sel.size() > 1) ImGui::Text("%zu 個の項目", sel.size()); else ImGui::Text("%s", e.name.c_str());
    ImGui::TextDisabled("フォルダへドロップで移動");
}

void AssetBrowserPanel::AcceptDropOnFolder(EditorContext& ctx, const std::filesystem::path& destDir)
{
    const char* types[] = {kMovePayloadType, kDragDropPayloadType, "DND_SCRIPT"};
    for (const char* t : types)
    {
        const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(t);
        if (!pl) continue;
        const std::string data(static_cast<const char*>(pl->Data));
        std::vector<RelocateOp> ops;
        size_t pos = 0;
        while (pos <= data.size())
        {
            size_t end = data.find('\n', pos);
            if (end == std::string::npos) end = data.size();
            const std::string one = data.substr(pos, end - pos);
            pos = end + 1;
            if (one.empty()) continue;
            const std::filesystem::path src(one);
            if (src.parent_path() == destDir) continue;   // 同じフォルダ（何もしない）
            ops.push_back({src, destDir / src.filename()});
        }
        if (!ops.empty()) BeginRelocate(ctx, std::move(ops), false, true);
        return;
    }
}

void AssetBrowserPanel::OpenEntry(EditorContext& ctx, const Entry& e)
{
    if (e.isDir) { RequestNavigate(e.path); return; }
    switch (e.kind)
    {
    case Kind::Model:
    case Kind::Prefab:
    {
        PendingSpawnRequest req;
        req.modelPath = e.path.string();
        req.position = SpawnPositionFrontOfCamera(ctx);
        ctx.pendingSpawns.push_back(req);
        break;
    }
    case Kind::Scene:
        ctx.pendingLoadPath = e.path.string();   // ダブルクリックでシーン切り替え（VS Code で開くのは右クリック「開く」から）
        break;
    case Kind::Script:
    case Kind::Shader:
        OpenInVSCode(e.path.string());
        break;
    case Kind::Material:
        ctx.pendingOpenMaterialPath = e.path.string();
        ctx.showMaterialEditor = true;
        break;
    case Kind::MaterialGraph:
        ctx.pendingOpenMatGraphPath = e.path.string();   // .dxmg = マテリアルグラフ窓で開く（G3）
        ctx.showMaterialGraph = true;
        break;
    case Kind::UiAnim:
        ctx.pendingOpenUiAnimPath = e.path.string();
        ctx.showAnimEditor = true;
        break;
    case Kind::SpriteSheet:
        ctx.pendingOpenSpriteSheetPath = e.path.string();
        ctx.showSpriteSheetEditor = true;
        break;
    default:
        dx12e::guard::ShellExecuteGuarded(nullptr, "open", e.path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
}

DirectX::XMFLOAT3 AssetBrowserPanel::SpawnPositionFrontOfCamera(const EditorContext& ctx)
{
    // カメラの前（床との交点があればそこ、無ければ 6m 先）。遠くで作業中に「置いたのに見えない」を避ける。
    if (!ctx.camValid) return {0.0f, 0.0f, 0.0f};
    const auto& p = ctx.camPos;
    const auto& f = ctx.camFwd;
    if (f.y < -0.05f)
    {
        const float t = -p.y / f.y;
        if (t > 1.0f && t < 60.0f) return {p.x + f.x * t, 0.0f, p.z + f.z * t};
    }
    return {p.x + f.x * 6.0f, p.y + f.y * 6.0f, p.z + f.z * 6.0f};
}

void AssetBrowserPanel::DrawItemMenu(EditorContext& ctx, const Entry& e)
{
    const std::vector<std::filesystem::path> sel = SelectedPaths();
    const bool multi = sel.size() > 1;

    // 主操作（種別ごと）
    if (!multi)
    {
        if (e.kind == Kind::Scene)
        {
            if (ui::MenuItem(ICON_FILM, "シーンを読み込み")) ctx.pendingLoadPath = e.path.string();
        }
        else if (e.kind == Kind::Model || e.kind == Kind::Prefab)
        {
            if (ui::MenuItem(ICON_PLUS, "シーンに追加"))
            {
                PendingSpawnRequest req;
                req.modelPath = e.path.string();
                req.position = SpawnPositionFrontOfCamera(ctx);
                ctx.pendingSpawns.push_back(req);
            }
        }
        else if (e.kind == Kind::Material || e.kind == Kind::MaterialGraph || e.kind == Kind::UiAnim || e.kind == Kind::SpriteSheet)
        {
            if (ui::MenuItem(ICON_EXTERNAL, "エディタで開く")) OpenEntry(ctx, e);
        }
        if (e.isDir)
        {
            if (ui::MenuItem(ICON_FOLDER_OPEN, "開く")) RequestNavigate(e.path);
        }
        else if (ui::MenuItem(ICON_FILE, "開く"))
        {
            if (e.kind == Kind::Scene || e.kind == Kind::Script || e.kind == Kind::Shader) OpenInVSCode(e.path.string());
            else dx12e::guard::ShellExecuteGuarded(nullptr, "open", e.path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
    }

    if (ui::MenuItem(ICON_FILE_TEXT, "名前を変更", "F2", false, !multi)) StartRename(e.path, false);
    if (ui::MenuItem(ICON_COPY, "複製", nullptr, false, true))
    {
        std::filesystem::path made;
        for (const auto& p : sel) DuplicateEntry(&ctx, p, &made);
    }
    if (ui::MenuItem(ICON_FOLDER_PLUS, "新しいフォルダーに入れる", nullptr, false, true))
    {
        std::filesystem::path folder;
        if (CreateFolderHere(&folder, /*rename=*/false))
        {
            std::vector<RelocateOp> ops;
            for (const auto& p : sel) ops.push_back({p, folder / p.filename()});
            BeginRelocate(ctx, std::move(ops), false, true);
        }
    }
    ImGui::Separator();
    if (ui::MenuItem(ICON_LINK, "パスをコピー", nullptr, false, !multi))
    {
        const std::string rel = AssetRelPath(e.path);
        ImGui::SetClipboardText((rel.empty() ? e.path.string() : rel).c_str());
        ctx.Notify(ui::ToastKind::Info, "パスをコピーしました: " + (rel.empty() ? e.path.string() : rel));
    }
    if (ui::MenuItem(ICON_EXTERNAL, "エクスプローラーで表示", nullptr, false, !multi))
        RevealInExplorer(e.path);
    ImGui::Separator();
    char delLabel[64];
    if (multi) std::snprintf(delLabel, sizeof(delLabel), "削除（%zu 個）", sel.size()); else std::snprintf(delLabel, sizeof(delLabel), "削除");
    if (ui::MenuItem(ICON_TRASH, delLabel, "Del")) RequestDelete(sel);
}

void AssetBrowserPanel::RevealInExplorer(const std::filesystem::path& p)
{
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec))
        dx12e::guard::ShellExecuteGuarded(nullptr, "explore", p.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else
    {
        const std::string args = "/select,\"" + p.string() + "\"";
        dx12e::guard::ShellExecuteGuarded(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void AssetBrowserPanel::DrawBackgroundMenu(EditorContext& ctx)
{
    if (ui::MenuItem(ICON_FOLDER_PLUS, "新しいフォルダ")) CreateFolderHere(nullptr, true);
    if (ui::MenuItem(ICON_T_MATERIAL, "新しいマテリアル")) CreateMaterialHere(nullptr);
    ImGui::Separator();
    if (ui::MenuItem(ICON_FILM, "新規シーン"))
    {
        ctx.showNewSceneDialog = true;
        ctx.newSceneDialogIsCreate = true;
        std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
        strncpy_s(ctx.newSceneNameBuf, "NewScene", _TRUNCATE);
    }
    if (ui::MenuItem(ICON_FILE_CODE, "新規スクリプト"))
    {
        ctx.showNewScriptDialog = true;
        std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
        strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
    }
    if (ui::MenuItem(ICON_T_SHADER, "新規シェーダー"))
    {
        ctx.showNewShaderDialog = true;
        std::memset(ctx.newShaderNameBuf, 0, sizeof(ctx.newShaderNameBuf));
        strncpy_s(ctx.newShaderNameBuf, "NewShader", _TRUNCATE);
    }
    ImGui::Separator();
    if (ui::MenuItem(ICON_UPLOAD, "ファイルをインポート…")) OpenImportDialog(ctx);
    if (ui::MenuItem(ICON_REFRESH, "再読み込み")) ForceRefresh();
    if (ui::MenuItem(ICON_EXTERNAL, "エクスプローラーで開く")) RevealInExplorer(m_currentDir);
}

// ============================================================ 詳細帯

void AssetBrowserPanel::DrawDetailStrip()
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight() + ui::Px(10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), ImGui::GetColorU32(th::Border));
    const float ty = p.y + ui::Px(5.0f);
    float x = p.x + ui::Px(6.0f);

    char left[192];
    if (m_sel.set.size() > 1)
        std::snprintf(left, sizeof(left), "%zu 個を選択", m_sel.set.size());
    else if (m_sel.set.size() == 1)
    {
        const std::string& pr = m_sel.primary;
        const std::filesystem::path pp(pr);
        // 選択物の情報は一覧のエントリから引く（表示順の探索は O(n)。選択が変わった時だけでよいが 1 行なので毎フレームでも軽い上限つき）
        const Entry* found = nullptr;
        for (size_t i = 0; i < m_entries.size() && i < 200000; ++i) if (m_entries[i].path == pp) { found = &m_entries[i]; break; }
        if (found)
        {
            std::snprintf(left, sizeof(left), "%s  ·  %s%s%s", found->name.c_str(), abl::KindLabel(found->kind),
                          found->isDir ? "" : "  ·  ", found->isDir ? "" : abl::FormatSize(found->size).c_str());
        }
        else std::snprintf(left, sizeof(left), "%s", pp.filename().string().c_str());
    }
    else left[0] = '\0';
    if (left[0])
    {
        dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(th::TextMid), left);
    }

    // 右: 件数
    char right[96];
    if (m_kindMask != 0 || !m_appliedQuery.empty())
        std::snprintf(right, sizeof(right), "%zu / %zu 項目%s", m_view.size() - (HasUpEntry() ? 1 : 0), m_entries.size(), m_searchTruncated ? "+" : "");
    else
        std::snprintf(right, sizeof(right), "%zu 項目", m_entries.size());
    ui::PushMono();
    const ImVec2 rs = ImGui::CalcTextSize(right);
    dl->AddText(ImVec2(p.x + w - rs.x - ui::Px(8.0f), ty), ImGui::GetColorU32(th::TextDim), right);
    ui::PopMono();
    if (m_importRunning)
        dl->AddText(ImVec2(p.x + w - rs.x - ui::Px(140.0f), ty), ImGui::GetColorU32(th::AccentHover), "取り込み中…");
    ImGui::Dummy(ImVec2(1.0f, h));
}

// ============================================================ キーボード

void AssetBrowserPanel::HandleKeyboard(EditorContext& ctx)
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !m_renamePath.empty()) return;   // 検索欄 / 名前変更の入力中は反応しない

    const int n = static_cast<int>(m_order.size());
    auto moveTo = [&](int idx, bool extend) {
        if (idx < 0 || idx >= n) return;
        abl::ApplyClick(m_sel, m_order, idx, false, extend);
        m_cursor = idx;
        m_cursorMoved = true;
    };
    const int cols = m_listView ? 1 : (std::max)(1, m_lastColumns);
    // 「..」の分だけ表示上の列がずれる: 上下移動の行計算は表示添字（「..」込み）で行う
    const int off = HasUpEntry() ? 1 : 0;
    auto step = [&](int dx, int dy) {
        if (n == 0) return;
        if (m_cursor < 0) { moveTo(0, false); return; }
        int v = m_cursor + off;
        int nv = v + dx + dy * cols;
        if (dx != 0 && cols > 1 && nv / cols != v / cols) nv = v;   // 左右は行をまたがない
        nv = (std::max)(off, (std::min)(n - 1 + off, nv));
        moveTo(nv - off, io.KeyShift);
    };
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  step(-1, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(1, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    step(0, -1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  step(0, 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) moveTo(0, io.KeyShift);
    if (ImGui::IsKeyPressed(ImGuiKey_End))  moveTo(n - 1, io.KeyShift);

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        m_sel.Clear();
        for (const std::string& p : m_order) m_sel.set.insert(p);
        if (!m_order.empty()) m_sel.primary = m_sel.anchor = m_order.front();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m_sel.Clear(); m_cursor = -1; }

    // Enter = 開く / F2 = 名前変更 / Del = 削除確認 / Backspace = 親フォルダへ
    if (!m_sel.primary.empty())
    {
        const std::filesystem::path pp(m_sel.primary);
        const Entry* found = nullptr;
        for (const Entry& e : m_entries) if (e.path == pp) { found = &e; break; }
        if (found && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) OpenEntry(ctx, *found);
        if (found && ImGui::IsKeyPressed(ImGuiKey_F2, false) && m_sel.set.size() == 1) StartRename(pp, false);
    }
    if (!m_sel.set.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) RequestDelete(SelectedPaths());
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && HasUpEntry()) RequestNavigate(m_currentDir.parent_path());
}

// ============================================================ 名前の変更

void AssetBrowserPanel::StartRename(const std::filesystem::path& p, bool isNew)
{
    m_renamePath = p;
    m_renameIsNew = isNew;
    m_renameWarmup = 3;   // 3 フレーム分フォーカス安定を待つ（セルが描かれてから数える）
    std::memset(m_renameBuf, 0, sizeof(m_renameBuf));
    strncpy_s(m_renameBuf, p.filename().string().c_str(), _TRUNCATE);
}

void AssetBrowserPanel::DrawRenameField(EditorContext& ctx)
{
    if (m_renameWarmup > 0)
    {
        ImGui::SetKeyboardFocusHere();
        --m_renameWarmup;
    }
    const bool entered = ui::InputText("##Rename", m_renameBuf, sizeof(m_renameBuf),
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_renamePath.clear(); return; }
    if (entered) { CommitRename(ctx); return; }
    if (m_renameWarmup == 0 && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) CommitRename(ctx);   // フォーカスが外れたら確定
}

void AssetBrowserPanel::CommitRename(EditorContext& ctx)
{
    const std::filesystem::path src = m_renamePath;
    const bool isNew = m_renameIsNew;
    m_renamePath.clear();
    std::string name = m_renameBuf;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name == src.filename().string()) return;
    if (!ValidFileName(name))
    {
        ctx.Notify(ui::ToastKind::Warn, "この名前は使えません（\\ / : * ? \" < > | は使えず、末尾に空白や . も付けられません）");
        return;
    }
    BeginRelocate(ctx, {RelocateOp{src, src.parent_path() / name}}, /*isRename=*/true, /*checkRefs=*/!isNew);
}

// ============================================================ ファイル操作

bool AssetBrowserPanel::CreateFolderHere(std::filesystem::path* out, bool rename)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string name = abl::UniqueName("新しいフォルダ", [&](const std::string& n) { return fs::exists(m_currentDir / n, ec); });
    const fs::path dir = m_currentDir / name;
    if (!fs::create_directory(dir, ec) || ec) { if (m_dropCtx) m_dropCtx->Notify(ui::ToastKind::Error, "フォルダを作れませんでした: " + ec.message()); return false; }
    if (out) *out = dir;
    m_index.ForceRescan();
    m_scrollToPath = dir.string();
    if (rename) StartRename(dir, true);
    return true;
}

bool AssetBrowserPanel::CreateMaterialHere(std::filesystem::path* out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string fileName = abl::UniqueName("NewMaterial.dxmat", [&](const std::string& n) { return fs::exists(m_currentDir / n, ec); });
    const fs::path file = m_currentDir / fileName;
    MaterialAssetData d;
    d.name = file.stem().string();
    d.metallic = 0.0f;
    d.roughness = 0.5f;
    const auto wr = dx12e::atomicfile::WriteFile(file, SerializeMaterialAsset(d), dx12e::atomicfile::JsonVerifier());
    if (!wr)
    {
        Logger::Warn("マテリアルの作成に失敗しました: {} ({})", file.string(), wr.error);
        if (m_dropCtx) m_dropCtx->Notify(ui::ToastKind::Error, "マテリアルを作れませんでした: " + wr.error);
        return false;
    }
    if (out) *out = file;
    m_index.ForceRescan();
    m_scrollToPath = file.string();
    StartRename(file, true);
    return true;
}

bool AssetBrowserPanel::DuplicateEntry(EditorContext* ctx, const std::filesystem::path& src, std::filesystem::path* out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(src, ec)) return false;
    const fs::path dir = src.parent_path();
    const std::string name = abl::UniqueName(src.filename().string(), [&](const std::string& n) { return fs::exists(dir / n, ec); }, "_copy");
    const fs::path dst = dir / name;
    // 原子的に複製する（途中で失敗したら複製先に半端なものを残さない）
    const atomicfile::Result copyRes = atomicfile::CopyTree(src, dst, /*overwrite=*/false);
    if (!copyRes.ok)
    {
        if (ctx) ctx->Notify(ui::ToastKind::Error, "複製できませんでした: " + copyRes.error);
        return false;
    }
    if (out) *out = dst;
    m_index.ForceRescan();
    m_scrollToPath = dst.string();
    if (ctx) ctx->Notify(ui::ToastKind::Success, "複製しました: " + name);
    return true;
}

void AssetBrowserPanel::RequestDelete(std::vector<std::filesystem::path> paths)
{
    if (paths.empty()) return;
    m_pendingDelete = std::move(paths);
}

// 名前変更 / 移動。参照しているファイルがあれば確認を出してから実行する（GUID 参照が無い現状では黙って切れるため）。
bool AssetBrowserPanel::BeginRelocate(EditorContext& ctx, std::vector<RelocateOp> ops, bool isRename, bool checkRefs)
{
    namespace fs = std::filesystem;
    std::vector<RelocateOp> ok;
    std::string firstProblem;
    for (RelocateOp& op : ops)
    {
        if (isRename)
        {
            std::error_code ec;
            if (fs::exists(op.dst, ec)) { if (firstProblem.empty()) firstProblem = "同じ名前のファイルがあります"; continue; }
            ok.push_back(op);
            continue;
        }
        const abl::MoveCheck c = abl::CheckMove(op.src, op.dst.parent_path(), [](const fs::path& p) { std::error_code e; return fs::exists(p, e); });
        if (c != abl::MoveCheck::Ok) { if (firstProblem.empty()) firstProblem = abl::MoveCheckMessage(c); continue; }
        ok.push_back(op);
    }
    if (ok.empty())
    {
        if (!firstProblem.empty()) ctx.Notify(ui::ToastKind::Warn, firstProblem);
        return false;
    }
    if (!firstProblem.empty()) ctx.Notify(ui::ToastKind::Warn, firstProblem);

    PendingRelocate pr;
    pr.ops = ok;
    pr.isRename = isRename;
    if (checkRefs)
    {
        std::vector<std::string> needles;
        for (const RelocateOp& op : ok)
        {
            std::string rel = AssetRelPath(op.src);
            if (rel.empty()) continue;
            std::error_code ec;
            if (fs::is_directory(op.src, ec)) rel.push_back('/');
            needles.push_back(rel);
        }
        const auto excluded = [&](const fs::path& f) {
            for (const RelocateOp& op : ok) if (abl::IsInside(f, op.src)) return true;
            return false;
        };
        const abl::RefScanResult rs = abl::FindReferences({m_assetsRoot, m_scriptsRoot}, needles, excluded);
        pr.refs = rs.files;
        pr.refsTruncated = rs.truncated;
    }
    if (pr.refs.empty())
        return ExecuteRelocate(&ctx, ok);
    pr.open = true;
    m_relocate = std::move(pr);
    return true;
}

bool AssetBrowserPanel::ExecuteRelocate(EditorContext* ctx, const std::vector<RelocateOp>& ops)
{
    namespace fs = std::filesystem;
    int moved = 0;
    std::string lastError;
    std::string lastName;
    for (const RelocateOp& op : ops)
    {
        std::error_code ec;
        // ★上書きしない。fs::rename は Windows では既存のファイルを黙って置き換える（BeginRelocate の確認をすり抜けた場合の最後の砦）。
        if (fs::exists(op.dst, ec)) { lastError = "同じ名前があります"; continue; }
        // 改名。別ボリュームなどのときはコピーを完成させてから元を消す。失敗したら元のまま（移動先に半端なものを残さない）
        const atomicfile::Result moveRes = atomicfile::MovePath(op.src, op.dst);
        if (!moveRes.ok) { lastError = moveRes.error; continue; }
        ++moved;
        lastName = op.dst.filename().string();
        // 開いているシーン / 選択 / 現在のフォルダが動いた分を追従させる
        if (ctx && !ctx->currentScenePath.empty())
        {
            const fs::path cur(ctx->currentScenePath);
            if (abl::IsInside(cur, op.src))
            {
                std::error_code e2;
                ctx->currentScenePath = (op.dst / fs::relative(cur, op.src, e2)).string();
            }
        }
        if (abl::IsInside(m_currentDir, op.src) && fs::is_directory(op.dst))
        {
            std::error_code e2;
            m_currentDir = op.dst / fs::relative(m_currentDir, op.src, e2);
        }
    }
    if (moved > 0)
    {
        const fs::path first = ops.front().dst;
        m_sel.Clear();
        for (const RelocateOp& op : ops) m_sel.set.insert(op.dst.string());
        m_sel.primary = m_sel.anchor = first.string();
        m_scrollToPath = first.string();
        m_index.ForceRescan();
        if (ctx)
        {
            if (moved == 1) ctx->Notify(ui::ToastKind::Success, std::string(ops.front().src.parent_path() == ops.front().dst.parent_path() ? "名前を変更しました: " : "移動しました: ") + lastName);
            else ctx->Notify(ui::ToastKind::Success, std::to_string(moved) + " 個を移動しました");
        }
    }
    if (!lastError.empty() && ctx) ctx->Notify(ui::ToastKind::Error, "一部を移動できませんでした: " + lastError);
    return moved > 0;
}

// ============================================================ UI 自動テスト用の操作（ダイアログを介さない）

bool AssetBrowserPanel::TestCreateFolder(std::filesystem::path* outCreated)
{
    return CreateFolderHere(outCreated, /*rename=*/false);
}

bool AssetBrowserPanel::TestRename(const std::filesystem::path& src, const std::string& newName)
{
    if (!ValidFileName(newName)) return false;
    return ExecuteRelocate(nullptr, {RelocateOp{src, src.parent_path() / newName}});
}

bool AssetBrowserPanel::TestDuplicate(const std::filesystem::path& src, std::filesystem::path* outCopy)
{
    return DuplicateEntry(nullptr, src, outCopy);
}

bool AssetBrowserPanel::TestMove(const std::filesystem::path& src, const std::filesystem::path& destDir)
{
    namespace fs = std::filesystem;
    const abl::MoveCheck c = abl::CheckMove(src, destDir, [](const fs::path& p) { std::error_code e; return fs::exists(p, e); });
    if (c != abl::MoveCheck::Ok) return false;
    return ExecuteRelocate(nullptr, {RelocateOp{src, destDir / src.filename()}});
}

bool AssetBrowserPanel::TestDelete(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    m_index.ForceRescan();
    return !ec;
}

// ============================================================ 取り込み（OS ドロップ / インポートボタン）

void AssetBrowserPanel::ProcessOsDrops(EditorContext& ctx)
{
    if (ctx.pendingOsDrops.empty()) return;
    if (m_importRunning) return;   // 前の取り込みが終わるまで持ち越す
    std::vector<EditorContext::OsFileDrop> drops = std::move(ctx.pendingOsDrops);
    ctx.pendingOsDrops.clear();
    if (ctx.isPlaying)
    {
        ctx.Notify(ui::ToastKind::Warn, "Play 中はアセットを取り込めません（停止してからドロップしてください）");
        return;
    }
    for (size_t i = 0; i < drops.size(); ++i)
    {
        if (i > 0 && m_importRunning)   // 1 度に 1 つ。残りは次のフレームで
        {
            ctx.pendingOsDrops.insert(ctx.pendingOsDrops.end(), drops.begin() + static_cast<ptrdiff_t>(i), drops.end());
            break;
        }
        const EditorContext::OsFileDrop& d = drops[i];
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        const float ix = mv->Pos.x + d.x, iy = mv->Pos.y + d.y;
        const bool inViewport = ctx.viewportW > 0.0f && ix >= ctx.viewportX && ix < ctx.viewportX + ctx.viewportW
                             && iy >= ctx.viewportY && iy < ctx.viewportY + ctx.viewportH;
        std::vector<std::filesystem::path> paths;
        for (const std::wstring& w : d.paths) paths.emplace_back(w);
        StartImport(ctx, std::move(paths), inViewport);
    }
}

void AssetBrowserPanel::StartImport(EditorContext& ctx, std::vector<std::filesystem::path> paths, bool placeInScene)
{
    if (paths.empty() || m_importRunning) return;
    std::filesystem::path dest = m_currentDir;
    if (dest.empty()) dest = m_assetsRoot;
    ctx.Notify(ui::ToastKind::Info, "取り込み中…");
    m_importRunning = true;
    m_import = std::async(std::launch::async, [paths = std::move(paths), dest, placeInScene]() -> ImportResult {
        namespace fs = std::filesystem;
        ImportResult r;
        r.placeInScene = placeInScene;
        std::vector<abl::ImportSource> srcs;
        for (const fs::path& abs : paths)
        {
            std::error_code ec;
            if (fs::is_directory(abs, ec))
            {
                // フォルダごと: フォルダ名を保って中身を取り込む（上限 5000 ファイル）
                size_t n = 0;
                for (fs::recursive_directory_iterator it(abs, ec), end; !ec && it != end; it.increment(ec))
                {
                    std::error_code e2;
                    if (!it->is_regular_file(e2)) continue;
                    if (++n > 5000) break;
                    fs::path rel = fs::relative(it->path().parent_path(), abs.parent_path(), e2);
                    srcs.push_back({it->path(), rel});
                }
            }
            else srcs.push_back({abs, {}});
        }
        const auto ops = abl::PlanImport(srcs, dest, [](const fs::path& p) { std::error_code e; return fs::exists(p, e); });
        for (const abl::CopyOp& op : ops)
        {
            if (op.skipped) { ++r.skipped; continue; }
            std::error_code ec;
            fs::create_directories(op.dst.parent_path(), ec);
            // 既にあれば上書きしない（スキップ）。原子的にコピーする
            const bool dstExists = fs::exists(op.dst, ec);
            const atomicfile::Result cr = dstExists ? atomicfile::Result{false, "同じ名前があります"} : atomicfile::CopyFileAtomic(op.src, op.dst, /*overwrite=*/false);
            if (cr.ok)
            {
                ++r.ok;
                r.created.push_back(op.dst);
            }
            else
            {
                ++r.failed;
                if (r.firstError.empty()) r.firstError = cr.error;
            }
        }
        return r;
    });
}

void AssetBrowserPanel::PollImport(EditorContext& ctx)
{
    if (!m_importRunning) return;
    if (m_import.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ImportResult r = m_import.get();
    m_importRunning = false;
    if (r.ok > 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%d 個のファイルを取り込みました", r.ok);
        ctx.Notify(ui::ToastKind::Success, msg);
        m_scrollToPath = r.created.front().string();
        m_index.ForceRescan();
        if (r.placeInScene)
        {
            int n = 0;
            for (const auto& f : r.created)
            {
                const Kind k = abl::ClassifyExtension(f.extension().string());
                if (k != Kind::Model && k != Kind::Prefab) continue;
                PendingSpawnRequest req;
                req.modelPath = f.string();
                req.position = SpawnPositionFrontOfCamera(ctx);
                req.position.x += 2.0f * static_cast<float>(n++);
                ctx.pendingSpawns.push_back(req);
            }
        }
    }
    if (r.skipped > 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%d 個は対象外の形式のため取り込みませんでした", r.skipped);
        ctx.Notify(ui::ToastKind::Warn, msg);
    }
    if (r.failed > 0)
        ctx.Notify(ui::ToastKind::Error, "取り込めなかったファイルがあります: " + r.firstError);
}

void AssetBrowserPanel::OpenImportDialog(EditorContext& ctx)
{
    if (dx12e::guard::Blocked("Asset import dialog")) return;   // 仮想入力 / UI テスト中はダイアログを出さない
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetActiveWindow();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"取り込むファイルを選ぶ";
    ofn.lpstrFilter = L"アセット\0*.png;*.jpg;*.jpeg;*.dds;*.tga;*.bmp;*.hdr;*.gltf;*.glb;*.fbx;*.obj;*.bin;*.wav;*.mp3;*.ogg;*.lua;*.hlsl;*.prefab;*.dxmat;*.dxmg;*.uianim;*.spranim\0すべてのファイル\0*.*\0";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&ofn)) return;

    std::vector<std::filesystem::path> out;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) out.emplace_back(first);   // 1 個だけ選ぶとフルパス
    else
    {
        while (*p)
        {
            out.push_back(std::filesystem::path(first) / std::wstring(p));
            p += wcslen(p) + 1;
        }
    }
    StartImport(ctx, std::move(out), false);
}

// ============================================================ モーダル（削除の確認 / 参照が切れる警告）

void AssetBrowserPanel::DrawModals(EditorContext& ctx)
{
    // ---- 削除確認（Del キー / 右クリック「削除」から。複数選択対応）----
    if (!m_pendingDelete.empty() && !m_deletePopupOpen)
    {
        ImGui::OpenPopup("##DeleteAssetConfirm");
        m_deletePopupOpen = true;
    }
    ImGui::SetNextWindowSize(ui::Px(400.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##DeleteAssetConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const size_t n = m_pendingDelete.size();
        if (n == 1)
            ImGui::Text("「%s」を削除しますか？", m_pendingDelete[0].filename().string().c_str());
        else
            ImGui::Text("選択した %zu 個を削除しますか？", n);

        bool anyDir = false, isCurrentScene = false;
        std::error_code ec;
        for (const auto& p : m_pendingDelete)
        {
            if (std::filesystem::is_directory(p, ec)) anyDir = true;
            if (!ctx.currentScenePath.empty() && abl::IsInside(std::filesystem::path(ctx.currentScenePath), p)) isCurrentScene = true;
        }
        if (anyDir)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, th::Warn);
            ImGui::TextUnformatted("フォルダ内のすべてが削除されます。");
            ImGui::PopStyleColor();
        }
        if (isCurrentScene)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, th::Warn);
            ImGui::TextUnformatted("※現在開いているシーンです。");
            ImGui::PopStyleColor();
        }
        // ごみ箱へ送るので OS 側から復元できる（エディタの Undo では戻らない）。
        ImGui::TextDisabled("ごみ箱へ移動します（エディタの Undo では戻りません）");
        ImGui::Separator();

        if (!m_deleteError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, th::Bad);
            ImGui::TextUnformatted(m_deleteError.c_str());
            ImGui::PopStyleColor();
        }

        if (ui::DangerButton("削除", ui::Px(120.0f, 0.0f)))
        {
            // ごみ箱へ。失敗しても完全削除へフォールバックしない（戻せない操作に化けるため）。
            int okCount = 0;
            std::string failed;
            for (const auto& p : m_pendingDelete)
            {
                if (MoveToRecycleBin(p))
                {
                    ++okCount;
                    Logger::Info("Asset moved to recycle bin: {}", p.string());
                    // ★ファイルが実際に消えた時だけ副作用を起こす（失敗したのに currentScenePath だけ空になる、を防ぐ）
                    if (!ctx.currentScenePath.empty() && abl::IsInside(std::filesystem::path(ctx.currentScenePath), p))
                        ctx.currentScenePath.clear();
                    m_sel.set.erase(p.string());
                }
                else
                {
                    Logger::Warn("アセットをごみ箱へ移動できませんでした: {}（削除していません）", p.string());
                    if (failed.empty()) failed = p.filename().string();
                }
            }
            m_index.ForceRescan();
            if (failed.empty())
            {
                if (okCount > 0) ctx.Notify(ui::ToastKind::Success, okCount == 1 ? "ごみ箱へ移動しました" : std::to_string(okCount) + " 個をごみ箱へ移動しました");
                m_pendingDelete.clear();
                m_deleteError.clear();
                m_deletePopupOpen = false;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                // 窓は閉じない。閉じると「押したのに消えていない」だけが残り、理由はログにしか出ない。
                m_deleteError = "ごみ箱へ移動できませんでした（他のアプリが開いている可能性）: " + failed + "。削除していません。";
                m_pendingDelete.erase(std::remove_if(m_pendingDelete.begin(), m_pendingDelete.end(),
                                      [](const std::filesystem::path& p) { std::error_code e; return !std::filesystem::exists(p, e); }),
                                      m_pendingDelete.end());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(120.0f, 0.0f)))
        {
            m_pendingDelete.clear();
            m_deleteError.clear();
            m_deletePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- 名前変更 / 移動で参照が切れる恐れがあるときの確認 ----
    if (m_relocate.open)
    {
        ImGui::OpenPopup("##RelocateConfirm");
        m_relocate.open = false;
        m_relocateOpen = true;
    }
    ImGui::SetNextWindowSize(ui::Px(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##RelocateConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const bool one = m_relocate.ops.size() == 1;
        if (one) ImGui::Text("「%s」を%sしますか？", m_relocate.ops[0].src.filename().string().c_str(), m_relocate.isRename ? "名前変更" : "移動");
        else ImGui::Text("%zu 個を移動しますか？", m_relocate.ops.size());
        ImGui::PushStyleColor(ImGuiCol_Text, th::Warn);
        ImGui::TextWrapped("参照しているファイルが %zu 件%s見つかりました。参照はパス文字列なので自動では更新されません（実行後に切れます）。",
                           m_relocate.refs.size(), m_relocate.refsTruncated ? "以上" : "");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        const size_t show = (std::min)(m_relocate.refs.size(), static_cast<size_t>(8));
        for (size_t i = 0; i < show; ++i) ImGui::BulletText("%s", m_relocate.refs[i].c_str());
        if (m_relocate.refs.size() > show) ImGui::TextDisabled("ほか %zu 件", m_relocate.refs.size() - show);
        ImGui::Separator();
        if (ui::DangerButton(m_relocate.isRename ? "名前変更する" : "移動する", ui::Px(140.0f, 0.0f)))
        {
            ExecuteRelocate(&ctx, m_relocate.ops);
            m_relocate = PendingRelocate{};
            m_relocateOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(120.0f, 0.0f)))
        {
            m_relocate = PendingRelocate{};
            m_relocateOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace dx12e
