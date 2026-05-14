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

    RenderLetterboxedImage(gameViewTextureId, avail);

    (void)isPlaying;  // Editor 中もカメラ視点を表示するため、overlay は出さない

    ImGui::End();
}

void GameViewPanel::RenderLetterboxedImage(u64 gameViewTextureId, ImVec2 avail)
{
    // 16:9 固定 (Unity の Aspect Drop-down と同じ思想)
    constexpr f32 kAspect = 16.0f / 9.0f;
    f32 fitW, fitH;
    if (avail.x / avail.y > kAspect)
    {
        fitH = avail.y;
        fitW = fitH * kAspect;
    }
    else
    {
        fitW = avail.x;
        fitH = fitW / kAspect;
    }

    const f32 padX = (avail.x - fitW) * 0.5f;
    const f32 padY = (avail.y - fitH) * 0.5f;

    // 黒帯 (レターボックス/ピラーボックス) は端の 2 矩形だけ塗る
    // (フル画面 fill だと Maximize on Play 時に全ピクセルを 2 回塗ることになる)
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImU32 black = IM_COL32(0, 0, 0, 255);
    if (padY > 0.0f)
    {
        // 上下
        dl->AddRectFilled(origin,
            ImVec2(origin.x + avail.x, origin.y + padY), black);
        dl->AddRectFilled(ImVec2(origin.x, origin.y + avail.y - padY),
            ImVec2(origin.x + avail.x, origin.y + avail.y), black);
    }
    if (padX > 0.0f)
    {
        // 左右
        dl->AddRectFilled(ImVec2(origin.x, origin.y + padY),
            ImVec2(origin.x + padX, origin.y + avail.y - padY), black);
        dl->AddRectFilled(ImVec2(origin.x + avail.x - padX, origin.y + padY),
            ImVec2(origin.x + avail.x, origin.y + avail.y - padY), black);
    }

    // レターボックス内に Image を中央配置
    const ImVec2 origCursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(origCursor.x + padX, origCursor.y + padY));

    if (gameViewTextureId != 0)
    {
        // ImGui DX12 backend では ImTextureID = D3D12_GPU_DESCRIPTOR_HANDLE.ptr (u64)
        ImGui::Image(static_cast<ImTextureID>(gameViewTextureId), ImVec2(fitW, fitH));
    }
    else
    {
        ImGui::Dummy(ImVec2(fitW, fitH));
    }
}

} // namespace dx12e
