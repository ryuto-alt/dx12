#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <Windows.h>
#include <mmreg.h>
#include "core/Types.h"

namespace dx12e
{

class AudioClip
{
public:
    bool LoadFromFile(const std::string& filePath);
    bool LoadFromMemory(const uint8_t* data, size_t size, const std::string& extHint);

    const u8*         GetPCMData() const { return m_pcmData.data(); }
    u32               GetSizeInBytes() const { return static_cast<u32>(m_pcmData.size()); }
    const WAVEFORMATEX& GetFormat() const { return m_format; }

private:
    bool LoadWav(const std::string& filePath);
    bool LoadMp3(const std::string& filePath);
    bool LoadOgg(const std::string& filePath);
    bool LoadWavFromMemory(const uint8_t* data, size_t size);
    bool LoadMp3FromMemory(const uint8_t* data, size_t size);
    bool LoadOggFromMemory(const uint8_t* data, size_t size);

    std::vector<u8> m_pcmData;
    WAVEFORMATEX    m_format{};
};

} // namespace dx12e
