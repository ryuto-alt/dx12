#pragma once

// ===========================================================================
// 起動画面の「実進捗」の純ロジック（ヘッダオンリー。Win32 に依存しない）
// ---------------------------------------------------------------------------
// ・初期化を重み付きの段階（Stage）に分け、いまの段階＋段階内の進み具合(0..1)から全体進捗 0..1 を出す。
// ・段階内の進み具合が報告されない段階（更新確認 = WinHTTP で最大数秒 / シェーダ構築 など）は、
//   段階の 90% までを指数で「微進み」させる。止まって見えず、かつ実際の完了までは 100% に達しない。
// ・全体進捗は単調増加（後退しない）。上限は kMaxProgress（100% は起動完了の演出だけが出す）。
// ・前回の起動所要時間の指数移動平均（EMA）と startup.json の読み書き（起動音の適応同期が使う）。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dx12e::splash
{

// ---------------------------------------------------------------- 段階

enum class Stage : int
{
    // --- エディタ起動（main.cpp / Application::Initialize）。実行順
    UpdateCheck = 0,   // アップデートを確認中
    Window,            // ウィンドウを作成中
    Graphics,          // グラフィックスデバイス
    Audio,             // オーディオ
    Physics,           // 物理エンジン
    Shaders,           // シェーダーとパイプライン
    Assets,            // アセット（ResourceManager / Scene）
    Scripts,           // スクリプトエンジン
    ShadowMap,         // シャドウマップ
    EditorUi,          // エディタ UI（ImGui）
    Renderer,          // レンダラー（RT・ポスト）
    Thumbnails,        // モデルのサムネイル（未キャッシュ分だけ。件数で進む）
    EnvMap,            // 環境マップのベイク
    Finalize,          // 画面の準備
    // --- プロジェクトを開く（ApplicationProject.cpp）。--project 直開きでは起動の続きに並ぶ
    ProjectCreate,     // プロジェクトを作成中（新規のみ）
    ProjectScene,      // シーンの解析
    ProjectAssets,     // アセット読込（件数で進む）
    ProjectMaterials,  // マテリアルのサムネイル（件数で進む）
    ProjectFinalize,   // 仕上げ
    Count
};

// 段階の名前（診断ログ用）と、既定の状態文言（UTF-8・標準語）。Application は必要に応じて文言を上書きできる。
inline const char* StageName(Stage s)
{
    switch (s)
    {
    case Stage::UpdateCheck: return "UpdateCheck";     case Stage::Window: return "Window";
    case Stage::Graphics: return "Graphics";           case Stage::Audio: return "Audio";
    case Stage::Physics: return "Physics";             case Stage::Shaders: return "Shaders";
    case Stage::Assets: return "Assets";               case Stage::Scripts: return "Scripts";
    case Stage::ShadowMap: return "ShadowMap";         case Stage::EditorUi: return "EditorUi";
    case Stage::Renderer: return "Renderer";           case Stage::Thumbnails: return "Thumbnails";
    case Stage::EnvMap: return "EnvMap";               case Stage::Finalize: return "Finalize";
    case Stage::ProjectCreate: return "ProjectCreate"; case Stage::ProjectScene: return "ProjectScene";
    case Stage::ProjectAssets: return "ProjectAssets"; case Stage::ProjectMaterials: return "ProjectMaterials";
    case Stage::ProjectFinalize: return "ProjectFinalize";
    default: return "?";
    }
}
inline const char* StageLabelUtf8(Stage s)
{
    switch (s)
    {
    case Stage::UpdateCheck: return "アップデートを確認中...";
    case Stage::Window: return "ウィンドウを作成中...";
    case Stage::Graphics: return "グラフィックスデバイスを初期化中...";
    case Stage::Audio: return "オーディオを初期化中...";
    case Stage::Physics: return "物理エンジンを初期化中...";
    case Stage::Shaders: return "シェーダーとパイプラインを構築中...";
    case Stage::Assets: return "アセットを読み込み中...";
    case Stage::Scripts: return "スクリプトエンジンを初期化中...";
    case Stage::ShadowMap: return "シャドウマップを準備中...";
    case Stage::EditorUi: return "エディタUIを初期化中...";
    case Stage::Renderer: return "レンダラーを初期化中...";
    case Stage::Thumbnails: return "サムネイルを生成中...";
    case Stage::EnvMap: return "環境マップをベイク中...";
    case Stage::Finalize: return "画面を準備しています...";
    case Stage::ProjectCreate: return "プロジェクトを作成中...";
    case Stage::ProjectScene: return "シーンを読み込み中...";
    case Stage::ProjectAssets: return "アセットを読み込み中...";
    case Stage::ProjectMaterials: return "マテリアルを読み込み中...";
    case Stage::ProjectFinalize: return "仕上げています...";
    default: return "";
    }
}

struct StageWeight
{
    Stage  stage;
    double weight;     // 相対重み（合計で正規化する）
    double creepTau;   // 段階内の報告が無いときの微進みの時定数（秒）
};

// 重み（合計 1.0）。2026-09-30 の実測（--background・ウォームキャッシュ・小さなプロジェクト）から起こした値:
//   段階の所要 ms: Window 16 / Graphics 250 / Audio 31 / Physics 1 / Shaders 47 / Assets 62 / Scripts 78 /
//   ShadowMap 110 / EditorUi 78 / Renderer 609 / EnvMap 16 / Finalize+読込 170（合計 約 1.5 秒）。
//   ・更新確認は開発ツリーでは 0ms、配布版では WinHTTP で数百 ms〜数秒（読めないので小さめ + 遅い微進み）。
//   ・Shaders / Thumbnails / EnvMap は実測が小さいが、コールドスタート（初回・キャッシュ無し）で膨らむので余裕を持たせる。
//   実測は Application が SetStage の開始時刻を Info ログ（「スプラッシュ: stage X @ N ms」）へ出すので、
//   重みを直すときはそのログの差分を見ること。
inline std::vector<StageWeight> StartupPlan(bool withProject)
{
    std::vector<StageWeight> p = {
        { Stage::UpdateCheck, 0.07, 2.5 },
        { Stage::Window,      0.02, 0.2 },
        { Stage::Graphics,    0.15, 0.6 },
        { Stage::Audio,       0.02, 0.2 },
        { Stage::Physics,     0.01, 0.2 },
        { Stage::Shaders,     0.05, 0.6 },
        { Stage::Assets,      0.04, 0.4 },
        { Stage::Scripts,     0.05, 0.5 },
        { Stage::ShadowMap,   0.06, 0.6 },
        { Stage::EditorUi,    0.05, 0.5 },
        { Stage::Renderer,    0.34, 0.9 },
        { Stage::Thumbnails,  0.03, 1.5 },
        { Stage::EnvMap,      0.05, 0.5 },
        { Stage::Finalize,    0.06, 0.4 },
    };
    if (withProject)
    {
        p.push_back({ Stage::ProjectScene,     0.10, 0.6 });
        p.push_back({ Stage::ProjectAssets,    0.55, 2.0 });
        p.push_back({ Stage::ProjectMaterials, 0.15, 1.2 });
        p.push_back({ Stage::ProjectFinalize,  0.05, 0.4 });
    }
    return p;
}

// プロジェクトを開く単独のスプラッシュ（ランチャーから開いた場合）。isNew のときだけ作成段階が付く。
inline std::vector<StageWeight> ProjectLoadPlan(bool isNew)
{
    std::vector<StageWeight> p;
    if (isNew) p.push_back({ Stage::ProjectCreate, 0.10, 0.8 });
    p.push_back({ Stage::ProjectScene,     0.10, 0.6 });
    p.push_back({ Stage::ProjectAssets,    0.55, 2.0 });
    p.push_back({ Stage::ProjectMaterials, 0.20, 1.2 });
    p.push_back({ Stage::ProjectFinalize,  0.05, 0.4 });
    return p;
}

// ---------------------------------------------------------------- 進捗の追跡

class ProgressTracker
{
public:
    static constexpr double kMaxProgress = 0.995;   // 段階だけで進む限界。1.0 は ready の演出が出す
    static constexpr double kCreepMax    = 0.90;    // 段階内の微進みの上限（段階の 90%）

    void Configure(std::vector<StageWeight> plan)
    {
        plan_ = std::move(plan);
        total_ = 0.0;
        for (const auto& s : plan_) total_ += s.weight;
        cur_ = -1;
        sub_ = -1.0;
        absolute_ = -1.0;
        high_ = 0.0;
        stageStart_ = 0.0;
    }

    // 段階に入る（now: 経過秒）。計画に無い段階は無視する（＝ラベルだけ変えたい呼び出しは SetStatus を使う）。
    // 逆戻り（すでに過ぎた段階への再入）は無視。
    bool Begin(Stage s, double now)
    {
        for (size_t i = 0; i < plan_.size(); ++i)
        {
            if (plan_[i].stage != s) continue;
            if (static_cast<int>(i) < cur_) return false;
            if (static_cast<int>(i) == cur_) return true;
            cur_ = static_cast<int>(i);
            sub_ = -1.0;
            absolute_ = -1.0;
            stageStart_ = now;
            return true;
        }
        return false;
    }

    // 現在の段階内の進み具合 0..1（件数など実測できるとき）。
    void SetSub(double f)
    {
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        if (f > sub_) sub_ = f;
    }
    // 段階を無視して全体進捗を直接指定（SetProgress API）。次の Begin まで有効。
    void SetAbsolute(double v)
    {
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        absolute_ = v;
    }

    // 全体進捗 0..kMaxProgress。単調増加。
    double Value(double now)
    {
        double v = 0.0;
        if (absolute_ >= 0.0)
        {
            v = absolute_;
        }
        else if (cur_ >= 0 && total_ > 0.0)
        {
            double base = 0.0;
            for (int i = 0; i < cur_; ++i) base += plan_[static_cast<size_t>(i)].weight;
            const StageWeight& s = plan_[static_cast<size_t>(cur_)];
            double sub = sub_;
            if (sub < 0.0)
            {
                const double el = std::max(0.0, now - stageStart_);
                sub = kCreepMax * (1.0 - std::exp(-el / std::max(0.05, s.creepTau)));
            }
            v = (base + s.weight * sub) / total_;
        }
        if (v > kMaxProgress) v = kMaxProgress;
        if (v > high_) high_ = v;
        return high_;
    }

    int StepIndex() const { return cur_ < 0 ? 0 : cur_ + 1; }        // 1 始まり
    int StepTotal() const { return static_cast<int>(plan_.size()); }
    int CurrentStageIndex() const { return cur_; }
    size_t PlanSize() const { return plan_.size(); }
    double StageStart() const { return stageStart_; }

private:
    std::vector<StageWeight> plan_;
    double total_ = 0.0;
    int    cur_ = -1;
    double sub_ = -1.0;
    double absolute_ = -1.0;
    double high_ = 0.0;
    double stageStart_ = 0.0;
};

// ---------------------------------------------------------------- 起動所要時間（startup.json）

constexpr double kStartupDefaultMs = 3000.0;
constexpr double kStartupMinMs = 1500.0;
constexpr double kStartupMaxMs = 6000.0;
constexpr double kStartupEmaAlpha = 0.4;     // 新しい実測の重み（直近寄り。環境が変わったら 2〜3 回で追従）

inline double ClampStartupMs(double ms)
{
    if (!(ms == ms) || ms <= 0.0 || ms > 1e9) return kStartupDefaultMs;   // NaN / 0 以下 / 桁外れ → 既定
    return ms < kStartupMinMs ? kStartupMinMs : (ms > kStartupMaxMs ? kStartupMaxMs : ms);
}

// 指数移動平均。prevMs が無効（<=0 / NaN）なら実測をそのまま初期値にする。実測は 1500〜6000 に丸めてから混ぜる。
inline double UpdateStartupEma(double prevMs, double measuredMs)
{
    const double m = ClampStartupMs(measuredMs);
    if (!(prevMs == prevMs) || prevMs <= 0.0) return m;
    const double p = ClampStartupMs(prevMs);
    return ClampStartupMs(p + (m - p) * kStartupEmaAlpha);
}

// {"startupMs": 3123.4} を読む。壊れていたら既定 3000。値は 1500〜6000 にクランプ。
inline double ParseStartupJson(std::string_view text)
{
    const size_t k = text.find("\"startupMs\"");
    if (k == std::string_view::npos) return kStartupDefaultMs;
    size_t c = text.find(':', k);
    if (c == std::string_view::npos) return kStartupDefaultMs;
    ++c;
    while (c < text.size() && (text[c] == ' ' || text[c] == '\t' || text[c] == '\r' || text[c] == '\n')) ++c;
    const std::string num(text.substr(c, 32));
    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (end == num.c_str()) return kStartupDefaultMs;
    return ClampStartupMs(v);
}

inline std::string FormatStartupJson(double emaMs)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\n  \"startupMs\": %.1f\n}\n", ClampStartupMs(emaMs));
    return buf;
}

} // namespace dx12e::splash
