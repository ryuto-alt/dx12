// MCP ファイル書き込みジャーナル（設計と形式は McpJournal.h の冒頭）。標準ライブラリだけ。
#include "core/mcp/McpJournal.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace dx12e
{
namespace mcpjournal
{

// ---------------------------------------------------------------------------
// パス
// ---------------------------------------------------------------------------
fs::path PathFromUtf8(const std::string& s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string Utf8FromPath(const fs::path& p)
{
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

namespace
{
// ---------------------------------------------------------------------------
// ごく小さな JSON（manifest.json 専用。TS の JSON.stringify が書いたものも読める）
// ---------------------------------------------------------------------------
struct JVal
{
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool                         b = false;
    double                       n = 0.0;
    std::string                  s;
    std::vector<JVal>            a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* Get(const char* key) const
    {
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string Str(const char* key, const std::string& dflt = {}) const
    {
        const JVal* v = Get(key);
        return (v && v->t == T::Str) ? v->s : dflt;
    }
    bool Bool(const char* key, bool dflt) const
    {
        const JVal* v = Get(key);
        return (v && v->t == T::Bool) ? v->b : dflt;
    }
    double Num(const char* key, double dflt) const
    {
        const JVal* v = Get(key);
        return (v && v->t == T::Num) ? v->n : dflt;
    }
};

struct JParser
{
    const std::string& s;
    size_t             i = 0;
    bool               ok = true;

    explicit JParser(const std::string& str) : s(str) {}

    void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }

    static void AppendUtf8(std::string& o, unsigned cp)
    {
        if (cp < 0x80)         { o += static_cast<char>(cp); }
        else if (cp < 0x800)   { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else                   { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                                 o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    unsigned Hex4()
    {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k)
        {
            if (i >= s.size()) { ok = false; return 0; }
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else { ok = false; return 0; }
        }
        return v;
    }

    std::string Str()
    {
        std::string o;
        if (i >= s.size() || s[i] != '"') { ok = false; return o; }
        ++i;
        while (i < s.size() && s[i] != '"')
        {
            char c = s[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) { ok = false; return o; }
            const char e = s[i++];
            switch (e)
            {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case '/': o += '/'; break;
            case '\\': o += '\\'; break;
            case '"': o += '"'; break;
            case 'u':
            {
                unsigned cp = Hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    i += 2;
                    const unsigned lo = Hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(o, cp);
                break;
            }
            default: ok = false; return o;
            }
        }
        if (i >= s.size()) { ok = false; return o; }
        ++i;   // 閉じの "
        return o;
    }

    JVal Value(int depth = 0)
    {
        JVal v;
        if (depth > 32) { ok = false; return v; }
        Ws();
        if (i >= s.size()) { ok = false; return v; }
        const char c = s[i];
        if (c == '{')
        {
            v.t = JVal::T::Obj;
            ++i; Ws();
            if (i < s.size() && s[i] == '}') { ++i; return v; }
            while (ok)
            {
                Ws();
                std::string k = Str();
                Ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                ++i;
                JVal x = Value(depth + 1);
                v.o.emplace_back(std::move(k), std::move(x));
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false;
            }
        }
        else if (c == '[')
        {
            v.t = JVal::T::Arr;
            ++i; Ws();
            if (i < s.size() && s[i] == ']') { ++i; return v; }
            while (ok)
            {
                v.a.push_back(Value(depth + 1));
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false;
            }
        }
        else if (c == '"') { v.t = JVal::T::Str; v.s = Str(); }
        else if (s.compare(i, 4, "true") == 0)  { v.t = JVal::T::Bool; v.b = true;  i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.t = JVal::T::Bool; v.b = false; i += 5; }
        else if (s.compare(i, 4, "null") == 0)  { v.t = JVal::T::Null; i += 4; }
        else
        {
            const size_t st = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                    s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
            if (i == st) { ok = false; return v; }
            v.t = JVal::T::Num;
            v.n = std::strtod(s.substr(st, i - st).c_str(), nullptr);
        }
        return v;
    }
};

std::string Quote(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c)); o += b; }
            else o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

bool ReadAll(const fs::path& p, std::string& out)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

bool WriteAll(const fs::path& p, const std::string& data)
{
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(ofs);
}

bool SameContent(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536], bb[65536];
    for (;;)
    {
        fa.read(ba, sizeof(ba));
        fb.read(bb, sizeof(bb));
        const auto na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) return true;
        if (std::memcmp(ba, bb, static_cast<size_t>(na)) != 0) return false;
    }
}

bool IsEntryDirName(const std::string& n)
{
    if (n.size() < 8 || n[6] != '-') return false;
    for (int k = 0; k < 6; ++k) if (n[static_cast<size_t>(k)] < '0' || n[static_cast<size_t>(k)] > '9') return false;
    return true;
}

std::string SafeMethod(const std::string& m)
{
    std::string o;
    for (char c : m) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return o.empty() ? std::string("call") : o;
}

EntryInfo ParseManifest(const std::string& text, bool& ok)
{
    EntryInfo e;
    JParser p(text);
    const JVal v = p.Value();
    ok = p.ok && v.t == JVal::T::Obj;
    if (!ok) return e;
    e.id        = v.Str("id");
    e.method    = v.Str("method");
    e.label     = v.Str("label");
    e.state     = v.Str("state", "committed");
    e.createdAt = static_cast<int64_t>(v.Num("createdAt", 0));
    e.complete  = v.Bool("complete", true);
    e.note      = v.Str("note");
    if (const JVal* tx = v.Get("txLabel"); tx && tx->t == JVal::T::Str) { e.hasTx = true; e.txLabel = tx->s; }
    if (const JVal* files = v.Get("files"); files && files->t == JVal::T::Arr)
    {
        for (const JVal& f : files->a)
        {
            if (f.t != JVal::T::Obj) continue;
            FileRec r;
            r.path    = f.Str("path");
            r.existed = f.Bool("existed", false);
            r.backup  = f.Str("backup");
            r.bytes   = static_cast<uint64_t>(f.Num("bytes", 0));
            r.skipped = f.Str("skipped");
            if (!r.path.empty()) e.files.push_back(std::move(r));
        }
    }
    ok = !e.id.empty();
    return e;
}
} // namespace

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------
struct Journal::Entry
{
    EntryInfo   info;
    bool        materialized = false;
    fs::path    dir;
    int         nextBackup = 0;
};

Journal::Journal() = default;
Journal::~Journal() = default;

int64_t Journal::Now() const
{
    if (m_clock) return m_clock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Journal::SetBaseDir(const fs::path& baseDir)
{
    Configure(baseDir, baseDir / ".dx12" / "journal");
}

void Journal::Configure(const fs::path& baseDir, const fs::path& journalRoot)
{
    if (m_base == baseDir && m_root == journalRoot) return;
    m_base = baseDir;
    m_root = journalRoot;
    m_lastSeq = 0;
}

std::string Journal::RelPath(const fs::path& absPath) const
{
    std::error_code ec;
    const fs::path abs = fs::absolute(absPath, ec).lexically_normal();
    if (!m_base.empty())
    {
        const fs::path base = fs::absolute(m_base, ec).lexically_normal();
        const fs::path rel = abs.lexically_relative(base);
        if (!rel.empty() && *rel.begin() != fs::path("..") && !rel.is_absolute())
            return Utf8FromPath(rel);
    }
    return Utf8FromPath(abs);
}

fs::path Journal::Resolve(const std::string& p) const
{
    fs::path fp = PathFromUtf8(p);
    if (fp.is_absolute()) return fp;
    return m_base / fp;
}

bool Journal::BeginCall(const std::string& method, const std::string& label)
{
    if (m_tx) return false;
    if (m_cur) EndCall();
    m_cur = std::make_unique<Entry>();
    m_cur->info.method = method;
    m_cur->info.label  = label;
    m_cur->info.state  = "open";
    return true;
}

void Journal::BeginTx(const std::string& label)
{
    if (m_tx) CommitTx();
    if (m_cur) EndCall();
    m_tx = std::make_unique<Entry>();
    m_tx->info.method  = "transaction";
    m_tx->info.label   = label;
    m_tx->info.hasTx   = true;
    m_tx->info.txLabel = label;
    m_tx->info.state   = "open";
}

std::string Journal::TxLabel() const { return m_tx ? m_tx->info.txLabel : std::string(); }

bool Journal::Materialize(Entry& e)
{
    if (e.materialized) return true;
    if (m_root.empty()) return false;
    std::error_code ec;
    fs::create_directories(m_root, ec);
    if (ec) return false;
    uint32_t seq = m_lastSeq;
    for (const auto& de : fs::directory_iterator(m_root, ec))
    {
        const std::string n = Utf8FromPath(de.path().filename());
        if (IsEntryDirName(n)) seq = (std::max)(seq, static_cast<uint32_t>(std::strtoul(n.substr(0, 6).c_str(), nullptr, 10)));
    }
    ++seq;
    m_lastSeq = seq;
    char idb[16];
    std::snprintf(idb, sizeof(idb), "%06u", seq);
    e.info.id        = std::string(idb) + "-" + SafeMethod(e.info.method);
    e.info.createdAt = Now();
    e.dir            = m_root / PathFromUtf8(e.info.id);
    fs::create_directories(e.dir / "files", ec);
    if (ec) return false;
    e.materialized = true;
    WriteManifest(e);
    return true;
}

void Journal::WriteManifest(const Entry& e) const
{
    if (!e.materialized) return;
    const EntryInfo& i = e.info;
    std::string o = "{\"version\":1";
    o += ",\"id\":" + Quote(i.id);
    o += ",\"method\":" + Quote(i.method);
    o += ",\"label\":" + Quote(i.label);
    o += ",\"createdAt\":" + std::to_string(i.createdAt);
    o += ",\"state\":" + Quote(i.state);
    o += ",\"txLabel\":" + (i.hasTx ? Quote(i.txLabel) : std::string("null"));
    o += std::string(",\"complete\":") + (i.complete ? "true" : "false");
    if (!i.note.empty()) o += ",\"note\":" + Quote(i.note);
    o += ",\"files\":[";
    for (size_t k = 0; k < i.files.size(); ++k)
    {
        const FileRec& f = i.files[k];
        if (k) o += ',';
        o += "{\"path\":" + Quote(f.path);
        o += std::string(",\"existed\":") + (f.existed ? "true" : "false");
        o += ",\"backup\":" + (f.backup.empty() ? std::string("null") : Quote(f.backup));
        o += ",\"bytes\":" + std::to_string(f.bytes);
        o += ",\"skipped\":" + (f.skipped.empty() ? std::string("null") : Quote(f.skipped)) + "}";
    }
    o += "]}\n";
    const fs::path tmp = e.dir / "manifest.json.tmp";
    if (WriteAll(tmp, o))
    {
        std::error_code ec;
        fs::rename(tmp, e.dir / "manifest.json", ec);
        if (ec) WriteAll(e.dir / "manifest.json", o);
    }
}

bool Journal::BackupInto(Entry& e, const fs::path& absPath)
{
    const std::string rel = RelPath(absPath);
    for (const FileRec& f : e.info.files) if (f.path == rel) return true;   // 1 エントリで 1 回だけ
    if (!Materialize(e)) return false;

    FileRec rec;
    rec.path = rel;
    std::error_code ec;
    if (fs::is_regular_file(absPath, ec))
    {
        rec.existed = true;
        rec.bytes   = static_cast<uint64_t>(fs::file_size(absPath, ec));
        if (rec.bytes > kMaxFileBytes)
        {
            rec.skipped = "too_large";
            e.info.complete = false;
        }
        else
        {
            const std::string name = "files/" + std::to_string(e.nextBackup++) + ".bin";
            fs::copy_file(absPath, e.dir / PathFromUtf8(name), fs::copy_options::overwrite_existing, ec);
            if (ec) { rec.skipped = "copy_failed"; e.info.complete = false; }
            else rec.backup = name;
        }
    }
    else if (fs::exists(absPath, ec))
    {
        // ディレクトリなど通常ファイル以外。退避できない。
        rec.existed = true;
        rec.skipped = "not_regular_file";
        e.info.complete = false;
    }
    else
    {
        rec.existed = false;   // 新規作成される: 戻すときは消す
    }
    e.info.files.push_back(std::move(rec));
    WriteManifest(e);
    return true;
}

bool Journal::Backup(const fs::path& absPath)
{
    Entry* e = m_tx ? m_tx.get() : m_cur.get();
    if (!e) return false;
    return BackupInto(*e, absPath);
}

bool Journal::BackupTree(const fs::path& absPath)
{
    Entry* e = m_tx ? m_tx.get() : m_cur.get();
    if (!e) return false;
    std::error_code ec;
    if (!fs::is_directory(absPath, ec)) return BackupInto(*e, absPath);
    size_t   count = 0;
    uint64_t bytes = 0;
    bool     ok = true;
    for (fs::recursive_directory_iterator it(absPath, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        const uint64_t sz = static_cast<uint64_t>(it->file_size(ec));
        if (count + 1 > kMaxTreeFiles || bytes + sz > kMaxTreeBytes)
        {
            e->info.complete = false;
            e->info.note += (e->info.note.empty() ? "" : " / ") + std::string("ディレクトリの退避が上限(2000 ファイル / 256MB)を超えた: ") + RelPath(absPath);
            if (Materialize(*e)) WriteManifest(*e);
            return false;
        }
        ++count;
        bytes += sz;
        ok = BackupInto(*e, it->path()) && ok;
    }
    return ok;
}

void Journal::MarkIncomplete(const std::string& reason)
{
    Entry* e = m_tx ? m_tx.get() : m_cur.get();
    if (!e) return;
    e->info.complete = false;
    if (!reason.empty()) e->info.note += (e->info.note.empty() ? "" : " / ") + reason;
    if (Materialize(*e)) WriteManifest(*e);
}

std::string Journal::Finish(std::unique_ptr<Entry>& slot, const char* state)
{
    if (!slot) return {};
    std::unique_ptr<Entry> e = std::move(slot);
    if (!e->materialized) return {};   // 何も退避しなかった: フォルダは作っていない
    if (e->info.files.empty() && e->info.complete && e->info.note.empty())
    {
        std::error_code ec;
        fs::remove_all(e->dir, ec);
        return {};
    }
    e->info.state = state;
    WriteManifest(*e);
    Prune();
    return e->info.id;
}

std::string Journal::EndCall() { return Finish(m_cur, "committed"); }
std::string Journal::CommitTx() { return Finish(m_tx, "committed"); }

std::string Journal::RollbackTx(RestoreResult* out)
{
    if (!m_tx) return {};
    if (m_tx->materialized)
    {
        RestoreResult r = RestoreEntry(m_tx->info, /*backupFirst=*/false);
        if (out) *out = r;
    }
    return Finish(m_tx, "rolledBack");
}

bool Journal::Load(const std::string& id, EntryInfo& out) const
{
    if (m_root.empty() || id.empty() || id.find_first_of("/\\") != std::string::npos || id.find("..") != std::string::npos) return false;
    std::string text;
    if (!ReadAll(m_root / PathFromUtf8(id) / "manifest.json", text)) return false;
    bool ok = false;
    EntryInfo e = ParseManifest(text, ok);
    if (!ok) return false;
    e.id = id;   // フォルダ名を正とする
    out = std::move(e);
    return true;
}

std::vector<EntryInfo> Journal::List(size_t limit) const
{
    std::vector<EntryInfo> out;
    if (m_root.empty()) return out;
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& de : fs::directory_iterator(m_root, ec))
    {
        const std::string n = Utf8FromPath(de.path().filename());
        if (de.is_directory(ec) && IsEntryDirName(n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end(), std::greater<std::string>());
    for (const std::string& n : names)
    {
        if (out.size() >= limit) break;
        EntryInfo e;
        if (Load(n, e)) out.push_back(std::move(e));
    }
    return out;
}

size_t Journal::EntryCount() const
{
    if (m_root.empty()) return 0;
    std::error_code ec;
    size_t n = 0;
    for (const auto& de : fs::directory_iterator(m_root, ec))
        if (de.is_directory(ec) && IsEntryDirName(Utf8FromPath(de.path().filename()))) ++n;
    return n;
}

RestoreResult Journal::RestoreEntry(const EntryInfo& info, bool backupFirst)
{
    RestoreResult r;
    r.found    = true;
    r.complete = info.complete;
    const fs::path entryDir = m_root / PathFromUtf8(info.id);
    // 後ろから戻す（同じ path は 1 エントリに 1 回だけなので順序は本質ではないが、慣習として逆順）
    for (auto it = info.files.rbegin(); it != info.files.rend(); ++it)
    {
        const FileRec& f = *it;
        const fs::path target = Resolve(f.path);
        bool dotdot = false;
        {
            const fs::path pp = PathFromUtf8(f.path);
            if (!pp.is_absolute())
                for (const auto& part : pp)
                    if (part == "..") dotdot = true;
        }
        if (dotdot) { r.warnings.push_back("path に .. が含まれるため戻さない: " + f.path); continue; }
        {
            std::error_code ec;
            if (!f.existed)
            {
                if (!fs::exists(target, ec)) { r.unchanged.push_back(f.path); continue; }
                if (backupFirst) Backup(target);
                fs::remove(target, ec);
                if (ec) r.warnings.push_back("削除に失敗: " + f.path);
                else r.restored.push_back(f.path);
                continue;
            }
            if (!f.skipped.empty() || f.backup.empty())
            {
                r.missing.push_back(f.path);
                r.warnings.push_back("バックアップが無い(" + (f.skipped.empty() ? std::string("backup 無し") : f.skipped) + "): " + f.path);
                continue;
            }
            const fs::path src = entryDir / PathFromUtf8(f.backup);
            if (!fs::is_regular_file(src, ec)) { r.missing.push_back(f.path); r.warnings.push_back("バックアップのファイルが無い: " + f.path); continue; }
            if (fs::is_regular_file(target, ec) && SameContent(target, src)) { r.unchanged.push_back(f.path); continue; }
            if (backupFirst) Backup(target);
            fs::create_directories(target.parent_path(), ec);
            ec.clear();
            fs::copy_file(src, target, fs::copy_options::overwrite_existing, ec);
            if (ec) { r.warnings.push_back("書き戻しに失敗: " + f.path + " (" + ec.message() + ")"); r.missing.push_back(f.path); }
            else r.restored.push_back(f.path);
        }
    }
    if (!info.complete) r.warnings.push_back("元のエントリは不完全(退避できなかったファイルがある)。戻せる範囲だけ戻した");
    return r;
}

RestoreResult Journal::Restore(const std::string& id, bool backupFirst)
{
    EntryInfo info;
    if (!Load(id, info)) return RestoreResult{};
    RestoreResult r = RestoreEntry(info, backupFirst);
    // 元のエントリの印を更新（不完全でない・全部戻せたときだけ restored）
    if (r.missing.empty() && info.state != "open")
    {
        Entry tmp;
        tmp.info = info;
        tmp.info.state = "restored";
        tmp.materialized = true;
        tmp.dir = m_root / PathFromUtf8(id);
        WriteManifest(tmp);
    }
    return r;
}

void Journal::Prune()
{
    if (m_root.empty()) return;
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& de : fs::directory_iterator(m_root, ec))
    {
        const std::string n = Utf8FromPath(de.path().filename());
        if (de.is_directory(ec) && IsEntryDirName(n)) names.push_back(n);
    }
    if (names.size() <= m_keep) return;
    std::sort(names.begin(), names.end(), std::greater<std::string>());   // 新しい順
    size_t kept = 0;
    const int64_t now = Now();
    for (const std::string& n : names)
    {
        // 今このプロセスが開いているエントリは刈らない
        if ((m_cur && m_cur->info.id == n) || (m_tx && m_tx->info.id == n)) { ++kept; continue; }
        EntryInfo e;
        const bool ok = Load(n, e);
        // open は基本刈らない。ただし別プロセスが残した 24 時間より古い open（クラッシュの残骸）は刈る
        if (ok && e.state == "open" && now - e.createdAt < 24ll * 3600 * 1000) { ++kept; continue; }
        if (kept < m_keep) { ++kept; continue; }
        fs::remove_all(m_root / PathFromUtf8(n), ec);
    }
}

Journal& Instance()
{
    static Journal j;
    return j;
}

} // namespace mcpjournal
} // namespace dx12e
