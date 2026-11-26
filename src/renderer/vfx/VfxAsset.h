#pragma once
// ===========================================================================
// .uvfx（UnoFX のアセット。JSON）の読み書きと検証、GPU の命令列への変換（W1-BD4）。
//   GPU / D3D に依存しない純ロジック（tests/vfx_asset_test.cpp が直接リンクする）。
//
//   {
//     "uvfx": 1, "name": "...", "duration": 4.0, "loop": true,
//     "emitters": [ {
//        "name": "flame", "maxParticles": 2048,
//        "spawn":  { "rate": 80, "startDelay": 0, "bursts": [ { "t": 0, "count": 30 } ] },
//        "init":   [ { "module": "shape", ... }, { "module": "velocity", ... }, ... ],
//        "update": [ { "module": "gravity", ... }, { "module": "curlNoise", ... }, ... ],
//        "render": { "kind": "sprite", "blend": "additive", "softDistance": 0.5, "intensity": 1, ... }
//     } ]
//   }
//
//   * モジュールは書いた順に GPU が実行する（スタック）。未知のモジュール / 範囲外の値は「エラー」で読み込まない
//     （黙って捨てない）。未知のキーは警告にして残す。
//   * 読み込み → 書き出し → 読み込みで同じ内容になる（既定値を補った正規形で書く）。
// ===========================================================================
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "renderer/vfx/VfxTypes.h"

namespace dx12e::vfx
{

struct ModuleDesc
{
    std::string    module;   // "shape" / "velocity" / ...
    nlohmann::json params;   // 既定値を補った正規形（キーは module ごとに固定）
};

struct BurstDesc
{
    float t     = 0.0f;   // 秒（エフェクトの時刻）
    u32   count = 0;
};

struct SpawnDesc
{
    float rate       = 0.0f;   // 個 / 秒
    float startDelay = 0.0f;   // 秒
    std::vector<BurstDesc> bursts;   // t の昇順
};

struct RenderDesc
{
    RenderKind kind  = RenderKind::Sprite;
    Blend      blend = Blend::Additive;
    Fade       fade  = Fade::InOut;
    float softDistance = 0.5f;   // 深度との差でフェードする距離（m）。0 でソフトなし
    float intensity    = 1.0f;   // 色の倍率（HDR）
    float stretch      = 0.0f;   // 速度方向への伸ばし（0 で伸ばさない）
    // フリップブック（BD7）。kind=flipbook のとき使う。BD4 では読み書きと検証だけ（描画は sprite と同じ）。
    std::string texture;
    u32   flipCols = 1, flipRows = 1;
    float flipFps  = 0.0f;
};

struct EmitterDesc
{
    std::string name;
    u32         maxParticles = 1024;
    SpawnDesc   spawn;
    std::vector<ModuleDesc> init;
    std::vector<ModuleDesc> update;
    RenderDesc  render;
};

struct VfxAsset
{
    std::string name;
    float       duration = 0.0f;   // 秒。0 以下は「終わりなし」
    bool        loop     = true;
    std::vector<EmitterDesc> emitters;
};

struct ParseResult
{
    bool        ok = false;
    std::string error;                  // ok=false のとき（"emitters[0].init[2]: ..." の形で場所つき）
    std::vector<std::string> warnings;
    VfxAsset    asset;
};

// JSON 文字列から読む。失敗したら ok=false（asset は未定義）。
ParseResult ParseUvfx(const std::string& text);
// 正規形で書き出す（インデント 2）。
std::string SerializeUvfx(const VfxAsset&);

// GPU の命令列へ変換。モジュールの検証は ParseUvfx で済んでいる前提（不正なら ok=false）。
struct CompiledEmitter
{
    std::vector<GOp> init;
    std::vector<GOp> update;
    float maxLife = 0.0f;   // 寿命の最大値（init の lifetime から。終了判定に使う）
};
bool CompileEmitter(const EmitterDesc&, CompiledEmitter& out, std::string& error);

// プリセットに使えるモジュール名（ドキュメント / MCP のエラーの案内用）。
const std::vector<std::string>& InitModuleNames();
const std::vector<std::string>& UpdateModuleNames();

} // namespace dx12e::vfx
