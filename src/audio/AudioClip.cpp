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
// stb_vorbis (OGG Vorbis デコーダー、シングルファイル実装)
#pragma warning(disable: 4127 4189 4457 4459 4569 4702)
#define STB_VORBIS_NO_PUSHDATA_API   // pull API(decode_filename/memory)のみ使う
#include <stb_vorbis.c>
#pragma warning(pop)

namespace dx12e
{

void AudioClip::DownmixToMono()
{
    const u16 ch = m_format.nChannels;
    if (ch <= 1 || m_format.wBitsPerSample != 16) return;

    const size_t frames = m_pcmData.size() / (static_cast<size_t>(ch) * 2);
    std::vector<u8> mono(frames * 2);
    auto* src = reinterpret_cast<const int16_t*>(m_pcmData.data());
    auto* dst = reinterpret_cast<int16_t*>(mono.data());
    for (size_t i = 0; i < frames; ++i)
    {
        int sum = 0;
        for (u16 c = 0; c < ch; ++c) sum += src[i * ch + c];
        dst[i] = static_cast<int16_t>(sum / ch);
    }
    m_pcmData = std::move(mono);
    m_format.nChannels       = 1;
    m_format.nBlockAlign     = 2;
    m_format.nAvgBytesPerSec = m_format.nSamplesPerSec * 2;
}

bool AudioClip::LoadFromFile(const std::string& filePath)
{
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav")
        return LoadWav(filePath);
    else if (ext == ".mp3")
        return LoadMp3(filePath);
    else if (ext == ".ogg")
        return LoadOgg(filePath);

    Logger::Error("未対応のオーディオ形式です: {}", ext);
    return false;
}

bool AudioClip::LoadWav(const std::string& filePath)
{
    drwav wav;
    if (!drwav_init_file(&wav, filePath.c_str(), nullptr))
    {
        Logger::Error("WAV を開けません: {}", filePath);
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
        Logger::Error("MP3 を開けません: {}", filePath);
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

bool AudioClip::LoadOgg(const std::string& filePath)
{
    int channels = 0, sampleRate = 0;
    short* samples = nullptr;
    int totalFrames = stb_vorbis_decode_filename(filePath.c_str(), &channels, &sampleRate, &samples);

    if (totalFrames <= 0 || !samples)
    {
        Logger::Error("OGG を開けません: {}", filePath);
        return false;
    }

    // WAVEFORMATEX 設定
    m_format.wFormatTag      = WAVE_FORMAT_PCM;
    m_format.nChannels       = static_cast<WORD>(channels);
    m_format.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    m_format.wBitsPerSample  = 16;
    m_format.nBlockAlign     = static_cast<WORD>(channels * 2);
    m_format.nAvgBytesPerSec = sampleRate * m_format.nBlockAlign;
    m_format.cbSize          = 0;

    // PCMデータをバイト配列にコピー
    size_t dataSize = static_cast<size_t>(totalFrames) * channels * sizeof(short);
    m_pcmData.resize(dataSize);
    std::memcpy(m_pcmData.data(), samples, dataSize);

    free(samples);   // stb_vorbis_decode_* は malloc 確保

    Logger::Info("OGG loaded: {} ({}Hz, {}ch, {:.1f}s)",
                 filePath, sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

bool AudioClip::LoadFromMemory(const uint8_t* data, size_t size, const std::string& extHint)
{
    std::string ext = extHint;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav")
        return LoadWavFromMemory(data, size);
    else if (ext == ".mp3")
        return LoadMp3FromMemory(data, size);
    else if (ext == ".ogg")
        return LoadOggFromMemory(data, size);

    Logger::Error("未対応のオーディオ形式です（メモリ読み込み）: {}", ext);
    return false;
}

bool AudioClip::LoadWavFromMemory(const uint8_t* data, size_t size)
{
    drwav wav;
    if (!drwav_init_memory(&wav, data, size, nullptr))
    {
        Logger::Error("WAV をメモリから開けません（{} バイト）", size);
        return false;
    }

    // 16bit PCM に変換して読み込み
    u64 totalFrames = wav.totalPCMFrameCount;
    u32 channels    = wav.channels;
    u32 sampleRate  = wav.sampleRate;

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

    Logger::Info("WAV loaded from memory ({}Hz, {}ch, {:.1f}s)",
                 sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

bool AudioClip::LoadMp3FromMemory(const uint8_t* data, size_t size)
{
    drmp3_config config{};
    drmp3_uint64 totalFrames = 0;

    drmp3_int16* samples = drmp3_open_memory_and_read_pcm_frames_s16(
        data, size, &config, &totalFrames, nullptr);

    if (!samples)
    {
        Logger::Error("MP3 をメモリから開けません（{} バイト）", size);
        return false;
    }

    u32 channels   = config.channels;
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

    Logger::Info("MP3 loaded from memory ({}Hz, {}ch, {:.1f}s)",
                 sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

bool AudioClip::LoadOggFromMemory(const uint8_t* data, size_t size)
{
    int channels = 0, sampleRate = 0;
    short* samples = nullptr;
    int totalFrames = stb_vorbis_decode_memory(data, static_cast<int>(size),
                                               &channels, &sampleRate, &samples);

    if (totalFrames <= 0 || !samples)
    {
        Logger::Error("OGG をメモリから開けません（{} バイト）", size);
        return false;
    }

    // WAVEFORMATEX 設定
    m_format.wFormatTag      = WAVE_FORMAT_PCM;
    m_format.nChannels       = static_cast<WORD>(channels);
    m_format.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    m_format.wBitsPerSample  = 16;
    m_format.nBlockAlign     = static_cast<WORD>(channels * 2);
    m_format.nAvgBytesPerSec = sampleRate * m_format.nBlockAlign;
    m_format.cbSize          = 0;

    // PCMデータをバイト配列にコピー
    size_t dataSize = static_cast<size_t>(totalFrames) * channels * sizeof(short);
    m_pcmData.resize(dataSize);
    std::memcpy(m_pcmData.data(), samples, dataSize);

    free(samples);   // stb_vorbis_decode_* は malloc 確保

    Logger::Info("OGG loaded from memory ({}Hz, {}ch, {:.1f}s)",
                 sampleRate, channels,
                 static_cast<f32>(totalFrames) / static_cast<f32>(sampleRate));
    return true;
}

} // namespace dx12e
