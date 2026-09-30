#pragma once

// マテリアルグラフ窓（G3: ノードエディタ UI の接続）。G1 の MaterialGraph（マテリアルのノードグラフ）を、G0 のノードエディタ
// （src/editor/nodegraph/）で編集する。設計は docs/MATERIAL_GRAPH_DESIGN.md §6。
//
// レイアウト: 左 = ノードパレット（カテゴリ折りたたみ + 検索 + D&D / ダブルクリックで追加）/ 中央 = グラフキャンバス /
//            右 = プレビュー領域（G2c 用の空き枠）+ 詳細（選択ノードのプロパティ・出力ノードの接続状況・グラフ設定・パラメータ一覧）/
//            下 = 診断リスト（クリックで該当ノードへ）+ 生成 HLSL ビューア（読み取り専用・色分け・コピー）。
// 開き方: メニュー「ツール > マテリアルグラフ」/ コマンドパレット（Ctrl+K）/ アセットブラウザで .dxmg をダブルクリック。
// ファイル: .dxmg（G1 のシリアライズ。ノード位置・コメント・ズーム / パンも入る）。新規 / 開く / 保存 / 名前を付けて保存（コマンド表 matgraph.*）。
//
// 状態は Render() 内の関数ローカル static が持つ（Application 側の追記を増やさないため）。純ロジックは src/editor/matgraph/ にあり、
// tests/matgraph_editor_test.cpp が単体で検証する。この窓は描画と入力だけ。
//
// ★呼び出し経路: ApplicationRender.cpp を触らない約束のため、NodeGraphSandboxPanel::Render の末尾から呼ばれる。

#include <functional>
#include <string>

struct ID3D12GraphicsCommandList;

#include "editor/matgraph/MatGraphEditor.h"

namespace dx12e
{

class EditorContext;
class GraphicsDevice;
class RootSignature;
class ResourceManager;
class DescriptorHeap;
class GraphMaterialSystem;

namespace MaterialGraphPanel
{

// 毎フレーム 1 回。窓が閉じていれば即 return（フォーカス状態を落とす）。
void Render(EditorContext& ctx);

// EditorCommandTable の matgraph.* / graph.* / file.*（フォーカス時の振り向け）を次のフレームで実行する。窓が開いていなければ false。
bool ExecuteCommand(EditorContext& ctx, const char* commandId);

// テスト・MCP 用: 窓を開いてファイルを読む（未保存の変更は破棄する）。成功したら true。
bool OpenFileNow(EditorContext& ctx, const std::string& path, std::string* error);

// 将来（G2c）の差し込み口。どちらも未設定なら空き枠 / プレースホルダーを描く。
//   ・プレビュー枠（右カラム最上部）: 画面座標の矩形 [x0,y0]-[x1,y1] に描く。true を返したら空き枠の絵を描かない。
//     ed = 今のグラフ（Result() = 最新の生成結果: HLSL / スロット / 診断）。呼び出しは ImGui の窓の中（ImDrawList へ描ける）。
using PreviewDrawer = std::function<bool(float x0, float y0, float x1, float y1, mg::MatGraphEditor& ed)>;
void SetPreviewDrawer(PreviewDrawer fn);
//   ・テクスチャのサムネイル: abs パス → ImTextureID（0 = まだ無い）。ノードのプレビュー（GraphView::SetNodePreviewDrawer）用。
using ThumbnailProvider = std::function<unsigned long long(const std::string& absPath)>;
void SetThumbnailProvider(ThumbnailProvider fn);

// ---- G2c: ライブプレビュー / ノード内サムネイル（GPU）----
//   Application（エディタのときだけ）が GraphMaterialSystem の準備後に InitializeGpu、終了時（デバイス解放より前）に ShutdownGpu、
//   毎フレーム Render の直後に RenderGpu（プレビューの RT とサムネイルのアトラスを描く）を呼ぶ。
//   InitializeGpu が呼ばれない環境（テスト / GameRuntime）では従来どおり空き枠 / プレースホルダーのまま（ノードは大きくならない）。
struct GpuInit
{
    GraphicsDevice*      device = nullptr;
    RootSignature*       rootSignature = nullptr;   // メイン RS（バインドレス）
    ResourceManager*     resources = nullptr;
    DescriptorHeap*      srvHeap = nullptr;
    GraphMaterialSystem* graphs = nullptr;
    std::wstring         shaderDirW;                // IBLBaker の .cso の場所（PathResolver::ShaderDirW()）
};
bool InitializeGpu(const GpuInit& init);
void ShutdownGpu();
bool GpuReady();
void RenderGpu(ID3D12GraphicsCommandList* cmd, unsigned frameIndex);

// MCP / 自動テスト用の窓の操作と状態（JSON 文字列の入出力。op: status / open / new / edit / preview / screenshot / bench）。
// 窓が開いていなければ開く。詳細は docs/MATGRAPH_G2C.md「MCP」。
std::string DebugCall(EditorContext& ctx, const std::string& jsonIn);

} // namespace MaterialGraphPanel

} // namespace dx12e
