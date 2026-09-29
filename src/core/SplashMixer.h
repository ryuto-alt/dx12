#pragma once

// ===========================================================================
// 起動音のソフトウェアミキサー（ヘッダオンリーの純ロジック。音デバイス / Win32 に依存しない）
// ---------------------------------------------------------------------------
// 1 本の音源を「riser（頂点までの準備）」と「hit + tail（頂点〜余韻）」に分けて持ち、
// 実際の初期化の終わり方に合わせて つなぎ方だけを変える。
//
//   源（frames）:  [0 ............ H .. O ............................ N)
//                   └── riser ──┘└X┘└──────────── hit + tail ───────────┘
//     O = onset（頂点の立ち上がり）/ X = クロスフェード長 / H = O - X（hit 区間の開始）
//     riser は [0, O) を持つ（H〜O は「定刻」のとき riser がそのまま流れ込む区間）。
//
//   ・定刻:   riser が H に届いた瞬間に TriggerHit → 同じ内容を線形クロスフェード = 源と完全に一致（継ぎ目なし）
//   ・早着:   riser の途中で TriggerHit → 80〜150ms（X）で riser を絞り hit を上げる。onset は trigger + X に来る
//   ・遅着:   riser が H に届いても TriggerHit が来ない → 直前 holdLoop(300ms) を区間内ループで保持
//             （ループの継ぎ目は wrap 直後の seam 区間で「直進 → ループ先頭」を等パワーで混ぜる）
//   ・中止:   Abort() で短くフェードアウトして停止
//
// 再生開始位置（適応同期）: Start(T) の T は「TriggerHit が来ると予測する時刻（秒）」。riser を
//   H - T*sr から再生し始め、予測どおりなら T 秒後にちょうど H に届く。T が riser より長ければ先頭に無音を置く。
//
// すべて float のステレオ(interleaved)。出力は [-1,1] にクリップ。スレッド安全性は呼び出し側が持つ
// （音デバイスのスレッドが Render、別スレッドが RequestHit は atomic 経由: SplashSound が包む）。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dx12e::splash
{

// ---------------------------------------------------------------- WAV（PCM16 の最小パーサ。メモリ上）

struct WavData
{
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> frames;   // ステレオ interleaved（モノラル入力は L=R へ複製）
    size_t frameCount() const { return frames.size() / 2; }
};

inline uint32_t RdU32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t RdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// RIFF/WAVE の PCM 16bit（モノ/ステレオ）だけを受け付ける。それ以外は false。
inline bool ParseWavPcm16(const uint8_t* data, size_t size, WavData& out)
{
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return false;
    size_t pos = 12;
    int fmtTag = 0, ch = 0, bits = 0, rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    while (pos + 8 <= size)
    {
        const uint32_t len = RdU32(data + pos + 4);
        const uint8_t* body = data + pos + 8;
        const size_t avail = size - (pos + 8);
        const size_t n = len < avail ? len : avail;
        if (std::memcmp(data + pos, "fmt ", 4) == 0 && n >= 16)
        {
            fmtTag = RdU16(body);
            ch = RdU16(body + 2);
            rate = static_cast<int>(RdU32(body + 4));
            bits = RdU16(body + 14);
        }
        else if (std::memcmp(data + pos, "data", 4) == 0)
        {
            pcm = body;
            pcmBytes = n;
        }
        pos += 8 + static_cast<size_t>(len) + (len & 1u);
    }
    if (fmtTag != 1 || bits != 16 || (ch != 1 && ch != 2) || rate <= 0 || !pcm) return false;
    const size_t frames = pcmBytes / (static_cast<size_t>(ch) * 2);
    out.sampleRate = rate;
    out.channels = ch;
    out.frames.assign(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i)
    {
        const int16_t l = static_cast<int16_t>(RdU16(pcm + i * static_cast<size_t>(ch) * 2));
        const int16_t r = (ch == 2) ? static_cast<int16_t>(RdU16(pcm + i * 4 + 2)) : l;
        out.frames[i * 2]     = static_cast<float>(l) / 32768.0f;
        out.frames[i * 2 + 1] = static_cast<float>(r) / 32768.0f;
    }
    return true;
}

// ---------------------------------------------------------------- メタ JSON（tools/prep_splash_sound.py が書く splash_d.json）

// {"onsetFrame": 187000, ...} のような単純な数値フィールドを取り出す（nlohmann に依存しないための最小実装）。
inline bool ParseJsonInt(const char* text, size_t len, const char* key, int64_t& out)
{
    const std::string pat = std::string("\"") + key + "\"";
    const std::string_view sv(text, len);
    size_t k = sv.find(pat);
    if (k == std::string_view::npos) return false;
    k = sv.find(':', k + pat.size());
    if (k == std::string_view::npos) return false;
    ++k;
    while (k < len && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n')) ++k;
    const std::string num(sv.substr(k, 24));
    char* end = nullptr;
    const long long v = std::strtoll(num.c_str(), &end, 10);
    if (end == num.c_str()) return false;
    out = static_cast<int64_t>(v);
    return true;
}

// ---------------------------------------------------------------- 音源

struct SoundAsset
{
    int sampleRate = 44100;
    std::vector<float> src;         // ステレオ interleaved（全区間）
    int64_t onsetFrame = 0;         // O。0 なら riser 無し（hit だけ）
    int64_t popOffsetFrames = 0;    // 視覚の「ポン」を合わせる位置（onset からの遅れ）
    // 便宜
    int64_t frameCount() const { return static_cast<int64_t>(src.size() / 2); }
};

// WAV とメタ（onset / popOffset）から音源を組む。onset が範囲外なら false（riser 無しの hit 単体としては使わない）。
inline bool MakeAssetFromWav(WavData&& w, int64_t onsetFrame, int64_t popOffsetFrames, SoundAsset& out)
{
    if (w.frames.empty() || onsetFrame < 0 || onsetFrame >= static_cast<int64_t>(w.frameCount())) return false;
    out.sampleRate = w.sampleRate;
    out.src = std::move(w.frames);
    out.onsetFrame = onsetFrame;
    out.popOffsetFrames = std::max<int64_t>(0, popOffsetFrames);
    return true;
}

struct MixerConfig
{
    double xfadeSec = 0.12;        // riser → hit のクロスフェード（80〜150ms）
    double holdLoopSec = 0.30;     // 遅着時にループ保持する区間長
    double holdSeamSec = 0.06;     // ループの継ぎ目のクロスフェード
    double startFadeSec = 0.04;    // riser を途中から始めるときのフェードイン
    double abortFadeSec = 0.06;    // 中止時のフェードアウト
    double holdDuckTo = 0.45;      // 遅着で保持が長引くとき、ループの音量をここまで絞る（同じ 300ms の繰り返しが耳に残らないように）
    double holdDuckSec = 3.0;      // 保持を始めてからこの秒数かけて絞る（0 で無効）
};

enum class MixState { Idle, Delay, Riser, Hold, Crossfade, Hit, Tail, Stopped };

class Mixer
{
public:
    explicit Mixer(std::shared_ptr<const SoundAsset> asset, MixerConfig cfg = MixerConfig())
        : a_(std::move(asset)), cfg_(cfg)
    {
        const int sr = a_ ? a_->sampleRate : 44100;
        N_ = a_ ? a_->frameCount() : 0;
        O_ = a_ ? std::min<int64_t>(a_->onsetFrame, N_) : 0;
        X_ = std::min<int64_t>(SecToFrames(cfg_.xfadeSec, sr), O_);
        H_ = O_ - X_;
        Lh_ = std::min<int64_t>(SecToFrames(cfg_.holdLoopSec, sr), H_);
        S_ = std::min<int64_t>(SecToFrames(cfg_.holdSeamSec, sr), std::min<int64_t>(Lh_ / 2, X_ > 0 ? X_ : Lh_ / 2));
        startFade_ = SecToFrames(cfg_.startFadeSec, sr);
        duckFrames_ = SecToFrames(cfg_.holdDuckSec, sr);
        abortFade_ = std::max<int64_t>(1, SecToFrames(cfg_.abortFadeSec, sr));
    }

    // 予測した「TriggerHit が来る時刻」T（秒）を渡して開始。riser の再生開始位置と先頭の無音を決める。
    void Start(double predictedTriggerSec)
    {
        started_ = true;
        out_ = 0;
        hitOnsetOut_ = -1;
        hit_ = false; hitStartOut_ = -1;
        hitReq_ = false; hitReqFrame_ = -1;
        aborted_ = false; stopped_ = false;
        blendK_ = -1; wrapAllowed_ = true;
        looping_ = false; holdFrames_ = 0;
        riserActive_ = H_ > 0;
        const int sr = a_ ? a_->sampleRate : 44100;
        int64_t p0 = H_ - SecToFrames(std::max(0.0, predictedTriggerSec), sr);
        delay_ = 0;
        if (p0 < 0) { delay_ = -p0; p0 = 0; }
        pos_ = p0;
        riserStartFrame_ = -1;        // 最初に音が出たフレーム（フェードイン用）
        fadeFromMid_ = (p0 > 0);
    }

    // 次に Render するサンプルから hit へ移る。
    void RequestHit() { hitReq_ = true; hitReqFrame_ = -1; }
    // 出力フレーム番号 f から hit へ移る（テスト・サンプル精度の同期用）。
    void RequestHitAtFrame(int64_t f) { hitReq_ = true; hitReqFrame_ = f; }
    void Abort() { if (!aborted_ && started_ && !stopped_) { aborted_ = true; abortStart_ = out_; } }

    bool Finished() const { return stopped_ || !started_; }
    int64_t OutputFrame() const { return out_; }
    // 頂点（onset）が出力の何フレーム目に来るか。TriggerHit されるまで -1。
    int64_t HitOnsetOutputFrame() const { return hitOnsetOut_; }
    // 視覚の「ポン」を合わせるフレーム（onset + popOffset）。TriggerHit されるまで -1。
    int64_t PopOutputFrame() const { return hitOnsetOut_ < 0 ? -1 : hitOnsetOut_ + (a_ ? a_->popOffsetFrames : 0); }
    // TriggerHit を出してから「ポン」までの遅れ（秒）。スケジューラが逆算に使う。
    double TriggerToPopSec() const
    {
        const int sr = a_ ? a_->sampleRate : 44100;
        return static_cast<double>(X_ + (a_ ? a_->popOffsetFrames : 0)) / sr;
    }
    // riser が H に届く位置（=定刻の TriggerHit 位置）。テスト用。
    int64_t HoldEnd() const { return H_; }
    int64_t Onset() const { return O_; }
    int64_t Xfade() const { return X_; }
    int64_t HoldLoop() const { return Lh_; }
    int64_t Seam() const { return S_; }

    MixState State() const
    {
        if (!started_) return MixState::Idle;
        if (stopped_) return MixState::Stopped;
        if (hit_)
        {
            const int64_t k = out_ - hitStartOut_;
            if (k < X_) return MixState::Crossfade;
            if (k < X_ + kHitSpan) return MixState::Hit;
            return MixState::Tail;
        }
        if (delay_ > 0) return MixState::Delay;
        if (looping_) return MixState::Hold;
        return MixState::Riser;
    }

    // interleaved stereo を frames 分書く。停止後は 0 を書く。
    void Render(float* out, size_t frames)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            float l = 0.0f, r = 0.0f;
            if (started_ && !stopped_) Sample(l, r);
            l = std::max(-1.0f, std::min(1.0f, l));
            r = std::max(-1.0f, std::min(1.0f, r));
            out[i * 2] = l; out[i * 2 + 1] = r;
            ++out_;
        }
    }

private:
    static constexpr int64_t kHitSpan = 22050;   // Hit と Tail の境（状態表示だけに使う）

    static int64_t SecToFrames(double s, int sr) { return static_cast<int64_t>(std::llround(s * sr)); }

    void Src(int64_t i, float& l, float& r) const
    {
        if (i < 0 || i >= N_) { l = r = 0.0f; return; }
        l = a_->src[static_cast<size_t>(i) * 2];
        r = a_->src[static_cast<size_t>(i) * 2 + 1];
    }

    void Sample(float& l, float& r)
    {
        // --- TriggerHit の受付（このサンプルから）
        if (!hit_ && hitReq_ && (hitReqFrame_ < 0 || out_ >= hitReqFrame_))
        {
            hit_ = true;
            hitStartOut_ = out_;
            hitPos_ = H_;
            wrapAllowed_ = false;                 // これ以上 riser をループさせない
            hitOnsetOut_ = out_ + (O_ - H_);      // hit は H から流れ、O が onset
        }

        double gr = 1.0, gh = 0.0;
        if (hit_)
        {
            const int64_t k = out_ - hitStartOut_;
            gh = X_ > 0 ? std::min(1.0, static_cast<double>(k) / static_cast<double>(X_)) : 1.0;
            gr = 1.0 - gh;
        }
        if (aborted_)
        {
            const double f = 1.0 - static_cast<double>(out_ - abortStart_) / static_cast<double>(abortFade_);
            if (f <= 0.0) { stopped_ = true; return; }
            gr *= f; gh *= f;
        }

        // --- riser
        float rl = 0.0f, rr = 0.0f;
        if (riserActive_ && gr > 0.0)
        {
            if (delay_ > 0)
            {
                --delay_;
            }
            else
            {
                if (riserStartFrame_ < 0) riserStartFrame_ = out_;
                // H に届いて、まだ hit が来ていなければループ保持へ（判定は TriggerHit の受付より後）。
                if (wrapAllowed_ && pos_ >= H_ && Lh_ > 0 && blendK_ < 0)
                {
                    looping_ = true;
                    pos_ = H_ - Lh_;
                    blendK_ = (S_ > 0) ? 0 : -1;
                }
                if (blendK_ >= 0 && blendK_ < S_)
                {
                    // wrap 直後の seam: 「直進（H+k）」から「ループ先頭（H-Lh+k = pos_）」へ等パワーで移る。
                    float a0, a1, b0, b1;
                    Src(H_ + blendK_, a0, a1);
                    Src(pos_, b0, b1);
                    const double u = (static_cast<double>(blendK_) + 0.5) / static_cast<double>(S_);
                    const float wa = static_cast<float>(std::cos(u * 1.5707963267948966));
                    const float wb = static_cast<float>(std::sin(u * 1.5707963267948966));
                    rl = a0 * wa + b0 * wb;
                    rr = a1 * wa + b1 * wb;
                    ++blendK_;
                    if (blendK_ >= S_) blendK_ = -1;
                }
                else
                {
                    Src(pos_, rl, rr);
                }
                ++pos_;
                if (pos_ >= O_ && (!wrapAllowed_ || Lh_ == 0)) riserActive_ = false;   // 直進で O まで来た（hit へ移行済み）
                // フェードイン（途中から始めたとき）
                if (fadeFromMid_ && startFade_ > 0)
                {
                    const int64_t k = out_ - riserStartFrame_;
                    if (k < startFade_)
                    {
                        const float g = static_cast<float>(0.5 - 0.5 * std::cos(kPiF * static_cast<double>(k) / static_cast<double>(startFade_)));
                        rl *= g; rr *= g;
                    }
                }
            }
        }

        // 保持が長引くほど riser の音量を絞る（クロスフェードで hit へ移るときは絞った音から上がる）
        if (looping_ && duckFrames_ > 0)
        {
            const double k = std::min(1.0, static_cast<double>(holdFrames_) / static_cast<double>(duckFrames_));
            const float g = static_cast<float>(1.0 - (1.0 - cfg_.holdDuckTo) * k);
            rl *= g; rr *= g;
            ++holdFrames_;
        }

        // --- hit
        float hl = 0.0f, hr = 0.0f;
        if (hit_)
        {
            if (hitPos_ >= N_) { stopped_ = true; return; }
            Src(hitPos_, hl, hr);
            ++hitPos_;
        }
        l = static_cast<float>(rl * gr + hl * gh);
        r = static_cast<float>(rr * gr + hr * gh);
        if (hit_ && hitPos_ >= N_) stopped_ = true;   // 源の最後のサンプルまで出し切った
    }

    static constexpr double kPiF = 3.14159265358979323846;

    std::shared_ptr<const SoundAsset> a_;
    MixerConfig cfg_;
    int64_t N_ = 0, O_ = 0, X_ = 0, H_ = 0, Lh_ = 0, S_ = 0, startFade_ = 0, abortFade_ = 1, duckFrames_ = 0, holdFrames_ = 0;

    bool started_ = false, stopped_ = false, aborted_ = false;
    int64_t out_ = 0, abortStart_ = 0;
    // riser
    bool riserActive_ = false, fadeFromMid_ = false, looping_ = false, wrapAllowed_ = true;
    int64_t pos_ = 0, delay_ = 0, blendK_ = -1, riserStartFrame_ = -1;
    // hit
    bool hit_ = false, hitReq_ = false;
    int64_t hitReqFrame_ = -1, hitStartOut_ = -1, hitPos_ = 0, hitOnsetOut_ = -1;
};

// ---------------------------------------------------------------- 検査用: サンプル間の不連続量

// 隣り合うサンプル（同チャンネル）の差の最大値。クリック（区間端の段差）の検出に使う。
// 区間 [from, to)（フレーム）だけ見る。
inline float MaxStepDiff(const std::vector<float>& interleaved, size_t fromFrame, size_t toFrame)
{
    float m = 0.0f;
    const size_t n = interleaved.size() / 2;
    if (toFrame > n) toFrame = n;
    for (size_t i = fromFrame + 1; i < toFrame; ++i)
        for (int c = 0; c < 2; ++c)
            m = std::max(m, std::fabs(interleaved[i * 2 + c] - interleaved[(i - 1) * 2 + c]));
    return m;
}

} // namespace dx12e::splash
