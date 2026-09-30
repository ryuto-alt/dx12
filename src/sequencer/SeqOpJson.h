#pragma once
// SeqOp の JSON 表現(MCP / AI が宣言的にシーケンスを編集するための入口)。
// 純ロジック(標準ライブラリ + nlohmann + 自ディレクトリだけ。SequencerCore の純粋さの検査を守る)。
//
// ・op 1 個 = {"op": "<名前>", …フィールド}。名前は SeqOp の名前(OpName)を大文字小文字/'_'/'-' 無視で受ける
//   ("addKey" / "AddKey" / "add_key" は同じ)。フィールドは docs/DXSEQ_FORMAT.md §18。
// ・ID(id / trackId 以外の「新しく作る要素の id」)は省略できる。省略したら IdIssuer で発行する
//   (AI は ID を考えなくてよい。発行は決定論的な IdAllocator を渡せば再現できる)。
// ・時刻はティック(整数)。秒で書きたいときは「〜Sec」版(start→startSec / t→tSec / newT→newSec / キーオブジェクトの sec)。
// ・キーは [t, v, "a"] のタプルか、{"t"|"sec", "v", "ip"?, "inDt"…} のオブジェクト。
// ・ヘッダ系(setBindingHeader / setTrackHeader)は「書いたフィールドだけ変える」(現在の値から始めてマージする)。

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "sequencer/SeqOps.h"

namespace dx12e::seq
{

// 新しい要素の ID を発行する窓口(prefix は 'b' バインディング / 't' トラック / 'k' クリップ / 'e' イベント / 'm' マーカー / 'c' カット)。
using IdIssuer = std::function<std::string(char prefix)>;

struct OpParseResult
{
    bool ok = false;
    std::string error;   // 日本語。位置つき("ops[2] addKey: key: …")
    explicit operator bool() const { return ok; }
};

// op 1 個(JSON オブジェクトのテキスト)→ SeqOp。cur は「今のシーケンス」(tps とヘッダのマージ元に使う。変更しない)。
OpParseResult ParseOpJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, SeqOp& out);

// op の配列(`[…]` または `{"label": "…", "ops": […]}`)→ SeqTxn。ids は txn 全体で通し(先に発行した ID は後の op が参照できない点に注意)。
OpParseResult ParseTxnJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, SeqTxn& out);

// 受け付ける op 名の一覧(正準名 = OpName)
std::vector<std::string> OpJsonNames();

// JSON 断片(バインディング / トラック)の ID 補完と時刻(〜Sec)の正規化をしたうえでパースして返す。
// AddBinding / AddTrack の中身のほか、ホスト側(entity 指定で作るバインディング)からも使う。
OpParseResult ParseBindingJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, Binding& out);
OpParseResult ParseTrackJson(std::string_view text, const Sequence& cur, const IdIssuer& ids, Track& out);

} // namespace dx12e::seq
