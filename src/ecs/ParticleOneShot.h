#pragma once
// ワンショット放出器（ParticleLayer::looping=false）の寿命と再トリガー（W1-A12 / BUG-12）。
// Application.cpp の放出ループと InspectorPanel の「再生」ボタンが同じ規則を使う。
// データ形式（ParticleLayer のシリアライズ項目）は変えない。_active/_age/_emitAccum はランタイム専有。
#include "ecs/Components.h"

namespace dx12e::ParticleOneShot
{
// このフレームでレイヤーを駆動してよいか。dt 分だけ _age を進め、寿命に達したら _active を落とす。
//  ・ループ: Play 中は _active に従う（StopEffect で止まる）。エディタは常時プレビュー。
//  ・ワンショット: Play でもエディタでも _active に従い、duration 秒で止まる。
//    （寿命に達したフレームはこれまでどおり最後の放出を行う = true を返す）
inline bool Advance(ParticleLayer& pe, bool playing, float dt)
{
    if (pe.looping)
        return playing ? pe._active : true;
    if (!pe._active) return false;
    pe._age += dt;
    if (pe._age >= pe.duration) pe._active = false;
    return true;
}

// 先頭から再生し直す（Inspector の「再生」ボタン、Lua の PlayEffect と同じ状態）。
inline void Retrigger(ParticleLayer& pe)
{
    pe._active = true; pe._age = 0.0f; pe._emitAccum = 0.0f;
}
inline void Retrigger(ParticleEmitter& em)
{
    // ループのレイヤーは触らない（Play 中に StopEffect で止めたものを勝手に再開しない）。
    for (auto& l : em.layers) if (!l.looping) Retrigger(l);
}
}
