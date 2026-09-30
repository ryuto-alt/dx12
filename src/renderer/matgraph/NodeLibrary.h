// ============================================================================
// NodeLibrary.h — ノード登録表（データ駆動）
//
//   1 ノード型 = 1 個の NodeDef。入力 / 出力ピン・プロパティ・HLSL 断片（テンプレート）・
//   CPU 参照実装・説明（日本語）・カテゴリをここに持つ。
//   新ノードは BuiltinNodes.cpp に登録を 1 件足すだけで、モデル・型推論・コード生成・
//   CPU 評価・シリアライズのすべてに乗る（コンパイラ側の分岐は増やさない）。
//   足し方は docs/MATGRAPH_FORMAT.md「ノード登録表の増やし方」を参照。
// ============================================================================
#pragma once

#include "renderer/matgraph/MatGraphTypes.h"

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// ピン
// ---------------------------------------------------------------------------
enum class PinMode : uint8_t
{
    Fixed,  // 型が固定。接続元から CheckCast で変換される
    Poly,   // 多相（F1〜F4）。同じ polyGroup の入力の最大次元に揃う（F1 はスカラー拡張）。出力なら「result の型」
    Free,   // 接続元の型をそのまま受ける（次元を保つ）。Append / ComponentMask / Custom 用
};

struct PinDecl
{
    std::string name;
    PinMode     mode      = PinMode::Fixed;
    ValueType   type      = ValueType::F1;   // Fixed のときの型
    int         polyGroup = 1;               // Poly のとき（1..3）
    bool        required  = false;           // 未接続で既定値が無いとエラー
    bool        allowTexture = false;        // Free で Tex2D も受ける（Custom / Reroute）
    bool        hasDefault = false;          // 未接続のときに使う既定値
    Value       defaultValue;
    std::string defaultBuiltin;              // 既定値が組み込み入力のとき: "uv" / "vertexNormalWS" ...（空 = なし）
    bool        implicitTexture = false;     // Tex 未接続ならノードの texture プロパティから暗黙のテクスチャスロットを作る
    bool        reserved  = false;           // 予約ピン（灰色・接続不可）
    std::string surfaceField;                // 出力ノード: 対応する UnoSurface のフィールド名
    std::string desc;                        // 日本語の説明（ツールチップ）
};

struct OutPinDecl
{
    std::string name;
    PinMode     mode      = PinMode::Fixed;  // Fixed: type / Poly: ノードの result 型に従う
    ValueType   type      = ValueType::F1;
    std::string swizzle;                     // 空 = 変数そのもの。"xyz" などは変数の成分（多出力ノード）
    int         needDim   = 0;               // この出力が存在するのに必要な変数の次元（Split.B → 3 など）
    std::string desc;
};

// ---------------------------------------------------------------------------
// プロパティ（ノードの設定値）
// ---------------------------------------------------------------------------
enum class PropType : uint8_t { Float, Vec2, Vec3, Vec4, Color, Int, Bool, String, Text, Enum, Texture };

enum class PropRole : uint8_t
{
    Code,       // 変えると HLSL が変わる（再コンパイルが要る）
    SlotValue,  // 値はパラメータプールのスロットに入る（変えても再コンパイル不要。inline=true のノードでは Code 扱い）
    Meta,       // 名前・グループ・範囲など（HLSL にも値スロットにも影響しない）
};

struct PropDecl
{
    std::string name;
    PropType    type = PropType::Float;
    PropRole    role = PropRole::Code;
    Json        defaultValue;
    std::vector<std::string> enumValues;   // Enum のとき
    std::string desc;
    float       minValue = 0.0f, maxValue = 0.0f;  // UI ヒント（両方 0 = 制限なし）
};

// ノードのプロパティを型付きで読むビュー（未設定なら PropDecl の既定値）
class PropView
{
public:
    PropView(const struct NodeDef& def, const std::map<std::string, Json>& props) : m_def(def), m_props(props) {}
    const Json& Raw(const std::string& name) const;
    float       Float(const std::string& name) const;
    void        Vec(const std::string& name, float out[4]) const;   // 足りない成分は 0
    int         Int(const std::string& name) const;
    bool        Bool(const std::string& name) const;
    std::string Str(const std::string& name) const;
private:
    const struct NodeDef&               m_def;
    const std::map<std::string, Json>&  m_props;
};

// ---------------------------------------------------------------------------
// コールバックの文脈
// ---------------------------------------------------------------------------
struct InferCtx
{
    const struct NodeDef*       def = nullptr;
    const PropView*             props = nullptr;
    std::string                 nodeId;
    std::vector<ValueType>      inTypes;       // 入力ピン順。解決後の型（Free は接続元の型、未存在は Invalid）
    std::vector<bool>           inPresent;     // 接続 / リテラル / 既定値のどれかがある
    int                         polyDim[4] = {1, 1, 1, 1};   // group 1..3 の解決次元
    ValueType                   result = ValueType::Invalid; // 出力変数の型（既定推論済み。上書き可）
    std::vector<ValueType>      outTypes;      // 出力ピン順（既定推論済み。上書き可）
    std::vector<Diagnostic>*    diags = nullptr;

    void Error(const char* code, const std::string& pin, const std::string& msg, const std::string& hint = {}) const;
};

// HLSL 生成の文脈（コンパイラが実装する）
class EmitCtx
{
public:
    virtual ~EmitCtx() = default;
    virtual bool        Has(const std::string& pin) const = 0;        // その入力があるか（未接続で既定なし = false）
    virtual std::string In(const std::string& pin) const = 0;         // 入力のオペランド式（ピンの型へキャスト済み）
    virtual ValueType   InType(const std::string& pin) const = 0;     // キャスト後の型
    virtual bool        InIsLiteral(const std::string& pin) const = 0;
    virtual Value       InLiteral(const std::string& pin) const = 0;
    virtual ValueType   ResultType() const = 0;                       // この変数の型
    virtual std::string Slot(const std::string& prop) const = 0;      // スロット読み出し式 "pool.F3(4)"（inline なら HLSL リテラル）
    virtual const PropView& Props() const = 0;
    virtual std::string Format(const std::string& tmpl) const = 0;    // {Pin} {P:prop} {SLOT:prop} {T} を展開
};

// CPU 参照実装の文脈
struct CpuVal
{
    ValueType type = ValueType::Invalid;
    float     v[4] = {0, 0, 0, 0};   // Int / Bool もここ（Bool は 0 / 1）
    float x() const { return v[0]; }
};

struct EvalEnv
{
    float uv[2]            = {0, 0};
    float worldPos[3]      = {0, 0, 0};
    float vertexNormalWS[3]= {0, 0, 1};
    float cameraPos[3]     = {0, 0, 0};
    float vertexColor[4]   = {1, 1, 1, 1};
    float time             = 0;
    // テクスチャスロット（SlotInfo.slot）→ サンプル結果。無ければ TextureSample は評価不能
    std::function<void(int slot, const float uv[2], float mipBias, float outRgba[4])> sampleTexture;
};

class EvalCtx
{
public:
    virtual ~EvalCtx() = default;
    virtual bool          Has(const std::string& pin) const = 0;
    virtual const CpuVal& In(const std::string& pin) const = 0;     // キャスト済み
    virtual CpuVal        Slot(const std::string& prop) const = 0;  // スロットの現在値（上書き反映済み）
    virtual const PropView& Props() const = 0;
    virtual const EvalEnv&  Env() const = 0;
    virtual ValueType     ResultType() const = 0;
    virtual int           TextureSlot(const std::string& pin) const = 0;  // Tex 入力が指すスロット（無ければ -1）
    virtual void          Fail(const std::string& why) = 0;               // 評価不能（診断に載る）
};

using InferFn = std::function<void(InferCtx&)>;
using EmitFn  = std::function<std::string(EmitCtx&)>;
using EvalFn  = std::function<CpuVal(EvalCtx&)>;

// ---------------------------------------------------------------------------
// ノード定義
// ---------------------------------------------------------------------------
struct NodeDef
{
    std::string type;          // 型 ID（.dxmg の "type"。変えない）
    std::string displayName;   // 表示名（日本語可）
    std::string category;      // カテゴリ（日本語。順序は kCategoryOrder）
    std::string description;   // 説明（日本語・標準語）
    std::string keywords;      // 検索語（空白区切り。英語 / 別名）

    std::vector<PinDecl>    inputs;
    std::vector<OutPinDecl> outputs;
    std::vector<PropDecl>   props;

    // 出力変数（1 ノード = 1 個の HLSL 変数）の型
    PinMode   resultMode  = PinMode::Fixed;
    ValueType resultType  = ValueType::F1;
    int       resultGroup = 1;

    bool isOutput      = false;   // MaterialOutput
    bool isParameter   = false;   // 名前つきパラメータ（インスタンスで上書きできる）
    bool isTransparent = false;   // Reroute（IR から消える）
    bool noCse         = false;   // 共通部分式にまとめない（Custom）
    bool isCustom      = false;   // Custom HLSL ノード（関数を別出しする）
    bool needsPixelDerivatives = false; // 画面微分を使う（将来の resolve 非対応判定用）

    std::string hlsl;             // 式テンプレート（emit が無いとき）。{Pin} {P:prop} {SLOT:prop} {T}
    EmitFn      emit;             // テンプレートで書けないとき
    EvalFn      eval;             // CPU 参照実装
    InferFn     infer;            // 既定の型推論で足りないとき
    std::string helperName;       // 生成ファイルの先頭に 1 回だけ出す補助関数（名前と本文）
    std::string helperHlsl;

    // ---- 検索ヘルパ ----
    const PinDecl*    FindInput(const std::string& name) const;
    int               InputIndex(const std::string& name) const;   // -1 = 無い
    const OutPinDecl* FindOutput(const std::string& name) const;
    int               OutputIndex(const std::string& name) const;
    const PropDecl*   FindProp(const std::string& name) const;
};

// カテゴリの表示順
extern const char* const kCategoryOrder[];
extern const int         kCategoryCount;

// ---------------------------------------------------------------------------
// 登録表
// ---------------------------------------------------------------------------
class NodeLibrary
{
public:
    void Register(NodeDef def);                       // 同じ type は上書き
    const NodeDef* Find(const std::string& type) const;
    std::vector<const NodeDef*> List() const;         // カテゴリ順 → 型名順
    std::vector<const NodeDef*> Search(const std::string& query) const;  // 表示名 / 型 / 検索語 / 説明の部分一致
    size_t Size() const { return m_defs.size(); }

    // 組み込みノード（初回呼び出しで構築。以後は不変）。拡張したいときはコピーして Register する。
    static const NodeLibrary& Builtin();

private:
    std::map<std::string, NodeDef> m_defs;
};

// BuiltinNodes.cpp が実装
void RegisterBuiltinNodes(NodeLibrary& lib);

// プロパティ値を PropDecl の型へ正規化（Float → 浮動小数、Vec → 長さ固定の配列 ...）。不正なら false
bool NormalizeProp(const PropDecl& decl, const Json& in, Json& out);

} // namespace dx12e::matgraph
