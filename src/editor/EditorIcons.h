#pragma once

#include "core/Types.h"


// ============================================================================
// アイコンフォント（Lucide, ISC License: assets/editor/fonts/LICENSE-lucide.txt）
// ----------------------------------------------------------------------------
// ImGuiManager が assets/editor/fonts/lucide.ttf を本文/太字/等幅の各フォントへマージするので、
// どの ImGui テキストにも文字として混ぜられる:
//     ImGui::MenuItem(ICON_SAVE "  保存", "Ctrl+S");
//     ImGui::Button(ICON_PLAY " 再生");
// ★リテラル連結（ICON_X " ラベル"）で使うこと。文字列の中で 16 進エスケープの直後へ英数字をつなげると
//   C++ の 16 進エスケープが貪欲に食って化ける（マクロ + 別リテラルなら安全）。
// ★グリフは 1em 角。ラベルとの間は半角スペース 1〜2 個。整列は ui::Icon / ui::IconButton が面倒を見る。
// コードポイントの追加は tools/ か lucide-static/font/codepoints.json から引く（U+E000〜 の私用領域）。
// ============================================================================
#define ICON_BLANK "\xe2\x80\x83"   // U+2003 EM SPACE: アイコンの無いメニュー項目の桁揃え用（1em 幅）
#define ICON_CHEVRON_DOWN   "\xee\x81\xad"   // U+E06D lucide:chevron-down
#define ICON_CHEVRON_RIGHT  "\xee\x81\xaf"   // U+E06F lucide:chevron-right
#define ICON_CHEVRON_UP     "\xee\x81\xb0"   // U+E070 lucide:chevron-up
#define ICON_ELLIPSIS       "\xee\x82\xb6"   // U+E0B6 lucide:ellipsis
#define ICON_SEARCH         "\xee\x85\x91"   // U+E151 lucide:search
#define ICON_CLOSE          "\xee\x86\xb2"   // U+E1B2 lucide:x
#define ICON_PLUS           "\xee\x84\xbd"   // U+E13D lucide:plus
#define ICON_MINUS          "\xee\x84\x9c"   // U+E11C lucide:minus
#define ICON_CHECK          "\xee\x81\xac"   // U+E06C lucide:check
#define ICON_EYE            "\xee\x82\xba"   // U+E0BA lucide:eye
#define ICON_EYE_OFF        "\xee\x82\xbb"   // U+E0BB lucide:eye-off
#define ICON_LOCK           "\xee\x84\x8b"   // U+E10B lucide:lock
#define ICON_UNLOCK         "\xee\x84\x8c"   // U+E10C lucide:lock-open
#define ICON_FOLD_ALL       "\xee\x88\xa8"   // U+E228 lucide:chevrons-down-up
#define ICON_UNFOLD_ALL     "\xee\x88\x91"   // U+E211 lucide:chevrons-up-down
#define ICON_ARROW_UP       "\xee\x81\x8a"   // U+E04A lucide:arrow-up
#define ICON_REFRESH        "\xee\x85\x85"   // U+E145 lucide:refresh-cw
#define ICON_FILTER         "\xee\x83\x9c"   // U+E0DC lucide:filter
#define ICON_LIST           "\xee\x84\x86"   // U+E106 lucide:list
#define ICON_MOVE           "\xee\x8b\xa5"   // U+E2E5 lucide:move-3d
#define ICON_ROTATE         "\xee\x8b\xaa"   // U+E2EA lucide:rotate-3d
#define ICON_SCALE          "\xee\x8b\xab"   // U+E2EB lucide:scale-3d
#define ICON_SPACE_WORLD    "\xee\x83\xa8"   // U+E0E8 lucide:globe
#define ICON_SPACE_LOCAL    "\xee\x81\xa1"   // U+E061 lucide:box
#define ICON_PLAY           "\xee\x84\xbc"   // U+E13C lucide:play
#define ICON_STOP           "\xee\x85\xa7"   // U+E167 lucide:square
#define ICON_PAUSE          "\xee\x84\xae"   // U+E12E lucide:pause
#define ICON_WINDOWS        "\xee\x83\xbf"   // U+E0FF lucide:layout-grid
#define ICON_UI_MODE        "\xee\x88\x87"   // U+E207 lucide:layout-template
#define ICON_VIEW_3D        "\xee\x94\xa4"   // U+E524 lucide:cuboid
#define ICON_VIEW_2D        "\xee\x94\x89"   // U+E509 lucide:square-dashed-mouse-pointer
#define ICON_POINTER        "\xee\x87\x83"   // U+E1C3 lucide:mouse-pointer-2
#define ICON_SAVE           "\xee\x85\x8d"   // U+E14D lucide:save
#define ICON_UNDO           "\xee\x8a\xa1"   // U+E2A1 lucide:undo-2
#define ICON_REDO           "\xee\x8a\xa0"   // U+E2A0 lucide:redo-2
#define ICON_COPY           "\xee\x82\x9e"   // U+E09E lucide:copy
#define ICON_PASTE          "\xee\x8f\xa8"   // U+E3E8 lucide:clipboard-paste
#define ICON_TRASH          "\xee\x86\x8e"   // U+E18E lucide:trash-2
#define ICON_FILE           "\xee\x83\x80"   // U+E0C0 lucide:file
#define ICON_FILE_TEXT      "\xee\x83\x8c"   // U+E0CC lucide:file-text
#define ICON_FILE_CODE      "\xee\x83\x83"   // U+E0C3 lucide:file-code
#define ICON_FILE_PLUS      "\xee\x83\x89"   // U+E0C9 lucide:file-plus
#define ICON_FOLDER         "\xee\x83\x97"   // U+E0D7 lucide:folder
#define ICON_FOLDER_OPEN    "\xee\x89\x87"   // U+E247 lucide:folder-open
#define ICON_FOLDER_PLUS    "\xee\x83\x99"   // U+E0D9 lucide:folder-plus
#define ICON_IMAGE          "\xee\x83\xb6"   // U+E0F6 lucide:image
#define ICON_MUSIC          "\xee\x84\xa2"   // U+E122 lucide:music
#define ICON_FILM           "\xee\x83\x90"   // U+E0D0 lucide:film
#define ICON_PACKAGE        "\xee\x84\xa9"   // U+E129 lucide:package
#define ICON_SETTINGS       "\xee\x85\x94"   // U+E154 lucide:settings
#define ICON_WRENCH         "\xee\x86\xb1"   // U+E1B1 lucide:wrench
#define ICON_HAMMER         "\xee\x83\xac"   // U+E0EC lucide:hammer
#define ICON_INFO           "\xee\x83\xb9"   // U+E0F9 lucide:info
#define ICON_KEYBOARD       "\xee\x8a\x84"   // U+E284 lucide:keyboard
#define ICON_HELP           "\xee\x82\x82"   // U+E082 lucide:circle-help
#define ICON_DOWNLOAD       "\xee\x82\xb2"   // U+E0B2 lucide:download
#define ICON_UPLOAD         "\xee\x86\x9e"   // U+E19E lucide:upload
#define ICON_EXTERNAL       "\xee\x82\xb9"   // U+E0B9 lucide:external-link
#define ICON_POWER          "\xee\x85\x80"   // U+E140 lucide:power
#define ICON_T_MESH         "\xee\x81\xa1"   // U+E061 lucide:box
#define ICON_T_LIGHT        "\xee\x87\x82"   // U+E1C2 lucide:lightbulb
#define ICON_T_SUN          "\xee\x85\xb8"   // U+E178 lucide:sun
#define ICON_T_CAMERA       "\xee\x86\xa5"   // U+E1A5 lucide:video
#define ICON_T_AUDIO        "\xee\x86\xab"   // U+E1AB lucide:volume-2
#define ICON_T_SCRIPT       "\xee\x83\x83"   // U+E0C3 lucide:file-code
#define ICON_T_PHYSICS      "\xee\x8f\x97"   // U+E3D7 lucide:atom
#define ICON_T_COLLIDER     "\xee\x87\x8b"   // U+E1CB lucide:box-select
#define ICON_T_UI           "\xee\x87\x81"   // U+E1C1 lucide:layout-dashboard
#define ICON_T_EMPTY        "\xee\x8d\x85"   // U+E345 lucide:circle-dot
#define ICON_T_GROUP        "\xee\x8c\xbc"   // U+E33C lucide:folder-tree
#define ICON_T_TERRAIN      "\xee\x88\xb1"   // U+E231 lucide:mountain
#define ICON_T_PARTICLE     "\xee\x90\x92"   // U+E412 lucide:sparkles
#define ICON_T_TRIGGER      "\xee\x86\xb4"   // U+E1B4 lucide:zap
#define ICON_T_DECAL        "\xee\x8e\xbb"   // U+E3BB lucide:stamp
#define ICON_T_BRAIN        "\xee\x8f\x86"   // U+E3C6 lucide:brain
#define ICON_T_ANIM         "\xee\x80\xb8"   // U+E038 lucide:activity
#define ICON_T_NET          "\xee\x86\xae"   // U+E1AE lucide:wifi
#define ICON_T_TRANSFORM    "\xee\x8b\xa5"   // U+E2E5 lucide:move-3d
#define ICON_T_MATERIAL     "\xee\x87\x9d"   // U+E1DD lucide:palette
#define ICON_T_SPRITE       "\xee\x83\xb6"   // U+E0F6 lucide:image
#define ICON_T_CHARACTER    "\xee\x8e\xb9"   // U+E3B9 lucide:footprints
#define ICON_T_TEXT         "\xee\x86\x98"   // U+E198 lucide:type
#define ICON_T_BUTTON       "\xee\x84\xa0"   // U+E120 lucide:mouse-pointer-click
#define ICON_T_LAYERS       "\xee\x94\xa9"   // U+E529 lucide:layers
#define ICON_T_GRID         "\xee\x83\xa9"   // U+E0E9 lucide:grid-3x3
#define ICON_T_NAV          "\xee\x84\xa3"   // U+E123 lucide:navigation
#define ICON_T_PREFAB       "\xee\x93\xba"   // U+E4FA lucide:blocks
#define ICON_T_SLIDERS      "\xee\x8a\x9a"   // U+E29A lucide:sliders-horizontal
#define ICON_T_SPLINE       "\xee\x8e\x8b"   // U+E38B lucide:spline
#define ICON_T_MONITOR      "\xee\x84\x9d"   // U+E11D lucide:monitor
#define ICON_T_SHADER       "\xee\x8d\xaa"   // U+E36A lucide:braces
#define ICON_T_FOG          "\xee\x88\x94"   // U+E214 lucide:cloud-fog
#define ICON_T_WIND         "\xee\x86\xb0"   // U+E1B0 lucide:wind
#define ICON_T_REVERB       "\xee\x8a\x83"   // U+E283 lucide:waves
#define ICON_T_TRAIL        "\xee\x94\xbe"   // U+E53E lucide:route
#define ICON_WARN           "\xee\x86\x93"   // U+E193 lucide:triangle-alert
#define ICON_ERROR          "\xee\x81\xb7"   // U+E077 lucide:circle-alert
#define ICON_OK             "\xee\x88\xa6"   // U+E226 lucide:circle-check
#define ICON_BUG            "\xee\x88\x8c"   // U+E20C lucide:bug
#define ICON_TERMINAL       "\xee\x86\x81"   // U+E181 lucide:terminal
#define ICON_GIT_BRANCH     "\xee\x83\xa2"   // U+E0E2 lucide:git-branch
#define ICON_GIT_COMMIT     "\xee\x83\xa3"   // U+E0E3 lucide:git-commit
#define ICON_CLOUD          "\xee\x82\x88"   // U+E088 lucide:cloud
#define ICON_CPU            "\xee\x82\xa9"   // U+E0A9 lucide:cpu
#define ICON_GAUGE          "\xee\x86\xbf"   // U+E1BF lucide:gauge
#define ICON_GIT_MERGE      "\xee\x83\xa4"   // U+E0E4 lucide:git-merge
#define ICON_ARROW_DOWN     "\xee\x81\x82"   // U+E042 lucide:arrow-down
#define ICON_LINK           "\xee\x84\x82"   // U+E102 lucide:link
#define ICON_CHEVRON_LEFT   "\xee\x81\xae"   // U+E06E lucide:chevron-left
#define ICON_GIT_PULL       "\xee\x83\xa5"   // U+E0E5 lucide:git-pull-request
#define ICON_SQUARE_PLUS    "\xee\x85\xb3"   // U+E173 lucide:square-plus
#define ICON_CIRCLE_PLAY    "\xee\x82\x80"   // U+E080 lucide:circle-play
// ---- プロジェクトランチャー用（2026-09-30）----
#define ICON_CLOCK          "\xee\x82\x87"   // U+E087 lucide:clock
#define ICON_PIN            "\xee\x89\x99"   // U+E259 lucide:pin
#define ICON_PIN_OFF        "\xee\x8a\xb6"   // U+E2B6 lucide:pin-off
#define ICON_NEWSPAPER      "\xee\x8d\x88"   // U+E348 lucide:newspaper
#define ICON_BOOK_OPEN      "\xee\x81\x9f"   // U+E05F lucide:book-open
#define ICON_CROSSHAIR      "\xee\x82\xac"   // U+E0AC lucide:crosshair
#define ICON_ROCKET         "\xee\x8a\x86"   // U+E286 lucide:rocket
#define ICON_GITHUB         "\xee\x83\xa6"   // U+E0E6 lucide:github
#define ICON_STAR           "\xee\x85\xb6"   // U+E176 lucide:star
#define ICON_GAMEPAD        "\xee\x83\x9f"   // U+E0DF lucide:gamepad-2
#define ICON_SPARKLES       "\xee\x90\x92"   // U+E412 lucide:sparkles
#define ICON_GIT_FORK       "\xee\x8a\x8d"   // U+E28D lucide:git-fork
#define ICON_CLOUD_DOWNLOAD "\xee\x82\x89"   // U+E089 lucide:cloud-download
#define ICON_ARROW_RIGHT    "\xee\x81\x89"   // U+E049 lucide:arrow-right
#define ICON_GRAD_CAP       "\xee\x88\xb4"   // U+E234 lucide:graduation-cap
#define ICON_LIFE_BUOY      "\xee\x84\x81"   // U+E101 lucide:life-buoy
#define ICON_MEGAPHONE      "\xee\x88\xb5"   // U+E235 lucide:megaphone
#define ICON_CIRCLE_USER    "\xee\x91\xa1"   // U+E461 lucide:circle-user
#define ICON_LOG_IN         "\xee\x84\x8d"   // U+E10D lucide:log-in
#define ICON_WAND           "\xee\x8d\x97"   // U+E357 lucide:wand-sparkles
#define ICON_HISTORY        "\xee\x87\xb5"   // U+E1F5 lucide:history
#define ICON_FOLDER_GIT     "\xee\x90\x8a"   // U+E40A lucide:folder-git-2
// ---- ビューポート専用ツールバー用（フェーズ 1a）----
#define ICON_BOOKMARK       "\xee\x81\xa0"   // U+E060 lucide:bookmark
#define ICON_MAGNET         "\xee\x8a\xb5"   // U+E2B5 lucide:magnet
#define ICON_RULER          "\xee\x85\x8b"   // U+E14B lucide:ruler
#define ICON_RECT           "\xee\x8d\xb6"   // U+E376 lucide:rectangle-horizontal
#define ICON_AXIS3D         "\xee\x8b\xbe"   // U+E2FE lucide:axis-3d
#define ICON_CAMERA         "\xee\x81\xa4"   // U+E064 lucide:camera
#define ICON_FOCUS          "\xee\x8a\x9e"   // U+E29E lucide:focus
#define ICON_SCAN_EYE       "\xee\x94\xb6"   // U+E536 lucide:scan-eye
#define ICON_RATIO          ""   // U+E4E8 lucide:ratio

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
        save = 0, git = 0, github = 0, commit = 0, push = 0,
        pull = 0, fetch = 0, merge = 0, refresh = 0;

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
