// screens2.cpp - дерево маршрутов, сессии, пути, устройства, удалённый узел, интерфейсы.
namespace pnl {

// 201:1f2a:…:e0 - адрес Yggdrasil коротко
inline std::string TinyAddr(const std::string& a) {
    auto g = util::SplitKeep(a, ':');
    if (g.size() < 4) return a;
    return g[0] + ":" + g[1] + ":…:" + g.back();
}

// Имя узла: из адресной книги, потом имя пира
inline std::string NodeName(App& a, const std::string& addr, const std::string& key) {
    std::string n = a.BookName(addr);
    if (!n.empty()) return n;
    return PeerNameByKey(a.s, key);
}

// ------------------------------------------------------------------ дерево маршрутов
struct TreeScreen : Screen {
    struct Node { std::string key, addr, parent; std::vector<int> kids; int sub = 0; };
    std::vector<Node> nodes;
    std::map<std::string, int> idx;
    int root = -1, self = -1;
    long long builtFor = -1;
    std::set<std::string> open;            // развёрнутые узлы (по ключу)
    std::set<std::string> onPath;          // путь корень -> я (всегда развёрнут)
    struct Row { int n; std::string prefix; };
    std::vector<Row> rows;
    ListView lv;
    std::string selKey;
    bool searching = false;
    LineEdit search;

    bool Typing() const override { return searching; }
    void Build(App& a) {
        if (builtFor == a.s.treeAt && !nodes.empty()) return;
        builtFor = a.s.treeAt;
        nodes.clear(); idx.clear(); root = self = -1; onPath.clear();
        const json::Value& T = a.s.tree;
        for (size_t i = 0; i < T.size(); ++i) {
            Node n{ T[i]["key"].str(), T[i]["address"].str(), T[i]["parent"].str(), {}, 0 };
            if (n.key.empty() || idx.count(n.key)) continue;
            idx[n.key] = (int)nodes.size();
            nodes.push_back(n);
        }
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].parent == nodes[i].key) { root = (int)i; continue; }
            auto it = idx.find(nodes[i].parent);
            if (it != idx.end()) nodes[(size_t)it->second].kids.push_back((int)i);
        }
        for (auto& n : nodes) std::sort(n.kids.begin(), n.kids.end(), [&](int x, int y) { return nodes[(size_t)x].addr < nodes[(size_t)y].addr; });
        std::function<int(int, int)> count = [&](int u, int d) {
            if (d > 300) return 0;
            int s = 0;
            for (int k : nodes[(size_t)u].kids) s += 1 + count(k, d + 1);
            nodes[(size_t)u].sub = s;
            return s;
        };
        if (root >= 0) count(root, 0);
        auto si = idx.find(a.s.self["key"].str());
        self = si != idx.end() ? si->second : -1;
        for (int k = self, d = 0; k >= 0 && d < 300; ++d) {
            onPath.insert(nodes[(size_t)k].key);
            if (k == root) break;
            auto it = idx.find(nodes[(size_t)k].parent);
            k = it == idx.end() ? -1 : it->second;
        }
        if (self >= 0) open.insert(nodes[(size_t)self].key);   // свои потомки видны сразу
        if (selKey.empty() && self >= 0) selKey = nodes[(size_t)self].key;   // сначала выбран ты
        Flatten();
    }
    void Flatten() {
        rows.clear();
        if (root < 0) return;
        std::function<void(int, const std::string&, bool, int)> walk = [&](int u, const std::string& pre, bool last, int d) {
            if (d > 300) return;
            std::string me = d == 0 ? "" : pre + (last ? "└─ " : "├─ ");
            rows.push_back({ u, me });
            const Node& n = nodes[(size_t)u];
            // развёрнутый узел - все дети; узел на пути к тебе - только ветка, ведущая к тебе
            std::vector<int> kids;
            if (open.count(n.key)) kids = n.kids;
            else if (onPath.count(n.key)) for (int k : n.kids) if (onPath.count(nodes[(size_t)k].key)) kids.push_back(k);
            std::string next = d == 0 ? "" : pre + (last ? "   " : "│  ");
            for (size_t i = 0; i < kids.size(); ++i) walk(kids[i], next, i + 1 == kids.size(), d + 1);
        };
        walk(root, "", true, 0);
        // выбранный остаётся выбранным
        if (!selKey.empty()) for (size_t i = 0; i < rows.size(); ++i) if (nodes[(size_t)rows[i].n].key == selKey) lv.sel = (int)i;
    }
    int Depth(int u) const {
        int d = 0;
        for (int k = u; k >= 0 && k != root && d < 300; ++d) { auto it = idx.find(nodes[(size_t)k].parent); k = it == idx.end() ? -1 : it->second; }
        return d;
    }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "ДЕРЕВО МАРШРУТОВ")) return;
        Build(a);
        int myDepth = self >= 0 ? Depth(self) : -1;
        Header(c, "ДЕРЕВО МАРШРУТОВ", "getTree · " + std::to_string(nodes.size()) + " узлов" + (myDepth >= 0 ? " · ты на глубине " + std::to_string(myDepth) : ""));
        int W = c.w - 4, lw = std::max(40, W * 64 / 100), rw = W - lw - 1, H = c.h - 5;
        c.Box(2, 3, lw, H, "от корня до тебя", BF, B);
        int vis = H - 4;
        lv.Clamp((int)rows.size(), vis);
        if (!rows.empty() && lv.sel < (int)rows.size()) selKey = nodes[(size_t)rows[(size_t)lv.sel].n].key;
        for (int i = 0; i < vis && lv.top + i < (int)rows.size(); ++i) {
            const Row& r = rows[(size_t)(lv.top + i)];
            const Node& n = nodes[(size_t)r.n];
            int y = 4 + i;
            bool on = lv.top + i == lv.sel;
            RowBg(c, 3, y, 2 + lw - 1, on);
            int x = c.Put(5, y, r.prefix, DM2);
            bool isSelf = r.n == self, isRoot = r.n == root;
            std::string peer = PeerNameByKey(a.s, n.key), name = a.BookName(n.addr);
            if (isRoot) x = c.Put(x, y, "◆ ", OR);
            else if (isSelf) x = c.Put(x, y, "● ", B);
            std::vector<P> seg;
            if (isSelf) seg = { { "ТЫ ", B, true }, { TinyAddr(n.addr), B, true } };
            else seg = { { TinyAddr(n.addr), on ? WH : TX, on } };
            if (isRoot) seg.push_back({ " корень", OR });
            if (!name.empty()) seg.push_back({ " " + name, G });
            else if (!peer.empty()) seg.push_back({ " · твой пир " + peer, DM });
            // сколько потомков скрыто: все минус показанные ветки целиком
            int hidden = n.sub;
            if (open.count(n.key)) hidden = 0;
            else if (onPath.count(n.key)) for (int k : n.kids) if (onPath.count(nodes[(size_t)k].key)) hidden -= 1 + nodes[(size_t)k].sub;
            if (hidden > 0) seg.push_back({ " +" + std::to_string(hidden), DM });
            c.Seq(x, y, seg, 2 + lw - 1);
        }
        if (rows.empty()) c.Put(5, 5, "дерево пусто - видимо, нет ни одного пира", DM);
        int rest = (int)nodes.size() - (int)rows.size();
        if (rest > 0 && H > 6) c.Put(5, 3 + H - 2, "ещё " + std::to_string(rest) + " узлов - [Enter] на узле раскрывает ветку", DM, false, Color(), 2 + lw - 1);
        if (searching) c.Seq(5, 3 + H - 2, { { "/ ", B, true }, { search.text, WH }, { "▌", B }, { "   адрес или его часть, Enter - найти", DM } }, 2 + lw - 1);
        // узел
        int rx = 2 + lw + 1;
        c.Box(rx, 3, rw, H, "узел", BF, B);
        if (lv.sel < (int)rows.size()) {
            const Node& n = nodes[(size_t)rows[(size_t)lv.sel].n];
            int y = 4, x = rx + 2, x1 = rx + rw - 1;
            std::string book = a.BookName(n.addr), peer = PeerNameByKey(a.s, n.key);
            std::string title = rows[(size_t)lv.sel].n == self ? "это ты" : !book.empty() ? book : !peer.empty() ? peer : rows[(size_t)lv.sel].n == root ? "корень сети" : "узел";
            c.Put(x, y++, title, WH, true, Color(), x1);
            c.Put(x, y++, !book.empty() ? "из адресной книги" : !peer.empty() ? "твой пир" : "", DM, false, Color(), x1);
            ++y;
            c.Put(x, y++, "адрес", DM);
            for (const auto& l : Wrap(util::Replace(n.addr, ":", ": "), rw - 4)) c.Put(x, y++, util::Replace(l, ": ", ":"), TX, false, Color(), x1);
            ++y;
            c.Put(x, y++, "ключ", DM);
            c.Put(x, y++, ShortKey(n.key), TX, false, Color(), x1);
            ++y;
            bool sess = false;
            for (size_t i = 0; i < a.s.sessions.size(); ++i) if (a.s.sessions[i]["key"].str() == n.key) sess = true;
            c.Put(x, y++, "глубина " + std::to_string(Depth(rows[(size_t)lv.sel].n)) + " · " + (sess ? "сессия есть" : "сессии нет"), DM, false, Color(), x1);
            std::string pt = a.PingText(n.addr);
            if (!pt.empty()) c.Put(x, y++, pt, B, false, Color(), x1);
            ++y;
            for (const auto& [k, t] : std::vector<std::pair<std::string, std::string>>{ { "[p]", " пинг" }, { "[i]", " сведения (NodeInfo)" }, { "[c]", " копировать адрес" }, { "[b]", " в устройства" } })
                if (y < 3 + H - 1) c.Seq(x, y++, { { k, B, true }, { t, TX } }, x1);
        }
        Footer(c, a, { { "↑↓", "узел" }, { "Enter", "развернуть" }, { "/", "найти по адресу" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        if (searching) {
            if (k.k == K::Esc) { searching = false; return true; }
            if (k.k == K::Enter) {
                searching = false;
                std::string q = util::Lower(util::Trim(search.text));
                for (size_t i = 0; i < nodes.size(); ++i)
                    if (!q.empty() && (util::Contains(util::Lower(nodes[i].addr), q) || util::Contains(util::Lower(a.BookName(nodes[i].addr)), q))) {
                        // развернуть путь до найденного
                        for (int u = (int)i, d = 0; u >= 0 && d < 300; ++d) { open.insert(nodes[(size_t)u].key); if (u == root) break; auto it = idx.find(nodes[(size_t)u].parent); u = it == idx.end() ? -1 : it->second; }
                        selKey = nodes[i].key;
                        Flatten();
                        return true;
                    }
                a.Flash("Не нашёл узел по «" + search.text + "»", OR);
                return true;
            }
            search.Handle(k);
            return true;
        }
        int vis = a.term.canvas.h - 9;
        if (lv.Handle(k, (int)rows.size(), vis)) { if (lv.sel < (int)rows.size()) selKey = nodes[(size_t)rows[(size_t)lv.sel].n].key; return true; }
        if (lv.sel >= (int)rows.size()) return false;
        const Node& n = nodes[(size_t)rows[(size_t)lv.sel].n];
        if (k.k == K::Enter) {
            if (n.kids.empty()) { a.Flash("У этого узла нет потомков в дереве.", DM); return true; }
            if (open.count(n.key)) open.erase(n.key); else open.insert(n.key);
            selKey = n.key;
            Flatten();
            return true;
        }
        if (k.is('/')) { searching = true; search.text.clear(); return true; }
        if (k.is('p') || k.text == "з") { a.PingAsync(n.addr); return true; }
        if (k.is('i') || k.text == "ш") { a.Push(MakeRemote(n.key)); return true; }
        if (k.is('c') || k.text == "с") { a.Copy(n.addr, "Адрес"); return true; }
        if (k.is('b') || k.text == "и") { a.Push(MakeBookAdd(n.addr)); return true; }
        return false;
    }
};
Screen* MakeTree() { return new TreeScreen(); }

// Адрес с именем из книги
inline std::vector<P> AddrSeg(App& a, const std::string& addr, bool on) {
    std::string nm = a.BookName(addr);
    if (nm.empty()) return { { addr, on ? WH : TX, on } };
    return { { nm, G, true }, { " " + addr, DM } };
}
// Мини-график скорости из последних значений
inline std::string Spark(const std::deque<float>& h, size_t n) {
    static const char* lv[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    std::string out;
    if (h.empty()) return out;
    float mx = 1;
    size_t from = h.size() > n ? h.size() - n : 0;
    for (size_t i = from; i < h.size(); ++i) mx = std::max(mx, h[i]);
    for (size_t i = from; i < h.size(); ++i) out += lv[std::min(7, (int)(h[i] / mx * 7.99))];
    return out;
}

// ------------------------------------------------------------------ сессии
struct SessionsScreen : Screen {
    ListView lv;
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "СЕССИИ")) return;
        const json::Value& S = a.s.sessions;
        Header(c, "СЕССИИ", "getSessions · " + std::to_string(S.size()) + " активных");
        int W = c.w - 4, vis = std::max(3, c.h - 10);
        lv.Clamp((int)S.size(), vis);
        c.Box(2, 3, W, vis + 3, "сессии", BF, B);
        // адрес | ключ 10 | ↓ 10 | ↑ 10 | время 10 | скорость
        int aw = std::max(24, W - 52);
        c.Put(5, 4, "адрес", DM); c.Put(6 + aw, 4, "ключ", DM); c.Put(17 + aw, 4, "↓ получ.", DM); c.Put(28 + aw, 4, "↑ отпр.", DM);
        c.Put(39 + aw, 4, "время", DM); c.Put(50 + aw, 4, "скорость", DM, false, Color(), 2 + W - 1);
        for (int i = 0; i < vis && lv.top + i < (int)S.size(); ++i) {
            int idx = lv.top + i, y = 5 + i;
            const json::Value& e = S[(size_t)idx];
            bool on = idx == lv.sel;
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Seq(5, y, AddrSeg(a, e["address"].str(), on), 5 + aw);
            c.Put(6 + aw, y, ShortKey(e["key"].str()), DM);
            c.Put(17 + aw, y, Bytes(e["bytes_recvd"].num()), B);
            c.Put(28 + aw, y, Bytes(e["bytes_sent"].num()), G);
            c.Put(39 + aw, y, DurShort(e["uptime"].num(-1)), TX);
            auto it = a.s.sessRate.find(e["key"].str());
            if (it != a.s.sessRate.end()) c.Put(50 + aw, y, Spark(it->second, (size_t)std::max(4, W - 50 - aw)), B, false, Color(), 2 + W - 1);
        }
        if (S.size() == 0) c.Put(5, 5, "активных сессий нет - сессия появляется, когда ты общаешься с другим узлом", DM, false, Color(), c.w - 3);
        int dy = 3 + vis + 3;
        if (lv.sel < (int)S.size()) {
            const json::Value& e = S[(size_t)lv.sel];
            c.Seq(3, dy, { { e["address"].str(), WH }, { "   " + e["key"].str(), DM } }, c.w - 2);
            std::string pt = a.PingText(e["address"].str());
            if (!pt.empty()) c.Put(3, dy + 1, pt, B);
        }
        Footer(c, a, { { "p", "пинг" }, { "i", "запросить узел" }, { "c", "копировать адрес" }, { "b", "в устройства" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        const json::Value& S = a.s.sessions;
        if (lv.Handle(k, (int)S.size(), std::max(3, a.term.canvas.h - 10))) return true;
        if (lv.sel >= (int)S.size()) return false;
        const json::Value& e = S[(size_t)lv.sel];
        if (k.is('p') || k.text == "з") { a.PingAsync(e["address"].str()); return true; }
        if (k.is('i') || k.text == "ш") { a.Push(MakeRemote(e["key"].str())); return true; }
        if (k.is('c') || k.text == "с") { a.Copy(e["address"].str(), "Адрес"); return true; }
        if (k.is('b') || k.text == "и") { a.Push(MakeBookAdd(e["address"].str())); return true; }
        return false;
    }
};
Screen* MakeSessions() { return new SessionsScreen(); }

// ------------------------------------------------------------------ таблица путей
struct PathsScreen : Screen {
    ListView lv;
    int sort = 0;    // 0 адрес, 1 длина пути, 2 через, 3 seq
    std::vector<int> order;
    void Sort(App& a) {
        const json::Value& Pt = a.s.paths_;
        order.resize(Pt.size());
        for (size_t i = 0; i < Pt.size(); ++i) order[i] = (int)i;
        auto via = [&](int i) { const json::Value& pp = Pt[(size_t)i]["path"]; return pp.size() ? PeerNameByPort(a.s, pp[0].i64()) : std::string("—"); };
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
            switch (sort) {
                case 1: return Pt[(size_t)x]["path"].size() < Pt[(size_t)y]["path"].size();
                case 2: return via(x) < via(y);
                case 3: return Pt[(size_t)x]["sequence"].num() > Pt[(size_t)y]["sequence"].num();
                default: return Pt[(size_t)x]["address"].str() < Pt[(size_t)y]["address"].str();
            }
        });
    }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "ТАБЛИЦА ПУТЕЙ")) return;
        Sort(a);
        const json::Value& Pt = a.s.paths_;
        static const char* sn[] = { "адрес", "длина пути", "через", "seq" };
        Header(c, "ТАБЛИЦА ПУТЕЙ", "getPaths · " + std::to_string(Pt.size()) + " путей · сортировка: " + sn[sort]);
        int W = c.w - 4, vis = std::max(3, c.h - 10);
        lv.Clamp((int)order.size(), vis);
        c.Box(2, 3, W, vis + 3, "пути", BF, B);
        int aw = std::max(24, W - 50);
        c.Put(5, 4, std::string("адрес") + (sort == 0 ? " ↓" : ""), sort == 0 ? B : DM);
        c.Put(6 + aw, 4, std::string("путь по портам") + (sort == 1 ? " ↓" : ""), sort == 1 ? B : DM);
        c.Put(28 + aw, 4, std::string("через") + (sort == 2 ? " ↓" : ""), sort == 2 ? B : DM);
        c.Put(41 + aw, 4, std::string("seq") + (sort == 3 ? " ↓" : ""), sort == 3 ? B : DM);
        for (int i = 0; i < vis && lv.top + i < (int)order.size(); ++i) {
            int row = lv.top + i, y = 5 + i;
            const json::Value& e = Pt[(size_t)order[(size_t)row]];
            bool on = row == lv.sel;
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Seq(5, y, AddrSeg(a, e["address"].str(), on), 5 + aw);
            std::string ports;
            for (size_t k = 0; k < e["path"].size(); ++k) ports += (k ? " " : "") + e["path"][k].text();
            c.Put(6 + aw, y, util::U8Fit(ports.empty() ? "—" : ports, 21), DM);
            c.Put(28 + aw, y, util::U8Fit(e["path"].size() ? PeerNameByPort(a.s, e["path"][0].i64()) : "—", 12), B);
            c.Put(41 + aw, y, e["sequence"].text().empty() ? "—" : e["sequence"].text(), DM, false, Color(), 2 + W - 1);
        }
        if (Pt.size() == 0) c.Put(5, 5, "путей нет", DM);
        if (lv.sel < (int)order.size()) {
            std::string pt = a.PingText(Pt[(size_t)order[(size_t)lv.sel]]["address"].str());
            if (!pt.empty()) c.Put(3, 3 + vis + 3, pt, B);
        }
        Footer(c, a, { { "Tab", "сортировка" }, { "p", "пинг" }, { "i", "запросить узел" }, { "c", "копировать" }, { "b", "в устройства" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        if (k.k == K::Tab) { sort = (sort + 1) % 4; return true; }
        if (lv.Handle(k, (int)order.size(), std::max(3, a.term.canvas.h - 10))) return true;
        if (lv.sel >= (int)order.size()) return false;
        const json::Value& e = a.s.paths_[(size_t)order[(size_t)lv.sel]];
        if (k.is('p') || k.text == "з") { a.PingAsync(e["address"].str()); return true; }
        if (k.is('i') || k.text == "ш") { a.Push(MakeRemote(e["key"].str())); return true; }
        if (k.is('c') || k.text == "с") { a.Copy(e["address"].str(), "Адрес"); return true; }
        if (k.is('b') || k.text == "и") { a.Push(MakeBookAdd(e["address"].str())); return true; }
        return false;
    }
};
Screen* MakePaths() { return new PathsScreen(); }

// ------------------------------------------------------------------ устройства (адресная книга)
struct BookScreen : Screen {
    ListView lv;
    std::string addAddr;      // открыли «в устройства» с адресом - сразу спросить имя
    std::vector<BookEntry> View(App& a) {
        a.EnsureBook();
        std::vector<BookEntry> v = a.book;
        const json::Value& d = a.s.wd["devices"];
        for (size_t i = 0; i < d.size(); ++i) {
            std::string h = d[i]["host"].str();
            bool dup = false;
            for (const auto& e : a.book) if (NormAddr(e.addr) == NormAddr(h)) dup = true;
            if (!dup) v.push_back({ d[i]["name"].str(), h, true });
        }
        return v;
    }
    std::string Seen(App& a, const std::string& addr) {
        std::string n = NormAddr(addr), r;
        for (size_t i = 0; i < a.s.sessions.size(); ++i) if (NormAddr(a.s.sessions[i]["address"].str()) == n) { r += "сессия "; break; }
        for (size_t i = 0; i < a.s.tree.size(); ++i) if (NormAddr(a.s.tree[i]["address"].str()) == n) { r += "дерево "; break; }
        for (size_t i = 0; i < a.s.paths_.size(); ++i) if (NormAddr(a.s.paths_[i]["address"].str()) == n) { r += "путь"; break; }
        return util::Trim(r);
    }
    void Edit(App& a, const std::string& oldName, const std::string& oldAddr) {
        a.Ask(oldName.empty() ? "новое устройство" : "изменить", "Имя показывается в дереве, сессиях и путях рядом с адресом.",
              { { "имя", oldName }, { "адрес Yggdrasil", oldAddr } }, [&a, oldName, oldAddr](const std::vector<std::string>& v) {
            std::string name = v[0], addr = NormAddr(v[1]);
            if (name.empty() || addr.empty()) return;
            if (name.find('\t') != std::string::npos) { a.Flash("В имени не должно быть табуляции", RD); return; }
            if (!ygg::IsYggAddress(addr)) { a.Flash("Адрес должен быть адресом Yggdrasil (200:… или 300:…)", RD); return; }
            a.EnsureBook();
            bool upd = false;
            for (auto& e : a.book)
                if ((!oldAddr.empty() && NormAddr(e.addr) == NormAddr(oldAddr) && e.name == oldName) || NormAddr(e.addr) == addr) { e.name = name; e.addr = addr; upd = true; break; }
            if (!upd) a.book.push_back({ name, addr, false });
            if (SaveBook(a.book)) a.Flash(upd ? "Запись обновлена." : "Устройство добавлено.", G);
            else a.Flash("Не удалось записать " + BookPath(), RD);
        });
    }
    void OnShow(App& a) override {
        if (!addAddr.empty()) { std::string ad = addAddr; addAddr.clear(); a.EnsureBook(); std::string nm = a.BookName(ad); Edit(a, nm, ad); }
    }
    void Draw(Canvas& c, App& a) override {
        auto v = View(a);
        Header(c, "УСТРОЙСТВА", "адресная книга · " + BookPath());
        int W = c.w - 4, vis = std::max(3, c.h - 10);
        lv.Clamp((int)v.size(), vis);
        c.Box(2, 3, W, vis + 3, "устройства", BF, B);
        int nw = 18, aw = std::max(24, W - 58);
        c.Put(5, 4, "имя", DM); c.Put(6 + nw, 4, "адрес", DM); c.Put(7 + nw + aw, 4, "пинг", DM); c.Put(19 + nw + aw, 4, "сейчас видно", DM);
        for (int i = 0; i < vis && lv.top + i < (int)v.size(); ++i) {
            int idx = lv.top + i, y = 5 + i;
            const BookEntry& e = v[(size_t)idx];
            bool on = idx == lv.sel;
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Put(5, y, util::U8Fit(e.name, (size_t)nw), on ? WH : TX, true);
            c.Put(6 + nw, y, util::U8Fit(e.addr, (size_t)aw), DM);
            double p = a.PingOf(e.addr);
            c.Put(7 + nw + aw, y, p == -3 ? "—" : p == -2 ? "…" : p < 0 ? "нет ответа" : util::Fixed(p, 0) + " мс", p == -3 ? DM : p == -2 ? B : p < 0 ? RD : G);
            std::string seen = Seen(a, e.addr);
            if (e.fromWd) seen += std::string(seen.empty() ? "" : " · ") + "сторож";
            c.Put(19 + nw + aw, y, seen.empty() ? "—" : seen, seen.empty() ? DM : B, false, Color(), 2 + W - 1);
        }
        if (v.empty()) c.Put(5, 5, "записей нет - a добавить, или «в устройства» [b] на экранах дерева, сессий и путей", DM, false, Color(), c.w - 3);
        int ny = 3 + vis + 3;
        for (const auto& l : Wrap("Имена из книги видны в дереве, сессиях и путях. Устройства сторожа (пометка «сторож») правятся в самом стороже.", c.w - 6))
            if (ny < c.h - 1) c.Put(3, ny++, l, DM);
        Footer(c, a, { { "a", "добавить" }, { "e", "изменить" }, { "d", "удалить" }, { "p", "пинг" }, { "P", "пинг всех" }, { "c", "копировать" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        auto v = View(a);
        if (lv.Handle(k, (int)v.size(), std::max(3, a.term.canvas.h - 10))) return true;
        if (k.is('a') || k.text == "ф") { Edit(a, "", ""); return true; }
        if (k.is('P') || k.text == "З") { for (const auto& e : v) a.PingAsync(e.addr, true); a.Flash("Пингую все устройства…"); return true; }
        if (lv.sel >= (int)v.size()) return false;
        BookEntry e = v[(size_t)lv.sel];
        if (k.is('e') || k.text == "у" || k.k == K::Enter) {
            if (e.fromWd) { a.Flash("Это устройство сторожа - правится в ygg-watchdog (экран «Устройства»).", OR); return true; }
            Edit(a, e.name, e.addr); return true;
        }
        if (k.is('d') || k.text == "в") {
            if (e.fromWd) { a.Flash("Это устройство сторожа - удаляется в ygg-watchdog.", OR); return true; }
            a.Confirm("удалить", "Удалить из книги «" + e.name + "» (" + e.addr + ")?\n\nEnter - удалить", [&a, e] {
                a.book.erase(std::remove_if(a.book.begin(), a.book.end(), [&](const BookEntry& x) { return x.name == e.name && x.addr == e.addr; }), a.book.end());
                SaveBook(a.book);
                a.Flash("Удалено.");
            }, true);
            return true;
        }
        if (k.is('p') || k.text == "з") { a.PingAsync(e.addr, true); return true; }
        if (k.is('c') || k.text == "с") { a.Copy(e.addr, "Адрес"); return true; }
        return false;
    }
};
Screen* MakeBook() { return new BookScreen(); }
Screen* MakeBookAdd(const std::string& addr) { auto* b = new BookScreen(); b->addAddr = addr; return b; }

// ------------------------------------------------------------------ удалённый узел
struct RemoteScreen : Screen {
    LineEdit key;
    bool fieldFocus = true;
    ListView lv;
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> out = std::make_shared<std::vector<std::pair<std::string, std::string>>>();   // строка, ключ в ней
    std::shared_ptr<std::string> title = std::make_shared<std::string>();
    bool asked = false;
    bool Typing() const override { return fieldFocus; }
    // открыли с ключом («сведения» в дереве, сессиях, путях) - сразу спросить NodeInfo
    void OnShow(App& a) override { if (!asked && IsHexKey(util::Trim(key.text))) { asked = true; Ask(a, "getNodeInfo", "NodeInfo"); } }
    void Ask(App& a, const std::string& cmd, const std::string& t) {
        std::string k = util::Trim(key.text);
        if (!IsHexKey(k)) { a.Flash("Нужен публичный ключ узла - 64 hex-символа", RD); return; }
        if (!HasCommand(a.s, cmd)) { a.Flash("Эта версия Yggdrasil не знает команду " + cmd, OR); return; }
        auto mu_ = mu; auto O = out; auto T = title;
        { std::lock_guard<std::mutex> lk(*mu_); O->clear(); *T = t + " · жду ответа узла (до 20 с)…"; }
        fieldFocus = false;
        lv = ListView();
        a.Bg([cmd, k, mu_, O, T, t] {
            json::Value v; std::string err;
            std::vector<std::pair<std::string, std::string>> lines;
            std::string tt = t;
            if (!ygg::Ctl({ cmd, "key=" + k }, v, err, 20000)) tt += " · узел не ответил: " + err;
            else for (const auto& l : util::Lines(json::Dump(v, true))) {
                std::string found;
                size_t q = l.find('"');
                while (q != std::string::npos) {
                    size_t e = l.find('"', q + 1);
                    if (e == std::string::npos) break;
                    if (IsHexKey(l.substr(q + 1, e - q - 1))) { found = l.substr(q + 1, e - q - 1); break; }
                    q = l.find('"', e + 1);
                }
                lines.push_back({ l, found });
            }
            std::lock_guard<std::mutex> lk(*mu_);
            *O = lines; *T = tt;
            return std::string();
        }, nullptr);
    }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "УДАЛЁННЫЙ УЗЕЛ")) return;
        Header(c, "УДАЛЁННЫЙ УЗЕЛ", "запросы по ключу: getNodeInfo, debug_remote*");
        int W = c.w - 4;
        c.Box(2, 3, W, 4, "публичный ключ узла", BF, B);
        c.FillBg(4, 4, 2 + W - 2, fieldFocus ? BH : Hex("#161b22"));
        int ex = c.Put(5, 4, key.text.empty() && !fieldFocus ? "64 hex-символа - из дерева, сессий, путей ([i]) или вставь" : key.text,
                       key.text.empty() ? DM2 : WH, false, Color(), 2 + W - 2);
        if (fieldFocus) c.Put(ex, 4, "▌", B);
        c.Seq(4, 5, { { "[n]", B, true }, { " NodeInfo   ", TX }, { "[s]", B, true }, { " сведения   ", TX }, { "[p]", B, true }, { " его пиры   ", TX },
                      { "[t]", B, true }, { " его дерево", TX } }, 2 + W - 1);
        std::vector<std::pair<std::string, std::string>> o; std::string t;
        { std::lock_guard<std::mutex> lk(*mu); o = *out; t = *title; }
        int H = c.h - 10;
        c.Box(2, 7, W, H, t.empty() ? "ответ" : t, BF, B);
        int vis = H - 2;
        lv.Clamp((int)o.size(), vis);
        for (int i = 0; i < vis && lv.top + i < (int)o.size(); ++i) {
            int y = 8 + i;
            bool on = !fieldFocus && lv.top + i == lv.sel;
            const auto& [line, k] = o[(size_t)(lv.top + i)];
            RowBg(c, 3, y, 2 + W - 1, on);
            c.Put(5, y, line, !k.empty() ? G : TX, on, Color(), 2 + W - 1);
        }
        if (o.empty() && t.empty()) {
            int ny = 8;
            for (const auto& l : Wrap("NodeInfo - то, что узел сам о себе рассказывает (имя, версия, платформа). Отвечает только узел в сети. "
                                      "Зелёные строки ответа - ключи других узлов: выбери и нажми Enter, чтобы спросить уже его.", W - 4)) c.Put(5, ny++, l, DM);
        }
        if (fieldFocus) Footer(c, a, { { "Enter", "NodeInfo" }, { "Tab", "к ответу и командам" }, { "Esc", "назад" } });
        else Footer(c, a, { { "n s p t", "запрос" }, { "Enter", "спросить узел из строки" }, { "Tab", "к ключу" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        if (k.k == K::Tab) { fieldFocus = !fieldFocus; return true; }
        if (fieldFocus) {
            if (k.k == K::Enter) { Ask(a, "getNodeInfo", "NodeInfo"); return true; }
            if (k.k == K::Esc) return false;
            return key.Handle(k) || k.k == K::Char;
        }
        std::vector<std::pair<std::string, std::string>> o;
        { std::lock_guard<std::mutex> lk(*mu); o = *out; }
        if (lv.Handle(k, (int)o.size(), a.term.canvas.h - 12)) return true;
        if (k.is('n') || k.text == "т") { Ask(a, "getNodeInfo", "NodeInfo"); return true; }
        if (k.is('s') || k.text == "ы") { Ask(a, "debug_remoteGetSelf", "сведения"); return true; }
        if (k.is('p') || k.text == "з") { Ask(a, "debug_remoteGetPeers", "его пиры"); return true; }
        if (k.is('t') || k.text == "е") { Ask(a, "debug_remoteGetTree", "его дерево"); return true; }
        if (k.k == K::Enter && lv.sel < (int)o.size() && !o[(size_t)lv.sel].second.empty()) {
            key.text = o[(size_t)lv.sel].second;
            Ask(a, "getNodeInfo", "NodeInfo");
            return true;
        }
        return false;
    }
};
Screen* MakeRemote(const std::string& key) {
    auto* r = new RemoteScreen();
    r->key.text = key;
    r->fieldFocus = key.empty();
    return r;
}

// ------------------------------------------------------------------ интерфейсы
struct IfacesScreen : Screen {
    void OnShow(App& a) override { if (!a.cfgLoaded) a.ReloadConfig(); }
    void Draw(Canvas& c, App& a) override {
        if (NoCtl(c, a, "ИНТЕРФЕЙСЫ")) return;
        Header(c, "ИНТЕРФЕЙСЫ", "getTUN · getMulticastInterfaces · Listen");
        int W = c.w - 4, lw = std::max(36, W * 42 / 100), rw = W - lw - 1, H = c.h - 5;
        const Snap& s = a.s;
        c.Box(2, 3, lw, H, "TUN-адаптер", BF, B);
        int y = 4, x = 4, x1 = 2 + lw - 1;
        if (s.tun.isObject()) {
            bool en = s.tun["enabled"].boolean(true);
            c.Seq(x, y++, { { "состояние ", DM }, { en ? "включён" : "выключен", en ? G : OR } }, x1);
            c.Seq(x, y++, { { "имя       ", DM }, { s.tun["name"].str(), TX } }, x1);
            c.Seq(x, y++, { { "MTU       ", DM }, { s.tun["mtu"].text(), TX } }, x1);
        } else c.Put(x, y++, "getTUN не ответил", DM);
        ++y;
        c.Put(x, y++, "адрес", DM); c.Put(x, y++, s.self["address"].str(), WH, false, Color(), x1);
        c.Put(x, y++, "подсеть", DM); c.Put(x, y++, s.self["subnet"].str(), TX, false, Color(), x1);
        ++y;
        c.Put(x, y++, "адреса на интерфейсе (ip)", DM);
        if (s.tunAddrs.empty()) c.Put(x, y++, "—", DM);
        for (const auto& ad : s.tunAddrs) if (y < 3 + H - 4) c.Put(x, y++, ad, TX, false, Color(), x1);
        ++y;
        for (const auto& l : Wrap("Подсеть /64 можно раздавать устройствам в локальной сети - у каждого будет свой адрес Yggdrasil без установки Yggdrasil на них.", lw - 4))
            if (y < 3 + H - 1) c.Put(x, y++, l, DM, false, Color(), x1);
        int rx = 2 + lw + 1;
        int mh = std::max(5, H / 2);
        c.Box(rx, 3, rw, mh, "автопоиск соседей · multicast", BF, B);
        y = 4; x = rx + 2; x1 = rx + rw - 1;
        json::Value arr = FirstArray(s.multicast);
        if (arr.size() == 0 && s.multicast.isObject())
            for (const auto& kv : s.multicast.obj) if (y < 3 + mh - 1) c.Put(x, y++, kv.first + (kv.second.isObject() ? " " + json::Dump(kv.second) : ""), TX, false, Color(), x1);
        for (size_t i = 0; i < arr.size() && y < 3 + mh - 1; ++i) {
            const json::Value& e = arr[i];
            if (!e.isObject()) { c.Put(x, y++, e.str(), TX, false, Color(), x1); continue; }
            std::vector<std::string> f;
            if (e["beacon"].boolean()) f.push_back("beacon");
            if (e["listen"].boolean()) f.push_back("listen");
            if (e["port"].i64()) f.push_back("порт " + e["port"].text());
            if (e["priority"].i64()) f.push_back("приоритет " + e["priority"].text());
            if (e["password"].boolean()) f.push_back("с паролем");
            if (!e["address"].str().empty()) f.push_back(e["address"].str());
            c.Seq(x, y++, { { "● " + (e["name"].str().empty() ? json::Dump(e) : e["name"].str()), G }, { "  " + util::Join(f, " · "), DM } }, x1);
        }
        if (arr.size() == 0 && !s.multicast.isObject()) c.Put(x, y++, "multicast выключен или недоступен", DM, false, Color(), x1);
        int ly = 3 + mh;
        c.Box(rx, ly, rw, H - mh, "входящие · Listen из конфига", BF, B);
        y = ly + 1;
        const json::Value& L = a.cfgOrig["Listen"];
        if (!a.cfgLoaded) c.Put(x, y++, a.cfgLoading ? "читаю конфиг…" : "конфиг не прочитан: " + a.cfgErr, DM, false, Color(), x1);
        else if (L.size() == 0) c.Put(x, y++, "Listen пуст - входящих пиров не ждём", DM, false, Color(), x1);
        for (size_t i = 0; i < L.size() && y < ly + H - mh - 1; ++i) c.Put(x, y++, "● " + L[i].str(), B, false, Color(), x1);
        Footer(c, a, { { "m", "настроить автопоиск" }, { "l", "настроить Listen" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (NoCtlKey(k, a)) return true;
        if (k.is('m') || k.text == "ь") { a.Push(MakeConfig("MulticastInterfaces")); return true; }
        if (k.is('l') || k.text == "д") { a.Push(MakeConfig("Listen")); return true; }
        return false;
    }
};
Screen* MakeIfaces() { return new IfacesScreen(); }

} // namespace pnl
