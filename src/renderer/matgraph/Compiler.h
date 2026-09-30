// ============================================================================
// Compiler.h — グラフ → IR → HLSL（純ロジック。DXC / D3D 非依存）
//
//   検証（GraphAnalysis）→ 到達可能ノードの後順 → IR（CSE 済み）→ スロット割当 → HLSL 生成。
//   生成物:
//     ・hlsl        : UnoMatEval() を持つ 1 ファイル（契約は docs/MATGRAPH_FORMAT.md「生成 HLSL の契約」）
//     ・slots       : パラメータプール（float4 の並び）のスロット表。値の編集で再コンパイルしないための仕掛け
//     ・diagnostics : nodeId / pin つきの診断
//     ・sourceMap   : 生成 HLSL の行 → ノード（`#line` と併用して DXC エラーを逆引きする）
//   決定論: 同じグラフ（位置・スロット値・名前・コメントを除く）→ 同じ HLSL バイト列。
// ============================================================================
#pragma once

#include "renderer/matgraph/GraphAnalysis.h"
#include "renderer/matgraph/GraphModel.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

// ---------------------------------------------------------------------------
// スロット表（パラメータプールの 1 レコード = float4 × slotCount）
// ---------------------------------------------------------------------------
enum class SlotKind : uint8_t
{
    Scalar,           // ScalarParameter（x に値）
    Vector,           // VectorParameter（F4 / xyzw に値）
    Texture,          // TextureParameter（x = SRV 添字を asuint で、y = 1/幅, z = 1/高さ）
    Constant,         // Float / Float2 / Float3 / Float4 ノードの値
    NodeProp,         // ノードのスロット化プロパティ（TexCoord の tiling / offset など）
    ImplicitTexture,  // TextureSample / NormalMap の texture プロパティ（Tex 未接続のとき）
};

struct SlotInfo
{
    int         slot = 0;                    // float4 単位の添字（0 始まり・連続）
    SlotKind    kind = SlotKind::Constant;
    ValueType   type = ValueType::F1;        // F1..F4 / Tex2D
    std::string name;                        // パラメータ名（Constant / NodeProp / ImplicitTexture は空）
    std::string group;
    int         priority = 0;
    NodeId      nodeId;
    std::string prop;                        // 値を持つプロパティ名（"value" / "default" / "tiling" ...）
    float       value[4] = {0, 0, 0, 0};     // 既定値（利用者が見る値。srgb の変換前）
    float       rangeMin = 0.0f, rangeMax = 1.0f;
    bool        hasRange = false;
    std::string texturePath;                 // Texture / ImplicitTexture の既定パス
    std::string textureUsage;                // "Color" / "LinearColor" / "Normal" / "Mask"
    bool        srgb = false;                // true: 格納時に RGB を sRGB → リニアへ変換する
};

// ---------------------------------------------------------------------------
// IR（CSE 済み。CpuEval もこれを評価する）
// ---------------------------------------------------------------------------
struct IrOperand
{
    enum class Kind : uint8_t { None, Op, Literal };
    Kind      kind = Kind::None;
    int       op = -1;                 // Op: IrOp の添字
    uint8_t   comps[4] = {0, 1, 2, 3}; // Op: 変数から取る成分（スウィズル）
    int       ncomps = 0;              // 0 = 変数そのまま
    uint8_t   conv = 0;                // 0 = なし / 1 = (floatN) へ / 2 = (int) へ（Int / Bool 由来）
    ValueType srcType = ValueType::Invalid;  // キャスト前の型
    ValueType type = ValueType::Invalid;     // キャスト後の型（ピンが受ける型）
    Value     literal;                 // Literal: キャスト後の型へ畳み込み済み
};

struct IrOp
{
    int         id = 0;
    NodeId      node;                  // 最初にこの Op を作ったノード（CSE で他ノードが合流しても変わらない）
    const NodeDef* def = nullptr;      // 組み込み入力 / 暗黙テクスチャは nullptr
    std::string opcode;                // NodeDef::type / "builtin:uv" / "implicitTex"
    ValueType   type = ValueType::Invalid;
    std::vector<IrOperand>       in;   // def->inputs の順
    std::map<std::string, Json>  props;
    std::map<std::string, int>   slotOf;   // プロパティ名 → スロット番号
    bool        inlineConst = false;
    std::string builtin;               // "builtin:*"
    int         implicitSlot = -1;     // implicitTex
    int         customIndex = -1;      // Custom ノードの通し番号
    std::string key;                   // CSE キー（デバッグ用）
};

struct IrOutput
{
    std::string field;                 // UnoSurface のフィールド名
    std::string pin;                   // MaterialOutput のピン名
    IrOperand   value;
};

struct Ir
{
    std::vector<IrOp>     ops;
    std::vector<IrOutput> outputs;
};

struct SourceMapEntry
{
    int    firstLine = 0;   // 生成 HLSL の行（1 始まり）
    int    lastLine  = 0;
    NodeId nodeId;
};

struct CompileStats
{
    int nodesTotal = 0;      // グラフの全ノード
    int nodesReachable = 0;  // 出力に繋がっているノード
    int ops = 0;             // IR の Op 数（CSE 後）
    int cseHits = 0;         // CSE で合流した回数
    int slots = 0;           // スロット数（= レコード長 float4）
    int textures = 0;        // テクスチャスロット数
    int hlslLines = 0;
};

struct CompileOptions
{
    // 生成ファイルの先頭に置く include 行。エンジンでは既定の ForwardGraph.hlsl、テストでは参照契約ヘッダへ差し替える
    std::string includeLine = "#include \"forward/ForwardGraph.hlsl\"";
    std::string shaderModel = "6_6";        // 先頭の "// @sm 6_6"
    std::string sourceName;                 // ヘッダのコメントに書くだけ（例 "materials/rock_wet.dxmg"）
    bool        emitLineDirectives = true;  // #line "node:<id>" を出す
    // ★部分グラフ出力（G2c。ノード内サムネイル / ノード単位プレビュー）。空でなければ MaterialOutput の代わりにこのノードの出力ピンを
    //   「表示色」として書き出す: F1 = (v,v,v) / F2 = (x,y,0) / F3 = そのまま / F4 = rgb（alpha は s.opacity）/ Tex2D = そのテクスチャを
    //   mi.uv でサンプルした rgb。s.baseColor / s.opacity にだけ書く（他の UnoSurface のフィールドは既定のまま）。
    //   到達可能性 / エラー判定はこのノードの上流だけで行う（MaterialOutput が無い / 別の枝が壊れていても評価できる）。
    //   既定（空）は従来と 1 バイトも変わらない（G1 のゴールデンは不変）。
    NodeId      previewNode;
    std::string previewPin;                 // 空 = 最初の出力ピン
};

constexpr int kCodegenVersion = 1;

struct CompileResult
{
    bool                     ok = false;       // false のとき hlsl は空
    std::string              hlsl;
    uint64_t                 hash = 0;         // hlsl 本体（ヘッダ行を除く）の FNV-1a 64
    std::vector<SlotInfo>    slots;            // slot 昇順・連続
    int                      slotCount = 0;
    std::vector<Diagnostic>  diagnostics;      // 解析の診断 + コンパイルの診断
    std::vector<SourceMapEntry> sourceMap;
    CompileStats             stats;
    Ir                       ir;
    GraphSettings            settings;
    ValueType                previewType = ValueType::Invalid;   // 部分グラフ出力（previewNode 指定）のとき、出力ピンの型（F1..F4 / Tex2D）

    bool HasErrors() const;
    const SlotInfo* FindSlotByName(const std::string& name) const;
};

CompileResult CompileGraph(const MaterialGraph& g, const CompileOptions& opt = {});

// DXC のエラー出力（`node:<id>:<line>:<col>: error: ...`）を nodeId つきの診断へ逆引きする。
// `#line` が効かない生成スキャフォールド側のエラーは nodeId が空になる。
std::vector<Diagnostic> MapDxcLog(const std::string& log);

// ---------------------------------------------------------------------------
// パラメータレコード（float4 × slotCount）の組み立て。G2b のプール書き込みと同じ規則。
// ---------------------------------------------------------------------------
struct ParamOverride
{
    float       v[4] = {0, 0, 0, 0};
    std::string texture;       // テクスチャパラメータの上書き（assets 相対パス）
    bool        isTexture = false;
};
using ParamOverrides = std::map<std::string, ParamOverride>;   // パラメータ名 → 上書き

struct TextureBinding
{
    uint32_t srvIndex = 0;
    float    width = 1.0f, height = 1.0f;
};
using TextureResolver = std::function<TextureBinding(const std::string& path, const std::string& usage)>;

std::vector<float> PackParamRecord(const CompileResult& r, const ParamOverrides& overrides, const TextureResolver& tex);

// sRGB → リニア（厳密な区分関数）。srgb 指定のスロットへ格納するときに使う。
float SrgbToLinear(float c);

} // namespace dx12e::matgraph
