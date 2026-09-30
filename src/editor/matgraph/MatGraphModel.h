#pragma once

// ===== マテリアルグラフ: G1 の MaterialGraph を G0 の IGraphModel へ適合させるアダプタ =====
// マテリアルグラフ G3（UI 接続）。docs/MATERIAL_GRAPH_DESIGN.md §6 / MATGRAPH_G0_REPORT.md §7 / MATGRAPH_G1_REPORT.md 申し送り。
// ImGui にも D3D にも依存しない純ロジック（tests/matgraph_editor_test.cpp が単体で検証する）。
//
// 役割:
//   ・ID の相互変換: G0 は整数 NodeId（セッション内で安定）、G1 は文字列 ID（"n_071829"。保存形式の一部）。
//     表は「一度割り当てた整数は同じ文字列にしか使わない」（Undo が同じ整数で AddNode し直すので、文字列も元のまま戻る）。
//     整数はセッション内でだけ有効で、保存は常に文字列（.dxmg）。
//   ・ノード型カタログ: NodeLibrary（52 種）から NodeTypeDesc を生成（カテゴリ → 色スロット、ワンキー、ピン、値欄、プロパティ行）。
//   ・ピン: G1 の入力ピン（予約ピンを除く）が先、その後ろに「プロパティ行」（値欄だけの行 = 定数・パラメータ既定値・列挙など）。
//   ・ピン型: ValueType → 整数 PinType（PinTypes 列挙）。多相は ResolvedPinType で G1 の型推論結果を返す。
//   ・履歴: ConnectUnchecked は G1 の MaterialGraph::ConnectUnchecked（型検査なし）へ。Undo が厳密に戻る（G0 レポートの罠）。
//   ・NodeData::extra: G0 の値欄（PinValue）に載らない状態（文字列プロパティ = パラメータ名 / Custom の HLSL / テクスチャパス、
//     型の違うリテラル）を丸ごと運ぶ不透明な JSON。RemoveNode → Undo（AddNode）・コピペがこれで完全に元へ戻る。
//   ・NodeData::label: パラメータ名・テクスチャ名（ヘッダ直下の名前欄）。

#include "editor/nodegraph/IGraphModel.h"
#include "renderer/matgraph/GraphAnalysis.h"
#include "renderer/matgraph/GraphModel.h"

#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx12e::mg
{

namespace mat = dx12e::matgraph;
namespace ng = dx12e::ng;

// G0 の PinType（整数）の割り当て
enum PinTypes : int
{
    kPtF1 = 0, kPtF2, kPtF3, kPtF4, kPtTex, kPtBool, kPtAnyNum, kPtWild, kPtInt, kPtOther, kPtCount
};

ng::PinType PinTypeOf(mat::ValueType t);          // 型 → PinType（未対応の型は kPtOther）
const char* CategoryLabel(int slot);              // 色スロットの説明（テスト・ドキュメント用）
int CategorySlotOf(const std::string& category);  // G1 のカテゴリ名 → G0 のヘッダ色スロット（0..9）
char HotkeyOf(const std::string& nodeType);       // UE 風ワンキー（無ければ 0）

class MatGraphModel final : public ng::IGraphModel
{
public:
    explicit MatGraphModel(mat::MaterialGraph* graph);
    ~MatGraphModel() override;
    MatGraphModel(const MatGraphModel&) = delete;
    MatGraphModel& operator=(const MatGraphModel&) = delete;

    mat::MaterialGraph& Graph() { return *m_g; }
    const mat::MaterialGraph& Graph() const { return *m_g; }

    // ---- ID 変換 ----
    ng::NodeId  IntOf(const std::string& id) const;     // 無ければ割り当てる。文字列が空なら 0
    ng::NodeId  FindInt(const std::string& id) const;   // 割り当て済みのときだけ。無ければ 0
    std::string StrOf(ng::NodeId id) const;             // 割り当て表に無ければ空
    // グラフごと入れ替えた後（開く・新規）に呼ぶ。割り当て表と全キャッシュを捨てる（履歴も呼び出し側が消すこと）。
    void ResetMapping();

    // ---- IGraphModel ----
    const std::vector<ng::NodeTypeDesc>& NodeTypes() const override { return m_types; }
    const ng::NodeTypeDesc* FindNodeType(const std::string& id) const override;
    const ng::PinTypeDesc&  PinTypeInfo(ng::PinType t) const override;
    bool CanConnectTypes(ng::PinType from, ng::PinType to) const override;
    const char* RerouteTypeId() const override { return "Reroute"; }

    const std::vector<ng::NodeId>& Nodes() const override;
    const ng::NodeData* FindNode(ng::NodeId id) const override;
    const std::vector<ng::Edge>& Edges() const override;
    const ng::Edge* FindEdgeTo(ng::PinRef input) const override;
    ng::ConnectCheck CanConnect(ng::PinRef output, ng::PinRef input) const override;
    ng::PinType ResolvedPinType(ng::PinRef pin) const override;
    void Downstream(ng::NodeId n, std::vector<ng::NodeId>& out) const override;
    void Upstream(ng::NodeId n, std::vector<ng::NodeId>& out) const override;
    uint64_t Revision() const override { return m_g->Version(); }

    ng::NodeId AddNode(const ng::NodeData& d) override;
    bool   RemoveNode(ng::NodeId id) override;
    bool   Connect(ng::PinRef output, ng::PinRef input) override;
    bool   ConnectUnchecked(ng::PinRef output, ng::PinRef input) override;
    bool   Disconnect(ng::PinRef input) override;
    void   SetNodePos(ng::NodeId id, ng::Vec2 pos) override;
    void   SetPinValue(ng::PinRef input, const ng::PinValue& v) override;
    void   Clear() override;

    // ノードのプレビュー領域（本体下部の画像枠）を型ごとに有効 / 無効にする（プレビュー担当 G2c が TextureSample などで使う）。
    // 呼んだら Doc().Layout().Invalidate() でレイアウトを作り直すこと。既定はどの型も無し（ノードを小さく保つ）。
    void SetNodePreview(const std::string& typeId, bool on);

    // ---- G1 のピン名 ⇄ G0 のピン添字 ----
    // ok: 対応するピンがあるか。pin.output=false の添字がプロパティ行のときは ok=false（ワイヤを繋げない）。
    struct PinName { bool ok = false; std::string node, pin; };
    PinName NameOf(ng::PinRef pin) const;
    ng::PinRef PinOf(const std::string& node, const std::string& pinName, bool output) const;   // 無ければ node=0

    // ---- プロパティ（文字列を含む全プロパティ。値欄の行になっているものは SetPinValue でも編集できる）----
    // 現在の値（未設定なら宣言の既定値）。ノード / プロパティが無ければ null。
    mat::Json GetNodeProp(ng::NodeId id, const std::string& key) const;
    // 生の設定（Undo を積まない。G1 が正規化する。不正なら false）。
    bool SetNodeProp(ng::NodeId id, const std::string& key, const mat::Json& value);

    // ---- 診断（G1 の型推論 + 検証。グラフが変わるたびに 1 回だけ計算する。ノードの移動・コメントでは再計算しない）----
    const mat::GraphAnalysis& Analysis() const;
    uint64_t AnalysisTick() const { return m_tick; }   // Analysis() が変わりうる変更のたびに増える

    // ---- 名前欄・型情報の問い合わせ ----
    const mat::NodeDef* DefOf(ng::NodeId id) const;    // 未知の型なら nullptr
    // パラメータ名の重複を避けた名前（"Scalar" → "Scalar_1"）。exceptNode のパラメータ自身は数えない。
    std::string UniqueParamName(const std::string& base, ng::NodeId exceptNode = 0) const;

    // 入力ピン添字 → G1 のピン宣言（プロパティ行・範囲外は nullptr）/ プロパティ行の添字 → プロパティ名（ピンなら空）
    const mat::PinDecl* InputDecl(const ng::NodeTypeDesc& t, size_t uiIndex) const;
    std::string PropRowName(const ng::NodeTypeDesc& t, size_t uiIndex) const;

    // 値欄に載らない（文字列など）プロパティの一覧を、宣言順で返す（詳細パネル用）。
    struct PropInfo { const mat::PropDecl* decl = nullptr; bool inlineRow = false; };
    std::vector<PropInfo> PropsOf(ng::NodeId id) const;

    // 内部の不変条件の検査（テスト用）: 退避したリテラルは「接続中でリテラルなし」のピンにだけ存在する。違反があれば理由を返す。
    bool CheckInvariants(std::string* why) const;

private:
    struct TypeInfo
    {
        const mat::NodeDef* def = nullptr;
        std::vector<int>         inDef;    // UI 入力添字 → def->inputs の添字（プロパティ行は -1）
        std::vector<std::string> inProp;   // UI 入力添字 → プロパティ名（ピンは空）
        std::vector<int>         outDef;   // UI 出力添字 → def->outputs の添字
    };
    struct Entry { ng::NodeData data; bool valid = false; };

    void BuildCatalog();
    void OnChange(const mat::GraphChange& c);
    const TypeInfo* InfoFor(const ng::NodeTypeDesc* t) const;
    const ng::NodeTypeDesc* DescOfType(const std::string& type) const;   // 未知の型は合成した記述（ピンなし）を返す
    void FillEntry(const std::string& sid, Entry& e) const;
    std::string BuildExtra(const mat::Node& n) const;
    void ApplyExtra(const std::string& sid, const std::string& extra);
    void RebuildStructure() const;
    ng::NodeId Assign(const std::string& sid, ng::NodeId wanted) const;
    void Touch();

    mat::MaterialGraph* m_g = nullptr;
    mat::MaterialGraph::ListenerId m_listener = 0;

    std::vector<ng::NodeTypeDesc> m_types;
    std::vector<TypeInfo>         m_infos;                 // m_types と同じ添字
    std::unordered_map<std::string, int> m_typeIndex;
    mutable std::deque<ng::NodeTypeDesc> m_unknown;        // 未知の型の合成記述（アドレス安定）
    mutable std::deque<TypeInfo>         m_unknownInfo;
    mutable std::unordered_map<std::string, int> m_unknownIndex;
    ng::PinTypeDesc m_pinTypes[kPtCount];

    // ID 割り当て（mutable: 読み取り時に遅延割り当てする）
    mutable std::unordered_map<std::string, ng::NodeId> m_toInt;
    mutable std::unordered_map<ng::NodeId, std::string> m_toStr;
    mutable ng::NodeId m_nextInt = 1;

    // 構造キャッシュ（ノード追加・削除・接続の変化で作り直す）
    mutable bool m_structDirty = true;
    mutable std::vector<ng::NodeId> m_ids;
    mutable std::vector<ng::Edge>   m_edges;
    mutable std::unordered_map<ng::NodeId, std::vector<ng::NodeId>> m_down, m_up;

    // ノード単位のキャッシュ（プロパティ・リテラル・接続が変わったノードだけ作り直す。移動は位置だけ更新）
    mutable std::unordered_map<ng::NodeId, Entry> m_entries;
    mutable std::unordered_set<std::string> m_dirty;

    // 型推論（診断）
    mutable mat::GraphAnalysis m_an;
    mutable uint64_t m_anTick = ~0ull;
    uint64_t m_tick = 1;

    // CanConnect のメモ（多相ピンは G1 が「試しに繋いで再解析」するので、ドラッグ中の同じ組を繰り返し問わない）
    mutable uint64_t m_canRev = ~0ull;
    mutable std::unordered_map<uint64_t, ng::ConnectCheck> m_canCache;

    // 「接続で隠れたリテラル」。G1 は 1 つの入力にリンクとリテラルを同時に持てず、接続するとリテラルが消える。
    // そのままだと「値を入れる → 接続する → Undo」でリテラルが戻らない。接続で消えるリテラルをここへ退避し、切断（Undo）で戻す。
    // extra にも入るので、ノードの削除 → Undo でも失われない。キーは「ノード文字列 ID + 改行 + ピン名」。
    std::map<std::string, mat::Value> m_shadow;
};

// ---- 純関数（テスト用に公開）----
// G1 のリテラルを G0 の値欄の値へ（kind は値欄の種別。F1 のスカラーは全成分へ広げる）
ng::PinValue ToPinValue(const mat::Value& v, ng::ValueKind kind);
// 値欄の値 → G1 のリテラル（ピン型 t = 値欄が受けるピンの型。多相・Free のときは kind から決める）
mat::Value ToLiteral(const ng::PinValue& v, ng::ValueKind kind, mat::ValueType pinType);

} // namespace dx12e::mg
