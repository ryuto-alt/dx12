#include "core/save/SaveService.h"

#include "core/Logger.h"
#include "core/save/UserData.h"

namespace dx12e::save
{

SaveService& SaveService::Get()
{
    static SaveService s_service;
    return s_service;
}

SaveService::SaveService()
    : m_sessionStart(std::chrono::steady_clock::now())
{
}

std::filesystem::path SaveService::Dir() const
{
    return m_dirOverride.empty() ? SavesDir() : m_dirOverride;
}

double SaveService::PlayTime() const
{
    const auto now = std::chrono::steady_clock::now();
    return m_playBase + std::chrono::duration<double>(now - m_sessionStart).count();
}

void SaveService::ResetSession(double baseSeconds)
{
    m_playBase = baseSeconds < 0.0 ? 0.0 : baseSeconds;
    m_sessionStart = std::chrono::steady_clock::now();
}

WriteResult SaveService::Write(const std::string& slot, const nlohmann::json& body)
{
    const std::filesystem::path dir = Dir();
    WriteResult r = WriteSlot(Ops(), dir, slot, EncodeSaveFile(body));
    if (r.ok)
        Logger::Info("セーブ: スロット '{}' を書いた ({})", slot, PathToUtf8(SlotPath(dir, slot)));
    else
        Logger::Warn("セーブ: スロット '{}' の書き込みに失敗: {} ({})", slot, r.error, PathToUtf8(dir));
    return r;
}

ReadResult SaveService::Read(const std::string& slot)
{
    return ReadSlot(Ops(), Dir(), slot);
}

bool SaveService::Exists(const std::string& slot)
{
    return IsValidSlotName(slot) && Ops().Exists(SlotPath(Dir(), slot));
}

bool SaveService::Delete(const std::string& slot)
{
    return DeleteSlot(Ops(), Dir(), slot);
}

std::vector<SlotSummary> SaveService::List()
{
    return ListSlots(Ops(), Dir());
}

} // namespace dx12e::save
