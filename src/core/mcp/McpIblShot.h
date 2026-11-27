#pragma once
// screenshot / screenshot_final の「IBL の焼き直しを待った」応答の欄（W1-FU-E-30 / C9）。
//   Application.h は W7 まで凍結なので、状態は ApplicationMcpLaneC.cpp のファイル内（撮影は同時に 1 枚だけ）に持つ。
//   ・Arm …… 撮影を受け付けたとき（要求の有無と、足した待ちフレーム数）
//   ・Append …… 撮影が終わったとき。応答 JSON へ iblRebakePending（依頼時点）・iblWaitFrames・
//               iblRebakePendingAtCapture（完了時点で IblRebakePending を取り直した結果）・note の前置きを足し、状態を消す
#include <nlohmann/json_fwd.hpp>

namespace dx12e
{
class Application;

namespace mcpibl
{
void Arm(bool requested, int waitFrames);
void Append(Application& app, nlohmann::json& reply);
} // namespace mcpibl
} // namespace dx12e
