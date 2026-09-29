#include "editor/UiWidgets.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "core/VirtualGuard.h"   // 仮想入力モード中は ShellExecute / ダイアログを実行しない
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/ModelThumbnailRenderer.h"
#include "editor/panels/MaterialPreviewRenderer.h"
#include "resource/ResourceManager.h"
#include "resource/MaterialAssetIO.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "project/ProjectManager.h"
#include "scene/SceneSerializer.h"
#include "core/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

namespace
{
// アセットをごみ箱へ送る（完全削除しない）。
// 以前は remove_all/remove で即座に消していたので、間違って消したら復旧手段が無かった。
// アセットの参照元を調べる仕組みがまだ無い（何がこのテクスチャを使っているか分からない）以上、
// 「消す前に確認」だけでは足りず「消した後に戻せる」が要る。
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
        // ShellExecute はシェル経由でエディタから独立して起動するため、VSCode が自分の
        // ライフサイクルを正しく管理でき、閉じた後でも開き直せる。
        std::string args = "\"" + filePath + "\"";
        HINSTANCE r = dx12e::guard::ShellExecuteGuarded(nullptr, "open", cachedExe.c_str(), args.c_str(),
                                    nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32)
            return;  // 起動成功（> 32 が成功の規約）
    }

    // フォールバック: 拡張子の関連付けで開く
    dx12e::guard::ShellExecuteGuarded(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // anonymous namespace

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

// サムネイルの長辺上限[px]。セルは最大でも 128px 程度なので 256 で足りる。
// ponytail: 固定値。セルサイズに追従させたいなら m_cellSize から求める（キャッシュキーが
//           増えるので、素材を等倍と縮小の 2 通り持つことになる点に注意）。
constexpr u32 kThumbnailMaxDim = 256;

void AssetBrowserPanel::Initialize(const std::string& assetsDir,
                                    const std::string& scriptsDir,
                                    ResourceManager* resourceManager,
                                    DescriptorHeap* srvHeap)
{
    m_assetsRoot       = std::filesystem::path(assetsDir);
    m_scriptsRoot      = std::filesystem::path(scriptsDir);
    m_currentDir       = m_assetsRoot;
    m_resourceManager  = resourceManager;
    m_srvHeap          = srvHeap;
    Refresh();
}

void AssetBrowserPanel::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    if (m_pendingThumbnailLoads.empty() || !m_resourceManager || !cmdList)
        return;

    constexpr size_t kMaxLoadsPerFrame = 3;
    size_t count = (std::min)(m_pendingThumbnailLoads.size(), kMaxLoadsPerFrame);

    for (size_t i = 0; i < count; ++i)
    {
        const auto& pathStr = m_pendingThumbnailLoads[i];
        std::filesystem::path path(pathStr);

        ThumbnailInfo info;
        // ★サムネイルは 256px 上限で読む。80px のセルに 4K テクスチャを等倍で載せると、
        //   1 枚 85MB(mip 込み)がキャッシュに残り続ける（ResourceManager にエビクションは無い）。
        //   assets/textures を一度スクロールしただけで VRAM が数 GB 飛び、以後そのセッションの
        //   ずっと戻らない。ログも警告も出ないまま「なぜか重い」になる。
        Texture* tex = m_resourceManager->GetOrLoadTexture(
            path.wstring(), cmdList, true, TextureUsage::Unknown, kThumbnailMaxDim);
        if (tex && tex->GetSrvIndex() != UINT32_MAX)
        {
            auto gpuHandle = m_srvHeap->GetGpuHandle(tex->GetSrvIndex());
            info.gpuHandle = gpuHandle.ptr;
            info.loaded = true;
        }
        else
        {
            info.failed = true;
        }
        m_thumbnailCache[pathStr] = info;
    }

    m_pendingThumbnailLoads.erase(
        m_pendingThumbnailLoads.begin(),
        m_pendingThumbnailLoads.begin() + static_cast<ptrdiff_t>(count));
}

u64 AssetBrowserPanel::GetOrQueueThumbnail(const std::string& absPath)
{
    auto it = m_thumbnailCache.find(absPath);
    if (it != m_thumbnailCache.end())
        return (it->second.loaded && !it->second.failed) ? it->second.gpuHandle : 0;

    // グリッド表示(Render内)と同じキュー+プレースホルダ方式。LoadPendingThumbnailsが
    // 次フレーム以降に実ロードする(呼び出し側は0が返る間は「読み込み中」として扱うこと)。
    m_pendingThumbnailLoads.push_back(absPath);
    m_thumbnailCache[absPath] = ThumbnailInfo{};
    return 0;
}

const char* AssetBrowserPanel::GetTypeIcon(AssetType type)
{
    switch (type)
    {
    case AssetType::Folder:  return "Folder";
    case AssetType::Model:   return "Model";
    case AssetType::Texture: return "Texture";
    case AssetType::Scene:   return "Scene";
    case AssetType::Script:  return "Script";
    case AssetType::Audio:   return "Audio";
    case AssetType::Prefab:  return "Prefab";
    case AssetType::Shader:  return "Shader";
    case AssetType::Material: return "Material";
    case AssetType::UiAnim:  return "UiAnim";
    case AssetType::SpriteSheet: return "SpriteSheet";
    default:                 return "File";
    }
}

// AssetType → 種別カラー（file scope）。テーマの「控えめな色味」に揃える: フォルダは中性グレー、
// それ以外は彩度を落とした種別色（旧: 高彩度の黄/水色/緑/橙/青/紫/桃で画面がうるさかった）。
static ImVec4 AssetTypeColor(int type)
{
    namespace th = dx12e::theme;
    switch (type)
    {
    case 0: return th::TypeFolder;               // Folder
    case 1: return th::Hex(0x8FB4E8);            // Model
    case 2: return th::Hex(0x7CC49A);            // Texture
    case 3: return th::TypeScene;                // Scene
    case 4: return th::TypeScript;               // Script
    case 5: return th::TypeAudio;                // Audio
    case 6: return th::TypePrefab;               // Prefab
    case 7: return th::Hex(0xC08AE6);            // Shader
    case 8: return th::TypeLight;                // Material（琥珀）
    case 9: return th::TypeUi;                   // UiAnim（アニメ系はマゼンタ寄りで統一）
    case 10: return th::Hex(0xE59C7A);           // SpriteSheet
    default: return th::TypeEmpty;
    }
}

// AssetType → アイコングリフ（Lucide。旧: DrawList で描いた立体アイコン。線画に統一）
static const char* AssetTypeGlyph(int type)
{
    switch (type)
    {
    case 0:  return ICON_FOLDER;
    case 1:  return ICON_T_MESH;
    case 2:  return ICON_IMAGE;
    case 3:  return ICON_FILM;
    case 4:  return ICON_FILE_CODE;
    case 5:  return ICON_MUSIC;
    case 6:  return ICON_PACKAGE;
    case 7:  return ICON_T_SHADER;
    case 8:  return ICON_T_MATERIAL;
    case 9:  return ICON_T_ANIM;
    case 10: return ICON_T_GRID;
    default: return ICON_FILE;
    }
}

// AssetType → アイコン描画（線画グリフを中央へ。カードの大きさに合わせて拡縮）
static void DrawAssetGlyph(ImDrawList* dl, ImVec2 cardMin, float sz, int type,
                           const ImVec4& color, bool isUp)
{
    const ImVec2 c(cardMin.x + sz * 0.5f, cardMin.y + sz * 0.5f - sz * 0.03f);
    const float px = (std::max)(16.0f, sz * 0.62f);
    dx12e::ui::DrawIconCentered(dl, isUp ? ICON_ARROW_UP : AssetTypeGlyph(type), c,
                                ImGui::GetColorU32(color), px);
}

// プレビュー画像に 1px の枠と、下辺の細い種別カラー帯を重ねる（直前の Image アイテム基準。UE のコンテンツブラウザ風）
static void DecoratePreview(ImVec2 mn, float sz, const ImVec4& typeColor)
{
    namespace th = dx12e::theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mx = ImVec2(mn.x + sz, mn.y + sz);
    const bool hov = ImGui::IsItemHovered();
    dl->AddRect(mn, mx, ImGui::GetColorU32(hov ? th::BorderStrong : th::Border), 2.0f, 0, 1.0f);
    dl->AddRectFilled(ImVec2(mn.x + 1.0f, mx.y - 3.0f), ImVec2(mx.x - 1.0f, mx.y - 1.0f),
                      ImGui::GetColorU32(th::WithAlpha(typeColor, 0.9f)));
}

// ===== フォルダツリー（再帰描画）=====
void AssetBrowserPanel::DrawFolderTree(const std::filesystem::path& dir, bool& needRefresh)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    // サブフォルダを収集
    std::vector<std::filesystem::path> subDirs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (entry.is_directory(ec))
            subDirs.push_back(entry.path());
    }
    std::sort(subDirs.begin(), subDirs.end());

    for (const auto& subDir : subDirs)
    {
        std::string name = subDir.filename().string();
        if (name[0] == '.') continue; // .thumbcache 等の隠しフォルダをスキップ
        bool isSelected = (m_currentDir == subDir);

        // 行全体（左端〜右端）を選択面にし、chevron・フォルダアイコン・名前は自前で描く
        // （標準の矢印は巨大な三角で UE 風に合わせられない。判定・開閉は TreeNodeEx が担う）。
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;

        // サブフォルダがあるか簡易チェック
        bool hasSubDirs = false;
        {
            std::error_code ec2;
            for (const auto& sub : std::filesystem::directory_iterator(subDir, ec2))
            {
                if (ec2) break;
                if (sub.is_directory(ec2)) { hasSubDirs = true; break; }
            }
        }
        if (!hasSubDirs) flags |= ImGuiTreeNodeFlags_Leaf;

        namespace th = dx12e::theme;
        const float rowX = ImGui::GetCursorScreenPos().x;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));   // 標準の矢印と文字は隠す
        bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        {
            ImDrawList* tdl = ImGui::GetWindowDrawList();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            const float cy = (mn.y + mx.y) * 0.5f;
            const bool hov = ImGui::IsItemHovered();
            if (isSelected)
                tdl->AddRectFilled(mn, ImVec2(mn.x + 2.0f, mx.y), ImGui::GetColorU32(th::Accent));
            if (hasSubDirs)
                dx12e::ui::DrawIconCentered(tdl, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                                            ImVec2(rowX + 4.0f + ImGui::GetFontSize() * 0.5f, cy),
                                            ImGui::GetColorU32(hov || isSelected ? th::Text : th::TextDim), 13.0f);
            const float tx = rowX + ImGui::GetFontSize() + 8.0f;
            dx12e::ui::DrawIconCentered(tdl, open && hasSubDirs ? ICON_FOLDER_OPEN : ICON_FOLDER,
                                        ImVec2(tx + 8.0f, cy), ImGui::GetColorU32(th::TypeFolder), 16.0f);
            tdl->AddText(ImVec2(tx + 22.0f, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f + 0.5f)),
                         ImGui::GetColorU32(th::Text), name.c_str());
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            m_currentDir = subDir;
            needRefresh = true;
        }

        if (open)
        {
            DrawFolderTree(subDir, needRefresh);
            ImGui::TreePop();
        }
    }
}

void AssetBrowserPanel::Render(EditorContext& ctx, f32 dt)
{
    m_refreshTimer += dt;
    if (m_refreshTimer >= kRefreshInterval)
    {
        m_refreshTimer = 0.0f;
        Refresh();
    }

    ImGui::Begin("\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6");  // Asset Browser

    bool needRefresh = false;

    // ===== 上部: 検索 + 種別フィルタ + サイズスライダー =====
    {
        // 検索が一番よく使うので左端・幅広に置く（フォルダを掘らずに名前で辿り着ける）
        ImGui::SetNextItemWidth(220);
        if (ui::SearchField("##Search", m_searchBuf, sizeof(m_searchBuf), "検索（このフォルダ以下）"))
            needRefresh = true;

        ImGui::SameLine(0, 12);
        const char* filterNames[] = {"All", "3D Models", "Scenes", "Textures", "Scripts", "Audio", "Materials"};
        // フィルタは平たい「ピル」。選択中だけ面（アクセント 30%）を敷き、他は文字だけ（色付きボタンの羅列にしない）。
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        for (int i = 0; i < 7; ++i)
        {
            if (i > 0) ImGui::SameLine(0, 2);
            bool active = (m_filterIndex == i);
            ImGui::PushStyleColor(ImGuiCol_Button,
                active ? dx12e::theme::Selection : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                active ? dx12e::theme::SelectionActive : dx12e::theme::Bg3);
            ImGui::PushStyleColor(ImGuiCol_Text,
                active ? dx12e::theme::Text : dx12e::theme::TextDim);
            if (ImGui::SmallButton(filterNames[i]))
                m_filterIndex = i;
            ImGui::PopStyleColor(3);
        }
        ImGui::PopStyleVar(2);

        // サイズスライダーは右端へ（普段触らないものが左にあると検索/フィルタが探しにくい）
        const float sliderW = 110.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - sliderW);
        ImGui::SetNextItemWidth(sliderW);
        ui::SliderFloat("##Size", &m_cellSize, 56.0f, 192.0f, "%.0f px");
    }

    ImGui::Separator();

    // ===== 2カラムレイアウト: 左=フォルダツリー | 右=ファイルグリッド =====
    float treeWidth = 160.0f;
    ImGui::BeginChild("##FolderTree", ImVec2(treeWidth, 0), true);
    {
        // assets ルート
        bool assetsSelected = (m_currentDir == m_assetsRoot);
        if (ImGui::Selectable("assets##ShortcutRoot", assetsSelected))
        {
            m_currentDir = m_assetsRoot;
            needRefresh = true;
        }

        DrawFolderTree(m_assetsRoot, needRefresh);

        ImGui::Separator();

        // scripts ルート
        bool scriptsSelected = false;
        {
            namespace fs = std::filesystem;
            // m_currentDir が scripts 配下かチェック
            std::error_code ec;
            auto rel = fs::relative(m_currentDir, m_scriptsRoot, ec);
            scriptsSelected = !ec && !rel.empty() && rel.native()[0] != '.';
            if (m_currentDir == m_scriptsRoot) scriptsSelected = true;
        }
        if (ImGui::Selectable("scripts##ShortcutScripts", scriptsSelected))
        {
            m_currentDir = m_scriptsRoot;
            needRefresh = true;
        }

        if (std::filesystem::exists(m_scriptsRoot))
            DrawFolderTree(m_scriptsRoot, needRefresh);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右ペイン: ファイルグリッド =====
    ImGui::BeginChild("##FileGrid", ImVec2(0, 0));
    {
        // Breadcrumb
        {
            // 現在のディレクトリが assets 配下か scripts 配下か判定
            namespace fs = std::filesystem;
            bool inScripts = false;
            {
                std::error_code ec;
                auto rel = fs::relative(m_currentDir, m_scriptsRoot, ec);
                if (!ec && !rel.empty() && rel.native()[0] != '.')
                    inScripts = true;
                if (m_currentDir == m_scriptsRoot) inScripts = true;
            }

            fs::path rootDir = inScripts ? m_scriptsRoot : m_assetsRoot;
            const char* rootLabel = inScripts ? "scripts" : "assets";

            // パンくずは平たい文字ボタン（面はホバーだけ）。区切りは薄い chevron。
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextMid);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            if (ImGui::SmallButton(rootLabel))
            {
                m_currentDir = rootDir;
                needRefresh = true;
            }

            if (fs::exists(m_currentDir) && fs::exists(rootDir))
            {
                std::error_code ec;
                auto relative = fs::relative(m_currentDir, rootDir, ec);
                if (!ec && relative != "." && !relative.empty())
                {
                    auto current = rootDir;
                    for (const auto& part : relative)
                    {
                        current /= part;
                        ImGui::SameLine(0, 2);
                        ImGui::TextDisabled("%s", ICON_CHEVRON_RIGHT);
                        ImGui::SameLine(0, 2);
                        std::string partStr = part.string();
                        ImGui::PushID(current.string().c_str());
                        if (ImGui::SmallButton(partStr.c_str()))
                        {
                            m_currentDir = current;
                            needRefresh = true;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            // 検索中はそれを明示（今見ているのがフォルダの中身ではないと分かるように）
            if (m_searchBuf[0] != '\0')
            {
                ImGui::SameLine(0, 10);
                ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::AccentLight);
                ImGui::Text("\"%s\" の検索結果 %zu 件%s",
                            m_searchBuf, m_entries.size(), m_searchTruncated ? "+" : "");
                ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        // ファイルグリッド
        float gridWidth = ImGui::GetContentRegionAvail().x;
        int columns = (std::max)(1, static_cast<int>(gridWidth / (m_cellSize + 8.0f)));

        // フィルタ適用
        auto entries = m_entries;
        if (m_filterIndex > 0)
        {
            AssetType filterType = AssetType::Other;
            switch (m_filterIndex)
            {
            case 1: filterType = AssetType::Model;   break;
            case 2: filterType = AssetType::Scene;    break;
            case 3: filterType = AssetType::Texture;  break;
            case 4: filterType = AssetType::Script;   break;
            case 5: filterType = AssetType::Audio;    break;
            case 6: filterType = AssetType::Material; break;
            }
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [filterType](const AssetEntry& e) {
                        return !e.isDirectory && e.type != filterType;
                    }),
                entries.end());
        }

        float thumbnailSize = m_cellSize - 24.0f;
        if (thumbnailSize < 16.0f) thumbnailSize = 16.0f;

        if (ImGui::BeginTable("AssetGrid", columns))
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const auto& entry = entries[i];
                ImGui::TableNextColumn();

                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginGroup();

                // --- サムネイル / アイコンカード ---
                bool hasPreview = false;

                // テクスチャプレビュー
                if (entry.type == AssetType::Texture && !entry.isDirectory)
                {
                    std::string key = entry.path.string();
                    auto it = m_thumbnailCache.find(key);
                    if (it != m_thumbnailCache.end() && it->second.loaded && it->second.gpuHandle != 0)
                    {
                        ImTextureID texId = static_cast<ImTextureID>(it->second.gpuHandle);
                        ImVec2 pv = ImGui::GetCursorScreenPos();
                        ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
                        DecoratePreview(pv, thumbnailSize, AssetTypeColor(static_cast<int>(entry.type)));
                        hasPreview = true;
                    }
                    else if (it == m_thumbnailCache.end())
                    {
                        m_pendingThumbnailLoads.push_back(key);
                        m_thumbnailCache[key] = ThumbnailInfo{};
                    }
                }

                // マテリアルプレビュー: 球体サムネイル(MaterialPreviewRenderer)を優先。
                // 生成待ちの間・レンダラー不在時は従来の albedo 平面サムネで代用する。
                if (entry.type == AssetType::Material && !entry.isDirectory)
                {
                    u64 sphereHandle = m_materialPreview
                        ? m_materialPreview->GetOrQueueThumbnail(entry.path.string()) : 0;
                    if (sphereHandle != 0)
                    {
                        ImTextureID texId = static_cast<ImTextureID>(sphereHandle);
                        ImVec2 pv = ImGui::GetCursorScreenPos();
                        ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
                        DecoratePreview(pv, thumbnailSize, AssetTypeColor(static_cast<int>(entry.type)));
                        hasPreview = true;
                    }
                    else if (!entry.materialThumbSource.empty())
                    {
                        const std::string& key = entry.materialThumbSource;
                        auto it = m_thumbnailCache.find(key);
                        if (it != m_thumbnailCache.end() && it->second.loaded && it->second.gpuHandle != 0)
                        {
                            ImTextureID texId = static_cast<ImTextureID>(it->second.gpuHandle);
                            ImVec2 pv = ImGui::GetCursorScreenPos();
                            ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
                            DecoratePreview(pv, thumbnailSize, AssetTypeColor(static_cast<int>(entry.type)));
                            hasPreview = true;
                        }
                        else if (it == m_thumbnailCache.end())
                        {
                            m_pendingThumbnailLoads.push_back(key);
                            m_thumbnailCache[key] = ThumbnailInfo{};
                        }
                    }
                }

                // 3Dモデルプレビュー
                if (entry.type == AssetType::Model && !entry.isDirectory && m_thumbRenderer)
                {
                    std::string key = entry.path.string();
                    u64 handle = m_thumbRenderer->GetCachedHandle(key);
                    if (handle != 0)
                    {
                        ImTextureID texId = static_cast<ImTextureID>(handle);
                        ImVec2 pv = ImGui::GetCursorScreenPos();
                        ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
                        DecoratePreview(pv, thumbnailSize, AssetTypeColor(static_cast<int>(entry.type)));
                        hasPreview = true;
                    }
                    else
                    {
                        m_thumbRenderer->Request(key);
                    }
                }

                if (!hasPreview)
                {
                    ImVec4 typeColor = AssetTypeColor(static_cast<int>(entry.type));
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 cardMin = pos;
                    ImVec2 cardMax = ImVec2(pos.x + thumbnailSize, pos.y + thumbnailSize);

                    bool hovered = ImGui::IsMouseHoveringRect(cardMin, cardMax);

                    // カード背景（Bg2。ホバーで Bg3）+ 1px の枠。種別は下辺の細い色帯で示す
                    // （旧: 種別色の太い枠。色が画面中に散ってうるさかった）。
                    namespace th = dx12e::theme;
                    dl->AddRectFilled(cardMin, cardMax,
                        ImGui::GetColorU32(hovered ? th::Bg3 : th::Bg2), 3.0f);
                    dl->AddRect(cardMin, cardMax,
                        ImGui::GetColorU32(hovered ? th::BorderStrong : th::Border), 3.0f, 0, 1.0f);
                    dl->AddRectFilled(ImVec2(cardMin.x + 1.0f, cardMax.y - 3.0f), ImVec2(cardMax.x - 1.0f, cardMax.y - 1.0f),
                        ImGui::GetColorU32(th::WithAlpha(typeColor, entry.isDirectory ? 0.0f : 0.85f)));

                    // ベクターアイコン（中央）
                    bool isUp = (entry.displayName == "..");
                    DrawAssetGlyph(dl, cardMin, thumbnailSize, static_cast<int>(entry.type), typeColor, isUp);

                    // 拡張子（下部。控えめな中性ピル）
                    if (!entry.isDirectory)
                    {
                        std::string ext = entry.path.extension().string();
                        if (!ext.empty())
                        {
                            dx12e::ui::PushMono();
                            ImVec2 ts = ImGui::CalcTextSize(ext.c_str());
                            ImVec2 bMin = ImVec2(cardMin.x + (thumbnailSize - ts.x) * 0.5f - 4.0f,
                                                 cardMax.y - ts.y - 8.0f);
                            ImVec2 bMax = ImVec2(bMin.x + ts.x + 8.0f, bMin.y + ts.y + 2.0f);
                            dl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(th::WithAlpha(th::Bg0, 0.75f)), 3.0f);
                            dl->AddText(ImVec2(bMin.x + 4.0f, bMin.y + 1.0f),
                                ImGui::GetColorU32(th::TextDim), ext.c_str());
                            dx12e::ui::PopMono();
                        }
                    }

                    // InvisibleButton でクリック判定
                    ImGui::InvisibleButton("##card", ImVec2(thumbnailSize, thumbnailSize));
                }

                // --- ドラッグ&ドロップソース ---
                // ★Shader(.hlsl) もドラッグできる。以前は一覧に出るだけで掴めず、
                //   カスタムシェーダーの割り当ては Inspector のコンボから探すしかなかった。
                if (!entry.isDirectory &&
                    (entry.type == AssetType::Model || entry.type == AssetType::Texture ||
                     entry.type == AssetType::Script || entry.type == AssetType::Prefab ||
                     entry.type == AssetType::Material || entry.type == AssetType::UiAnim ||
                     entry.type == AssetType::SpriteSheet || entry.type == AssetType::Shader))
                {
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string pathStr = entry.path.string();
                        const char* payloadId = (entry.type == AssetType::Script) ? "DND_SCRIPT" : kDragDropPayloadType;
                        ImGui::SetDragDropPayload(payloadId,
                            pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("%s", entry.displayName.c_str());
                        if (entry.type == AssetType::Model || entry.type == AssetType::Prefab)
                            ImGui::TextDisabled("Drop to Scene");
                        else if (entry.type == AssetType::Shader)
                            ImGui::TextDisabled("Drop to MeshRenderer / Sprite2D / Camera");
                        ImGui::EndDragDropSource();
                    }
                }

                // --- ファイル名（必ず1行。折り返すとセルの高さが揃わず、グリッドが
                //     ガタついて目が滑る。全文はツールチップで読める）---
                {
                    std::string name = entry.displayName;
                    const float maxWidth = thumbnailSize;
                    if (ImGui::CalcTextSize(name.c_str()).x > maxWidth)
                    {
                        while (name.size() > 2 && ImGui::CalcTextSize((name + "…").c_str()).x > maxWidth)
                            name.pop_back();
                        name += "…";
                    }
                    const float nameW  = ImGui::CalcTextSize(name.c_str()).x;
                    const float offset = (thumbnailSize - nameW) * 0.5f;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                    // フォルダは白、ファイルは少し落として「入れ物」と「中身」を見分けやすくする
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        entry.isDirectory ? dx12e::theme::Text : dx12e::theme::TextMid);
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::EndGroup();

                // --- 単一クリックで選択（Del キー削除の対象になる）---
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    m_selectedPath = entry.path;
                // 選択中はアクセント枠でハイライト
                if (!m_selectedPath.empty() && m_selectedPath == entry.path)
                    ImGui::GetWindowDrawList()->AddRect(
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        ImGui::GetColorU32(dx12e::theme::Accent), 3.0f, 0, 2.0f);

                // --- ダブルクリック（EndGroup 後 = グループ全体のホバー判定）---
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    if (entry.isDirectory)
                    {
                        m_currentDir = entry.path;
                        needRefresh = true;
                    }
                    else if (entry.type == AssetType::Model)
                    {
                        PendingSpawnRequest req;
                        req.modelPath = entry.path.string();
                        req.position = {0.0f, 0.0f, 0.0f};
                        ctx.pendingSpawns.push_back(req);
                    }
                    else if (entry.type == AssetType::Scene)
                    {
                        // ダブルクリックでシーン切り替え（VS Code で開くのは右クリック「開く」から）
                        ctx.pendingLoadPath = entry.path.string();
                    }
                    else if (entry.type == AssetType::Prefab)
                    {
                        PendingSpawnRequest req;
                        req.modelPath = entry.path.string();
                        req.position = {0.0f, 0.0f, 0.0f};
                        ctx.pendingSpawns.push_back(req);
                    }
                    else if (entry.type == AssetType::Script || entry.type == AssetType::Shader)
                    {
                        OpenInVSCode(entry.path.string());
                    }
                    else if (entry.type == AssetType::Material)
                    {
                        ctx.pendingOpenMaterialPath = entry.path.string();
                        ctx.showMaterialEditor = true;
                    }
                    else if (entry.type == AssetType::UiAnim)
                    {
                        ctx.pendingOpenUiAnimPath = entry.path.string();
                        ctx.showAnimEditor = true;
                    }
                    else if (entry.type == AssetType::SpriteSheet)
                    {
                        ctx.pendingOpenSpriteSheetPath = entry.path.string();
                        ctx.showSpriteSheetEditor = true;
                    }
                }

                // --- 右クリックコンテキストメニュー（アイテム単位）---
                char ctxId[32];
                snprintf(ctxId, sizeof(ctxId), "##ctx_%d", static_cast<int>(i));
                if (ImGui::BeginPopupContextItem(ctxId))
                {
                    // 主操作（種別ごと）: シーン=読み込み、モデル/プレハブ=シーンに追加
                    if (entry.type == AssetType::Scene)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x81\xbf"))  // シーンを読み込み
                            ctx.pendingLoadPath = entry.path.string();
                    }
                    else if (entry.type == AssetType::Model || entry.type == AssetType::Prefab)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\xab\xe8\xbf\xbd\xe5\x8a\xa0"))  // シーンに追加
                        {
                            PendingSpawnRequest req;
                            req.modelPath = entry.path.string();
                            ctx.pendingSpawns.push_back(req);
                        }
                    }

                    // 開く（全ファイル共通。シーン/スクリプトは VS Code、その他は OS 既定アプリ）
                    if (!entry.isDirectory)
                    {
                        if (ImGui::MenuItem("\xe9\x96\x8b\xe3\x81\x8f"))  // 開く
                        {
                            if (entry.type == AssetType::Scene || entry.type == AssetType::Script ||
                                entry.type == AssetType::Shader)
                                OpenInVSCode(entry.path.string());
                            else
                                dx12e::guard::ShellExecuteGuarded(nullptr, "open", entry.path.string().c_str(),
                                              nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    }
                    else
                    {
                        if (ImGui::MenuItem("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x97\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xa9\xe3\x83\xbc\xe3\x81\xa7\xe9\x96\x8b\xe3\x81\x8f"))  // エクスプローラーで開く
                            dx12e::guard::ShellExecuteGuarded(nullptr, "explore", entry.path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    // 削除（シーン/スクリプト含む全アセット・フォルダ。確認ダイアログ経由）
                    ImGui::Separator();
                    if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4"))  // 削除
                    {
                        m_selectedPath     = entry.path;
                        m_pendingDeletePath = entry.path;
                    }
                    ImGui::EndPopup();
                }

                // ツールチップ
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", entry.path.filename().string().c_str());
                    const char* typeLabel = GetTypeIcon(entry.type);
                    ImGui::PushStyleColor(ImGuiCol_Text, AssetTypeColor(static_cast<int>(entry.type)));
                    ImGui::Text("[%s]", typeLabel);
                    ImGui::PopStyleColor();
                    if (entry.type == AssetType::Model)
                        ImGui::TextDisabled("ダブルクリック: シーンに配置 / ドラッグ: シーンへドロップ");
                    else if (entry.type == AssetType::Scene)
                        ImGui::TextDisabled("ダブルクリック: シーンを開く");
                    else if (entry.type == AssetType::Script || entry.type == AssetType::Shader)
                        ImGui::TextDisabled("ダブルクリック: VS Code で開く");
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Del キー: このパネルにフォーカスがあり選択中のアセットがあれば削除確認へ。
        // ★検索欄で文字を消すための Del では反応しない（WantTextInput）。ヒエラルキー / ビューポートの
        //   Del（エンティティ削除）は「フォーカスのあるパネルだけ」で効くので、ここと同時には発火しない。
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::GetIO().WantTextInput
            && !m_selectedPath.empty()
            && std::filesystem::exists(m_selectedPath)
            && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            m_pendingDeletePath = m_selectedPath;
        }

        // ===== 右クリックコンテキストメニュー =====
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("##AssetContextMenu");
        }

        if (ImGui::BeginPopup("##AssetContextMenu"))
        {
            // 新規シーン
            if (ImGui::MenuItem("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3"))  // 新規シーン
            {
                ctx.showNewSceneDialog = true;
                ctx.newSceneDialogIsCreate = true;
                std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
                strncpy_s(ctx.newSceneNameBuf, "NewScene", _TRUNCATE);
            }

            // 新規スクリプト
            if (ImGui::MenuItem("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88"))  // 新規スクリプト
            {
                ctx.showNewScriptDialog = true;
                std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
                strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
            }

            // 新規シェーダー
            if (ImGui::MenuItem("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc"))  // 新規シェーダー
            {
                ctx.showNewShaderDialog = true;
                std::memset(ctx.newShaderNameBuf, 0, sizeof(ctx.newShaderNameBuf));
                strncpy_s(ctx.newShaderNameBuf, "NewShader", _TRUNCATE);
            }

            ImGui::Separator();

            // フォルダを開く
            if (ImGui::MenuItem("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x97\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xa9\xe3\x83\xbc\xe3\x81\xa7\xe9\x96\x8b\xe3\x81\x8f"))  // エクスプローラーで開く
            {
                dx12e::guard::ShellExecuteGuarded(nullptr, "explore", m_currentDir.string().c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // ===== 削除確認モーダル（Del キー / 右クリック「削除」から）=====
    if (!m_pendingDeletePath.empty() && !m_deletePopupOpen)
    {
        ImGui::OpenPopup("##DeleteAssetConfirm");
        m_deletePopupOpen = true;
    }
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##DeleteAssetConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const bool isDir = std::filesystem::is_directory(m_pendingDeletePath);
        ImGui::Text("\xe3\x80\x8c%s\xe3\x80\x8d\xe3\x82\x92\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b\xef\xbc\x9f",  // 「%s」を削除しますか？
                    m_pendingDeletePath.filename().string().c_str());

        // 現在開いているシーンかどうか
        bool isCurrentScene = false;
        {
            std::error_code ec;
            if (!ctx.currentScenePath.empty())
                isCurrentScene =
                    std::filesystem::weakly_canonical(std::filesystem::path(ctx.currentScenePath), ec) ==
                    std::filesystem::weakly_canonical(m_pendingDeletePath, ec);
        }
        if (isDir)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                "\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe5\x86\x85\xe3\x81\xae\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe3\x81\x8c\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");  // フォルダ内のすべてが削除されます。
        if (isCurrentScene)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "\xe2\x80\xbb\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82");  // ※現在開いているシーンです。
        // ごみ箱へ送るので OS 側から復元できる（エディタの Undo では戻らない）。
        ImGui::TextDisabled("\xe3\x81\x94\xe3\x81\xbf\xe7\xae\xb1\xe3\x81\xb8\xe7\xa7\xbb\xe5\x8b\x95\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xef\xbc\x88\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf\xe3\x81\xae Undo \xe3\x81\xa7\xe3\x81\xaf\xe6\x88\xbb\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xef\xbc\x89");  // ごみ箱へ移動します（エディタの Undo では戻りません）
        ImGui::Separator();

        // 失敗した理由を窓に出しっぱなしにする（ログだけだと気づかない）
        if (!m_deleteError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_deleteError.c_str());

        if (ImGui::Button("\xe5\x89\x8a\xe9\x99\xa4", ImVec2(120, 0)))  // 削除
        {
            // ごみ箱へ。失敗しても完全削除へフォールバックしない（戻せない操作に化けるため）。
            const bool moved = MoveToRecycleBin(m_pendingDeletePath);
            if (moved)
                Logger::Info("Asset moved to recycle bin: {}", m_pendingDeletePath.string());
            else
                Logger::Warn("アセットをごみ箱へ移動できませんでした: {}（削除していません）",
                             m_pendingDeletePath.string());

            // ★以下は以前 moved を見ずに走っていた。ファイルが残ったまま currentScenePath
            //   だけ空になると、エディタは「開いているシーンが無い」状態になり、次の Ctrl+S が
            //   実在するシーンファイルへ書かなくなる＝削除は失敗したのに破壊的な副作用だけ残る。
            if (moved)
            {
                if (isCurrentScene) ctx.currentScenePath.clear();
                if (m_selectedPath == m_pendingDeletePath) m_selectedPath.clear();
                m_pendingDeletePath.clear();
                m_deleteError.clear();
                m_deletePopupOpen = false;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                // 窓は閉じない。閉じると「押したのに消えていない」だけが残り、
                // 理由はログにしか出ない。
                m_deleteError = "\xe3\x81\x94\xe3\x81\xbf\xe7\xae\xb1\xe3\x81\xb8\xe7\xa7\xbb\xe5\x8b\x95\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f\xef\xbc\x88\xe4\xbb\x96\xe3\x81\xae\xe3\x82\xa2\xe3\x83\x97\xe3\x83\xaa\xe3\x81\x8c\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe5\x8f\xaf\xe8\x83\xbd\xe6\x80\xa7\xef\xbc\x89\xe3\x80\x82\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82";
            }
            needRefresh = true;   // 成否どちらでも実際の状態を出し直す
        }
        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
        {
            m_pendingDeletePath.clear();
            m_deleteError.clear();
            m_deletePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();

    if (needRefresh)
        Refresh();
}

void AssetBrowserPanel::Refresh()
{
    m_entries.clear();
    m_searchTruncated = false;

    if (!std::filesystem::exists(m_currentDir))
    {
        m_currentDir = m_assetsRoot;
        if (!std::filesystem::exists(m_currentDir))
            return;
    }

    // ---- 検索中: 現在フォルダ以下を再帰的に探す（フォルダを掘らずに辿り着けるように）----
    // 件数上限つき。0.5 秒ごとの Refresh で巨大ツリーを毎回全走査しないための保険。
    if (m_searchBuf[0] != '\0')
    {
        namespace fs = std::filesystem;
        std::string needle = m_searchBuf;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::error_code ec;
        // ponytail: 走査数にも上限を置く。ヒット数だけの制限だと「1件も一致しない巨大ツリー」で
        // 0.5 秒ごとに全走査してしまう。足りなくなったら差分監視に置き換える。
        size_t visited = 0;
        constexpr size_t kMaxVisit = 20000;
        for (fs::recursive_directory_iterator it(m_currentDir, ec), end; it != end; it.increment(ec))
        {
            if (ec) break;
            if (++visited > kMaxVisit) { m_searchTruncated = true; break; }
            const auto& p = it->path();
            std::string fileName = p.filename().string();
            if (!fileName.empty() && fileName[0] == '.')
            {
                if (it->is_directory(ec)) it.disable_recursion_pending();   // .thumbcache 等は覗かない
                continue;
            }
            std::string lower = fileName;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find(needle) == std::string::npos) continue;

            if (m_entries.size() >= kMaxSearchHits) { m_searchTruncated = true; break; }

            AssetEntry entry;
            entry.path        = p;
            entry.displayName = fileName;
            entry.isDirectory = it->is_directory(ec);
            entry.type = entry.isDirectory ? AssetType::Folder
                                           : ClassifyExtension(p.extension().string());
            m_entries.push_back(entry);
        }
        std::sort(m_entries.begin(), m_entries.end(),
            [](const AssetEntry& a, const AssetEntry& b) {
                if (a.isDirectory != b.isDirectory) return a.isDirectory;   // フォルダ優先
                return a.displayName < b.displayName;
            });
        return;
    }

    if (m_currentDir != m_assetsRoot && m_currentDir != m_scriptsRoot)
    {
        AssetEntry parent;
        parent.path = m_currentDir.parent_path();
        parent.displayName = "..";
        parent.type = AssetType::Folder;
        parent.isDirectory = true;
        m_entries.push_back(parent);
    }

    std::vector<AssetEntry> dirs;
    std::vector<AssetEntry> files;

    std::error_code ec;
    for (const auto& dirEntry : std::filesystem::directory_iterator(m_currentDir, ec))
    {
        if (ec) break;

        AssetEntry entry;
        entry.path = dirEntry.path();
        entry.displayName = dirEntry.path().filename().string();
        entry.isDirectory = dirEntry.is_directory(ec);

        if (entry.isDirectory)
        {
            if (entry.displayName[0] == '.') continue; // 隠しフォルダ除外
            entry.type = AssetType::Folder;
            dirs.push_back(entry);
        }
        else
        {
            entry.type = ClassifyExtension(dirEntry.path().extension().string());
            if (entry.type == AssetType::Material)
            {
                // サムネイルは .dxmat の albedo テクスチャを使い回す(軽量な同期読み込み、
                // JSON数百バイト程度なので Refresh の頻度でも問題にならない)。
                std::ifstream ifs(entry.path, std::ios::binary);
                if (ifs)
                {
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    MaterialAssetData data;
                    if (ParseMaterialAsset(bytes, data) && !data.albedoPath.empty())
                    {
                        std::filesystem::path abs = m_assetsRoot / data.albedoPath;
                        if (std::filesystem::exists(abs))
                            entry.materialThumbSource = abs.string();
                    }
                }
            }
            files.push_back(entry);
        }
    }

    auto sortFn = [](const AssetEntry& a, const AssetEntry& b) {
        return a.displayName < b.displayName;
    };
    std::sort(dirs.begin(), dirs.end(), sortFn);
    std::sort(files.begin(), files.end(), sortFn);

    m_entries.insert(m_entries.end(), dirs.begin(), dirs.end());
    m_entries.insert(m_entries.end(), files.begin(), files.end());
}

AssetBrowserPanel::AssetType AssetBrowserPanel::ClassifyExtension(const std::string& ext)
{
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj")
        return AssetType::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || ext == ".tga" || ext == ".bmp")
        return AssetType::Texture;
    if (ext == ".json")
        return AssetType::Scene;
    if (ext == ".lua")
        return AssetType::Script;
    if (ext == ".hlsl" || ext == ".hlsli")
        return AssetType::Shader;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
        return AssetType::Audio;
    if (ext == ".prefab")
        return AssetType::Prefab;
    if (ext == ".dxmat")
        return AssetType::Material;
    if (ext == ".uianim")
        return AssetType::UiAnim;
    if (ext == ".spranim")
        return AssetType::SpriteSheet;
    return AssetType::Other;
}

} // namespace dx12e
