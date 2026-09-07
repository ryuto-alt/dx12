#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const    kEngineName  = "Uno Engine";
const wchar_t* const kEngineNameW = L"Uno Engine";

const char* const kEngineVersion = "1.15.0";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "Uno Engine v1.15.0 の更新内容";

const char* const kWhatsNewBody =
    "v1.15.0: シーンの切り替え演出をサムネイルから選べるようになりました\n"
    "\n"
    "■【修正】ゲームで「アセットを読み込み中...」が出なくなりました\n"
    "  シーンを切り替えるたび、ビルドしたゲームにも開発者向けの読み込み画面\n"
    "  （スピナー・進捗％・ファイル名・経過秒）が出ていました。\n"
    "  ゲームでは出さず、演出で隠したまま切り替わります。\n"
    "  エディタでは今までどおり詳しい進捗が出ます。\n"
    "\n"
    "■【新機能】トランジションのプリセットをサムネイルで選べます\n"
    "  Scene Flow 窓に「トランジション（既定の演出）」が付きました。\n"
    "  9 種類のタイルから 1 つ選ぶだけです。押すとその場で実物が再生されます。\n"
    "\n"
    "    暗転 / ホワイトアウト / 横ワイプ / 縦ワイプ / アイリス /\n"
    "    菱形 / ブラインド / 時計ワイプ / シークバー早送り\n"
    "\n"
    "  ホワイトアウト・ブラインド・時計ワイプ・菱形は今回の新作です。\n"
    "  長さ（秒）もスライダーで詰められます。\n"
    "\n"
    "■【新機能】選んだ演出がビルドしたゲームにも付いていきます\n"
    "  選択は settings.json に保存され、ビルド時に同梱されます。\n"
    "  Trigger の FadeToScene と、新しい Lua の sceneTransition(rel) が\n"
    "  この既定を使います。\n"
    "\n"
    "  1 回だけ別の演出にしたいときは ID を渡してください。\n"
    "    transitionToScene(\"scenes/boss.json\", \"iris\")\n"
    "  ID: fade / flash / wipe / wipe_v / iris / diamond / blinds / clock / seek\n"
    "\n"
    "■ 既存プロジェクトへの影響\n"
    "  ありません。既定は今までと同じ「暗転 0.6 秒」です。\n"
    "  fadeToScene と番号指定の transitionToScene の意味も変わりません。\n";
} // namespace dx12e
