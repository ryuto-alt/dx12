#pragma once
// ============================================================================
// 仮想ジオメトリ（VG）の自動 cook ― Blender などから持ってきたハイポリの .glb / .gltf / .fbx / .obj を「置くだけ」で VG にする。
//
//   ・読み込み時（ResourceManager::GetOrLoadModel）に Query で判定する。メインスレッドでは重い仕事をしない:
//       Ready      … assets/.modelcache/vg/ に cook 済みの .vgeo がある → そのプロキシ（≤ 25 万三角形）を読み、VG で描く
//       Pending    … 裏のスレッドで cook 中（またはこの呼び出しで始めた）→ 仮表示（箱 1 個）。終わったら PumpVgAuto が差し替える
//       Ineligible … 対象外（静的でない / 小さい / アルファ材質 / 頂点カラー / 材質が多い / import.json で off）→ 通常メッシュ
//       NotCandidate … 拡張子が違う / 自動 cook が無効 / 配布ゲーム → 通常メッシュ
//   ・cook は ModelLoader::ExtractStaticForVg（LoadFromFile と同じ import 設定・座標変換・ノードの焼き込み）→ VGSRC 相当
//     → tools/vgeo_cook の Cook（階層 LOD / クラスタ / 材質ごとのセクション）→ 厳密検証 → 書き出し。
//   ・鍵 = 元ファイルの中身のハッシュ（64bit）+ import.json の中身 + 自動 cook の版 + cook パラメータの正準文字列。
//     中身のハッシュは「パス + サイズ + 更新時刻」で覚えておく（m<パスのハッシュ>.json。毎回 600 MB を読まない）。
//   ・置き場: <assets>/.modelcache/vg/v<鍵>.vgeo と、そのテクスチャ v<鍵>/m<材質>_<a|n|r|e>.<拡張子>
//     （.vgeo の材質は「.vgeo のフォルダ基準の安全な相対パス」しか持てない（VGEO_SPEC C24）ので、glb の埋め込み画像は書き出し、
//      外部の画像は写す。BC 圧縮のキャッシュ（.texcache）は中身のハッシュで引くので、写しても圧縮はやり直さない）。
//   ・判定の純粋な部分（DecideIneligible / PrecheckGltfJson / ComputeKey）は tests/vg_auto_cook_test.cpp が見張る。
// ============================================================================
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "resource/ModelLoader.h"

namespace dx12e::vgauto
{

// 自動 cook の版。出力の作り方（取り出し・材質の詰め方・cook の既定値）を変えたら上げる（古い .vgeo は鍵が外れて作り直される）。
inline constexpr uint32_t kAutoCookVersion = 1;

struct Config
{
    bool     enabled      = true;      // settings.json の vg_auto（0 = 自動 cook しない）
    uint64_t minTriangles = 200000;    // settings.json の vg_auto_min_triangles（"auto" のモデルはこれ以上で cook）
    uint32_t maxMaterials = 64;        // "auto" で cook する材質数の上限（UE Nanite の 1 クラスタ 64 材質に合わせた目安）
    uint32_t threads      = 0;         // cook のスレッド数（0 = 論理コア / 4。上限 12）
};

// モデルの性質（glTF の JSON だけ / Assimp で読んだ結果から作る）
struct Facts
{
    uint64_t triangles   = 0;
    bool     skin        = false;
    bool     animation   = false;
    bool     vertexColor = false;
    bool     nonOpaque   = false;      // MASK / BLEND の材質を使っている
    uint32_t materials   = 0;          // 使われている材質の数
};

// 空 = cook してよい / 非空 = しない理由（日本語の短文。vg_stats / ログに出す）
std::string DecideIneligible(ModelImportOptions::VgMode mode, const Config& cfg, const Facts& f);

// glTF の JSON（.gltf の中身 / .glb の JSON チャンク）から Facts と POSITION の範囲（ノード変換は無視。仮表示用）を作る。
bool PrecheckGltfJson(const std::string& json, Facts& out, float bmin[3], float bmax[3], bool& hasBounds, std::string* err);
// .glb / .gltf のファイルから（.glb は先頭のヘッダと JSON チャンクだけ読む）
bool PrecheckGltfFile(const std::filesystem::path& path, Facts& out, float bmin[3], float bmax[3], bool& hasBounds, std::string* err);

// 鍵（64bit）。contentHash = 元ファイルの中身のハッシュ / importJson = import.json の中身（無ければ空）
uint64_t ComputeKey(uint64_t contentHash, const std::string& importJson, const std::string& cookParams);
// ファイルの中身のハッシュ（8 MiB ずつ読む。失敗は 0）
uint64_t HashFileContents(const std::filesystem::path& path, std::atomic<bool>* cancel = nullptr);

enum class State : uint8_t { NotCandidate, Ready, Pending, Ineligible, Failed };
const char* StateName(State s);

struct QueryResult
{
    State       state = State::NotCandidate;
    std::string vgeoPath;              // Ready のとき（絶対パス・"/" 区切り）
    std::string reason;                // Ineligible / Failed のとき
    bool  hasBounds = false;           // Pending の仮表示の大きさ（glTF の accessor から）
    float bmin[3] = {-0.5f, 0.0f, -0.5f};
    float bmax[3] = { 0.5f, 1.0f,  0.5f};
};

class Cooker
{
public:
    static Cooker& Get();
    ~Cooker();

    void   SetConfig(const Config& c);
    Config GetConfig() const;

    // 読み込み時の判定（メインスレッド・安い。必要なら裏の cook を始める）。model は絶対パス。
    QueryResult Query(const std::filesystem::path& model);
    // 手動 cook（MCP vg_cook）。しきい値・材質数を見ない（import.json の off も無視）。force = 鍵が同じでも作り直す。
    bool Request(const std::filesystem::path& model, bool force, std::string* err);
    // 終わった（成功 / 対象外 / 失敗）cook のモデルのパス（Query / Request に渡した綴りを正規化したもの）。メインスレッドで毎フレーム。
    std::vector<std::string> TakeFinished();
    // 状態（vg_stats.autoCook）
    nlohmann::json StatusJson() const;
    uint32_t ActiveJobs() const;
    void Shutdown();                   // 裏の cook を止める（終了時。cook の途中なら切り離す = 待たない）

private:
    Cooker() = default;
    struct Job
    {
        std::string model;             // 正規化した絶対パス（generic）
        bool        manual = false;
        bool        force  = false;
        // 進捗（StatusJson が読む。stage は mutex 越し）
        std::string stage = "queued";
        std::chrono::steady_clock::time_point t0{};
        std::chrono::steady_clock::time_point tEnd{};
        State       result = State::Pending;
        std::string reason, vgeoPath;
        uint64_t    triangles = 0, clusters = 0, fileBytes = 0;
        uint32_t    materials = 0, levels = 0;
        double      secHash = 0, secRead = 0, secCook = 0, secWrite = 0;
        bool        done = false;
    };
    void WorkerMain();
    void RunJob(Job& j);
    void SetStage(Job& j, const char* s);
    void StartJobLocked(const std::string& model, bool manual, bool force);

    mutable std::mutex           m_mutex;
    Config                       m_cfg;
    std::deque<std::shared_ptr<Job>> m_queue;
    std::unordered_map<std::string, std::shared_ptr<Job>> m_active;   // 正規化パス → 実行中 / 待ち
    std::vector<std::shared_ptr<Job>> m_history;                      // 終わった物（最新 32 件）
    std::vector<std::string>     m_finished;                          // TakeFinished 用
    std::unordered_map<std::string, std::pair<std::string, std::string>> m_ineligibleCache;   // 正規化パス → (サイズ+時刻, 理由)
    std::condition_variable      m_cv;
    std::thread                  m_worker;
    std::atomic<bool>            m_quit{false};
    std::atomic<bool>            m_cancel{false};
};

} // namespace dx12e::vgauto
