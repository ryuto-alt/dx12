#pragma once
// インスタンス群のサイドカー（<シーン名>.inst/<guid>.jsonl）のファイル入出力。
// 数値の整形・解析（純関数）は ecs/InstanceGroup.h。ここはファイルシステム / VFS（pak）に触る部分だけ。
#include <string>
#include "ecs/InstanceGroup.h"

namespace dx12e::instgroup
{

// <シーン>.inst/<guid>.jsonl を読む（ゲームモードは pak から。無ければディスク）。ファイルが無ければ nullptr。
// 壊れた行は読み飛ばして Warn を 1 行出す。
InstanceSetPtr LoadSidecar(const std::string& scenePath, const std::string& guidHex);

struct SaveStats
{
    int written = 0;     // 書き換えたファイル数
    int unchanged = 0;   // 内容が同じなので触らなかったファイル数
    int removed = 0;     // 孤児（どのグループの guid でもない）として消したファイル数
    bool ok = true;
};
// 収集した実体を <シーン>.inst/ へ書く。内容が同じファイルには触らない（更新時刻・git の差分を動かさない）。
// 集めたものに無い *.jsonl は消す（削除したグループの残骸）。1 つも無ければフォルダごと消す。
SaveStats SaveSidecars(const std::string& scenePath, const SerializeCollector& collected);

} // namespace dx12e::instgroup
