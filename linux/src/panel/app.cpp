// app.cpp - каркас окна панели в терминале: экраны, диалоги, фоновые
// действия, конфиг в памяти, главное меню (по макету). Экраны - screens*.cpp.
#include "data.h"
#include "../common/tui.h"

namespace pnl {
using namespace tui;
using P = tui::Canvas::Seg;
const Color B = pal::BLUE, BF = pal::BFRAME, BH = pal::BHL, G = pal::GREEN, GF = pal::GFRAME, GH = pal::GHL,
            TX = pal::TXT, DM = pal::DIM, DM2 = pal::DIM2, WH = pal::WHITE, OR = pal::ORANGE, RD = pal::RED;

class App;
struct Screen {
    virtual ~Screen() {}
    virtual void Draw(Canvas& c, App& a) = 0;
    virtual bool Key(const tui::Key& k, App& a) = 0;    // true - клавиша обработана
    virtual void OnShow(App&) {}
    virtual bool Typing() const { return false; }        // идёт ввод текста - буквы не горячие клавиши
    virtual bool CanLeave(App&) { return true; }         // Esc: можно ли уйти (конфиг спросит)
};

// Диалог: подтверждение или поля ввода
struct Modal {
    bool open = false;
    std::string title, text;
    std::vector<std::pair<std::string, LineEdit>> fields;
    int focus = 0;
    bool danger = false;
    std::string okLabel = "Enter - да";
    std::function<void(const std::vector<std::string>&)> onOk;
};

// Фоновое действие: work() в потоке; "" - успех, "!текст" - ошибка, иначе - сообщение
struct Job {
    std::string label;
    std::atomic<bool> done{ false };
    std::string result;
    std::function<void(const std::string&)> after;
    bool quiet = false;
};

class App {
public:
    Terminal term;
    Data data;
    Snap s;                                   // снимок для этого кадра
    std::vector<std::unique_ptr<Screen>> stack;
    std::vector<std::shared_ptr<Job>> jobs;
    Modal modal;
    std::string flash;
    Color flashColor = TX;
    long long flashAt = 0;
    bool quit = false;
    int spin = 0;

    // конфиг: прочитанный и правленый
    bool cfgLoaded = false, cfgLoading = false;
    std::string cfgErr;
    json::Value cfgOrig, cfgEdit;

    // пинги: адрес -> мс (-1 нет ответа, -2 идёт)
    std::mutex pingMu;
    std::map<std::string, double> pingMs;

    // адресная книга
    std::vector<BookEntry> book;
    bool bookLoaded = false;

    // обновления
    PanelUpdate pupd;
    ygg::UpdateInfo yupd;
    bool pupdChecking = false, yupdChecking = false;

    // строка внизу - одна: переводы строк (вывод apt и т.п.) склеиваются, иначе ломают экран
    void Flash(const std::string& t, Color c = TX) { flash = util::OneLine(t); flashColor = c; flashAt = util::Now(); }
    void Push(Screen* sc) { stack.emplace_back(sc); sc->OnShow(*this); }
    void Pop() { if (stack.size() > 1) stack.pop_back(); if (!stack.empty()) stack.back()->OnShow(*this); }

    bool Busy(const std::string& label = "") {
        for (auto& j : jobs) if (!j->done && !j->quiet && (label.empty() || j->label == label)) return true;
        return false;
    }
    // Действие, которое что-то меняет: по одному с одинаковой подписью, итог - строкой внизу
    void Op(const std::string& label, std::function<std::string()> work, const std::string& okMsg = "",
            std::function<void(const std::string&)> after = nullptr) {
        if (Busy(label)) { Flash("уже идёт: " + label, OR); return; }
        auto j = std::make_shared<Job>();
        j->label = label;
        j->after = [this, okMsg, after](const std::string& res) {
            bool bad = !res.empty() && res[0] == '!';
            if (bad) Flash("Ошибка: " + res.substr(1), RD);
            else Flash(res.empty() ? (okMsg.empty() ? label_ok() : okMsg) : res, G);
            data.RefreshNow();
            if (after) after(res);
        };
        jobs.push_back(j);
        std::thread([j, work] { try { j->result = work(); } catch (...) { j->result = "!внутренняя ошибка"; } j->done = true; }).detach();
    }
    // Тихая работа в фоне (чтение, проверка) - без сообщения
    void Bg(std::function<std::string()> work, std::function<void(const std::string&)> after) {
        auto j = std::make_shared<Job>();
        j->quiet = true;
        j->after = after;
        jobs.push_back(j);
        std::thread([j, work] { try { j->result = work(); } catch (...) { j->result = "!внутренняя ошибка"; } j->done = true; }).detach();
    }
    static std::string label_ok() { return "Готово."; }
    void FinishJobs() {
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (!(*it)->done) { ++it; continue; }
            auto j = *it;
            it = jobs.erase(it);
            if (j->after) j->after(j->result);
        }
    }

    void Confirm(const std::string& title, const std::string& text, std::function<void()> ok, bool danger = false) {
        modal = Modal();
        modal.open = true; modal.title = title; modal.text = text; modal.danger = danger;
        modal.onOk = [ok](const std::vector<std::string>&) { ok(); };
    }
    void Ask(const std::string& title, const std::string& text, std::vector<std::pair<std::string, std::string>> fields,
             std::function<void(const std::vector<std::string>&)> ok) {
        modal = Modal();
        modal.open = true; modal.title = title; modal.text = text;
        for (auto& f : fields) { LineEdit e; e.text = f.second; modal.fields.push_back({ f.first, e }); }
        modal.okLabel = "Enter - готово";
        modal.onOk = ok;
    }

    // ---- конфиг
    void ReloadConfig() {
        if (cfgLoading) return;
        cfgLoading = true;
        ygg::Paths p = ygg::CtlPaths();
        auto holder = std::make_shared<json::Value>();
        Bg([p, holder] { std::string err; return ygg::LoadConfig(p, *holder, err) ? std::string() : "!" + err; },
           [this, holder](const std::string& res) {
               cfgLoading = false;
               if (!res.empty()) { cfgErr = res.substr(1); cfgLoaded = false; return; }
               cfgOrig = *holder; cfgEdit = cfgOrig; cfgLoaded = true; cfgErr.clear();
           });
    }
    bool InConfigPeers(const std::string& uri) const {
        const json::Value& p = cfgOrig["Peers"];
        for (size_t i = 0; i < p.size(); ++i) if (p[i].str() == uri) return true;
        return false;
    }
    // Дописать пиры в Peers и сохранить (с проверкой и копией старого конфига)
    void AddPeersToConfig(std::vector<std::string> uris, const std::string& okMsg) {
        if (!cfgLoaded) { Flash("Конфиг ещё не прочитан: " + (cfgErr.empty() ? "читаю…" : cfgErr), RD); ReloadConfig(); return; }
        json::Value cfg = cfgOrig;
        Op("записываю в конфиг", [cfg, uris]() mutable -> std::string {
            json::Value& arr = cfg.at("Peers");
            if (!arr.isArray()) arr = json::MakeArray();
            for (const auto& u : uris) {
                bool has = false;
                for (const auto& x : arr.arr) if (x.str() == u) has = true;
                if (!has) arr.arr.push_back(json::MakeString(u));
            }
            std::string e = ygg::SaveConfig(ygg::CtlPaths(), cfg);
            return e.empty() ? "" : "!" + e;
        }, okMsg, [this](const std::string&) { cfgLoaded = false; ReloadConfig(); });
    }

    // ---- пинг (до 3 попыток; ports - если пинг молчит, пробуем SSH/RDP/SMB)
    void PingAsync(const std::string& addr, bool ports = false) {
        if (addr.empty()) return;
        { std::lock_guard<std::mutex> lk(pingMu); pingMs[addr] = -2; }
        std::thread([this, addr, ports] {
            double best = -1;
            for (int i = 0; i < 3; ++i) { net::PingResult r = net::Ping6(addr, 3000); if (r.ok && (best < 0 || r.ms < best)) best = r.ms; }
            if (best < 0 && ports)
                for (int port : { 22, 3389, 445 }) {
                    double ms = -1;
                    int fd = net::ConnectTcp(addr, port, 2000, nullptr, &ms);
                    if (fd >= 0) { close(fd); best = ms; break; }
                }
            std::lock_guard<std::mutex> lk(pingMu);
            pingMs[addr] = best;
        }).detach();
    }
    double PingOf(const std::string& addr) {
        std::lock_guard<std::mutex> lk(pingMu);
        auto it = pingMs.find(addr);
        return it == pingMs.end() ? -3 : it->second;
    }
    std::string PingText(const std::string& addr) {
        double v = PingOf(addr);
        if (v == -3) return "";
        if (v == -2) return "пинг…";
        if (v < 0) return "пинг: нет ответа";
        return "пинг: " + util::Fixed(v, 0) + " мс";
    }

    // ---- адресная книга
    void EnsureBook() { if (!bookLoaded) { book = LoadBook(); bookLoaded = true; } }
    std::string BookName(const std::string& addr) {
        if (addr.empty()) return "";
        EnsureBook();
        std::string n = NormAddr(addr);
        for (const auto& e : book) if (NormAddr(e.addr) == n) return e.name;
        const json::Value& d = s.wd["devices"];
        for (size_t i = 0; i < d.size(); ++i) if (NormAddr(d[i]["host"].str()) == n) return d[i]["name"].str();
        return "";
    }

    // ---- буфер обмена: OSC 52 (работает в большинстве терминалов, и через SSH)
    void Copy(const std::string& text, const std::string& what) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        int val = 0, bits = -6;
        for (unsigned char c : text) {
            val = (val << 8) + c; bits += 8;
            while (bits >= 0) { b64 += tbl[(val >> bits) & 0x3F]; bits -= 6; }
        }
        if (bits > -6) b64 += tbl[((val << 8) >> (bits + 8)) & 0x3F];
        while (b64.size() % 4) b64 += '=';
        term.Raw("\x1b]52;c;" + b64 + "\x07");
        Flash(what + " скопирован: " + text + "  (если терминал не поддерживает буфер - выдели мышью)", G);
    }

    // ---- обновления
    void CheckPanelUpdate(bool quiet) {
        if (pupdChecking) return;
        pupdChecking = true;
        auto h = std::make_shared<PanelUpdate>();
        Bg([h] { *h = pnl::CheckPanelUpdate(); return std::string(); }, [this, h, quiet](const std::string&) {
            pupdChecking = false;
            pupd = *h;
            bool nw = pupd.ok && util::VersionNewer(PANEL_VERSION, pupd.tag);
            if (nw) Flash("В репозитории новая версия панели v" + pupd.tag + " - [u] Обновления.", G);
            else if (!quiet) Flash(pupd.ok ? "Панель последней версии (в репозитории v" + pupd.tag + ")" : "Панель: " + pupd.err, pupd.ok ? TX : OR);
        });
    }
    void CheckYggUpdate(bool quiet) {
        if (yupdChecking) return;
        yupdChecking = true;
        auto h = std::make_shared<ygg::UpdateInfo>();
        Bg([h] { *h = ygg::CheckYggUpdate(); return std::string(); }, [this, h, quiet](const std::string&) {
            yupdChecking = false;
            yupd = *h;
            bool nw = yupd.ok && !s.version.empty() && util::VersionNewer(s.version, yupd.tag);
            if (nw) Flash("Вышел Yggdrasil " + yupd.tag + " - [u] Обновления.", G);
            else if (!quiet) Flash(yupd.ok ? "Yggdrasil: новее нет (" + yupd.tag + ")" : "Yggdrasil: " + yupd.err, yupd.ok ? TX : OR);
        });
    }
    int UpdatesCount() const {
        int n = 0;
        if (pupd.ok && util::VersionNewer(PANEL_VERSION, pupd.tag)) ++n;
        if (yupd.ok && !s.version.empty() && util::VersionNewer(s.version, yupd.tag)) ++n;
        return n;
    }

    // ---- отдать терминал другой программе (окно сторожа, less)
    int RunInTerminal(const std::vector<std::string>& argv) {
        term.Suspend();
        pid_t pid = fork();
        int code = -1;
        if (pid == 0) {
            std::vector<char*> a;
            for (auto& x : argv) a.push_back(const_cast<char*>(x.c_str()));
            a.push_back(nullptr);
            execvp(a[0], a.data());
            _exit(127);
        }
        if (pid > 0) { int st = 0; waitpid(pid, &st, 0); code = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
        term.Resume();
        return code;
    }

    // ---- рисование общих частей
    void DrawModal(Canvas& c);
    bool ModalKey(const tui::Key& k);
    int Main();
};

// Пословный перенос
inline std::vector<std::string> Wrap(const std::string& text, int w) {
    std::vector<std::string> out;
    for (const auto& para : util::SplitKeep(text, '\n')) {
        std::string cur;
        for (const auto& word : util::Split(para, ' ')) {
            if (!cur.empty() && (int)(util::U8Len(cur) + util::U8Len(word) + 1) > w) { out.push_back(cur); cur.clear(); }
            cur += (cur.empty() ? "" : " ") + word;
        }
        out.push_back(cur);
    }
    return out;
}

void App::DrawModal(Canvas& c) {
    int w = std::min(c.w - 6, 76);
    auto wrapped = Wrap(modal.text, w - 4);
    int lines = (int)wrapped.size();
    int h = 4 + lines + (int)modal.fields.size() * 2 + (modal.fields.empty() ? 0 : 1);
    int x = (c.w - w) / 2, y = std::max(1, (c.h - h) / 2);
    c.FillRect(x, y, x + w, y + h, Hex("#11161d"));
    Color fr = modal.danger ? OR : B;
    c.Box(x, y, w, h, modal.title, fr, fr);
    for (int i = 0; i < lines; ++i) c.Put(x + 2, y + 1 + i, wrapped[(size_t)i], TX, false, Color(), x + w - 2);
    int fy = y + 2 + lines;
    for (size_t i = 0; i < modal.fields.size(); ++i) {
        bool f = (int)i == modal.focus;
        c.Put(x + 2, fy, modal.fields[i].first, DM);
        std::string t = modal.fields[i].second.text;
        size_t maxw = (size_t)(w - 6);
        std::string shown = t;
        if (util::U8Len(shown) > maxw) { auto cps = util::U8Decode(shown); shown = "…"; for (size_t k = cps.size() - (maxw - 1); k < cps.size(); ++k) shown += util::U8Encode(cps[k]); }
        c.FillBg(x + 2, fy + 1, x + w - 2, f ? BH : Hex("#161b22"));
        int ex = c.Put(x + 3, fy + 1, shown, WH, false, Color(), x + w - 2);
        if (f) c.Put(ex, fy + 1, "▌", B);
        fy += 2;
    }
    c.Seq(x + 2, y + h - 1, { { " " + modal.okLabel, fr, true }, { "  Esc - отмена ", DM } });
}

bool App::ModalKey(const tui::Key& k) {
    if (k.k == K::Esc) { modal.open = false; return true; }
    if (k.k == K::Enter) {
        if (!modal.fields.empty() && modal.focus + 1 < (int)modal.fields.size()) { ++modal.focus; return true; }
        std::vector<std::string> vals;
        for (auto& f : modal.fields) vals.push_back(util::Trim(f.second.text));
        auto cb = modal.onOk;
        modal.open = false;
        if (cb) cb(vals);
        return true;
    }
    if (modal.fields.empty()) {
        if (k.is('y') || k.text == "д" || k.text == "Д") { auto cb = modal.onOk; modal.open = false; if (cb) cb({}); }
        return true;
    }
    if (k.k == K::Tab || k.k == K::Down) { modal.focus = (modal.focus + 1) % (int)modal.fields.size(); return true; }
    if (k.k == K::BackTab || k.k == K::Up) { modal.focus = (modal.focus + (int)modal.fields.size() - 1) % (int)modal.fields.size(); return true; }
    modal.fields[(size_t)modal.focus].second.Handle(k);
    return true;
}

// ------------------------------------------------------------------ общие элементы экранов
// Заголовок экрана: «▲ ДЕРЕВО МАРШРУТОВ   getTree · 214 узлов»
inline void Header(Canvas& c, const std::string& title, const std::string& sub) {
    c.Seq(3, 1, { { "▲ ", B, true }, { title, B, true }, { "   " + sub, DM } }, c.w - 2);
}
// Подсказки внизу (или сообщение, или идущее действие справа)
inline void Footer(Canvas& c, App& a, const std::vector<std::pair<std::string, std::string>>& items) {
    int y = c.h - 1;
    if (!a.flash.empty() && util::Now() - a.flashAt < 9) {
        c.Put(3, y, util::U8Fit(a.flash, (size_t)std::max(10, c.w - 6)), a.flashColor);
        return;
    }
    int x = 3;
    for (const auto& [k, v] : items) {
        if (x + (int)util::U8Len(k) + (int)util::U8Len(v) + 2 > c.w - 2) break;
        x = c.Put(x, y, k, B, true) + 1;
        x = c.Put(x, y, v, DM) + 3;
    }
    std::string busy;
    for (auto& j : a.jobs) if (!j->done && !j->quiet) { busy = j->label; break; }
    if (!busy.empty()) {
        static const char* sp[] = { "◐", "◓", "◑", "◒" };
        std::string t = std::string(sp[(a.spin / 3) % 4]) + " " + busy + "…";
        c.Put(std::max(x + 2, c.w - 2 - (int)util::U8Len(t)), y, t, OR);
    }
}
// Нет связи с Yggdrasil - честно говорим, а не показываем пустые таблицы
inline bool NoCtl(Canvas& c, App& a, const std::string& title) {
    if (a.s.ctlOk) return false;
    Header(c, title, "");
    c.Box(2, 3, c.w - 4, 9, "нет связи с Yggdrasil", OR, OR);
    std::string why = !a.s.svc.exists ? "Служба yggdrasil не найдена." : a.s.svc.running() ? "Служба работает, но не отвечает." : "Служба не запущена (" + a.s.svc.active + ").";
    c.Put(4, 5, why, TX, false, Color(), c.w - 3);
    c.Put(4, 6, "yggdrasilctl: " + (a.s.ctlErr.empty() ? std::string("не ответил") : a.s.ctlErr), DM, false, Color(), c.w - 3);
    if (!a.s.paths.adminListen.empty()) c.Put(4, 7, "адрес управления (AdminListen): " + a.s.paths.adminListen, DM, false, Color(), c.w - 3);
    if (a.s.svc.exists && !a.s.svc.running()) c.Seq(4, 9, { { "[g]", B, true }, { " запустить службу", TX } });
    Footer(c, a, { { "g", "запустить службу" }, { "Esc", "назад" } });
    return true;
}
inline bool NoCtlKey(const tui::Key& k, App& a) {
    if (a.s.ctlOk) return false;
    if (k.is('g') || k.text == "п") {
        a.Op("запускаю службу", [] { std::string e = ygg::ServiceCtl("start"); return e.empty() ? std::string() : "!" + e; }, "Служба запущена.");
        return true;
    }
    return false;
}
// Строка таблицы с подсветкой выбранной
inline void RowBg(Canvas& c, int x0, int y, int x1, bool on) {
    if (!on) return;
    c.FillBg(x0, y, x1, BH);
    c.Put(x0, y, "▸", B);
}
inline Color RttColor(double ms) { return ms < 0 ? DM2 : ms < 120 ? G : ms < 300 ? OR : RD; }

// Состояние сторожа одним словом
inline void WdPhase(const Snap& s, std::string& word, Color& col) {
    const json::Value& st = s.wd;
    std::string ph = st["last_phase"].str();
    long long lt = st["last_tick_at"].i64();
    bool stale = lt > 0 && util::Now() - lt > 15 * 60;
    word = "?"; col = DM;
    if (s.wdExe.empty()) word = "не найден";
    else if (!s.wdOk) { word = "не отвечает"; col = RD; }
    else if (!st["timer"]["active"].boolean() && !st["timer"]["enabled"].boolean()) { word = "автопроверки выкл."; col = OR; }
    else if (stale) { word = "молчит " + util::Ago(util::Now() - lt); col = RD; }
    else if (ph == "ok") { word = "активен"; col = G; }
    else if (ph == "backup") { word = "на резервных"; col = G; }
    else if (ph == "down") { word = "ищет замену"; col = OR; }
    else if (ph == "crisis") { word = "крайний перебор"; col = OR; }
    else if (ph == "no_internet") { word = "нет интернета"; col = RD; }
    else if (ph == "battery") { word = "батарея"; col = RD; }
    else if (ph == "service_down") { word = "служба стоит"; col = RD; }
    else if (lt == 0) { word = "ещё не проверял"; col = OR; }
}

// ------------------------------------------------------------------ экраны (объявления)
Screen* MakeSelf(); Screen* MakePeers(); Screen* MakeHistory(); Screen* MakeTree(); Screen* MakeSessions();
Screen* MakePaths(); Screen* MakeBook(); Screen* MakeRemote(const std::string& key = ""); Screen* MakeIfaces();
Screen* MakeService(); Screen* MakeLog(); Screen* MakeConfig(const std::string& field = ""); Screen* MakeKey();
Screen* MakeCommands(); Screen* MakeUpdates(); Screen* MakeWdEvents(); Screen* MakeBookAdd(const std::string& addr);

void WatchdogTick(App& a) {
    if (a.s.wdExe.empty()) { a.Flash("Сторож не найден - установи ygg-watchdog.", OR); return; }
    std::string exe = a.s.wdExe;
    a.Op("сторож проверяет", [exe] {
        util::ProcResult r = util::Run({ exe, "tick" }, 10 * 60 * 1000);
        return r.started ? std::string() : std::string("!сторож не запустился");
    }, "Сторож проверил связь.");
}
void OpenWatchdog(App& a) {
    if (a.s.wdExe.empty()) { a.Flash("Сторож не найден - установи ygg-watchdog.", OR); return; }
    a.RunInTerminal({ a.s.wdExe });
    a.data.RefreshNow();
}

// ------------------------------------------------------------------ главное меню
struct MainScreen : Screen {
    struct Item { std::string key, name, hint; int box; };   // box: 0 узел и сеть, 1 служба, 2 настройка, 3 сторож
    std::vector<Item> items = {
        { "1", "Статус узла", "getSelf", 0 }, { "2", "Пиры", "getPeers", 0 }, { "3", "История задержек", "сторож", 0 },
        { "4", "Дерево маршрутов", "getTree", 0 }, { "5", "Сессии", "getSessions", 0 }, { "6", "Таблица путей", "getPaths", 0 },
        { "7", "Устройства", "адресная книга", 0 }, { "8", "Удалённый узел", "getNodeInfo", 0 }, { "9", "Интерфейсы", "TUN, multicast", 0 },
        { "s", "Служба Yggdrasil", "", 1 },
        { "l", "Лог (journalctl)", "", 2 }, { "c", "Конфиг", "", 2 }, { "k", "Ключ узла", "", 2 }, { "a", "Все команды", "", 2 },
        { "u", "Обновления", "", 2 },
        { "W", "Открыть сторожа", "", 3 }, { "T", "Проверить сейчас", "", 3 }, { "V", "Журнал событий", "", 3 },
    };
    int sel = 0;
    bool searching = false;
    LineEdit search;

    bool Typing() const override { return searching; }
    bool Match(const Item& it) const {
        if (!searching || search.text.empty()) return true;
        std::string q = util::U8Lower(search.text);
        return util::Contains(util::U8Lower(it.name + " " + it.hint), q);
    }
    void Open(int i, App& a) {
        const std::string& k = items[(size_t)i].key;
        searching = false; search.text.clear();
        if (k == "1") a.Push(MakeSelf()); else if (k == "2") a.Push(MakePeers()); else if (k == "3") a.Push(MakeHistory());
        else if (k == "4") a.Push(MakeTree()); else if (k == "5") a.Push(MakeSessions()); else if (k == "6") a.Push(MakePaths());
        else if (k == "7") a.Push(MakeBook()); else if (k == "8") a.Push(MakeRemote()); else if (k == "9") a.Push(MakeIfaces());
        else if (k == "s") a.Push(MakeService()); else if (k == "l") a.Push(MakeLog()); else if (k == "c") a.Push(MakeConfig());
        else if (k == "k") a.Push(MakeKey()); else if (k == "a") a.Push(MakeCommands()); else if (k == "u") a.Push(MakeUpdates());
        else if (k == "W") OpenWatchdog(a); else if (k == "T") WatchdogTick(a); else if (k == "V") a.Push(MakeWdEvents());
    }

    void DrawItem(Canvas& c, App& a, int i, int x, int y, int x1) {
        const Item& it = items[(size_t)i];
        bool on = i == sel, m = Match(it);
        Color kc = it.box == 3 ? G : B;
        if (on) { c.FillBg(x - 1, y, x1, it.box == 3 ? GH : BH); c.Put(x - 1, y, "▸", kc); }
        c.Put(x + 1, y, "[" + it.key + "]", m ? kc : DM2, on);
        int nx = c.Put(x + 5, y, it.name, !m ? DM2 : on ? WH : TX, on, Color(), x1);
        if (!it.hint.empty() && x1 - x > 44) c.Put(x + 30, y, it.hint, m ? DM : DM2, false, Color(), x1);
        if (it.key == "u") { int n = a.UpdatesCount(); if (n) c.Put(std::max(nx + 2, x1 - 9), y, std::to_string(n) + " новое", G, false, Color(), x1); }
        if (it.key == "T" && a.Busy("сторож проверяет")) c.Put(nx + 1, y, "(идёт…)", OR);
    }

    void Draw(Canvas& c, App& a) override {
        const Snap& s = a.s;
        int W = c.w;
        // ---- шапка: логотип, имя, сводка узла (в невысоком терминале - две строки без логотипа)
        bool compact = c.h < 29;
        int up0 = PeersUp(s);
        if (compact) {
            c.Seq(3, 1, { { "YGGDRASIL", B, true }, { " " + (s.version.empty() ? std::string("?") : s.version), DM },
                          { "   панель управления v" + std::string(PANEL_VERSION), DM } }, W - 2);
            double rxr = s.traffic.empty() ? 0 : s.traffic.back().rx, txr = s.traffic.empty() ? 0 : s.traffic.back().tx;
            c.Seq(3, 2, { { s.ctlOk ? s.self["address"].str() : "нет связи с Yggdrasil", s.ctlOk ? WH : RD }, { "   ● ", up0 ? G : RD },
                          { std::to_string(up0) + " на связи", up0 ? G : RD }, { "   ↓ " + Rate(rxr) + "  ↑ " + Rate(txr), DM } }, W - 2);
        } else {
            const char* logo[] = { "  ▄▄▄▄▄  ", "▄▄▄╲│╱▄▄▄", " ▀▀╲│╱▀▀ ", "    █    ", "  ╱╱█╲╲  " };   // как на макете
            for (int i = 0; i < 5; ++i) c.Put(3, 1 + i, logo[i], B);
            c.Seq(16, 2, { { "YGGDRASIL", B, true }, { " " + (s.version.empty() ? std::string("?") : s.version), DM } });
            c.Seq(16, 3, { { "панель управления  ", DM }, { std::string("v") + PANEL_VERSION, DM } });
        }
        if (!compact && W >= 84) {
            int rx = W - 44;
            c.Seq(rx, 1, { { "адрес   ", DM }, { s.ctlOk ? s.self["address"].str() : "нет связи с Yggdrasil", s.ctlOk ? WH : RD } }, W - 2);
            int up = PeersUp(s);
            c.Seq(rx, 2, { { "пиры    ", DM }, { "● ", up ? G : RD }, { std::to_string(up) + " на связи", up ? G : RD },
                           { s.peers.size() > (size_t)up ? "  из " + std::to_string(s.peers.size()) : "", DM } }, W - 2);
            int d = RootDepth(s, s.self["key"].str());
            c.Seq(rx, 3, { { "дерево  ", DM }, { (d >= 0 ? "глубина " + std::to_string(d) : std::string("—")) + " · " + std::to_string(s.tree.size()) + " узлов", TX } }, W - 2);
            double rxr = s.traffic.empty() ? 0 : s.traffic.back().rx, txr = s.traffic.empty() ? 0 : s.traffic.back().tx;
            c.Seq(rx, 4, { { "трафик  ", DM }, { "↓ " + Rate(rxr) + "   ↑ " + Rate(txr), TX } }, W - 2);
        }
        // ---- колонки
        int top = compact ? 4 : 7;
        int lw = W >= 90 ? (W - 6) * 57 / 100 : (W - 5) / 2 + 4;
        int rx = 2 + lw + 2, rw = W - 2 - rx;
        int leftH = 9 + 2 + 2;                              // 9 пунктов, рамка, строка поиска
        c.Box(2, top, lw, leftH, "узел и сеть", BF, B);
        for (int i = 0; i < 9; ++i) DrawItem(c, a, i, 4, top + 1 + i, 2 + lw - 1);
        if (searching) c.Seq(4, top + leftH - 2, { { "/ ", B, true }, { search.text, WH }, { "▌", B } }, 2 + lw - 1);
        else c.Seq(4, top + leftH - 2, { { "/ ", B }, { "поиск по меню", DM } }, 2 + lw - 1);
        // служба
        c.Box(rx, top, rw, 5, "служба", BF, B);
        DrawItem(c, a, 9, rx + 2, top + 1, rx + rw - 1);
        std::vector<P> sv;
        if (!s.svc.exists) sv = { { "● ", RD }, { "не установлена", RD } };
        else if (s.svc.running()) sv = { { "● ", G }, { "работает", G }, { s.svc.autoStart() ? " · автозапуск" : " · без автозапуска", s.svc.autoStart() ? G : OR } };
        else sv = { { "● ", RD }, { s.svc.active == "failed" ? "упала" : "остановлена", RD }, { s.svc.autoStart() ? " · автозапуск" : "", DM } };
        c.Seq(rx + 2, top + 2, sv, rx + rw - 1);
        c.Seq(rx + 2, top + 3, { { "перезапуск: ", DM }, { "сразу / через 15 с", DM } }, rx + rw - 1);
        // настройка
        int ny = top + 5;
        int nh = leftH - 5;
        c.Box(rx, ny, rw, nh, "настройка", BF, B);
        for (int i = 10; i < 15 && ny + 1 + (i - 10) < ny + nh - 1; ++i) DrawItem(c, a, i, rx + 2, ny + 1 + (i - 10), rx + rw - 1);
        // ---- сторож
        int wy = top + leftH + 1;
        int wh = std::min(5, c.h - wy - 2);
        if (wh >= 4) {
            c.Box(2, wy, W - 4, wh, "сторож пиров · отдельная программа", GF, G);
            std::string word; Color col;
            WdPhase(s, word, col);
            std::vector<P> l = { { "● ", col }, { word, col, true } };
            if (s.wdOk) {
                long long next = s.wd["timer"]["next"].i64(), now = util::Now();
                if (s.wd["tick_running"].boolean()) l.push_back({ "   проверяет сейчас", OR });
                else if (next > now) { l.push_back({ "   проверка ", DM }); l.push_back({ util::MinSec(next - now), TX }); }
                std::string ph = s.wd["last_phase"].str();
                l.push_back({ "   режим ", DM }); l.push_back({ ph == "backup" ? "на резервных" : ph == "crisis" ? "перебор" : "обычный", TX });
                int alive = 0;
                for (size_t i = 0; i < s.peers.size(); ++i) if (s.peers[i]["up"].boolean()) ++alive;
                l.push_back({ "   пиры ", DM }); l.push_back({ std::to_string(alive) + " живых", TX });
                int silent = 0;
                for (size_t i = 0; i < s.wd["devices"].size(); ++i) if (s.wd["devices"][i]["down_since"].i64()) ++silent;
                if (s.wd["devices"].size()) {
                    l.push_back({ "   устройства ", DM });
                    if (silent) l.push_back({ "◌ " + std::to_string(silent) + " молчит", OR }); else l.push_back({ "● все на связи", G });
                }
            } else if (s.wdExe.empty()) l.push_back({ "   установи ygg-watchdog - он сам ищет замену упавшим пирам", DM });
            c.Seq(4, wy + 1, l, W - 3);
            int bx = 4, by = wy + wh - 2;
            for (int i = 15; i < 18; ++i) {
                const Item& it = items[(size_t)i];
                bool on = i == sel;
                if (on) c.FillBg(bx - 1, by, bx + 4 + (int)util::U8Len(it.name) + 1, GH);
                int e = c.Put(bx, by, "[" + it.key + "]", G, true);
                e = c.Put(e + 1, by, it.name, on ? WH : TX, on);
                if (it.key == "T" && a.Busy("сторож проверяет")) e = c.Put(e + 1, by, "(идёт…)", OR);
                bx = e + 6;
            }
        }
        if (searching) Footer(c, a, { { "печатай", "фильтр" }, { "Enter", "открыть" }, { "Esc", "выход из поиска" } });
        else Footer(c, a, { { "↑↓", "выбор" }, { "Enter", "открыть" }, { "буква", "быстрый пункт" }, { "W", "сторож" }, { "/", "поиск" }, { "q", "выход" } });
    }

    int NextMatch(int from, int dir) {
        int n = (int)items.size();
        for (int k = 1; k <= n; ++k) { int i = ((from + dir * k) % n + n) % n; if (Match(items[(size_t)i])) return i; }
        return from;
    }
    bool Key(const tui::Key& k, App& a) override {
        if (searching) {
            if (k.k == K::Esc) { searching = false; search.text.clear(); return true; }
            if (k.k == K::Enter) { if (Match(items[(size_t)sel])) Open(sel, a); return true; }
            if (k.k == K::Up) { sel = NextMatch(sel, -1); return true; }
            if (k.k == K::Down) { sel = NextMatch(sel, 1); return true; }
            if (search.Handle(k)) { if (!Match(items[(size_t)sel])) sel = NextMatch(sel, 1); return true; }
            return true;
        }
        if (k.k == K::Up) { sel = (sel + (int)items.size() - 1) % (int)items.size(); return true; }
        if (k.k == K::Down || k.k == K::Tab) { sel = (sel + 1) % (int)items.size(); return true; }
        if (k.k == K::Left && sel >= 15) { sel = sel == 15 ? 17 : sel - 1; return true; }
        if (k.k == K::Right && sel >= 15) { sel = sel == 17 ? 15 : sel + 1; return true; }
        if (k.k == K::Enter) { Open(sel, a); return true; }
        if (k.is('/')) { searching = true; search.text.clear(); return true; }
        if (k.k == K::Char) {
            // буквы и их русские двойники на той же клавише
            static const std::map<std::string, std::string> ru = { { "ы", "s" }, { "д", "l" }, { "с", "c" }, { "л", "k" }, { "ф", "a" }, { "г", "u" },
                                                                   { "Ц", "W" }, { "Е", "T" }, { "М", "V" }, { "ц", "W" }, { "е", "T" }, { "м", "V" } };
            std::string t = k.text;
            auto it = ru.find(t);
            if (it != ru.end()) t = it->second;
            if (t == "w") t = "W";
            if (t == "t") t = "T";
            if (t == "v") t = "V";
            for (int i = 0; i < (int)items.size(); ++i) if (items[(size_t)i].key == t) { sel = i; Open(i, a); return true; }
        }
        if (k.is('q') || k.text == "й") { a.quit = true; return true; }
        if (k.k == K::Esc) return true;            // Esc в меню ничего не закрывает - выход только q
        return false;
    }
};

} // namespace pnl
