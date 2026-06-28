#include "editor/panels/AssetBrowserPanel.h"
#include "editor/EditorContext.h"
#include "editor/ModelThumbnailRenderer.h"
#include "resource/ResourceManager.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "project/ProjectManager.h"
#include "scene/SceneSerializer.h"
#include "core/Logger.h"

#include <algorithm>
#include <fstream>
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

namespace
{
void OpenInVSCode(const std::string& filePath)
{
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
        HINSTANCE r = ShellExecuteA(nullptr, "open", cachedExe.c_str(), args.c_str(),
                                    nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32)
            return;  // 起動成功（> 32 が成功の規約）
    }

    // フォールバック: 拡張子の関連付けで開く
    ShellExecuteA(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // anonymous namespace

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

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
        Texture* tex = m_resourceManager->GetOrLoadTexture(
            path.wstring(), cmdList, true);
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
    default:                 return "File";
    }
}

// AssetType → 色 ヘルパー（file scope）
static ImVec4 AssetTypeColor(int type)
{
    switch (type)
    {
    case 0: return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);  // Folder
    case 1: return ImVec4(0.4f, 0.75f, 1.0f, 1.0f);  // Model
    case 2: return ImVec4(0.3f, 0.9f, 0.5f, 1.0f);   // Texture
    case 3: return ImVec4(1.0f, 0.55f, 0.25f, 1.0f);  // Scene
    case 4: return ImVec4(0.4f, 0.55f, 1.0f, 1.0f);   // Script
    case 5: return ImVec4(0.85f, 0.35f, 0.85f, 1.0f);  // Audio
    case 6: return ImVec4(0.55f, 0.85f, 0.95f, 1.0f);  // Prefab
    default: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

// AssetType → ベクターアイコン描画（アイコンフォント不要・任意サイズで鮮明）
static void DrawAssetGlyph(ImDrawList* dl, ImVec2 cardMin, float sz, int type,
                           const ImVec4& color, bool isUp)
{
    const float u  = sz;
    const float cx = cardMin.x + sz * 0.5f;
    const float cy = cardMin.y + sz * 0.5f;

    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto shade = [&](float m, float a) -> ImU32 {
        return IM_COL32(int(cl(color.x * m) * 255), int(cl(color.y * m) * 255),
                        int(cl(color.z * m) * 255), int(cl(a) * 255));
    };
    const ImU32 base  = shade(1.0f, 1.0f);
    const ImU32 light = shade(1.4f, 1.0f);
    const ImU32 dark  = shade(0.55f, 1.0f);

    auto V = [](float x, float y) { return ImVec2(x, y); };

    // 親フォルダ（..）は上矢印
    if (isUp)
    {
        dl->AddTriangleFilled(V(cx, cy - u * 0.24f), V(cx - u * 0.24f, cy + u * 0.02f),
                              V(cx + u * 0.24f, cy + u * 0.02f), base);
        dl->AddRectFilled(V(cx - u * 0.09f, cy), V(cx + u * 0.09f, cy + u * 0.24f), base, u * 0.03f);
        return;
    }

    switch (type)
    {
    case 0: // Folder
    {
        float x0 = cx - u * 0.30f, x1 = cx + u * 0.30f;
        float y0 = cy - u * 0.20f, y1 = cy + u * 0.22f;
        dl->AddRectFilled(V(x0, y0), V(x0 + u * 0.26f, y0 + u * 0.13f), dark, u * 0.04f,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(V(x0, y0 + u * 0.07f), V(x1, y1), base, u * 0.06f);
        dl->AddRectFilled(V(x0, y0 + u * 0.07f), V(x1, y0 + u * 0.12f), light, 0.0f); // 上端ハイライト
        break;
    }
    case 1: // Model（アイソメトリックキューブ）
    {
        float R = u * 0.30f, H = R * 0.58f;
        ImVec2 top = V(cx, cy - 2 * H), upR = V(cx + R, cy - H), loR = V(cx + R, cy + H);
        ImVec2 bot = V(cx, cy + 2 * H), loL = V(cx - R, cy + H), upL = V(cx - R, cy - H);
        ImVec2 mid = V(cx, cy);
        dl->AddQuadFilled(top, upR, mid, upL, light); // 上面
        dl->AddQuadFilled(upL, mid, bot, loL, base);  // 左面
        dl->AddQuadFilled(upR, loR, bot, mid, dark);  // 右面
        break;
    }
    case 2: // Texture（画像フレーム）
    {
        float x0 = cx - u * 0.30f, y0 = cy - u * 0.26f, x1 = cx + u * 0.30f, y1 = cy + u * 0.26f;
        dl->AddRectFilled(V(x0, y0), V(x1, y1), shade(0.30f, 1.0f), u * 0.05f);
        dl->AddCircleFilled(V(x0 + u * 0.16f, y0 + u * 0.15f), u * 0.07f, light);             // 太陽
        dl->AddTriangleFilled(V(x0, y1), V(x0 + u * 0.22f, cy), V(x0 + u * 0.44f, y1), base);  // 山
        dl->AddTriangleFilled(V(cx - u * 0.02f, y1), V(cx + u * 0.18f, cy + u * 0.04f), V(x1, y1), light);
        dl->AddRect(V(x0, y0), V(x1, y1), base, u * 0.05f, 0, 2.0f);                           // フレーム
        break;
    }
    case 3: // Scene（カチンコ / クラップボード）
    {
        float x0 = cx - u * 0.30f, x1 = cx + u * 0.30f;
        float ty0 = cy - u * 0.28f, ty1 = cy - u * 0.10f;  // 上: クラッパー棒
        float by1 = cy + u * 0.28f;                         // 下: スレート板

        // スレート板（本体）
        dl->AddRectFilled(V(x0, ty1), V(x1, by1), base, u * 0.04f);
        dl->AddRect(V(x0, ty1), V(x1, by1), shade(0.35f, 1.0f), u * 0.04f, 0, 1.5f);
        // 板の罫線（記入欄に見立て）
        for (int i = 0; i < 2; ++i)
        {
            float ly = ty1 + u * 0.10f + i * u * 0.11f;
            dl->AddLine(V(x0 + u * 0.05f, ly), V(x1 - u * 0.05f, ly), shade(0.4f, 0.85f), 1.5f);
        }

        // クラッパー棒（上）＋斜めストライプ
        dl->AddRectFilled(V(x0, ty0), V(x1, ty1), dark, u * 0.03f);
        float sw = (x1 - x0) / 5.0f, sk = u * 0.05f;
        for (int k = 1; k < 5; k += 2)
        {
            float sx = x0 + k * sw;
            dl->AddQuadFilled(V(sx, ty1), V(sx + sw, ty1),
                              V(sx + sw + sk, ty0), V(sx + sk, ty0), light);
        }
        break;
    }
    case 4: // Script（コードページ </>）
    {
        float x0 = cx - u * 0.26f, y0 = cy - u * 0.28f, x1 = cx + u * 0.26f, y1 = cy + u * 0.28f;
        dl->AddRectFilled(V(x0, y0), V(x1, y1), IM_COL32(236, 239, 245, 255), u * 0.05f);
        dl->AddRect(V(x0, y0), V(x1, y1), base, u * 0.05f, 0, 1.5f);
        float th = sz * 0.045f; if (th < 1.8f) th = 1.8f;
        dl->AddLine(V(cx - u * 0.04f, cy - u * 0.11f), V(cx - u * 0.15f, cy), base, th); // <
        dl->AddLine(V(cx - u * 0.15f, cy), V(cx - u * 0.04f, cy + u * 0.11f), base, th);
        dl->AddLine(V(cx + u * 0.04f, cy - u * 0.11f), V(cx + u * 0.15f, cy), base, th); // >
        dl->AddLine(V(cx + u * 0.15f, cy), V(cx + u * 0.04f, cy + u * 0.11f), base, th);
        break;
    }
    case 5: // Audio（8分音符）
    {
        float th = sz * 0.05f; if (th < 2.0f) th = 2.0f;
        ImVec2 head = V(cx - u * 0.12f, cy + u * 0.20f);
        float hr = u * 0.11f;
        dl->AddCircleFilled(head, hr, base);
        ImVec2 stemTop = V(cx + u * 0.14f, cy - u * 0.26f);
        dl->AddLine(V(head.x + hr - 1.0f, head.y), stemTop, base, th);
        dl->AddLine(stemTop, V(cx + u * 0.26f, cy - u * 0.12f), base, th); // 旗
        break;
    }
    default: // Other（書類）
    {
        float x0 = cx - u * 0.24f, y0 = cy - u * 0.28f, x1 = cx + u * 0.24f, y1 = cy + u * 0.28f;
        float fold = u * 0.13f;
        dl->AddRectFilled(V(x0, y0), V(x1, y1), IM_COL32(226, 229, 236, 255), u * 0.04f);
        dl->AddTriangleFilled(V(x1 - fold, y0), V(x1, y0), V(x1, y0 + fold), IM_COL32(170, 175, 185, 255));
        dl->AddRect(V(x0, y0), V(x1, y1), base, u * 0.04f, 0, 1.5f);
        for (int i = 0; i < 3; ++i)
        {
            float ly = cy - u * 0.05f + i * u * 0.10f;
            dl->AddLine(V(x0 + u * 0.06f, ly), V(x1 - u * 0.06f, ly), IM_COL32(150, 153, 163, 255), 1.5f);
        }
        break;
    }
    }
}

// プレビュー画像にボーダー＋タイプ色のコーナーバッジを重ねる（直前の Image アイテム基準）
static void DecoratePreview(ImVec2 mn, float sz, const ImVec4& typeColor)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mx = ImVec2(mn.x + sz, mn.y + sz);
    bool hov = ImGui::IsItemHovered();
    ImU32 border = hov ? ImGui::GetColorU32(ImVec4(typeColor.x, typeColor.y, typeColor.z, 1.0f))
                       : IM_COL32(0, 0, 0, 140);
    dl->AddRect(mn, mx, border, 4.0f, 0, hov ? 2.5f : 1.0f);
    float t = sz * 0.28f;
    dl->AddTriangleFilled(mn, ImVec2(mn.x + t, mn.y), ImVec2(mn.x, mn.y + t),
                          ImGui::GetColorU32(typeColor));
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

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
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

        // フォルダアイコン色
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        ImGui::PopStyleColor();

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

    // ===== 上部: フィルタタブ + サイズスライダー =====
    {
        const char* filterNames[] = {"All", "3D Models", "Scenes", "Textures", "Scripts", "Audio"};
        for (int i = 0; i < 6; ++i)
        {
            if (i > 0) ImGui::SameLine();
            bool active = (m_filterIndex == i);
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            if (ImGui::SmallButton(filterNames[i]))
                m_filterIndex = i;
            if (active)
                ImGui::PopStyleColor();
        }

        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("##Size", &m_cellSize, 48.0f, 192.0f, "%.0f");
    }

    ImGui::Separator();

    // ===== 2カラムレイアウト: 左=フォルダツリー | 右=ファイルグリッド =====
    float treeWidth = 160.0f;
    ImGui::BeginChild("##FolderTree", ImVec2(treeWidth, 0), true);
    {
        // assets ルート
        bool assetsSelected = (m_currentDir == m_assetsRoot);
        if (assetsSelected)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
        if (ImGui::Selectable("assets##ShortcutRoot", assetsSelected))
        {
            m_currentDir = m_assetsRoot;
            needRefresh = true;
        }
        if (assetsSelected)
            ImGui::PopStyleColor();

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
        ImGui::PushStyleColor(ImGuiCol_Text,
            scriptsSelected ? ImVec4(1.0f, 1.0f, 0.4f, 1.0f) : ImVec4(0.4f, 0.55f, 1.0f, 1.0f));
        if (ImGui::Selectable("scripts##ShortcutScripts", scriptsSelected))
        {
            m_currentDir = m_scriptsRoot;
            needRefresh = true;
        }
        ImGui::PopStyleColor();

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
                        ImGui::SameLine();
                        ImGui::TextDisabled("/");
                        ImGui::SameLine();
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

                    // カード背景（ホバーで明るく）
                    dl->AddRectFilled(cardMin, cardMax,
                        hovered ? IM_COL32(60, 62, 72, 255) : IM_COL32(38, 40, 46, 255), 6.0f);

                    // タイプ色ボーダー（視認性アップ・ホバーで強調）
                    ImU32 borderCol = ImGui::GetColorU32(
                        ImVec4(typeColor.x, typeColor.y, typeColor.z, hovered ? 1.0f : 0.55f));
                    dl->AddRect(cardMin, cardMax, borderCol, 6.0f, 0, hovered ? 2.5f : 1.5f);

                    // ベクターアイコン（中央）
                    bool isUp = (entry.displayName == "..");
                    DrawAssetGlyph(dl, cardMin, thumbnailSize, static_cast<int>(entry.type), typeColor, isUp);

                    // 拡張子バッジ（下部・タイプ色）
                    if (!entry.isDirectory)
                    {
                        std::string ext = entry.path.extension().string();
                        if (!ext.empty())
                        {
                            ImVec2 ts = ImGui::CalcTextSize(ext.c_str());
                            ImVec2 bMin = ImVec2(cardMin.x + (thumbnailSize - ts.x) * 0.5f - 4.0f,
                                                 cardMax.y - ts.y - 5.0f);
                            ImVec2 bMax = ImVec2(bMin.x + ts.x + 8.0f, bMin.y + ts.y + 2.0f);
                            dl->AddRectFilled(bMin, bMax,
                                ImGui::GetColorU32(ImVec4(typeColor.x * 0.5f, typeColor.y * 0.5f,
                                                          typeColor.z * 0.5f, 0.9f)), 3.0f);
                            dl->AddText(ImVec2(bMin.x + 4.0f, bMin.y + 1.0f),
                                IM_COL32(235, 235, 235, 255), ext.c_str());
                        }
                    }

                    // InvisibleButton でクリック判定
                    ImGui::InvisibleButton("##card", ImVec2(thumbnailSize, thumbnailSize));
                }

                // --- ドラッグ&ドロップソース ---
                if (!entry.isDirectory &&
                    (entry.type == AssetType::Model || entry.type == AssetType::Texture ||
                     entry.type == AssetType::Script || entry.type == AssetType::Prefab))
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
                        ImGui::EndDragDropSource();
                    }
                }

                // --- ファイル名 ---
                {
                    std::string name = entry.displayName;
                    float textWidth = ImGui::CalcTextSize(name.c_str()).x;
                    float maxWidth = m_cellSize - 4.0f;
                    if (textWidth > maxWidth)
                    {
                        while (name.size() > 4 && ImGui::CalcTextSize((name + "..").c_str()).x > maxWidth)
                            name.pop_back();
                        name += "..";
                    }
                    float nameW = ImGui::CalcTextSize(name.c_str()).x;
                    float offset = (m_cellSize - nameW) * 0.5f;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                    ImGui::TextWrapped("%s", name.c_str());
                }

                ImGui::EndGroup();

                // --- 単一クリックで選択（Del キー削除の対象になる）---
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    m_selectedPath = entry.path;
                // 選択中はアクセント枠でハイライト
                if (!m_selectedPath.empty() && m_selectedPath == entry.path)
                    ImGui::GetWindowDrawList()->AddRect(
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        IM_COL32(76, 141, 255, 255), 4.0f, 0, 2.0f);

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
                        // JSON は Lua と同様 VS Code で開く（読み込みは右クリックメニューから）
                        OpenInVSCode(entry.path.string());
                    }
                    else if (entry.type == AssetType::Prefab)
                    {
                        PendingSpawnRequest req;
                        req.modelPath = entry.path.string();
                        req.position = {0.0f, 0.0f, 0.0f};
                        ctx.pendingSpawns.push_back(req);
                    }
                    else if (entry.type == AssetType::Script)
                    {
                        OpenInVSCode(entry.path.string());
                    }
                }

                // --- 右クリックコンテキストメニュー（アイテム単位）---
                char ctxId[32];
                snprintf(ctxId, sizeof(ctxId), "##ctx_%d", static_cast<int>(i));
                if (ImGui::BeginPopupContextItem(ctxId))
                {
                    if (entry.type == AssetType::Script)
                    {
                        if (ImGui::MenuItem("VS Code \xe3\x81\xa7\xe9\x96\x8b\xe3\x81\x8f"))  // VS Code で開く
                            OpenInVSCode(entry.path.string());
                    }
                    else if (entry.type == AssetType::Model)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\xab\xe8\xbf\xbd\xe5\x8a\xa0"))  // シーンに追加
                        {
                            PendingSpawnRequest req;
                            req.modelPath = entry.path.string();
                            ctx.pendingSpawns.push_back(req);
                        }
                    }
                    else if (entry.type == AssetType::Scene)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x81\xbf"))  // シーンを読み込み
                            ctx.pendingLoadPath = entry.path.string();
                    }
                    else if (entry.type == AssetType::Prefab)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\xab\xe8\xbf\xbd\xe5\x8a\xa0"))  // シーンに追加
                        {
                            PendingSpawnRequest req;
                            req.modelPath = entry.path.string();
                            ctx.pendingSpawns.push_back(req);
                        }
                    }
                    if (entry.isDirectory)
                    {
                        if (ImGui::MenuItem("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x97\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xa9\xe3\x83\xbc\xe3\x81\xa7\xe9\x96\x8b\xe3\x81\x8f"))  // エクスプローラーで開く
                            ShellExecuteA(nullptr, "explore", entry.path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
                        ImGui::TextDisabled("Double-click: Spawn | Drag: D&D to scene");
                    else if (entry.type == AssetType::Scene)
                        ImGui::TextDisabled("Double-click: Load scene");
                    else if (entry.type == AssetType::Script)
                        ImGui::TextDisabled("Double-click: Open in VS Code");
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Del キー: このパネルにフォーカスがあり選択中のアセットがあれば削除確認へ
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
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

            ImGui::Separator();

            // フォルダを開く
            if (ImGui::MenuItem("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x97\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xa9\xe3\x83\xbc\xe3\x81\xa7\xe9\x96\x8b\xe3\x81\x8f"))  // エクスプローラーで開く
            {
                ShellExecuteA(nullptr, "explore", m_currentDir.string().c_str(),
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
        ImGui::TextDisabled("\xe3\x81\x93\xe3\x81\xae\xe6\x93\x8d\xe4\xbd\x9c\xe3\x81\xaf\xe5\x85\x83\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x9b\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82");  // この操作は元に戻せません。
        ImGui::Separator();

        if (ImGui::Button("\xe5\x89\x8a\xe9\x99\xa4", ImVec2(120, 0)))  // 削除
        {
            std::error_code ec;
            if (isDir) std::filesystem::remove_all(m_pendingDeletePath, ec);
            else       std::filesystem::remove(m_pendingDeletePath, ec);
            if (ec) Logger::Warn("Asset delete failed: {} ({})", m_pendingDeletePath.string(), ec.message());
            else    Logger::Info("Asset deleted: {}", m_pendingDeletePath.string());
            if (isCurrentScene) ctx.currentScenePath.clear();
            if (m_selectedPath == m_pendingDeletePath) m_selectedPath.clear();
            m_pendingDeletePath.clear();
            m_deletePopupOpen = false;
            needRefresh = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
        {
            m_pendingDeletePath.clear();
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

    if (!std::filesystem::exists(m_currentDir))
    {
        m_currentDir = m_assetsRoot;
        if (!std::filesystem::exists(m_currentDir))
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
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
        return AssetType::Audio;
    if (ext == ".prefab")
        return AssetType::Prefab;
    return AssetType::Other;
}

} // namespace dx12e
