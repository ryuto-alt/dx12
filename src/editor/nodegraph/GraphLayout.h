#pragma once

// ===== ノードの大きさ・ピン位置の計算（純ロジック。文字幅の測定は差し込み）=====
// 幅はノード「型」ごとに決まる（値欄の有無は接続状態に依らず領域を確保する）ので、型ごとにキャッシュする。
// 単位はグラフ座標（DPI 100%・ズーム 100% で 1 = 論理 1px）。GraphView が描画時に scale を掛ける。

#include "editor/nodegraph/GraphTypes.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e::ng
{

struct LayoutConfig
{
    float headerH   = 27.0f;
    float rowH      = 22.0f;
    float padX      = 12.0f;
    float padBottom = 9.0f;
    float pinR      = 5.5f;
    float minW      = 136.0f;
    float slotGap   = 8.0f;
    float colGap    = 24.0f;
    float previewH  = 64.0f;
    float rerouteSize = 22.0f;
    float slotH     = 17.0f;
    float textUnits  = 12.5f;   // ピン名・値
    float titleUnits = 13.5f;   // タイトル
};

// 文字列の幅（グラフ単位。textUnits / titleUnits のサイズで測った値）。
using TextMeasureFn = std::function<float(const std::string& text, bool title)>;

struct TypeLayout
{
    Vec2  size;
    float headerH = 0.0f;
    std::vector<float> inY;        // 入力行の中心 Y（ノード上端から）
    std::vector<float> outY;
    std::vector<Rect>  inSlot;     // 値欄（ノード左上基準）。無ければ幅 0
    float slotColX = 0.0f;
    float inLabelX = 0.0f;
    float outLabelRight = 0.0f;    // 出力ラベルの右端 X
    Rect  preview;                 // 幅 0 = 予約なし
    bool  reroute = false;
};

// 値欄の 1 ピンあたりの幅（種別ごと）
float SlotWidth(ValueKind k);
// 値欄の中の成分 comp の矩形（Float=全体、Vec2..4=均等分割）。Color / Bool / Enum は全体。
Rect SlotComponentRect(const Rect& slot, ValueKind k, int comp);

class GraphLayout
{
public:
    explicit GraphLayout(TextMeasureFn measure = nullptr, LayoutConfig cfg = {});

    void SetMeasure(TextMeasureFn m) { m_measure = std::move(m); Invalidate(); }
    void Invalidate() { m_cache.clear(); }
    const LayoutConfig& Config() const { return m_cfg; }

    const TypeLayout& For(const NodeTypeDesc& t);
    // n.desc が必須（モデルの FindNode が埋める）。
    Rect NodeRect(const NodeData& n);
    Vec2 PinPos(const NodeData& n, PinRef pin);
    // 入力ピン index の値欄（グラフ座標。値欄なし / プロパティ以外で接続済みでも領域は返す）。幅 0 = 値欄なし。
    Rect SlotRect(const NodeData& n, int inputIndex);

    // 既定の測定（等幅近似。ImGui なしのテスト用）。
    static float ApproxWidth(const std::string& s, bool title);

private:
    TextMeasureFn m_measure;
    LayoutConfig  m_cfg;
    std::unordered_map<const NodeTypeDesc*, TypeLayout> m_cache;
};

} // namespace dx12e::ng
