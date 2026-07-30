// ShaderRegistry の staticDeps が、.hlsl が実際に #include しているものと合っているか。
//
// ★なぜ要るか
//   ホットリロードは ShaderRegistry.cpp の**手書きの依存表**しか引かない
//   （ShaderManager::Poll → FindShaderSourcesByStaticDep）。表から漏れた .hlsli を保存しても
//   その .hlsl は再コンパイルされず、それでいて**再コンパイルできた別の .hlsl の
//   「ホットリロード成功」ログだけが出る**ので、古い DXIL のまま動いていることに気づけない。
//   実際 Forward.hlsl と ForwardSkinned.hlsl が DecalApply.hlsli を落としていて、
//   デカールのシェーダーを直しても不透明パスとスキンドパスに反映されなかった。
//
//   逆向き（表にあるが include していない）は害が「余分に再コンパイルされる」だけなので
//   エラーにしない（共通ヘッダを先回りで入れてある場合もある）。ここは**漏れだけ**を見る。
//
// エンジンをリンクせず、ShaderRegistry.cpp と shaders/ をテキストとして読む。
//
// 実行: ctest --output-on-failure -R ShaderIncludeDeps

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Fail(const std::string& msg)
{
    ++g_failures;
    std::printf("  NG  %s\n", msg.c_str());
}

std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string Lower(std::string s)
{
    for (char& c : s)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// "forward/Forward.hlsl" → "forward"
std::string DirOf(const std::string& rel)
{
    const std::size_t p = rel.rfind('/');
    return (p == std::string::npos) ? std::string() : rel.substr(0, p);
}

// #include "X.hlsli" を拾う（相対 include なので、その .hlsl のあるフォルダ基準で解決）。
std::set<std::string> DirectIncludes(const std::string& shaderRoot, const std::string& rel)
{
    std::set<std::string> out;
    const std::string src = ReadFile(shaderRoot + "/" + rel);
    if (src.empty()) return out;

    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t h = line.find("#include");
        if (h == std::string::npos) continue;
        // 行頭から #include までが空白だけ（コメントアウトを拾わない）
        if (line.find_first_not_of(" \t") != h) continue;
        const std::size_t q0 = line.find('"', h);
        if (q0 == std::string::npos) continue;
        const std::size_t q1 = line.find('"', q0 + 1);
        if (q1 == std::string::npos) continue;

        std::string inc = line.substr(q0 + 1, q1 - q0 - 1);
        if (inc.empty()) continue;
        // "../ddgi/DdgiCommon.hlsli" のような相対を畳む
        std::string joined = DirOf(rel).empty() ? inc : DirOf(rel) + "/" + inc;

        std::vector<std::string> parts;
        std::istringstream ps(joined);
        std::string seg;
        while (std::getline(ps, seg, '/'))
        {
            if (seg == "." || seg.empty()) continue;
            if (seg == ".." ) { if (!parts.empty()) parts.pop_back(); continue; }
            parts.push_back(seg);
        }
        std::string norm;
        for (std::size_t i = 0; i < parts.size(); ++i)
            norm += (i ? "/" : "") + parts[i];
        if (!norm.empty()) out.insert(Lower(norm));
    }
    return out;
}

// 推移的に辿る（DecalApply.hlsli が DecalCommon.hlsli を include している等）。
void CollectIncludes(const std::string& shaderRoot, const std::string& rel,
                     std::set<std::string>& acc)
{
    for (const auto& inc : DirectIncludes(shaderRoot, rel))
        if (acc.insert(inc).second)
            CollectIncludes(shaderRoot, inc, acc);
}

// ShaderRegistry.cpp から { "forward/X.hlsl", ... { deps... }, } を抜く。
// エントリの区切りは「先頭カラムが 8 スペースの `},`」（このファイルの整形に依存する）。
struct Entry { std::string rel; std::set<std::string> deps; };

std::vector<Entry> ParseRegistry(const std::string& src)
{
    std::vector<Entry> out;
    std::istringstream in(src);
    std::string line;
    Entry cur;
    bool inEntry = false;

    while (std::getline(in, line))
    {
        const std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        const std::string trimmed = line.substr(i);

        // エントリ終端
        if (inEntry && trimmed.rfind("},", 0) == 0 && i == 8)
        {
            if (!cur.rel.empty()) out.push_back(cur);
            cur = Entry{};
            inEntry = false;
            continue;
        }
        if (trimmed.rfind("//", 0) == 0) continue;   // コメント行は見ない

        // .hlsl のパス（エントリ先頭）
        if (!inEntry)
        {
            const std::size_t q0 = trimmed.find('"');
            if (q0 != std::string::npos && trimmed.find(".hlsl\"") != std::string::npos)
            {
                const std::size_t q1 = trimmed.find('"', q0 + 1);
                if (q1 != std::string::npos)
                {
                    cur.rel = trimmed.substr(q0 + 1, q1 - q0 - 1);
                    inEntry = true;
                }
            }
            continue;
        }
        // deps 行（.hlsli を並べた行）
        if (trimmed.find(".hlsli\"") != std::string::npos)
        {
            std::size_t p = 0;
            while ((p = trimmed.find('"', p)) != std::string::npos)
            {
                const std::size_t e = trimmed.find('"', p + 1);
                if (e == std::string::npos) break;
                const std::string tok = trimmed.substr(p + 1, e - p - 1);
                if (tok.size() > 6 && tok.compare(tok.size() - 6, 6, ".hlsli") == 0)
                    cur.deps.insert(Lower(tok));
                p = e + 1;
            }
        }
    }
    return out;
}
}

int main()
{
    const std::string regSrc     = ReadFile(DX12E_SHADER_REGISTRY_CPP);
    const std::string shaderRoot = DX12E_SHADER_DIR;
    if (regSrc.empty())
    {
        std::printf("NG: ShaderRegistry.cpp が読めない\n");
        return 1;
    }

    const auto entries = ParseRegistry(regSrc);
    if (entries.size() < 10)
    {
        std::printf("NG: 登録エントリの抽出が壊れている(%zu 件しか取れていない)\n", entries.size());
        return 1;
    }
    std::printf("登録シェーダー %zu 本を検査\n", entries.size());

    for (const auto& e : entries)
    {
        std::set<std::string> actual;
        CollectIncludes(shaderRoot, e.rel, actual);
        if (actual.empty()) continue;   // include なし or ファイルが読めない

        for (const auto& inc : actual)
        {
            ++g_checks;
            if (e.deps.count(inc) == 0)
                Fail(e.rel + " が #include している " + inc + " が staticDeps に無い"
                     "（保存しても再コンパイルされない）");
        }
    }

    if (g_failures != 0)
    {
        std::printf("\nshader_include_deps: %d チェック中 %d 件 NG\n", g_checks, g_failures);
        std::printf("→ src/resource/ShaderRegistry.cpp の該当エントリへ足すこと。\n");
        return 1;
    }
    std::printf("shader_include_deps: %d チェックすべて通過\n", g_checks);
    return 0;
}
