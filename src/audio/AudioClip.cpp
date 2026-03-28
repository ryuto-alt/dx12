#include "audio/AudioClip.h"
#include "core/Logger.h"

#include <algorithm>
#include <filesystem>

// dr_libs implementations (must be in exactly one .cpp)
#pragma warning(push)
#pragma warning(disable: 4100 4244 4245 4267 4456 4701 4706)
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#pragma warning(pop)

namespace dx12e
{

bool AudioClip::LoadFromFile(const std::string& filePath)
{
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav")
        return LoadWav(filePath);
    else if (ext == ".mp3")
        return LoadMp3(filePath);

    Logger::Error("Unsupported audio format: {}", ext);
    return false;
}

bool AudioClip::LoadWav(const std::string& filePath)
{
    drwav wav;
    if (!drwav_init_file(&wav, filePath.c_str(), nullptr))
    {
        Logger::Error("Failed to open WAV: {}", filePath);
        return false;
    }

    // 16bit PCM に変換して読み込み
    u64 totalFrames = wav.totalPCMFrameCount;
    u32 channels = wav.channels;
    u32 sampleRate = wav.sampleRate;

    std::vector<drwav_int16> samples(static_cast<size_t>(totalFrames * channels));
    drwav_read_pcm_frames_s16(&wav, totalFrames, samples.data());
    drwav_uninit(&wav);

    // WAVEFORMATEX 設定
    m_format.wFormatTag      = WAVE_FORMAT_PCM;
    m_format.nChannels       = static_cast<WORD>(channels);
    m_format.nSamplesPerSec  = sampleRate;
    m_format.wBitsPerSample  = 16;
    m_format.nBlockAlign     = static_cast<WORD>(channels * 2);
    m_format.nAvgBytesPerSec = sampleRate * m_format.nBlockAlign;
    m_format.cbSize          = 0;

    // PCMデータをバイト配列にコピー
    m_pcmData.resize(samples.size() * sizeof(drwav_int16));
    std::memcpy(m_pcmData.data(), samples.data(), m_pcmData.size());

    Logger::Info("WAV loaded: {} ({}Hz, {}ch, {:.1f}s)",
                 filePath, sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

bool AudioClip::LoadMp3(const std::string& filePath)
{
    drmp3_config config{};
    drmp3_uint64 totalFrames = 0;

    drmp3_int16* samples = drmp3_open_file_and_read_pcm_frames_s16(
        filePath.c_str(), &config, &totalFrames, nullptr);

    if (!samples)
    {
        Logger::Error("Failed to open MP3: {}", filePath);
        return false;
    }

    u32 channels = config.channels;
    u32 sampleRate = config.sampleRate;

    // WAVEFORMATEX 設定
    m_format.wFormatTag      = WAVE_FORMAT_PCM;
    m_format.nChannels       = static_cast<WORD>(channels);
    m_format.nSamplesPerSec  = sampleRate;
    m_format.wBitsPerSample  = 16;
    m_format.nBlockAlign     = static_cast<WORD>(channels * 2);
    m_format.nAvgBytesPerSec = sampleRate * m_format.nBlockAlign;
    m_format.cbSize          = 0;

    // PCMデータをバイト配列にコピー
    size_t dataSize = static_cast<size_t>(totalFrames) * channels * sizeof(drmp3_int16);
    m_pcmData.resize(dataSize);
    std::memcpy(m_pcmData.data(), samples, dataSize);

    drmp3_free(samples, nullptr);

    Logger::Info("MP3 loaded: {} ({}Hz, {}ch, {:.1f}s)",
                 filePath, sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

} // namespace dx12e
