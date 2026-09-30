#pragma once

// ===== サンドボックス用のサンプルグラフ生成（純ロジック。テストと視覚検証が共用）=====

#include "editor/nodegraph/GraphDocument.h"
#include "editor/nodegraph/SandboxGraphModel.h"

#include <cstdint>

namespace dx12e::ng
{

struct SampleReport
{
    int nodes = 0, edges = 0, comments = 0;
    int failedLinks = 0;   // 繋げなかった本数（0 であること）
};

// 手組みの「岩 + 苔 + フレネル」風グラフ（約 40 ノード / コメント 3 個 / リルート 2 個）。文書は先に空にする。
// 生成後は履歴を空にする（サンプルを Undo で消せないように）。
SampleReport BuildSampleGraph(GraphDocument& doc, SandboxGraphModel& model);

// 合成の大きなグラフ（性能測定用）。列に並べ、前の列から入力へランダムに繋ぐ。決定的（seed 固定）。
SampleReport BuildStressGraph(GraphDocument& doc, SandboxGraphModel& model, int nodeCount, int edgeCount, uint32_t seed);

} // namespace dx12e::ng
