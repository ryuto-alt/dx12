#pragma once

// ===========================================================================
// 起動画面（スプラッシュ）の「動き」の純ロジック（ヘッダオンリー。Win32 / GPU / 音デバイスに依存しない）
// ---------------------------------------------------------------------------
// ・イージング / スプリング（減衰振動の解析解）/ 決定論の乱数（火花の軌道）
// ・タイムライン状態機械: Intro → Loading → Ready → Transition → Done
//     入力は「時刻 t・目標進捗・終了要求の時刻・メイン窓を出した時刻」だけ。窓も時計も持たないので、
//     プレビュー（--splash-preview）と実窓が【同じ関数】を通り、単体テストで決定論的に検証できる。
// ・文言のクロスフェード（TextSwapper）/ Tips のローテーション（TipRotator）
//
// 単位: 時間は秒(double)、画面寸法は論理 px（100% 表示の px。DPI 換算は描画側）。
// イージングの選び分け（docs/UI_STYLE_GUIDE.md のモーション表に沿う）:
//   ロゴのポンと登場 = スプリング（祝祭。行き過ぎて戻る）/ 文字の登場 = スプリング(やや硬め) /
//   リング完成 = out(cubic) / ハイライト一周 = inOut / 衝撃波 = expo / 文言の入替 = out /
//   退場 = 拡大 out(cubic) + フェード in(quad)。全要素を同じ duration / easing にしない。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dx12e::splash
{

// ---------------------------------------------------------------- 基本

inline double Clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- イージング（入力 t は 0..1 に丸める）

inline double EaseOutCubic(double t) { t = Clamp01(t); const double u = 1.0 - t; return 1.0 - u * u * u; }
inline double EaseOutQuint(double t) { t = Clamp01(t); const double u = 1.0 - t; return 1.0 - u * u * u * u * u; }
inline double EaseOutExpo(double t)  { t = Clamp01(t); return t >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * t); }
inline double EaseInQuad(double t)   { t = Clamp01(t); return t * t; }
inline double EaseInCubic(double t)  { t = Clamp01(t); return t * t * t; }
inline double EaseInOutCubic(double t)
{
    t = Clamp01(t);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}
inline double EaseInOutSine(double t) { t = Clamp01(t); return 0.5 - 0.5 * std::cos(kPi * t); }
// back: 行き過ぎて戻る（c1=1.70158 で約 10% のオーバーシュート）。
inline double EaseOutBack(double t)
{
    t = Clamp01(t);
    const double c1 = 1.70158, c3 = c1 + 1.0;
    const double u = t - 1.0;
    return 1.0 + c3 * u * u * u + c1 * u * u;
}
// 引いて（アンティシペーション）から発進する。
inline double EaseInBack(double t)
{
    t = Clamp01(t);
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return c3 * t * t * t - c1 * t * t;
}

// ---------------------------------------------------------------- スプリング（減衰振動の解析解）
// x'' + 2ζω x' + ω² x = 0（不足減衰 ζ<1）。初期変位 x0・初期速度 v0 から u 秒後の変位と速度。
// 「目標へ向かうスプリング」= 変位を「目標からのずれ」で持つ（x0 = -1 なら 0 → 1 のステップ応答 = 1 + x）。
struct SpringState { double x; double v; };

inline SpringState SpringFree(double u, double omega, double zeta, double x0, double v0)
{
    if (u <= 0.0) return { x0, v0 };
    if (zeta > 0.999) zeta = 0.999;
    if (zeta < 0.0)   zeta = 0.0;
    const double wd = omega * std::sqrt(1.0 - zeta * zeta);
    const double zw = zeta * omega;
    const double A = x0;
    const double B = (v0 + zw * x0) / wd;
    const double e = std::exp(-zw * u);
    const double c = std::cos(wd * u), s = std::sin(wd * u);
    SpringState r;
    r.x = e * (A * c + B * s);
    r.v = e * ((-zw * A + wd * B) * c + (-zw * B - wd * A) * s);
    return r;
}

// 0 → 1 のステップ応答（オーバーシュートして落ち着く）と、その速度。
inline double SpringStep(double u, double omega, double zeta)
{
    return 1.0 + SpringFree(u, omega, zeta, -1.0, 0.0).x;
}
inline double SpringStepVelocity(double u, double omega, double zeta)
{
    return SpringFree(u, omega, zeta, -1.0, 0.0).v;
}
// ステップ応答の最大オーバーシュート量（1 を超える分）。テスト/調整の目安。
inline double SpringOvershoot(double zeta)
{
    if (zeta >= 1.0) return 0.0;
    return std::exp(-kPi * zeta / std::sqrt(1.0 - zeta * zeta));
}

// ---------------------------------------------------------------- 決定論の乱数（火花の軌道など）

inline uint32_t Hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// seed と番号から 0..1 の値。
inline double Rand01(uint32_t seed, uint32_t i)
{
    return static_cast<double>(Hash32(seed * 0x9E3779B1U + i * 0x85EBCA6BU + 0x27D4EB2FU) >> 8) / 16777216.0;
}

// ---------------------------------------------------------------- 火花（バースト）
// 中心からの相対座標(論理 px)。t（バースト後の経過秒）の閉形式なので状態を持たない＝毎フレーム同じ絵になる。

struct BurstSpec
{
    int      count;          // 粒の数
    double   speedMin;       // px/s
    double   speedMax;
    double   life;           // 秒
    double   gravity;        // px/s²（下向き正）
    double   radius0;        // 発生位置（中心からの距離）
    double   sizeMin;        // px（星の半径）
    double   sizeMax;
    uint32_t seed;
    double   angleBias;      // 上向き寄りにする度合い 0..1（0=全方位）
};

struct Spark
{
    double x = 0, y = 0;     // 中心からの相対
    double size = 0;         // 半径 px（寿命で縮む）
    double alpha = 0;        // 0..1
    double rot = 0;          // ラジアン
    int    kind = 0;         // 0=星 1=点
    int    tint = 0;         // 0=白 1=淡い青 2=アクセント 3=琥珀（控えめに）
};

inline bool SparkAt(const BurstSpec& b, int i, double age, Spark& out)
{
    if (i < 0 || i >= b.count || age < 0.0 || age >= b.life) return false;
    const uint32_t u = static_cast<uint32_t>(i);
    const double a0 = (static_cast<double>(i) + Rand01(b.seed, u * 7 + 1) * 0.8) / b.count * 2.0 * kPi;
    // 上向き寄り: 角度を -90° 方向へ引き寄せる
    const double up = -0.5 * kPi;
    double ang = a0;
    {
        double d = std::remainder(a0 - up, 2.0 * kPi);
        ang = a0 - d * b.angleBias * 0.5;
    }
    const double sp = Lerp(b.speedMin, b.speedMax, Rand01(b.seed, u * 7 + 2));
    const double life = b.life * Lerp(0.65, 1.0, Rand01(b.seed, u * 7 + 3));
    if (age >= life) return false;
    const double k = age / life;                     // 0..1
    // 速度は指数で減衰（空気抵抗）: 位置 = v0/c (1 - e^{-c t})
    const double c = 3.2;
    const double dist = b.radius0 + sp / c * (1.0 - std::exp(-c * age));
    out.x = std::cos(ang) * dist;
    out.y = std::sin(ang) * dist + 0.5 * b.gravity * age * age;
    const double size0 = Lerp(b.sizeMin, b.sizeMax, Rand01(b.seed, u * 7 + 4));
    out.size  = size0 * (1.0 - 0.75 * EaseInQuad(k));
    out.alpha = (k < 0.12 ? k / 0.12 : 1.0) * (1.0 - EaseInCubic(k));
    out.rot   = Rand01(b.seed, u * 7 + 5) * kPi + age * (Rand01(b.seed, u * 7 + 6) - 0.5) * 6.0;
    out.kind  = (Rand01(b.seed, u * 7 + 6) < 0.62) ? 0 : 1;
    const double r = Rand01(b.seed, u * 11 + 9);
    out.tint  = r < 0.38 ? 0 : (r < 0.68 ? 1 : (r < 0.90 ? 2 : 3));
    return true;
}

// ---------------------------------------------------------------- タイムライン

enum class Phase { Intro, Loading, Ready, Transition, Done };

struct TimingParams
{
    double introDur      = 0.90;   // ロゴのポンと登場 + 火花 + ワードマーク
    double catchUpDur    = 0.25;   // ready: リングが現在値 → 100% へ
    double lapDur        = 0.35;   // ready: ハイライトがリングを一周
    double afterglow     = 0.28;   // ポンの後の余韻（ここでメイン窓を出す）
    double transitionDur = 0.35;   // メイン窓の上で拡大しながらフェードアウト
    double mainShowTimeout = 2.0;  // メイン窓が出せない（メインスレッドが詰まっている）ときの待ち上限
    double progressRate  = 5.0;    // 表示進捗が目標へ寄る速さ（1/s。指数接近）
    double progressCap   = 0.985;  // ready 前の表示進捗の上限（100% は ready の演出でだけ出す）

    double PopAfterReady() const { return catchUpDur + lapDur; }
    double ReadyDur() const { return PopAfterReady() + afterglow; }
};

// 1 フレームぶんの描画パラメータ（すべて論理 px / 0..1 / 秒）。
struct SplashFrame
{
    double t = 0;                 // 開始からの経過
    Phase  phase = Phase::Intro;

    // ウィンドウ全体（UpdateLayeredWindow の SourceConstantAlpha と、カードを含む全体の拡大）
    double windowAlpha = 0;       // 0..1（登場フェード × 退場フェード）
    double zoom = 1;              // 全体の拡大（登場 0.965→1、退場 1→1.09）

    // ロゴ（中心からの平行移動 / 拡縮 / 回転）
    double logoScaleX = 0, logoScaleY = 0;
    double logoDy = 0;
    double logoRotDeg = 0;
    double glow = 0;              // ロゴ背後の光 0..1（呼吸 + ポンで瞬発）
    double flash = 0;             // ロゴ中心の白い閃光 0..1（ポンの瞬間）

    // リング
    double ringTrack = 0;         // トラック（薄い円）の描き込み 0..1
    double progress = 0;          // 表示進捗 0..1
    int    percent = 0;           // 表示する % (0..100)
    double ringFlash = 0;         // ポンでリングが一瞬明るくなる 0..1
    bool   lapActive = false;     // ハイライトが周回中
    double lapAngle = 0;          // 周回の先頭位置 0..1（12 時から時計回り）
    double lapAlpha = 0;
    double shock = -1;            // 衝撃波の進行 0..1（<0 = 無し）

    // 個別の時計（描画側がワードマーク/情報のアニメに使う）。負 = まだ始まっていない
    double sinceReady = -1;
    double sincePop = -1;
    double sinceTransition = -1;

    // 火花: 発生時刻の一覧（intro のバースト / pop のバースト）
    double introBurstAge = -1;
    double popBurstAge = -1;

    // 状態イベント
    bool wantMainShow = false;    // 「今メイン窓を出してほしい」（owner が一度だけ拾う）
    bool finished = false;        // Done（窓を閉じてよい）
};

constexpr double kIntroLogoStart = 0.08;
constexpr double kIntroBurstAt   = 0.20;
constexpr double kRingTrackStart = 0.30;

// 火花の定義（バーストごとの見た目）
inline BurstSpec IntroBurst() { return { 16, 150.0, 300.0, 0.75, 260.0, 56.0, 3.0, 6.5, 0x5eed01u, 0.35 }; }
inline BurstSpec PopBurst()   { return { 26, 200.0, 420.0, 0.95, 300.0, 80.0, 3.5, 8.0, 0x5eed02u, 0.30 }; }

// ---- ワードマーク（1 文字ずつ弾んで登場 / ポンで波打つ）
struct LetterAnim { double dy = 0; double alpha = 0; double sy = 1; double sx = 1; };

inline LetterAnim LetterIntro(int i, double t)
{
    LetterAnim a;
    const double start = 0.20 + 0.035 * i;
    const double u = t - start;
    if (u <= 0.0) { a.dy = 26.0; return a; }
    const double p = SpringStep(u, 34.0, 0.52);       // やや硬めのスプリング。約 6% 行き過ぎ
    a.dy = 26.0 * (1.0 - p);                          // 下から来て、少し行き過ぎて着地
    a.alpha = Clamp01(u / 0.10);
    const double v = SpringStepVelocity(u, 34.0, 0.52) / 30.0;   // おおむね -0.3..1
    a.sy = 1.0 + 0.10 * std::max(-1.0, std::min(1.0, v));
    a.sx = 1.0 - 0.06 * std::max(-1.0, std::min(1.0, v));
    return a;
}
inline LetterAnim LetterWave(int i, double sincePop)
{
    LetterAnim a; a.alpha = 1.0;
    const double u = sincePop - 0.028 * i;
    if (u <= 0.0) return a;
    const SpringState s = SpringFree(u, 30.0, 0.34, 0.0, -190.0);   // 上へ跳ねて着地
    a.dy = s.x;
    return a;
}

// 情報（サブライン/Tips/最近）の登場: delay 秒後から 0.4 秒で下から滑り出る。
struct Reveal { double alpha = 0; double dy = 0; };
inline Reveal RevealAt(double t, double delay, double dur = 0.42, double slide = 10.0)
{
    Reveal r;
    const double u = Clamp01((t - delay) / dur);
    const double e = EaseOutCubic(u);
    r.alpha = e;
    r.dy = slide * (1.0 - e);
    return r;
}

// ---- タイムライン本体
class Timeline
{
public:
    explicit Timeline(TimingParams p = TimingParams()) : p_(p) {}

    const TimingParams& params() const { return p_; }

    void Reset() { lastT_ = -1.0; shown_ = 0.0; readyStart_ = -1.0; readyP0_ = 0.0; wantIssued_ = false; }

    // 1 フレームぶん進める。
    //   t          : 開始からの経過（単調増加）
    //   target     : 実進捗（0..1。まだ 1 には届かない値）
    //   finishAt   : 終了要求の時刻（<0 = まだ）。要求時刻はフレーム時刻に量子化されない
    //   mainShownAt: メイン窓を出した時刻（<0 = まだ）
    //   idleHop    : 「ときどきロゴが小さく跳ねる」演出を有効にするか（既定 true）
    SplashFrame Step(double t, double target, double finishAt, double mainShownAt, bool idleHop = true)
    {
        SplashFrame f;
        f.t = t;
        const double dt = (lastT_ < 0.0) ? 0.0 : std::max(0.0, t - lastT_);
        lastT_ = t;

        // --- ready の開始時刻を確定（intro が終わるまで待つ）
        if (readyStart_ < 0.0 && finishAt >= 0.0)
        {
            const double rs = std::max(finishAt, p_.introDur);
            if (t >= rs)
            {
                readyStart_ = rs;
                // ready 開始時点の表示進捗を控える（リングはここから 100% へ寄る）
                readyP0_ = std::min(shown_, p_.progressCap);
            }
        }
        const bool inReady = readyStart_ >= 0.0;

        // --- 表示進捗（loading 中: 目標へ指数接近・単調増加・上限 cap）
        if (!inReady)
        {
            const double goal = std::min(Clamp01(target), p_.progressCap);
            if (goal > shown_)
                shown_ += (goal - shown_) * (1.0 - std::exp(-p_.progressRate * dt));
            if (shown_ > p_.progressCap) shown_ = p_.progressCap;
        }

        // --- フェーズ
        double transReady = -1.0, transStart = -1.0;
        if (inReady)
        {
            transReady = readyStart_ + p_.ReadyDur();
            if (mainShownAt >= 0.0)                       transStart = std::max(transReady, mainShownAt);
            else if (t >= transReady + p_.mainShowTimeout) transStart = transReady + p_.mainShowTimeout;
        }
        if (!inReady)                                     f.phase = (t < p_.introDur) ? Phase::Intro : Phase::Loading;
        else if (transStart < 0.0 || t < transStart)      f.phase = Phase::Ready;
        else if (t < transStart + p_.transitionDur)       f.phase = Phase::Transition;
        else                                              f.phase = Phase::Done;
        f.finished = (f.phase == Phase::Done);
        if (inReady && t >= transReady && mainShownAt < 0.0 && !wantIssued_) { f.wantMainShow = true; wantIssued_ = true; }

        if (inReady) f.sinceReady = t - readyStart_;
        const double popT = inReady ? readyStart_ + p_.PopAfterReady() : -1.0;
        if (inReady && t >= popT) f.sincePop = t - popT;
        if (transStart >= 0.0 && t >= transStart) f.sinceTransition = t - transStart;

        // --- 進捗の見せ方
        if (!inReady)
        {
            f.progress = shown_;
        }
        else
        {
            const double u = f.sinceReady / p_.catchUpDur;
            f.progress = Lerp(readyP0_, 1.0, EaseOutCubic(u));
            if (u >= 1.0) f.progress = 1.0;
        }
        // 100% は「リングが完成した（catch-up が終わった）」後だけ。イージングの尾で 99.95% を超えても 99 で止める。
        f.percent = (inReady && f.sinceReady >= p_.catchUpDur) ? 100
                  : std::min(99, static_cast<int>(std::floor(f.progress * 100.0)));

        // --- 登場（カード全体）
        double zoom = Lerp(0.965, 1.0, EaseOutExpo(t / 0.28));
        double alpha = EaseOutCubic(t / 0.16);
        if (f.sinceTransition >= 0.0)
        {
            const double u = f.sinceTransition / p_.transitionDur;
            zoom = 1.0 + 0.09 * EaseOutCubic(u);
            alpha = 1.0 - EaseInQuad(u);
        }
        f.zoom = zoom;
        f.windowAlpha = Clamp01(alpha);
        if (f.phase == Phase::Done) f.windowAlpha = 0.0;

        // --- ロゴ: イントロのポンと登場（スプリング + スクワッシュ&ストレッチ）
        {
            const double u = t - kIntroLogoStart;
            double sc = 0.0, vn = 0.0;
            if (u > 0.0)
            {
                sc = SpringStep(u, 22.0, 0.50);
                vn = SpringStepVelocity(u, 22.0, 0.50) / 14.0;
                vn = std::max(-1.2, std::min(1.2, vn));
            }
            f.logoScaleX = sc * (1.0 - 0.15 * vn);
            f.logoScaleY = sc * (1.0 + 0.21 * vn);
            f.logoDy = (u > 0.0) ? -18.0 * (1.0 - sc) : -18.0;
        }
        // --- ロゴ: ときどきの小さな跳ね（loading 中。「生きてる」感じ）
        if (idleHop && (f.phase == Phase::Loading) )
        {
            const double period = 2.7;
            const double t0 = p_.introDur + 0.9;
            const double k = (t - t0) / period;
            if (k >= 0.0)
            {
                const double u = (k - std::floor(k)) * period;   // 周期内の経過秒
                const double hopDur = 0.5;
                if (u < hopDur)
                {
                    const double s = u / hopDur;
                    const double arc = std::sin(kPi * s);
                    f.logoDy += -7.0 * arc;
                    const double land = (s > 0.80) ? std::sin(kPi * (s - 0.80) / 0.20) : 0.0;   // 着地のつぶれ
                    f.logoScaleY *= 1.0 + 0.05 * arc - 0.07 * land;
                    f.logoScaleX *= 1.0 - 0.03 * arc + 0.05 * land;
                    const int n = static_cast<int>(std::floor(k));
                    f.logoRotDeg += ((n & 1) ? -1.0 : 1.0) * 2.4 * std::sin(2.0 * kPi * s) * arc;
                }
            }
        }
        // --- ロゴ: ready の予備動作（つぶれ）→ ポン（伸び → 行き過ぎて戻る）
        if (inReady)
        {
            const double preLen = 0.10;
            const double tp = f.sinceReady - (p_.PopAfterReady() - preLen);   // 予備動作の経過
            if (tp >= 0.0 && f.sincePop < 0.0)
            {
                const double s = EaseInOutCubic(tp / preLen);
                f.logoScaleY *= 1.0 - 0.09 * s;
                f.logoScaleX *= 1.0 + 0.06 * s;
                f.logoDy += 2.5 * s;
            }
            if (f.sincePop >= 0.0)
            {
                // つぶれた状態(-0.09)から、上向きの速度で伸びて行き過ぎ、減衰して戻る
                const SpringState s = SpringFree(f.sincePop, 26.0, 0.30, -0.09, 7.4);
                f.logoScaleY *= 1.0 + s.x;
                f.logoScaleX *= 1.0 - 0.62 * s.x;
                const double bump = 1.0 + 0.35 * s.x;
                f.logoScaleX *= bump; f.logoScaleY *= bump;
                f.logoDy += -12.0 * std::max(0.0, s.x);
            }
        }

        // --- 光（呼吸）と閃光
        {
            const double breathe = 0.5 + 0.5 * std::sin(2.0 * kPi * (t - 0.4) / 2.4);
            const double intro = EaseOutCubic((t - 0.15) / 0.5);
            f.glow = intro * (0.30 + 0.16 * breathe);
            if (f.sincePop >= 0.0)
            {
                f.glow = std::min(1.0, f.glow + 0.75 * std::exp(-f.sincePop * 3.4));
                f.flash = std::exp(-f.sincePop * 11.0);
                f.ringFlash = std::exp(-f.sincePop * 5.0);
            }
            else if (inReady)
            {
                // リング完成に向けてじわっと明るく
                f.glow = std::min(1.0, f.glow + 0.20 * EaseInQuad(f.sinceReady / p_.PopAfterReady()));
            }
        }

        // --- リング: トラックの描き込み / 周回ハイライト / 衝撃波
        f.ringTrack = EaseOutExpo((t - kRingTrackStart) / 0.55);
        if (inReady)
        {
            const double lapStart = p_.catchUpDur;
            const double u = (f.sinceReady - lapStart) / p_.lapDur;
            if (u >= 0.0 && u <= 1.0)
            {
                f.lapActive = true;
                f.lapAngle = EaseInOutCubic(u);
                f.lapAlpha = std::sin(kPi * std::min(1.0, u * 1.0)) * 0.5 + 0.5 * (u < 0.5 ? EaseOutCubic(u * 2.0) : 1.0);
                f.lapAlpha = Clamp01(f.lapAlpha);
            }
            if (f.sincePop >= 0.0 && f.sincePop < 0.5) f.shock = f.sincePop / 0.5;
        }

        // --- 火花のバースト時計
        if (t >= kIntroBurstAt) f.introBurstAge = t - kIntroBurstAt;
        if (f.sincePop >= 0.0)  f.popBurstAge   = f.sincePop;
        return f;
    }

    // 内部状態の参照（テスト/音の同期用）
    double ReadyStart() const { return readyStart_; }
    double PopTime() const { return readyStart_ < 0.0 ? -1.0 : readyStart_ + p_.PopAfterReady(); }
    double DisplayedProgress() const { return shown_; }

private:
    TimingParams p_;
    double lastT_ = -1.0;
    double shown_ = 0.0;
    double readyStart_ = -1.0;
    double readyP0_ = 0.0;
    bool   wantIssued_ = false;
};

// ---------------------------------------------------------------- 文言のクロスフェード

struct TextSwapState
{
    std::wstring cur;
    std::wstring prev;
    double u = 1.0;        // 0..1（1 = 入替完了）
};

class TextSwapper
{
public:
    explicit TextSwapper(double dur = 0.25) : dur_(dur) {}
    // 文言が変わったときだけ時刻 t を記録する（同じ文言なら何もしない）。
    void Set(const std::wstring& s, double t)
    {
        if (s == cur_) return;
        prev_ = cur_;
        cur_ = s;
        changeT_ = t;
        first_ = prev_.empty();
    }
    TextSwapState Query(double t) const
    {
        TextSwapState r;
        r.cur = cur_;
        r.prev = prev_;
        r.u = first_ ? 1.0 : Clamp01((t - changeT_) / dur_);
        if (r.u >= 1.0) r.prev.clear();
        return r;
    }
private:
    double dur_;
    std::wstring cur_, prev_;
    double changeT_ = -1e9;
    bool first_ = true;
};

// ---------------------------------------------------------------- Tips のローテーション

struct TipState
{
    int    index = 0;
    double alpha = 0;      // 0..1（フェードイン → 保持 → フェードアウト）
    double dy = 0;         // 軽い縦スライド（入: 下から / 出: 上へ）
};

class TipRotator
{
public:
    // count: Tips の総数 / startSeed: 開始位置（乱数の種。0..count-1 に丸める）/ period: 切替間隔 / fade: フェード秒
    TipRotator(int count, uint32_t startSeed, double period = 3.5, double fade = 0.30)
        : count_(count > 0 ? count : 1), period_(period), fade_(fade),
          start_(static_cast<int>(Hash32(startSeed) % static_cast<uint32_t>(count > 0 ? count : 1))) {}

    TipState Query(double t) const
    {
        TipState s;
        if (t < 0.0) t = 0.0;
        const int slot = static_cast<int>(std::floor(t / period_));
        const double u = t - slot * period_;
        s.index = (start_ + slot) % count_;
        const double in  = EaseOutCubic(u / fade_);
        const double out = EaseOutCubic((period_ - u) / fade_);
        s.alpha = std::min(in, out);
        s.dy = (u < fade_) ? 6.0 * (1.0 - in) : (period_ - u < fade_ ? -6.0 * (1.0 - out) : 0.0);
        return s;
    }
    int start() const { return start_; }
    double period() const { return period_; }
private:
    int count_;
    double period_, fade_;
    int start_;
};

} // namespace dx12e::splash
