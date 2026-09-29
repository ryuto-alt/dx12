#pragma once

// 起動音の再生（winmm waveOut の薄いラッパー）。ミキシングの計算は core/SplashMixer.h の Mixer が持つ。
// このクラスは「音源を探して読む → 専用スレッドで小さなチャンクを供給する → 止める」だけ。
//
//   ・音源: <soundDir>/splash_d.wav + splash_d.json（tools/prep_splash_sound.py の出力。コミットしない）。
//           無ければ埋め込みの案 A（SplashSoundData.h。riser 無しの単発音）へ自動で縮退する。
//   ・デバイスが無い / 開けない / 音源が読めない: 黙って何もしない（診断ログのみ。窓の演出は続く）。
//   ・全 API は未開始・多重呼び出しに安全。呼び出しスレッドは問わない。
//
// ★検証で実デバイスを鳴らさないこと: 単体テストとプレビューは Mixer をオフラインで回して WAV に書き出す
//   （tests/splash_sync_dump.cpp）。ここは実機のスプラッシュからしか呼ばれない。

#include <string>

namespace dx12e
{

class SplashSound
{
public:
    // 音源を読み、riser の再生を始める（適応同期）。
    //   predictedFinishSec : 初期化が終わる（Finish が呼ばれる）と予測する時刻（Show からの秒。前回までの EMA）
    //   popAfterReadySec   : 終了要求から視覚の「ポン」までの秒（SplashTimeline の catchUp + lap）
    // riser は「TriggerHit を出すと予測する時刻」に区間の終端へ届くよう、途中から（または遅れて）鳴り始める。
    // 成功したら true。失敗（デバイス無し等）は false で、以降の呼び出しは no-op。
    static bool Start(const std::wstring& soundDir, double predictedFinishSec, double popAfterReadySec);

    // 終了要求（ready の開始）から TriggerHit を出すまでの秒。視覚の「ポン」が音の頂点と揃うよう逆算した値。
    // Start 成功後に有効（それ以前は 0）。
    static double TriggerOffsetSec();

    // 頂点（hit）へ移る合図。次のチャンクから効く。
    static void TriggerHit();

    // 中止（短くフェードアウトして止める）。
    static void Abort();

    // 再生スレッドが動いているか。
    static bool Active();

    // 使った音源の名前（診断用）。
    static const char* SourceName();
};

} // namespace dx12e
