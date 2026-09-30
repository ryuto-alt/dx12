#include "editor/EditorPrefs.h"
#include "editor/EditorPrefsStore.h"
#include "project/ProjectManager.h"
#include "project/LauncherLogic.h"   // launcher::PathFromUtf8

#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e::prefs
{
namespace
{

// テスト用の保存先（空なら本番＝ProjectManager の editor_state.json）
std::string g_testPath;

std::string LoadProd() { return ProjectManager::LoadEditorSection("prefs"); }
void        SaveProd(const std::string& text) { ProjectManager::SaveEditorSection("prefs", text); }

// テスト用: 指定ファイルを editor_state.json と同じ形（トップレベルに "prefs"）で読み書きする。他のキーは保つ。
std::string LoadFile()
{
    std::ifstream f(launcher::PathFromUtf8(g_testPath), std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("prefs") || !j["prefs"].is_object()) return {};
    return j["prefs"].dump();
}
void SaveFile(const std::string& text)
{
    nlohmann::json section = nlohmann::json::parse(text, nullptr, false);
    if (section.is_discarded() || !section.is_object()) return;
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream f(launcher::PathFromUtf8(g_testPath), std::ios::binary);
        if (f)
        {
            std::stringstream ss; ss << f.rdbuf();
            nlohmann::json old = nlohmann::json::parse(ss.str(), nullptr, false);
            if (!old.is_discarded() && old.is_object()) j = std::move(old);
        }
    }
    j["prefs"] = std::move(section);
    std::ofstream o(launcher::PathFromUtf8(g_testPath), std::ios::binary | std::ios::trunc);
    o << j.dump(2);
}

Store& S()
{
    static Store s;
    static const bool wired = [] { s.SetIO(&LoadProd, &SaveProd); return true; }();
    (void)wired;
    return s;
}

} // namespace

bool        GetBool(const char* key, bool def)                    { return S().GetBool(key, def); }
int         GetInt(const char* key, int def)                      { return S().GetInt(key, def); }
float       GetFloat(const char* key, float def)                  { return S().GetFloat(key, def); }
std::string GetString(const char* key, const std::string& def)    { return S().GetString(key, def); }
bool        Has(const char* key)                                  { return S().Has(key); }
std::vector<std::string> KeysWithPrefix(const std::string& p)     { return S().KeysWithPrefix(p); }

void SetBool(const char* key, bool v)                             { S().SetBool(key, v); }
void SetInt(const char* key, int v)                               { S().SetInt(key, v); }
void SetFloat(const char* key, float v)                           { S().SetFloat(key, v); }
void SetString(const char* key, const std::string& v)            { S().SetString(key, v); }
void Erase(const char* key)                                       { S().Erase(key); }

void Tick()                                                       { S().Tick(); }
void FlushNow()                                                   { S().Flush(); }
void SetWriteEnabled(bool on)                                     { S().SetWriteEnabled(on); }

void SetPathForTests(const std::string& utf8Path)
{
    g_testPath = utf8Path;
    if (g_testPath.empty()) S().SetIO(&LoadProd, &SaveProd);
    else                    S().SetIO(&LoadFile, &SaveFile);
}

} // namespace dx12e::prefs
