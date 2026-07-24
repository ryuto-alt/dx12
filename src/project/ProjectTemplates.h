#pragma once

#include <string>
#include <vector>

namespace dx12e::templates
{

// 新規プロジェクト作成時に書き出す 1 ファイル。relPath はプロジェクトルートからの相対パス。
struct TemplateFile
{
    const char* relPath;
    const char* content;
};

// テンプレート ID ("empty" / "fps" / "tps" / "2d") に対応するファイル一覧を返す。
// 未知の ID は "empty" として扱う。
const std::vector<TemplateFile>& GetFiles(const std::string& templateId);

} // namespace dx12e::templates
