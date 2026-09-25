#pragma once

// セーブファイル（スロット 1 つ = <dir>/<slot>.sav）の純粋な部分。
//
// なぜ Lua バインディングから切り出したか: ここは間違えると「プレイヤーの数十時間が消える」
// 場所なので、GPU も Lua も要らない形にしてテストで固定する（tests/save_file_test.cpp）。
// ファイル操作は IFileOps 越しに行う＝テストで「書き込みの途中で失敗した」を注入できる。
//
// 本体（body）は JSON オブジェクト。中身の組み立て（Lua テーブル・エンティティの状態）は
// scripting/ScriptSaveBindings.cpp の担当で、ここはスロット名・ファイル・一覧だけを見る。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::save
{

// スロット名はそのままファイル名になる。英数字と _ - だけ・1〜64 文字。
// パス区切り / .. / Windows の予約名（CON, NUL, COM1 …）は通さない。
// 数値スロット（Lua の save.write(1, …)）は呼び出し側で "1" にしてから渡す。
bool IsValidSlotName(std::string_view name, std::string* why = nullptr);

// ---- ファイル操作（差し替え可能: テストで失敗を注入する）----
class IFileOps
{
public:
    virtual ~IFileOps() = default;
    virtual bool ReadAll(const std::filesystem::path& p, std::string& out) = 0;
    // 書いてディスクへ flush する（途中で失敗したら false。中途半端なファイルが残り得る）
    virtual bool WriteAll(const std::filesystem::path& p, std::string_view bytes) = 0;
    virtual bool Exists(const std::filesystem::path& p) = 0;
    // 無ければ true（消す目的は達している）
    virtual bool Remove(const std::filesystem::path& p) = 0;
    virtual bool CreateDirs(const std::filesystem::path& dir) = 0;
    // dir 直下の通常ファイル（ファイル名だけ）。dir が無ければ空。
    virtual std::vector<std::string> ListFiles(const std::filesystem::path& dir) = 0;
};

// 実ファイル（Win32）。スレッドセーフではない（呼ぶのはメインスレッドだけ）。
IFileOps& RealFileOps();

// ---- 封筒（ファイル全体のバイト列 ⇔ 本体 JSON）----
std::string EncodeSaveFile(const nlohmann::json& body);

enum class DecodeStatus
{
    Ok,
    Missing,          // ファイルが無い
    BadJson,          // JSON として読めない
};
const char* DecodeStatusName(DecodeStatus s);

struct DecodeResult
{
    DecodeStatus   status = DecodeStatus::Missing;
    nlohmann::json body;
    std::string    detail;   // 人が読む補足（パースエラーの位置など）
    bool ok() const { return status == DecodeStatus::Ok; }
};
DecodeResult DecodeSaveFile(std::string_view bytes);

// ---- スロット ----
std::filesystem::path SlotPath(const std::filesystem::path& dir, const std::string& slot);

struct WriteResult
{
    bool        ok = false;
    std::string error;
};
WriteResult WriteSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot,
                      const std::string& bytes);

struct ReadResult
{
    DecodeResult decoded;
    bool ok() const { return decoded.ok(); }
};
ReadResult ReadSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot);

bool DeleteSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot);

struct SlotSummary
{
    std::string    slot;
    DecodeStatus   status = DecodeStatus::Missing;
    nlohmann::json body;   // status==Ok のときだけ中身がある（一覧用に data も含めて持つ）
};
// 新しい順（savedAtUnix 降順、同値はスロット名順）。壊れたスロットも status 付きで返す。
std::vector<SlotSummary> ListSlots(IFileOps& ops, const std::filesystem::path& dir);

} // namespace dx12e::save
