#pragma once
// dx12_perceive 1 回ぶんの状態（受付 → 数フレーム落ち着かせる → ID パスを記録 → 読み戻して集計 → 応答）。
// Application は std::unique_ptr<McpPerceiveJob> で持つ（null = 受け付けていない）。
// 流れと各段の担当:
//   受付        … ApplicationMcpPerceive.cpp の McpDefine("perceive")。カメラを退避して指定視点へ
//   Settling    … 決定論キャプチャと同じ仕組みで N フレーム回す（時間・ジッタ・フォグの位相を固定）
//   Pending     … Run ループの決定論ブロックが立てる。次の Render が記録する
//   Captured    … Application::RecordPerceptionIds（ApplicationRender.cpp）が最終画のコピー・ID パス・
//                 被覆マスクを同じコマンドリストへ積んだ
//   応答        … Application::FinishPerception が読み戻して perception::Analyze → JSON → 後始末
#include "core/Types.h"
#include "core/mcp/McpDeferred.h"
#include "renderer/PerceptionStats.h"

#include <DirectXMath.h>
#include <entt/entt.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace dx12e
{

struct McpPerceiveJob
{
    enum class Phase { Settling, Pending, Captured };
    enum class Cam   { Current, Explicit, Game };

    McpDeferred reply;
    Phase phase      = Phase::Settling;
    int   waitFrames = 0;   // Pending のまま撮れなかったフレーム数（窓の最小化などで Render が素通りした時の安全網）

    // ---- カメラ ----
    Cam               camMode = Cam::Current;
    DirectX::XMFLOAT3 camPos{}, camTarget{};
    float             fovDeg = 0.0f;   // 0 = 変えない
    // 退避したカメラ（Explicit / Game のときだけ戻す）
    bool              saved = false;
    DirectX::XMFLOAT3 savedPos{};
    float savedYaw = 0, savedPitch = 0, savedFov = 0, savedAspect = 0, savedNear = 0, savedFar = 0, savedOrthoH = 0;
    bool  savedOrtho = false;

    // ---- 要求 ----
    u32 width = 0, height = 0;   // 0 = 表示矩形と同じ
    u32 top = 8;
    int settleFrames = 8;
    std::vector<std::string>  targetNames;
    std::vector<entt::entity> targetEntities;
    std::string path;            // 最終画の保存先（空 = 保存しない）
    bool includeTransparent = true;   // 半透明を「手前の面」として ID パスに描くか

    // ---- 撮影時のスナップショット ----
    std::vector<std::vector<u32>> groupItems;   // 対象ごとの描画アイテム添字（ID-1）
    std::vector<perception::Group> groups;       // ids / AABB（isolatedMask は読み戻し後に差し込む）
    perception::CameraInfo camera;
    DirectX::XMFLOAT3 camForward{};
    float camFovDeg = 0.0f;
    u32   drawCalls = 0, transparentCount = 0, isolationUsed = 0;
    u32   analysisW = 0, analysisH = 0;

    // ---- 計測 ----
    std::chrono::steady_clock::time_point t0;
    double recordMs = 0.0;
    int    frames   = 0;   // 受付から応答までに回したフレーム数
};

} // namespace dx12e
