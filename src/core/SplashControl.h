#pragma once
// 起動画面の軽い制御口（SplashScreen.h の重ヘッダへ足さないための別ヘッダ。EXEC §0.6）。
namespace dx12e::splash
{
// スキップ要求（Graphite Gauge）。完了要求の前なら、絵はその時点でロックのフレームへ飛ぶ（読み込みが終わるまで窓は残る）。
// 起動画面の窓へのクリックでも同じ。本体が最初の入力を受けたときに呼ぶ想定（ロック以降は何もしない）。未表示なら no-op。
void RequestSplashSkip();
}
