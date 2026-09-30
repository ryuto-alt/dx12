#pragma once
// カーブ評価: イージング統一表・キー補間(Step/Linear/Auto/Bezier/Ease)・範囲外挙動・クォータニオン補間・色空間。
// すべて純関数(状態を持たない = 履歴(ヒステリシス)に依存しない)。

#include <string_view>

#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// イージング統一表
// ---------------------------------------------------------------------------
struct EaseInfo
{
    EaseId id;
    const char* name;        // 正準名(ファイルの "e:<name>")
    int uiIndex;             // C++ UiEase の型番号(src/ui/UiEase.h)。無ければ -1
    bool inLua;              // Lua の Ease テーブル(ScriptEngine prelude)にある名前と同一か
    const char* legacyTs;    // sequence.ts の名前(in/out/inOut/…)。無ければ nullptr
};

const EaseInfo& GetEaseInfo(EaseId id);
const char* EaseName(EaseId id);
// 正準名 + 別名(sequence.ts の "in" / "out" / "inOut")から引く。
bool EaseFromName(std::string_view name, EaseId& out);
bool EaseFromUiIndex(int uiEaseType, EaseId& out);   // UiEase 型番号 → EaseId
int  UiIndexOf(EaseId id);                            // EaseId → UiEase 型番号(無ければ -1)
// p を [0,1] へ clamp してから評価する(UiEase と同じ)。端点は厳密に 0 / 1 を返す。
double EvalEase(EaseId id, double p);

// ---------------------------------------------------------------------------
// 色空間
// ---------------------------------------------------------------------------
double SrgbToLinear(double c);   // IEC 61966-2-1
double LinearToSrgb(double c);
// チャンネル名の末尾要素が "a"(アルファ)なら色空間変換の対象外
bool ChannelIsAlpha(std::string_view channelName);

// ---------------------------------------------------------------------------
// スカラーカーブ
// ---------------------------------------------------------------------------
// keys が空でないチャンネルを時刻 t で評価する。srgb=true ならキー値を sRGB→リニアへ変換してから補間し、リニアで返す。
// ・範囲外: ch.pre / ch.post(Hold / Linear(端の接線で延長) / Loop / PingPong)
// ・同時刻のキー: 配列の後ろが勝つ(t がその時刻ちょうどなら後ろのキーの値)
// ・単一キー: 常にその値
double EvalChannel(const Channel& ch, Tick t, bool srgb = false);

// Auto 補間のキー i の接線(値/ティック)。両隣から Catmull-Rom(非等間隔)で求め、極値で 0、
// |m| <= 3*min(左右の割線傾き) に制限する(Fritsch-Carlson: 単調区間でオーバーシュートしない)。端は片側の割線。
double AutoTangent(const std::vector<Key>& keys, std::size_t i, bool srgb = false);

// SetInterp で Bezier に切り替えるときの既定ハンドル(Auto と一致する見た目になる 1/3 スパン)。
void DefaultBezierHandles(const std::vector<Key>& keys, std::size_t i,
                          Tick& inDt, double& inDv, Tick& outDt, double& outDv);

// Hermite(接線 m0/m1 は値/ティック、h は区間のティック幅、p は 0..1)
double Hermite(double v0, double m0, double v1, double m1, double h, double p);

// ---------------------------------------------------------------------------
// クォータニオン
// ---------------------------------------------------------------------------
struct Quat { double x = 0, y = 0, z = 0, w = 1; };

double QuatDot(const Quat& a, const Quat& b);
Quat   QuatNormalize(const Quat& q);                       // 長さ 0 なら恒等
Quat   QuatNeg(const Quat& q);
// 最短経路の球面線形補間(dot < 0 なら b を反転)。結果は正規化済み。
Quat   QuatSlerp(const Quat& a, const Quat& b, double t);
// 球面 4 点補間(Auto / Bezier 区間に使う)。s0/s1 は QuatSquadTangent で求める内部制御点。
// 入力の 4 点は同じ半球に揃えておくこと(EvalQuatChannels が揃える)。内側/外側の slerp は符号を反転しない。
Quat   QuatSquad(const Quat& q0, const Quat& q1, const Quat& s0, const Quat& s1, double t);
Quat   QuatSquadTangent(const Quat& prev, const Quat& cur, const Quat& next);
// 2 つの回転の角度差(ラジアン、0..π。q と -q は同一視)
double QuatAngle(const Quat& a, const Quat& b);

// x/y/z/w の 4 チャンネル(キー時刻が揃っている前提。CanonicalizeTrack が検査)を時刻 t で評価する。
// ・各キーは正規化し、隣り合うキーで半球を揃える(q と -q の入れ替わりで出力が跳ばない)
// ・Step: 直前キー / Linear: slerp / Ease: イージングした p で slerp / Auto・Bezier: squad(等間隔を仮定)
// ・範囲外: Hold / Loop / PingPong(Linear は Hold 扱い)
Quat EvalQuatChannels(const Channel& x, const Channel& y, const Channel& z, const Channel& w, Tick t);

// ---------------------------------------------------------------------------
// 範囲外の時刻の畳み込み(テスト・再生用に公開)
// ---------------------------------------------------------------------------
// Loop / PingPong のとき t を [first, last] へ畳む。それ以外(Hold/Linear)は t をそのまま返す。
Tick WrapTimeForExtrap(Extrap mode, Tick t, Tick first, Tick last);

// Euler 角(度)を前の値に対して ±180° 以内へ連続化する(キー挿入時の unwrap 用ヘルパ)。
double UnwrapDegrees(double prev, double cur);

} // namespace dx12e::seq
