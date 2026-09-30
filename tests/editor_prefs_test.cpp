// エディタ設定ストア（editor/EditorPrefsStore.h）の単体テスト（フェーズ 1b W）。
//   ・往復（Set → 保存 → 別ストアで読む）/ 型違いの読み替え / 壊れた JSON / デバウンス / 同値 Set は dirty にしない
//   ・他機能が同じ editor_state.json に書いたキーを消さない（保存関数が外側の JSON へ "prefs" だけ差し込む形で検証）
// nlohmann と標準ライブラリだけ（ImGui も GPU も要らない）。
#include "editor/EditorPrefsStore.h"

#include <cmath>
#include <cstdio>
#include <string>

using dx12e::prefs::Store;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

struct Disk   // メモリ上の「editor_state.json」
{
    std::string text;
    int saves = 0;
};

void Wire(Store& s, Disk& d, double* clock)
{
    s.SetIO(
        [&d]() -> std::string {
            const auto j = nlohmann::json::parse(d.text, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("prefs")) return {};
            return j["prefs"].dump();
        },
        [&d](const std::string& t) {
            nlohmann::json root = nlohmann::json::parse(d.text, nullptr, false);
            if (root.is_discarded() || !root.is_object()) root = nlohmann::json::object();
            root["prefs"] = nlohmann::json::parse(t);   // 他のキーは保つ
            d.text = root.dump();
            ++d.saves;
        });
    s.SetClock([clock]() { return *clock; });
}

void TestRoundTrip()
{
    Disk d; double t = 0.0;
    Store a; Wire(a, d, &t);
    a.SetBool("x.b", true); a.SetInt("x.i", -7); a.SetFloat("x.f", 2.5f); a.SetString("x.s", "こんにちは");
    a.Flush();
    CHECK(d.saves == 1, "Flush で 1 回");
    Store b; Wire(b, d, &t);
    CHECK(b.GetBool("x.b", false) == true, "bool");
    CHECK(b.GetInt("x.i", 0) == -7, "int");
    CHECK(b.GetFloat("x.f", 0) == 2.5f, "float");
    CHECK(b.GetString("x.s", "") == "こんにちは", "string（日本語）");
    CHECK(b.GetInt("none", 42) == 42 && b.GetString("none", "d") == "d", "無いキーは既定値");
    CHECK(b.Has("x.i") && !b.Has("none"), "Has");
}

void TestTypeCoercion()
{
    Disk d; double t = 0.0;
    d.text = R"({"prefs":{"n":3,"f":2.6,"b":true,"s":"abc","z":0}})";
    Store s; Wire(s, d, &t);
    CHECK(s.GetBool("n", false) == true, "数値→bool（非 0）");
    CHECK(s.GetBool("z", true) == false, "0→false");
    CHECK(s.GetInt("f", 0) == 3, "小数→int は四捨五入");
    CHECK(s.GetInt("b", 0) == 1, "bool→int");
    CHECK(s.GetFloat("n", 0) == 3.0f, "int→float");
    CHECK(s.GetInt("s", 9) == 9 && s.GetBool("s", true) == true && s.GetFloat("s", 1.5f) == 1.5f, "文字列は数値に読み替えず既定値");
    CHECK(s.GetString("n", "d") == "d", "数値は string にならない");
}

void TestBroken()
{
    for (const char* bad : {"", "{", "not json", "[1,2,3]", "{\"prefs\":5}", "{\"prefs\":[1]}", "null"})
    {
        Disk d; double t = 0.0; d.text = bad;
        Store s; Wire(s, d, &t);
        CHECK(s.GetInt("k", 5) == 5, "壊れた入力でも既定値: %s", bad);
        s.SetInt("k", 6);
        s.Flush();
        Store s2; Wire(s2, d, &t);
        CHECK(s2.GetInt("k", 0) == 6, "壊れた後でも保存できて読める: %s", bad);
    }
    // ストア用 JSON そのものが壊れている（"prefs" の中身が文字列）
    Disk d; double t = 0.0;
    Store s; s.SetIO([]() { return std::string("{oops"); }, [&d](const std::string& x) { d.text = x; ++d.saves; });
    s.SetClock([&t]() { return t; });
    CHECK(s.GetBool("a", true), "ロード文字列が壊れていても落ちない");
    s.SetBool("a", false);
    s.Flush();
    CHECK(d.text.find("\"a\":false") != std::string::npos, "次の保存で正常化");
}

void TestDebounce()
{
    Disk d; double t = 100.0;
    Store s; Wire(s, d, &t);
    s.SetDebounceSeconds(1.5);
    s.SetInt("a", 1);
    CHECK(s.IsDirty(), "変更で dirty");
    s.Tick();
    CHECK(d.saves == 0, "デバウンス中は書かない");
    t += 1.0; s.SetInt("a", 2); s.Tick();   // 連続変更しても最初の dirty から数える
    CHECK(d.saves == 0, "1.0 秒では書かない");
    t += 0.6; s.Tick();
    CHECK(d.saves == 1 && !s.IsDirty(), "最初の変更から 1.5 秒で 1 回だけ書く");
    for (int i = 0; i < 100; ++i) { t += 0.1; s.Tick(); }
    CHECK(d.saves == 1, "dirty でなければ書かない（毎フレーム Tick でも I/O 無し）");
    s.SetInt("a", 2);
    CHECK(!s.IsDirty(), "同じ値の Set は dirty にしない");
    s.SetFloat("f", 0.25f); s.SetFloat("f", 0.25f);
    CHECK(s.IsDirty(), "新規キーは dirty");
    s.Flush();
    CHECK(d.saves == 2, "Flush は即時");
    s.Flush();
    CHECK(d.saves == 2, "dirty でない Flush は何もしない");
}

void TestWriteEnabled()
{
    Disk d; double t = 0.0;
    Store s; Wire(s, d, &t);
    s.SetWriteEnabled(false);
    s.SetInt("a", 1);
    s.Flush();
    CHECK(d.saves == 0, "書き込み無効の間は保存しない");
    CHECK(s.GetInt("a", 0) == 1, "メモリ上の値は変わる");
}

void TestPreserveUnknown()
{
    Disk d; double t = 0.0;
    d.text = R"({"lastOpenedScene":"scenes/a.json","startupSound":false,"prefs":{"keep.me":11,"x":1}})";
    Store s; Wire(s, d, &t);
    s.SetInt("x", 2);
    s.Flush();
    const auto j = nlohmann::json::parse(d.text);
    CHECK(j["lastOpenedScene"] == "scenes/a.json" && j["startupSound"] == false, "他機能のトップレベルのキーを消さない");
    CHECK(j["prefs"]["keep.me"] == 11, "prefs 内の未知キーも保つ");
    CHECK(j["prefs"]["x"] == 2, "更新は反映");
}

void TestKeysAndErase()
{
    Disk d; double t = 0.0;
    Store s; Wire(s, d, &t);
    s.SetString("layout.named.A", "1"); s.SetString("layout.named.B", "2"); s.SetString("other", "3");
    auto keys = s.KeysWithPrefix("layout.named.");
    CHECK(keys.size() == 2 && keys[0] == "layout.named.A" && keys[1] == "layout.named.B", "prefix 列挙");
    s.Erase("layout.named.A");
    CHECK(s.KeysWithPrefix("layout.named.").size() == 1, "Erase");
    s.Flush();
    Store s2; Wire(s2, d, &t);
    CHECK(!s2.Has("layout.named.A") && s2.Has("layout.named.B"), "Erase が保存される");
    s2.Clear();
    s2.Flush();
    Store s3; Wire(s3, d, &t);
    CHECK(!s3.Has("layout.named.B"), "Clear");
}

void TestNonFinite()
{
    Disk d; double t = 0.0;
    Store s; Wire(s, d, &t);
    s.SetFloat("nan", std::nanf(""));
    CHECK(s.GetFloat("nan", 9.0f) == 0.0f, "NaN は 0 として保存（JSON に書けない値を入れない）");
}
} // namespace

int main()
{
    TestRoundTrip();
    TestTypeCoercion();
    TestBroken();
    TestDebounce();
    TestWriteEnabled();
    TestPreserveUnknown();
    TestKeysAndErase();
    TestNonFinite();
    std::printf("EditorPrefsTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
