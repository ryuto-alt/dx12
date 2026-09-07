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
// ★形（絵）はここには無い。実装は shaders/post/TransitionCurtain.hlsli ただ 1 箇所で、
//   エディタのサムネイルもプレビュー窓もその関数を GPU で走らせて描く
//   （src/editor/panels/TransitionPreviewPanel.cpp）。C++ 側に近似実装は持たない。
//
// ★既定秒は 1.0 秒を基準にしている。0.6 秒台は「切り替わった」と認識する前に終わってしまい、
//   演出の形（星・渦・ガラス割れなど）がまったく読み取れなかった。長さは UI のスライダーで
//   あとから詰められるので、初期値は**演出が読み取れる長さ**に寄せてある。
//
// ImGui にも GPU にも依存しない（TransitionType というただの enum しか触らない）＝
// ヘッドレスからも MCP からも同じ表を引ける。

#include <cstring>

#include "renderer/SceneTransition.h"

namespace dx12e
{

// タイルの並びを章立てするためのグループ。UI はこの順・この区切りで見出しを出す。
enum class TransitionGroup
{
    Basic = 0,   // 基本（どのジャンルでも外さない）
    Wipe,        // ワイプ・スライド系（形が動いて拭き取る）
    Iris,        // アイリス系（形が閉じる）
    Effect,      // 演出系（ジャンルの色が付く）
    Count
};

inline const char* TransitionGroupLabel(TransitionGroup g)
{
    switch (g)
    {
    case TransitionGroup::Basic:  return "基本";
    case TransitionGroup::Wipe:   return "ワイプ・スライド";
    case TransitionGroup::Iris:   return "アイリス（形が閉じる）";
    case TransitionGroup::Effect: return "演出（ジャンルの色が付く）";
    default:                      return "";
    }
}

struct TransitionPreset
{
    const char*     id;        // 保存 / Lua / MCP 用の安定 ID（英小文字）
    const char*     label;     // エディタのタイル表示（日本語）
    const char*     tip;       // ひとことの説明（どんなゲームで使うか）
    TransitionType  type;
    float           duration;  // 「閉じる → 開く」の合計秒
    TransitionGroup group;     // タイルの並びの章
};

// ★並び順がそのままエディタのタイルの並び順になる（グループごとにまとまっていること）。
inline const TransitionPreset kTransitionPresets[] = {
    // ---- 基本 ----
    {"fade", "暗転", "画面が黒く沈んで戻る。どのジャンルでも外さない既定",
     TransitionType::FadeBlack, 1.0f, TransitionGroup::Basic},

    {"flash", "ホワイトアウト", "白く飛ばす。閃光・回想の入り・場面の飛躍に",
     TransitionType::FadeWhite, 1.0f, TransitionGroup::Basic},

    {"dissolve", "ディゾルブ", "ノイズの粒がまばらに沈む。霧・砂・記憶が薄れる場面に",
     TransitionType::Dissolve, 1.0f, TransitionGroup::Basic},

    {"mosaic", "モザイク", "四角いブロックがランダムに埋まる。レトロ・ドット絵の定番",
     TransitionType::Mosaic, 1.0f, TransitionGroup::Basic},

    // ---- ワイプ・スライド ----
    {"wipe", "横ワイプ", "左から右へ拭き取る。軽快でテンポの速いゲーム向け",
     TransitionType::Wipe, 1.0f, TransitionGroup::Wipe},

    {"wipe_v", "縦ワイプ", "上から下へ拭き取る。章立て・見出しの切り替えに",
     TransitionType::WipeVertical, 1.0f, TransitionGroup::Wipe},

    {"wipe_diag", "斜めワイプ", "左上から右下へ斜めに拭く。スポーツ・レース系の勢いに",
     TransitionType::WipeDiagonal, 1.0f, TransitionGroup::Wipe},

    {"curtain", "カーテン", "左右の幕が中央で合わさる。舞台・ステージ制・幕間に",
     TransitionType::Curtain, 1.0f, TransitionGroup::Wipe},

    {"slide", "スライド", "一枚板が右から入り、そのまま左へ抜ける。メニュー・章送りに",
     TransitionType::SlidePush, 1.0f, TransitionGroup::Wipe},

    {"blinds", "ブラインド", "横帯が同時に閉じる。機械的・SF・ハッキング演出に",
     TransitionType::Blinds, 1.0f, TransitionGroup::Wipe},

    {"blinds_v", "縦ブラインド", "縦帯が同時に閉じる。横スクロール・格ゲーの場面切替に",
     TransitionType::BlindsVertical, 1.0f, TransitionGroup::Wipe},

    {"clock", "時計ワイプ", "12時から時計回りに扇が回る。時間・ターン制のモチーフに",
     TransitionType::RadialClock, 1.2f, TransitionGroup::Wipe},

    {"spiral", "渦巻き", "中心から渦を巻いて飲み込む。ワープ・異空間への入り口に",
     TransitionType::Spiral, 1.2f, TransitionGroup::Wipe},

    // ---- アイリス ----
    {"iris", "アイリス", "四隅から円で閉じる。レトロ・アドベンチャーの定番",
     TransitionType::Circle, 1.0f, TransitionGroup::Iris},

    {"diamond", "菱形", "アイリスの角ばった版。カード・パズル系の硬い印象に",
     TransitionType::Diamond, 1.0f, TransitionGroup::Iris},

    {"star", "星アイリス", "5 芒星が閉じる。ステージクリア・ごほうびの場面に",
     TransitionType::Star, 1.0f, TransitionGroup::Iris},

    {"plus", "十字アイリス", "十字に閉じる。医療・回復・セーブポイントのモチーフに",
     TransitionType::Plus, 1.0f, TransitionGroup::Iris},

    {"heart", "ハートアイリス", "ハートが閉じる。恋愛・カジュアル・かわいい系に",
     TransitionType::Heart, 1.0f, TransitionGroup::Iris},

    // ---- 演出 ----
    {"melt", "メルト", "縦の列がばらばらの速さで垂れ落ちる。ホラー・レトロ FPS の融解",
     TransitionType::Melt, 1.3f, TransitionGroup::Effect},

    {"shatter", "ガラス割れ", "三角の破片が順に現れ、ひびが青白く走る。被弾・敗北・急転に",
     TransitionType::Shatter, 1.2f, TransitionGroup::Effect},

    {"glitch", "グリッチ", "走査帯がずれて色収差が出る。サイバー・電脳・システム異常に",
     TransitionType::Glitch, 1.0f, TransitionGroup::Effect},

    {"hex", "ハニカム", "六角形のセルが中心から広がる。SF・戦術・シールド展開の演出に",
     TransitionType::Hexagon, 1.1f, TransitionGroup::Effect},

    {"checker", "チェッカー", "市松のマスが 2 段階で埋まる。ボード・パズル・レトロ UI に",
     TransitionType::Checker, 1.1f, TransitionGroup::Effect},

    {"ripple", "波紋", "中心から波紋が広がり、縁が青く光る。水・魔法・詠唱の場面に",
     TransitionType::Ripple, 1.1f, TransitionGroup::Effect},

    {"flood", "水没", "下から水位が上がり、水面が波打つ。水中・沈没・潜行の入りに",
     TransitionType::Flood, 1.2f, TransitionGroup::Effect},

    {"burn", "フィルムバーン", "オレンジの焼け縁がフィルムを食う。回想・破棄・記録の終わりに",
     TransitionType::Burn, 1.2f, TransitionGroup::Effect},

    {"rush", "集中線", "白い集中線が中心へ収束しながら閉じる。アニメ風の必殺技・戦闘突入に",
     TransitionType::SpeedLines, 1.0f, TransitionGroup::Effect},

    {"seek", "シークバー早送り", "動画プレイヤー風。プレイヘッドが掃き、下にシークバーが伸びる",
     TransitionType::Seek, 1.2f, TransitionGroup::Effect},
};

inline constexpr int kTransitionPresetCount =
    static_cast<int>(sizeof(kTransitionPresets) / sizeof(kTransitionPresets[0]));

// 型を指定しない経路（Trigger の FadeToScene / Lua の fadeToScene）が使う既定秒。
// ★0.6 秒だと「何が起きたか」を認識する前に終わる。1.0 秒が下限の目安。
inline constexpr float kDefaultTransitionDuration = 1.0f;

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
