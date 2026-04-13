#include "editor/panels/AssetBrowserPanel.h"
#include "editor/EditorContext.h"
#include "resource/ResourceManager.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"

#include <algorithm>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
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

    // 1フレームあたり最大3枚までロード（フレーム落ち防止）
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

    // --- サイズスライダー + Breadcrumb ---
    ImGui::SliderFloat("Size", &m_cellSize, 48.0f, 192.0f, "%.0f");
    ImGui::SameLine();

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
                    ImGui::TextDisabled(">");
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

    // --- ファイルグリッド ---
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columns = (std::max)(1, static_cast<int>(panelWidth / (m_cellSize + 8.0f)));

    auto entries = m_entries;

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

            // --- サムネイル / アイコン ---
            bool hasPreview = false;

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
                    // ロードキューに追加（次フレームでロードされる）
                    m_pendingThumbnailLoads.push_back(key);
                    // 重複防止のため仮エントリを入れとく
                    m_thumbnailCache[key] = ThumbnailInfo{};
                }
            }

            if (!hasPreview)
            {
                const char* icon = "[?]";
                ImVec4 iconColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                switch (entry.type)
                {
                case AssetType::Folder:  icon = "[DIR]";    iconColor = ImVec4(0.9f, 0.8f, 0.3f, 1.0f); break;
                case AssetType::Model:   icon = "[MODEL]";  iconColor = ImVec4(0.4f, 0.7f, 1.0f, 1.0f); break;
                case AssetType::Texture: icon = "[TEX]";    iconColor = ImVec4(0.3f, 0.9f, 0.5f, 1.0f); break;
                case AssetType::Scene:   icon = "[SCENE]";  iconColor = ImVec4(0.9f, 0.5f, 0.3f, 1.0f); break;
                case AssetType::Script:  icon = "[LUA]";    iconColor = ImVec4(0.3f, 0.5f, 0.9f, 1.0f); break;
                case AssetType::Audio:   icon = "[SND]";    iconColor = ImVec4(0.8f, 0.3f, 0.8f, 1.0f); break;
                default: break;
                }

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, iconColor);
                ImGui::Button(icon, ImVec2(thumbnailSize, thumbnailSize));
                ImGui::PopStyleColor(2);
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
                if (textWidth > m_cellSize - 4.0f)
                {
                    while (name.size() > 4 && ImGui::CalcTextSize((name + "..").c_str()).x > m_cellSize - 4.0f)
                        name.pop_back();
                    name += "..";
                }
                ImGui::TextWrapped("%s", name.c_str());
            }

            ImGui::EndGroup();

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s", entry.path.filename().string().c_str());
                if (entry.type == AssetType::Model)
                    ImGui::TextDisabled("Drag to scene");
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
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
