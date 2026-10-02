#pragma once

// ===== ゲームビルドの裏ジョブ =====
// BuildGame は大きなプロジェクトの初回ビルドで 1〜2 分かかる（テクスチャの BC 圧縮の焼き込み）。
// これを UI スレッドで同期実行するとエディタが固まるので、入力の取り込み（メインスレッドだけが触れる
// Application の状態 → BuildInput）と、実際の書き出し（ファイル IO と子プロセスの待ち = ワーカー）を分ける。
//   ・ワーカーは Application / PathResolver / エディタの状態を一切読まない（BuildInput に写した値だけ使う）。
//   ・書き出しは保存済みのディスクの内容が対象。ビルド中のシーン編集（メモリ上の未保存の変更）は配布物に入らない。
//   ・同時に走らせるビルドは 1 本（二重起動は拒否）。キャンセルは段の切れ目とファイルごと、焼き込みの子プロセスは即終了。
//   ・進捗は BuildProgress（アトミック + 文字列）を UI / MCP が読むだけ。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dx12e
{

// メインスレッドで集めたビルドの入力。ワーカーはこれだけを見る。
struct BuildInput
{
    // ビルド設定
    std::string title;
    int         winW = 1280, winH = 720;
    std::string startSceneRel = "scenes/default.json";
    bool        openFolderAfter = false;
    bool        fromUi = false;                 // UI のボタンから（完了時に Explorer を開く）。MCP / CLI は false

    // 出力
    std::filesystem::path outputDir;
    std::string           productName;
    std::string           exeName;

    // 取り込むもの（プロジェクトを開き直しても変わらないよう、開始時の絶対パスを固定する）
    std::filesystem::path exeDir;               // GameRuntime.exe と DLL のある場所
    std::filesystem::path assetsDir, scriptsDir, shadersDir;
    std::filesystem::path bindingsSrc, persistSrc;
    std::string           bundledFont, bundledFontLicense;   // エンジン同梱の日本語フォントとそのライセンス文（絶対パス。無ければ空）

    // メインスレッドで先に DXC を通した成果物（ShaderManager / GraphMaterialSystem はスレッドをまたいで触らない）
    std::unordered_map<std::string, std::vector<uint8_t>> shaderOverrides;   // shaders/ 相対 → 上書き .cso
    struct Blob { std::string rel; std::vector<uint8_t> bytes; };
    std::vector<Blob> blobs;                                                   // カスタムシェーダー・グラフ材質
};

// 進捗と結果。ワーカーが書き、UI / MCP が読む。
struct BuildProgress
{
    enum State : int { Idle = 0, Running, Succeeded, Failed, Cancelled };

    static constexpr int kStageCount = 5;
    static const char* StageName(int stage)   // 1 起算
    {
        switch (stage)
        {
        case 1: return "準備";
        case 2: return "ランタイムのコピー";
        case 3: return "アセットのパック";
        case 4: return "テクスチャの事前生成";
        case 5: return "仕上げ";
        default: return "";
        }
    }
    // 各段の全体に占める割合（%）。段 4 は進捗が読めないので経過時間で動く不定バー。
    static float StageStartPct(int stage)
    {
        static const float s[kStageCount + 1] = {0.0f, 0.0f, 2.0f, 5.0f, 40.0f, 97.0f};
        return s[stage < 1 ? 1 : (stage > kStageCount ? kStageCount : stage)];
    }
    static float StageEndPct(int stage)
    {
        static const float e[kStageCount + 1] = {0.0f, 2.0f, 5.0f, 40.0f, 97.0f, 100.0f};
        return e[stage < 1 ? 1 : (stage > kStageCount ? kStageCount : stage)];
    }

    std::atomic<int>  state{Idle};
    std::atomic<bool> cancelRequested{false};
    std::atomic<int>  stage{0};
    std::atomic<int>  done{0};
    std::atomic<int>  total{0};        // 0 = 不定
    std::atomic<bool> notified{false}; // 完了の通知（トースト等）を出したか

    mutable std::mutex mu;
    std::string detail;                // 段の補足（「123 / 456 ファイル」など）
    std::string error;                 // 失敗の理由（空なら汎用）
    std::string outputDir;
    std::chrono::steady_clock::time_point t0{}, t1{};

    void Begin(const std::string& out)
    {
        std::lock_guard<std::mutex> lk(mu);
        detail.clear(); error.clear(); outputDir = out;
        t0 = std::chrono::steady_clock::now(); t1 = {};
        stage = 0; done = 0; total = 0; notified = false; cancelRequested = false;
        state = Running;
    }
    void SetStage(int s, const std::string& d = {}, int tot = 0)
    {
        std::lock_guard<std::mutex> lk(mu);
        stage = s; done = 0; total = tot; detail = d;
    }
    void SetDetail(const std::string& d) { std::lock_guard<std::mutex> lk(mu); detail = d; }
    void Finish(State s, const std::string& err = {})
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!err.empty()) error = err;
        t1 = std::chrono::steady_clock::now();
        state = s;   // 最後に書く（読む側は state が終端なら他の値は確定済み）
    }
    double ElapsedSec() const
    {
        std::lock_guard<std::mutex> lk(mu);
        if (t0 == std::chrono::steady_clock::time_point{}) return 0.0;
        const auto end = (state.load() == Running || t1 == std::chrono::steady_clock::time_point{})
            ? std::chrono::steady_clock::now() : t1;
        return std::chrono::duration<double>(end - t0).count();
    }
    // 0..1。段内の進みが読める段は割合で、読めない段（total = 0）は段の始まりの値。
    float Fraction() const
    {
        const int st = stage.load();
        if (state.load() == Succeeded) return 1.0f;
        if (st < 1) return 0.0f;
        const float a = StageStartPct(st), b = StageEndPct(st);
        const int tot = total.load();
        const float inner = tot > 0 ? std::min(1.0f, static_cast<float>(done.load()) / static_cast<float>(tot)) : 0.0f;
        return (a + (b - a) * inner) / 100.0f;
    }
};

// 走っているビルド 1 本ぶん。
struct BuildJob
{
    BuildInput    input;
    BuildProgress progress;
    std::thread   worker;
};

} // namespace dx12e
