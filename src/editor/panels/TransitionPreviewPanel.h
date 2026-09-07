#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"

struct ImDrawList;
struct ImVec2;

namespace dx12e
{
class GraphicsDevice;
class CommandList;
class EditorContext;

// ===== トランジション専用ツール窓（ツール > トランジション）=====
//
// ★なぜ専用窓か: 以前はプリセットのタイルを押すと**編集中のシーンの上で**実物が再生された。
//   幕の形と作りかけのシーンが混ざって形が読めず、しかも中間点で本当にシーンが
//   切り替わるわけではないので「切り替わった感」も分からない＝プレビューとして機能していなかった。
//   この窓は中身の分かっている架空のゲーム画面 2 枚（昼のフィールド → 夜のダンジョン）を
//   専用のオフスクリーン RT に描き、遷移の中間点で A から B へ差し替える。
//
// ★サムネイルも実物と同じシェーダーで描く。プリセット全件を 1 枚のアトラス RT へ
//   タイル単位のビューポートで焼き、ImGui はその UV を切り出すだけ。
//   ＝ C++ 側に「形の近似実装」が一切無い（以前は editor/TransitionSwatch.h に写しがあった）。
class TransitionPreviewPanel
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void RecreatePipelines(GraphicsDevice& device);
    bool IsReady() const { return m_ready; }

    // ImGui フレームより前に呼ぶこと（VfxEditorPanel::RenderPreview3D と同じ運用）。
    // 大プレビューとサムネイルアトラスを自前の RT へ描き、再生時計を進める。
    void RenderOffscreen(CommandList& cmd, f32 dt);

    // 「トランジション」窓の中身。既定プリセットが変わったら true を返す
    // （呼び出し側が settings.json へ保存する）。
    bool RenderWindow(EditorContext& ctx, int& defaultType, f32& defaultDur);

    // 現在の既定プリセットに合わせて選択を差し替え、頭から再生する。
    // Scene Flow 窓の「選ぶ / プレビュー…」ボタンから呼ぶ。
    void Focus(int type, f32 dur);

    // プリセット 1 件のサムネイルを [a,b] へ置く（アトラスの UV を切り出すだけ）。
    // アトラスがまだ焼けていなければ枠だけ描く。
    void DrawPresetThumb(ImDrawList* dl, const ImVec2& a, const ImVec2& b, int presetIndex) const;

private:
    void DrawTransitionPass(CommandList& cmd, RenderTarget& rt,
                            u32 x, u32 y, u32 w, u32 h,
                            f32 progress, f32 total, int type);
    // 再生位置 m_t から被覆率を出す（SceneTransition::Progress と同じ式）。
    f32  Progress() const;
    f32  TotalNorm() const;

    // 大プレビュー。16:9。窓の中では幅に合わせて拡大縮小する。
    static constexpr u32 kPreviewW = 640;
    static constexpr u32 kPreviewH = 360;
    // サムネイルアトラス。1 タイル 16:9、kAtlasCols 列で必要な行数だけ確保する。
    static constexpr u32 kTileW     = 192;
    static constexpr u32 kTileH     = 108;
    static constexpr u32 kAtlasCols = 6;

    GraphicsDevice* m_device  = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    DescriptorHeap  m_rtvHeap;
    RenderTarget    m_previewRT;
    RenderTarget    m_atlasRT;
    u32             m_atlasRows = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::wstring m_shaderDir;
    bool         m_ready      = false;
    bool         m_atlasValid = false;

    // ---- 再生状態 ----
    int  m_type    = 0;        // プレビュー中の TransitionType
    f32  m_dur     = 1.0f;     // 「閉じる→開く」の合計秒
    f32  m_t       = 0.0f;     // 0..m_dur
    f32  m_speed   = 1.0f;     // 再生速度（ゆっくり見たい時用）
    bool m_playing = true;
    bool m_loop    = true;
    // サムネイルのループ時計。タイルごとに位相をずらして波打たせる。
    f32  m_atlasTime = 0.0f;
};

} // namespace dx12e
