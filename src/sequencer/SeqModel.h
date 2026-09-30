#pragma once
// シーケンサーのデータモデル(.dxseq の中身)。純データ + 純関数。
// 設計: docs/SEQUENCER_DESIGN.md §2 / 形式: docs/DXSEQ_FORMAT.md

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sequencer/SeqTypes.h"

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// 補間・イージング
// ---------------------------------------------------------------------------
// キー → 次のキー までの区間の補間(そのキーが持つ)。
//   Step   定数(次のキーまで値を保持。bool/int/enum/文字列的な値は必ずこれ)
//   Linear 線形
//   Auto   スムーズ(Catmull-Rom → Hermite。極値で接線 0・単調区間でオーバーシュート無し)
//   Bezier 重み付きベジェ(ハンドル = inDt/inDv/outDt/outDv)
//   Ease   名前付きイージング(ease フィールド。v0→v1 を 1 本の曲線で結ぶ)
enum class Interp : std::uint8_t { Step = 0, Linear, Auto, Bezier, Ease };

// 既存 3 系統(Lua Ease 9 種 / C++ UiEase 12 種 / sequence.ts 6 種)の和集合 = 15 種。式は変えない。
enum class EaseId : std::uint8_t
{
    Linear = 0, InQuad, OutQuad, InOutQuad, InCubic, OutCubic, InOutCubic, InOutSine,
    OutBack, OutBounce, OutElastic, OutExpo, InBack, InOutBack, OutQuint,
    Count
};

// 範囲外(最初のキーより前 = pre / 最後のキーより後 = post)の挙動
enum class Extrap : std::uint8_t { Hold = 0, Linear, Loop, PingPong };

// ---------------------------------------------------------------------------
// キー
// ---------------------------------------------------------------------------
// ハンドルの規約(Bezier のみ有効。他の補間では 0 に正規化される):
//   out ハンドル位置 = (t + outDt, v + outDv)     … このキーから次のキーへ向かう側
//   in  ハンドル位置 = (t - inDt,  v - inDv)      … 前のキーから来る側
//   → 対称でなめらかな接線は inDt==outDt かつ inDv==outDv。dt はティック(>= 0)、dv は値。
struct Key
{
    Tick   t  = 0;
    double v  = 0.0;
    Interp ip = Interp::Auto;
    EaseId ease = EaseId::Linear;   // ip == Ease のときだけ意味を持つ
    Tick   inDt = 0,  outDt = 0;
    double inDv = 0.0, outDv = 0.0;
};

bool operator==(const Key& a, const Key& b);
inline bool operator!=(const Key& a, const Key& b) { return !(a == b); }

// 正準化: ip != Bezier ならハンドル 0、ip != Ease なら ease=Linear、(Ease, Linear) は Linear へ、-0 は 0 へ。
void NormalizeKey(Key& k);
// 値の妥当性(有限・ハンドルの dt が非負・時刻が範囲内・ip/ease が有効)。ok なら空文字。
std::string CheckKey(const Key& k);

struct Channel
{
    std::string name;
    Extrap pre  = Extrap::Hold;
    Extrap post = Extrap::Hold;
    std::vector<Key> keys;   // t 昇順(同時刻は配列の後ろが勝つ)
};

// ---------------------------------------------------------------------------
// トラック
// ---------------------------------------------------------------------------
enum class TrackType : std::uint8_t
{
    // カーブ族(チャンネル = カーブ。全区間で連続)
    Transform,   // position.{x,y,z} / rotation.{x,y,z}(Euler度) or rotation.{x,y,z,w}(クォータニオン) / scale.{x,y,z}
    Property,    // 任意プロパティ path = "Component.field"(float / bool / int / color)
    Camera,      // fov / near / far / orthoSize / dofAperture / dofFocalLength / dofBlurSize / dofFocusDist
    Post,        // ポストプロセス(scene バインディング。チャンネル名 = PostProcessSettings のフィールド名)
    TimeScale,   // タイムスケール(scene。チャンネル "value")
    Light,       // ライトのプリセット(intensity / color.{r,g,b} / range …。中身は Property と同じ)
    // クリップ族(区間 {start,dur} を持つ)
    AnimationClip, Audio, VfxSpawn, Shake, Subsequence,
    // イベント族(点)
    Event,
    // 制約族(キーを持たない・毎フレーム計算。S0 では型とシリアライズだけ)
    Aim,
    Count
};

enum class TrackFamily : std::uint8_t { Curve, Clip, Event, Constraint };
TrackFamily FamilyOf(TrackType t);
const char* TrackTypeName(TrackType t);                 // "transform" 等(ファイル上の表記)
bool        TrackTypeFromName(std::string_view s, TrackType& out);

enum class ValueType : std::uint8_t { Float = 0, Bool, Int, Color };       // Property の値型
enum class ColorSpace : std::uint8_t { Linear = 0, Srgb };                 // Color のキー値の空間
enum class RotationMode : std::uint8_t { Euler = 0, Quat };                // Transform の回転表現

struct Clip
{
    std::string id;
    Tick start = 0;
    Tick dur   = 0;          // 0 = 点(t == start でだけ有効)
    SeqValue params;         // Object。予約種別のパラメータ(clip 名・音声パス・amp/freq …)
};

struct EventItem
{
    std::string id;
    Tick t = 0;
    std::string kind;        // "emit" / "lua" / "loadScene" / "log" 等
    std::string name;
    SeqValue params;         // Object(data など)
};

// トラックのヘッダ(チャンネル/クリップ/イベントを除く部分。SetTrackHeader で丸ごと入れ替える)
struct TrackHeader
{
    std::string name;                        // 表示名(空 = 既定表示)
    bool mute = false;
    bool lock = false;
    std::string path;                        // Property: "Component.field"
    ValueType   valueType  = ValueType::Float;
    ColorSpace  colorSpace = ColorSpace::Linear;
    RotationMode rotation  = RotationMode::Euler;   // Transform
    SeqValue params;                         // Object。その他のフィールド(aim.target / camera.focus …)
};

struct Track : TrackHeader
{
    std::string id;
    TrackType type = TrackType::Transform;
    std::vector<Channel>   channels;   // Curve 族
    std::vector<Clip>      clips;      // Clip 族
    std::vector<EventItem> events;     // Event 族(並びは任意。発火時に時刻で整列する)
};

// ---------------------------------------------------------------------------
// バインディング(トラック群の対象)
// ---------------------------------------------------------------------------
enum class BindingKind : std::uint8_t { Entity = 0, Scene, Spawnable };

struct BindingHint
{
    std::string guid;   // EntityGuid の 16 桁 hex(空 = 未設定)
    std::string path;   // 最寄りの guid 付き祖先からの相対階層パス "A/B/C"
    std::string name;   // エンティティ名
};
bool operator==(const BindingHint& a, const BindingHint& b);

struct BindingHeader
{
    std::string name;
    BindingKind kind = BindingKind::Entity;
    BindingHint hint;
    SeqValue params;
};

struct Binding : BindingHeader
{
    std::string id;
    std::vector<Track> tracks;
};

// ---------------------------------------------------------------------------
// カット・マーカー・シーケンス
// ---------------------------------------------------------------------------
struct Cut
{
    std::string id;
    Tick start = 0;
    Tick end   = 0;          // [start, end)。最後のカットで end == 範囲末尾なら末尾フレームも含む
    std::string camera;      // バインディング ID
    Tick blend = 0;          // 予約(ディゾルブ。S7)
};

struct Marker
{
    std::string id;
    Tick t = 0;
    std::string name;
    SeqValue params;         // Object(color 等)
};

struct RenderSettings
{
    int width  = 1920;
    int height = 1080;
    double shutter = 0.5;
    int warmup = 8;
};
bool operator==(const RenderSettings& a, const RenderSettings& b);

struct Sequence
{
    std::string id;
    std::string name;
    std::int64_t ticksPerSecond = kDefaultTicksPerSecond;
    int frameRate = 30;
    bool hasRange = false;   // false = 全要素の外接(GetRange が計算する)
    Tick rangeStart = 0;
    Tick rangeEnd   = 0;
    std::optional<RenderSettings> render;
    SeqValue meta;           // Object(ユーザー/ツールのメタデータ)
    std::vector<Cut> cuts;
    std::vector<Marker> markers;
    std::vector<Binding> bindings;
};

// ---------------------------------------------------------------------------
// ヘルパ
// ---------------------------------------------------------------------------
struct TrackLoc { int binding = -1; int track = -1; bool Valid() const { return binding >= 0; } };

int  FindBinding(const Sequence& s, std::string_view id);            // -1 = 無い
TrackLoc FindTrack(const Sequence& s, std::string_view trackId);
Track*       GetTrack(Sequence& s, std::string_view trackId);
const Track* GetTrack(const Sequence& s, std::string_view trackId);
int  FindChannel(const Track& t, std::string_view name);              // -1 = 無い

// 全要素の ID(シーケンス自身・バインディング・トラック・クリップ・イベント・マーカー・カット)を集める。
void CollectIds(const Sequence& s, std::unordered_set<std::string>& out);
bool IdExists(const Sequence& s, std::string_view id);
// 予約済みの ID で IdAllocator を初期化する(以後 New() は既存と衝突しない)。
void ReserveAllIds(const Sequence& s, IdAllocator& alloc);

// 全キー/クリップ終端/イベント/マーカー/カットの外接時刻(空なら 0)。最小は常に 0 を含む。
Tick SequenceExtent(const Sequence& s);
// 再生範囲。hasRange なら [rangeStart, rangeEnd]、無ければ [0, SequenceExtent]。
void GetRange(const Sequence& s, Tick& start, Tick& end);

// 予約語(トラック/クリップ/イベントの params のキーに使えない名前 = ファイルの型付きフィールドと衝突する)
bool IsReservedTrackParam(std::string_view key);
bool IsReservedClipParam(std::string_view key);
bool IsReservedEventParam(std::string_view key);
bool IsReservedBindingParam(std::string_view key);
bool IsReservedMarkerParam(std::string_view key);

const char* BindingKindName(BindingKind k);
bool BindingKindFromName(std::string_view s, BindingKind& out);
const char* ExtrapName(Extrap e);
bool ExtrapFromName(std::string_view s, Extrap& out);

// トラック単体の構造検査+正準化(キーの正準化・並び・チャンネル名の一意・ID 形式・族の整合・クォータニオンの整列)。
// 失敗なら理由(日本語)を返す。ok なら空。tr は正準化されて返る(Sort はしない。昇順違反は誤り)。
std::string CanonicalizeTrack(Track& tr);

// クォータニオン回転(Transform, rotation=quat)の rotation.x/y/z/w が 4 本揃い、キー数と時刻が一致しているか。
// 問題なければ空、あれば理由(日本語)。揃っていなくても構造エラーにはならない(評価はその Transform の回転を通常のスカラーとして扱う)。
std::string QuatAlignmentProblem(const Track& tr);

// 検査結果 1 件。Error は「このデータは扱えない」(パーサが拒否する)、Warning は「扱えるが要注意」。
struct SeqIssue
{
    enum class Severity : std::uint8_t { Error, Warning };
    Severity severity = Severity::Error;
    std::string path;      // "bindings[1].tracks[0]" のような位置
    std::string message;   // 日本語
};

// シーケンス全体の検査(ID の一意/形式・トラック構造・カットの参照/範囲・フレームレート・ティックの整合)。
// Error: ID 重複/形式不正・トラック構造不正・カットが未知バインディングを指す・tps/fps <= 0・時刻が範囲外。
// Warning: カットの重なり/穴・fps がティックに整数で乗らない・quat の整列など。
std::vector<SeqIssue> ValidateSequence(const Sequence& s);
// Error が 1 件でもあればその文言(なければ空)。
std::string CheckSequenceStructure(const Sequence& s);

} // namespace dx12e::seq
