// screens3.cpp - служба, обновления, лог, конфиг, ключ, все команды, сторож.
namespace pnl {

// ------------------------------------------------------------------ обновления (блок общий для «Службы» и «Обновлений»)
void InstallPanel(App& a) {
    if (!(a.pupd.ok && util::VersionNewer(PANEL_VERSION, a.pupd.tag))) { a.Flash("Новой версии панели нет - сначала проверь [r].", OR); a.CheckPanelUpdate(false); return; }
    std::string tag = a.pupd.tag;
    a.Confirm("обновить панель", "Скачать панель v" + tag + " из репозитория и поставить?\nПроверка: git-отпечаток файла и самопроверка новой версии. "
              "Старая останется рядом как ygg-control.backup и вернётся сама, если новая не запустится. После установки панель перезапустится.\n\nEnter - установить",
              [&a, tag] {
        a.Op("ставлю панель v" + tag, [tag] { std::string e = ApplyPanelUpdate(tag); return e.empty() ? std::string() : "!" + e; }, "",
             [&a](const std::string& res) {
                 if (!res.empty() && res[0] == '!') return;
                 // перезапуск на новой версии
                 a.data.Stop();
                 a.term.Stop();
                 std::string self = util::SelfExe();
                 char* argv[] = { const_cast<char*>(self.c_str()), nullptr };
                 execv(self.c_str(), argv);
                 _exit(0);
             });
    });
}
void InstallYgg(App& a) {
    bool nw = a.yupd.ok && !a.s.version.empty() && util::VersionNewer(a.s.version, a.yupd.tag);
    if (!nw) { a.Flash("Новой версии Yggdrasil нет - сначала проверь [r].", OR); a.CheckYggUpdate(false); return; }
    if (a.yupd.debUrl.empty()) { a.Flash("В релизе " + a.yupd.tag + " нет .deb для этой архитектуры - обнови вручную: " + a.yupd.page, OR); return; }
    std::string url = a.yupd.debUrl, name = a.yupd.debName, tag = a.yupd.tag;
    a.Confirm("обновить Yggdrasil", "Скачать " + name + " и поставить через apt?\nКонфиг и ключ останутся прежними. Служба перезапустится - "
              "связь через Yggdrasil пропадёт на несколько секунд (если ты сидишь по SSH через Yggdrasil - сессия оборвётся).\n\nEnter - установить",
              [&a, url, name, tag] {
        a.Op("ставлю Yggdrasil " + tag, [url, name] {
            net::RequestOpts o; o.maxBody = 200 * 1024 * 1024; o.timeoutMs = 180000;
            net::HttpResult r = net::Get(url, o);
            if (!r.transportOk || r.status != 200 || !r.complete || r.body.size() < 100000) return std::string("!не удалось скачать ") + name;
            std::string path = "/tmp/" + name;
            if (!util::WriteFile(path, r.body, 0644)) return "!не удалось записать " + path;
            // без вопросов (DEBIAN_FRONTEND), свой конфиг не заменять (--force-confold)
            util::ProcResult p = util::Run({ "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "--allow-downgrades",
                                             "-o", "Dpkg::Options::=--force-confold", path }, 600000);
            unlink(path.c_str());
            std::string logPath = util::SelfDir() + "apt-yggdrasil.log";
            util::WriteFile(logPath, p.out);
            if (p.code != 0) {
                // причина - строки «E:» (или dpkg: error), а не «Reading package lists...»
                std::string why;
                for (const auto& l : util::Lines(p.out)) {
                    std::string t = util::Trim(l);
                    if (util::StartsWith(t, "E:") || util::IContains(t, "dpkg: error") || util::IContains(t, "dependency problems") || util::IContains(t, "depends"))
                        why += (why.empty() ? "" : " | ") + t;
                }
                if (why.empty()) { auto ls = util::Lines(util::Trim(p.out)); why = ls.empty() ? "код " + std::to_string(p.code) : ls.back(); }
                return "!apt не справился: " + util::U8Left(why, 300) + " (весь вывод: " + logPath + ")";
            }
            return "Yggdrasil обновлён: " + name;
        }, "", [&a](const std::string&) { a.data.ReloadPaths(); });
    }, true);
}
void DrawUpdates(Canvas& c, App& a, int x, int y, int w, int h) {
    c.Box(x, y, w, h, "обновления", BF, B);
    int x0 = x + 2, x1 = x + w - 1;
    bool pnew = a.pupd.ok && util::VersionNewer(PANEL_VERSION, a.pupd.tag);
    std::vector<P> l1 = { { "панель     ", DM }, { std::string("у тебя v") + PANEL_VERSION, TX } };
    if (a.pupdChecking) l1.push_back({ "   проверяю…", DM });
    else if (!a.pupd.checked) l1.push_back({ "   ещё не проверял", DM });
    else if (!a.pupd.ok) l1.push_back({ "   " + a.pupd.err, OR });
    else {
        l1.push_back({ "   в репозитории ", DM }); l1.push_back({ "v" + a.pupd.tag, pnew ? G : TX, pnew });
        l1.push_back({ pnew ? "" : "   ✓ последняя", G });
        l1.push_back({ "   проверено " + util::TimeText(a.pupd.at, "%H:%M"), DM });
    }
    c.Seq(x0, y + 1, l1, x1);
    bool ynew = a.yupd.ok && !a.s.version.empty() && util::VersionNewer(a.s.version, a.yupd.tag);
    std::vector<P> l2 = { { "Yggdrasil  ", DM }, { "у тебя " + (a.s.version.empty() ? std::string("?") : a.s.version), TX } };
    if (a.yupdChecking) l2.push_back({ "   проверяю…", DM });
    else if (!a.yupd.checked) l2.push_back({ "   ещё не проверял", DM });
    else if (!a.yupd.ok) l2.push_back({ "   " + a.yupd.err, OR });
    else if (ynew) { l2.push_back({ "   вышла ", DM }); l2.push_back({ a.yupd.tag, G, true }); }
    else l2.push_back({ "   ✓ последняя", G });
    c.Seq(x0, y + 2, l2, x1);
    int yy = y + 4;
    if (pnew && yy < y + h - 1) c.Seq(x0, yy++, { { "[i]", G, true }, { " установить панель v" + a.pupd.tag, G, true }, { "   скачать → самопроверка → замена, старая останется как .backup", DM } }, x1);
    if (ynew && yy < y + h - 1) c.Seq(x0, yy++, { { "[y]", G, true }, { " установить Yggdrasil " + a.yupd.tag, G, true }, { "   .deb через apt, конфиг и ключ не трогаются", DM } }, x1);
    if (yy < y + h - 1) c.Seq(x0, yy, { { "[r]", B, true }, { " проверить сейчас", TX } }, x1);
}
bool UpdatesKey(const tui::Key& k, App& a) {
    if (k.is('i') || k.text == "ш") { InstallPanel(a); return true; }
    if (k.is('y') || k.text == "н") { InstallYgg(a); return true; }
    if (k.is('r') || k.text == "к") { a.CheckPanelUpdate(false); a.CheckYggUpdate(false); return true; }
    return false;
}

struct UpdatesScreen : Screen {
    void OnShow(App& a) override {
        if (!a.pupd.checked || util::Now() - a.pupd.at > 600) a.CheckPanelUpdate(true);
        if (!a.yupd.checked || util::Now() - a.yupd.at > 600) a.CheckYggUpdate(true);
    }
    void Draw(Canvas& c, App& a) override {
        Header(c, "ОБНОВЛЕНИЯ", std::string("панель - из репозитория ") + PANEL_REPO_PAGE + " · Yggdrasil - релизы на GitHub");
        DrawUpdates(c, a, 2, 3, c.w - 4, 9);
        int y = 13;
        for (const auto& l : Wrap("Релизов у панели нет: версия берётся из linux/src/panel/version.h, программа - linux/ygg-control. "
                                  "Скачанный файл сверяется с git-отпечатком и запускается с --selftest - если номер подняли, а программу забыли закоммитить, "
                                  "панель откажется ставить старый файл как новый.", c.w - 6))
            if (y < c.h - 2) c.Put(3, y++, l, DM);
        Footer(c, a, { { "r", "проверить" }, { "i", "установить панель" }, { "y", "установить Yggdrasil" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override { return UpdatesKey(k, a); }
};
Screen* MakeUpdates() { return new UpdatesScreen(); }

// ------------------------------------------------------------------ служба
struct ServiceScreen : Screen {
    long long cpuPid = 0, cpuTicks = 0, cpuAt = 0;
    double cpuPct = -1;
    void OnShow(App& a) override {
        if (!a.pupd.checked) a.CheckPanelUpdate(true);
        if (!a.yupd.checked) a.CheckYggUpdate(true);
    }
    void SampleCpu(long long pid) {
        if (pid <= 0) { cpuPct = -1; return; }
        std::string st = util::ReadFile("/proc/" + std::to_string(pid) + "/stat");
        size_t rp = st.rfind(')');
        if (rp == std::string::npos) return;
        auto f = util::Split(st.substr(rp + 2), ' ');
        if (f.size() < 15) return;
        long long t = util::ToLL(f[11]) + util::ToLL(f[12]);
        long long now = util::Now();
        if (pid == cpuPid && now > cpuAt) cpuPct = (double)(t - cpuTicks) / (double)sysconf(_SC_CLK_TCK) / (double)(now - cpuAt) * 100.0;
        if (pid != cpuPid || now - cpuAt >= 3) { cpuPid = pid; cpuTicks = t; cpuAt = now; }
    }
    void Draw(Canvas& c, App& a) override {
        const ygg::ServiceInfo& sv = a.s.svc;
        SampleCpu(sv.pid);
        Header(c, "СЛУЖБА", "yggdrasil.service");
        int W = c.w - 4, lw = W * 55 / 100, rw = W - lw - 1;
        c.Box(2, 3, lw, 9, "состояние", BF, B);
        int x = 4, x1 = 2 + lw - 1;
        if (!sv.exists) {
            c.Put(x, 4, "служба yggdrasil не найдена", RD, true);
            c.Put(x, 5, "Поставь Yggdrasil: пакет yggdrasil (yggdrasil-network.github.io).", DM, false, Color(), x1);
        } else {
            bool run = sv.running();
            c.Seq(x, 4, { { "● ", run ? G : RD }, { run ? "работает" : sv.active == "failed" ? "упала" : "остановлена", run ? G : RD, true },
                          { sv.since ? "   с " + util::DateRu(sv.since) : "", DM } }, x1);
            c.Seq(x, 5, { { "автозапуск ", DM }, { sv.autoStart() ? "включён" : "выключен", sv.autoStart() ? TX : OR } }, x1);
            c.Seq(x, 6, { { "память ", DM }, { sv.memBytes >= 0 ? Bytes((double)sv.memBytes) : "—", TX },
                          { "   процессор ", DM }, { cpuPct >= 0 ? util::FixedRu(cpuPct, 1) + " %" : "—", TX } }, x1);
            c.Seq(x, 7, { { "конфиг ", DM }, { a.s.paths.conf, TX } }, x1);
            c.Seq(x, 9, { { "[r]", OR, true }, { " перезапустить сейчас", OR } }, x1);
            c.Seq(x, 10, { { "[R]", B, true }, { " через 15 с - если ты сидишь по Ygg", TX } }, x1);
        }
        int rx = 2 + lw + 1;
        c.Box(rx, 3, rw, 9, "действия", BF, B);
        if (sv.exists) {
            c.Seq(rx + 2, 4, { { "[x]", B, true }, { " остановить", sv.running() ? TX : DM2 } }, rx + rw - 1);
            c.Seq(rx + 2, 5, { { "[g]", B, true }, { " запустить", sv.running() ? DM2 : TX } }, rx + rw - 1);
            c.Seq(rx + 2, 6, { { "[e]", B, true }, { sv.autoStart() ? " выключить автозапуск" : " включить автозапуск", TX } }, rx + rw - 1);
            int y = 8;
            for (const auto& l : Wrap("остановка обрывает все сессии, спросит подтверждение", rw - 4)) if (y < 11) c.Put(rx + 2, y++, l, DM);
        }
        DrawUpdates(c, a, 2, 13, W, std::min(8, c.h - 15));
        Footer(c, a, { { "буква", "действие" }, { "Esc", "назад" }, { "q", "выход" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        const ygg::ServiceInfo& sv = a.s.svc;
        auto act = [&a](const std::string& action, const std::string& label, const std::string& ok) {
            a.Op(label, [action] { std::string e = ygg::ServiceCtl(action); return e.empty() ? std::string() : "!" + e; }, ok,
                 [&a](const std::string&) { a.data.ReloadPaths(); });
        };
        if (k.is('r') || k.text == "к") {
            a.Confirm("перезапуск", "Перезапустить службу Yggdrasil сейчас? Все соединения через Yggdrasil оборвутся на несколько секунд.\n\nEnter - перезапустить",
                      [act] { act("restart", "перезапускаю службу", "Служба перезапущена."); }, true);
            return true;
        }
        if (k.is('R') || k.text == "К") {
            a.Op("планирую перезапуск", [] { std::string e = ygg::RestartDelayed(15); return e.empty() ? std::string("Служба перезапустится через 15 секунд.") : "!" + e; });
            return true;
        }
        if ((k.is('x') || k.text == "ч") && sv.running()) {
            a.Confirm("остановить службу", "Остановить Yggdrasil? Все сессии оборвутся, адрес Yggdrasil станет недоступен, пока не запустишь снова.\n"
                      "Если ты подключён к этой машине через Yggdrasil - потеряешь доступ.\n\nEnter - остановить",
                      [act] { act("stop", "останавливаю службу", "Служба остановлена."); }, true);
            return true;
        }
        if ((k.is('g') || k.text == "п") && !sv.running()) { act("start", "запускаю службу", "Служба запущена."); return true; }
        if (k.is('e') || k.text == "у") {
            if (sv.autoStart()) act("disable", "выключаю автозапуск", "Автозапуск выключен - после перезагрузки служба не стартует сама.");
            else act("enable", "включаю автозапуск", "Автозапуск включён.");
            return true;
        }
        return UpdatesKey(k, a);
    }
};
Screen* MakeService() { return new ServiceScreen(); }

// ------------------------------------------------------------------ лог (journalctl)
// Строка journalctl -o short-unix: "1729512131.123456 host yggdrasil[123]: 2026/10/21 14:02:11 сообщение"
inline bool ParseJournalLine(const std::string& l, long long& ts, std::string& text) {
    if (l.empty() || l[0] == '-') return false;              // "-- No entries --"
    size_t sp = l.find(' ');
    ts = util::ToLL(l.substr(0, l.find('.')), 0);
    if (!ts || sp == std::string::npos) return false;
    std::string rest = l.substr(sp + 1);
    size_t c = rest.find(": ");                              // "host yggdrasil[123]: "
    if (c != std::string::npos && c < 80) rest = rest.substr(c + 2);
    // свой штамп времени Yggdrasil "2026/10/21 14:02:11 " - лишний
    if (rest.size() > 20 && rest[4] == '/' && rest[7] == '/' && rest[13] == ':') rest = rest.substr(20);
    text = rest;
    return true;
}

struct LogScreen : Screen {
    struct Line { long long ts = 0; int lvl = 1; std::string text; };
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<Line>> lines = std::make_shared<std::vector<Line>>();
    std::shared_ptr<std::string> err = std::make_shared<std::string>();
    std::shared_ptr<std::atomic<bool>> loading = std::make_shared<std::atomic<bool>>(false);
    long long loadedAt = 0;
    int level = 0;            // 0 все, 1 инфо, 2 предупр., 3 ошибки
    bool follow = true, searching = false;
    LineEdit search;
    int top = 0;

    bool Typing() const override { return searching; }
    static int Level(const std::string& l) {
        std::string s = util::Lower(l);
        if (util::Contains(s, "error") || util::Contains(s, "failed") || util::Contains(s, "panic") || util::Contains(s, "fatal")) return 3;
        if (util::Contains(s, "warn") || util::Contains(s, "timeout") || util::Contains(s, "reset") || util::Contains(s, "refused")) return 2;
        return 1;
    }
    void Load() {
        if (*loading) return;
        *loading = true;
        loadedAt = util::Now();
        auto mu_ = mu; auto L = lines; auto E = err; auto ld = loading;
        std::thread([mu_, L, E, ld] {
            util::ProcResult r = util::Run({ "journalctl", "-u", "yggdrasil", "-o", "short-unix", "--no-pager", "-n", "3000" }, 20000);
            std::vector<Line> out;
            std::string e;
            if (!r.started) e = "journalctl не найден";
            for (const auto& l : util::Lines(r.out)) {
                Line x;
                if (!ParseJournalLine(l, x.ts, x.text)) continue;
                x.lvl = Level(x.text);
                out.push_back(x);
            }
            if (out.empty() && e.empty()) e = util::Contains(r.out, "No entries") || r.out.empty() ? "в журнале нет записей службы yggdrasil" : util::U8Left(util::Trim(r.out), 200);
            std::lock_guard<std::mutex> lk(*mu_);
            *L = out; *E = e;
            *ld = false;
        }).detach();
    }
    void OnShow(App&) override { Load(); }
    std::vector<const Line*> Filtered(const std::vector<Line>& all) const {
        std::vector<const Line*> v;
        std::string q = util::U8Lower(util::Trim(search.text));
        for (const auto& l : all) {
            if (level && l.lvl != level) continue;
            if (!q.empty() && !util::Contains(util::U8Lower(l.text), q)) continue;
            v.push_back(&l);
        }
        return v;
    }
    void Draw(Canvas& c, App& a) override {
        if (follow && util::Now() - loadedAt >= 3) Load();
        std::vector<Line> all; std::string e;
        { std::lock_guard<std::mutex> lk(*mu); all = *lines; e = *err; }
        auto v = Filtered(all);
        c.Seq(3, 1, { { "▲ ", B, true }, { "ЛОГ", B, true }, { "   journalctl -u yggdrasil · ", DM }, { follow ? "● следую за новыми" : "◌ пауза", follow ? G : OR } }, c.w - 2);
        int x = 3;
        static const char* ln[] = { "все", "инфо", "предупр.", "ошибки" };
        x = c.Put(x, 3, "уровень ", DM);
        for (int i = 0; i < 4; ++i) x = c.Put(x, 3, level == i ? std::string("[") + ln[i] + "]" : std::string(" ") + ln[i] + " ", level == i ? WH : DM, level == i) + 1;
        x += 4;
        if (searching || !search.text.empty()) { x = c.Put(x, 3, "/ ", B, true); x = c.Put(x, 3, search.text, WH); if (searching) x = c.Put(x, 3, "▌", B); }
        c.Put(x + 3, 3, std::to_string(v.size()) + " из " + std::to_string(all.size()), DM);
        int H = c.h - 7, W = c.w - 4, vis = H - 2;
        c.Box(2, 4, W, H, "", BF, B);
        int maxTop = std::max(0, (int)v.size() - vis);
        if (follow) top = maxTop;
        top = std::max(0, std::min(top, maxTop));
        std::string q = util::U8Lower(util::Trim(search.text));
        for (int i = 0; i < vis && top + i < (int)v.size(); ++i) {
            const Line& l = *v[(size_t)(top + i)];
            int y = 5 + i;
            c.Put(4, y, util::TimeText(l.ts, "%H:%M:%S"), DM);
            static const char* lv[] = { "", "INFO", "WARN", "ERROR" };
            Color lc = l.lvl == 3 ? RD : l.lvl == 2 ? OR : DM;
            c.Put(14, y, lv[l.lvl], lc, l.lvl > 1);
            Color tc = l.lvl == 3 ? RD : l.lvl == 2 ? OR : TX;
            c.Put(21, y, l.text, tc, false, Color(), 2 + W - 1);
            // подсветка найденного
            if (!q.empty()) {
                std::string low = util::U8Lower(l.text);
                size_t p = low.find(q);
                if (p != std::string::npos) {
                    int cx = 21 + (int)util::U8Len(l.text.substr(0, p)), n = (int)util::U8Len(search.text);
                    c.FillBg(cx, y, std::min(cx + n, 2 + W - 1), BH);
                }
            }
        }
        if (v.empty()) c.Put(4, 5, *loading && all.empty() ? "читаю журнал…" : (!e.empty() ? e : "ничего не подходит под фильтр"), DM, false, Color(), c.w - 3);
        if (top + vis >= (int)v.size() && !v.empty() && 5 + (int)v.size() - top < 4 + H - 1) c.Put(4, 5 + (int)v.size() - top, "▼ конец лога", DM2);
        Footer(c, a, { { "Tab", "уровень" }, { "/", "поиск" }, { "PgUp/PgDn", "листать" }, { "f", "следить" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        int vis = a.term.canvas.h - 9;
        if (searching) {
            if (k.k == K::Enter || k.k == K::Esc) { searching = false; if (k.k == K::Esc) search.text.clear(); return true; }
            search.Handle(k);
            return true;
        }
        if (k.k == K::Tab) { level = (level + 1) % 4; return true; }
        if (k.is('/')) { searching = true; return true; }
        if (k.is('f') || k.text == "а") { follow = !follow; if (follow) Load(); return true; }
        if (k.k == K::PgUp) { follow = false; top -= vis; return true; }
        if (k.k == K::PgDn) { top += vis; return true; }
        if (k.k == K::Up) { follow = false; --top; return true; }
        if (k.k == K::Down) { ++top; return true; }
        if (k.k == K::Home) { follow = false; top = 0; return true; }
        if (k.k == K::End) { follow = true; return true; }
        return false;
    }
};
Screen* MakeLog() { return new LogScreen(); }

// ------------------------------------------------------------------ конфиг
struct CfgHelp { const char* key; const char* text; };
static const CfgHelp CFG_HELP[] = {
    { "Peers", "Исходящие пиры: к кому подключаться. Формат tls://host:port, можно с ?key=... (закрепить ключ) и ?password=..." },
    { "InterfacePeers", "Пиры только через указанный сетевой интерфейс: \"имя интерфейса\": [\"uri\", ...]" },
    { "Listen", "Входящие: на каких адресах и портах принимать пиров, например tls://0.0.0.0:443. Пусто - никого не ждём." },
    { "AdminListen", "Адрес управления для yggdrasilctl (и этой панели). none - выключить управление." },
    { "MulticastInterfaces", "Автопоиск соседей в локальной сети. Запись: Regex (какие интерфейсы), Beacon (сообщать о себе), Listen (принимать), Port, Priority, Password." },
    { "AllowedPublicKeys", "Белый список ключей входящих пиров. Пусто - пускать всех." },
    { "IfName", "Имя TUN-адаптера. auto - выбрать само, none - работать без адаптера." },
    { "IfMTU", "MTU адаптера, обычно 65535." },
    { "NodeInfoPrivacy", "true - не рассказывать другим узлам версию и платформу в NodeInfo." },
    { "NodeInfo", "Сведения об узле, которые видят другие через getNodeInfo, например {\"name\": \"KJELE\"}." },
    { "LogLookups", "Писать в лог поиск узлов (для отладки)." },
};
inline const char* CfgHelpFor(const std::string& k) {
    for (const auto& h : CFG_HELP) if (k == h.key) return h.text;
    return "Нет описания: поле этой версии Yggdrasil, правится как JSON.";
}
inline bool HiddenKey(const std::string& k) { return k == "PrivateKey" || k == "PrivateKeyPath"; }
inline std::string ItemText(const json::Value& v) { return v.isString() ? v.str() : json::Dump(v); }

struct ConfigScreen : Screen {
    std::string startField;
    ListView keysLv, itemsLv;
    bool right = false;          // фокус справа (строки поля)
    std::vector<std::string> Keys(App& a) {
        std::vector<std::string> k;
        for (const auto& h : CFG_HELP) if (a.cfgEdit.has(h.key)) k.push_back(h.key);
        for (const auto& kv : a.cfgEdit.obj) if (!HiddenKey(kv.first) && std::find(k.begin(), k.end(), kv.first) == k.end()) k.push_back(kv.first);
        return k;
    }
    static bool Changed(App& a, const std::string& k) { return json::Dump(a.cfgOrig[k]) != json::Dump(a.cfgEdit[k]); }
    static bool AnyChanged(App& a) {
        if (!a.cfgLoaded) return false;
        std::set<std::string> all;
        for (auto& kv : a.cfgOrig.obj) all.insert(kv.first);
        for (auto& kv : a.cfgEdit.obj) all.insert(kv.first);
        for (auto& k : all) if (Changed(a, k)) return true;
        return false;
    }
    void OnShow(App& a) override {
        if (!a.cfgLoaded) a.ReloadConfig();
    }
    std::string Summary(const json::Value& v) {
        if (v.isArray()) return v.size() ? std::to_string(v.size()) : "—";
        if (v.isObject()) return v.size() ? "{" + std::to_string(v.size()) + "}" : "{}";
        if (v.isBool()) return v.boolean() ? "да" : "нет";
        if (v.isString()) return v.str().empty() ? "—" : v.str();
        return v.text();
    }
    std::string SelKey(App& a) {
        auto k = Keys(a);
        return keysLv.sel < (int)k.size() ? k[(size_t)keysLv.sel] : "";
    }
    void Draw(Canvas& c, App& a) override {
        std::string conf = a.s.paths.conf.empty() ? ygg::CtlPaths().conf : a.s.paths.conf;
        Header(c, "КОНФИГ", conf);
        if (!a.cfgLoaded) {
            c.Box(2, 3, c.w - 4, 5, "конфиг", BF, B);
            c.Put(4, 5, a.cfgLoading ? "читаю конфиг…" : "Конфиг не прочитан: " + a.cfgErr, a.cfgLoading ? DM : RD, false, Color(), c.w - 3);
            Footer(c, a, { { "r", "прочитать снова" }, { "Esc", "назад" } });
            return;
        }
        auto keys = Keys(a);
        if (!startField.empty()) { for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == startField) { keysLv.sel = (int)i; right = true; } startField.clear(); }
        int W = c.w - 4, lw = std::max(30, W * 40 / 100), rw = W - lw - 1, H = c.h - 6;
        c.Box(2, 3, lw, H, "поля", BF, B);
        int vis = H - 5;
        keysLv.Clamp((int)keys.size(), vis);
        for (int i = 0; i < vis && keysLv.top + i < (int)keys.size(); ++i) {
            const std::string& k = keys[(size_t)(keysLv.top + i)];
            int y = 4 + i;
            bool on = keysLv.top + i == keysLv.sel;
            if (on) { c.FillBg(3, y, 2 + lw - 1, right ? Hex("#13202f") : BH); c.Put(3, y, "▸", B); }
            c.Put(5, y, k, on ? WH : TX, on, Color(), 2 + lw - 12);
            bool ch = Changed(a, k);
            c.Put(2 + lw - 11, y, util::U8Fit(ch ? "изм." : Summary(a.cfgEdit[k]), 9), ch ? OR : DM);
        }
        int py = 4 + std::min(vis, (int)keys.size());
        if (a.cfgOrig.has("PrivateKey") || a.cfgOrig.has("PrivateKeyPath")) { c.Put(5, py, "PrivateKey", DM2); c.Put(2 + lw - 11, py, "скрыт", OR); py += 2; }
        c.Put(4, std::min(py, 3 + H - 3), "закрытый ключ панель", DM2);
        c.Put(4, std::min(py + 1, 3 + H - 2), "не показывает и не правит", DM2);

        std::string K = SelKey(a);
        int rx = 2 + lw + 1, x = rx + 2, x1 = rx + rw - 1;
        c.Box(rx, 3, rw, H, K.empty() ? "поле" : K + (Changed(a, K) ? " · изменено" : ""), BF, Changed(a, K) ? OR : B);
        int y = 4;
        if (!K.empty()) {
            const json::Value& V = a.cfgEdit[K];
            const json::Value& O = a.cfgOrig[K];
            for (const auto& l : Wrap(CfgHelpFor(K), rw - 4)) c.Put(x, y++, l, DM, false, Color(), x1);
            ++y;
            int listH = std::max(3, H - 19);
            if (V.isArray() || V.isObject()) {
                std::vector<std::pair<std::string, Color>> rowsTxt;
                std::set<std::string> orig;
                if (V.isArray()) {
                    for (size_t i = 0; i < O.size(); ++i) orig.insert(ItemText(O[i]));
                    for (size_t i = 0; i < V.size(); ++i) { std::string t = ItemText(V[i]); rowsTxt.push_back({ t, orig.count(t) ? G : G }); }
                } else for (const auto& kv : V.obj) rowsTxt.push_back({ kv.first + ": " + ItemText(kv.second), G });
                itemsLv.Clamp((int)rowsTxt.size(), listH);
                for (int i = 0; i < listH && itemsLv.top + i < (int)rowsTxt.size(); ++i) {
                    int idx = itemsLv.top + i;
                    bool on = right && idx == itemsLv.sel;
                    bool isNew = V.isArray() && !orig.count(rowsTxt[(size_t)idx].first);
                    if (on) c.FillBg(rx + 1, y + i, x1, BH);
                    c.Put(x, y + i, isNew ? "+" : "●", isNew ? G : B);
                    int e = c.Put(x + 2, y + i, rowsTxt[(size_t)idx].first, isNew ? G : TX, on, Color(), x1);
                    if (isNew) c.Put(e + 3, y + i, "новый", G, false, Color(), x1);
                }
                // удалённые строки (были, сейчас нет)
                int yy = y + std::min(listH, (int)rowsTxt.size());
                if (V.isArray()) {
                    std::set<std::string> now;
                    for (size_t i = 0; i < V.size(); ++i) now.insert(ItemText(V[i]));
                    for (size_t i = 0; i < O.size() && yy < y + listH; ++i)
                        if (!now.count(ItemText(O[i]))) { c.Put(x, yy, "−", RD); c.Put(x + 2, yy++, ItemText(O[i]) + "   удалён", RD, false, Color(), x1); }
                }
                if (rowsTxt.empty()) c.Put(x, y, "пусто - [a] добавить", DM);
                y += listH + 1;
                c.Seq(x, y++, { { "[a]", B, true }, { " добавить   ", TX }, { "[d]", B, true }, { " убрать   ", TX }, { "[Enter]", B, true }, { " изменить", TX } }, x1);
            } else if (V.isBool()) {
                c.Seq(x, y++, { { V.boolean() ? "◉ да   ○ нет" : "○ да   ◉ нет", WH } }, x1);
                y += 1;
                c.Seq(x, y++, { { "[Enter]", B, true }, { " переключить", TX } }, x1);
            } else {
                c.Put(x, y++, ItemText(V).empty() ? "—" : ItemText(V), WH, false, Color(), x1);
                if (Changed(a, K)) c.Put(x, y++, "было: " + ItemText(O), DM, false, Color(), x1);
                y += 1;
                c.Seq(x, y++, { { "[Enter]", B, true }, { " изменить", TX } }, x1);
            }
        }
        // перед записью
        int by = 3 + H - 9;
        if (by > y) {
            c.Put(x, by, "перед записью:", DM);
            c.Seq(x, by + 1, { { "✓ ", G }, { "проверка: yggdrasil -normaliseconf", TX } }, x1);
            c.Seq(x, by + 2, { { "✓ ", G }, { "копия " + util::BaseName(conf) + ".bak-" + util::TimeText(util::Now(), "%d%m%Y"), TX } }, x1);
            c.Seq(x, by + 3, { { "✓ ", G }, { "запись целиком или никак", TX } }, x1);
            c.Put(x, by + 4, "новые пиры заработают после перезапуска службы", DM, false, Color(), x1);
        }
        bool any = AnyChanged(a);
        c.Seq(x, 3 + H - 3, { { "[S]", any ? OR : DM2, true }, { " сохранить и перезапустить", any ? OR : DM2 } }, x1);
        c.Seq(x, 3 + H - 2, { { "[w]", any ? B : DM2, true }, { " только сохранить   ", any ? TX : DM2 }, { "[z]", any ? B : DM2, true }, { " отменить правки", any ? TX : DM2 } }, x1);
        Footer(c, a, { { "↑↓", "поле" }, { "Enter", "изменить" }, { "Tab", "поле ↔ строки" }, { "S", "сохранить" }, { "Esc", "назад (спросит)" } });
    }
    void EditItem(App& a, const std::string& K, int idx) {
        json::Value& V = a.cfgEdit.at(K);
        bool strings = true;
        for (size_t i = 0; i < V.size(); ++i) if (V.isArray() && !V[i].isString()) strings = false;
        if (V.isArray()) {
            std::string cur = idx >= 0 && idx < (int)V.size() ? ItemText(V[(size_t)idx]) : "";
            a.Ask(idx < 0 ? "добавить в " + K : "изменить строку " + K, strings ? "Строка, например tls://host:port" : "JSON-объект",
                  { { "значение", cur } }, [&a, K, idx, strings](const std::vector<std::string>& v) {
                if (v[0].empty()) return;
                json::Value item;
                if (strings && !(v[0][0] == '{' || v[0][0] == '[')) item = json::MakeString(v[0]);
                else if (!json::Parse(v[0], item)) { a.Flash("Это не JSON - проверь скобки и кавычки", RD); return; }
                json::Value& arr = a.cfgEdit.at(K);
                if (!arr.isArray()) arr = json::MakeArray();
                if (idx < 0) arr.arr.push_back(item);
                else if (idx < (int)arr.arr.size()) arr.arr[(size_t)idx] = item;
            });
        } else if (V.isObject()) {
            std::string ck = idx >= 0 && idx < (int)V.obj.size() ? V.obj[(size_t)idx].first : "";
            std::string cv = idx >= 0 && idx < (int)V.obj.size() ? ItemText(V.obj[(size_t)idx].second) : "";
            a.Ask(idx < 0 ? "добавить в " + K : "изменить " + K, "Значение - строка или JSON.", { { "ключ", ck }, { "значение", cv } },
                  [&a, K, ck](const std::vector<std::string>& v) {
                if (v[0].empty()) return;
                json::Value item;
                if (!json::Parse(v[1], item)) item = json::MakeString(v[1]);
                json::Value& obj = a.cfgEdit.at(K);
                if (!ck.empty() && ck != v[0]) obj.erase(ck);
                obj.at(v[0]) = item;
            });
        } else if (V.isBool()) {
            V.b = !V.b;
        } else {
            bool num = V.isNumber();
            a.Ask("изменить " + K, CfgHelpFor(K), { { "значение", ItemText(V) } }, [&a, K, num](const std::vector<std::string>& v) {
                json::Value item;
                if (num) { if (!util::IsNumber(v[0])) { a.Flash("Здесь нужно число", RD); return; } item = json::MakeNumber(util::ToD(v[0], 0)); }
                else if (!v[0].empty() && (v[0][0] == '{' || v[0][0] == '[') && json::Parse(v[0], item)) {}
                else item = json::MakeString(v[0]);
                a.cfgEdit.at(K) = item;
            });
        }
    }
    void Save(App& a, bool restart) {
        json::Value cfg = a.cfgEdit;
        a.Op(restart ? "сохраняю конфиг и перезапускаю службу" : "сохраняю конфиг", [cfg, restart] {
            std::string backup;
            std::string e = ygg::SaveConfig(ygg::CtlPaths(), cfg, &backup);
            if (!e.empty()) return "!" + e;
            if (restart) {
                e = ygg::ServiceCtl("restart");
                if (!e.empty()) return "!конфиг сохранён, но служба: " + e;
                return "Конфиг сохранён, служба перезапущена. Копия: " + util::BaseName(backup);
            }
            return "Конфиг сохранён (заработает после перезапуска службы). Копия: " + util::BaseName(backup);
        }, "", [&a](const std::string& res) { if (res.empty() || res[0] != '!') { a.cfgLoaded = false; a.ReloadConfig(); a.data.ReloadPaths(); } });
    }
    bool CanLeave(App& a) override {
        if (!AnyChanged(a)) return true;
        a.Confirm("несохранённые изменения", "В конфиге есть несохранённые изменения. Выйти и забыть их?\n\nEnter - выйти без сохранения",
                  [&a] { a.cfgEdit = a.cfgOrig; a.Pop(); }, true);
        return false;
    }
    bool Key(const tui::Key& k, App& a) override {
        if (!a.cfgLoaded) { if (k.is('r') || k.text == "к") { a.ReloadConfig(); return true; } return false; }
        auto keys = Keys(a);
        std::string K = SelKey(a);
        if (k.k == K::Tab || (k.k == K::Right && !right) || (k.k == K::Left && right)) { right = !right; return true; }
        if (k.is('S') || k.text == "Ы") { if (AnyChanged(a)) Save(a, true); return true; }
        if (k.is('w') || k.text == "ц") { if (AnyChanged(a)) Save(a, false); return true; }
        if (k.is('z') || k.text == "я") { a.cfgEdit = a.cfgOrig; a.Flash("Изменения отменены."); return true; }
        if (K.empty()) return keysLv.Handle(k, (int)keys.size(), a.term.canvas.h - 11);
        const json::Value& V = a.cfgEdit[K];
        bool list = V.isArray() || V.isObject();
        if (!right) {
            if (keysLv.Handle(k, (int)keys.size(), a.term.canvas.h - 11)) { itemsLv = ListView(); return true; }
            if (k.k == K::Enter) { if (list) right = true; else EditItem(a, K, -1); return true; }
        } else {
            if (list && itemsLv.Handle(k, (int)V.size(), std::max(3, a.term.canvas.h - 21))) return true;
            if (k.k == K::Enter) { EditItem(a, K, list ? (V.size() ? itemsLv.sel : -1) : -1); return true; }
        }
        if (list && (k.is('a') || k.text == "ф")) { right = true; EditItem(a, K, -1); return true; }
        if (list && right && (k.is('d') || k.text == "в") && itemsLv.sel < (int)V.size()) {
            json::Value& M = a.cfgEdit.at(K);
            if (M.isArray()) M.arr.erase(M.arr.begin() + itemsLv.sel);
            else M.obj.erase(M.obj.begin() + itemsLv.sel);
            return true;
        }
        return false;
    }
};
Screen* MakeConfig(const std::string& field) { auto* s = new ConfigScreen(); s->startField = field; return s; }

// ------------------------------------------------------------------ ключ узла
struct KeyScreen : Screen {
    void OnShow(App& a) override { if (!a.cfgLoaded) a.ReloadConfig(); }
    void Draw(Canvas& c, App& a) override {
        Header(c, "КЛЮЧ УЗЛА", "адрес Yggdrasil вычисляется из ключа");
        int W = c.w - 4, x = 4, x1 = 2 + W - 1;
        c.Box(2, 3, W, 7, "узел", BF, B);
        c.Seq(x, 4, { { "адрес           ", DM }, { a.s.self["address"].str().empty() ? "—" : a.s.self["address"].str(), WH } }, x1);
        c.Seq(x, 5, { { "подсеть         ", DM }, { a.s.self["subnet"].str().empty() ? "—" : a.s.self["subnet"].str(), TX } }, x1);
        c.Seq(x, 6, { { "публичный ключ  ", DM }, { a.s.self["key"].str().empty() ? "—" : a.s.self["key"].str(), TX } }, x1);
        std::string src = !a.cfgLoaded ? (a.cfgLoading ? "читаю конфиг…" : "конфиг не прочитан") :
                          !a.cfgOrig["PrivateKeyPath"].str().empty() ? "файл PEM: " + a.cfgOrig["PrivateKeyPath"].str() : "PrivateKey внутри конфига";
        c.Seq(x, 7, { { "закрытый ключ   ", DM }, { src, TX } }, x1);
        int y = 11;
        for (const auto& l : Wrap("Закрытый ключ - это и есть твой адрес: кто его знает, тот может выдать себя за этот узел. Панель его не показывает.", W - 2))
            c.Put(3, y++, l, DM);
        ++y;
        c.Seq(3, y++, { { "[c]", B, true }, { " копировать публичный ключ", TX } });
        ++y;
        c.Put(3, y++, "экспорт ключа", B, true);
        for (const auto& l : Wrap("Сохраняет закрытый ключ в файл PEM (yggdrasil -exportkey), права только для root. Пригодится, чтобы перенести адрес на другую машину "
                                  "или хранить ключ отдельно от конфига (PrivateKeyPath). Храни файл как пароль.", W - 2)) c.Put(3, y++, l, DM);
        c.Seq(3, y++, { { "[e]", B, true }, { " экспорт в PEM…", TX } });
        ++y;
        c.Put(3, y++, "новый ключ = новый адрес", RD, true);
        for (const auto& l : Wrap("Создаёт новый ключ (yggdrasil -genconf), записывает его в конфиг и перезапускает службу. Адрес сменится: всё, что знает старый адрес "
                                  "(SSH, правила файрвола, AllowedPublicKeys у друзей, сторож на других машинах), придётся поправить. Старый конфиг со старым ключом останется копией.", W - 2))
            if (y < c.h - 3) c.Put(3, y++, l, DM);
        if (y < c.h - 2) c.Seq(3, y++, { { "[n]", RD, true }, { " новый ключ", RD } });
        Footer(c, a, { { "c", "копировать" }, { "e", "экспорт" }, { "n", "новый ключ" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.is('c') || k.text == "с") { if (!a.s.self["key"].str().empty()) a.Copy(a.s.self["key"].str(), "Публичный ключ"); return true; }
        if (k.is('e') || k.text == "у") {
            std::string home = getenv("SUDO_USER") ? "/home/" + std::string(getenv("SUDO_USER")) : "/root";
            a.Ask("экспорт ключа", "Куда сохранить закрытый ключ (PEM, права 600):", { { "файл", home + "/yggdrasil-key.pem" } }, [&a](const std::vector<std::string>& v) {
                std::string path = v[0];
                if (path.empty()) return;
                a.Op("экспорт ключа", [path] {
                    const ygg::Paths& p = ygg::CtlPaths();
                    util::ProcResult r = util::Run({ p.exe, "-useconffile", p.conf, "-exportkey" }, 15000);
                    if (!r.started || r.code != 0 || r.out.find("-----BEGIN") == std::string::npos)
                        return "!yggdrasil не выдал ключ: " + util::U8Left(util::Trim(r.out), 160);
                    if (!util::WriteFile(path, r.out.substr(r.out.find("-----BEGIN")), 0600)) return std::string("!не удалось записать файл");
                    chmod(path.c_str(), 0600);
                    return "Ключ сохранён: " + path + " (права 600)";
                });
            });
            return true;
        }
        if (k.is('n') || k.text == "т") {
            if (!a.cfgLoaded) { a.Flash("Конфиг ещё не прочитан", OR); return true; }
            json::Value cfg = a.cfgOrig;
            a.Confirm("СМЕНИТЬ КЛЮЧ", "Создать НОВЫЙ ключ? Адрес этого узла сменится навсегда.\n\nВсё, что знает старый адрес (SSH, правила файрвола, "
                      "AllowedPublicKeys у друзей), перестанет его находить. Старый конфиг сохранится копией.\n\nEnter - сменить ключ", [&a, cfg] {
                a.Op("меняю ключ", [cfg]() mutable -> std::string {
                    const ygg::Paths& p = ygg::CtlPaths();
                    util::ProcResult r = util::Run({ p.exe, "-genconf", "-json" }, 15000);
                    json::Value gen;
                    if (!json::ParseLoose(r.out, gen) || gen["PrivateKey"].str().empty()) return "!yggdrasil -genconf не выдал ключ";
                    cfg.at("PrivateKey") = gen["PrivateKey"];
                    cfg.erase("PrivateKeyPath");
                    std::string backup;
                    std::string e = ygg::SaveConfig(p, cfg, &backup);
                    if (!e.empty()) return "!" + e;
                    e = ygg::ServiceCtl("restart");
                    if (!e.empty()) return "!ключ записан, но служба: " + e;
                    return "Новый ключ записан, служба перезапущена. Старый конфиг: " + util::BaseName(backup);
                }, "", [&a](const std::string&) { a.cfgLoaded = false; a.ReloadConfig(); a.data.ReloadPaths(); });
            }, true);
            return true;
        }
        return false;
    }
};
Screen* MakeKey() { return new KeyScreen(); }

// ------------------------------------------------------------------ все команды yggdrasilctl
struct CommandsScreen : Screen {
    ListView lv, outLv;
    bool outFocus = false;
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::string>> out = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::string> title = std::make_shared<std::string>();
    struct Cmd { std::string name, desc; std::vector<std::string> fields; };
    std::vector<Cmd> List(App& a) {
        std::vector<Cmd> v;
        json::Value l = FirstArray(a.s.commands);
        for (size_t i = 0; i < l.size(); ++i) {
            Cmd c;
            c.name = l[i].isObject() ? l[i]["command"].str() : l[i].str();
            c.desc = l[i]["description"].str();
            for (size_t k = 0; k < l[i]["fields"].size(); ++k) c.fields.push_back(l[i]["fields"][k].str());
            if (!c.name.empty()) v.push_back(c);
        }
        return v;
    }
    void RunCmd(App& a, const Cmd& cmd, const std::vector<std::string>& vals) {
        std::vector<std::string> args = { ygg::CtlPaths().ctl };
        for (const auto& e : ygg::CtlPaths().EndpointArgs()) args.push_back(e);
        args.push_back("-json");
        args.push_back(cmd.name);
        for (size_t i = 0; i < cmd.fields.size() && i < vals.size(); ++i) if (!vals[i].empty()) args.push_back(cmd.fields[i] + "=" + vals[i]);
        auto mu_ = mu; auto O = out; auto T = title;
        { std::lock_guard<std::mutex> lk(*mu_); O->clear(); *T = "выполняю " + cmd.name + "…"; }
        outLv = ListView();
        std::string name = cmd.name;
        a.Bg([args, mu_, O, T, name] {
            util::ProcResult r = util::Run(args, 30000);
            json::Value v;
            std::string text = json::ParseLoose(r.out, v) ? json::Dump(v, true) : r.out;
            std::lock_guard<std::mutex> lk(*mu_);
            *O = util::Lines(text);
            *T = name + (r.code == 0 ? "" : " · код " + std::to_string(r.code));
            return std::string();
        }, [&a](const std::string&) { a.data.RefreshNow(); });
        outFocus = true;
    }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "ВСЕ КОМАНДЫ")) return;
        auto cmds = List(a);
        Header(c, "ВСЕ КОМАНДЫ", "yggdrasilctl list · " + std::to_string(cmds.size()) + " команд");
        int W = c.w - 4, lw = std::max(28, W * 32 / 100), rw = W - lw - 1, H = c.h - 5;
        c.Box(2, 3, lw, H, "команды", BF, B);
        int vis = H - 2;
        lv.Clamp((int)cmds.size(), vis);
        for (int i = 0; i < vis && lv.top + i < (int)cmds.size(); ++i) {
            bool on = lv.top + i == lv.sel;
            if (on) { c.FillBg(3, 4 + i, 2 + lw - 1, outFocus ? Hex("#13202f") : BH); c.Put(3, 4 + i, "▸", B); }
            c.Put(5, 4 + i, cmds[(size_t)(lv.top + i)].name, on ? WH : TX, on, Color(), 2 + lw - 1);
        }
        if (cmds.empty()) c.Put(5, 4, "список команд не получен", DM);
        int rx = 2 + lw + 1, x = rx + 2, x1 = rx + rw - 1;
        std::vector<std::string> o; std::string t;
        { std::lock_guard<std::mutex> lk(*mu); o = *out; t = *title; }
        c.Box(rx, 3, rw, H, t.empty() ? "команда" : t, BF, B);
        int y = 4;
        if (lv.sel < (int)cmds.size()) {
            const Cmd& cmd = cmds[(size_t)lv.sel];
            c.Put(x, y++, cmd.name, WH, true, Color(), x1);
            for (const auto& l : Wrap(cmd.desc, rw - 4)) if (!l.empty()) c.Put(x, y++, l, DM, false, Color(), x1);
            if (!cmd.fields.empty()) c.Seq(x, y++, { { "параметры: ", DM }, { util::Join(cmd.fields, ", "), B } }, x1);
            c.Seq(x, y++, { { "[Enter]", B, true }, { " выполнить", TX } }, x1);
            ++y;
        }
        int ovis = 3 + H - 1 - y;
        outLv.Clamp((int)o.size(), std::max(1, ovis));
        for (int i = 0; i < ovis && outLv.top + i < (int)o.size(); ++i) c.Put(x, y + i, o[(size_t)(outLv.top + i)], TX, false, Color(), x1);
        if (o.empty() && t.empty()) {
            for (const auto& l : Wrap("Все команды этой версии Yggdrasil. Ответ показывается как есть, в JSON - так можно сделать то, для чего в панели нет отдельной кнопки.", rw - 4))
                if (y < 3 + H - 1) c.Put(x, y++, l, DM, false, Color(), x1);
        }
        Footer(c, a, { { "↑↓", "команда" }, { "Enter", "выполнить" }, { "Tab", "к ответу" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        auto cmds = List(a);
        if (k.k == K::Tab) { outFocus = !outFocus; return true; }
        if (outFocus) {
            size_t n;
            { std::lock_guard<std::mutex> lk(*mu); n = out->size(); }
            if (k.k == K::Up) { outLv.top = std::max(0, outLv.top - 1); outLv.sel = outLv.top; return true; }
            if (k.k == K::Down) { outLv.top = std::min((int)n - 1, outLv.top + 1); outLv.sel = outLv.top; return true; }
            if (k.k == K::PgUp) { outLv.top = std::max(0, outLv.top - 10); outLv.sel = outLv.top; return true; }
            if (k.k == K::PgDn) { outLv.top = std::min((int)n - 1, outLv.top + 10); outLv.sel = outLv.top; return true; }
        } else if (lv.Handle(k, (int)cmds.size(), a.term.canvas.h - 7)) return true;
        if (k.k == K::Enter && lv.sel < (int)cmds.size()) {
            Cmd cmd = cmds[(size_t)lv.sel];
            if (cmd.fields.empty()) { RunCmd(a, cmd, {}); return true; }
            std::vector<std::pair<std::string, std::string>> f;
            for (const auto& x : cmd.fields) f.push_back({ x, "" });
            a.Ask(cmd.name, cmd.desc.empty() ? "Параметры команды (пустые не передаются):" : cmd.desc, f,
                  [this, &a, cmd](const std::vector<std::string>& v) { RunCmd(a, cmd, v); });
            return true;
        }
        return false;
    }
};
Screen* MakeCommands() { return new CommandsScreen(); }

// ------------------------------------------------------------------ сторож: состояние и журнал событий
struct WdEventsScreen : Screen {
    int top = 0;
    void Draw(Canvas& c, App& a) override {
        const Snap& s = a.s;
        Header(c, "СТОРОЖ ПИРОВ", s.wdExe.empty() ? "ygg-watchdog не найден" : s.wdExe);
        int W = c.w - 4, x = 4, x1 = 2 + W - 1;
        if (s.wdExe.empty()) {
            c.Box(2, 3, W, 7, "сторож не найден", GF, G);
            int y = 4;
            for (const auto& l : Wrap("ygg-watchdog сам следит за пирами: ищет замену упавшим, перебирает страны, шлёт уведомления в Telegram. Панель находит его "
                                      "по /etc/yozmor/watchdog_path (его пишет установка сторожа) или по команде ygg-watchdog. Репозиторий: github.com/Yozmor/ygg-watchdog", W - 4))
                c.Put(x, y++, l, TX, false, Color(), x1);
            Footer(c, a, { { "Esc", "назад" } });
            return;
        }
        const json::Value& st = s.wd;
        std::string word; Color col;
        WdPhase(s, word, col);
        c.Box(2, 3, W, 8, "состояние", GF, G);
        c.Seq(x, 4, { { "● ", col }, { word, col, true }, { "   версия ", DM }, { st["version"].str().empty() ? "—" : "v" + st["version"].str(), TX } }, x1);
        long long lt = st["last_tick_at"].i64(), next = st["timer"]["next"].i64(), now = util::Now();
        c.Seq(x, 5, { { "последняя проверка ", DM }, { lt ? util::TimeText(lt, "%H:%M:%S") + " (" + util::Ago(now - lt) + " назад)" : "ещё не было", TX },
                      { "   следующая ", DM }, { !st["timer"]["active"].boolean() ? "таймер выключен" : st["tick_running"].boolean() ? "идёт сейчас" : next > now ? util::MinSec(next - now) : "—",
                                                  st["timer"]["active"].boolean() ? TX : OR } }, x1);
        const json::Value& mp = st["main_peers"];
        int up = 0;
        for (size_t i = 0; i < mp.size(); ++i)
            for (size_t j = 0; j < s.peers.size(); ++j) if (s.peers[j]["remote"].str() == mp[i].str() && s.peers[j]["up"].boolean()) { ++up; break; }
        c.Seq(x, 6, { { "основные пиры ", DM }, { std::to_string(up) + " / " + std::to_string(mp.size()), up ? G : RD }, { "   резервные ", DM },
                      { std::to_string(st["backup_peers"].size()), TX } }, x1);
        const json::Value& dg = st["diagnosis"];
        if (!dg["verdict"].str().empty() && (!dg["any_up"].boolean() || !dg["advice"].str().empty()))
            c.Seq(x, 7, { { "▲ ", dg["any_up"].boolean() ? OR : RD }, { dg["verdict"].str(), dg["any_up"].boolean() ? OR : RD } }, x1);
        c.Seq(x, 9, { { "[W]", G, true }, { " открыть сторожа   ", TX }, { "[T]", G, true }, { " проверить сейчас   ", TX },
                      { st["timer"]["active"].boolean() ? "[x]" : "[i]", G, true }, { st["timer"]["active"].boolean() ? " выключить автопроверки" : " включить автопроверки", TX } }, x1);
        // события
        int H = c.h - 14;
        c.Box(2, 11, W, H + 2, "журнал событий · то же, что уходит в Telegram", GF, G);
        std::vector<std::string> ev(s.wdEvents.rbegin(), s.wdEvents.rend());   // новые сверху
        top = std::max(0, std::min(top, std::max(0, (int)ev.size() - 1)));
        // длинные события переносятся целиком - текст ошибки должен быть виден
        int y = 12;
        for (int i = top; i < (int)ev.size() && y < 12 + H; ++i) {
            auto f = util::SplitKeep(ev[(size_t)i], '\t');
            if (f.size() < 3) continue;
            Color ec = f[1] == "ok" ? G : f[1] == "warn" ? OR : f[1] == "alert" ? RD : DM;
            const char* ic = f[1] == "ok" ? "●" : f[1] == "warn" ? "◐" : f[1] == "alert" ? "▲" : "·";
            c.Seq(x, y, { { util::TimeText(util::ToLL(f[0]), "%d.%m %H:%M") + " ", DM }, { std::string(ic) + " ", ec } }, x1);
            for (const auto& l : Wrap(f[2], x1 - x - 14)) { if (y >= 12 + H) break; c.Put(x + 14, y++, l, ec == DM ? TX : ec, false, Color(), x1); }
        }
        if (ev.empty()) c.Put(x, 12, "событий пока нет", DM);
        Footer(c, a, { { "W", "открыть" }, { "T", "проверить" }, { "↑↓", "листать" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.is('W') || k.is('w') || k.text == "ц" || k.text == "Ц") { OpenWatchdog(a); return true; }
        if (k.is('T') || k.is('t') || k.text == "е" || k.text == "Е") { WatchdogTick(a); return true; }
        if (k.k == K::Up) { --top; return true; }
        if (k.k == K::Down) { ++top; return true; }
        if (k.k == K::PgUp) { top -= 10; return true; }
        if (k.k == K::PgDn) { top += 10; return true; }
        if (!a.s.wdExe.empty() && (k.is('x') || k.text == "ч") && a.s.wd["timer"]["active"].boolean()) {
            std::string exe = a.s.wdExe;
            a.Confirm("выключить автопроверки", "Остановить таймер сторожа? Он перестанет сам следить за пирами, пока не включишь снова.\n\nEnter - выключить", [&a, exe] {
                a.Op("выключаю автопроверки", [exe] { util::ProcResult r = util::Run({ exe, "stop-timer" }, 60000); return r.code == 0 ? std::string() : "!" + util::U8Left(util::Trim(r.out), 160); },
                     "Автопроверки выключены.");
            }, true);
            return true;
        }
        if (!a.s.wdExe.empty() && (k.is('i') || k.text == "ш") && !a.s.wd["timer"]["active"].boolean()) {
            std::string exe = a.s.wdExe;
            a.Op("включаю автопроверки", [exe] { util::ProcResult r = util::Run({ exe, "start-timer" }, 60000); return r.code == 0 ? std::string() : "!" + util::U8Left(util::Trim(r.out), 160); },
                 "Автопроверки включены: сторож проверяет каждые 3 минуты.");
            return true;
        }
        return false;
    }
};
Screen* MakeWdEvents() { return new WdEventsScreen(); }

} // namespace pnl
