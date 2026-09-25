#pragma once
// ===========================================================================
// ViewDesc — 「どの視点から・どの大きさで・何を有効にして・どこへ」描くか（1 ビューの記述）
// ---------------------------------------------------------------------------
// Application::RenderView(const ViewDesc&, ...) がこれ 1 つを受け取って、影 → … → ポストまでを
// 1 本の経路で描く。メインカメラもカメラプレビューもこの経路を通る（以前はプレビューだけ
// 機能を削った複製コードだった）。反射プローブのキャプチャ / 複数カメラ / 知覚層の別視点も
// ここへ ViewDesc を渡すだけで同じ絵作りを再利用できる。
//
// ★行列はすべて DirectXMath の行ベクトル規約（v' = v * M）で「非転置」。シェーダへ送るときに
//   転置するのは受け取った側の責任（従来どおり）。
// ★ジッタは 2 本に分けて持つ。viewProj（ジッタなし）は再投影・深度線形化・太陽投影・カスケード
//   分割・前フレーム行列の保存など「計算に使うもの」、viewProjJittered は深度プリパス / Forward /
//   スカイ / パーティクル / スプライトなど「画を出すもの全部」（00-COORDINATION §5.5）。
// ★GPU 資源は持たない（生ポインタとハンドルを借りるだけ）。寿命は呼び出し側。
// ===========================================================================
#include <DirectXMath.h>
#include <directx/d3d12.h>

#include "core/Types.h"

namespace dx12e
{

class RenderTarget;
class ConstantBuffer;

// ビューで有効にする機能。立っていない機能は「その段を丸ごと飛ばす」か「無効時の既定値
// （AO=白 / SSR=黒 / クラスタ=総当たり など）を張る」。メインカメラは kViewAllFeatures。
enum ViewFeature : u32
{
    kViewShadows        = 1u << 0,   // スポット / ポイント / CSM の影マップをこのビューで描く（無ければ今ある影マップを読むだけ）
    kViewScreenSpace    = 1u << 1,   // 深度プリパスと、それを読むもの（SSAO / コンタクトシャドウ / SSR / SSGI / RT 画面パス / Hi-Z）
    kViewClustered      = 1u << 2,   // クラスタカリング（無ければ先頭 64 灯の総当たり。デカールも出ない）
    kViewVolumetricFog  = 1u << 3,   // froxel フォグ（構築と合成）
    kViewSkybox         = 1u << 4,   // スカイボックス
    kViewParticles      = 1u << 5,   // CPU / GPU パーティクルと歪みバッファ
    kViewWorldSprites   = 1u << 6,   // ワールド空間の Sprite2D
    kViewDebugDraw      = 1u << 7,   // 物理 / ナビのワイヤ、render_debug、クラスタのデバッグ表示、速度の可視化
    kViewPostChain      = 1u << 8,   // TAA / DoF / MB / 自動露出 / ブルーム / ゴッドレイ / フレア / LUT / スクリーンシェーダー
                                     //   （無ければトーンマップ + ガンマだけ。露出はメインビューの自動露出を読む）
    kViewAllFeatures    = (1u << 9) - 1u,
};

struct ViewDesc
{
    const char* name = "view";   // ログ / 診断用

    // ---- カメラ（すべてジッタなし・非転置）----
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 proj{};
    DirectX::XMFLOAT4X4 viewProj{};           // = view * proj（Camera::GetViewProjMatrix と同じ値を入れる）
    DirectX::XMFLOAT3   position{};
    f32  nearZ = 0.1f;
    f32  farZ  = 1000.0f;
    bool orthographic = false;

    // ---- ジッタ（TAA）----
    DirectX::XMFLOAT2   jitterNdc{0.0f, 0.0f};   // 0 ならジッタなし
    DirectX::XMFLOAT4X4 viewProjJittered{};      // ラスタライズ用（ジッタなしなら viewProj と同じ値）

    // ---- 描画先（シーン系。すべてこのビューのレンダー解像度）----
    u32 width  = 0;
    u32 height = 0;
    RenderTarget*               sceneColor = nullptr;   // HDR のシーン RT（kSceneColorFormat）
    ID3D12Resource*             depth      = nullptr;   // 深度（DEPTH_WRITE で置かれている前提。出口でも戻す）
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    u32                         depthSrvIndex = 0xFFFFFFFFu;   // 深度を読むパス（kViewScreenSpace / パーティクル / ポスト）用
    // b1（PerFrameConstants）の置き場。★同じフレームで描くビューごとに別のバッファを渡すこと
    //   （主ビューの分を上書きすると、先に記録した主ビューの描画まで副ビューの視点で読んでしまう）。
    ConstantBuffer*             perFrameCB = nullptr;

    // ---- 出力先 ----
    // outputToBackBuffer=true … スワップチェーンの今のバックバッファの矩形 (outX,outY,outW,outH) へ出す
    //                            （PRESENT→RENDER_TARGET の遷移と下地の塗りもこのビューが行う）
    // false                   … output（LDR の RenderTarget）の全面へ出し、出口で PIXEL_SHADER_RESOURCE に置く
    bool          outputToBackBuffer = true;
    RenderTarget* output = nullptr;
    u32 outX = 0, outY = 0, outW = 0, outH = 0;

    // ---- 機能と役割 ----
    u32  features = kViewAllFeatures;
    // このフレームの主ビューか。★1 フレームに 1 つだけ。主ビューだけが
    //   GPU / CPU の計測スコープ、描画統計（perf_stats の main / shadow）、フレームで 1 回だけの
    //   シーン側の更新（ライト / デカールの転送、DDGI のプローブ更新）、時間方向の履歴
    //   （TAA の履歴破棄 / 前フレーム行列 / SSR 用カラー退避）、診断フラグを担う。
    //   副ビューは主ビューが作った b1 のシーン側（ライト / 影行列 / IBL / DDGI）を流用し、
    //   視点に依る値だけを差し替える。
    bool primary = true;
    // ゲームの絵か（true ならエディタのグリッドを出さない / fx:pulse の画面インパクトを掛ける）。
    bool isGameView = false;

    bool Has(u32 f) const { return (features & f) == f; }
};

} // namespace dx12e
