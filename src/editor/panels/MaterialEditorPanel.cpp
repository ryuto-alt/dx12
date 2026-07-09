#include "editor/panels/MaterialEditorPanel.h"
#include "editor/EditorContext.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "resource/MaterialAssetManager.h"
#include "core/Logger.h"

#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

namespace dx12e
{

namespace
{
// AssetBrowserPanel/InspectorPanel と同じ絶対→assets相対の正規化(バックスラッシュ→スラッシュ、
// assetsDir プレフィックス除去)。ここでも独立実装が一番シンプルなので踏襲する。
std::string ToRelative(const std::string& absOrRelPath, const std::string& assetsDir)
{
    std::string abs  = fs::path(absOrRelPath).lexically_normal().string();
    std::string base = fs::path(assetsDir).lexically_normal().string();
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    if (abs.rfind(base, 0) == 0)
    {
        std::string rel = abs.substr(base.size());
        while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
        return rel;
    }
    return abs;  // 既に相対 or assetsDir 外
}

std::string Slugify(const std::string& name)
{
    std::string s;
    s.reserve(name.size());
    for (char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!s.empty() && s.back() != '_') s += '_';
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "material" : s;
}
}

void MaterialEditorPanel::Initialize(MaterialAssetManager* materialAssetManager, AssetBrowserPanel* assetBrowser)
{
    m_materialAssetManager = materialAssetManager;
    m_assetBrowser = assetBrowser;
}

void MaterialEditorPanel::NewAsset()
{
    m_current = MaterialAssetData{};
    m_current.metallic = 1.0f;
    m_current.roughness = 1.0f;
    m_current.uvTilingU = 1.0f;
    m_current.uvTilingV = 1.0f;
    m_current.name = "NewMaterial";
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());
}

bool MaterialEditorPanel::LoadAsset(const std::string& relPath, const std::string& assetsDir)
{
    std::ifstream ifs(assetsDir + relPath, std::ios::binary);
    if (!ifs) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    MaterialAssetData data;
    if (!ParseMaterialAsset(bytes, data)) return false;
    if (data.name.empty()) data.name = fs::path(relPath).stem().string();

    m_current = data;
    m_currentPath = relPath;
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());
    return true;
}

bool MaterialEditorPanel::SaveAsset(const std::string& assetsDir)
{
    m_current.name = m_nameBuf;
    if (m_currentPath.empty())
        m_currentPath = "materials/" + Slugify(m_current.name) + ".dxmat";

    std::error_code ec;
    fs::path fullPath(assetsDir + m_currentPath);
    fs::create_directories(fullPath.parent_path(), ec);

    std::ofstream ofs(fullPath, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        Logger::Warn("マテリアルアセットの保存に失敗しました: {}", fullPath.string());
        return false;
    }
    std::string json = SerializeMaterialAsset(m_current);
    ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
    ofs.close();

    if (m_materialAssetManager)
        m_materialAssetManager->Invalidate(m_currentPath);
    return true;
}

void MaterialEditorPanel::DrawTextureSlot(const std::string& assetsDir, const char* label, std::string& texRelPath)
{
    ImGui::PushID(label);
    constexpr float kThumbSize = 48.0f;

    bool hasTex = !texRelPath.empty();
    u64 gpuHandle = (hasTex && m_assetBrowser) ? m_assetBrowser->GetOrQueueThumbnail(assetsDir + texRelPath) : 0;

    ImGui::Text("%s", label);
    bool clicked;
    if (gpuHandle != 0)
        clicked = ImGui::ImageButton("##thumb", static_cast<ImTextureID>(gpuHandle), ImVec2(kThumbSize, kThumbSize));
    else
        clicked = ImGui::Button(hasTex ? "..." : "(none)", ImVec2(kThumbSize * 2.5f, kThumbSize));
    if (clicked)
        ImGui::OpenPopup("MatEditTexPicker");

    bool changed = false;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
        {
            const char* droppedPath = static_cast<const char*>(payload->Data);
            std::string ext = fs::path(droppedPath).extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
            {
                texRelPath = ToRelative(droppedPath, assetsDir);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (hasTex)
    {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 180.0f);
        ImGui::TextWrapped("%s", texRelPath.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("x")) { texRelPath.clear(); changed = true; }
        ImGui::EndGroup();
    }

    if (ImGui::BeginPopup("MatEditTexPicker"))
    {
        ImGui::TextDisabled("Select Texture");
        ImGui::Separator();
        if (ImGui::Selectable("(None)", !hasTex)) { texRelPath.clear(); changed = true; ImGui::CloseCurrentPopup(); }

        std::error_code ec;
        fs::path root(assetsDir);
        if (fs::exists(root, ec))
        {
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec) continue;
                std::string rowExt = it->path().extension().string();
                for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Texture) continue;

                fs::path relPath = fs::relative(it->path(), root, fec);
                if (fec) continue;
                std::string relStr = relPath.generic_string();
                if (ImGui::Selectable(relStr.c_str(), texRelPath == relStr))
                {
                    texRelPath = relStr;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();

    // テクスチャは離散的な割当操作なので、変更が確定した時点で即保存+Invalidate(SRV再構築)する。
    if (changed && !m_currentPath.empty())
        SaveAsset(assetsDir);
}

void MaterialEditorPanel::RenderWindow(EditorContext& ctx, const std::string& assetsDir)
{
    if (!ctx.pendingOpenMaterialPath.empty())
    {
        std::string rel = ToRelative(ctx.pendingOpenMaterialPath, assetsDir);
        ctx.pendingOpenMaterialPath.clear();
        LoadAsset(rel, assetsDir);
        ctx.showMaterialEditor = true;
    }

    if (!ctx.showMaterialEditor) return;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf###MaterialEditorFloating",  // マテリアルエディタ
                      &ctx.showMaterialEditor, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("\xe6\x96\xb0\xe8\xa6\x8f New")) NewAsset();  // 新規
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_currentPath.empty() ? "(unsaved)" : m_currentPath.c_str());

    ImGui::Separator();

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("\xe5\x90\x8d\xe5\x89\x8d Name", m_nameBuf, sizeof(m_nameBuf));  // 名前
    ImGui::SameLine();
    if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98 Save"))
    {
        if (SaveAsset(assetsDir))
        {
            m_statusMsg = "\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f: " + m_current.name;  // 保存しました:
            m_statusFlash = 2.0f;
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Textures");
    DrawTextureSlot(assetsDir, "Albedo", m_current.albedoPath);
    DrawTextureSlot(assetsDir, "Normal", m_current.normalPath);
    DrawTextureSlot(assetsDir, "MetalRoughness (ARM: G=Rough/B=Metal)", m_current.metalRoughnessPath);

    ImGui::Spacing();
    ImGui::TextDisabled("Scalars");

    // ドラッグ中は UpdateScalarsOnly で SRV を触らず即時プレビュー反映、指を離した瞬間にディスクへ保存する。
    auto liveSlider = [&](const char* label, f32* v, f32 lo, f32 hi)
    {
        bool dragging = ImGui::SliderFloat(label, v, lo, hi, "%.3f");
        if (dragging && m_materialAssetManager && !m_currentPath.empty())
        {
            m_materialAssetManager->UpdateScalarsOnly(m_currentPath, m_current.metallic, m_current.roughness,
                                                       m_current.uvTilingU, m_current.uvTilingV);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !m_currentPath.empty())
            SaveAsset(assetsDir);
    };
    liveSlider("\xe9\x87\x91\xe5\xb1\x9e\xe6\x84\x9f Metallic", &m_current.metallic, 0.0f, 1.0f);
    liveSlider("\xe7\xb2\x97\xe3\x81\x95 Roughness", &m_current.roughness, 0.0f, 1.0f);

    ImGui::Spacing();
    ImGui::TextDisabled("UV Tiling");
    liveSlider("U", &m_current.uvTilingU, 0.01f, 32.0f);
    liveSlider("V", &m_current.uvTilingV, 0.01f, 32.0f);

    if (m_currentPath.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
            "\xe6\x9c\xaa\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\xae\xe6\x96\xb0\xe8\xa6\x8f\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab"
            "\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82\xe5\x90\x8d\xe5\x89\x8d\xe3\x82\x92\xe4\xbb\x98\xe3\x81\x91\xe3\x81\xa6\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");
            // 未保存の新規マテリアルです。名前を付けて保存してください

    if (m_statusFlash > 0.0f)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
        m_statusFlash -= ImGui::GetIO().DeltaTime;
    }

    ImGui::End();
}

} // namespace dx12e
