#pragma once
// ===========================================================================
// 自動保存の守り（W05-02 / BUG-06）。標準ライブラリだけ（tests/autosave_origin_test.cpp が単体で検証する）。
//
//   「人の未保存の変更」があるとき、シーン本体（main.json など）には書かない。代わりに退避（世代）を残す。
//   MCP 由来の変更だけなら今までどおり本体へ保存する。
//   退避に失敗したときに本体へ書き戻すフォールバックは作らない（失敗は知らせて、本体は書かない）。
// ===========================================================================
#include <functional>
#include <string>

namespace dx12e::autosaveguard
{

enum class Outcome
{
    SavedMain,      // MCP 由来の変更だけだったので本体へ保存した
    SaveFailed,     // 本体への保存に失敗した
    Protected,      // 人の変更があったので、本体には書かずに退避した（backupId）
    ProtectFailed,  // 人の変更があったが退避できなかった。本体には書いていない
};

struct Ops
{
    std::function<bool()>                          saveMain;   // 本体へ保存する
    std::function<std::string(std::string* err)>   backup;     // 退避する。成功なら世代の id（空にしない）。失敗は空 + err
};

struct Result
{
    Outcome     outcome = Outcome::SaveFailed;
    std::string backupId;
    std::string error;
};

inline Result ProtectOrSave(bool humanDirty, const Ops& ops)
{
    Result r;
    if (!humanDirty)
    {
        r.outcome = ops.saveMain && ops.saveMain() ? Outcome::SavedMain : Outcome::SaveFailed;
        return r;
    }
    // 人の変更がある: saveMain は呼ばない（どの分岐でも）。
    std::string err;
    r.backupId = ops.backup ? ops.backup(&err) : std::string();
    if (r.backupId.empty()) { r.outcome = Outcome::ProtectFailed; r.error = err; return r; }
    r.outcome = Outcome::Protected;
    return r;
}

// 退避が「進んだ」かの判定（自動保存のたびに同じ物を直列化しない）。
struct ProtectStamp
{
    unsigned long long editSeq  = ~0ull;
    unsigned long long settings = ~0ull;
    bool Same(unsigned long long e, unsigned long long s) const { return editSeq == e && settings == s; }
    void Set(unsigned long long e, unsigned long long s) { editSeq = e; settings = s; }
    void Reset() { editSeq = ~0ull; settings = ~0ull; }
};

// 退避を省いてよいか。force（閉じる・開く・置き換え・fleet 終了の全経路）は絶対に省かない。
// 省けるのは UpdateMcpAutoSave の経路（force=false）だけ: 同じ状態で退避済み、または直前の退避から間もない。
inline bool CanSkipProtect(bool force, bool blockedAndSameStamp, bool hasProtected, double secSinceLast, double minIntervalSec)
{
    if (force) return false;
    if (blockedAndSameStamp) return true;
    return hasProtected && secSinceLast < minIntervalSec;
}

} // namespace dx12e::autosaveguard
