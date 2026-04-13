#include "editor/panels/AssetBrowserPanel.h"
#include "editor/EditorContext.h"
#include "editor/ModelThumbnailRenderer.h"
#include "resource/ResourceManager.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"

#include <algorithm>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

void AssetBrowserPanel::Initialize(const std::string& assetsDir,
                                    ResourceManager* resourceManager,
                                    DescriptorHeap* srvHeap)
{
    m_assetsRoot       = std::filesystem::path(assetsDir);
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
    case AssetType::Folder:  return "DIR";
    case AssetType::Model:   return "3D";
    case AssetType::Texture: return "TEX";
    case AssetType::Scene:   return "SCN";
    case AssetType::Script:  return "LUA";
    case AssetType::Audio:   return "SND";
    default:                 return "?";
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
    default: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
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
        // ルートボタン
        bool rootSelected = (m_currentDir == m_assetsRoot);
        if (rootSelected)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
        if (ImGui::Selectable("assets", rootSelected))
        {
            m_currentDir = m_assetsRoot;
            needRefresh = true;
        }
        if (rootSelected)
            ImGui::PopStyleColor();

        DrawFolderTree(m_assetsRoot, needRefresh);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右ペイン: ファイルグリッド =====
    ImGui::BeginChild("##FileGrid", ImVec2(0, 0));
    {
        // Breadcrumb
        {
            if (ImGui::SmallButton("assets"))
            {
                m_currentDir = m_assetsRoot;
                needRefresh = true;
            }

            if (std::filesystem::exists(m_currentDir) && std::filesystem::exists(m_assetsRoot))
            {
                std::error_code ec;
                auto relative = std::filesystem::relative(m_currentDir, m_assetsRoot, ec);
                if (!ec && relative != "." && !relative.empty())
                {
                    auto current = m_assetsRoot;
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
                        ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
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
                        ImGui::Image(texId, ImVec2(thumbnailSize, thumbnailSize));
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
                    const char* icon = GetTypeIcon(entry.type);

                    // カード背景 + 色付きアクセントバー
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    // 背景
                    ImVec2 cardMin = pos;
                    ImVec2 cardMax = ImVec2(pos.x + thumbnailSize, pos.y + thumbnailSize);
                    dl->AddRectFilled(cardMin, cardMax,
                        IM_COL32(40, 40, 45, 255), 4.0f);

                    // 上部アクセントバー
                    ImVec2 barMax = ImVec2(pos.x + thumbnailSize, pos.y + 3.0f);
                    dl->AddRectFilled(cardMin, barMax,
                        ImGui::GetColorU32(typeColor), 4.0f, ImDrawFlags_RoundCornersTop);

                    // アイコンテキスト（中央）
                    ImVec2 textSize = ImGui::CalcTextSize(icon);
                    ImVec2 textPos = ImVec2(
                        pos.x + (thumbnailSize - textSize.x) * 0.5f,
                        pos.y + (thumbnailSize - textSize.y) * 0.5f);
                    dl->AddText(textPos, ImGui::GetColorU32(typeColor), icon);

                    // 拡張子を下部に小さく表示
                    if (!entry.isDirectory)
                    {
                        std::string ext = entry.path.extension().string();
                        ImVec2 extSize = ImGui::CalcTextSize(ext.c_str());
                        ImVec2 extPos = ImVec2(
                            pos.x + (thumbnailSize - extSize.x) * 0.5f,
                            pos.y + thumbnailSize - extSize.y - 4.0f);
                        dl->AddText(extPos, IM_COL32(160, 160, 160, 200), ext.c_str());
                    }

                    // InvisibleButton でクリック判定
                    ImGui::InvisibleButton("##card", ImVec2(thumbnailSize, thumbnailSize));
                }

                // --- クリック ---
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
                        Logger::Info("Queued spawn: {}", entry.path.string());
                    }
                    else if (entry.type == AssetType::Scene)
                    {
                        ctx.pendingLoadPath = entry.path.string();
                        Logger::Info("Queued scene load: {}", entry.path.string());
                    }
                }

                // --- ドラッグ&ドロップソース ---
                if (!entry.isDirectory && (entry.type == AssetType::Model || entry.type == AssetType::Texture))
                {
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        std::string pathStr = entry.path.string();
                        ImGui::SetDragDropPayload(kDragDropPayloadType,
                            pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("%s", entry.displayName.c_str());
                        if (entry.type == AssetType::Model)
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
                    // ファイル名を中央寄せ
                    float nameW = ImGui::CalcTextSize(name.c_str()).x;
                    float offset = (m_cellSize - nameW) * 0.5f;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                    ImGui::TextWrapped("%s", name.c_str());
                }

                ImGui::EndGroup();

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
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

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

    if (m_currentDir != m_assetsRoot)
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
    return AssetType::Other;
}

} // namespace dx12e
