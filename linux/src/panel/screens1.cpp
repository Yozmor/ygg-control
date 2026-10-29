// screens1.cpp - статус узла, пиры, добавление пира, история задержек.
namespace pnl {

// Столбиковый график в терминале: values по столбцам (-1 - нет данных),
// rows строк высотой, с восьмыми долями блока сверху. lo..hi - шкала.
inline void Columns(Canvas& c, int x, int y, int rows, const std::vector<double>& values, double lo, double hi, Color col, Color hot = Color(),
                    double hotFrom = -1) {
    if (hi - lo < 1e-9) hi = lo + 1;
    for (size_t i = 0; i < values.size(); ++i) {
        double v = values[i];
        if (v < 0) continue;
        double units = std::max(0.5, (v - lo) / (hi - lo) * rows * 8);
        Color cc = (!hot.none() && hotFrom >= 0 && v > hotFrom) ? hot : col;
        for (int r = 0; r < rows; ++r) {
            double cell = units - (rows - 1 - r) * 8;
            if (cell >= 8) c.Put(x + (int)i, y + r, "█", cc);
            else if (cell > 0) c.Put(x + (int)i, y + r, std::string(&"▁▂▃▄▅▆▇"[std::max(0, (int)cell - 1) * 3], 3), cc);
        }
    }
}
// «Круглый» верх шкалы: 1, 2 или 5 × 10^k
inline double NiceTop(double mx) {
    if (mx <= 0) return 1;
    double raw = mx;
    double mag = pow(10.0, floor(log10(raw)));
    double f = raw / mag;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * mag;
}

// ------------------------------------------------------------------ статус узла
struct SelfScreen : Screen {
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "СТАТУС УЗЛА")) return;
        const Snap& s = a.s;
        const json::Value& self = s.self;
        Header(c, "СТАТУС УЗЛА", "getSelf");
        int W = c.w - 4;
        c.Box(2, 3, W, 9, "узел", BF, B);
        auto row = [&](int y, const std::string& k, const std::string& v, Color vc = TX) {
            c.Put(4, y, k, DM);
            c.Put(18, y, v, vc, false, Color(), 2 + W - 1);
        };
        row(4, "адрес", self["address"].str(), WH);
        row(5, "подсеть", self["subnet"].str());
        row(6, "ключ", self["key"].str());
        row(7, "версия", self["build_name"].str() + " " + self["build_version"].str());
        row(8, "маршрутов", self["routing_entries"].text().empty() ? "—" : self["routing_entries"].text());
        std::string tun = "—";
        if (s.tun.isObject()) tun = s.tun["enabled"].boolean(true) ? s.tun["name"].str() + "   MTU " + s.tun["mtu"].text() : "TUN выключен";
        row(9, "интерфейс", tun);
        std::string svc = s.svc.running() ? "работает" : s.svc.active;
        if (s.svc.running() && s.svc.since) svc += " · " + Dur((double)(util::Now() - s.svc.since));
        row(10, "служба", svc, s.svc.running() ? G : RD);

        // плитки
        int depth = RootDepth(s, self["key"].str());
        struct Tile { const char* cap; std::string val; };
        Tile tiles[] = { { "пиры", std::to_string(PeersUp(s)) + " / " + std::to_string(s.peers.size()) },
                         { "глубина", depth >= 0 ? std::to_string(depth) : "—" },
                         { "путей", std::to_string(s.paths_.size()) }, { "сессий", std::to_string(s.sessions.size()) } };
        int tw = (W - 3 * 2) / 4;
        for (int i = 0; i < 4; ++i) {
            int tx = 2 + i * (tw + 2);
            c.Box(tx, 12, tw, 3, tiles[i].cap, BF, DM);
            c.Put(tx + 2, 13, tiles[i].val, WH, true);
        }

        // трафик за 60 минут: столбик - отрезок времени, верх шкалы - круглое число
        int gy = 15, gh = c.h - gy - 2;
        if (gh >= 7) {
            int cols = W - 12;
            std::vector<double> rx((size_t)cols, -1), tx((size_t)cols, -1);
            long long now = util::Now();
            for (const auto& p : s.traffic) {
                int i = (int)((p.t - (now - 3600)) * cols / 3600);
                if (i < 0 || i >= cols) continue;
                rx[(size_t)i] = std::max(rx[(size_t)i], p.rx);
                tx[(size_t)i] = std::max(tx[(size_t)i], p.tx);
            }
            double mx = 0;
            for (int i = 0; i < cols; ++i) mx = std::max(mx, std::max(rx[(size_t)i], tx[(size_t)i]));
            double top = NiceTop(mx);
            int rows = (gh - 4) / 2;
            c.Box(2, gy, W, gh, "трафик за 60 минут · сумма по пирам", BF, B);
            if (s.traffic.size() < 2) c.Put(5, gy + 2, "данные копятся - график появится через пару обновлений", DM);
            else {
                c.Put(4, gy + 1, "↓", B, true); c.Put(4, gy + 1 + rows, "↑", G, true);
                c.Put(6, gy + 1, util::U8Fit(Rate(top), 7), DM);
                c.Put(6, gy + 1 + rows, util::U8Fit(Rate(top), 7), DM);
                Columns(c, 14, gy + 1, rows, rx, 0, top, B);
                Columns(c, 14, gy + 1 + rows, rows, tx, 0, top, G);
            }
            double r1 = s.traffic.empty() ? 0 : s.traffic.back().rx, t1 = s.traffic.empty() ? 0 : s.traffic.back().tx;
            c.Seq(4, gy + gh - 1, { { " сейчас ", DM }, { "↓ " + Rate(r1), B }, { "  ↑ " + Rate(t1), G },
                                    { "   всего с подключения пиров ↓ " + Bytes(s.totalRx) + "  ↑ " + Bytes(s.totalTx) + " ", DM } }, 2 + W - 1);
        }
        Footer(c, a, { { "c", "копировать адрес" }, { "k", "копировать ключ" }, { "r", "обновить" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        if (k.is('c') || k.text == "с") { a.Copy(a.s.self["address"].str(), "Адрес"); return true; }
        if (k.is('k') || k.text == "л") { a.Copy(a.s.self["key"].str(), "Ключ"); return true; }
        if (k.is('r') || k.text == "к") { a.data.RefreshNow(); a.Flash("Обновляю…"); return true; }
        return false;
    }
};
Screen* MakeSelf() { return new SelfScreen(); }

// ------------------------------------------------------------------ пиры
Screen* MakeAddPeer();

struct PeersScreen : Screen {
    ListView lv;
    void OnShow(App& a) override { if (!a.cfgLoaded) a.ReloadConfig(); }
    int Visible(const Canvas& c) const { return std::max(3, c.h - 13); }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "ПИРЫ")) return;
        const json::Value& P = a.s.peers;
        int up = PeersUp(a.s);
        Header(c, "ПИРЫ", "getPeers · " + std::to_string(up) + " на связи из " + std::to_string(P.size()));
        int W = c.w - 4, vis = Visible(c);
        lv.Clamp((int)P.size(), vis);
        c.Box(2, 3, W, vis + 3, "пиры", BF, B);
        // столбцы: имя 14 | адрес | задержка 14 | трафик 19 | время 10
        int uriW = std::max(18, W - 66);
        int xL = 22 + uriW, xT = xL + 15, xU = xT + 20;
        c.Put(6, 4, "имя", DM); c.Put(21, 4, "адрес", DM); c.Put(xL, 4, "задержка", DM); c.Put(xT, 4, "трафик ↓ / ↑", DM); c.Put(xU, 4, "время", DM);
        for (int i = 0; i < vis && lv.top + i < (int)P.size(); ++i) {
            int idx = lv.top + i, y = 5 + i;
            const json::Value& p = P[(size_t)idx];
            bool on = idx == lv.sel, pu = p["up"].boolean();
            double lat = p["latency"].num(-1) / 1e6;
            std::string uri = p["remote"].str();
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Put(4, y, pu ? "●" : "◌", pu ? RttColor(lat) : DM2);
            std::string sub = p["inbound"].boolean() ? "вх" : a.cfgLoaded ? (a.InConfigPeers(uri) ? "" : "*") : "";
            c.Put(6, y, util::U8Fit(ShortName(uri) + (sub.empty() ? "" : " " + sub), 14), pu ? (on ? WH : TX) : DM, on);
            c.Put(21, y, util::U8Fit(uri, (size_t)uriW), pu ? B : DM);
            if (pu && lat >= 0) {
                c.Bar(xL, y, 6, std::max(0.08, 1.0 - std::min(lat, 400.0) / 400.0), RttColor(lat));
                c.Put(xL + 7, y, util::Fixed(lat, 0) + " мс", RttColor(lat));
            } else c.Put(xL, y, pu ? "—" : "не подключён", DM);
            if (pu) {
                c.Put(xT, y, util::U8Fit(Bytes(p["bytes_recvd"].num()) + " / " + Bytes(p["bytes_sent"].num()), 19), TX);
                c.Put(xU, y, DurShort(p["uptime"].num(-1)), DM, false, Color(), 2 + W - 1);
            }
        }
        if (P.size() == 0) c.Put(5, 5, "пиров нет - добавь клавишей a", DM);
        // подробности
        int dy = 3 + vis + 3;
        if (lv.sel < (int)P.size()) {
            const json::Value& p = P[(size_t)lv.sel];
            std::string uri = p["remote"].str();
            c.Put(3, dy, uri, WH, true, Color(), c.w - 2);
            std::string origin = p["inbound"].boolean() ? "входящий" : !a.cfgLoaded ? "исходящий" : a.InConfigPeers(uri) ? "из конфига" : "только сейчас (* - нет в конфиге)";
            c.Seq(3, dy + 1, { { "адрес ", DM }, { p["address"].str().empty() ? "—" : p["address"].str(), TX }, { "   ключ ", DM }, { ShortKey(p["key"].str()), TX },
                               { "   порт ", DM }, { p["port"].text(), TX }, { "   приоритет ", DM }, { p["priority"].text(), TX },
                               { "   ", DM }, { origin, DM } }, c.w - 2);
            std::string err = p["last_error"].str(), pt = a.PingText(p["address"].str());
            if (!err.empty()) c.Seq(3, dy + 2, { { "последняя ошибка: ", DM }, { err, RD } }, c.w - 2);
            if (!pt.empty()) c.Put(std::max(3, c.w - 3 - (int)util::U8Len(pt)), dy + 2, pt, B);
        }
        bool canDel = lv.sel < (int)P.size() && !P[(size_t)lv.sel]["inbound"].boolean();
        bool canCfg = canDel && a.cfgLoaded && !a.InConfigPeers(P[(size_t)lv.sel]["remote"].str());
        std::vector<std::pair<std::string, std::string>> f = { { "a", "добавить" } };
        if (canDel) f.push_back({ "d", "убрать" });
        f.push_back({ "p", "пинг всех" });
        if (canCfg) f.push_back({ "w", "в конфиг" });
        f.push_back({ "Esc", "назад" });
        Footer(c, a, f);
    }
    void Remove(App& a) {
        const json::Value& P = a.s.peers;
        if (lv.sel >= (int)P.size() || P[(size_t)lv.sel]["inbound"].boolean()) return;
        std::string uri = P[(size_t)lv.sel]["remote"].str();
        bool inCfg = a.InConfigPeers(uri);
        json::Value cfg = a.cfgOrig;
        a.Confirm("убрать пир", "Отключить " + uri + "?\n" +
                  (inCfg ? "Он прописан в конфиге - уберу и оттуда (копия старого конфига сохранится), иначе он вернётся после перезапуска службы."
                         : "В конфиге его нет - просто отключу до перезапуска службы.") + "\n\nEnter - убрать",
                  [&a, uri, inCfg, cfg] {
            a.Op("убираю пир", [uri, inCfg, cfg]() mutable -> std::string {
                json::Value v; std::string err;
                if (!ygg::Ctl({ "removePeer", "uri=" + uri }, v, err)) return "!" + err;
                if (inCfg) {
                    json::Value& arr = cfg.at("Peers");
                    arr.arr.erase(std::remove_if(arr.arr.begin(), arr.arr.end(), [&](const json::Value& x) { return x.str() == uri; }), arr.arr.end());
                    std::string e = ygg::SaveConfig(ygg::CtlPaths(), cfg);
                    if (!e.empty()) return "!отключил, но конфиг не сохранён: " + e;
                }
                return std::string("Пир убран") + (inCfg ? " и из конфига." : ".");
            }, "", [&a](const std::string&) { a.cfgLoaded = false; a.ReloadConfig(); });
        }, true);
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        const json::Value& P = a.s.peers;
        if (lv.Handle(k, (int)P.size(), Visible(a.term.canvas))) return true;
        if (k.is('a') || k.text == "ф") { a.Push(MakeAddPeer()); return true; }
        if (k.is('d') || k.text == "в") { Remove(a); return true; }
        if (k.is('p') || k.text == "з") {
            for (size_t i = 0; i < P.size(); ++i) if (P[i]["up"].boolean()) a.PingAsync(P[i]["address"].str());
            a.Flash("Пингую пиров по адресам Yggdrasil…");
            return true;
        }
        if ((k.is('w') || k.text == "ц") && lv.sel < (int)P.size() && !P[(size_t)lv.sel]["inbound"].boolean())
        { a.AddPeersToConfig({ P[(size_t)lv.sel]["remote"].str() }, "Пир записан в конфиг - останется после перезапуска."); return true; }
        return false;
    }
};
Screen* MakePeers() { return new PeersScreen(); }

// ------------------------------------------------------------------ добавить пир
struct AddPeerScreen : Screen {
    LineEdit uri, pass, key;
    int focus = 0;               // 0 адрес, 1 пароль, 2 ключ, 3 куда, 4 добавить, 5 проверить
    int where = 0;               // 0 сейчас и в конфиг, 1 только сейчас, 2 только в конфиг
    struct Check { int state = 0; std::string text; double ms = -1; };   // 0 нет, 1 идёт, 2 ок, 3 плохо, 4 пропущено
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::array<Check, 3>> checks = std::make_shared<std::array<Check, 3>>();
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(false);

    bool Typing() const override { return focus <= 2; }
    std::string FullUri() const {
        std::string base = util::Trim(uri.text);
        std::vector<std::string> q;
        if (!util::Trim(key.text).empty()) q.push_back("key=" + util::Trim(key.text));
        if (!util::Trim(pass.text).empty()) q.push_back("password=" + util::Trim(pass.text));
        std::string sep = base.find('?') == std::string::npos ? "?" : "&";
        for (auto& x : q) { base += sep + x; sep = "&"; }
        return base;
    }
    void Draw(Canvas& c, App& a) override {
        Header(c, "ДОБАВИТЬ ПИР", "addPeer · проверка до добавления");
        int W = c.w - 4;
        c.Box(2, 3, W, 13, "пир", BF, B);
        auto field = [&](int y, int idx, const std::string& cap, const LineEdit& e, const std::string& ph) {
            c.Put(4, y, cap, DM);
            bool f = focus == idx;
            c.FillBg(22, y, 2 + W - 2, f ? BH : Hex("#161b22"));
            int ex = c.Put(23, y, e.text.empty() && !f ? ph : e.text, e.text.empty() ? DM2 : WH, false, Color(), 2 + W - 2);
            if (f) c.Put(ex, y, "▌", B);
        };
        field(4, 0, "адрес", uri, "tls://host:port  (tcp tls quic ws wss)");
        field(6, 1, "пароль", pass, "нет - если узел не требует");
        field(8, 2, "ключ пира", key, "любой - закрепить необязательно");
        c.Put(4, 10, "куда", DM);
        const char* wn[] = { "сейчас и в конфиг", "только сейчас", "только в конфиг" };
        int x = 23;
        for (int i = 0; i < 3; ++i) {
            bool on = where == i;
            std::string t = std::string(on ? "◉ " : "○ ") + wn[i];
            if (focus == 3 && on) c.FillBg(x - 1, 10, x + (int)util::U8Len(t) + 1, BH);
            x = c.Put(x, 10, t, on ? WH : DM, on) + 3;
        }
        util::PeerUri pu = util::ParseUri(util::Trim(uri.text));
        bool ok = pu.ok && !*running;
        auto btn = [&](int bx, int idx, const std::string& t, Color col) {
            bool f = focus == idx;
            if (f) c.FillBg(bx - 1, 12, bx + (int)util::U8Len(t) + 1, ok ? BH : Hex("#161b22"));
            return c.Put(bx, 12, t, ok ? col : DM2, f) + 4;
        };
        int bx = btn(23, 4, "[ ДОБАВИТЬ ]", G);
        btn(bx, 5, "[ ПРОВЕРИТЬ ]", B);
        c.Seq(4, 14, { { "итоговый адрес: ", DM }, { FullUri().empty() ? "—" : FullUri(), TX } }, 2 + W - 1);
        // проверка
        c.Box(2, 16, W, 6, "проверка", BF, B);
        const char* names[] = { "DNS", "порт", "рукопожатие" };
        std::array<Check, 3> ch;
        { std::lock_guard<std::mutex> lk(*mu); ch = *checks; }
        for (int i = 0; i < 3; ++i) {
            Color col = ch[i].state == 2 ? G : ch[i].state == 3 ? RD : ch[i].state == 1 ? OR : DM2;
            c.Put(4, 17 + i, "●", col);
            c.Put(6, 17 + i, names[i], TX);
            c.Put(20, 17 + i, ch[i].state == 0 ? "—" : ch[i].text, ch[i].state == 3 ? RD : DM, false, Color(), 2 + W - 10);
            if (ch[i].ms >= 0) c.Put(2 + W - 9, 17 + i, util::Fixed(ch[i].ms, 0) + " мс", G);
        }
        Footer(c, a, { { "Tab", "поле" }, { "←→", "куда" }, { "Enter", "добавить" }, { "Esc", "назад" } });
    }
    void Run(App& a, bool withAdd) {
        std::string u = FullUri();
        util::PeerUri pu = util::ParseUri(util::Trim(uri.text));
        if (!pu.ok) { a.Flash("Адрес пира должен быть вида tls://host:port", RD); return; }
        if (*running) return;
        *running = true;
        { std::lock_guard<std::mutex> lk(*mu); *checks = {}; (*checks)[0] = { 1, "ищу…", -1 }; }
        int wh = where;
        auto mu_ = mu; auto ch = checks; auto run = running;
        auto set = [mu_, ch](int i, Check v) { std::lock_guard<std::mutex> lk(*mu_); (*ch)[(size_t)i] = v; };
        a.Bg([pu, u, withAdd, wh, set]() -> std::string {
            bool ipLiteral = pu.host.find(':') != std::string::npos || pu.host.find_first_not_of("0123456789.") == std::string::npos;
            std::string ip = pu.host;
            if (!ipLiteral) {
                addrinfo hints{}; hints.ai_socktype = SOCK_STREAM;
                addrinfo* res = nullptr;
                ip.clear();
                if (getaddrinfo(pu.host.c_str(), nullptr, &hints, &res) == 0 && res) {
                    char b[INET6_ADDRSTRLEN] = "";
                    void* ad = res->ai_family == AF_INET ? (void*)&((sockaddr_in*)res->ai_addr)->sin_addr : (void*)&((sockaddr_in6*)res->ai_addr)->sin6_addr;
                    inet_ntop(res->ai_family, ad, b, sizeof(b));
                    ip = b;
                    freeaddrinfo(res);
                }
            }
            if (ip.empty()) { set(0, { 3, "имя не находится - проверь адрес или DNS", -1 }); return "dns"; }
            set(0, { 2, "найден " + ip, -1 });
            std::string pr = util::Lower(pu.proto);
            bool tcp = pr == "tcp" || pr == "tls" || pr == "ws" || pr == "wss";
            if (!tcp || pu.port.empty()) set(1, { 4, pr == "quic" ? "QUIC работает по UDP - заранее не проверить" : "не проверяется для " + pr, -1 });
            else {
                set(1, { 1, "подключаюсь…", -1 });
                double ms = -1;
                int fd = net::ConnectTcp(pu.host, (int)util::ToLL(pu.port), 5000, nullptr, &ms);
                if (fd >= 0) { close(fd); set(1, { 2, "порт " + pu.port + " открыт", ms }); }
                else set(1, { 3, "порт " + pu.port + " закрыт или недоступен", -1 });
            }
            if (!withAdd) { set(2, { 4, "будет проверено при добавлении", -1 }); return "check"; }
            if (wh == 2) { set(2, { 4, "только в конфиг - подключится после перезапуска службы", -1 }); return "cfg"; }
            set(2, { 1, "addPeer, жду рукопожатия (до 20 с)…", -1 });
            json::Value v; std::string err;
            if (!ygg::Ctl({ "addPeer", "uri=" + u }, v, err)) { set(2, { 3, err, -1 }); return "fail"; }
            for (int i = 0; i < 20; ++i) {
                util::SleepMs(1000);
                if (ygg::Ctl("getPeers", v, err)) {
                    const json::Value& arr = v["peers"];
                    for (size_t k = 0; k < arr.size(); ++k)
                        if (arr[k]["remote"].str() == u && arr[k]["up"].boolean()) { set(2, { 2, "ключ принят, связь есть", arr[k]["latency"].num() / 1e6 }); return "up"; }
                }
            }
            set(2, { 3, "пир добавлен, но за 20 с не подключился (ошибка - в списке пиров)", -1 });
            return "noup";
        }, [&a, u, wh, run](const std::string& res) {
            *run = false;
            a.data.RefreshNow();
            if (res == "cfg") a.AddPeersToConfig({ u }, "Пир записан в конфиг. Подключится после перезапуска службы.");
            else if (res == "up" || res == "noup") {
                if (wh == 0) a.AddPeersToConfig({ u }, res == "up" ? "Пир подключён и записан в конфиг." : "Пир записан в конфиг, но связи пока нет.");
                else a.Flash(res == "up" ? "Пир подключён (до перезапуска службы)." : "Пир добавлен, но связи пока нет.", res == "up" ? G : OR);
            }
        });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.k == K::Tab || k.k == K::Down) { focus = (focus + 1) % 6; return true; }
        if (k.k == K::BackTab || k.k == K::Up) { focus = (focus + 5) % 6; return true; }
        if (k.k == K::Enter) { Run(a, focus != 5); return true; }
        if (focus == 3 && k.k == K::Left) { where = (where + 2) % 3; return true; }
        if (focus == 3 && k.k == K::Right) { where = (where + 1) % 3; return true; }
        LineEdit* e = focus == 0 ? &uri : focus == 1 ? &pass : focus == 2 ? &key : nullptr;
        if (e && e->Handle(k)) return true;
        return false;
    }
};
Screen* MakeAddPeer() { return new AddPeerScreen(); }

// ------------------------------------------------------------------ история задержек (history.log сторожа)
// Строка: unix<TAB>фаза<TAB>интернет<TAB>uri=U|мс;uri=D|-1;...
struct HistoryScreen : Screen {
    struct Tick { long long ts = 0; std::string phase; std::vector<std::pair<int, double>> v; };
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<Tick>> hist = std::make_shared<std::vector<Tick>>();
    std::shared_ptr<std::vector<std::string>> peers = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::string> err = std::make_shared<std::string>();
    long long loadedAt = 0;
    std::shared_ptr<std::atomic<bool>> loading = std::make_shared<std::atomic<bool>>(false);
    int range = 1;   // 0 - 6 ч, 1 - 24 ч, 2 - 48 ч
    ListView lv;

    void Load(App& a) {
        if (*loading) return;
        *loading = true;
        loadedAt = util::Now();
        if (a.s.wdExe.empty()) { std::lock_guard<std::mutex> lk(*mu); *err = "Сторож не найден - историю пишет он после каждой проверки. Установи ygg-watchdog."; *loading = false; return; }
        std::string path = util::DirOf(a.s.wdExe) + "history.log";
        std::set<std::string> cur;
        for (size_t i = 0; i < a.s.peers.size(); ++i) cur.insert(a.s.peers[i]["remote"].str());
        for (const char* key : { "main_peers", "backup_peers" }) for (size_t i = 0; i < a.s.wd[key].size(); ++i) cur.insert(a.s.wd[key][i].str());
        auto mu_ = mu; auto H = hist; auto Pp = peers; auto E = err; auto ld = loading;
        a.Bg([path, cur, mu_, H, Pp, E] {
            std::vector<Tick> h;
            std::vector<std::string> pr;
            std::string e;
            if (!util::FileExists(path)) e = "Истории пока нет: сторож пишет history.log после каждой проверки (раз в 3 минуты).";
            std::map<std::string, int> idx;
            for (const auto& ln : util::Lines(util::ReadFileTail(path, 3 * 1024 * 1024))) {
                auto f = util::SplitKeep(ln, '\t');
                if (f.size() < 4) continue;
                Tick t;
                t.ts = util::ToLL(f[0], 0);
                if (!t.ts) continue;
                t.phase = f[1];
                for (const auto& item : util::Split(f[3], ';')) {
                    size_t eq = item.rfind('=');
                    if (eq == std::string::npos) continue;
                    std::string uri = item.substr(0, eq), val = item.substr(eq + 1);
                    if (!cur.empty() && !cur.count(uri)) continue;      // только нынешние пиры
                    bool up = val.size() > 1 && val[0] == 'U';
                    double ms = up ? util::ToD(val.substr(2), -1) : -1;
                    auto it = idx.find(uri);
                    int pi;
                    if (it == idx.end()) { pi = (int)pr.size(); idx[uri] = pi; pr.push_back(uri); } else pi = it->second;
                    t.v.push_back({ pi, ms });
                }
                h.push_back(t);
            }
            std::lock_guard<std::mutex> lk(*mu_);
            *H = h; *Pp = pr; *E = e;
            return std::string();
        }, [ld](const std::string&) { *ld = false; });
    }
    void OnShow(App& a) override { Load(a); }
    void Draw(Canvas& c, App& a) override {
        if (!*loading && util::Now() - loadedAt > 180) Load(a);
        std::vector<Tick> h; std::vector<std::string> pr; std::string e;
        { std::lock_guard<std::mutex> lk(*mu); h = *hist; pr = *peers; e = *err; }
        long long now = util::Now(), span = range == 0 ? 6 * 3600 : range == 1 ? 24 * 3600 : 48 * 3600, t0 = now - span;
        Header(c, "ИСТОРИЯ ЗАДЕРЖЕК", std::string("history.log сторожа · за ") + (range == 0 ? "6" : range == 1 ? "24" : "48") + " ч");
        int W = c.w - 4;
        if (h.empty()) {
            c.Box(2, 3, W, 5, "история", BF, B);
            c.Put(4, 5, *loading ? "читаю history.log…" : (e.empty() ? "нет данных" : e), DM, false, Color(), c.w - 3);
            Footer(c, a, { { "Tab", "6 / 24 / 48 ч" }, { "Esc", "назад" } });
            return;
        }
        int tableH = std::max(3, std::min((int)pr.size(), c.h - 20));
        lv.Clamp((int)pr.size(), tableH);
        int sel = lv.sel;
        // график выбранного пира
        int cols = W - 12;
        std::vector<double> ser((size_t)cols, -1);
        std::vector<int> state((size_t)cols, -1), phase((size_t)cols, -1);
        for (const auto& t : h) {
            if (t.ts < t0) continue;
            int i = (int)((t.ts - t0) * cols / span);
            if (i < 0 || i >= cols) continue;
            std::string p = t.phase;
            phase[(size_t)i] = (p == "ok" || p == "backup") ? 0 : (p == "down" || p == "crisis") ? 1 : (p == "no_internet" || p == "battery" || p == "service_down") ? 2 : 3;
            for (const auto& pv : t.v) if (pv.first == sel) {
                if (pv.second >= 0) { ser[(size_t)i] = std::max(ser[(size_t)i], pv.second); if (state[(size_t)i] != 0) state[(size_t)i] = 1; }
                else state[(size_t)i] = 0;
            }
        }
        int gh = 10;
        std::string selName = sel < (int)pr.size() ? pr[(size_t)sel] : "";
        c.Box(2, 3, W, gh, "задержка: " + selName, BF, B);
        double mx = 0;
        for (double v : ser) mx = std::max(mx, v);
        int rows = gh - 5;
        if (mx > 0) {
            double top = NiceTop(mx);
            c.Put(4, 4, util::Fixed(top, 0), DM);
            c.Put(4, 3 + rows, "0 мс", DM);
            Columns(c, 12, 4, rows, ser, 0, top, B, OR, std::max(150.0, top * 0.8));
        } else c.Put(12, 5, "за период пир не отвечал или не измерялся", DM);
        c.Put(4, 4 + rows, "связь", DM);
        for (int i = 0; i < cols; ++i) c.Put(12 + i, 4 + rows, state[(size_t)i] < 0 ? "·" : "█", state[(size_t)i] < 0 ? DM2 : state[(size_t)i] ? G : RD);
        c.Put(4, 5 + rows, "режим", DM);
        for (int i = 0; i < cols; ++i) {
            int p = phase[(size_t)i];
            c.Put(12 + i, 5 + rows, p < 0 ? "·" : "▀", p < 0 ? DM2 : p == 0 ? G : p == 1 ? OR : p == 2 ? RD : DM);
        }
        long long step = range == 0 ? 3600 : range == 1 ? 4 * 3600 : 8 * 3600;
        for (long long ts = (t0 / step + 1) * step; ts < now; ts += step) {
            int xx = 12 + (int)((ts - t0) * cols / span);
            if (xx + 5 < 2 + W - 1) c.Put(xx, 6 + rows, util::TimeText(ts, "%H:%M"), DM);
        }
        // таблица пиров
        int ty = 3 + gh;
        c.Box(2, ty, W, tableH + 3, "пиры · на связи и задержка за период", BF, B);
        int uriW = std::max(20, W - 50);
        c.Put(5, ty + 1, "пир", DM); c.Put(6 + uriW, ty + 1, "на связи", DM); c.Put(17 + uriW, ty + 1, "ср / мин / макс", DM); c.Put(37 + uriW, ty + 1, "сейчас", DM);
        for (int r = 0; r < tableH && lv.top + r < (int)pr.size(); ++r) {
            int pi = lv.top + r, y = ty + 2 + r;
            int n = 0, up = 0; double sum = 0, mn = 1e18, mxv = 0, last = -2;
            for (const auto& t : h) {
                if (t.ts < t0) continue;
                for (const auto& pv : t.v) if (pv.first == pi) {
                    ++n; last = pv.second;
                    if (pv.second >= 0) { ++up; sum += pv.second; mn = std::min(mn, pv.second); mxv = std::max(mxv, pv.second); }
                }
            }
            bool on = pi == sel;
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Put(5, y, util::U8Fit(pr[(size_t)pi], (size_t)uriW), on ? WH : TX, on);
            if (n == 0) { c.Put(6 + uriW, y, "нет данных за период", DM); continue; }
            int pct = (int)(100.0 * up / n + 0.5);
            c.Put(6 + uriW, y, std::to_string(pct) + " %", pct >= 95 ? G : pct >= 50 ? OR : RD);
            auto ms = [](double x) { return util::Fixed(x, 0); };
            c.Put(17 + uriW, y, up ? ms(sum / up) + " / " + ms(mn) + " / " + ms(mxv) + " мс" : "—", TX);
            c.Put(37 + uriW, y, last >= 0 ? ms(last) + " мс" : "лежит", last >= 0 ? G : RD, false, Color(), 2 + W - 1);
        }
        Footer(c, a, { { "↑↓", "пир" }, { "Tab", "6 / 24 / 48 ч" }, { "r", "перечитать" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        size_t n;
        { std::lock_guard<std::mutex> lk(*mu); n = peers->size(); }
        if (lv.Handle(k, (int)n, std::max(3, std::min((int)n, a.term.canvas.h - 20)))) return true;
        if (k.k == K::Tab) { range = (range + 1) % 3; return true; }
        if (k.is('r') || k.text == "к") { loadedAt = 0; Load(a); return true; }
        return false;
    }
};
Screen* MakeHistory() { return new HistoryScreen(); }

} // namespace pnl
