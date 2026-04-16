#include "editor/panels/GameViewPanel.h"

namespace dx12e
{

void GameViewPanel::Render(bool isPlaying, u64 gameViewTextureId)
{
    // タイトルは "ゲーム" (UTF-8 エスケープ)
    constexpr const char* kTitle = "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0";

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(kTitle, nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (!open)
    {
        m_isHovered = false;
        ImGui::End();
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f) avail.x = 1.0f;
    if (avail.y < 1.0f) avail.y = 1.0f;
    m_contentSize = avail;
    m_isHovered   = ImGui::IsWindowHovered();

    if (gameViewTextureId != 0)
    {
        // ImGui DX12 backend では ImTextureID = D3D12_GPU_DESCRIPTOR_HANDLE.ptr (u64)
        ImGui::Image(static_cast<ImTextureID>(gameViewTextureId), avail);
    }
    else
    {
        ImGui::Dummy(avail);
    }

    (void)isPlaying;  // Editor 中もカメラ視点を表示するため、overlay は出さない

    ImGui::End();
}

} // namespace dx12e
