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
    // フリップブック（BD7）。kind=flipbook のとき使う。texture = 炎族 / RLU / 色のアトラス。
    //   channels: fire（炎族: R 温度 G 不透明度 B 浸食場 A 発光。黒体の色）/ smoke6way（texture = RLU、flipDbf = DBF。6-way のライティング）
    //             / color（straight な RGBA をそのまま）。
    //   anchor: bottom（根元固定。四角形の下端が粒子の位置。炎と煙の枠の規約）/ center。既定は fire・smoke6way = bottom、color = center。
    //   mv: R16G16_SNORM のモーションベクトル（無ければ MV 補間なし）。mvRange: snorm 1.0 が枠 uv の何倍か（NB2 の焼き込みは 0.5）。
    std::string texture;
    u32   flipCols = 1, flipRows = 1;
    u32   flipFrames = 0;          // 0 = cols * rows
    float flipFps  = 0.0f;         // 0 = 寿命に渡って先頭から最後まで 1 回
    std::string flipChannels = "fire";
    std::string flipDbf, flipMv;
    float flipMvRange = 0.5f;
    bool  flipAnchorSet = false;   // false = channels の既定
    bool  flipAnchorBottom = true;
    float flipTempMin = 1000.0f, flipTempMax = 2800.0f, flipEmission = 3.0f;   // 炎族（K, K, 放射の倍率）
    float flipErodeStart = 0.35f, flipErodeWidth = 0.15f;                      // 浸食（寿命の割合の開始、縁の幅）
    bool  AnchorBottom() const { return flipAnchorSet ? flipAnchorBottom : flipChannels != "color"; }
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
