#pragma once
// ---------------------------------------------------------------------------
// AnimGraphAsset — `.animfsm`（アニメーションステートマシンの JSON アセット）の
// データ型と、遷移判定の純関数。
//
// このエンジンは「ゲームの中身は全部データ（シーン JSON + .lua）」という設計思想なので、
// グラフの構造は JSON に置き、グラフエディタ UI は作らない（人間はテキストで、
// Claude Code もテキストで同じものを編集できる）。先例は `.uianim` と `.spranim`。
//
// Lua からはパラメータだけを書く（e:setAnimFloat("speed", v)）。
// FSM の構造を Lua で組む API は意図的に作らない（真実の源を 1 つに保つため）。
//
// 依存は STL + core/Types.h + animation/AnimEvent.h だけ（nlohmann は .cpp のみ）。
// tests/ から直接ビルドして検証できる。
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "core/Types.h"
#include "animation/AnimEvent.h"

namespace dx12e
{

// ---------------------------------------------------------------------------
// パラメータ
// ---------------------------------------------------------------------------
enum class AnimParamType : uint8_t { Float = 0, Bool = 1, Trigger = 2 };

struct AnimParamDef
{
    std::string   name;
    AnimParamType type = AnimParamType::Float;
    f32           defFloat = 0.0f;
    bool          defBool  = false;
};

// 実行時のパラメータ値。Trigger は「立ったら次に条件を満たした遷移が消費する」bool。
struct AnimParamValue
{
    AnimParamType type = AnimParamType::Float;
    f32           f    = 0.0f;
    bool          b    = false;
};

using AnimParamMap = std::unordered_map<std::string, AnimParamValue>;

// ---------------------------------------------------------------------------
// 遷移条件
// ---------------------------------------------------------------------------
enum class AnimCondOp : uint8_t
{
    Gt = 0, Lt, Ge, Le, Eq, Ne,   // float 用
    IsTrue, IsFalse,              // bool 用
    Trigger                       // trigger 用（成立したら消費される）
};

struct AnimCondition
{
    std::string param;
    AnimCondOp  op    = AnimCondOp::Gt;
    f32         value = 0.0f;   // float 系の比較値
};

// ---------------------------------------------------------------------------
// ブレンドツリー（Step 4。Step 3 の時点では enabled=false のまま）
// ---------------------------------------------------------------------------
struct AnimBlendSample1D
{
    f32         value = 0.0f;    // パラメータ軸上の位置（例: 移動速度 m/s）
    std::string clip;
    f32         speed = 1.0f;    // このサンプル固有の再生速度倍率
    i32         _clipIndex = -1; // ロード時に解決（クリップ名 → index）
};

enum class AnimBlendTreeType : uint8_t { None = 0, OneD = 1 };

struct AnimBlendTree
{
    AnimBlendTreeType type = AnimBlendTreeType::None;
    std::string       param;                 // 1D の入力パラメータ名
    std::vector<AnimBlendSample1D> samples;  // value 昇順（ロード時にソートする）
    bool syncPhase  = true;   // 全クリップを正規化時間で位相同期する
    bool speedMatch = false;  // param の値に合わせて再生レートを補正する（足滑り対策）
};

// ---------------------------------------------------------------------------
// ステート / 遷移 / レイヤー
// ---------------------------------------------------------------------------
struct AnimStateDef
{
    std::string   name;
    std::string   clip;         // 単一クリップ（blendTree があればそちらが優先）
    bool          loop  = true;
    f32           speed = 1.0f;
    AnimBlendTree blendTree;
    i32           _clipIndex = -1;   // ロード時に解決
};

struct AnimTransitionDef
{
    std::string from;              // "*" = Any State
    std::string to;
    f32         duration = 0.2f;   // クロスフェード秒
    std::vector<AnimCondition> conditions;
    bool        interruptible = true;  // この遷移の進行中に別の遷移へ割り込めるか
    bool        hasExitTime   = false;
    f32         exitTime      = 0.0f;  // 現ステートの正規化時間がこれ以上になってから

    i32 _from = -1;   // -1 かつ fromAny=false は「解決失敗」
    i32 _to   = -1;
    bool _fromAny = false;
};

enum class AnimLayerBlend : uint8_t { Override = 0, Additive = 1 };

struct AnimMaskDef
{
    std::vector<std::string> include;      // ボーン名
    bool includeChildren = true;           // 子孫も含める
    f32  weight = 1.0f;
};

struct AnimLayerDef
{
    std::string    name = "Base";
    f32            weight = 1.0f;
    AnimLayerBlend blend  = AnimLayerBlend::Override;
    bool           hasMask = false;
    AnimMaskDef    mask;
    std::string    referenceClip;   // additive の参照ポーズ（空ならバインドポーズ）
    i32            _referenceClipIndex = -1;
    std::string    defaultState;
    std::vector<AnimStateDef>      states;
    std::vector<AnimTransitionDef> transitions;
};

// ---------------------------------------------------------------------------
// アセット本体
// ---------------------------------------------------------------------------
struct AnimGraphClipEvents
{
    std::string            clip;
    std::vector<AnimEvent> events;
};

struct AnimGraphExtraClip
{
    std::string path;   // assets 相対（例 "models/human/sneakWalk.gltf"）
    std::string name;   // 空ならファイル側のクリップ名をそのまま使う
};

struct AnimGraphAsset
{
    i32 version = 1;
    std::vector<AnimParamDef>       parameters;
    std::vector<AnimLayerDef>       layers;
    std::vector<AnimGraphClipEvents> clipEvents;
    std::vector<AnimGraphExtraClip>  extraClips;
};

// ---------------------------------------------------------------------------
// 純関数（テスト対象）
//
// 決定 4（GPU/ECS/Jolt/JSON に依存しない純関数）に従い、遷移判定はすべて
// ヘッダ inline にしてある。tests/ は .cpp を 1 本もビルドせずに検証できる。
// ---------------------------------------------------------------------------

// レイヤー内のステート名 → 添字。見つからなければ -1。
inline i32 FindAnimState(const AnimLayerDef& layer, const std::string& name)
{
    for (size_t i = 0; i < layer.states.size(); ++i)
        if (layer.states[i].name == name) return static_cast<i32>(i);
    return -1;
}

// パラメータ 1 件の条件判定。パラメータが存在しなければ false（黙って不成立）。
inline bool EvalCondition(const AnimCondition& c, const AnimParamMap& params)
{
    auto it = params.find(c.param);
    if (it == params.end()) return false;
    const AnimParamValue& v = it->second;

    switch (c.op)
    {
    case AnimCondOp::Gt:      return v.f >  c.value;
    case AnimCondOp::Lt:      return v.f <  c.value;
    case AnimCondOp::Ge:      return v.f >= c.value;
    case AnimCondOp::Le:      return v.f <= c.value;
    case AnimCondOp::Eq:      return v.f == c.value;
    case AnimCondOp::Ne:      return v.f != c.value;
    case AnimCondOp::IsTrue:  return v.b;
    case AnimCondOp::IsFalse: return !v.b;
    case AnimCondOp::Trigger: return v.b;   // 立っているトリガ。発火したら消費する
    }
    return false;
}

// 全条件の AND。条件が 0 件なら true（＝exitTime だけで遷移する形）。
inline bool EvalConditions(const std::vector<AnimCondition>& conds, const AnimParamMap& params)
{
    for (const AnimCondition& c : conds)
        if (!EvalCondition(c, params)) return false;
    return true;
}

// 発火できる遷移の添字を返す（-1 = なし）。
//   curState             現在のステート添字（-1 なら Any State 遷移のみ対象）
//   normalizedTime       現ステートの正規化時間（0..1）
//   inTransition         いま遷移中か
//   currentInterruptible いま進行中の遷移が割り込み可能か（inTransition のときだけ見る）
// 評価順は**宣言順**（最初に成立したものが勝つ）。Any State（from="*"）は
// 自分自身へは遷移しない。「被弾やジャンプのトリガを必ず勝たせたい」場合は、
// .animfsm の transitions 配列でその遷移を**先に**書くこと（優先度はデータ側の責任）。
inline i32 PickTransition(const AnimLayerDef& layer, i32 curState, f32 normalizedTime,
                          bool inTransition, bool currentInterruptible,
                          const AnimParamMap& params)
{
    // 割り込み不可の遷移が進行中なら何も選ばない
    if (inTransition && !currentInterruptible) return -1;

    for (size_t i = 0; i < layer.transitions.size(); ++i)
    {
        const AnimTransitionDef& tr = layer.transitions[i];
        if (tr._to < 0) continue;                       // 解決できなかった遷移は無視

        if (tr._fromAny)
        {
            if (tr._to == curState) continue;            // Any State は自分自身へは行かない
        }
        else
        {
            if (tr._from < 0 || tr._from != curState) continue;
        }

        // exitTime: 現ステートの正規化時間がここに達するまで遷移しない
        if (tr.hasExitTime && normalizedTime < tr.exitTime) continue;

        if (!EvalConditions(tr.conditions, params)) continue;

        return static_cast<i32>(i);
    }
    return -1;
}

// 遷移が参照したトリガを消費する（false に戻す）。遷移が実際に発火したときだけ呼ぶ。
inline void ConsumeTriggers(const AnimTransitionDef& tr, AnimParamMap& params)
{
    for (const AnimCondition& c : tr.conditions)
    {
        if (c.op != AnimCondOp::Trigger) continue;
        auto it = params.find(c.param);
        if (it != params.end()) it->second.b = false;
    }
}

// パラメータ定義から実行時マップの初期値を作る。
inline void InitAnimParams(const AnimGraphAsset& asset, AnimParamMap& out)
{
    out.clear();
    for (const AnimParamDef& p : asset.parameters)
    {
        AnimParamValue v;
        v.type = p.type;
        v.f    = p.defFloat;
        // トリガの初期値は必ず false（立ったまま始まると 1 フレーム目に暴発する）
        v.b    = (p.type == AnimParamType::Trigger) ? false : p.defBool;
        out[p.name] = v;
    }
}

// ---------------------------------------------------------------------------
// JSON IO（AnimGraphAsset.cpp）
// ---------------------------------------------------------------------------

// JSON バイト列 → AnimGraphAsset。パース失敗なら false（err にメッセージ）。
// 成功時はブレンドツリーのサンプルが value 昇順にソートされている。
bool ParseAnimGraphAsset(const std::vector<uint8_t>& jsonBytes, AnimGraphAsset& out, std::string& err);

// AnimGraphAsset → JSON 文字列（整形あり）。MCP の describe_anim_graph が使う。
std::string SerializeAnimGraphAsset(const AnimGraphAsset& asset);

} // namespace dx12e
