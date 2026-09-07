#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// ★値は保存値（settings.json / シーン JSON）と Lua の transitionToScene(type) に直接出る。
//   既存の番号は動かさないこと。新しい型は必ず末尾へ足す。
// ★形の実装は shaders/post/TransitionCurtain.hlsli の TransitionCurtain() **ただ 1 箇所**。
//   実機（Transition.hlsl）もエディタのプレビュー窓/サムネイル（TransitionPreview.hlsl）も
//   同じ関数を呼ぶので、C++ 側に形の写しは無い＝「サムネイルと実物が違う」は起きない。
//   型を足すときに触るのは「この enum」「TransitionCurtain.hlsli の分岐」
//   「TransitionPresets.h の表」の 3 つだけ。
enum class TransitionType
{
    FadeBlack    = 0,
    Wipe         = 1,   // 横ワイプ
    Circle       = 2,   // 円（アイリス）
    WipeVertical = 3,
    Seek         = 4,   // シークバー早送り（プレイヘッド掃引+シークバー。動画モチーフのゲーム向け）
    FadeWhite    = 5,   // ホワイトアウト（閃光・回想の入り）
    Blinds       = 6,   // ブラインド（横帯が同時に閉じる）
    RadialClock  = 7,   // 時計ワイプ（12時から時計回り）
    Diamond      = 8,   // 菱形アイリス（Circle の角ばった版）
    // ---- ここから拡張（ワイプ系）----
    WipeDiagonal   = 9,    // 斜めワイプ（左上→右下）
    Curtain        = 10,   // カーテン（左右の 2 枚が中央で合わさる）
    SlidePush      = 11,   // スライド（一枚板が右から入り、そのまま左へ抜ける）
    BlindsVertical = 12,   // 縦ブラインド
    // ---- アイリス系 ----
    Star  = 13,   // 星アイリス（5 芒星）
    Plus  = 14,   // 十字アイリス
    Heart = 15,   // ハートアイリス
    // ---- エフェクト系 ----
    Dissolve   = 16,   // ノイズディゾルブ
    Mosaic     = 17,   // ブロックモザイク
    Melt       = 18,   // メルト（Doom 融解）
    Shatter    = 19,   // ガラス割れ
    Glitch     = 20,   // グリッチ（走査帯 + 色収差）
    Hexagon    = 21,   // ハニカム
    Checker    = 22,   // 市松
    Spiral     = 23,   // 渦巻き
    Ripple     = 24,   // 波紋
    Flood      = 25,   // 水没
    Burn       = 26,   // フィルムバーン
    SpeedLines = 27,   // 集中線
};

// TransitionType の個数。保存値の範囲チェックに使う。
inline constexpr int kTransitionTypeCount = 28;

// 画面遷移オーバーレイ。progress を 0→1→0 で動かし、中間点でシーンロードを発火させる。
class SceneTransition
{
public:
    void Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    // 遷移開始。totalDuration 秒で「閉じる→（中間でロード）→開く」。
    void Start(TransitionType type, float totalDuration);
    void Update(float dt);

    // true の間は中間点（画面が隠れきった状態）で進行を止める。
    // シーンのロードが複数フレームに分かれるとき、終わるまで開かせないために使う。
    void SetHold(bool hold) { m_hold = hold; }

    bool IsActive() const { return m_active; }
    // 中間点（画面が隠れた瞬間）に一度だけ true を返す。シーンロードのトリガに使う。
    bool ConsumeHalfway();

    // 呼び出し側で対象 RTV を先にバインドしておくこと。
    void Render(ID3D12GraphicsCommandList* cmd, float aspect);

private:
    float Progress() const;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;
    DXGI_FORMAT  m_outFormat = DXGI_FORMAT_UNKNOWN;

    bool  m_active        = false;
    bool  m_hold          = false;   // 中間点で待機（ロード完了待ち）
    bool  m_halfwayPending = false;
    bool  m_halfwayFired   = false;
    float m_t   = 0.0f;
    float m_dur = 1.0f;
    TransitionType m_type = TransitionType::FadeBlack;
};

} // namespace dx12e
