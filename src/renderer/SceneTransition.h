#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

enum class TransitionType
{
    FadeBlack    = 0,
    Wipe         = 1,   // 横ワイプ
    Circle       = 2,   // 円（アイリス）
    WipeVertical = 3,
};

// 画面遷移オーバーレイ。progress を 0→1→0 で動かし、中間点でシーンロードを発火させる。
class SceneTransition
{
public:
    void Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir);

    // 遷移開始。totalDuration 秒で「閉じる→（中間でロード）→開く」。
    void Start(TransitionType type, float totalDuration);
    void Update(float dt);

    bool IsActive() const { return m_active; }
    // 中間点（画面が隠れた瞬間）に一度だけ true を返す。シーンロードのトリガに使う。
    bool ConsumeHalfway();

    // 呼び出し側で対象 RTV を先にバインドしておくこと。
    void Render(ID3D12GraphicsCommandList* cmd, float aspect);

private:
    float Progress() const;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    bool  m_active        = false;
    bool  m_halfwayPending = false;
    bool  m_halfwayFired   = false;
    float m_t   = 0.0f;
    float m_dur = 1.0f;
    TransitionType m_type = TransitionType::FadeBlack;
};

} // namespace dx12e
