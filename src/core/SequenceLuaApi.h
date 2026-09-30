#pragma once
// Lua の `Sequence` グローバル(scripting/ScriptEngine.cpp)がシーケンサーのホストを叩く窓口。
// 依存ゼロ(標準ライブラリだけ)。実体は core/SequencerHost.h。
// ScriptEngine は Application が一度だけ注入したポインタを保持し、Lua から呼ばれた時に参照する(null 許容)。

#include <string>

namespace dx12e
{

struct SequencePlayOptions
{
    double rate = 1.0;        // 再生速度(負 = 逆再生)
    int loop = -1;            // -1 = シーケンスの meta.loop(無ければ 1 回)/ 0 = 1 回 / 1 = ループ / 2 = ピンポン
    double from = 0.0;        // 開始位置(秒。範囲の先頭からの相対ではなく絶対位置)
    bool clockGame = false;   // true = ゲーム時間(タイムスケール適用)。既定は実時間(設計書 §3.5)
    bool restoreOnEnd = true; // 終了時にタイムスケール・カメラの選択を戻す
    double startDelay = 0.0;  // 再生開始までの待ち(秒。実時間 / ゲーム時間はクロック設定に従う)
};

class ISequenceLuaApi
{
public:
    virtual ~ISequenceLuaApi() = default;
    // 再生を始める。戻り値は再生 ID(1 以上)。失敗は 0 で err に理由。同じシーケンスが再生中なら、それを止めて頭から入れ替える。
    virtual int LuaPlay(const std::string& name, const SequencePlayOptions& opt, std::string& err) = 0;
    // name(シーケンス名)または "#<再生ID>" で止める。"*" は全部。止めたら true。
    virtual bool LuaStop(const std::string& nameOrId) = 0;
    virtual bool LuaPause(const std::string& name, bool paused) = 0;
    virtual bool LuaSeek(const std::string& name, double seconds) = 0;
    virtual bool LuaIsPlaying(const std::string& name) const = 0;
    // シーケンスの長さ(秒)。アセットが無ければ -1。読み込みはしない(再生中か、読み込み済みのものだけ。無ければ読む)。
    virtual double LuaDuration(const std::string& name) = 0;
    // 再生位置(秒)。再生していなければ -1。
    virtual double LuaTime(const std::string& name) const = 0;
};

} // namespace dx12e
