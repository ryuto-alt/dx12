#include "resource/ShaderManager.h"
#include "resource/ShaderRegistry.h"
#include "core/Logger.h"
#include "core/PathResolver.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <unordered_set>

namespace dx12e
{
namespace
{

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string Utf8FromPath(const std::filesystem::path& p)
{
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

} // namespace

ShaderManager* ShaderManager::s_instance = nullptr;

ShaderManager* ShaderManager::Instance() { return s_instance; }

void ShaderManager::SetInstance(ShaderManager* instance) { s_instance = instance; }

std::wstring ShaderManager::LowerCsoName(const std::wstring& csoPathOrName)
{
    std::filesystem::path p(csoPathOrName);
    std::wstring name = p.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return name;
}

std::string ShaderManager::LowerRelPath(const std::string& relPath)
{
    std::string out = relPath;
    for (char& c : out)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::wstring ShaderManager::ResolveSourcePath(const std::string& relPath) const
{
    std::wstring projPath = PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()) +
                             PathResolver::Utf8ToWide(relPath);
    std::error_code ec;
    if (std::filesystem::exists(projPath, ec) && !ec)
        return projPath;
    return PathResolver::ShaderSourceDirW() + PathResolver::Utf8ToWide(relPath);
}

std::vector<std::wstring> ShaderManager::IncludeDirsFor(const std::string& relPath) const
{
    namespace fs = std::filesystem;
    fs::path rp(relPath);
    fs::path subdir = rp.has_parent_path() ? rp.parent_path() : fs::path();
    std::wstring subdirW = subdir.empty() ? L"" : (subdir.generic_wstring() + L"/");

    std::vector<std::wstring> dirs;
    dirs.push_back(PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()) + subdirW);
    dirs.push_back(PathResolver::ShaderSourceDirW() + subdirW);
    return dirs;
}

bool ShaderManager::CompileSourceAllVariants(const ShaderSource& src, const std::wstring& hlslAbsPath)
{
    std::vector<std::wstring> includeDirs = IncludeDirsFor(src.relPath);
    std::unordered_map<std::wstring, std::vector<u8>> compiled;

    for (const auto& variant : src.variants)
    {
        ShaderRuntimeCompiler::CompileRequest req;
        req.hlslPath = hlslAbsPath;
        req.entry    = variant.entry;
        req.profile  = variant.profile;
        if (variant.define)
            req.defines.push_back(variant.define);
        req.includeDirs = includeDirs;

        ShaderRuntimeCompiler::CompileResult result = m_compiler.Compile(req);
        if (!result.success)
        {
            Logger::Error("シェーダーホットリロード失敗: {} -> {}\n{}", src.relPath,
                          WideToUtf8(variant.csoName), result.errorLog);
            return false;
        }
        compiled[LowerCsoName(variant.csoName)] = std::move(result.dxil);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [name, bytes] : compiled)
            m_overrides[name] = std::move(bytes);
    }
    Logger::Info("シェーダーホットリロード成功: {}", src.relPath);
    return true;
}

void ShaderManager::EstablishBaseline()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::wstring projRootW = PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir());

    for (const auto& src : AllShaderSources())
    {
        std::wstring resolved = ResolveSourcePath(src.relPath);
        ec.clear();
        auto mtime = fs::last_write_time(resolved, ec);
        if (ec)
            continue;  // 解決先が存在しない(異常系)。監視対象外のまま。

        const bool isProjectOverride = resolved.rfind(projRootW, 0) == 0;
        if (isProjectOverride)
            CompileSourceAllVariants(src, resolved);  // 失敗しても Logger::Error 済み。baked .cso のまま続行。

        m_watchedSources[LowerRelPath(src.relPath)] = { resolved, mtime };

        for (const char* dep : src.staticDeps)
        {
            std::string depKey = LowerRelPath(dep);
            if (m_watchedDeps.count(depKey))
                continue;
            std::wstring depResolved = ResolveSourcePath(dep);
            ec.clear();
            auto depMtime = fs::last_write_time(depResolved, ec);
            if (ec)
                continue;
            m_watchedDeps[depKey] = { depResolved, depMtime };
        }
    }

    ScanCustomShaders(nullptr);
}

void ShaderManager::Initialize()
{
    m_available = m_compiler.Initialize();
    if (!m_available)
        return;
    EstablishBaseline();
}

void ShaderManager::OnProjectRootChanged()
{
    if (!m_available)
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overrides.clear();
        m_customs.clear();
    }
    m_watchedSources.clear();
    m_watchedDeps.clear();
    m_customMTimes.clear();

    EstablishBaseline();
}

void ShaderManager::ScanCustomShaders(std::vector<std::wstring>* changedOut)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root(PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()));
    if (!fs::exists(root, ec) || ec)
    {
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec))
    {
        std::error_code fec;
        const fs::directory_entry entry = *it;
        if (!entry.is_regular_file(fec) || fec)
            continue;
        if (entry.path().extension() != L".hlsl")
            continue;

        fs::path relFs = fs::relative(entry.path(), root, fec);
        if (fec)
            continue;
        std::string relPath = Utf8FromPath(relFs);

        if (FindShaderSourceByRelPath(relPath) != nullptr)
            continue;  // レジストリ一致 = オーバーライド(通常経路で処理済み)

        auto mtime = fs::last_write_time(entry.path(), fec);
        if (fec)
            continue;

        const std::string key = LowerRelPath(relPath);
        auto found = m_customMTimes.find(key);
        const bool isNew     = (found == m_customMTimes.end());
        const bool isChanged = !isNew && found->second != mtime;
        if (isNew || isChanged)
        {
            m_customMTimes[key] = mtime;
            if (CompileCustomShader(relPath) && changedOut)
                changedOut->push_back(L"custom:" + PathResolver::Utf8ToWide(key));
        }
    }
}

std::vector<std::wstring> ShaderManager::Poll()
{
    std::vector<std::wstring> changedCsoKeys;
    if (!m_available)
        return changedCsoKeys;

    namespace fs = std::filesystem;
    std::error_code ec;
    std::unordered_set<std::string> dirtySrc;

    // 1. 依存(.hlsli)の変化 → 影響ソースを dirty 化
    for (auto& [depKeyLower, watched] : m_watchedDeps)
    {
        ec.clear();
        std::wstring resolved = ResolveSourcePath(depKeyLower);
        auto mtime = fs::last_write_time(resolved, ec);
        if (ec)
            continue;
        if (watched.resolvedPath == resolved && watched.mtime == mtime)
            continue;
        watched.resolvedPath = resolved;
        watched.mtime        = mtime;
        for (const ShaderSource* s : FindShaderSourcesByStaticDep(depKeyLower))
            dirtySrc.insert(LowerRelPath(s->relPath));
    }

    // 2. ソース自体の変化(オーバーライド追加/削除/編集)を検出
    for (auto& [relKeyLower, watched] : m_watchedSources)
    {
        ec.clear();
        std::wstring resolved = ResolveSourcePath(relKeyLower);
        auto mtime = fs::last_write_time(resolved, ec);
        if (ec)
            continue;  // 解決先が消えた(理論上起きない)
        if (watched.resolvedPath != resolved || watched.mtime != mtime)
        {
            watched.resolvedPath = resolved;
            watched.mtime        = mtime;
            dirtySrc.insert(relKeyLower);
        }
    }

    // 3. dirty ソースを再コンパイル
    for (const auto& relKeyLower : dirtySrc)
    {
        const ShaderSource* src = FindShaderSourceByRelPath(relKeyLower);
        if (!src)
            continue;
        auto it = m_watchedSources.find(relKeyLower);
        if (it == m_watchedSources.end())
            continue;
        if (CompileSourceAllVariants(*src, it->second.resolvedPath))
        {
            for (const auto& variant : src->variants)
                changedCsoKeys.push_back(LowerCsoName(variant.csoName));
        }
    }

    // 4. カスタムシェーダー(Registry外)の新規/変更を検出
    ScanCustomShaders(&changedCsoKeys);

    return changedCsoKeys;
}

void ShaderManager::RegisterReloadHandler(std::initializer_list<const wchar_t*> csoNames,
                                           std::function<void()> handler)
{
    const size_t idx = m_handlers.size();
    m_handlers.push_back(std::move(handler));
    for (const wchar_t* name : csoNames)
        m_handlersByCso[LowerCsoName(name)].push_back(idx);
}

void ShaderManager::RegisterCustomReloadHandler(std::function<void(const std::string&)> handler)
{
    m_customHandlers.push_back(std::move(handler));
}

void ShaderManager::DispatchReloadHandlers(const std::vector<std::wstring>& changedKeys)
{
    std::unordered_set<size_t> firedHandlers;
    for (const auto& key : changedKeys)
    {
        if (key.rfind(L"custom:", 0) == 0)
        {
            const std::string relKey = WideToUtf8(key.substr(7));
            for (auto& h : m_customHandlers)
                h(relKey);
            continue;
        }
        auto it = m_handlersByCso.find(key);
        if (it == m_handlersByCso.end())
            continue;
        for (size_t idx : it->second)
            firedHandlers.insert(idx);
    }
    for (size_t idx : firedHandlers)
        m_handlers[idx]();
}

bool ShaderManager::RecompileAllForBuild(std::vector<std::string>* errorsOut)
{
    if (!m_available)
    {
        if (errorsOut)
            errorsOut->push_back("シェーダーの実行時コンパイルが利用できません");
        return false;
    }

    namespace fs = std::filesystem;
    bool ok = true;

    // 1) Registry 一致のプロジェクトオーバーライドを全部再コンパイル
    for (const auto& src : AllShaderSources())
    {
        std::wstring projPath = PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()) +
                                 PathResolver::Utf8ToWide(src.relPath);
        std::error_code ec;
        if (!fs::exists(projPath, ec))
            continue;  // オーバーライド無し = baked .cso のまま出荷
        if (!CompileSourceAllVariants(src, projPath))
        {
            ok = false;
            if (errorsOut)
                errorsOut->push_back(src.relPath);
        }
    }

    // 2) カスタムシェーダー(Registry外)を全部再コンパイル(通常の ScanCustomShaders は差分のみだが、
    //    ビルド時は「今の内容」を確実に反映するため無条件で回す)
    {
        std::error_code ec;
        fs::path root(PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()));
        if (fs::exists(root, ec))
        {
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec)
                    continue;
                if (it->path().extension() != L".hlsl")
                    continue;
                fs::path rel = fs::relative(it->path(), root, fec);
                if (fec)
                    continue;
                std::string relPath = Utf8FromPath(rel);
                if (FindShaderSourceByRelPath(relPath) != nullptr)
                    continue;  // Registry一致 = オーバーライド(上で処理済み)
                if (!CompileCustomShader(relPath))
                {
                    ok = false;
                    if (errorsOut)
                        errorsOut->push_back(relPath);
                }
            }
        }
    }

    return ok;
}

std::vector<std::string> ShaderManager::AllValidCustomRelPaths() const
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [relKey, entry] : m_customs)
        if (entry.valid)
            out.push_back(relKey);
    return out;
}

std::vector<std::wstring> ShaderManager::AllKnownReloadKeys() const
{
    std::vector<std::wstring> keys;
    keys.reserve(m_handlersByCso.size() + m_customs.size());
    for (const auto& [cso, idxs] : m_handlersByCso)
        keys.push_back(cso);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [relKey, entry] : m_customs)
            keys.push_back(L"custom:" + PathResolver::Utf8ToWide(relKey));
    }
    return keys;
}

const std::vector<u8>* ShaderManager::TryGetOverride(const std::wstring& csoPath) const
{
    const std::wstring name = LowerCsoName(csoPath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_overrides.find(name);
    return it != m_overrides.end() ? &it->second : nullptr;
}

bool ShaderManager::CompileCustomShader(const std::string& relPath)
{
    if (!m_available)
        return false;

    const std::wstring hlslPath = PathResolver::Utf8ToWide(PathResolver::ProjectShaderDir()) +
                                   PathResolver::Utf8ToWide(relPath);
    const std::vector<std::wstring> includeDirs = IncludeDirsFor(relPath);

    ShaderRuntimeCompiler::CompileRequest vsReq;
    vsReq.hlslPath    = hlslPath;
    vsReq.entry       = L"VSMain";
    vsReq.profile     = L"vs_6_0";
    vsReq.includeDirs = includeDirs;
    ShaderRuntimeCompiler::CompileResult vsResult = m_compiler.Compile(vsReq);

    ShaderRuntimeCompiler::CompileRequest psReq;
    psReq.hlslPath    = hlslPath;
    psReq.entry       = L"PSMain";
    psReq.profile     = L"ps_6_0";
    psReq.includeDirs = includeDirs;
    ShaderRuntimeCompiler::CompileResult psResult = m_compiler.Compile(psReq);

    const std::string key = LowerRelPath(relPath);
    std::lock_guard<std::mutex> lock(m_mutex);
    CustomEntry& entry = m_customs[key];
    if (vsResult.success && psResult.success)
    {
        entry.vs    = std::move(vsResult.dxil);
        entry.ps    = std::move(psResult.dxil);
        entry.valid = true;
        entry.errorLog.clear();
        Logger::Info("カスタムシェーダーのコンパイルに成功しました: {}", relPath);
        return true;
    }

    entry.valid    = false;
    entry.errorLog = !vsResult.success ? vsResult.errorLog : psResult.errorLog;
    Logger::Error("カスタムシェーダーのコンパイルに失敗しました: {}\n{}", relPath, entry.errorLog);
    return false;
}

const std::vector<u8>* ShaderManager::GetCustomVsBytecode(const std::string& relPath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_customs.find(LowerRelPath(relPath));
    if (it == m_customs.end() || !it->second.valid)
        return nullptr;
    return &it->second.vs;
}

const std::vector<u8>* ShaderManager::GetCustomPsBytecode(const std::string& relPath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_customs.find(LowerRelPath(relPath));
    if (it == m_customs.end() || !it->second.valid)
        return nullptr;
    return &it->second.ps;
}

bool ShaderManager::HasValidCustomShader(const std::string& relPath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_customs.find(LowerRelPath(relPath));
    return it != m_customs.end() && it->second.valid;
}

std::string ShaderManager::GetCustomShaderError(const std::string& relPath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_customs.find(LowerRelPath(relPath));
    return it != m_customs.end() ? it->second.errorLog : std::string();
}

} // namespace dx12e
