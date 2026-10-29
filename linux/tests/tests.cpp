// tests.cpp - проверки ygg-control (Linux) без сети и без Yggdrasil.
// Сборка и запуск: ./tests/run_tests.sh (пишет файлы только во временную папку).
#define YGG_CONTROL_NO_MAIN
#include "../src/panel/main.cpp"

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else { ++g_fail; printf("  ПРОВАЛ: %s\n", what.c_str()); }
}
static void Eq(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what + ": получено «" + got + "», ожидалось «" + want + "»");
}
static void Section(const char* n) { printf("- %s\n", n); }

static void TestNames() {
    Section("имена и адреса");
    Eq(pnl::ShortName("tls://ygg-msk-1.example.ru:443"), "ygg-msk-1", "имя пира из адреса");
    Eq(pnl::ShortName("tcp://ygg.kjele.net:8363"), "kjele", "общее слово ygg пропускается");
    Eq(pnl::ShortName("tls://1.2.3.4:443"), "1.2.3.4", "IP остаётся IP");
    Eq(pnl::ShortName("tls://[2001:db8::1]:443"), "2001:db8::1", "IPv6");
    Eq(pnl::ShortKey(std::string(64, 'a')), "aaaa…aaaa", "ключ коротко");
    Eq(pnl::NormAddr("200:0000:0000::0001"), "200::1", "адрес приводится к одному виду");
    Check(pnl::NormAddr("200::1") == pnl::NormAddr("200:0:0:0:0:0:0:1"), "разные записи одного адреса совпадают");
    Eq(pnl::TinyAddr("201:1f2a:1:2:3:4:5:e0"), "201:1f2a:…:e0", "адрес в дереве коротко");
    Check(pnl::IsHexKey(std::string(64, 'f')) && !pnl::IsHexKey("abc") && !pnl::IsHexKey(std::string(63, 'f') + "z"), "ключ узла - 64 hex");
}

static void TestUnits() {
    Section("единицы");
    Eq(pnl::Bytes(512), "512 Б", "байты");
    Eq(pnl::Bytes(1536), "1,5 КБ", "килобайты");
    Eq(pnl::Bytes(15.0 * 1024 * 1024), "15 МБ", "мегабайты без дробной части");
    Eq(pnl::DurShort(5400), "1 ч 30 м", "время коротко");
    Eq(pnl::DurShort(90000), "1 д 1 ч", "сутки");
    Eq(pnl::DurShort(45), "45 с", "секунды");
    Check(pnl::NiceTop(48) == 50 && pnl::NiceTop(130) == 200 && pnl::NiceTop(0.7) == 1, "круглый верх шкалы");
}

static void TestBook() {
    Section("адресная книга");
    char tmpl[] = "/tmp/yggctl-test-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string path = dir + "/addressbook.txt";
    util::WriteFile(path, "# комментарий\nThinkPad\t200::1\nсломанная строка\nKjele\t201:aa::2\told_third_column\n");
    // LoadBook читает рядом с программой - подменяем путь через копию функции разбора
    std::vector<pnl::BookEntry> out;
    for (const auto& l : util::Lines(util::ReadFile(path))) {
        std::string t = util::Trim(l);
        if (t.empty() || t[0] == '#') continue;
        auto f = util::SplitKeep(l, '\t');
        if (f.size() >= 2) out.push_back({ util::Trim(f[0]), util::Trim(f[1]), false });
    }
    Check(out.size() == 2 && out[0].name == "ThinkPad" && out[1].addr == "201:aa::2", "книга: имя<TAB>адрес, третий столбец не мешает");
    util::Run({ "rm", "-rf", dir });
}

static void TestJournal() {
    Section("лог (journalctl)");
    long long ts; std::string t;
    Check(pnl::ParseJournalLine("1729512131.123456 kjele yggdrasil[812]: 2026/10/21 14:02:11 Connected TLS: x@tls://a:443", ts, t) &&
          ts == 1729512131 && t == "Connected TLS: x@tls://a:443", "время и текст без своего штампа Yggdrasil");
    Check(pnl::ParseJournalLine("1729512131.1 kjele yggdrasil[812]: Started Yggdrasil", ts, t) && t == "Started Yggdrasil", "строка без штампа");
    Check(!pnl::ParseJournalLine("-- No entries --", ts, t), "«нет записей» - не строка лога");
    Check(pnl::LogScreen::Level("Failed to connect to tls://x") == 3 && pnl::LogScreen::Level("peer x: handshake timeout") == 2 &&
          pnl::LogScreen::Level("Connected TLS") == 1, "уровень строки");
}

static void TestTree() {
    Section("дерево маршрутов");
    // корень r -> a -> я; у корня ещё ветка b с двумя детьми; у меня ребёнок c
    auto key = [](const std::string& n) { return std::string(64 - n.size(), '0') + n; };
    json::Value tree = json::MakeArray();
    auto add = [&](const std::string& n, const std::string& p, const std::string& ad) {
        json::Value e = json::MakeObject();
        e.at("key") = json::MakeString(key(n)); e.at("parent") = json::MakeString(key(p)); e.at("address") = json::MakeString(ad);
        tree.arr.push_back(e);
    };
    add("r", "r", "200::a"); add("a", "r", "200::b"); add("me", "a", "200::c"); add("b", "r", "200::d");
    add("b1", "b", "200::e"); add("b2", "b", "200::f"); add("c", "me", "200::10");
    pnl::App app;
    app.s.tree = tree;
    app.s.treeAt = 1;
    app.s.self = json::MakeObject();
    app.s.self.at("key") = json::MakeString(key("me"));
    pnl::TreeScreen ts;
    ts.Build(app);
    std::vector<std::string> shown;
    for (const auto& r : ts.rows) shown.push_back(ts.nodes[(size_t)r.n].addr);
    Eq(util::Join(shown, " "), "200::a 200::b 200::c 200::10", "от корня - только путь до тебя и твои потомки");
    Check(ts.Depth(ts.self) == 2, "глубина тебя от корня");
    ts.open.insert(key("r"));
    ts.Flatten();
    Check(ts.rows.size() == 5, "Enter на корне раскрывает его ветки (b появилась, её дети - нет)");
}

int main() {
    printf("Тесты ygg-control %s\n", PANEL_VERSION);
    TestNames();
    TestUnits();
    TestBook();
    TestJournal();
    TestTree();
    printf("\nИтог: %d пройдено, %d провалено\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
