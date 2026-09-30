#pragma once
// .dxseq(JSON)の読み書き。書き出しは自前で決定的(キー順固定・1 キー 1 行・末尾改行)で、git 差分が読める。
// Serialize(Parse(x)) == x(x が正準形のとき、バイト一致)/ Parse(Serialize(m)) は m とシリアライズ結果が一致。
// 形式の仕様: docs/DXSEQ_FORMAT.md

#include <string>
#include <string_view>
#include <vector>

#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

constexpr int kDxseqVersion = 1;

struct ParseResult
{
    bool ok = false;
    std::string error;                  // 日本語。位置つき("bindings[1].tracks[0].channels[position.x].keys[3]: …")
    std::vector<std::string> warnings;  // 無視した未知フィールド・昇順に直したキー など
    explicit operator bool() const { return ok; }
};

// text → Sequence。失敗したら out は不定。成功時は構造検査(ID 重複・カットの参照など)を通過している。
// ・キーが時刻昇順でなければ stable sort で直し、警告を積む
// ・トラック/クリップ/イベント/バインディング/マーカーの未知フィールドは params に保持(往復で失わない)。ルート/カット/hint/render の未知フィールドは警告のうえ無視
ParseResult ParseSequence(std::string_view text, Sequence& out);

// Sequence → 正準形の JSON テキスト(LF、末尾改行)。
std::string SerializeSequence(const Sequence& seq);

// ファイル入出力。Save は一時ファイルへ書いてからリネームする(アトミック)。
ParseResult LoadSequenceFile(const std::string& path, Sequence& out);
bool SaveSequenceFile(const std::string& path, const Sequence& seq, std::string* error = nullptr);

} // namespace dx12e::seq
