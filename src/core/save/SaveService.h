#pragma once

// セーブの実行時の窓口（プロセスに 1 つ）。Lua の save.* はここを通る。
//
// ★なぜ Application のメンバではなくプロセス単位の 1 個か:
//   Lua の状態はシーンを切り替えるたびに作り直されるが、プレイ時間・ゲームデータの版は
//   シーンをまたいで続く必要がある。ScriptEngine / Application の両方から同じものに届き、
//   どちらのヘッダも増やさずに済むのでこの形にした（音の担当と同じく、他の担当との衝突を避ける）。
//
// 書き込み先は save::SavesDir()（配布: %LOCALAPPDATA%/<ゲーム名>/saves、エディタ: <プロジェクト>/.dx12/saves）。
// 呼ぶのはメインスレッドだけ。

#include "core/save/SaveFile.h"

#include <chrono>
#include <functional>
#include <string>

namespace dx12e::save
{

class SaveService
{
public:
    static SaveService& Get();

    // 書き込み先。テスト / ツールは SetDirOverride で差し替えられる（空で解除）。
    std::filesystem::path Dir() const;
    void SetDirOverride(const std::filesystem::path& dir) { m_dirOverride = dir; }

    // ---- プレイ時間（秒）----
    // 基準値 + 最後に Reset / 読込してからの実時間。読込（save.read）で基準値がセーブの値になる。
    double PlayTime() const;
    void   ResetSession(double baseSeconds = 0.0);

    // ---- いま開いているシーン（assets 相対）。Application が注入する ----
    void SetSceneProvider(std::function<std::string()> fn) { m_sceneProvider = std::move(fn); }
    std::string CurrentScene() const { return m_sceneProvider ? m_sceneProvider() : std::string(); }

    // ---- スロット操作（中身の組み立ては呼び出し側）----
    WriteResult Write(const std::string& slot, const nlohmann::json& body);
    ReadResult  Read(const std::string& slot);
    bool        Exists(const std::string& slot);
    bool        Delete(const std::string& slot);
    std::vector<SlotSummary> List();

    // テストで失敗を注入する口（nullptr で実ファイルへ戻す）
    void SetFileOps(IFileOps* ops) { m_ops = ops; }
    IFileOps& Ops() { return m_ops ? *m_ops : RealFileOps(); }

private:
    SaveService();

    std::filesystem::path m_dirOverride;
    std::function<std::string()> m_sceneProvider;
    IFileOps* m_ops = nullptr;

    double m_playBase = 0.0;
    std::chrono::steady_clock::time_point m_sessionStart;
};

} // namespace dx12e::save
