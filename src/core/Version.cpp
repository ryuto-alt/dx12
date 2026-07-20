#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.4.3";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.4.3 の更新内容";
const char* const kWhatsNewBody =
    "v1.4.3: 自動アップデートが無限に繰り返されるバグを修正\n"
    "\n"
    "・v1.2.2〜v1.4.2 の配布物に「自分を v1.2.1 と誤認する更新チェック」が\n"
    "  混入していたため、最新版なのに毎回更新を求められていた問題を修正\n"
    "・再発防止として、パッケージ作成時にバージョン整合性を自動検証するように変更\n";
} // namespace dx12e
