#include "resource/ShaderRuntimeCompiler.h"
#include "core/Logger.h"

#include <dxcapi.h>

using Microsoft::WRL::ComPtr;

namespace dx12e
{
namespace
{

// IDxcIncludeHandler をラップし、実際に読まれた include の絶対パスを記録するだけの薄いプロキシ。
// Compile() のスタック上に置いて使う(ヒープ確保・delete 不要。AddRef/Release は参照カウントを
// 積むだけの no-op で、DXC 側が Release 過多で解放しようとしてもクラッシュしない)。
class RecordingIncludeHandler : public IDxcIncludeHandler
{
public:
    explicit RecordingIncludeHandler(IDxcIncludeHandler* inner) : m_inner(inner) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override
    {
        HRESULT hr = m_inner->LoadSource(pFilename, ppIncludeSource);
        if (SUCCEEDED(hr) && ppIncludeSource && *ppIncludeSource)
            includedFiles.push_back(pFilename);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject)
            return E_POINTER;
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown))
        {
            *ppvObject = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { return m_refs > 0 ? --m_refs : 0; }

    std::vector<std::wstring> includedFiles;

private:
    IDxcIncludeHandler* m_inner;
    ULONG m_refs = 1;
};

} // namespace

ShaderRuntimeCompiler::ShaderRuntimeCompiler() = default;
ShaderRuntimeCompiler::~ShaderRuntimeCompiler() = default;

bool ShaderRuntimeCompiler::Initialize()
{
    if (m_initialized)
        return true;

    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils))))
    {
        Logger::Error("ShaderRuntimeCompiler: IDxcUtils の生成に失敗しました(実行時シェーダーコンパイルは無効)");
        return false;
    }
    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler))))
    {
        Logger::Error("ShaderRuntimeCompiler: IDxcCompiler3 の生成に失敗しました(実行時シェーダーコンパイルは無効)");
        return false;
    }
    if (FAILED(m_utils->CreateDefaultIncludeHandler(&m_defaultIncludeHandler)))
    {
        Logger::Error("ShaderRuntimeCompiler: デフォルト include ハンドラの生成に失敗しました");
        return false;
    }

    m_initialized = true;
    return true;
}

ShaderRuntimeCompiler::CompileResult ShaderRuntimeCompiler::Compile(const CompileRequest& req)
{
    CompileResult out;
    if (!m_initialized)
    {
        out.errorLog = "ShaderRuntimeCompiler が初期化されていません";
        return out;
    }

    ComPtr<IDxcBlobEncoding> source;
    HRESULT hr = m_utils->LoadFile(req.hlslPath.c_str(), nullptr, &source);
    if (FAILED(hr) || !source)
    {
        out.errorLog = "ソースファイルを開けません";
        return out;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr      = source->GetBufferPointer();
    sourceBuffer.Size     = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    // 引数の実体(wstring)は Compile() 呼び出しが終わるまで生存させる必要がある。
    std::vector<std::wstring> argStorage;
    argStorage.push_back(req.hlslPath);   // args[0] はエラーメッセージ用のファイル名表示にのみ使われる
    argStorage.push_back(L"-E");
    argStorage.push_back(req.entry);
    argStorage.push_back(L"-T");
    argStorage.push_back(req.profile);
    for (const auto& def : req.defines)
    {
        argStorage.push_back(L"-D");
        argStorage.push_back(def);
    }
    for (const auto& dir : req.includeDirs)
    {
        argStorage.push_back(L"-I");
        argStorage.push_back(dir);
    }

    std::vector<LPCWSTR> args;
    args.reserve(argStorage.size());
    for (const auto& a : argStorage)
        args.push_back(a.c_str());

    RecordingIncludeHandler includeHandler(m_defaultIncludeHandler.Get());

    ComPtr<IDxcResult> result;
    hr = m_compiler->Compile(&sourceBuffer, args.data(), static_cast<UINT32>(args.size()),
                              &includeHandler, IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result)
    {
        out.errorLog = "IDxcCompiler3::Compile の呼び出しに失敗しました";
        return out;
    }

    out.includedFiles = std::move(includeHandler.includedFiles);

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
        out.errorLog.assign(errors->GetStringPointer(), errors->GetStringLength());

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        out.success = false;
        if (out.errorLog.empty())
            out.errorLog = "コンパイルに失敗しました(詳細不明)";
        return out;
    }

    ComPtr<IDxcBlob> objBlob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objBlob), nullptr);
    if (!objBlob || objBlob->GetBufferSize() == 0)
    {
        out.success = false;
        if (out.errorLog.empty())
            out.errorLog = "コンパイルは成功しましたが出力バイトコードが空です";
        return out;
    }

    const auto* bytes = static_cast<const u8*>(objBlob->GetBufferPointer());
    out.dxil.assign(bytes, bytes + objBlob->GetBufferSize());
    out.success = true;
    return out;
}

} // namespace dx12e
