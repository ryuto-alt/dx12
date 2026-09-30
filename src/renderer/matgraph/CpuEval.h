// ============================================================================
// CpuEval.h — IR の CPU 参照実装（数値テストの正解）
//
//   CompileGraph が作った IR（CSE・キャスト・スロット割当済み）をそのまま評価する。
//   つまり「生成 HLSL が GPU でやること」を CPU で再現する。テクスチャは EvalEnv::sampleTexture を
//   渡したときだけ評価できる。Custom ノードなど eval を持たないノードは診断 E_CPU_UNSUPPORTED。
// ============================================================================
#pragma once

#include "renderer/matgraph/Compiler.h"

#include <vector>

namespace dx12e::matgraph
{

// UnoSurface のうちグラフが書ける部分（既定値は UnoSurfaceDefault() と同じ）
struct SurfaceValues
{
    float baseColor[3]        = {0.5f, 0.5f, 0.5f};
    float metallic            = 0.0f;
    float roughness           = 0.5f;
    float normalTS[3]         = {0.0f, 0.0f, 1.0f};
    bool  hasNormal           = false;
    float emissive[3]         = {0.0f, 0.0f, 0.0f};
    float ao                  = 1.0f;
    float opacity             = 1.0f;
    float opacityMask         = 1.0f;
    float subsurfaceColor[3]  = {0.0f, 0.0f, 0.0f};
    float subsurfaceOpacity   = 0.0f;
    float clearCoat           = 0.0f;
    float clearCoatRoughness  = 0.0f;
    float anisotropy          = 0.0f;
};

struct CpuEvalResult
{
    bool                    ok = false;
    std::vector<Diagnostic> diagnostics;   // E_CPU_UNSUPPORTED など
    SurfaceValues           surface;
    std::vector<CpuVal>     opValues;      // IR の Op ごとの値（デバッグ / 部分比較用）
};

// overrides: パラメータ名 → 上書き値（無ければスロットの既定値）
CpuEvalResult EvaluateCpu(const CompileResult& r, const EvalEnv& env, const ParamOverrides* overrides = nullptr);

} // namespace dx12e::matgraph
