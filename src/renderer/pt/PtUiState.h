#pragma once
// ===========================================================================
// パストレーサー(リファレンスレンダー)のエディタ UI と Application の受け渡し(POD)。
// パネル(editor/panels/PathTracerPanel)が入力欄を書いて requestStart を立て、
// Application(core/ApplicationPathTracer.cpp)が毎フレーム読んで実行し、進捗を書き戻す。
// ===========================================================================
#include <cstdint>

namespace dx12e::pt
{

struct UiState
{
    // ---- 入力(パネル → アプリ)----
    int   width = 1920, height = 1080;
    int   spp = 256, bounces = 8, seed = 1;
    float frameBudgetMs = 12.0f;       // 1 フレームあたりに PT へ使う GPU 時間の上限(ms)。小さいほどエディタが軽い
    float maxRadiance = 0.0f;          // 0 = クランプ無し(GT の既定)
    float exposure = 1.0f;             // プレビュー PNG の露出(倍率)
    char  outputBase[260] = {};        // 出力の基準パス(拡張子なし)。空 = 既定(<project>/.dx12/pt/render_<日時>)
    bool  useEditorCamera = true;      // true = 今の描画カメラ / false = 保存カメラ(cameraPos / cameraTarget / fovDeg)
    float cameraPos[3] = {0, 2, -6};
    float cameraTarget[3] = {0, 1, 0};
    float fovDeg = 45.0f;
    bool  writePfm = true, writeExr = false, writePng = true;
    bool  requestStart = false, requestCancel = false;

    // ---- 出力(アプリ → パネル)----
    int   phase = 0;                   // 0 待機 / 1 準備 / 2 実行 / 3 仕上げ / 4 完了 / 5 失敗 / 6 中止
    float progress = 0.0f;             // 0..1
    int   samplesDone = 0, samplesTarget = 0;
    float elapsedSec = 0.0f, etaSec = -1.0f, msPerSpp = 0.0f;
    char  message[256] = {};
    char  lastOutput[260] = {};        // 直近に書いたファイルの基準パス
};

} // namespace dx12e::pt
