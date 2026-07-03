#pragma once

#include "core/Types.h"
#include "resource/ShaderRuntimeCompiler.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e
{

// プロジェクト独自 HLSL シェーダー(既存シェーダーの上書き + 完全新規のカスタムシェーダー)を
// 実行時コンパイルし、保存 0.5s ポーリングでホットリロードするための中枢マネージャ。
// エディタモードのみ Application が所有・生成する(ゲームモードでは作らない=常にオーバーライド無し)。
//
// 使い方の要点:
//  - Initialize() を最初の ShaderCompiler::LoadFromFile より前に呼ぶ(RootSignature 生成直後)。
//  - Poll() を 0.5s おきに呼ぶ。戻り値が非空なら CommandQueue::WaitIdle() 後に該当ハンドラを実行し、
//    Application 側で hotReloadFlash 等を立てる。
//  - ShaderCompiler::LoadFromFile はコンパイル成否に関わらず TryGetOverride() を先に見る
//    (ゲームモードでは Instance() が未生成のため常にヒットしない=挙動不変)。
// 所有権は Application(unique_ptr)。ShaderCompiler::LoadFromFile 等からグローバルに参照できるよう
// 生ポインタを静的に公開するだけで、生成/破棄の主体にはならない(SetInstance は登録/解除のみ)。
class ShaderManager
{
public:
    static ShaderManager* Instance();                   // 未登録なら nullptr(ゲームモード等)
    static void SetInstance(ShaderManager* instance);   // Application が Initialize/Shutdown で呼ぶ

    // Registry 一致のプロジェクトオーバーライドを走査コンパイルし、監視ベースラインを記録する。
    // 個々のソースはプロジェクト側 .hlsl が無ければエンジン側 .hlsl を「解決先」として記録するだけ
    // (baked .cso が既に正しいのでコンパイルはしない。エンジン側を直接編集した時だけ Poll() が拾う)。
    void Initialize();

    // プロジェクト切替(ランチャーからのロード)時に呼ぶ。ストア全消去→Initialize() を再実行。
    void OnProjectRootChanged();

    // 0.5s ポーリング。変化を検知したソースを再コンパイルしてストアを更新する。
    // 戻り値: 影響を受けた「リロードキー」(cso ファイル名、小文字)の集合。
    // カスタムシェーダーの変更は relPath(小文字)を "custom:" プレフィックス付きで返す。
    std::vector<std::wstring> Poll();

    // csoNames(ファイル名、大小無視)のいずれかが Poll() の戻り値に含まれたら handler を呼ぶ。
    void RegisterReloadHandler(std::initializer_list<const wchar_t*> csoNames, std::function<void()> handler);
    // "custom:<relPath>" 形式のキー向け。任意のカスタムシェーダー変更で呼ばれる(relPath を渡す)。
    void RegisterCustomReloadHandler(std::function<void(const std::string& relPath)> handler);

    // handlerKeys(Poll() の戻り値そのもの)に対応する全ハンドラを実行する。
    void DispatchReloadHandlers(const std::vector<std::wstring>& changedKeys);

    // 登録済みの全リロードキー(cso名 + "custom:"付きカスタムシェーダー)。
    // プロジェクト切替直後など「差分を問わず全部作り直したい」場面で DispatchReloadHandlers に渡す。
    std::vector<std::wstring> AllKnownReloadKeys() const;

    // ShaderCompiler::LoadFromFile から呼ぶ。csoPath はフルパスで良い(ファイル名部分のみ照合)。
    const std::vector<u8>* TryGetOverride(const std::wstring& csoPath) const;

    bool IsRuntimeCompileAvailable() const { return m_available; }

    // --- カスタムシェーダー(Registry外、MeshRenderer 割当用) ---
    // relPath はプロジェクト assets/shaders 相対(スラッシュ区切り)。VSMain/PSMain 固定・vs_6_0/ps_6_0 固定。
    bool CompileCustomShader(const std::string& relPath);
    const std::vector<u8>* GetCustomVsBytecode(const std::string& relPath) const;
    const std::vector<u8>* GetCustomPsBytecode(const std::string& relPath) const;
    bool HasValidCustomShader(const std::string& relPath) const;
    std::string GetCustomShaderError(const std::string& relPath) const;

    // --- BuildGame(配布)用 ---
    // プロジェクトの全オーバーライド+全カスタムシェーダーを差分無視で強制再コンパイルする。
    // 1つでも失敗したら false を返し、errorsOut に relPath を積む(m_overrides/m_customs の既存値は
    // 保持されるので、呼び出し側は false 時にビルドを中断すること=壊れたシェーダーを出荷しない)。
    bool RecompileAllForBuild(std::vector<std::string>* errorsOut);
    // 現在有効なカスタムシェーダーの relPath 一覧(小文字正規化、valid なもののみ)。
    std::vector<std::string> AllValidCustomRelPaths() const;

private:
    // 1ソース分の全バリアントをコンパイルし、全部成功した時だけ m_overrides へ反映する。
    // hlslAbsPath はこの呼び出しで実際にコンパイルする .hlsl の絶対パス。
    bool CompileSourceAllVariants(const struct ShaderSource& src, const std::wstring& hlslAbsPath);

    // relPath (例 "forward/Forward.hlsl") の解決先絶対パスを返す。プロジェクト優先。
    std::wstring ResolveSourcePath(const std::string& relPath) const;
    // relPath の直下ディレクトリを、プロジェクト側/エンジン側それぞれの絶対パスとして返す(-I 用)。
    std::vector<std::wstring> IncludeDirsFor(const std::string& relPath) const;
    // プロジェクト assets/shaders/ を走査し、Registry に無い .hlsl(=カスタム)の新規/変更を検出・コンパイルする。
    // changedOut が非null なら "custom:<relPath小文字>" 形式のキーを追記する。
    void ScanCustomShaders(std::vector<std::wstring>* changedOut);
    // Initialize()/OnProjectRootChanged() 共通処理: 全 Registry ソースを解決・監視ベースライン記録、
    // プロジェクトオーバーライドがあれば即コンパイル、カスタムシェーダーも走査する。
    void EstablishBaseline();

    static std::wstring LowerCsoName(const std::wstring& csoPathOrName);
    static std::string  LowerRelPath(const std::string& relPath);

    ShaderRuntimeCompiler m_compiler;
    bool m_available = false;

    mutable std::mutex m_mutex;

    // csoファイル名(小文字)→バイトコード。ShaderCompiler::LoadFromFile が優先参照する。
    std::unordered_map<std::wstring, std::vector<u8>> m_overrides;

    // 監視ベースライン: relPath(小文字)→(解決済み絶対パス, mtime)。Poll() が差分検知に使う。
    struct WatchedSource
    {
        std::wstring resolvedPath;
        std::filesystem::file_time_type mtime{};
    };
    std::unordered_map<std::string, WatchedSource> m_watchedSources;   // key: relPath小文字（Registry）
    std::unordered_map<std::string, WatchedSource> m_watchedDeps;      // key: .hlsli relPath小文字

    // カスタムシェーダー
    struct CustomEntry
    {
        std::vector<u8> vs, ps;
        bool valid = false;
        std::string errorLog;
    };
    std::unordered_map<std::string, CustomEntry> m_customs;                       // key: relPath小文字
    std::unordered_map<std::string, std::filesystem::file_time_type> m_customMTimes; // key: relPath小文字

    // コールバック登録
    std::vector<std::function<void()>> m_handlers;
    std::unordered_map<std::wstring, std::vector<size_t>> m_handlersByCso; // key: csoファイル名小文字
    std::vector<std::function<void(const std::string&)>> m_customHandlers;

    static ShaderManager* s_instance;
};

} // namespace dx12e
