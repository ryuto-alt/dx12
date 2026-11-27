#pragma once
// レンダーバックエンドの拡張メンバの入口（W05-01b では枠だけ。中身は F1〜F3 / W1 の C が書く）。
// Application.h は凍結なので、C のレンダリング状態はここへ足す（Application の private からだけ触る）。
#include "graphics/PipelineState.h"

#include <memory>

namespace dx12e
{
// W1-C6: Epic の動的 CSM（2048 D16 × 3、pancaking）の状態と PSO。epicDynamicCsm が立つまで何も作らない。
struct EpicCsmState
{
    bool  active = false;      // D16 × 3 の影マップへ切り替え済み（m_shadowMapDirty で作り直す）
    bool  psosValid = false;
    float biasScale = 0.0f;    // PSO を作ったときの倍率
    int   depthBias = 0;
    float slopeBias = 0.0f;
    bool  dirty = false;       // シェーダの再読込みなどで作り直しが要る
    bool  buildFailed = false; // PSO を作れなかった（dirty になるまで再試行しない）
    bool  pancake = true;      // PSO を作ったときの pancaking（DepthClip=FALSE と、カリングから near 面を外す）
    std::unique_ptr<PipelineState> stat, inst, skin;            // 静的 / インスタンシング / スキンド
    std::unique_ptr<PipelineState> maskStat, maskInst, maskSkin; // MASK 版
};

struct RenderBackendExt
{
    EpicCsmState epicCsm;
};
} // namespace dx12e
