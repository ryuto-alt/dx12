#pragma once

// ===== マテリアルグラフ エディタ文書（純ロジック。ImGui 非依存）=====
// 1 つのグラフを編集するために必要なものを束ねる: G1 の MaterialGraph・G0 への適合（MatGraphModel）・G0 の文書（GraphDocument。
// コメントと Undo/Redo）・保存 / 読込（.dxmg）・未保存判定・コンパイル結果（HLSL / 診断 / スロット）・パラメータ昇格などの編集操作。
// パネル（MaterialGraphPanel）は描画と入力だけを持ち、状態はすべてここ。tests/matgraph_editor_test.cpp が単体で検証する。
//
// ★保存の方針: ノードの位置 / コメント / ズーム・パンは G1 の .dxmg（layout / comments / view）へ入れる。G0 の SaveGraphJson（整数 ID）は
//   クリップボード専用。ID は文字列（"n_071829"）が正で、整数はセッション内だけ。
// ★Undo は G0 の独立スタック（GraphHistory）。保存では消さない。開く / 新規で消す。
// ★未保存判定は「保存時の正準テキスト（view を除く）」との比較。Undo で保存状態へ戻ればマークも消える。

#include "editor/matgraph/MatGraphModel.h"
#include "editor/nodegraph/GraphDocument.h"
#include "editor/nodegraph/GraphIO.h"
#include "renderer/matgraph/Compiler.h"

#include <memory>
#include <string>
#include <vector>

namespace dx12e::mg
{

// 診断リストの 1 行（G1 の Diagnostic + UI が引く情報）
struct DiagItem
{
    mat::Severity severity = mat::Severity::Error;
    std::string   code;
    ng::NodeId    node = 0;          // 0 = グラフ全体（フォーカス先なし）
    std::string   nodeTitle;         // "Multiply" / パラメータ名つきなら "ScalarParameter \"Roughness\""
    std::string   pin;
    std::string   message;
    std::string   hint;
    bool          reachable = true;
};

// 新規グラフのひな形
enum class NewTemplate { Empty, StandardPbr, Sample, Big200 };   // Big200 = 実測ゲート用の約 200 ノード（BuildBigGraph）

class MatGraphEditor
{
public:
    MatGraphEditor();
    ~MatGraphEditor();
    MatGraphEditor(const MatGraphEditor&) = delete;
    MatGraphEditor& operator=(const MatGraphEditor&) = delete;

    // ---- 部品 ----
    mat::MaterialGraph& Graph() { return *m_graph; }
    const mat::MaterialGraph& Graph() const { return *m_graph; }
    MatGraphModel&      Model() { return *m_model; }
    const MatGraphModel& Model() const { return *m_model; }
    ng::GraphDocument&  Doc() { return *m_doc; }
    const ng::GraphDocument& Doc() const { return *m_doc; }

    // ---- 文書 ----
    // 新規（履歴も消す）。出力ノード（MaterialOutput）だけを持つグラフを作る。ファイルパスは無し。
    void NewGraph(NewTemplate t = NewTemplate::Empty);
    bool Open(const std::string& path, std::string* error);
    // path が空なら現在のパスへ。パスが無ければ false（呼び出し側が SaveAs へ）。view = 保存するパン・ズーム（null なら前回の値のまま）
    bool Save(std::string* error, const ng::ViewState* view = nullptr);
    bool SaveAs(const std::string& path, std::string* error, const ng::ViewState* view = nullptr);
    const std::string& Path() const { return m_path; }
    bool HasPath() const { return !m_path.empty(); }
    std::string DisplayName() const;   // 「rock_wet.dxmg」/「名称未設定」
    bool IsDirty() const;
    // 開いた / 新規直後にビューへ渡すパン・ズーム（.dxmg の view）。読み出すと消費される。
    bool TakeLoadedView(ng::ViewState* out);

    // ---- フレームごとの更新（コメントの同期・再コンパイル・診断の更新）。パネルが毎フレーム 1 回呼ぶ ----
    void Update();

    // ---- コンパイル ----
    const mat::CompileResult& Result() const { return m_result; }
    bool HasResult() const { return m_hasResult; }
    int  CompileCount() const { return m_compileCount; }            // これまでに HLSL を生成した回数（値のみの変更では増えない）
    double LastCompileMs() const { return m_lastCompileMs; }
    int  ValueOnlyEditsSinceCompile() const { return m_valueOnlyEdits; }   // 直近のコンパイル以降の「値だけの変更」の数（HLSL は不変）
    uint64_t ValueOnlyEditSeq() const { return m_valueOnlySeq; }           // 値だけの変更のたびに増える（パネルの表示アニメ用）
    int  ErrorCount() const { return m_errors; }
    int  WarningCount() const { return m_warnings; }
    // 診断（到達可能なものを先に。エラー → 警告 → 情報）。includeInfo = false で「出力に繋がらないノード」等の情報を除く
    std::vector<DiagItem> Diagnostics(bool includeInfo) const;
    // ★G2c: ランタイム（DXC / PSO）が返したエラーをノードへ逆引きした診断。Diagnostics() に足される（赤枠 + ツールチップ + 診断リストからジャンプ）。
    //   空を渡すと消える。グラフ側の診断（ErrorCount / WarningCount）には数えない。
    void SetExternalDiagnostics(std::vector<mat::Diagnostic> d);
    uint64_t ExternalDiagRev() const { return m_extRev; }
    int ExternalErrorCount() const;
    // 診断 → フォーカスするノード（グラフ全体の診断は出力ノード。無ければ 0）
    ng::NodeId FocusNodeFor(const DiagItem& d) const;
    // 強制的に再コンパイル（テスト・「再生成」ボタン）
    void Recompile();

    // ---- パラメータ一覧（名前 / 型 / 既定値 / グループ）----
    std::vector<mat::ParamDecl> Parameters() const;

    // ---- 編集（Undo を積む）----
    // プロパティの変更。値は G1 が正規化する（不正なら false）。連続入力は呼び出し側で「確定時に 1 回」にすること。
    bool SetNodeProp(ng::NodeId id, const std::string& key, const mat::Json& value);
    bool SetSettings(const mat::GraphSettings& s);
    // ドラッグ中などの連続編集: 生の変更（Undo を積まない）→ 確定で 1 コマンド（変化なしなら何も積まない）
    bool SetPropLive(ng::NodeId id, const std::string& key, const mat::Json& value);
    bool CommitProp(ng::NodeId id, const std::string& key, const mat::Json& oldValue);
    void SetSettingsLive(const mat::GraphSettings& s) { m_graph->SetSettings(s); }
    bool CommitSettings(const mat::GraphSettings& oldValue);
    // 定数 / 暗黙テクスチャをパラメータへ昇格（Undo 1 回で戻る）。newNode に新しいノード（テクスチャは新しい TextureParameter）を返す
    bool CanPromote(ng::NodeId id, std::string* whyNot = nullptr) const;
    bool PromoteToParameter(ng::NodeId id, ng::NodeId* newNode = nullptr);
    // Custom / プロパティ編集は SetProp を使う。ノードの複製 / 削除 / 接続は Doc() の編集 API（Undo つき）。

    // 出力ノード（無ければ 0）
    ng::NodeId OutputNode() const;

    // ---- コメントの同期（GraphDocument ⇄ MaterialGraph のコメント。保存 / 読込のたびに内部で呼ぶ）----
    void SyncCommentsToGraph();
    void SyncCommentsFromGraph();

    // ---- 検証・テスト用 ----
    // ID 採番を決定的にする
    void SetIdSeed(uint32_t seed) { m_graph->SetIdSeed(seed); }
    // 保存されるテキスト（view を含む / 含まない）
    std::string CanonicalText(bool includeView) const;
    // 状態の同一性（G0 の Signature + コメント + 設定）。Undo ファズ・往復テスト用
    std::string Signature() const;

private:
    void Recheck();
    void MarkSaved();
    bool WriteTo(const std::string& path, std::string* error);

    std::unique_ptr<mat::MaterialGraph> m_graph;
    std::unique_ptr<MatGraphModel>      m_model;
    std::unique_ptr<ng::GraphDocument>  m_doc;

    std::string m_path;
    std::string m_savedText;               // 保存した時点の正準テキスト（view なし）
    mutable uint64_t m_dirtyVersion = ~0ull;
    mutable uint64_t m_dirtyCommentRev = ~0ull;
    mutable bool m_dirtyCache = false;

    uint64_t m_syncedCommentRev = ~0ull;
    bool m_hasLoadedView = false;
    ng::ViewState m_loadedView;

    // コンパイル
    mat::CompileResult m_result;
    bool     m_hasResult = false;
    uint64_t m_compiledStruct = ~0ull;
    uint64_t m_diagTick = ~0ull;
    uint64_t m_lastVersion = 0;
    std::vector<mat::Diagnostic> m_diags;
    std::vector<mat::Diagnostic> m_extDiags;   // G2c: DXC / PSO のエラー（ランタイム由来）
    uint64_t m_extRev = 0;
    int      m_compileCount = 0;
    double   m_lastCompileMs = 0.0;
    int      m_valueOnlyEdits = 0;
    uint64_t m_valueOnlySeq = 0;
    int      m_errors = 0, m_warnings = 0;
};

// ---- 純関数 ----
// ひな形のグラフ（Empty = 出力ノードだけ / StandardPbr = ベースカラー・ラフネス・ノーマルを持つ標準構成）
void BuildTemplate(mat::MaterialGraph& g, NewTemplate t);
// 見本（設計書の合否用）: ベースカラー = テクスチャ × 色、ラフネス = ノイズ、ノーマル、フレネル → エミッシブ。約 30 ノード + コメント。
void BuildSampleGraph(mat::MaterialGraph& g);
// ★G2c（実測ゲート / ベンチ用）: 見本 + テクスチャ x 定数 → Lerp の枝を足して約 targetNodes 個のグラフにする。salt でリテラルが変わる = HLSL が変わる。
void BuildBigGraph(mat::MaterialGraph& g, int targetNodes, float salt);
// Metallic に「リテラル同士の Multiply」を挟む（saltm ノード）。salt ごとに生成される HLSL のテキストが変わる（構造の編集の代役）。
void SaltGraph(mat::MaterialGraph& g, float salt);

// コメント色（6 色）と .dxmg の "#rrggbb"
const char* CommentHex(int colorIndex);
int CommentColorFromHex(const std::string& hex);

} // namespace dx12e::mg
