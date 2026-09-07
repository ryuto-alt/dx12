#pragma once

// ===== シーントランジションの「見た目プリセット」=====
//
// ポストプロセスのプリセット（editor/PostPresets.h）と同じ流儀のヘッダオンリー。
// 「型 + 秒数」の組み合わせに名前と説明を付けただけの表で、これ自体は何も描かない。
//
// ★エディタで選んだ 1 件が「プロジェクトの既定トランジション」になり、
//   settings.json（scene_transition_type / scene_transition_dur）へ保存される。
//   settings.json はゲームのビルドへ同梱される（ApplicationProject.cpp の BuildGame）ので、
//   エディタで選んだ演出がそのまま配布ゲームで出る。
//
// ★サムネイルは editor/TransitionSwatch.h。型ごとの絵は
//   shaders/post/Transition.hlsl の TransPS と一対一で対応させること。
//
// ImGui にも GPU にも依存しない（TransitionType というただの enum しか触らない）＝
// ヘッドレスからも MCP からも同じ表を引ける。

#include <cstring>

#include "renderer/SceneTransition.h"

namespace dx12e
{

struct TransitionPreset
{
    const char*    id;        // 保存 / Lua / MCP 用の安定 ID（英小文字）
    const char*    label;     // エディタのタイル表示（日本語）
    const char*    tip;       // ひとことの説明（どんなゲームで使うか）
    TransitionType type;
    float          duration;  // 「閉じる → 開く」の合計秒
};

// ★並び順がそのままエディタのタイルの並び順になる。
//   秒数は「その演出が一番それらしく見える長さ」。UI のスライダーで後から詰められる。
inline const TransitionPreset kTransitionPresets[] = {
    {"fade", "暗転", "画面が黒く沈んで戻る。どのジャンルでも外さない既定",
     TransitionType::FadeBlack, 0.6f},

    {"flash", "ホワイトアウト", "白く飛ばす。閃光・回想の入り・場面の飛躍に",
     TransitionType::FadeWhite, 0.5f},

    {"wipe", "横ワイプ", "左から右へ拭き取る。軽快でテンポの速いゲーム向け",
     TransitionType::Wipe, 0.5f},

    {"wipe_v", "縦ワイプ", "上から下へ拭き取る。章立て・見出しの切り替えに",
     TransitionType::WipeVertical, 0.5f},

    {"iris", "アイリス", "四隅から円で閉じる。レトロ・アドベンチャーの定番",
     TransitionType::Circle, 0.7f},

    {"diamond", "菱形", "アイリスの角ばった版。カード・パズル系の硬い印象に",
     TransitionType::Diamond, 0.6f},

    {"blinds", "ブラインド", "横帯が同時に閉じる。機械的・SF・ハッキング演出に",
     TransitionType::Blinds, 0.6f},

    {"clock", "時計ワイプ", "12時から時計回りに扇が回る。時間・ターン制のモチーフに",
     TransitionType::RadialClock, 0.8f},

    {"seek", "シークバー早送り", "動画プレイヤー風。プレイヘッドが掃き、下にシークバーが伸びる",
     TransitionType::Seek, 1.0f},
};

inline constexpr int kTransitionPresetCount =
    static_cast<int>(sizeof(kTransitionPresets) / sizeof(kTransitionPresets[0]));

// ID から引く。見つからなければ nullptr（呼び出し側が既定へ倒す）。
inline const TransitionPreset* FindTransitionPreset(const char* id)
{
    if (!id) return nullptr;
    for (const TransitionPreset& p : kTransitionPresets)
        if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}

// 型から引く。プリセットは 1 型 1 件なので、保存済みの型番号を UI の選択へ戻すのに使う。
// 見つからなければ -1（＝タイルをどれも選択表示にしない）。
inline int FindTransitionPresetIndexByType(TransitionType type)
{
    for (int i = 0; i < kTransitionPresetCount; ++i)
        if (kTransitionPresets[i].type == type) return i;
    return -1;
}

} // namespace dx12e
