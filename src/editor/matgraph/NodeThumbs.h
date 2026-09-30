#pragma once

// ===== ノード内サムネイルの管理（マテリアルグラフ G2c。ImGui / GPU 非依存の純ロジック）=====
//   どのノードに・どのタイルを割り当て・いつ作り直すか、だけを決める。GPU（GraphMaterialSystem のインスタンス + GraphPreviewRenderer のタイル描画）とは
//   NodeThumbBackend の 2 関数で繋がる（テストは偽物を渡す）。
//
//   ・可視ノードだけ: 画面内（呼び出し側が渡す visible）で、サムネイルを持つ型のノードだけを対象にする。画面外へ出たものは一定フレーム後にタイルを解放する
//     （LRU）。常駐の上限は maxTiles（アトラスの 64 枚まで）。上限を超える分は「出さない」（ノードの枠は予約済みなのでプレースホルダー）。
//   ・更新は変更があったノードとその下流だけ: MaterialGraph の変更通知（どのノードが変わったか）を溜め、サムネイルの上流（前回 + 今回）に
//     変更ノードが含まれるものだけ部分グラフを作り直す。上流が変わらないノードは CompileGraph すら呼ばない。作り直した結果が前回と同じ
//     （HLSL のハッシュ + スロット値が同じ）なら GPU へも送らない。
//   ・値だけの変更は HLSL が変わらないので、GPU 側は再コンパイルなし（レコードを書き直して再描画するだけ）。構造の変更は新しい変種をワーカーが作る。
//   ・1 フレームの作り直し数に上限（maxCompilesPerFrame）: 大きなグラフを開いた直後でも UI を止めない。残りは次のフレームへ持ち越す。
//   ・エラー（上流のエラー / 型が決まらない）はタイルを割り当てず、理由を保持する（ノード上は「?」のプレースホルダー）。

#include "editor/matgraph/MatGraphEditor.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dx12e::mg
{

class NodeThumbBackend
{
public:
    virtual ~NodeThumbBackend() = default;
    // key のサムネイル用インスタンスへ部分グラフのコンパイル結果を渡す（HLSL が変わらなければ値の更新だけで済む）
    virtual void SetCompiled(const std::string& key, std::shared_ptr<const mat::CompileResult> cr) = 0;
    // key のインスタンスを捨てる
    virtual void Remove(const std::string& key) = 0;
};

class NodeThumbs
{
public:
    struct Config
    {
        int maxTiles = 48;              // 常駐の上限（GraphPreviewRenderer のアトラスは 64 枚）
        int maxCompilesPerFrame = 6;    // 1 フレームに作り直す部分グラフの数
        int evictAfterFrames = 90;      // 画面外になってからタイルを解放するまで
    };
    struct Stats
    {
        int resident = 0;               // タイルを持っているノード数
        int wanted = 0;                 // 直近の Update で対象になった可視ノード数
        int compiledThisFrame = 0;      // 直近の Update で作り直した数
        int drawPending = 0;            // GPU に描いてもらう待ち
        uint64_t compilesTotal = 0;     // 部分グラフの CompileGraph 回数（累計）
        uint64_t sentTotal = 0;         // GPU（バックエンド）へ送った回数（累計）
        uint64_t skippedSame = 0;       // 作り直したが前回と同じ（送らなかった）回数
        uint64_t evicted = 0;           // 解放したタイル数（累計）
        uint64_t overBudget = 0;        // 上限で出せなかった回数（累計）
    };
    struct TileWork
    {
        int         tile = 0;
        std::string key;
    };
    enum class State : uint8_t { None, Pending, Ready, Error };

    NodeThumbs();
    ~NodeThumbs();
    NodeThumbs(const NodeThumbs&) = delete;
    NodeThumbs& operator=(const NodeThumbs&) = delete;

    void SetConfig(const Config& c) { m_cfg = c; }
    const Config& GetConfig() const { return m_cfg; }
    void SetEnabled(bool on) { m_enabled = on; }
    bool Enabled() const { return m_enabled; }

    // サムネイルを出す型か（テクスチャ / 色 / 数値）
    static bool WantsThumb(const mat::NodeDef* def);
    // その型を持つノードのプレビュー領域を予約する（MatGraphModel::SetNodePreview を型ごとに呼ぶ）
    static void ReserveInModel(MatGraphModel& model);

    // 対象のグラフ（変更通知を購読する）。グラフの寿命はこのオブジェクトより長いこと。null で解除
    void Attach(mat::MaterialGraph* g);

    // 毎フレーム 1 回。visible = 画面内のノード（G0 の NodeId）。戻り値 = このフレームに GPU で描く（描き直す）タイル
    // （まだ GPU が使えなくて描けなかった分も毎フレーム含まれる。描けたら MarkDrawn で外す）
    std::vector<TileWork> Update(MatGraphEditor& ed, const std::vector<ng::NodeId>& visible, NodeThumbBackend& backend, uint64_t frame);
    void MarkDrawn(int tile);

    // 描画側の問い合わせ（ノードごと）
    State StateOf(ng::NodeId id) const;
    int TileOf(ng::NodeId id) const;                 // 無ければ -1
    std::string ErrorOf(ng::NodeId id) const;
    // 全部捨てる（グラフを開き直した / 新規）。バックエンドのインスタンスも消す
    void Reset(NodeThumbBackend* backend);
    // 全ノードを作り直し対象にする（エンジンのシェーダー更新後など）
    void InvalidateAll();

    Stats GetStats() const { return m_stats; }
    static std::string KeyFor(const std::string& nodeStr) { return "__mgnode/" + nodeStr; }

private:
    struct Entry
    {
        int         tile = -1;
        mat::NodeId nodeStr;
        std::string key;
        State       state = State::None;
        std::string error;
        uint64_t    sig = 0;
        bool        needCompile = true;
        bool        needDraw = false;
        bool        sent = false;                   // バックエンドに 1 回でも送った
        std::set<mat::NodeId> anc;                  // 前回コンパイルしたときの上流（自分を含む）
        uint64_t    lastWanted = 0;
    };

    void OnChange(const mat::GraphChange& c);
    int  AllocTile();
    void FreeEntry(std::map<ng::NodeId, Entry>::iterator it, NodeThumbBackend* be);
    static void CollectAncestors(const mat::MaterialGraph& g, const mat::NodeId& node, std::set<mat::NodeId>& out);

    Config m_cfg;
    bool   m_enabled = true;
    mat::MaterialGraph* m_graph = nullptr;
    mat::MaterialGraph::ListenerId m_listener = 0;
    std::map<ng::NodeId, Entry> m_entries;
    std::vector<int> m_freeTiles;                   // 空きタイル（昇順に使う）
    std::set<mat::NodeId> m_dirty;                  // 変更通知で溜めた（変更のあったノード）
    bool m_dirtyAll = true;
    Stats m_stats;
    std::set<int> m_needDraw;
};

} // namespace dx12e::mg
