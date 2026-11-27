#pragma once

// ===========================================================================
// 起動音 Calibrate の「音を主時計にする」純ロジック（W1-BD5）。ヘッダオンリー。音デバイス / Win32 に依存しない。
// ---------------------------------------------------------------------------
// 構成（すべて決定論。テストとオフライン書き出しは実デバイスを開かない）:
//
//   CalibrateBank        … 起動音の素材（graphite_1.wav / graphite_1_rm.wav / bed / click x24 / lock_tail）をデコードして持つ
//   CalibrateSequencer   … ワンショットのボイス（最大 8）を 1 本のステレオバッファへ混ぜる。出力フレーム番号 = タイムライン上のサンプル
//   AudioClockMap        … デバイスの再生位置の報告（フレーム数, QPC）から「いま聞こえているサンプル」を換算する（量子化と報告の遅れをならす）
//   TimelineClock        … 絵の時計。音が動いていれば音の時計、無い・止まったときは QPC の時計で完走させる（単調）
//   SoundGate            … 鳴らさない条件（設定オフ / 引数 / 環境変数 / ヘッドレス・selftest / リモートデスクトップ）
//
// 経路（SplashCalibrate の表 §2.6 と同じ。時刻は SplashCalibrateEvents.h が正）:
//   ・速い経路: graphite_1.wav（Reduce Motion は graphite_1_rm.wav）を【1 ボイスでそのまま】鳴らす（完成した master。ステムから組み立てない）。
//   ・遅い経路: 速い経路は最後の歯の手前（C22）で持ち替える（GaugeFastDriver::HandedOver）。持ち替えでは full を数十 ms でフェードアウトし、
//     ベッド（エアとうなり。ループ区間つき）へ等ゲインでクロスフェード、以降は予約された Click を click_NN で（パンは等パワー + Haas 遅延）、
//     LockTail の予約（T_lock'）で lock_tail.wav を【サンプル単位】で合わせる。LockTail までにベッドとクリックは必ず 0 になる。
//   ・スキップ: 鳴っているボイスを 30ms でフェードアウトし、lock_tail.wav を頭から鳴らす。まだ鳴っていない予約は捨てる。
//   lock_tail.wav は full の master の T_lock 以降をサンプル単位で切り出したもの。だから【どの経路でもロック以降の出力は lock_tail と一致する】。
//
// スレッド: CalibrateSequencer は単一スレッド専用。実機（SplashAudio.cpp）は呼び出し側で mutex を掛ける。
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "core/SplashCalibrateEvents.h"
#include "core/SplashGaugeMotion.h"
#include "core/SplashMixer.h"

namespace dx12e::splash
{

namespace seq
{
inline constexpr int kSampleRate = calibrate::kSampleRate;       // 44100
inline constexpr int kMaxVoices = 8;                              // 同時に鳴らすワンショットの上限
inline constexpr double kSkipFadeMs = 30.0;                       // スキップで鳴っているボイスを落とす長さ
inline constexpr double kTailCutFadeMs = 20.0;                    // LockTail の直前にベッドを 0 へ落とす長さ（LockTail の開始時刻に 0 になる）
inline constexpr double kHandoverFadeMs = 30.0;                   // 速い経路 → 遅い経路の持ち替えのクロスフェード
inline constexpr double kHandoverMinFadeMs = 4.0;                 // 持ち替えが遅れても、これより短くはしない
inline constexpr double kCancelFadeMs = 5.0;                      // 取り消す予約が鳴り始めていたときの落とし方
inline constexpr double kAbortFadeMs = 60.0;                      // 中止（エディタの終了など）

inline int64_t MsToFrame(double ms) { return static_cast<int64_t>(std::llround(ms * kSampleRate / 1000.0)); }
inline double FrameToMs(double frame) { return frame * 1000.0 / kSampleRate; }
inline int64_t MsToFrameCeil(double ms) { return static_cast<int64_t>(std::ceil(ms * kSampleRate / 1000.0)); }

// float → PCM16。ParseWavPcm16 の逆変換（x / 32768 の逆）。WAV を 1 ボイスで鳴らした出力が元の WAV とビット一致するよう、丸めは厳密な逆にする。
inline std::int16_t FloatToPcm16(float v)
{
    const long q = std::lround(static_cast<double>(v) * 32768.0);
    return static_cast<std::int16_t>(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
}

// 等パワーのパン + Haas 遅延（events.json の panLaw と同じ式。tools/gen_boot_sound_calibrate.py の mix_stems と同じ）:
//   L = cos((pan+1)pi/4), R = sin((pan+1)pi/4)。pan > 0 は L を、pan < 0 は R を |pan| * haasMs だけ遅らせる。
struct PanLaw
{
    float gainL = 1.f, gainR = 1.f;
    std::int64_t delayL = 0, delayR = 0;   // フレーム
};
inline PanLaw MakePanLaw(double pan, double haasMs = calibrate::kHaasMs)
{
    const double p = std::clamp(pan, -1.0, 1.0);
    const double ang = (p + 1.0) * 3.14159265358979323846 / 4.0;
    PanLaw l;
    l.gainL = static_cast<float>(std::cos(ang));
    l.gainR = static_cast<float>(std::sin(ang));
    const std::int64_t d = static_cast<std::int64_t>(std::llround(std::fabs(p) * haasMs * 1e-3 * kSampleRate));
    if (p > 0.0) l.delayL = d;
    else if (p < 0.0) l.delayR = d;
    return l;
}
} // namespace seq

// ---------------------------------------------------------------- 素材

struct BlobView
{
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct CalibrateBlobs
{
    BlobView full, rm, bed, lockTail;
    BlobView click[calibrate::kClickCount];
};

struct CalibrateBank
{
    std::vector<float> full, rm, bed, lockTail;                              // ステレオ interleaved
    std::array<std::vector<float>, calibrate::kClickCount> click;            // モノラル（パン前）

    std::int64_t FullFrames() const { return static_cast<std::int64_t>(full.size() / 2); }
    bool Valid() const
    {
        if (full.empty() || rm.size() != full.size() || bed.empty() || lockTail.empty()) return false;
        for (const auto& c : click) if (c.empty()) return false;
        return true;
    }
};

namespace seq_detail
{
inline bool Decode(const BlobView& b, int wantChannels, std::vector<float>& out)
{
    if (!b.data || b.size == 0) return false;
    WavData w;
    if (!ParseWavPcm16(b.data, b.size, w) || w.sampleRate != seq::kSampleRate || w.channels != wantChannels) return false;
    if (wantChannels == 2) { out = std::move(w.frames); return true; }
    out.resize(w.frames.size() / 2);                   // モノラルは L=R に複製されているので L を取る
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = w.frames[i * 2];
    return true;
}
} // namespace seq_detail

// 長さの整合（full と rm は同じ長さ、lock_tail は full の T_lock 以降、ベッドはループ区間を含む）まで確かめる。
inline bool BuildCalibrateBank(const CalibrateBlobs& in, CalibrateBank& out)
{
    CalibrateBank b;
    if (!seq_detail::Decode(in.full, 2, b.full) || !seq_detail::Decode(in.rm, 2, b.rm) || !seq_detail::Decode(in.bed, 2, b.bed)
        || !seq_detail::Decode(in.lockTail, 2, b.lockTail))
        return false;
    for (int i = 0; i < calibrate::kClickCount; ++i)
        if (!seq_detail::Decode(in.click[i], 1, b.click[static_cast<std::size_t>(i)])) return false;
    if (b.full.size() != b.rm.size()) return false;
    if (b.FullFrames() != calibrate::kLengthSamples) return false;
    if (static_cast<std::int64_t>(b.lockTail.size() / 2) != b.FullFrames() - calibrate::kLockSample) return false;
    if (static_cast<std::int64_t>(b.bed.size() / 2) < seq::MsToFrame(calibrate::kBedLoopEndMs)) return false;
    out = std::move(b);
    return true;
}

// ---------------------------------------------------------------- 音の時計

// デバイスの再生位置の報告 (frames, qpcMs) から、「いま聞こえているサンプル」を換算する。
// 報告の frames は粗く（エンジンの周期ごと）しか進まず、しかも切り捨ての向きに遅れる。報告ごとに offset = qpcMs - frames/sr（ストリームの開始時刻の推定）を
// 取り、直近 N 個の最小値を採る（量子化は offset を大きくする側にだけずれる）。frames == 0 の報告は無視（デバイスがまだ動き出していない）。
class AudioClockMap
{
public:
    explicit AudioClockMap(int window = 64) : m_window(std::max(1, window)) {}

    void Report(std::int64_t deviceFrames, double qpcMs)
    {
        if (deviceFrames <= 0) return;
        const double off = qpcMs - static_cast<double>(deviceFrames) * 1000.0 / seq::kSampleRate;
        if (static_cast<int>(m_ring.size()) < m_window) m_ring.push_back(off);
        else { m_ring[m_next] = off; }
        m_next = (m_next + 1) % static_cast<std::size_t>(m_window);
        m_start = *std::min_element(m_ring.begin(), m_ring.end());
        m_valid = true;
    }
    bool Valid() const { return m_valid; }
    // qpcMs の時点で聞こえているタイムライン上の時刻（ms）。始まる前は 0
    double TimelineMsAt(double qpcMs) const { return m_valid ? std::max(0.0, qpcMs - m_start) : 0.0; }
    double FramesAt(double qpcMs) const { return TimelineMsAt(qpcMs) * seq::kSampleRate / 1000.0; }

private:
    int m_window;
    std::vector<double> m_ring;
    std::size_t m_next = 0;
    double m_start = 0.0;
    bool m_valid = false;
};

// 絵の時計。音が動いているときは音の時計（音が主）、音を使わない・開けない・止まったときは QPC で完走させる。単調。
//   ・音を使う（Expect）のに音の時計がまだ動かない間は 0 のまま待つ（デバイスのオープンは数十〜数百 ms）。kOpenGraceMs を過ぎたら QPC に切り替える。
//   ・音の時計が QPC より kStallMs 以上遅れたら（デバイスの停止・取りこぼし）、以後は QPC に切り替える（戻らない）。
class TimelineClock
{
public:
    static constexpr double kOpenGraceMs = 700.0;
    static constexpr double kStallMs = 250.0;

    // wallMs: 単調な QPC（ms）。expectAudio: 音を鳴らす（デバイスを開きに行く）なら true
    void Begin(double wallMs, bool expectAudio)
    {
        m_begin = wallMs;
        m_expect = expectAudio;
        m_useWall = !expectAudio;
        m_base = wallMs;
        m_last = 0.0;
        m_started = true;
    }
    bool UsingAudio() const { return !m_useWall; }
    // 音のデバイスを開けなかった・途中で止まったと分かったとき、猶予を待たずに QPC へ（いまの時刻から連続させる）
    void ForceWall(double wallMs) { if (!m_useWall) SwitchToWall(wallMs); }

    // audioMs: 音の時計の時刻（動いていなければ負）。戻り値: 絵のタイムライン時刻（ms。単調）
    // 停止判定は「音を初めて観測した時点の (wall, audio)」を基準にした相対値で行う（デバイスを開くのに時間が掛かっても止まったと誤判定しない）
    double Now(double wallMs, double audioMs)
    {
        if (!m_started) Begin(wallMs, false);
        double t = m_last;
        if (!m_useWall)
        {
            if (audioMs >= 0.0)
            {
                t = audioMs;
                if (!m_sawAudio) { m_sawAudio = true; m_refWall = wallMs; m_refAudio = audioMs; }
                if ((wallMs - m_refWall) - (t - m_refAudio) > kStallMs) SwitchToWall(wallMs);   // 止まった（音の進みが QPC の進みより大きく遅れた）
            }
            else if (!m_sawAudio && wallMs - m_begin > kOpenGraceMs)
                SwitchToWall(wallMs);                                                            // 開けなかった・動き出さない
            else if (m_sawAudio && (wallMs - m_refWall) - (m_last - m_refAudio) > kStallMs)
                SwitchToWall(wallMs);
        }
        if (m_useWall) t = wallMs - m_base;
        m_last = std::max(m_last, t);
        return m_last;
    }

private:
    void SwitchToWall(double wallMs)
    {
        m_useWall = true;
        m_base = wallMs - m_last;       // いまの時刻から連続させる（戻らない・飛ばない）
    }
    double m_begin = 0, m_base = 0, m_last = 0, m_refWall = 0, m_refAudio = 0;
    bool m_expect = false, m_useWall = true, m_sawAudio = false, m_started = false;
};

// 鳴らさない条件（GG の se_cue_sheet §5 から。EXEC §2.6）。どれか 1 つでも当てはまれば音声デバイスを開かない。
struct SoundGate
{
    bool settingOn = true;       // 設定 startupSound
    bool argOn = true;           // --no-splash-sound が無い
    bool envOff = false;         // UNO_NO_SPLASH_SOUND
    bool headless = false;       // --headless / --background / --splash-selftest / テストの窓なし
    bool remoteSession = false;  // リモートデスクトップ
    bool Allowed() const { return settingOn && argOn && !envOff && !headless && !remoteSession; }
};

// 条件を満たすときだけ openDevice を呼ぶ（呼ばなければ false）。実機の SplashAudio はこれを通してデバイスを開く。
inline bool OpenIfAllowed(const SoundGate& gate, const std::function<bool()>& openDevice)
{
    if (!gate.Allowed() || !openDevice) return false;
    return openDevice();
}

// ---------------------------------------------------------------- シーケンサ（ボイスのミキサー）

enum class VoiceKind : std::uint8_t { Master, Bed, Click, LockTail };

class CalibrateSequencer
{
public:
    explicit CalibrateSequencer(std::shared_ptr<const CalibrateBank> bank, bool reduceMotion = false)
        : m_bank(std::move(bank)), m_reduce(reduceMotion) {}

    // ---- 開始: 速い経路は master を 1 ボイスでそのまま（フレーム 0 から）
    void Begin()
    {
        Voice v;
        v.kind = VoiceKind::Master;
        v.src = (m_reduce ? m_bank->rm : m_bank->full).data();
        v.frames = m_bank->FullFrames();
        m_hasMaster = true;
        AddVoice(v);
    }
    // 遅い経路を最初から（GaugeSlowDriver を単独で使う場合）。ベッドをフレーム 0 から鳴らす
    void BeginSlow() { StartBed(0, 0); }

    // ---- コマンド（タイムライン ms → 出力フレーム。出力フレーム = タイムラインのサンプル）
    // 速い経路 → 遅い経路の持ち替え（GaugeFastDriver::HandOverPending が立った最初のフレームで 1 回）。nowMs はいま聞こえている時刻。
    // 戻り値: 持ち替えた（または持ち替えが要らない状態）なら true。手遅れ（master の C23 の頭までに最短のフェードを終えられない。描き置きが進み過ぎ）なら
    // false で何もしない: master のまま最後まで鳴らす。呼び出し側は絵（ドライバ）も速い経路のままにすること（VetoHandOver）。
    bool HandOver(double nowMs)
    {
        if (m_handed || m_skipped || m_tailStarted || m_reduce) return true;
        const std::int64_t s = std::max(m_outFrame, seq::MsToFrame(nowMs));
        // full のクリック C23 の頭（1,543）までにフェードを終える（遅れても 4ms は掛ける）
        const std::int64_t deadline = seq::MsToFrame(calibrate::kClickMs[calibrate::kClickCount - 1]);
        const std::int64_t minLen = seq::MsToFrame(seq::kHandoverMinFadeMs);
        if (m_hasMaster && s + minLen > deadline - 1) { ++m_handRefused; return false; }   // C23 とロックが二重に鳴るので持ち替えない
        m_handed = true;
        const std::int64_t len = std::clamp<std::int64_t>(deadline - s - 1, minLen, seq::MsToFrame(seq::kHandoverFadeMs));
        m_handFrame = s;
        m_handLen = len;
        for (Voice& v : m_voices)
            if (v.kind == VoiceKind::Master) FadeOut(v, s, len);
        StartBed(s, len);
        return true;
    }

    // 予約（GaugeCue）。Click は click_NN（パンつき）、LockTail は lock_tail を T_lock' にサンプル単位で合わせる。
    void AddCue(const GaugeCue& c)
    {
        if (m_skipped) return;                      // スキップ後の予約は鳴らさない（スキップの LockTail は Skip() が鳴らす）
        const std::int64_t at = std::max(m_outFrame, seq::MsToFrame(c.playAtMs));
        if (at > seq::MsToFrame(c.playAtMs)) ++m_lateCues;
        if (c.kind == GaugeCueKind::Click)
        {
            if (c.index < 0 || c.index >= calibrate::kClickCount) return;
            if (m_tailStarted && at >= m_tailT) return;
            Voice v;
            v.kind = VoiceKind::Click;
            const std::vector<float>& s = m_bank->click[static_cast<std::size_t>(c.index)];
            v.src = s.data();
            v.frames = static_cast<std::int64_t>(s.size());
            v.mono = true;
            v.start = at;
            const seq::PanLaw pl = seq::MakePanLaw(c.pan);
            v.gainL = pl.gainL; v.gainR = pl.gainR; v.delayL = pl.delayL; v.delayR = pl.delayR;
            v.cueId = c.id;
            AddVoice(v);
        }
        else
        {
            if (m_tailStarted) return;              // ロックは 1 回だけ
            StartTail(at, /*cutOthers*/ true);
        }
    }

    // スキップで取り消す予約（GaugeSlowDriver::TakeCancelledCueIds）。まだ鳴っていなければ外し、鳴り始めていれば短く落とす
    void CancelCue(std::uint32_t id)
    {
        if (id == 0) return;
        for (std::size_t i = 0; i < m_voices.size(); ++i)
        {
            Voice& v = m_voices[i];
            if (v.cueId != id) continue;
            if (v.start >= m_outFrame) { m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(i)); return; }
            FadeOut(v, m_outFrame, seq::MsToFrame(seq::kCancelFadeMs));
            return;
        }
    }

    // スキップ（キー・クリック）。鳴っているボイスを 30ms で落とし、lock_tail を頭から鳴らす。以降の予約は鳴らさない。
    // 描き置き済みのぶんは書き換えられないので、実際の時刻は max(tMs, 描き終えた時刻)。usedMs にそれを返す（絵も同じ時刻でロックへ飛ばす。1 回の呼び出しで原子的）。
    // 効いたら true。ロックの音が既に鳴り始めている・master のロックを過ぎている・2 回目なら何もせず false（ロック音を重ねない）。
    bool Skip(double tMs, double* usedMs = nullptr)
    {
        const std::int64_t f = std::max(m_outFrame, seq::MsToFrame(tMs));
        const double used = f > seq::MsToFrame(tMs) ? seq::FrameToMs(static_cast<double>(f)) : tMs;
        if (usedMs) *usedMs = used;
        if (m_skipped) return false;
        if (m_tailStarted && m_tailT <= f) return false;   // ドライバと同じ: ロック以降のスキップは何もしない（ロック音を重ねない）
        if (m_hasMaster && !m_handed && !m_tailStarted && used >= calibrate::kLockMs) return false;   // master 自身がロックを鳴らしている（Reduce Motion の通常経路も）
        m_skipped = true;
        const std::int64_t len = seq::MsToFrame(seq::kSkipFadeMs);
        for (std::size_t i = 0; i < m_voices.size();)
        {
            Voice& v = m_voices[i];
            if (v.start >= f) { m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(i)); continue; }   // まだ鳴っていない（予約したロック音を含む）
            FadeOut(v, f, len);
            ++i;
        }
        m_tailStarted = false;
        StartTail(f, /*cutOthers*/ false);
        return true;
    }

    // 中止（短くフェードアウト）。以降の予約は受け付けない
    void Abort()
    {
        if (m_aborted) return;
        m_aborted = true;
        m_skipped = true;
        const std::int64_t len = seq::MsToFrame(seq::kAbortFadeMs);
        for (std::size_t i = 0; i < m_voices.size();)
        {
            Voice& v = m_voices[i];
            if (v.start >= m_outFrame) { m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
            FadeOut(v, m_outFrame, len);
            ++i;
        }
    }

    // ---- 出力: stereo interleaved の float。clip しない（変換側で丸める）。n フレーム進める
    void Render(float* out, int n)
    {
        std::fill(out, out + static_cast<std::size_t>(n) * 2, 0.0f);
        for (const Voice& v : m_voices) MixVoice(v, out, n);
        m_outFrame += n;
        std::erase_if(m_voices, [&](const Voice& v) { return VoiceEnd(v) <= m_outFrame; });
    }

    // ---- 照会
    std::int64_t OutFrame() const { return m_outFrame; }                     // 次に Render するフレーム
    double RenderedMs() const { return seq::FrameToMs(static_cast<double>(m_outFrame)); }   // 音が描き終えたタイムライン時刻（これより前は書き換えられない）
    bool Idle() const { return m_voices.empty() && m_outFrame > 0; }         // 鳴らすものが無い（終わった）
    std::size_t VoiceCount() const { return m_voices.size(); }
    bool Handed() const { return m_handed; }
    bool Skipped() const { return m_skipped; }
    bool TailStarted() const { return m_tailStarted; }
    std::int64_t TailStartFrame() const { return m_tailStarted ? m_tailT : -1; }
    int LateCues() const { return m_lateCues; }                              // 発音時刻に間に合わず後ろへずれた予約の数（診断）
    int StolenVoices() const { return m_stolen; }                            // ボイスの枠が足りず、ほかのクリックを外した数
    int DroppedCues() const { return m_dropped; }                            // 枠が無くて鳴らせなかった数
    int HandOverRefused() const { return m_handRefused; }                    // 手遅れで持ち替えを断った数（診断）
    std::int64_t HandOverFrame() const { return m_handed ? m_handFrame : -1; }
    std::int64_t HandOverLen() const { return m_handed ? m_handLen : 0; }
    // 診断: 種別ごとの生きているボイス数
    int CountKind(VoiceKind k) const { int n = 0; for (const Voice& v : m_voices) if (v.kind == k) ++n; return n; }

private:
    struct Voice
    {
        const float* src = nullptr;
        std::int64_t frames = 0;             // ソースのフレーム数
        bool mono = false;
        std::int64_t start = 0;              // src[srcOffset] が鳴る出力フレーム
        std::int64_t srcOffset = 0;
        float gainL = 1.f, gainR = 1.f;
        std::int64_t delayL = 0, delayR = 0;
        bool loop = false;
        std::int64_t loopStart = 0, loopEnd = 0, loopX = 0;
        std::int64_t fadeInLen = 0;
        std::int64_t fadeOutStart = -1, fadeOutLen = 0;
        std::uint32_t cueId = 0;
        VoiceKind kind = VoiceKind::Click;
        std::uint64_t order = 0;
    };

    static constexpr double kPi = 3.14159265358979323846;

    static void FadeOut(Voice& v, std::int64_t from, std::int64_t len)
    {
        len = std::max<std::int64_t>(1, len);
        if (v.fadeOutStart >= 0 && v.fadeOutStart <= from) return;   // もっと早く落ち始めている
        v.fadeOutStart = from;
        v.fadeOutLen = len;
    }

    // 同時に鳴るボイス（フェード中で残っているものも数える）は常に kMaxVoices 以下。満杯のとき:
    //   ・Click は鳴らさない（落とした数を数える。大量の歯を一度に跨いだときだけ起きる）
    //   ・大事なボイス（Bed / LockTail）はクリックを 1 本外して枠を空ける
    bool AddVoice(Voice v)
    {
        v.order = ++m_order;
        if (static_cast<int>(m_voices.size()) >= seq::kMaxVoices)
        {
            if (v.kind == VoiceKind::Click || !EvictClick()) { ++m_dropped; return false; }
        }
        m_voices.push_back(v);
        return true;
    }

    // クリックを 1 本外す。まだ鳴っていないもの（無音）→ フェード中で終わりが近いもの → いちばん古いもの の順
    bool EvictClick()
    {
        std::size_t best = m_voices.size();
        int bestRank = 3;
        std::int64_t bestKey = 0;
        for (std::size_t i = 0; i < m_voices.size(); ++i)
        {
            const Voice& v = m_voices[i];
            if (v.kind != VoiceKind::Click) continue;
            int rank;
            std::int64_t key;
            if (v.start > m_outFrame) { rank = 0; key = -v.start; }
            else if (v.fadeOutStart >= 0) { rank = 1; key = v.fadeOutStart + v.fadeOutLen; }
            else { rank = 2; key = static_cast<std::int64_t>(v.order); }
            if (rank < bestRank || (rank == bestRank && key < bestKey)) { best = i; bestRank = rank; bestKey = key; }
        }
        if (best == m_voices.size()) return false;
        m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(best));
        ++m_stolen;
        return true;
    }

    // ベッド（stems/bed.wav）をタイムライン時刻 atFrame から。bed はタイムラインと揃っている（srcOffset = atFrame）。ループ区間と継ぎ目は events.json の loop
    void StartBed(std::int64_t atFrame, std::int64_t fadeIn)
    {
        Voice v;
        v.kind = VoiceKind::Bed;
        v.src = m_bank->bed.data();
        v.frames = static_cast<std::int64_t>(m_bank->bed.size() / 2);
        v.start = atFrame;
        v.srcOffset = atFrame;
        v.loop = true;
        v.loopStart = seq::MsToFrame(calibrate::kBedLoopStartMs);
        v.loopEnd = seq::MsToFrame(calibrate::kBedLoopEndMs);
        v.loopX = seq::MsToFrame(calibrate::kBedLoopCrossfadeMs);
        v.fadeInLen = fadeIn;
        AddVoice(v);
    }

    // ロック音を tailT から頭出し。cutOthers: ベッドとクリックを tailT で 0 にする（フェードは tailT に向けて終える）。tailT より後に始まる予約は外す
    void StartTail(std::int64_t at, bool cutOthers)
    {
        m_tailStarted = true;
        m_tailT = at;
        if (cutOthers)
        {
            const std::int64_t len = seq::MsToFrame(seq::kTailCutFadeMs);
            for (std::size_t i = 0; i < m_voices.size();)
            {
                Voice& v = m_voices[i];
                if (v.start >= at) { m_voices.erase(m_voices.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
                const std::int64_t from = std::max(m_outFrame, at - len);
                if (v.fadeOutStart < 0 || v.fadeOutStart + v.fadeOutLen > at)
                {
                    v.fadeOutStart = -1;                              // 既存のもっと遅いフェードは置き換える
                    FadeOut(v, from, at - from);
                }
                ++i;
            }
        }
        Voice v;
        v.kind = VoiceKind::LockTail;
        v.src = m_bank->lockTail.data();
        v.frames = static_cast<std::int64_t>(m_bank->lockTail.size() / 2);
        v.start = at;
        AddVoice(v);
    }

    // ソースの位置 q（ループ展開前）のサンプル。ループの継ぎ目は等パワーのクロスフェード（雑音のベッドは無相関なので）
    float Fetch(const Voice& v, std::int64_t q, int ch) const
    {
        auto at = [&](std::int64_t i) -> float { return v.mono ? v.src[i] : v.src[i * 2 + ch]; };
        if (!v.loop) return q < v.frames ? at(q) : 0.0f;
        const std::int64_t span = v.loopEnd - v.loopStart;
        const std::int64_t u = q < v.loopStart ? q : v.loopStart + (q - v.loopStart) % span;
        if (u >= v.loopEnd - v.loopX && v.loopX > 0)
        {
            const double w = static_cast<double>(u - (v.loopEnd - v.loopX)) / static_cast<double>(v.loopX);
            const double a = std::cos(w * kPi * 0.5), b = std::sin(w * kPi * 0.5);
            return static_cast<float>(a * static_cast<double>(at(u)) + b * static_cast<double>(at(u - span)));
        }
        return at(u);
    }

    static float Envelope(const Voice& v, std::int64_t f)
    {
        float e = 1.0f;
        if (v.fadeInLen > 0)
        {
            const std::int64_t d = f - v.start;
            if (d < v.fadeInLen) e *= static_cast<float>(0.5 * (1.0 - std::cos(kPi * static_cast<double>(d) / static_cast<double>(v.fadeInLen))));
        }
        if (v.fadeOutStart >= 0 && f >= v.fadeOutStart)
        {
            const std::int64_t d = f - v.fadeOutStart;
            e *= d >= v.fadeOutLen ? 0.0f : static_cast<float>(0.5 * (1.0 + std::cos(kPi * static_cast<double>(d) / static_cast<double>(v.fadeOutLen))));
        }
        return e;
    }

    // このボイスが鳴り終わる出力フレーム（その手前まで）。ループは落とされるまで続く
    static std::int64_t VoiceEnd(const Voice& v)
    {
        std::int64_t end = v.loop ? std::numeric_limits<std::int64_t>::max()
                                  : v.start + std::max<std::int64_t>(0, v.frames - v.srcOffset) + std::max(v.delayL, v.delayR);
        if (v.fadeOutStart >= 0) end = std::min(end, v.fadeOutStart + v.fadeOutLen);
        return end;
    }

    void MixVoice(const Voice& v, float* out, int n) const
    {
        const std::int64_t f0 = m_outFrame;
        const std::int64_t end = VoiceEnd(v);
        const std::int64_t first = std::max(f0, v.start);
        const std::int64_t last = std::min(f0 + n, end);
        for (std::int64_t f = first; f < last; ++f)
        {
            const float env = Envelope(v, f);
            if (env == 0.0f) continue;
            float* o = out + static_cast<std::size_t>(f - f0) * 2;
            const std::int64_t pl = f - v.start - v.delayL, pr = f - v.start - v.delayR;
            if (pl >= 0) o[0] += Fetch(v, pl + v.srcOffset, 0) * v.gainL * env;
            if (pr >= 0) o[1] += Fetch(v, pr + v.srcOffset, 1) * v.gainR * env;
        }
    }

    std::shared_ptr<const CalibrateBank> m_bank;
    bool m_reduce = false;
    std::vector<Voice> m_voices;
    std::int64_t m_outFrame = 0;
    std::uint64_t m_order = 0;
    bool m_handed = false, m_skipped = false, m_tailStarted = false, m_aborted = false;
    std::int64_t m_tailT = 0;
    int m_lateCues = 0, m_stolen = 0, m_dropped = 0, m_handRefused = 0;
    bool m_hasMaster = false;
    std::int64_t m_handFrame = 0, m_handLen = 0;
};

} // namespace dx12e::splash
