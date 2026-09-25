#include "core/save/SaveFile.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace dx12e::save
{

namespace
{
std::string ToUpperAscii(std::string_view s)
{
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// ---- 実ファイル（Win32 のワイド API。日本語のゲーム名 / ユーザー名でも通る）----
class Win32FileOps final : public IFileOps
{
public:
    bool ReadAll(const std::filesystem::path& p, std::string& out) override
    {
        out.clear();
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) != 0 && size.QuadPart >= 0
                  && size.QuadPart < (1LL << 31);   // 2GB 超のセーブは想定しない（壊れている）
        if (ok)
        {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            ok = out.empty()
                 || (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) != 0
                     && got == out.size());
        }
        CloseHandle(h);
        if (!ok) out.clear();
        return ok;
    }

    bool WriteAll(const std::filesystem::path& p, std::string_view bytes) override
    {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        bool ok = bytes.empty()
                  || (::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) != 0
                      && wrote == bytes.size());
        // ★flush してから閉じる。閉じただけではキャッシュに残り、直後の電源断で中身が 0 になる
        ok = ok && FlushFileBuffers(h) != 0;
        CloseHandle(h);
        return ok;
    }

    bool Exists(const std::filesystem::path& p) override
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool Remove(const std::filesystem::path& p) override
    {
        if (DeleteFileW(p.c_str())) return true;
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }

    bool CreateDirs(const std::filesystem::path& dir) override
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return std::filesystem::is_directory(dir, ec);
    }

    std::vector<std::string> ListFiles(const std::filesystem::path& dir) override
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return out;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            const std::u8string u8 = it->path().filename().u8string();
            out.emplace_back(u8.begin(), u8.end());
        }
        return out;
    }
};
} // namespace

bool IsValidSlotName(std::string_view name, std::string* why)
{
    auto fail = [why](const char* msg) { if (why) *why = msg; return false; };
    if (name.empty()) return fail("スロット名が空");
    if (name.size() > 64) return fail("スロット名が長すぎる（64 文字まで）");
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '_' || c == '-';
        if (!ok) return fail("スロット名に使えるのは英数字と _ - だけ（パス区切りや空白は不可）");
    }
    // Windows の予約デバイス名（拡張子が付いていても開けない: "CON.sav" は CON を開く）
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL",
                                      "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::string up = ToUpperAscii(name);
    for (const char* r : kReserved)
        if (up == r) return fail("Windows の予約名はスロット名に使えない（CON / NUL / COM1 など）");
    return true;
}

IFileOps& RealFileOps()
{
    static Win32FileOps s_ops;
    return s_ops;
}

const char* DecodeStatusName(DecodeStatus s)
{
    switch (s)
    {
    case DecodeStatus::Ok:      return "ok";
    case DecodeStatus::Missing: return "missing";
    case DecodeStatus::BadJson: return "bad_json";
    }
    return "unknown";
}

std::string EncodeSaveFile(const nlohmann::json& body)
{
    return body.dump(1, '\t');
}

DecodeResult DecodeSaveFile(std::string_view bytes)
{
    DecodeResult r;
    try
    {
        r.body = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!r.body.is_object())
        {
            r.status = DecodeStatus::BadJson;
            r.detail = "本体が JSON オブジェクトではない";
            r.body = nlohmann::json();
            return r;
        }
        r.status = DecodeStatus::Ok;
    }
    catch (const std::exception& e)
    {
        r.status = DecodeStatus::BadJson;
        r.detail = e.what();
    }
    return r;
}

std::filesystem::path SlotPath(const std::filesystem::path& dir, const std::string& slot)
{
    return dir / std::filesystem::path(std::wstring(slot.begin(), slot.end()) + L".sav");
}

WriteResult WriteSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot,
                      const std::string& bytes)
{
    WriteResult r;
    std::string why;
    if (!IsValidSlotName(slot, &why)) { r.error = why; return r; }
    if (!ops.CreateDirs(dir)) { r.error = "セーブフォルダを作れない（書き込み権限？）"; return r; }
    if (!ops.WriteAll(SlotPath(dir, slot), bytes)) { r.error = "書き込みに失敗した"; return r; }
    r.ok = true;
    return r;
}

ReadResult ReadSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot)
{
    ReadResult r;
    std::string bytes;
    if (!IsValidSlotName(slot) || !ops.ReadAll(SlotPath(dir, slot), bytes))
    {
        r.decoded.status = DecodeStatus::Missing;
        return r;
    }
    r.decoded = DecodeSaveFile(bytes);
    return r;
}

bool DeleteSlot(IFileOps& ops, const std::filesystem::path& dir, const std::string& slot)
{
    if (!IsValidSlotName(slot)) return false;
    return ops.Remove(SlotPath(dir, slot));
}

std::vector<SlotSummary> ListSlots(IFileOps& ops, const std::filesystem::path& dir)
{
    std::vector<SlotSummary> out;
    for (const std::string& file : ops.ListFiles(dir))
    {
        constexpr std::string_view kExt = ".sav";
        if (file.size() <= kExt.size() || file.compare(file.size() - kExt.size(), kExt.size(), kExt) != 0)
            continue;
        const std::string slot = file.substr(0, file.size() - kExt.size());
        if (!IsValidSlotName(slot)) continue;
        SlotSummary s;
        s.slot = slot;
        ReadResult rr = ReadSlot(ops, dir, slot);
        s.status = rr.decoded.status;
        if (rr.ok()) s.body = std::move(rr.decoded.body);
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SlotSummary& a, const SlotSummary& b) {
        const long long ta = a.body.is_object() ? a.body.value("savedAtUnix", 0LL) : 0LL;
        const long long tb = b.body.is_object() ? b.body.value("savedAtUnix", 0LL) : 0LL;
        if (ta != tb) return ta > tb;
        return a.slot < b.slot;
    });
    return out;
}

} // namespace dx12e::save
