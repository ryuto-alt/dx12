#pragma once

#include <functional>
#include <string>

#include "core/SplashProgress.h"

namespace dx12e
{

// エディタ起動画面（スプラッシュ）。メインウィンドウが出る前に、ロゴ + 実進捗リング + 状態 + Tips を出し、
// 初期化が終わったらメイン窓へ滑らかに引き継ぐ。
//
// 実装（SplashScreen.cpp）:
//   ・専用スレッド + 自前のメッセージポンプ + 60fps の自前タイマ。メインスレッドが重い初期化
//     （D3D12 / シェーダ / PSO / アセット / 更新確認の WinHTTP）でブロックしていても止まらない。
//   ・Direct2D + DirectWrite で描き、UpdateLayeredWindow で per-pixel alpha の角丸カード + 柔らかい影の窓にする。
//     D2D が使えなければ GDI+ の簡易描画へ縮退する（窓が出ないより良い）。
//   ・演出（Timeline）は core/SplashMotion.h の純ロジック。--splash-preview と単体テストが同じ関数を通る。
//   ・起動音は core/SplashAudio.h（ミキサーは SplashMixer.h）。前回の起動時間から頂点を合わせる。
//
// 全 API は多重呼び出し・未表示時呼び出しに安全（no-op）。どのスレッドからでも呼べる（Pump だけはメインスレッド）。
class SplashScreen
{
public:
    // ---- 表示 ----
    // エディタ起動用のスプラッシュを表示する（すでに表示中なら何もしない）。起動音つき。
    //   titleUtf8   : 大見出し（例 "Uno Engine"）
    //   versionUtf8 : 版数（例 "v1.19.0"。空で非表示）
    //   logoPathUtf8: ロゴ PNG の絶対パス（空 or 読めない場合はテキストのみ）。<assets>/editor/icons/logo.png を想定し、
    //                 起動音（<assets>/editor/sounds/）と設定（<assets>/../settings.json）の場所もここから決める。
    static void Show(const std::string& titleUtf8,
                     const std::string& versionUtf8,
                     const std::string& logoPathUtf8);

    // プロジェクトを開くスプラッシュ（プロジェクト名 / シーン名 / 版 / ビルド日時を出す。起動音は鳴らさない）。
    // すでに（起動の）スプラッシュが出ていれば何もしない（起動の続きとして進む）。
    static void ShowProjectLoad(const std::string& titleUtf8,
                                const std::string& versionUtf8,
                                const std::string& logoPathUtf8,
                                const std::string& projectNameUtf8,
                                bool isNew);

    // Show より前に呼ぶ: 起動の直後にプロジェクトを開く（--project 直開き）ので、進捗の計画にその段階を含める。
    static void ExpectProjectLoad(bool on);

    // ---- 進捗 ----
    // 状態の 1 行だけを差し替える（UTF-8）。進捗は動かさない。
    static void SetStatus(const std::string& statusUtf8);

    // 段階に入る。進捗は段階の先頭へ寄り、段階内は微進みする（SetStageProgress が来れば実測に切替）。
    // labelUtf8 が空なら既定の文言（SplashProgress.h の StageLabelUtf8）を出す。
    static void SetStage(splash::Stage stage, const std::string& labelUtf8 = std::string());

    // いまの段階の中の進み具合 0..1（件数など実測できるとき）。
    static void SetStageProgress(float fraction);

    // 全体進捗を直接指定（0..1）。次の SetStage まで有効。
    static void SetProgress(float fraction);

    // 「手順 index / total」と文言を直接指定する（段階の計画に載らない用途向け）。
    static void SetStep(int index, int total, const std::string& labelUtf8);

    // プロジェクトを開くスプラッシュで見せる名前。
    static void SetProjectInfo(const std::string& projectNameUtf8, const std::string& sceneNameUtf8);

    // ---- 終了 ----
    // 初期化が終わった。リングを完成させ → ハイライトが一周 → ロゴがポンと弾む（起動音の頂点と同期）→
    // showMainWindow を呼んでメイン窓を出し、その上でスプラッシュが拡大しながらフェードアウトして消える。
    // ★非ブロッキング。showMainWindow は【メインスレッドの PumpMainThread()】から呼ばれる。
    //   スプラッシュが出ていない（抑止/失敗/終了済み）ときは、この中で即座に showMainWindow() を呼ぶ。
    static void Finish(std::function<void()> showMainWindow);

    // メインスレッドが毎フレーム呼ぶ。演出が「いまメイン窓を出してほしい」と言っていれば showMainWindow を実行する。
    static void PumpMainThread();

    // 即時に閉じてスレッドを合流する（アップデート適用・例外時など。演出なし）。未表示なら no-op。
    static void Close();

    // ---- 設定 ----
    // true の間 Show / ShowProjectLoad は何もしない（--background / --headless: 人の画面に窓を出さない）。
    static void SetSuppressed(bool on);

    // 起動音の有効/無効（--no-splash-sound）。既定は有効。次の 3 つのどれかで切れる（どれかが「オフ」なら鳴らさない）:
    //   ・引数 --no-splash-sound（main.cpp が SetSoundEnabled(false)）
    //   ・環境変数 UNO_NO_SPLASH_SOUND=1
    //   ・保存された設定 startupSound=false（下の Get/Set。エンジン設定の「起動音」チェックボックス）
    static void SetSoundEnabled(bool on);

    // 保存される設定 startupSound（既定 true）。%APPDATA%\DX12Engine\editor_state.json に持つ
    // （プロジェクトに依らないエディタ全体の設定。既存の lastOpenedScene と同じファイルで、読んでマージして書く）。
    static bool GetStartupSoundSetting();
    static void SetStartupSoundSetting(bool on);

    // 表示中か（抑止・終了済みなら false）。
    static bool IsShowing();

    // スプラッシュのスレッドが溜めた診断メッセージを Logger へ流す（メインスレッド・Logger 初期化後に呼ぶ）。
    static void FlushDiagnostics();

    // ---- 検証用（--splash-selftest。通常起動では使わない）----
    // true の間、窓を【作るが表示しない】（フォーカス/前面を一切奪わない）。描画・ULW・終了までの流れは通常どおり走るので、
    // スレッド・タイマ・D2D・Finish/Pump の連携と CPU 使用率を、人の画面に窓を出さずに確かめられる。起動音も鳴らさない。
    static void SetTestNoShow(bool on);
    struct TestStats
    {
        unsigned long long frames = 0;
        double avgRenderMs = 0.0, maxRenderMs = 0.0;   // 1 フレームの描画 + 転送 + ULW（ms）
        double lifetimeSec = 0.0;
        double threadCpuSec = 0.0;          // スプラッシュのスレッドの CPU 時間（立ち上げ込み）
        double steadyCpuPercent = 0.0;      // 30 フレーム目以降の定常の CPU 使用率（1 コア = 100%）
        bool d2d = false, warp = false;
    };
    static TestStats GetTestStats();
};

} // namespace dx12e
