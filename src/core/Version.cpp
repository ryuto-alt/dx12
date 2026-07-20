#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.4.6";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.4.6 の更新内容";
const char* const kWhatsNewBody =
    "v1.4.6: エンジン診断パネルを追加 + 終了時クラッシュを修正\n"
    "\n"
    "・[ツール > エンジン診断] を追加。ボタン一発でエディタUIを自動操作し、\n"
    "  クラッシュやフリーズが起きないか検査できます。不具合が出たときは\n"
    "  検査を実行して「結果をコピー」で報告してください\n"
    "・エディタ終了時に稀にクラッシュする不具合を修正\n"
    "  (この診断機能で発見しました)\n"
    "\n"
    "v1.4.5 の内容(本バージョンに含まれます):\n"
    "・ヒエラルキー選択やスクリプト/コンポーネント追加で落ちる問題を修正\n"
    "・メッシュの連番アニメ(スプライトシート)と UV スクロール\n"
    "・UIImage の連番アニメ対応と animMode(ループ/単発/往復)\n"
    "\n"
    "※ v1.4.2 以前をお使いの場合、自動更新が失敗することがあります。\n"
    "   その場合は GitHub Releases から最新 zip を手動で入れ直してください。\n";
} // namespace dx12e
