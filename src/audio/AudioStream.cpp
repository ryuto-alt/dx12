#include "audio/AudioStream.h"
#include "core/Logger.h"

#include <xaudio2.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

// stb_vorbis の宣言だけ（実装は AudioClip.cpp が 1 回だけ持つ）。
#pragma warning(push)
#pragma warning(disable: 4100 4244 4245 4267 4456 4701 4706 4127 4189 4457 4459 4569 4702)
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API   // AudioClip.cpp と同じ設定にする
#include <stb_vorbis.c>
#pragma warning(pop)

namespace dx12e
{

std::int64_t AudioNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace
{
// FillChunk へ渡すデコーダの薄い包み（ロックは呼び出し側が持つ）
struct VorbisDecoder
{
    stb_vorbis*   v  = nullptr;
    std::uint32_t ch = 2;
    std::uint32_t Read(std::int16_t* dst, std::uint32_t frames)
    {
        const int n = stb_vorbis_get_samples_short_interleaved(
            v, static_cast<int>(ch), dst, static_cast<int>(frames * ch));
        return (n > 0) ? static_cast<std::uint32_t>(n) : 0u;
    }
    bool Seek(std::uint64_t frame)
    {
        return stb_vorbis_seek(v, static_cast<unsigned int>(frame)) != 0;
    }
};

// Vorbis コメント "KEY=VALUE" の数値を読む（大文字小文字無視）。無ければ -1。
long long CommentNumber(const stb_vorbis_comment& c, const char* key)
{
    const std::size_t klen = std::strlen(key);
    for (int i = 0; i < c.comment_list_length; ++i)
    {
        const char* s = c.comment_list[i];
        if (!s || _strnicmp(s, key, klen) != 0 || s[klen] != '=') continue;
        char* end = nullptr;
        const long long v = std::strtoll(s + klen + 1, &end, 10);
        if (end != s + klen + 1 && v >= 0) return v;
    }
    return -1;
}
} // namespace

std::shared_ptr<AudioStream> AudioStream::Open(std::vector<std::uint8_t> oggBytes, std::string name,
                                               std::string& err)
{
    if (oggBytes.empty()) { err = "empty file"; return nullptr; }
    std::shared_ptr<AudioStream> s(new AudioStream());
    s->m_name = std::move(name);
    s->m_ogg  = std::move(oggBytes);
    int verr = 0;
    s->m_vorbis = stb_vorbis_open_memory(s->m_ogg.data(), static_cast<int>(s->m_ogg.size()), &verr, nullptr);
    if (!s->m_vorbis)
    {
        err = "stb_vorbis_open_memory failed (error " + std::to_string(verr) + ")";
        return nullptr;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(s->m_vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate <= 0)
    {
        err = "unsupported channels/sample rate (" + std::to_string(info.channels) + "ch)";
        return nullptr;   // デストラクタが m_vorbis を閉じる
    }
    s->m_total = stb_vorbis_stream_length_in_samples(s->m_vorbis);
    WAVEFORMATEX& f  = s->m_fmt;
    f.wFormatTag      = WAVE_FORMAT_PCM;
    f.nChannels       = static_cast<WORD>(info.channels);
    f.nSamplesPerSec  = static_cast<DWORD>(info.sample_rate);
    f.wBitsPerSample  = 16;
    f.nBlockAlign     = static_cast<WORD>(info.channels * 2);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    f.cbSize          = 0;
    for (auto& c : s->m_chunks) c.pcm.resize(static_cast<std::size_t>(kChunkFrames) * info.channels);

    // ループ点のタグ（RPG ツクール等で使われる書き方。値はサンプル数）
    const stb_vorbis_comment cm = stb_vorbis_get_comment(s->m_vorbis);
    const long long ls = CommentNumber(cm, "LOOPSTART");
    const long long ll = CommentNumber(cm, "LOOPLENGTH");
    const long long le = CommentNumber(cm, "LOOPEND");
    if (ls >= 0 && (ll > 0 || le > ls))
    {
        s->m_loop.start = static_cast<std::uint64_t>(ls);
        s->m_loop.end   = (ll > 0) ? static_cast<std::uint64_t>(ls + ll) : static_cast<std::uint64_t>(le);
        s->m_taggedLoop = true;
    }
    s->m_loopForPos = s->m_loop;
    return s;
}

AudioStream::~AudioStream()
{
    if (m_vorbis) stb_vorbis_close(m_vorbis);
}

void AudioStream::SetLoop(bool loop, std::uint64_t startFrame, std::uint64_t endFrame)
{
    std::lock_guard<std::mutex> ls(m_subMtx);
    std::lock_guard<std::mutex> ld(m_decMtx);
    m_loop.loop  = loop;
    m_loop.start = startFrame;
    m_loop.end   = endFrame;
    m_loopForPos = m_loop;
}

audio::StreamLoop AudioStream::GetLoop() const
{
    std::lock_guard<std::mutex> lk(m_decMtx);
    return m_loop;
}

std::uint32_t AudioStream::DecodeAvailable(std::uint32_t maxChunks)
{
    std::lock_guard<std::mutex> lk(m_decMtx);
    std::uint32_t n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (n < maxChunks && !m_decodeEos.load(std::memory_order_relaxed))
    {
        const std::uint64_t dec = m_decoded.load(std::memory_order_relaxed);
        const std::uint64_t rel = m_released.load(std::memory_order_acquire);
        if (audio::RingWritable(dec, rel, kChunkCount) == 0) break;
        Chunk& c = m_chunks[audio::RingSlot(dec, kChunkCount)];
        c.startFrame = m_pos;
        VorbisDecoder d{m_vorbis, m_fmt.nChannels};
        const audio::FillResult r = audio::FillChunk(d, m_pos, m_total, m_loop, c.pcm.data(),
                                                     kChunkFrames, m_fmt.nChannels);
        c.frames = r.frames;
        c.eos    = r.endOfStream;
        if (r.endOfStream) m_decodeEos.store(true, std::memory_order_relaxed);
        // ★チャンクの中身を書き終えてから公開する（ポンプは decoded 未満しか読まない）
        m_decoded.store(dec + 1, std::memory_order_release);
        m_decodedTotal.fetch_add(1, std::memory_order_relaxed);
        ++n;
    }
    if (n > 0)
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        m_decodeNs.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
    }
    return n;
}

void AudioStream::AttachVoice(IXAudio2SourceVoice* voice)
{
    std::lock_guard<std::mutex> lk(m_subMtx);
    m_voice        = voice;
    m_eosSubmitted = false;
    m_starving     = false;
    m_playingBase  = 0;
    m_releasedFrames = 0;
    // 付けた時点で「渡したが鳴っていない」ものは無い（新しいボイス）ので揃える
    m_submitted.store(m_released.load());
    const std::uint64_t dec = m_decoded.load(std::memory_order_acquire);
    m_playingStart = (dec > m_released.load())
        ? m_chunks[audio::RingSlot(m_released.load(), kChunkCount)].startFrame : m_pos;
    m_lastPumpMs.store(AudioNowMs());
}

void AudioStream::DetachVoice()
{
    std::lock_guard<std::mutex> lk(m_subMtx);
    m_voice = nullptr;
}

bool AudioStream::Pump(bool paused)
{
    std::lock_guard<std::mutex> lk(m_subMtx);
    m_lastPumpMs.store(AudioNowMs(), std::memory_order_relaxed);
    return PumpLocked(paused);
}

void AudioStream::WatchdogPump(std::int64_t nowMs, std::int64_t maxIdleMs)
{
    if (nowMs - m_lastPumpMs.load(std::memory_order_relaxed) < maxIdleMs) return;
    std::unique_lock<std::mutex> lk(m_subMtx, std::try_to_lock);
    if (!lk.owns_lock() || !m_voice) return;
    PumpLocked(false);
    m_watchdogPumps.fetch_add(1, std::memory_order_relaxed);
}

bool AudioStream::PumpLocked(bool paused)
{
    if (!m_voice) return false;
    XAUDIO2_VOICE_STATE st{};
    m_voice->GetState(&st, 0);

    // 鳴らし終えたチャンクを数える（位置の手がかりもここで進める）
    const std::uint64_t sub     = m_submitted.load(std::memory_order_relaxed);
    const std::uint64_t oldRel  = m_released.load(std::memory_order_relaxed);
    const std::uint64_t newRel  = audio::RingReleased(sub, st.BuffersQueued, oldRel);
    for (std::uint64_t r = oldRel; r < newRel; ++r)
        m_releasedFrames += m_chunks[audio::RingSlot(r, kChunkCount)].frames;
    if (newRel != oldRel)
    {
        m_released.store(newRel, std::memory_order_release);
        m_playingBase = m_releasedFrames;
        const std::uint64_t dec = m_decoded.load(std::memory_order_acquire);
        if (newRel < dec) m_playingStart = m_chunks[audio::RingSlot(newRel, kChunkCount)].startFrame;
    }

    // デコード済みを渡す
    std::uint64_t s = sub;
    const std::uint64_t dec = m_decoded.load(std::memory_order_acquire);
    while (s < dec)
    {
        const Chunk& c = m_chunks[audio::RingSlot(s, kChunkCount)];
        if (c.frames > 0)
        {
            XAUDIO2_BUFFER b{};
            b.AudioBytes = c.frames * m_fmt.nBlockAlign;
            b.pAudioData = reinterpret_cast<const BYTE*>(c.pcm.data());
            b.Flags      = c.eos ? XAUDIO2_END_OF_STREAM : 0;
            if (FAILED(m_voice->SubmitSourceBuffer(&b))) break;
        }
        if (c.eos) m_eosSubmitted = true;
        ++s;
    }
    m_submitted.store(s, std::memory_order_relaxed);

    // 途切れ（渡す物が無いのに鳴らし切った）を数える。1 回の途切れを 1 と数える
    XAUDIO2_VOICE_STATE st2{};
    m_voice->GetState(&st2, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    const bool starving = (st2.BuffersQueued == 0) && !m_eosSubmitted && !paused;
    if (starving && !m_starving) m_underruns.fetch_add(1, std::memory_order_relaxed);
    m_starving = starving;

    return m_eosSubmitted && st2.BuffersQueued == 0;
}

void AudioStream::SeekTo(std::uint64_t frame)
{
    std::lock_guard<std::mutex> ls(m_subMtx);
    std::lock_guard<std::mutex> ld(m_decMtx);
    if (m_total > 0) frame = (std::min)(frame, m_total - 1);
    stb_vorbis_seek(m_vorbis, static_cast<unsigned int>(frame));
    m_pos = frame;
    m_decodeEos.store(false);
    m_decoded.store(0);
    m_submitted.store(0);
    m_released.store(0);
    m_eosSubmitted   = false;
    m_starving       = false;
    m_playingStart   = frame;
    m_playingBase    = 0;
    m_releasedFrames = 0;
}

std::uint64_t AudioStream::PositionFrames() const
{
    std::lock_guard<std::mutex> lk(m_subMtx);
    if (!m_voice) return m_playingStart;
    XAUDIO2_VOICE_STATE st{};
    m_voice->GetState(&st, 0);
    const std::uint64_t played = (st.SamplesPlayed > m_playingBase) ? st.SamplesPlayed - m_playingBase : 0u;
    // ★デコーダのロック（m_decMtx）は取らない。ワーカーがデコード中だとメインが待たされる
    return audio::WrapLoopPosition(m_playingStart + played, m_loopForPos, m_total);
}

std::size_t AudioStream::MemoryBytes() const
{
    std::size_t b = m_ogg.size();
    for (const auto& c : m_chunks) b += c.pcm.size() * sizeof(std::int16_t);
    return b;
}

std::uint32_t AudioStream::BufferedChunks() const
{
    return audio::RingSubmittable(m_decoded.load(), m_released.load());
}

// ===== ワーカー =====

void AudioStreamWorker::Start()
{
    if (m_thread.joinable()) return;
    m_quit = false;
    m_thread = std::thread([this] { Run(); });
}

void AudioStreamWorker::Stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_streams.clear();
}

void AudioStreamWorker::Add(const std::shared_ptr<AudioStream>& s)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_streams.push_back(s);
        m_wake = true;
    }
    m_cv.notify_all();
}

void AudioStreamWorker::Remove(const AudioStream* s)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_streams.erase(std::remove_if(m_streams.begin(), m_streams.end(),
                                   [s](const std::shared_ptr<AudioStream>& p) { return p.get() == s; }),
                    m_streams.end());
}

void AudioStreamWorker::Wake()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_wake = true;
    }
    m_cv.notify_all();
}

std::size_t AudioStreamWorker::Count() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_streams.size();
}

void AudioStreamWorker::Run()
{
    SetThreadDescription(GetCurrentThread(), L"AudioStream");
    // デコードが遅れると音が途切れる。描画より先に回ってほしいので優先度を上げる。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::vector<std::shared_ptr<AudioStream>> local;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            // 10ms ごとに見回る（1 チャンク = 0.37 秒なので十分間に合う）
            m_cv.wait_for(lk, std::chrono::milliseconds(10), [this] { return m_quit || m_wake; });
            if (m_quit) return;
            m_wake = false;
            local = m_streams;   // 握っている間に消されても生きている（shared_ptr）
        }
        const std::int64_t now = AudioNowMs();
        for (auto& s : local)
        {
            s->DecodeAvailable();
            s->WatchdogPump(now, 200);
            s->DecodeAvailable();   // 代わりに渡した分の空きもすぐ埋める
        }
        local.clear();
    }
}

} // namespace dx12e
