// ecs/ParticleOneShot.h（W1-A12 / BUG-12）の検査。GPU 不要。
//  ・エディタでもワンショットが duration で止まり、再トリガーで再開する
//  ・ループはエディタで常時プレビュー、Play では _active に従う（従来どおり）
//  ・Application.cpp / InspectorPanel.cpp に配線が残っている
#include "ecs/ParticleOneShot.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace
{
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// dt 刻みで秒数ぶん回し、放出したフレーム数を返す。
int Run(dx12e::ParticleLayer& l, bool playing, float seconds, float dt)
{
    int n = 0;
    for (float t = 0; t < seconds - 1e-6f; t += dt)
        if (dx12e::ParticleOneShot::Advance(l, playing, dt)) ++n;
    return n;
}
std::string Slurp(const fs::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
}

int main()
{
    // ワンショットはエディタで止まる
    {
        dx12e::ParticleLayer l; l.looping = false; l.duration = 1.0f;
        const int n = Run(l, false, 3.0f, 0.1f);
        CHECK(n >= 10 && n <= 11);   // 約 1 秒ぶんだけ放出
        CHECK(!l._active);
        CHECK(!dx12e::ParticleOneShot::Advance(l, false, 0.1f));
        // 再トリガーで再開 → また止まる
        dx12e::ParticleOneShot::Retrigger(l);
        CHECK(l._active && l._age == 0.0f);
        const int m = Run(l, false, 3.0f, 0.1f);
        CHECK(m >= 10 && m <= 11);
        CHECK(!l._active);
    }
    // Play でも同じ
    {
        dx12e::ParticleLayer l; l.looping = false; l.duration = 0.5f;
        CHECK(Run(l, true, 2.0f, 0.1f) <= 6);
        CHECK(!l._active);
    }
    // ループ: エディタは常時、Play は _active
    {
        dx12e::ParticleLayer l; l.looping = true;
        { const int r = Run(l, false, 5.0f, 0.1f); CHECK(r >= 49 && r <= 51 && l._active); }
        l._active = false;
        CHECK(dx12e::ParticleOneShot::Advance(l, false, 0.1f));    // エディタは止まらない
        CHECK(!dx12e::ParticleOneShot::Advance(l, true, 0.1f));    // Play は StopEffect に従う
    }
    // エミッタ全体の再トリガー
    {
        dx12e::ParticleEmitter e; e.layers.resize(2);
        for (auto& l : e.layers) { l.looping = false; l.duration = 0.2f; Run(l, false, 1.0f, 0.1f); }
        dx12e::ParticleOneShot::Retrigger(e);
        for (auto& l : e.layers) CHECK(l._active && l._age == 0.0f && l._emitAccum == 0.0f);
    }
    // 配線
    {
        const fs::path root = fs::path(UI_LINT_ROOT);
        const std::string app = Slurp(root / "src/core/Application.cpp");
        const std::string ins = Slurp(root / "src/editor/panels/InspectorPanel.cpp");
        CHECK(app.find("dx12e::ParticleOneShot::Advance(pe, pedPlaying, dt)") != std::string::npos);
        CHECK(ins.find("ParticleOneShot::Retrigger(emitter)") != std::string::npos);
    }
    std::printf(g_fail ? "FAILED %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
