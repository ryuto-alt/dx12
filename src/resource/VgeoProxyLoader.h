#pragma once

#include <filesystem>

#include "resource/ModelLoader.h"   // ModelData

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class ResourceManager;

// `.vgeo`（仮想ジオメトリ）の「プロキシ」を通常のモデルとして読む。
//   PROXY セクション（無ければ NONVG）の 1 セクション = 1 Mesh + 1 Material（並列配列）。
//   TLAS・影・ピッキング・物理は、このプロキシを普通のメッシュとして見る＝無改造。
//   VG 本体（クラスタ DAG のページ）はここでは読まない（renderer/vg の VG システムが別に読む）。
// 検証エラー（VgeoError）は日本語ログを出して空の ModelData を返す（＝既存の「読み込み失敗」扱い）。
ModelData LoadVgeoProxy(GraphicsDevice& device,
                        ID3D12GraphicsCommandList* cmdList,
                        const std::filesystem::path& filePath,
                        ResourceManager& resourceManager);

// 拡張子が .vgeo か（大文字小文字を無視）。ModelLoader::LoadFromFile の分岐に使う。
bool IsVgeoPath(const std::filesystem::path& filePath);

} // namespace dx12e
