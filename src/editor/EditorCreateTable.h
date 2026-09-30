#pragma once

// ===== 「エンティティを作る」項目の表（純データ）=====
// ヒエラルキーの「エンティティ追加」ボタン・空白部分の右クリック・コマンドパレット(Ctrl+K の「作成:」)が
// 同じ表を見る。項目を足す時はここだけを直す。marker は PendingSpawnRequest::modelPath に入れる
// 特殊名（Application が解釈する）。marker が空の項目は「専用の作成窓を開く」もの（地形 / スカルプト）。

#include <cstddef>

namespace dx12e::cmd
{

struct CreateItem
{
    const char* id;        // "create.box"
    const char* label;     // メニュー表示（UI 自動テストが名前で引くので変えない）
    const char* labelEn;   // パレットの検索用の別名
    const char* marker;    // "__primitive_box__" 等。"" = 専用窓を開く（openTool を見る）
    float       defaultY;  // 置く高さ（ワールド Y）。地面の交点 / カメラ前のどちらでも XZ だけ使い、Y はこの値
    const char* group;     // メニューの区切り: "基本" "カメラ" "ライト" "Gimmick" "エフェクト" "UI"
    const char* openTool;  // marker が空のとき開く窓: "terrain" / "sculpt"
};

inline constexpr CreateItem kCreateItems[] = {
    {"create.box",      "Box",                         "Cube",             "__primitive_box__",    0.5f, "基本", ""},
    {"create.sphere",   "Sphere",                      "Ball",             "__primitive_sphere__", 0.5f, "基本", ""},
    {"create.plane",    "Plane",                       "Floor Ground",     "__primitive_plane__",  0.0f, "基本", ""},
    {"create.empty",    "Empty",                       "Empty Object",     "__empty__",            0.0f, "基本", ""},
    {"create.terrain",  "Terrain（地形・山を作る）",   "Terrain Landscape","",                     0.0f, "基本", "terrain"},
    {"create.sculpt",   "Sculpt（異形・洞窟・アーチ・岩）", "Sculpt Mesh", "",                    0.0f, "基本", "sculpt"},
    {"create.camera",   "Camera",                      "Camera",           "__camera__",           2.0f, "カメラ", ""},
    {"create.dirLight", "Directional Light",           "Sun Light",        "__directional_light__",5.0f, "ライト", ""},
    {"create.pointLight","Point Light",                "Point Light",      "__point_light__",      3.0f, "ライト", ""},
    {"create.spotLight","Spot Light",                  "Spot Light",       "__spot_light__",       5.0f, "ライト", ""},
    {"create.gimmickSpike","Spike Pulse（上下するトゲ）","Gimmick Spike",  "__gimmick_spike__",    0.7f, "Gimmick", ""},
    {"create.gimmickSlide","Slide Wall（左右に動く壁）", "Gimmick Slide Wall","__gimmick_slide__",  0.75f,"Gimmick", ""},
    {"create.gimmickWall","Static Wall（動かない壁）", "Gimmick Static Wall","__gimmick_wall__",   0.7f, "Gimmick", ""},
    {"create.particle", "Particle Emitter（配置エフェクト）","Particle Effect","__particle_emitter__",1.0f,"エフェクト", ""},
    {"create.trigger",  "Trigger（イベント範囲）",     "Trigger Volume",   "__trigger__",          1.0f, "エフェクト", ""},
    {"create.decal",    "Decal（投影デカール・弾痕/汚れ）","Decal Projection","__decal__",         1.0f, "エフェクト", ""},
    {"create.water",    "Water（水面・湖/海/川/プール）",  "Water Lake Ocean River Pool","__water__", 0.0f, "エフェクト", ""},
    {"create.uiCanvas", "Canvas（UIルート）",          "UI Canvas",        "__ui_canvas__",        0.0f, "UI", ""},
    {"create.uiImage",  "Image（画像/単色矩形）",      "UI Image",         "__ui_image__",         0.0f, "UI", ""},
    {"create.uiText",   "Text（テキスト）",            "UI Text Label",    "__ui_text__",          0.0f, "UI", ""},
    {"create.uiButton", "Button（ボタン）",            "UI Button",        "__ui_button__",        0.0f, "UI", ""},
    {"create.uiSlider", "Slider（スライダー）",        "UI Slider",        "__ui_slider__",        0.0f, "UI", ""},
    {"create.uiToggle", "Toggle（トグル）",            "UI Toggle Checkbox","__ui_toggle__",       0.0f, "UI", ""},
    {"create.uiScroll", "ScrollView（スクロール）",    "UI Scroll View",   "__ui_scrollview__",    0.0f, "UI", ""},
};
inline constexpr size_t kCreateItemCount = sizeof(kCreateItems) / sizeof(kCreateItems[0]);

// マーカーが UI 要素（位置を持たず、Application が Canvas の子へ配置する）か
inline bool IsUiMarker(const char* marker)
{
    return marker && marker[0] == '_' && marker[1] == '_' && marker[2] == 'u' && marker[3] == 'i';
}

} // namespace dx12e::cmd
