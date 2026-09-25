#pragma once
// ===========================================================================
// OGG Vorbis のストリーミング再生（BGM / 長い環境音）。
// ---------------------------------------------------------------------------
// ★なぜ要るか: 以前は全部をメモリへ丸ごとデコードしていた。3 分のステレオ BGM は
//   16bit PCM で約 31MB、しかも再生開始前にデコードし切るので曲を切り替えるたびに
//   メインスレッドが数百 ms 止まっていた。ここでは圧縮されたままの OGG（数 MB）を持ち、
//   ワーカースレッドが 16384 フレーム（44.1kHz で約 0.37 秒）ずつデコードして
//   12 チャンクのリングへ詰める（先読み約 4.5 秒・PCM は 768KB で固定）。
//
// スレッドの役割
//   ワーカー（AudioStreamWorker）: DecodeAvailable() … 鳴らし終わったチャンクへ次をデコード
//   メイン（AudioSystem::Tick）  : Pump()            … デコード済みを XAudio2 へ渡すだけ
//   ★メインが長く止まった（シーンのロード等）ときは、ワーカーが 200ms 待ってから代わりに
//     Pump() する。XAudio2 の SubmitSourceBuffer は別スレッドから呼んでよい（公式の
//     ストリーミング例もワーカーから渡している）。これが無いとロード中に BGM が途切れる。
//   Pump と「ボイスの付け外し」は m_subMtx、デコーダとカーソルは m_decMtx で守る。
//   ロックの順は必ず m_subMtx → m_decMtx（逆順で取る箇所は無い）。
//
// ループ点: LOOPSTART / LOOPLENGTH / LOOPEND（サンプル数）の Vorbis コメントがあれば使う
//   （ゲーム音楽で広く使われている書き方）。SetLoop で上書きできる。
// 配布ゲーム: バイト列は vfs::ReadAsset で読む（pak から）。std::filesystem は使わない。
// ===========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <Windows.h>
#include <mmreg.h>
#include "audio/AudioStreamRing.h"

struct IXAudio2SourceVoice;
struct stb_vorbis;

namespace dx12e
{

class AudioStream
{
public:
    static constexpr std::uint32_t kChunkFrames = 16384;
    static constexpr std::uint32_t kChunkCount  = 12;

    // OGG の中身（圧縮されたまま）を受け取って開く。失敗は nullptr と err。
    static std::shared_ptr<AudioStream> Open(std::vector<std::uint8_t> oggBytes, std::string name,
                                             std::string& err);
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    const WAVEFORMATEX& Format() const { return m_fmt; }
    std::uint32_t SampleRate() const { return m_fmt.nSamplesPerSec; }
    std::uint32_t Channels() const { return m_fmt.nChannels; }
    std::uint64_t TotalFrames() const { return m_total; }
    const std::string& Name() const { return m_name; }

    // ループ設定（フレーム）。end = 0 は曲の終わりまで。デコード中でもよい（次のチャンクから効く）。
    void SetLoop(bool loop, std::uint64_t startFrame, std::uint64_t endFrame);
    audio::StreamLoop GetLoop() const;
    bool HasTaggedLoop() const { return m_taggedLoop; }

    // ---- ワーカー（とプリフィル）----
    // 書ける分だけ（最大 maxChunks）デコード。デコードしたチャンク数を返す。
    // 鳴らし始める前にメインから 2 チャンクだけ呼ぶ（プリフィル）と、鳴り出しが空かない。
    std::uint32_t DecodeAvailable(std::uint32_t maxChunks = 0xFFFFFFFFu);
    // メインが maxIdleMs 以上 Pump していなければ代わりに Pump する（ロード中に途切れない）。
    void WatchdogPump(std::int64_t nowMs, std::int64_t maxIdleMs);

    // ---- メイン ----
    // ボイスへ付ける（新しいボイス限定。付ける前に Prefill しておくと鳴り出しが空かない）。
    void AttachVoice(IXAudio2SourceVoice* voice);
    // ボイスから外す。★DestroyVoice の前に必ず呼ぶ（ワーカーが壊れたボイスへ渡さないように）。
    void DetachVoice();
    // デコード済みを渡し、鳴らし終えた数を数える。戻り値 true = 曲が終わって全部鳴らし終えた。
    bool Pump(bool paused);
    // 位置を変える。★ボイスは外して壊してから呼ぶこと（渡し済みのバッファを捨てるため）。
    void SeekTo(std::uint64_t frame);
    // いま鳴っている位置（フレーム）。付いていなければデコード位置。
    std::uint64_t PositionFrames() const;

    // ---- 計測（audio_state）----
    std::size_t   MemoryBytes() const;          // 圧縮データ + リングの PCM
    std::uint64_t Underruns() const { return m_underruns.load(); }
    double        DecodeMs() const { return static_cast<double>(m_decodeNs.load()) / 1.0e6; }
    std::uint64_t ChunksDecoded() const { return m_decodedTotal.load(); }
    std::uint32_t BufferedChunks() const;       // デコード済みでまだ鳴り終わっていない数
    std::uint64_t WatchdogPumps() const { return m_watchdogPumps.load(); }

private:
    AudioStream() = default;
    bool PumpLocked(bool paused);

    std::string                m_name;
    std::vector<std::uint8_t>  m_ogg;               // stb_vorbis がこのメモリを直接読む（生かし続ける）
    stb_vorbis*                m_vorbis = nullptr;
    WAVEFORMATEX               m_fmt{};
    std::uint64_t              m_total = 0;
    bool                       m_taggedLoop = false;

    struct Chunk
    {
        std::vector<std::int16_t> pcm;
        std::uint32_t frames     = 0;
        std::uint64_t startFrame = 0;
        bool          eos        = false;
    };
    Chunk m_chunks[kChunkCount];

    mutable std::mutex         m_decMtx;            // m_vorbis / m_pos / m_loop / チャンクへの書き込み
    std::uint64_t              m_pos = 0;           // デコーダのカーソル
    audio::StreamLoop          m_loop;
    std::atomic<bool>          m_decodeEos{false};

    std::atomic<std::uint64_t> m_decoded{0};
    std::atomic<std::uint64_t> m_submitted{0};
    std::atomic<std::uint64_t> m_released{0};

    mutable std::mutex         m_subMtx;            // m_voice / 渡し / 位置の手がかり
    IXAudio2SourceVoice*       m_voice = nullptr;
    bool                       m_eosSubmitted = false;
    bool                       m_starving = false;
    std::uint64_t              m_playingStart = 0;  // いま鳴っているチャンクの先頭フレーム
    std::uint64_t              m_playingBase  = 0;  // そのチャンクが始まった時点の SamplesPlayed
    std::uint64_t              m_releasedFrames = 0;
    audio::StreamLoop          m_loopForPos;        // 位置表示用のループ設定の写し（m_subMtx で守る）

    std::atomic<std::int64_t>  m_lastPumpMs{0};
    std::atomic<std::uint64_t> m_underruns{0};
    std::atomic<std::uint64_t> m_decodeNs{0};
    std::atomic<std::uint64_t> m_decodedTotal{0};
    std::atomic<std::uint64_t> m_watchdogPumps{0};
};

// デコード用のワーカースレッド 1 本（全ストリームで共有）。
class AudioStreamWorker
{
public:
    ~AudioStreamWorker() { Stop(); }
    void Start();
    void Stop();
    void Add(const std::shared_ptr<AudioStream>& s);
    void Remove(const AudioStream* s);
    void Wake();
    std::size_t Count() const;

private:
    void Run();
    std::thread                                m_thread;
    mutable std::mutex                         m_mtx;
    std::condition_variable                    m_cv;
    bool                                       m_quit = false;
    bool                                       m_wake = false;
    std::vector<std::shared_ptr<AudioStream>>  m_streams;
};

std::int64_t AudioNowMs();

} // namespace dx12e
