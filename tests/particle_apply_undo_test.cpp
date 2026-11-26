// パーティクルエディタの「適用」（W1-A15 / BUG-21 の暫定策）の検査。GPU 不要。
//  ・置き換えは 1 枚目に中身があるときだけ確認が要る（空・既定値のままは要らない）
//  ・適用 → Undo で元のレイヤーの値に戻る → Redo で適用後に進む
//  ・「レイヤーとして追加」→ Undo でレイヤーが 1 枚減る
//  ・旧 ParticleLayer のデータ形式（gpu / offset / shaderPath）は適用で触らない
//  ・パネルに確認の配線が残っている
#include "editor/VfxApplyLogic.h"
#include "editor/UndoCore.h"
#include "editor/UndoSystem.h"

#include <entt/entt.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace dx12e;
namespace fs = std::filesystem;

namespace
{
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
std::string Slurp(const fs::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

VfxAsset MakeAsset()
{
    VfxAsset a;
    a.name = "Campfire";
    a.kind = 2; a.rate = 77.0f; a.size = 1.5f; a.intensity = 9.0f; a.texturePath = "tex/smoke.png";
    return a;
}
}

int main()
{
    // 確認の要否
    {
        ParticleEmitter none;
        CHECK(!vfxapply::ReplaceNeedsConfirm(none));                 // レイヤー無し
        ParticleEmitter def; def.layers.emplace_back();
        CHECK(!vfxapply::ReplaceNeedsConfirm(def));                  // 既定値のまま
        ParticleEmitter used; used.layers.emplace_back(); used.layers[0].rate = 5.0f;
        CHECK(vfxapply::ReplaceNeedsConfirm(used));                  // 中身あり
    }

    // 適用 → Undo / Redo
    {
        entt::registry reg;
        const entt::entity e = reg.create();
        ParticleEmitter em;
        em.layers.emplace_back();
        em.layers[0].name = "Torch"; em.layers[0].kind = 1; em.layers[0].rate = 11.0f; em.layers[0].gpu = true;
        em.layers[0].offset = {1.0f, 2.0f, 3.0f}; em.layers[0].shaderPath = "my.hlsl";
        em.layers.emplace_back(); em.layers[1].name = "Second"; em.layers[1].rate = 3.0f;
        reg.emplace<ParticleEmitter>(e, em);
        const ParticleEmitter original = reg.get<ParticleEmitter>(e);
        CHECK(vfxapply::ReplaceNeedsConfirm(original));

        const VfxAsset asset = MakeAsset();
        UndoSystem undo;
        const ParticleEmitter before = reg.get<ParticleEmitter>(e);
        const ParticleEmitter after = vfxapply::BuildApplied(before, asset, "vfx/Campfire.json", false);
        // パネルが使う経路（置き換え = asNewLayer=false）を共通の ApplyWithUndo で通す
        CHECK(vfxapply::ApplyWithUndo(reg, e, undo, asset, "vfx/Campfire.json", false));
        CHECK(reg.get<ParticleEmitter>(e) == after);
        CHECK(undo.UndoDepth() == 1);

        const ParticleEmitter& cur = reg.get<ParticleEmitter>(e);
        CHECK(cur.layers.size() == 2);
        CHECK(cur.layers[0].rate == 77.0f && cur.layers[0].kind == 2 && cur.layers[0].size == 1.5f);
        CHECK(cur.layers[0].texturePath == "tex/smoke.png" && cur.layers[0].vfxPath == "vfx/Campfire.json");
        CHECK(cur.layers[0].name == "Torch");                        // 既存の名前は残す（従来どおり）
        CHECK(cur.layers[0].gpu == true && cur.layers[0].shaderPath == "my.hlsl" && cur.layers[0].offset.y == 2.0f);
        CHECK(cur.layers[1] == original.layers[1]);                  // 2 枚目は不変
        CHECK(before == original);                                   // BuildApplied は before を変えない

        undo.Undo();
        CHECK(reg.get<ParticleEmitter>(e) == original);              // 元のレイヤーの値に戻る
        CHECK(reg.get<ParticleEmitter>(e).layers[0].rate == 11.0f);
        undo.Redo();
        CHECK(reg.get<ParticleEmitter>(e) == after);
        undo.Undo();
        CHECK(reg.get<ParticleEmitter>(e) == original);
    }

    // 「レイヤーとして追加」→ Undo で 1 枚減る
    {
        entt::registry reg;
        const entt::entity e = reg.create();
        ParticleEmitter em; em.layers.emplace_back(); em.layers[0].rate = 4.0f;
        reg.emplace<ParticleEmitter>(e, em);
        UndoSystem undo;
        const ParticleEmitter before = reg.get<ParticleEmitter>(e);
        const ParticleEmitter after = vfxapply::BuildApplied(before, MakeAsset(), "", true);
        CHECK(after.layers.size() == 2 && after.layers[0] == before.layers[0]);
        CHECK(after.layers[1].name == "Campfire" && after.layers[1].rate == 77.0f);
        CHECK(vfxapply::ApplyWithUndo(reg, e, undo, MakeAsset(), "", true));
        CHECK(reg.get<ParticleEmitter>(e) == after);
        undo.Undo();
        CHECK(reg.get<ParticleEmitter>(e) == before);
        CHECK(reg.get<ParticleEmitter>(e).layers.size() == 1);
    }

    // 空の放出器へ置き換えても 1 枚できる
    {
        ParticleEmitter none;
        const ParticleEmitter after = vfxapply::BuildApplied(none, MakeAsset(), "", false);
        CHECK(after.layers.size() == 1 && after.layers[0].rate == 77.0f);
    }

    // 確認の要否（実入力の人は aiSessionEver でも確認する）
    {
        ParticleEmitter full; full.layers.emplace_back(); full.layers[0].rate = 11.0f;
        CHECK( vfxapply::ShouldConfirmReplace(full, false, false, false));
        CHECK( vfxapply::ShouldConfirmReplace(full, false, false, true));
        CHECK( vfxapply::ShouldConfirmReplace(full, false, true,  false));   // 実入力 + 過去に MCP 接続: 出す
        CHECK(!vfxapply::ShouldConfirmReplace(full, false, true,  true));    // MCP + 仮想入力: 出さない
        CHECK(!vfxapply::ShouldConfirmReplace(full, true,  false, false));   // 追加は常に出さない
        CHECK(!vfxapply::ShouldConfirmReplace(ParticleEmitter{}, false, false, false));
    }

    // 配線
    {
        const std::string root = UI_LINT_ROOT;
        const std::string panel = Slurp(fs::path(root) / "src/editor/panels/VfxEditorPanel.cpp");
        CHECK(panel.find("VfxReplaceLayer0Popup") != std::string::npos);
        CHECK(panel.find("RequestApply(reg, ctx, /*asNewLayer=*/false)") != std::string::npos);
        CHECK(panel.find("ShouldConfirmReplace") != std::string::npos);
        CHECK(panel.find("vinput::Enabled()") != std::string::npos);
        CHECK(panel.find("ApplyWithUndo") != std::string::npos);
    }

    if (g_fail == 0) std::puts("PASS particle_apply_undo");
    return g_fail == 0 ? 0 : 1;
}
