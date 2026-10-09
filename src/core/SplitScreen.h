#pragma once
// ===========================================================================
// SplitScreenState — 画面分割（Lua: scene:setSplitScreen / setSplitView）の実行時状態。
//   req   : Lua が書く要求（区画数と各区画のカメラ姿勢）。ScriptEngine と共有する（GPU 資源なし）。
//   areas : 区画 2..N（添字 0..2 = area1..3）が描く先の GPU 資源。
//           HDR のシーン RT / 深度 + DSV / 専用の b1（★ビューごとに別 CB。共有すると先に記録した
//           ビューの描画まで後のビューの視点で読んでしまう）。
// 作る・作り直すのは Application::EnsureSplitResources（Run ループのフレーム外・WaitIdle 込み）だけ。
// 区画 1（area0）はメインカメラで、資源は既存の m_sceneRT 等をそのまま使う。
// ===========================================================================
#include <memory>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"
#include "graphics/Buffer.h"
#include "graphics/RenderTarget.h"
#include "renderer/SplitScreenLayout.h"

namespace dx12e
{

struct SplitAreaGpu
{
    std::unique_ptr<RenderTarget>   rt;      // HDR シーン RT（kSceneColorFormat）
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    D3D12_CPU_DESCRIPTOR_HANDLE     dsv{};   // 初回に 1 回だけ確保し、作り直しでは張り直すだけ（ヒープを食わない）
    std::unique_ptr<ConstantBuffer> frameCB; // この区画専用の b1
    u32 w = 0, h = 0;                        // 現在の RT の大きさ
    u32 pendW = 0, pendH = 0, settle = 0;    // リサイズのデバウンス
};

struct SplitScreenState
{
    SplitScreenRequest req;
    SplitAreaGpu areas[kSplitMaxAreas - 1];
};

} // namespace dx12e
