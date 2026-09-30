#pragma once

#include "core/Types.h"

#include <string>
#include <vector>
#include <wrl/client.h>

struct IDxcUtils;
struct IDxcCompiler3;
struct IDxcIncludeHandler;

namespace dx12e
{

// IDxcCompiler3 の薄いラッパー。ビルド時コンパイル(CMake+dxc.exe)とは別の、
// エディタ実行中のホットリロード用インプロセスコンパイラ。
// CMake の DXC 呼び出し(-T/-E/-D)と同一の引数意味論に揃えている。
class ShaderRuntimeCompiler
{
public:
    struct CompileRequest
    {
        std::wstring hlslPath;                  // 絶対パス
        std::wstring entry;                      // L"VSMain" 等
        std::wstring profile;                    // L"vs_6_0" / L"ps_6_0" / L"cs_6_0"
        std::vector<std::wstring> defines;        // L"LDR_OUTPUT=1" 等（空可）
        std::vector<std::wstring> includeDirs;    // -I 検索パス。優先順(先頭が最優先)
        // ★マテリアルグラフ G2b が追加（既定は空 = 従来の呼び出しは 1 バイトも変わらない）
        std::vector<std::wstring> extraArgs;      // 追加の DXC 引数（例 L"-HV", L"2021"）
        std::string sourceText;                   // 非空ならファイルを読まずこの UTF-8 テキストをコンパイルする（hlslPath は表示名）
    };

    struct CompileResult
    {
        bool success = false;
        std::vector<u8> dxil;
        std::string errorLog;                     // UTF-8。失敗時のみ非空とは限らない(警告あり得る)
        std::vector<std::wstring> includedFiles;   // 実際に #include で読まれた絶対パス群
    };

    ShaderRuntimeCompiler();
    // ComPtr<IDxcIncludeHandler> 等の完全型は dxcapi.h(.cpp側)でしか見えないため、
    // このクラスを値メンバとして持つクラス(ShaderManager)からの暗黙デストラクタ生成が
    // 前方宣言だけでは失敗する。デストラクタを .cpp 側で定義することで解決する。
    ~ShaderRuntimeCompiler();

    // false を返したら以降 Compile() は呼ばない(コンパイル機能が無効なだけでエディタは動く)。
    bool Initialize();

    CompileResult Compile(const CompileRequest& req);

    // DXC のバージョン文字列（例 "1.8.2502.8"）。ディスクキャッシュのキーに混ぜる。取れなければ "unknown"。
    std::string GetVersionString() const;

private:
    Microsoft::WRL::ComPtr<IDxcUtils>            m_utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3>        m_compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler>   m_defaultIncludeHandler;
    bool m_initialized = false;
};

} // namespace dx12e
