#pragma once

#include "core/Types.h"

namespace dx12e
{

// エディタ UI で使うアイコンの GPU ハンドル集合。
// 値は ImTextureID(=ImU64) として ImGui::Image / ImageButton に渡す。0 = 未読込（テキストにフォールバック）。
// Application が assets/editor/icons/*.png を読み込んで populate し、
// EditorContext::icons から各パネルが参照する。PNG は tools/gen_icons.ps1 で生成。
struct EditorUiIcons
{
    // ---- ランチャー / プロジェクト / Git ----
    u64 logo = 0, newProject = 0, openProject = 0, recent = 0,
        save = 0, git = 0, github = 0, commit = 0, push = 0;

    // ---- ツールバー ----
    u64 file = 0, play = 0, stop = 0, build = 0,
        gizmoMove = 0, gizmoRotate = 0, gizmoScale = 0,
        spaceWorld = 0, spaceLocal = 0, window = 0, uiMode = 0;

    // ---- エンティティ / コンポーネント種別（Hierarchy / Inspector） ----
    u64 entMesh = 0, entLight = 0, entCamera = 0, entAudio = 0,
        entScript = 0, entPhysics = 0, entCollider = 0, entUi = 0, entEmpty = 0;

    // ---- プロジェクトテンプレート（ランチャー） ----
    u64 tmplFps = 0, tmplTps = 0, tmpl2d = 0, tmplEmpty = 0;
};

} // namespace dx12e
