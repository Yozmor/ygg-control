// ygg_control.cpp - ygg-control: панель управления Yggdrasil для Windows.
//
// Всё, что умеет Yggdrasil, в одном окне: узел, пиры, дерево маршрутов,
// сессии, пути, запросы к удалённым узлам, интерфейсы, служба, лог,
// конфиг, ключ узла и любая команда yggdrasilctl. Плюс строка сторожа
// (ygg-watchdog), если он установлен.
//
// Данные - через yggdrasilctl -json (фоновый поток, раз в 3 секунды).
// Отрисовка - Direct2D (ui.h), без WebView и сторонних библиотек.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include "core.h"
#include "ui.h"
#include <dwmapi.h>
#include <commdlg.h>
#include <functional>
#include <set>
#include <cmath>
#include <memory>
#include <cstdlib>

using namespace ui;

static const float DW = 1120, DH = 700;                 // дизайн-холст
static const float MX = 304, MY = 78, MW = 792, MH = 536; // рабочая область
static const UINT WM_APP_TASK = WM_APP + 2;
static const UINT_PTR TIMER_CLOCK = 1, TIMER_BLINK = 2;

// ------------------------------------------------------------------
// Фоновые задачи: work() в отдельном потоке, done() - в потоке окна
// ------------------------------------------------------------------
struct AsyncTask {
    HWND hwnd;
    std::function<std::wstring()> work;
    std::function<void(const std::wstring&)> done;
    std::wstring result;
};

static DWORD WINAPI AsyncProc(LPVOID p) {
    AsyncTask* t = (AsyncTask*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try { t->result = t->work(); } catch (...) { t->result = L"!внутренняя ошибка"; }
    CoUninitialize();
    PostMessageW(t->hwnd, WM_APP_TASK, 0, (LPARAM)t);
    return 0;
}

// ------------------------------------------------------------------
// Таблица с выбором строки и прокруткой (строки рисует сам экран)
// ------------------------------------------------------------------
struct Grid {
    int sel = -1, scroll = 0, count = 0;
    float x = 0, y = 0, w = 0, h = 0, rowH = 30;
    int Visible() const { return (std::max)(1, (int)(h / rowH)); }
    void Clamp() {
        int mx = (std::max)(0, count - Visible());
        if (scroll > mx) scroll = mx;
        if (scroll < 0) scroll = 0;
        if (sel >= count) sel = count - 1;
    }
    bool Inside(D2D1_POINT_2F p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
    bool Click(D2D1_POINT_2F p) {
        if (!Inside(p)) return false;
        int i = scroll + (int)((p.y - y) / rowH);
        if (i >= 0 && i < count) sel = i;
        return true;
    }
    void Wheel(int delta) { scroll -= delta / 40; Clamp(); }
    void Move(int d) {
        if (count == 0) return;
        sel = sel < 0 ? 0 : sel + d;
        if (sel < 0) sel = 0;
        if (sel >= count) sel = count - 1;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + Visible()) scroll = sel - Visible() + 1;
        Clamp();
    }
};

// ------------------------------------------------------------------
// Мелочи
// ------------------------------------------------------------------
// Короткое имя пира из адреса: tls://ygg-msk-1.averyan.ru:8362 -> YGG-MSK-1
static std::wstring ShortName(const std::wstring& uri) {
    util::PeerUri u = util::ParseUri(uri);
    std::wstring h = u.ok ? u.host : uri;
    std::vector<std::wstring> parts;
    size_t st = 0;
    while (true) {
        size_t d = h.find(L'.', st);
        parts.push_back(h.substr(st, d == std::wstring::npos ? std::wstring::npos : d - st));
        if (d == std::wstring::npos) break;
        st = d + 1;
    }
    std::wstring s = parts.empty() ? h : parts[0];
    bool numeric = !s.empty() && (iswdigit(s[0]) || h.find(L':') != std::wstring::npos);
    static const wchar_t* generic[] = { L"ygg", L"yggdrasil", L"peer", L"peers", L"node", L"public", L"y" };
    for (auto g : generic) if (_wcsicmp(s.c_str(), g) == 0 && parts.size() > 2) { s = parts[1]; break; }
    if (numeric) s = h;
    if (s.size() > 16) s = s.substr(0, 16);
    for (auto& c : s) c = towupper(c);
    return s;
}

static std::vector<std::wstring> PrettyLines(const json::Value& v) {
    std::vector<std::wstring> out;
    std::wstringstream ss(json::DumpW(v, true));
    std::wstring l;
    while (std::getline(ss, l)) out.push_back(l);
    return out;
}

static std::vector<std::wstring> TextLines(const std::wstring& t) {
    std::vector<std::wstring> out;
    std::wstringstream ss(t);
    std::wstring l;
    while (std::getline(ss, l)) { if (!l.empty() && l.back() == L'\r') l.pop_back(); out.push_back(l); }
    return out;
}

// Первый массив в ответе (ответы разных версий бывают обёрнуты по-разному)
static json::Value FirstArray(const json::Value& v) {
    if (v.isArray()) return v;
    if (v.isObject() && v.obj) for (const auto& kv : *v.obj) if (kv.second.isArray()) return kv.second;
    return json::Value();
}

static bool IsHexKey(const std::wstring& s) {
    if (s.size() != 64) return false;
    for (wchar_t c : s) if (!iswxdigit(c)) return false;
    return true;
}

enum Screen {
    SCR_SELF, SCR_PEERS, SCR_TREE, SCR_SESSIONS, SCR_PATHS, SCR_REMOTE, SCR_IFACES,
    SCR_SERVICE, SCR_LOG, SCR_CONFIG, SCR_KEY, SCR_COMMANDS, SCR_WATCHDOG, SCR_BOOK, SCR_HISTORY, SCR_COUNT,
    SCR_ADDPEER = 100
};

enum {
    NAV_FIRST = 10,     // NAV_FIRST + Screen
    BTN_COPY_ADDR = 40, BTN_COPY_KEY, BTN_REFRESH, BTN_START_SVC,
    BTN_PEER_ADD = 50, BTN_PEER_DEL, BTN_PEER_PING, BTN_PEER_TOCFG,
    BTN_AP_ADD = 60, BTN_AP_CHECK, BTN_AP_CANCEL,
    BTN_AP_PROTO = 70,       // +0..4
    BTN_AP_WHERE = 80,       // +0..2
    BTN_TREE_ZOUT = 90, BTN_TREE_ZIN, BTN_TREE_FIT, BTN_TREE_PATH, BTN_TREE_PING, BTN_TREE_ASK, BTN_TREE_COPY,
    BTN_SES_PING = 100, BTN_SES_ASK, BTN_SES_COPY,
    BTN_PATH_PING = 110, BTN_PATH_ASK, BTN_PATH_COPY, BTN_PATH_SORT = 115, // +0..3
    BTN_RM_NODEINFO = 120, BTN_RM_SELF, BTN_RM_PEERS, BTN_RM_TREE, BTN_RM_USE,
    BTN_IF_CFG = 130, BTN_IF_LISTEN,
    BTN_SV_START = 140, BTN_SV_STOP, BTN_SV_RESTART, BTN_SV_LATER, BTN_SV_AUTO, BTN_SV_MANUAL, BTN_SV_DISABLED,
    BTN_SV_CONFDIR, BTN_SV_EXEDIR, BTN_SV_REREAD,
    BTN_LOG_LEVEL = 160,     // +0..3
    BTN_LOG_FOLLOW = 165, BTN_LOG_NOTEPAD, BTN_LOG_DIR,
    BTN_CF_ADD = 170, BTN_CF_REPLACE, BTN_CF_DEL, BTN_CF_SET, BTN_CF_TRUE, BTN_CF_FALSE,
    BTN_CF_APPLY, BTN_CF_SAVE, BTN_CF_REVERT,
    BTN_KEY_COPY = 190, BTN_KEY_EXPORT, BTN_KEY_NEW,
    BTN_CMD_RUN = 200,
    BTN_BK_SAVE = 220, BTN_BK_DEL, BTN_BK_PING, BTN_BK_PINGALL, BTN_BK_COPY,
    BTN_HIST_RANGE = 230,    // +0..2
    BTN_HIST_RELOAD = 235, BTN_HIST_ALL, BTN_HIST_NONE,
    BTN_SES_BOOK = 240, BTN_PATH_BOOK, BTN_TREE_BOOK,
    BTN_PUPD_CHECK = 250, BTN_PUPD_OPEN,
    BTN_WD_OPEN = 210, BTN_WD_TICK, BTN_WD_PICK, BTN_UPD_CHECK, BTN_UPD_INSTALL, BTN_WD_INSTALL, BTN_WD_REMOVE,
    BTN_CONFIRM_YES = 900, BTN_CONFIRM_NO = 901,
};

struct NavItem { int scr; const wchar_t* num; const wchar_t* label; bool gapBefore; bool warn; };
static const NavItem NAV[] = {
    { SCR_SELF,     L"01", L"СТАТУС УЗЛА",       false, false },
    { SCR_PEERS,    L"02", L"ПИРЫ",              false, false },
    { SCR_HISTORY,  L"",   L"ИСТОРИЯ ЗАДЕРЖЕК",  false, false },
    { SCR_TREE,     L"03", L"ДЕРЕВО МАРШРУТОВ",  false, false },
    { SCR_SESSIONS, L"04", L"СЕССИИ",            false, false },
    { SCR_PATHS,    L"05", L"ТАБЛИЦА ПУТЕЙ",     false, false },
    { SCR_REMOTE,   L"06", L"УДАЛЁННЫЙ УЗЕЛ",    false, false },
    { SCR_BOOK,     L"",   L"УСТРОЙСТВА",        false, false },
    { SCR_IFACES,   L"07", L"ИНТЕРФЕЙСЫ",        false, false },
    { SCR_SERVICE,  L"08", L"СЛУЖБА",            true,  true  },
    { SCR_LOG,      L"09", L"ЛОГ",               true,  false },
    { SCR_CONFIG,   L"10", L"КОНФИГ",            false, false },
    { SCR_KEY,      L"11", L"КЛЮЧ УЗЛА",         false, false },
    { SCR_COMMANDS, L"12", L"ВСЕ КОМАНДЫ",       false, false },
    { SCR_WATCHDOG, L"13", L"WATCHDOG",            true,  false },
};

// Узел дерева маршрутов для рисования
struct TreeNode {
    std::wstring key, address, parentKey;
    int bfsParent = -1;              // родитель в дереве «от меня»
    std::vector<int> kids;
    int depth = 0;                   // шагов от меня
    int rootDepth = -1;              // шагов до корня сети
    float wx = 0;                    // позиция в «листьях»
};

// Описание полей конфига
struct CfgHelp { const wchar_t* key; const wchar_t* text; };
static const CfgHelp CFG_HELP[] = {
    { L"Peers", L"Исходящие пиры: к кому подключаться. Формат tls://host:port, можно с ?key=... (закрепить ключ) и ?password=..." },
    { L"InterfacePeers", L"Пиры только через указанный сетевой интерфейс: \"имя интерфейса\": [\"uri\", ...]" },
    { L"Listen", L"Входящие: на каких адресах и портах принимать пиров, например tls://0.0.0.0:443. Пусто - никого не ждём." },
    { L"AdminListen", L"Адрес управления для yggdrasilctl (и этой панели). none - выключить управление." },
    { L"MulticastInterfaces", L"Автопоиск соседей в локальной сети. Каждая запись: Regex (какие интерфейсы), Beacon (сообщать о себе), Listen (принимать), Port, Priority, Password." },
    { L"AllowedPublicKeys", L"Белый список ключей входящих пиров. Пусто - пускать всех." },
    { L"IfName", L"Имя TUN-адаптера. auto - выбрать само, none - работать без адаптера." },
    { L"IfMTU", L"MTU адаптера, обычно 65535." },
    { L"NodeInfoPrivacy", L"true - не рассказывать другим узлам версию и платформу в NodeInfo." },
    { L"NodeInfo", L"Сведения о узле, которые видят другие через getNodeInfo, например {\"name\": \"KJOLESKAP\"}." },
    { L"LogLookups", L"Писать в лог поиск узлов (для отладки)." },
};

static const wchar_t* CfgHelpFor(const std::wstring& key) {
    for (const auto& h : CFG_HELP) if (key == h.key) return h.text;
    return L"Нет описания: поле этой версии Yggdrasil, правится как JSON.";
}

static bool IsHiddenCfgKey(const std::wstring& k) { return k == L"PrivateKey" || k == L"PrivateKeyPath"; }

// ==================================================================
// Окно
// ==================================================================
struct App {
    HWND hwnd = nullptr;
    Renderer r;
    ygg::DataSource data;
    ygg::Snapshot snap;
    int screen = SCR_SELF;
    std::vector<HitBox> hits;
    int hoverId = 0, pressId = 0;
    D2D1_POINT_2F mouse = { 0, 0 };

    // сообщение в подвале
    std::wstring flash;
    unsigned flashColor = 0;
    long long flashUntil = 0;

    // долгие действия (меняют что-то) - по одному за раз
    bool busy = false;
    std::wstring busyLabel;

    // подтверждение
    bool confirmOpen = false;
    std::wstring confirmText, confirmYes;
    unsigned confirmColor = COL_WARN;
    std::function<void()> confirmAction;

    // поля и списки
    TextBox tbApUri, tbApPass, tbApKey, tbRemoteKey, tbLogSearch, tbCfgKey, tbCfgItem;
    std::vector<TextBox> tbCmdArgs = std::vector<TextBox>(5);
    ListBox lbRemoteOut, lbIfMulticast, lbIfListen, lbLog, lbCfgKeys, lbCfgItems, lbCmds, lbCmdOut;
    TextBox* focusTb = nullptr;
    ListBox* focusLb = nullptr;
    bool navFocus = true;       // стрелки вверх/вниз ходят по командам слева
    bool gridFocus = false;     // стрелки ходят по таблице экрана (пиры, сессии, пути)
    int focusBtn = 0;           // кнопка с фокусом клавиатуры (Enter / Пробел - нажать)
    std::vector<int> frameButtons;   // доступные кнопки рабочей области в этом кадре (порядок Tab)
    Grid gPeers, gSessions, gPaths;

    // конфиг (в памяти: исходный и правленый)
    bool cfgLoaded = false, cfgLoading = false;
    std::wstring cfgError;
    json::Value cfgOrig, cfgEdit;

    App() {
        tbApUri.id = 300; tbApUri.placeholder = L"tls://host:port";
        tbApPass.id = 301; tbApPass.placeholder = L"нет";
        tbApKey.id = 302; tbApKey.placeholder = L"любой";
        tbBkName.id = 320; tbBkAddr.id = 321; tbBkAddr.placeholder = L"200:…";
        tbRemoteKey.id = 303; tbRemoteKey.placeholder = L"64 hex-символа публичного ключа";
        tbLogSearch.id = 304; tbLogSearch.placeholder = L"поиск…";
        tbCfgKey.id = 305; tbCfgKey.placeholder = L"ключ";
        tbCfgItem.id = 306;
        for (size_t i = 0; i < tbCmdArgs.size(); ++i) tbCmdArgs[i].id = 310 + (int)i;
        lbRemoteOut.emptyText = L"введи ключ узла и выбери запрос";
        lbRemoteOut.rowH = 20;
        lbIfMulticast.emptyText = L"multicast выключен или недоступен";
        lbIfListen.emptyText = L"Listen пуст - входящих не ждём";
        lbLog.rowH = 20;
        lbLog.emptyText = L"лог пуст или не найден";
        lbCfgKeys.emptyText = L"конфиг не загружен";
        lbCfgItems.emptyText = L"пусто";
        lbCmds.emptyText = L"список команд не получен";
        lbCmdOut.rowH = 20;
        lbCmdOut.emptyText = L"выбери команду и нажми «Выполнить»";
    }

    Theme Th() const { return ThemeBlue(); }
    void Redraw() { InvalidateRect(hwnd, nullptr, FALSE); }
    long long Now() const { return (long long)time(nullptr); }

    void Flash(const std::wstring& s, unsigned color = 0) {
        flash = s; flashColor = color; flashUntil = Now() + 10;
        Redraw();
    }

    void Async(std::function<std::wstring()> work, std::function<void(const std::wstring&)> done) {
        AsyncTask* t = new AsyncTask{ hwnd, work, done, L"" };
        HANDLE th = CreateThread(nullptr, 0, AsyncProc, t, 0, nullptr);
        if (th) CloseHandle(th); else delete t;
    }

    // Действие, которое что-то меняет: одно за раз, видно в подвале.
    // Результат: "" - успех (покажем okMsg), "!текст" - ошибка.
    void Op(const std::wstring& label, std::function<std::wstring()> work, const std::wstring& okMsg,
            std::function<void(bool)> after = nullptr) {
        if (busy) { Flash(L"Подожди, ещё выполняется: " + busyLabel, COL_WARN); return; }
        busy = true; busyLabel = label;
        Redraw();
        Async(work, [this, okMsg, after](const std::wstring& res) {
            busy = false;
            bool ok = res.empty() || res[0] != L'!';
            if (ok) Flash(res.empty() ? okMsg : res);
            else Flash(L"Ошибка: " + res.substr(1), COL_ALERT);
            data.RefreshNow();
            if (after) after(ok);
        });
    }

    void Confirm(const std::wstring& text, const std::wstring& yes, std::function<void()> action, unsigned color = COL_WARN) {
        confirmText = text; confirmYes = yes; confirmAction = action; confirmColor = color; confirmOpen = true;
        SetTb(nullptr);
        Redraw();
    }

    // ---------- производные данные ----------
    std::wstring SelfAddr() const { return snap.self[L"address"].str(); }
    std::wstring SelfKey() const { return snap.self[L"key"].str(); }

    int PeersUp() const { int n = 0; for (size_t i = 0; i < snap.peers.size(); ++i) if (snap.peers[i][L"up"].boolean()) ++n; return n; }

    std::wstring PeerNameByKey(const std::wstring& key) const {
        for (size_t i = 0; i < snap.peers.size(); ++i)
            if (snap.peers[i][L"key"].str() == key) return ShortName(snap.peers[i][L"remote"].str());
        return L"";
    }

    std::wstring PeerNameByPort(long long port) const {
        for (size_t i = 0; i < snap.peers.size(); ++i)
            if (snap.peers[i][L"port"].i64(-1) == port) return ShortName(snap.peers[i][L"remote"].str());
        return L"порт " + std::to_wstring(port);
    }

    bool InConfigPeers(const std::wstring& uri) const {
        const auto& p = cfgOrig[L"Peers"];
        for (size_t i = 0; i < p.size(); ++i) if (p[i].str() == uri) return true;
        return false;
    }

    bool HasCommand(const std::wstring& name) const {
        json::Value list = FirstArray(snap.commands);
        if (list.size() == 0) return true;   // список неизвестен - не мешаем
        for (size_t i = 0; i < list.size(); ++i) {
            std::wstring c = list[i].isObject() ? list[i][L"command"].str() : list[i].str();
            if (_wcsicmp(c.c_str(), name.c_str()) == 0) return true;
        }
        return false;
    }

    // Глубина узла в дереве (шагов до корня сети)
    int RootDepth(const std::wstring& key) const {
        std::map<std::wstring, std::wstring> parent;
        for (size_t i = 0; i < snap.tree.size(); ++i) parent[snap.tree[i][L"key"].str()] = snap.tree[i][L"parent"].str();
        int d = 0;
        std::wstring k = key;
        while (d < 256) {
            auto it = parent.find(k);
            if (it == parent.end()) return -1;
            if (it->second == k) return d;
            k = it->second; ++d;
        }
        return -1;
    }

    // ---------- виджеты ----------
    void Btn(int id, float x, float y, float w, float h, const std::wstring& label, unsigned color,
             bool filled = false, bool enabled = true) {
        bool blocked = !enabled || (confirmOpen && id < BTN_CONFIRM_YES);
        if (blocked) { r.Button(x, y, w, h, label, 0x4E5A64, false, false, false); return; }
        bool kf = focusBtn == id;
        r.Button(x, y, w, h, label, color, filled, hoverId == id || kf, pressId == id, 0x04101A);
        if (kf) r.Brackets(x - 4, y - 4, w + 8, h + 8, Rgb(Th().text, 0.9f), 8, 1.5f);
        hits.push_back({ x, y, w, h, id });
        if (x >= MX - 2 || confirmOpen) frameButtons.push_back(id);
    }

    // Переключатель-«фишка» (протокол, фильтр, режим)
    void Chip(int id, float x, float y, float w, const std::wstring& label, bool active, bool enabled = true) {
        const Theme th = Th();
        float h = 24;
        bool hov = hoverId == id || focusBtn == id;
        r.Fill(x, y, w, h, Rgb(th.accent, active ? 0.22f : (hov ? 0.10f : 0.03f)));
        r.Frame(x, y, w, h, Rgb(enabled ? th.accent : 0x4E5A64, active ? 0.95f : 0.45f));
        if (focusBtn == id) r.Brackets(x - 3, y - 3, w + 6, h + 6, Rgb(th.text, 0.9f), 6, 1.5f);
        r.Text(label, x, y, w, h, Rgb(enabled ? (active ? th.text : th.accent) : 0x4E5A64), 11, active, Center);
        if (enabled && !confirmOpen) { hits.push_back({ x, y, w, h, id }); frameButtons.push_back(id); }
    }

    void Field(TextBox& tb, float x, float y, float w, float h = 30) {
        tb.x = x; tb.y = y; tb.w = w; tb.h = h;
        r.DrawTextBox(tb, Th());
    }

    void List(ListBox& lb, float x, float y, float w, float h) {
        lb.x = x; lb.y = y; lb.w = w; lb.h = h;
        lb.Clamp();
        r.DrawList(lb, Th(), focusLb == &lb);
    }

    void SetTb(TextBox* tb) {
        for (TextBox* t : AllFields()) t->focus = (t == tb);
        focusTb = tb;
        if (tb) focusLb = nullptr;
    }

    std::vector<TextBox*> AllFields() {
        std::vector<TextBox*> v = { &tbApUri, &tbApPass, &tbApKey, &tbRemoteKey, &tbLogSearch, &tbCfgKey, &tbCfgItem,
                                    &tbBkName, &tbBkAddr };
        for (auto& t : tbCmdArgs) v.push_back(&t);
        return v;
    }

    void Caption(float x, float y, float w, const std::wstring& s) { r.Text(s, x, y, w, 16, Rgb(Th().accent, 0.8f), 10); }
    void Note(float x, float y, float w, const std::wstring& s) { r.TextWrapped(s, x, y, w, Rgb(Th().muted), 11); }

    void Row(float x, float y, float w, const std::wstring& k, const std::wstring& v, unsigned vc = 0, float kw = 120) {
        const Theme th = Th();
        r.Text(k, x, y, kw, 22, Rgb(th.muted), 11);
        r.Text(v, x + kw, y, w - kw, 22, Rgb(vc ? vc : th.text), 12);
    }

    // Заголовок столбца таблицы
    void ColHead(float x, float y, float w, const std::wstring& s, Align a = Left) {
        r.Text(s, x, y, w, 16, Rgb(Th().muted), 10, false, a);
    }

    // ---------- каркас ----------
    void DrawHeader() {
        const Theme th = Th();
        r.Text(L"YGGDRASIL", 24, 10, 260, 40, Rgb(th.text), 28);
        std::wstring node = snap.ctlOk ? SelfAddr() : (snap.service.state == SERVICE_RUNNING ? L"нет ответа yggdrasilctl" : L"служба не работает");
        r.Text(L"//  NODE " + node, 236, 18, 560, 24, Rgb(snap.ctlOk ? th.accent : COL_ALERT, 0.9f), 12);
        r.Line(24, 60, 290, 60, Rgb(th.accent, 0.8f), 2);
        r.Line(290, 60, DW - 24, 60, Rgb(th.accent, 0.25f), 1);

        r.Text(util::TimeText(Now()), DW - 280, 10, 256, 18, Rgb(th.muted), 12, false, Right);
        int up = PeersUp(), total = (int)snap.peers.size();
        std::wstring cnt = std::to_wstring(up) + L"/" + std::to_wstring(total);
        float cw = r.TextWidth(cnt, 12);
        r.Text(cnt, DW - 24 - cw, 30, cw, 18, Rgb(up > 0 ? th.text : COL_ALERT), 12);
        int bars = (std::min)(total, 6);
        float bx = DW - 24 - cw - 12 - bars * 6;
        for (int i = 0; i < bars; ++i) r.Fill(bx + i * 6, 34, 4, 10, Rgb(i < up ? th.accent : th.muted, i < up ? 0.9f : 0.4f));
        r.Text(L"UPLINK", bx - 62, 30, 56, 18, Rgb(th.accent, 0.85f), 11, false, Right);
    }

    void DrawNav() {
        const Theme th = Th();
        r.Panel(24, MY, 264, MH, th, L"КОМАНДЫ");
        float y = MY + 34;
        int cur = screen == SCR_ADDPEER ? SCR_PEERS : screen;
        for (const auto& n : NAV) {
            if (n.gapBefore) { r.Line(40, y + 6, 272, y + 6, Rgb(th.accent, 0.18f)); y += 14; }
            bool active = cur == n.scr, hov = hoverId == NAV_FIRST + n.scr;
            if (active) {
                r.Fill(30, y, 252, 24, Rgb(th.accent, navFocus ? 0.20f : 0.12f));
                r.Fill(30, y, 3, 24, Rgb(th.accent));
                r.Text(L">", 36, y, 12, 24, Rgb(th.accent), 12);
            }
            else if (hov) r.Fill(30, y, 252, 24, Rgb(th.accent, 0.07f));
            unsigned c = n.warn ? COL_WARN : (n.scr == SCR_WATCHDOG ? COL_OK : th.text);
            r.Text(n.label, 52, y, 226, 24, Rgb(c, active ? 1.0f : 0.85f), 12, active);
            if (!confirmOpen) hits.push_back({ 30, y, 252, 24, NAV_FIRST + n.scr });
            y += 26;
        }
        if (navFocus) r.Frame(28, MY + 30, 256, y - MY - 28, Rgb(th.accent, 0.35f));
    }

    // Нижняя строка: сторож слева, подсказки / сообщения справа
    void DrawFooter() {
        const Theme th = Th();
        r.Line(24, 624, DW - 24, 624, Rgb(th.accent, 0.25f));
        float y = 632;
        if (snap.wdExe.empty()) {
            r.Circle(32, y + 11, 4, Rgb(th.muted));
            r.Text(L"WATCHDOG  не найден", 44, y, 200, 22, Rgb(th.muted), 11);
            bool hov = hoverId == BTN_WD_PICK;
            r.Text(L"[указать ygg_watchdog.exe…]", 210, y, 260, 22, Rgb(th.accent, hov ? 1.0f : 0.75f), 11);
            if (!confirmOpen) hits.push_back({ 210, y, 230, 22, BTN_WD_PICK });
        } else {
            const auto& st = snap.wdStatus;
            std::wstring ph = st[L"last_phase"].str();
            long long lt = st[L"last_tick_at"].i64();
            bool stale = lt > 0 && Now() - lt > 10 * 60;
            (void)ph; (void)stale;
            std::wstring word; unsigned col;
            WdPhase(word, col);
            r.Disc(32, y + 11, 4, Rgb(col));
            bool hovW = hoverId == NAV_FIRST + SCR_WATCHDOG;
            r.Text(L"WATCHDOG", 44, y, 80, 22, Rgb(col, hovW ? 1.0f : 0.9f), 11, true);
            if (!confirmOpen) hits.push_back({ 24, y, 100, 22, NAV_FIRST + SCR_WATCHDOG });
            std::wstring info = word;
            if (snap.wdTask.ok && snap.wdTask.nextRun > 0) {
                long long left = snap.wdTask.nextRun - Now();
                if (snap.wdTask.state == 4) info += L"  ·  проверяет";
                else if (left >= 0) { wchar_t b[32]; swprintf(b, 32, L"  ·  next %02lld:%02lld", left / 60, left % 60); info += b; }
            }
            info += L"  ·  mode " + std::wstring(st[L"mode"].str() == L"regional" ? L"region" : L"normal");
            const auto& mp = st[L"main_peers"];
            if (mp.size() > 0) {
                int up = 0;
                for (size_t i = 0; i < mp.size(); ++i)
                    for (size_t j = 0; j < snap.peers.size(); ++j)
                        if (snap.peers[j][L"remote"].str() == mp[i].str() && snap.peers[j][L"up"].boolean()) { ++up; break; }
                info += L"  ·  peers " + std::to_wstring(up) + L"/" + std::to_wstring(mp.size());
            }
            r.Text(info, 128, y, 480, 22, Rgb(th.text, 0.85f), 11);
        }

        // справа: что выполняется / сообщение / подсказки
        std::wstring msg; unsigned mc = th.accent;
        if (busy) msg = L"выполняю: " + busyLabel + L"…";
        else if (!flash.empty() && Now() < flashUntil) { msg = flash; if (flashColor) mc = flashColor; }
        float right = DW - 24, left = 620;
        if (!msg.empty()) {
            float avail = right - left;
            if (r.TextWidth(msg, 11) > avail) { while (msg.size() > 1 && r.TextWidth(msg + L"…", 11) > avail) msg.pop_back(); msg += L"…"; }
            r.Text(msg, left, y, avail, 22, Rgb(mc), 11, false, Right);
            return;
        }
        struct Hint { int id; const wchar_t* text; };
        std::vector<Hint> hints;
        if (!snap.wdExe.empty()) { hints.push_back({ BTN_WD_OPEN, L"[W] watchdog" }); hints.push_back({ BTN_WD_TICK, L"[T] проверить" }); }
        float x = right;
        for (int i = (int)hints.size() - 1; i >= 0; --i) {
            float w = r.TextWidth(hints[i].text, 11);
            x -= w;
            bool hov = hoverId == hints[i].id;
            r.Text(hints[i].text, x, y, w + 2, 22, Rgb(th.accent, hov ? 1.0f : 0.75f), 11);
            if (!confirmOpen) hits.push_back({ x, y, w, 22, hints[i].id });
            x -= 22;
        }
    }

    void DrawConfirm() {
        const Theme th = Th();
        r.Fill(0, 0, DW, DH, Rgb(0x000000, 0.65f));
        float w = 600, x = (DW - w) / 2;
        float th1 = r.TextWrapped(confirmText, x + 20, 0, w - 40, Rgb(th.text), 13, false, false);
        float h = th1 + 130, y = (DH - h) / 2;
        r.Fill(x, y, w, h, Rgb(th.bg, 0.97f));
        r.Brackets(x, y, w, h, Rgb(confirmColor), 18, 2);
        r.Text(L"ПОДТВЕРДИ", x + 20, y + 14, w - 40, 20, Rgb(confirmColor), 12);
        r.TextWrapped(confirmText, x + 20, y + 44, w - 40, Rgb(th.text), 13);
        Btn(BTN_CONFIRM_YES, x + 20, y + h - 56, 270, 38, confirmYes + L"  [Enter]", confirmColor, true);
        Btn(BTN_CONFIRM_NO, x + w - 290, y + h - 56, 270, 38, L"ОТМЕНА  [Esc]", th.accent, false);
    }

    void Paint() {
        if (!r.EnsureTarget()) return;
        const Theme th = Th();
        r.BeginFrame(th.bg);
        hits.clear();
        frameButtons.clear();
        r.Backdrop(th);
        DrawHeader();
        DrawNav();
        switch (screen) {
            case SCR_SELF:     DrawSelf(); break;
            case SCR_PEERS:    DrawPeers(); break;
            case SCR_ADDPEER:  DrawAddPeer(); break;
            case SCR_TREE:     DrawTree(); break;
            case SCR_SESSIONS: DrawSessions(); break;
            case SCR_PATHS:    DrawPaths(); break;
            case SCR_REMOTE:   DrawRemote(); break;
            case SCR_IFACES:   DrawIfaces(); break;
            case SCR_SERVICE:  DrawService(); break;
            case SCR_LOG:      DrawLog(); break;
            case SCR_CONFIG:   DrawConfig(); break;
            case SCR_KEY:      DrawKey(); break;
            case SCR_COMMANDS: DrawCommands(); break;
            case SCR_WATCHDOG: DrawWatchdog(); break;
            case SCR_BOOK:     DrawBook(); break;
            case SCR_HISTORY:  DrawHistory(); break;
        }
        if (gridFocus) if (Grid* g = ScreenGrid()) r.Frame(g->x - 3, g->y - 3, g->w + 6, g->h + 6, Rgb(th.accent, 0.4f));
        DrawFooter();
        if (confirmOpen) DrawConfirm();
        r.EndFrame();
    }

    // Когда yggdrasilctl молчит - на экранах, которым он нужен, честно говорим
    bool NoCtl(const std::wstring& title) {
        if (snap.ctlOk) return false;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, title);
        r.Text(L"НЕТ СВЯЗИ С YGGDRASIL", MX + 24, MY + 60, MW - 48, 40, Rgb(COL_ALERT), 26);
        std::wstring why = snap.service.exists ? L"Служба: " + util::ServiceStateText(snap.service.state) + L". " : L"Служба Yggdrasil не найдена. ";
        why += L"yggdrasilctl: " + (snap.ctlError.empty() ? std::wstring(L"не ответил") : snap.ctlError);
        if (!snap.paths.adminListen.empty()) why += L"\nАдрес управления (AdminListen): " + snap.paths.adminListen;
        r.TextWrapped(why, MX + 24, MY + 110, MW - 48, Rgb(th.text), 12);
        if (snap.service.exists && snap.service.state != SERVICE_RUNNING)
            Btn(BTN_START_SVC, MX + 24, MY + 200, 260, 38, L"ЗАПУСТИТЬ СЛУЖБУ", th.accent, true);
        return true;
    }

    // ================== 01 СТАТУС УЗЛА ==================
    void DrawSelf() {
        if (NoCtl(L"СТАТУС УЗЛА  //  getSelf")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"СТАТУС УЗЛА  //  getSelf");
        float x = MX + 18, y = MY + 34, w = MW - 36;
        const auto& s = snap.self;
        Row(x, y, w, L"АДРЕС", s[L"address"].str(), th.text); y += 26;
        Row(x, y, w, L"ПОДСЕТЬ", s[L"subnet"].str()); y += 26;
        Row(x, y, w, L"КЛЮЧ", util::ShortKey(s[L"key"].str()) + L"   (ed25519)"); y += 26;
        Row(x, y, w, L"ВЕРСИЯ", s[L"build_name"].str() + L" " + s[L"build_version"].str()); y += 26;
        Row(x, y, w, L"МАРШРУТОВ", std::to_wstring(s[L"routing_entries"].i64())); y += 26;
        std::wstring tun;
        if (snap.tun.isObject()) {
            bool en = snap.tun[L"enabled"].boolean(true);
            tun = en ? snap.tun[L"name"].str() + L"   MTU " + std::to_wstring(snap.tun[L"mtu"].i64()) : L"TUN выключен";
        } else tun = L"—";
        Row(x, y, w, L"ИНТЕРФЕЙС", tun); y += 26;
        std::wstring svc = util::ServiceStateText(snap.service.state);
        if (snap.service.startedAt) svc += L"   ·   " + util::Duration((double)(Now() - snap.service.startedAt));
        Row(x, y, w, L"СЛУЖБА", svc, snap.service.state == SERVICE_RUNNING ? COL_OK : COL_ALERT); y += 34;

        // плитки
        int depth = RootDepth(SelfKey());
        struct Tile { const wchar_t* cap; std::wstring val; };
        Tile tiles[] = {
            { L"ПИРЫ", std::to_wstring(PeersUp()) + L" / " + std::to_wstring(snap.peers.size()) },
            { L"ГЛУБИНА", depth >= 0 ? std::to_wstring(depth) : L"—" },
            { L"ПУТЕЙ", std::to_wstring(snap.paths_.size()) },
            { L"СЕССИЙ", std::to_wstring(snap.sessions.size()) } };
        float tw = (w - 3 * 16) / 4;
        for (int i = 0; i < 4; ++i) {
            float tx = x + i * (tw + 16);
            r.Brackets(tx, y, tw, 62, Rgb(th.accent, 0.7f), 10, 1.5f);
            r.Text(tiles[i].cap, tx + 12, y + 6, tw - 24, 16, Rgb(th.accent, 0.8f), 10);
            r.Text(tiles[i].val, tx + 12, y + 22, tw - 24, 34, Rgb(th.text), 24);
        }
        y += 80;

        // трафик за 60 минут. Вертикальная ось подстраивается под значения: единица (B, KB, MB, GB)
        // и «круглый» шаг (1, 2 или 5 × 10^k) выбираются по максимуму, делений не больше четырёх.
        double mx = 0;
        for (const auto& p : snap.traffic) mx = (std::max)(mx, (std::max)(p.rx, p.tx));
        const wchar_t* units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
        int ui = 0; double uscale = 1;
        while (ui < 3 && mx / uscale >= 1024) { uscale *= 1024; ++ui; }
        double vmax = mx / uscale, vstep = 1, vtop = 4;
        if (mx <= 0) { ui = 1; uscale = 1024; vmax = 0; vstep = 1; vtop = 4; }   // нет трафика: шкала 0-4 KB/s
        else {
            double raw = vmax * 1.05 / 4;
            double mag = pow(10.0, floor(log10(raw)));
            double f = raw / mag;
            vstep = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * mag;
            vtop = ceil(vmax * 1.05 / vstep - 1e-9) * vstep;
            if (vtop < vstep) vtop = vstep;
        }
        int nT = (int)floor(vtop / vstep + 0.5);
        Caption(x, y, 460, std::wstring(L"ТРАФИК  60 МИН  (сумма по пирам)  ·  шкала ") + units[ui]);
        float cy = y + 24, ch = 96, ax = x + 56, aw = w - 56;
        for (int i = 0; i <= nT; ++i) {
            float gy = cy + ch - (ch - 6) * i / (float)nT;
            r.Line(ax, gy, ax + aw, gy, Rgb(th.accent, i == 0 ? 0.30f : 0.10f));
            wchar_t lb[32];
            if (vstep < 1) swprintf(lb, 32, L"%.1f", vstep * i); else swprintf(lb, 32, L"%.0f", vstep * i);
            r.Text(lb, x, gy - 8, 50, 16, Rgb(th.muted), 9, false, Right);
        }
        long long now = Now();
        auto px = [&](long long t) { return ax + aw * (float)(1.0 - (now - t) / 3600.0); };
        auto py = [&](double v) { return cy + ch - (float)(v / uscale / vtop) * (ch - 6); };
        r.Clip(ax, cy - 4, aw, ch + 8);
        for (size_t i = 1; i < snap.traffic.size(); ++i) {
            const auto& a = snap.traffic[i - 1]; const auto& b = snap.traffic[i];
            r.Line(px(a.t), py(a.rx), px(b.t), py(b.rx), Rgb(th.accent, 0.95f), 1.5f);
            r.Line(px(a.t), py(a.tx), px(b.t), py(b.tx), Rgb(COL_OK, 0.85f), 1.5f);
        }
        r.Unclip();
        if (snap.traffic.size() < 2) r.Text(L"данные копятся - график появится через пару обновлений", ax, cy, aw, ch, Rgb(th.muted), 11, false, Center);
        y = cy + ch + 6;
        double rx = snap.traffic.empty() ? 0 : snap.traffic.back().rx, tx = snap.traffic.empty() ? 0 : snap.traffic.back().tx;
        r.Text(L"▼ RX " + util::Rate(rx), x, y, 200, 18, Rgb(th.accent), 11);
        r.Text(L"▲ TX " + util::Rate(tx), x + 200, y, 200, 18, Rgb(COL_OK), 11);
        r.Text(L"всего с подключения пиров:  ↓ " + util::Bytes(snap.totalRx) + L"   ↑ " + util::Bytes(snap.totalTx),
               x, y, w, 18, Rgb(th.muted), 11, false, Right);

        float by = MY + MH - 50;
        Btn(BTN_COPY_ADDR, x, by, 200, 34, L"КОПИРОВАТЬ АДРЕС", th.accent);
        Btn(BTN_COPY_KEY, x + 214, by, 200, 34, L"КОПИРОВАТЬ КЛЮЧ", th.accent);
        Btn(BTN_REFRESH, x + 428, by, 160, 34, L"ОБНОВИТЬ", th.accent);
    }

    // ================== 02 ПИРЫ ==================
    std::map<std::wstring, double> pingMs;          // адрес -> мс (-1 нет ответа, -2 идёт)

    // ports: устройство из адресной книги - если пинг молчит (Windows по умолчанию режет
    // входящий ICMP фаерволом), пробуем подключиться к SMB/RDP/SSH; время подключения
    // примерно равно задержке.
    void PingAsync(const std::wstring& addr, bool ports = false) {
        if (addr.empty()) return;
        pingMs[addr] = -2;
        Redraw();
        Async([addr, ports] {
            double best = -1;
            for (int i = 0; i < 3; ++i) { double ms = util::Ping6(addr, 3000); if (ms >= 0 && (best < 0 || ms < best)) best = ms; }
            if (best < 0 && ports)
                for (const wchar_t* port : { L"445", L"3389", L"22" }) {
                    double ms = util::TcpConnect(util::NormalizeIpv6(addr), port, 2000);
                    if (ms >= 0) { best = ms; break; }
                }
            wchar_t b[32]; swprintf(b, 32, L"%.1f", best);
            return std::wstring(b);
        }, [this, addr](const std::wstring& res) { pingMs[addr] = _wtof(res.c_str()); Redraw(); });
    }

    std::wstring PingText(const std::wstring& addr) const {
        auto it = pingMs.find(addr);
        if (it == pingMs.end()) return L"";
        if (it->second == -2) return L"пинг…";
        if (it->second < 0) return L"пинг: нет ответа";
        wchar_t b[32]; swprintf(b, 32, L"пинг: %.0f мс", it->second);
        return b;
    }

    void DrawPeers() {
        if (NoCtl(L"ПИРЫ  //  getPeers")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"ПИРЫ  //  getPeers");
        float x = MX + 16;
        float hy = MY + 34;
        ColHead(x + 16, hy, 100, L"ИМЯ");
        ColHead(x + 150, hy, 200, L"АДРЕС");
        ColHead(x + 420, hy, 120, L"ЗАДЕРЖКА");
        ColHead(x + 560, hy, 120, L"ТРАФИК");
        ColHead(x + 680, hy, 80, L"ВРЕМЯ");
        r.Line(x, hy + 18, x + MW - 32, hy + 18, Rgb(th.accent, 0.2f));

        gPeers.x = x; gPeers.y = hy + 22; gPeers.w = MW - 32; gPeers.h = 280; gPeers.rowH = 35;
        gPeers.count = (int)snap.peers.size();
        gPeers.Clamp();
        double maxLat = 50;
        for (size_t i = 0; i < snap.peers.size(); ++i) maxLat = (std::max)(maxLat, snap.peers[i][L"latency"].num() / 1e6);
        r.Clip(gPeers.x, gPeers.y, gPeers.w, gPeers.h);
        for (int i = 0; i < gPeers.Visible() + 1 && gPeers.scroll + i < gPeers.count; ++i) {
            int idx = gPeers.scroll + i;
            const auto& p = snap.peers[idx];
            float ry = gPeers.y + i * gPeers.rowH;
            if (idx == gPeers.sel) { r.Fill(x, ry, gPeers.w, gPeers.rowH - 2, Rgb(th.accent, 0.13f)); r.Fill(x, ry, 3, gPeers.rowH - 2, Rgb(th.accent)); }
            bool up = p[L"up"].boolean(), in = p[L"inbound"].boolean();
            double lat = p[L"latency"].num() / 1e6;
            std::wstring uri = p[L"remote"].str();
            unsigned dot = !up ? 0x6B7A86 : (lat > 300 ? COL_WARN : COL_OK);
            r.Disc(x + 8, ry + 12, 4, Rgb(dot));
            r.Text(ShortName(uri), x + 16, ry + 2, 130, 18, Rgb(up ? th.text : th.muted), 12, true);
            std::wstring sub = in ? L"входящий" : (cfgLoaded ? (InConfigPeers(uri) ? L"конфиг" : L"только сейчас") : L"исходящий");
            r.Text(sub, x + 16, ry + 18, 130, 14, Rgb(th.muted), 9);
            r.Text(uri, x + 150, ry + 2, 262, 18, Rgb(th.accent, up ? 0.9f : 0.5f), 11);
            std::wstring err = p[L"last_error"].str();
            if (!up && !err.empty()) r.Text(err, x + 150, ry + 18, 262, 14, Rgb(COL_ALERT, 0.8f), 9);
            if (up && lat >= 0) {
                float bw = 70 * (float)(lat / maxLat);
                r.Fill(x + 420, ry + 8, 70, 8, Rgb(th.accent, 0.12f));
                r.Fill(x + 420, ry + 8, (std::max)(3.0f, bw), 8, Rgb(lat > 300 ? COL_WARN : th.accent, 0.85f));
                wchar_t b[32]; swprintf(b, 32, L"%.0f мс", lat);
                r.Text(b, x + 494, ry + 2, 64, 18, Rgb(lat > 300 ? COL_WARN : th.text), 11);
            } else r.Text(up ? L"—" : L"не подключён", x + 420, ry + 2, 140, 18, Rgb(th.muted), 11);
            if (up) {
                r.Text(L"↓" + util::Bytes(p[L"bytes_recvd"].num()), x + 560, ry + 2, 120, 16, Rgb(th.text), 10);
                r.Text(L"↑" + util::Bytes(p[L"bytes_sent"].num()), x + 560, ry + 17, 120, 16, Rgb(th.muted), 10);
                r.Text(util::Duration(p[L"uptime"].num()), x + 680, ry + 2, 90, 18, Rgb(th.text), 11);
            }
        }
        r.Unclip();
        if (snap.peers.size() == 0) r.Text(L"пиров нет - добавь кнопкой ниже", x, gPeers.y, gPeers.w, 30, Rgb(th.muted), 12);

        // подробности о выбранном
        float dy = gPeers.y + gPeers.h + 8;
        r.Line(x, dy, x + MW - 32, dy, Rgb(th.accent, 0.2f));
        dy += 6;
        if (gPeers.sel >= 0 && gPeers.sel < gPeers.count) {
            const auto& p = snap.peers[gPeers.sel];
            std::wstring a = p[L"address"].str();
            r.Text(p[L"remote"].str(), x, dy, MW - 32, 18, Rgb(th.text), 12); dy += 20;
            std::wstring line = L"адрес " + (a.empty() ? L"—" : a) + L"    ключ " + util::ShortKey(p[L"key"].str()) +
                                L"    порт " + std::to_wstring(p[L"port"].i64()) + L"    приоритет " + std::to_wstring(p[L"priority"].i64()) +
                                L"    cost " + std::to_wstring(p[L"cost"].i64());
            r.Text(line, x, dy, MW - 32, 18, Rgb(th.muted), 11); dy += 20;
            std::wstring err = p[L"last_error"].str();
            std::wstring pt = PingText(a);
            if (!err.empty()) r.Text(L"последняя ошибка: " + err, x, dy, MW - 250, 18, Rgb(COL_ALERT, 0.9f), 11);
            if (!pt.empty()) r.Text(pt, x, dy, MW - 32, 18, Rgb(th.accent), 11, false, Right);
        } else {
            int up = PeersUp();
            double sum = 0; int n = 0;
            for (size_t i = 0; i < snap.peers.size(); ++i) if (snap.peers[i][L"up"].boolean()) { sum += snap.peers[i][L"latency"].num() / 1e6; ++n; }
            wchar_t b[160];
            swprintf(b, 160, L"на связи %d из %d   ·   средняя задержка %.0f мс   ·   выбери пир, чтобы увидеть подробности",
                     up, (int)snap.peers.size(), n ? sum / n : 0.0);
            r.Text(b, x, dy, MW - 32, 18, Rgb(th.muted), 11);
        }

        const auto* sp = (gPeers.sel >= 0 && gPeers.sel < gPeers.count) ? &snap.peers[gPeers.sel] : nullptr;
        bool selOut = sp && !(*sp)[L"inbound"].boolean();
        bool selNotCfg = selOut && cfgLoaded && !InConfigPeers((*sp)[L"remote"].str());
        float by = MY + MH - 50;
        Btn(BTN_PEER_ADD, x, by, 170, 34, L"+ ДОБАВИТЬ", th.accent, true);
        Btn(BTN_PEER_DEL, x + 182, by, 170, 34, L"− УБРАТЬ", COL_WARN, false, selOut);
        Btn(BTN_PEER_PING, x + 364, by, 190, 34, L"ПРОВЕРИТЬ ВСЕ", th.accent);
        Btn(BTN_PEER_TOCFG, x + 566, by, 190, 34, L"В КОНФИГ", th.accent, false, selNotCfg);
    }

    void RemoveSelectedPeer() {
        if (gPeers.sel < 0 || gPeers.sel >= gPeers.count) return;
        std::wstring uri = snap.peers[gPeers.sel][L"remote"].str();
        bool inCfg = InConfigPeers(uri);
        std::wstring text = L"Отключить пир\n" + uri + L"?\n" +
            (inCfg ? L"Он прописан в конфиге - уберу и оттуда (копия старого конфига сохранится), иначе он вернётся после перезапуска службы."
                   : L"В конфиге его нет - просто отключу до перезапуска службы.");
        Confirm(text, L"УБРАТЬ", [this, uri, inCfg] {
            ygg::Paths p = snap.paths;
            json::Value cfg = json::Clone(cfgOrig);
            Op(L"убираю пир", [p, uri, inCfg, cfg]() mutable -> std::wstring {
                json::Value v; std::wstring err;
                if (!ygg::Ctl(p, L"removePeer uri=" + uri, v, err)) return L"!" + err;
                if (inCfg) {
                    auto& arr = (*cfg.obj)[L"Peers"];
                    if (arr.arr) arr.arr->erase(std::remove_if(arr.arr->begin(), arr.arr->end(),
                        [&](const json::Value& x) { return x.str() == uri; }), arr.arr->end());
                    std::wstring e = ygg::SaveConfig(p, cfg);
                    if (!e.empty()) return L"!отключил, но конфиг не сохранён: " + e;
                }
                return L"Пир убран" + std::wstring(inCfg ? L" и из конфига." : L".");
            }, L"", [this](bool) { ReloadConfig(); });
        });
    }

    void SelectedPeerToConfig() {
        if (gPeers.sel < 0 || gPeers.sel >= gPeers.count) return;
        std::wstring uri = snap.peers[gPeers.sel][L"remote"].str();
        AddPeersToConfig({ uri }, L"Пир записан в конфиг - останется после перезапуска.");
    }

    void AddPeersToConfig(std::vector<std::wstring> uris, const std::wstring& okMsg) {
        if (!cfgLoaded) { Flash(L"Конфиг ещё не прочитан: " + cfgError, COL_ALERT); return; }
        ygg::Paths p = snap.paths;
        json::Value cfg = json::Clone(cfgOrig);
        Op(L"записываю в конфиг", [p, cfg, uris]() mutable -> std::wstring {
            if (!cfg.obj) return L"!конфиг пуст";
            auto& arr = (*cfg.obj)[L"Peers"];
            if (!arr.isArray()) arr = json::MakeArray();
            for (const auto& u : uris) {
                bool has = false;
                for (const auto& x : *arr.arr) if (x.str() == u) has = true;
                if (!has) arr.arr->push_back(json::MakeString(u));
            }
            std::wstring e = ygg::SaveConfig(p, cfg);
            return e.empty() ? L"" : L"!" + e;
        }, okMsg, [this](bool) { ReloadConfig(); });
    }

    // ================== добавить пир ==================
    struct Check { int state = 0; std::wstring text; double ms = -1; };  // 0 нет, 1 идёт, 2 ок, 3 плохо, 4 пропущено
    Check apCheck[3];
    int apWhere = 0;          // 0 сейчас и в конфиг, 1 только сейчас, 2 только в конфиг
    bool apRunning = false;

    std::wstring ApUri() const {
        std::wstring base = util::Trim(tbApUri.text);
        std::vector<std::wstring> q;
        std::wstring pass = util::Trim(tbApPass.text), key = util::Trim(tbApKey.text);
        if (!key.empty()) q.push_back(L"key=" + key);
        if (!pass.empty()) q.push_back(L"password=" + pass);
        if (q.empty()) return base;
        std::wstring sep = base.find(L'?') == std::wstring::npos ? L"?" : L"&";
        for (auto& s : q) { base += sep + s; sep = L"&"; }
        return base;
    }

    void SetScheme(const std::wstring& scheme) {
        std::wstring u = util::Trim(tbApUri.text);
        size_t s = u.find(L"://");
        tbApUri.Set(scheme + L"://" + (s == std::wstring::npos ? u : u.substr(s + 3)));
    }

    void DrawAddPeer() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"ПИРЫ  //  добавить");
        float x = MX + 30, w = MW - 60;
        float y = MY + 34;
        r.Text(L"ДОБАВИТЬ ПИР", x, y, w, 22, Rgb(th.text), 14); y += 30;
        Caption(x, y, w, L"АДРЕС   (tcp://  tls://  quic://  ws://  wss://)"); y += 18;
        Field(tbApUri, x, y, w); y += 40;
        Caption(x, y, 200, L"ПРОТОКОЛ"); y += 18;
        static const wchar_t* protos[] = { L"tcp", L"tls", L"quic", L"ws", L"wss" };
        util::PeerUri pu = util::ParseUri(util::Trim(tbApUri.text));
        for (int i = 0; i < 5; ++i) Chip(BTN_AP_PROTO + i, x + i * 66, y, 58, protos[i], pu.scheme == protos[i]);
        y += 38;
        Caption(x, y, w / 2, L"ПАРОЛЬ  (если узел его требует)");
        Caption(x + w / 2 + 10, y, w / 2, L"КЛЮЧ ПИРА  (закрепить, необязательно)"); y += 18;
        Field(tbApPass, x, y, w / 2 - 10);
        Field(tbApKey, x + w / 2 + 10, y, w / 2 - 10); y += 42;

        Caption(x, y, w, L"ПРОВЕРКА"); y += 20;
        static const wchar_t* names[] = { L"DNS", L"порт", L"рукопожатие" };
        for (int i = 0; i < 3; ++i) {
            const Check& c = apCheck[i];
            unsigned col = c.state == 2 ? COL_OK : c.state == 3 ? COL_ALERT : c.state == 1 ? COL_WARN : 0x4E5A64;
            r.Disc(x + 6, y + 10, 4, Rgb(col));
            r.Text(names[i], x + 20, y, 120, 20, Rgb(th.text), 12);
            r.Text(c.state == 0 ? L"—" : c.text, x + 160, y, w - 260, 20, Rgb(c.state == 3 ? COL_ALERT : th.muted), 11);
            if (c.ms >= 0) { wchar_t b[32]; swprintf(b, 32, L"%.0f мс", c.ms); r.Text(b, x + w - 100, y, 100, 20, Rgb(COL_OK), 11, false, Right); }
            y += 24;
        }
        y += 10;
        Caption(x, y, w, L"КУДА"); y += 18;
        static const wchar_t* where[] = { L"сейчас и в конфиг", L"только сейчас", L"только в конфиг" };
        for (int i = 0; i < 3; ++i) Chip(BTN_AP_WHERE + i, x + i * 186, y, 176, where[i], apWhere == i);
        y += 32;
        r.Text(L"Итоговый адрес: " + (ApUri().empty() ? std::wstring(L"—") : ApUri()), x, y, w, 18, Rgb(th.muted), 10);

        float by = MY + MH - 54;
        bool can = pu.ok && !apRunning;
        Btn(BTN_AP_ADD, x, by, 200, 38, L"ДОБАВИТЬ", COL_OK, true, can);
        Btn(BTN_AP_CHECK, x + 212, by, 180, 38, L"ПРОВЕРИТЬ", th.accent, false, can);
        Btn(BTN_AP_CANCEL, x + 404, by, 160, 38, L"НАЗАД", th.accent);
        r.Text(L"Enter - добавить   Esc - назад", x + 580, by, w - 580, 38, Rgb(th.muted), 10, false, Right);
    }

    // Проверка: DNS и порт; withAdd - потом добавить и дождаться рукопожатия
    void RunAddPeer(bool withAdd) {
        std::wstring uri = ApUri();
        util::PeerUri pu = util::ParseUri(uri);
        if (!pu.ok) { Flash(L"Адрес пира должен быть вида tls://host:port", COL_ALERT); return; }
        for (auto& c : apCheck) c = Check();
        apCheck[0].state = 1; apCheck[0].text = L"ищу…";
        apRunning = true;
        Redraw();
        int where = apWhere;
        ygg::Paths p = snap.paths;
        bool ipLiteral = pu.host.find(L':') != std::wstring::npos ||
                         (!pu.host.empty() && iswdigit(pu.host[0]) && pu.host.find_first_not_of(L"0123456789.") == std::wstring::npos);
        Async([pu, ipLiteral] {
            std::wstring ip = ipLiteral ? pu.host : util::Resolve(pu.host);
            return ip;
        }, [this, uri, pu, withAdd, where, p](const std::wstring& ip) {
            if (ip.empty()) { apCheck[0] = { 3, L"имя не находится - проверь адрес или DNS", -1 }; apRunning = false; Redraw(); return; }
            apCheck[0] = { 2, L"найден " + ip, -1 };
            bool tcp = pu.scheme == L"tcp" || pu.scheme == L"tls" || pu.scheme == L"ws" || pu.scheme == L"wss";
            if (!tcp || pu.port.empty()) {
                apCheck[1] = { 4, pu.scheme == L"quic" ? L"QUIC работает по UDP - заранее не проверить" : L"не проверяется для " + pu.scheme, -1 };
                AfterPortCheck(uri, withAdd, where, p);
                return;
            }
            apCheck[1] = { 1, L"подключаюсь…", -1 };
            Redraw();
            Async([pu] { wchar_t b[32]; swprintf(b, 32, L"%.0f", util::TcpConnect(pu.host, pu.port, 5000)); return std::wstring(b); },
                  [this, uri, pu, withAdd, where, p](const std::wstring& res) {
                double ms = _wtof(res.c_str());
                if (ms < 0) apCheck[1] = { 3, L"порт " + pu.port + L" закрыт или недоступен", -1 };
                else apCheck[1] = { 2, L"порт " + pu.port + L" открыт", ms };
                AfterPortCheck(uri, withAdd, where, p);
            });
        });
    }

    void AfterPortCheck(const std::wstring& uri, bool withAdd, int where, const ygg::Paths& p) {
        if (!withAdd) { apRunning = false; apCheck[2] = { 4, L"будет проверено при добавлении", -1 }; Redraw(); return; }
        if (where == 2) {            // только в конфиг - живьём не подключаем
            apCheck[2] = { 4, L"только в конфиг - подключится после перезапуска службы", -1 };
            apRunning = false;
            AddPeersToConfig({ uri }, L"Пир записан в конфиг. Подключится после перезапуска службы.");
            return;
        }
        apCheck[2] = { 1, L"addPeer, жду рукопожатия (до 20 с)…", -1 };
        Redraw();
        Async([p, uri] {
            json::Value v; std::wstring err;
            if (!ygg::Ctl(p, L"addPeer uri=" + uri, v, err)) return L"!" + err;
            for (int i = 0; i < 20; ++i) {
                Sleep(1000);
                if (ygg::Ctl(p, L"getPeers", v, err)) {
                    const auto& arr = v[L"peers"];
                    for (size_t k = 0; k < arr.size(); ++k)
                        if (arr[k][L"remote"].str() == uri && arr[k][L"up"].boolean()) {
                            wchar_t b[32]; swprintf(b, 32, L"%.0f", arr[k][L"latency"].num() / 1e6);
                            return std::wstring(b);
                        }
                }
            }
            return std::wstring(L"!пир добавлен, но за 20 с не подключился (см. ошибку в списке пиров)");
        }, [this, uri, where](const std::wstring& res) {
            apRunning = false;
            if (!res.empty() && res[0] == L'!') apCheck[2] = { 3, res.substr(1), -1 };
            else apCheck[2] = { 2, L"ключ принят, связь есть", _wtof(res.c_str()) };
            data.RefreshNow();
            if (where == 0) AddPeersToConfig({ uri }, L"Пир подключён и записан в конфиг.");
            else Flash(apCheck[2].state == 2 ? L"Пир подключён (до перезапуска службы)." : L"Пир добавлен, но связи пока нет.",
                       apCheck[2].state == 2 ? 0 : COL_WARN);
            Redraw();
        });
    }

    // ================== 03 ДЕРЕВО МАРШРУТОВ ==================
    std::vector<TreeNode> tnodes;
    long long tLayoutFor = -1;
    int tSelf = -1, tRoot = -1, tSel = -1, tMaxDepth = 0;
    float tLeaves = 1;
    float tZoom = 1, tPanX = 0, tPanY = 0;
    bool tShowPath = true;
    bool tDragging = false, tDragMoved = false;
    D2D1_POINT_2F tDragStart = { 0, 0 };
    float tPanStartX = 0, tPanStartY = 0;
    const float TCX = MX, TCY = MY, TCW = 580, TCH = MH;   // холст дерева

    void BuildTree() {
        if (tLayoutFor == snap.treeAt) return;
        tLayoutFor = snap.treeAt;
        std::wstring selKey = (tSel >= 0 && tSel < (int)tnodes.size()) ? tnodes[tSel].key : L"";
        tnodes.clear(); tSelf = tRoot = tSel = -1; tMaxDepth = 0;
        std::map<std::wstring, int> idx;
        for (size_t i = 0; i < snap.tree.size(); ++i) {
            TreeNode n;
            n.key = snap.tree[i][L"key"].str();
            n.address = snap.tree[i][L"address"].str();
            n.parentKey = snap.tree[i][L"parent"].str();
            if (n.key.empty() || idx.count(n.key)) continue;
            idx[n.key] = (int)tnodes.size();
            tnodes.push_back(n);
        }
        if (tnodes.empty()) return;
        // соседство (родитель-ребёнок в обе стороны)
        std::vector<std::vector<int>> adj(tnodes.size());
        for (size_t i = 0; i < tnodes.size(); ++i) {
            if (tnodes[i].parentKey == tnodes[i].key) tRoot = (int)i;
            auto it = idx.find(tnodes[i].parentKey);
            if (it != idx.end() && it->second != (int)i) { adj[i].push_back(it->second); adj[it->second].push_back((int)i); }
        }
        auto si = idx.find(SelfKey());
        tSelf = si != idx.end() ? si->second : (tRoot >= 0 ? tRoot : 0);
        // обход в ширину от себя
        std::vector<int> order;
        std::vector<bool> seen(tnodes.size(), false);
        order.push_back(tSelf); seen[tSelf] = true;
        for (size_t h = 0; h < order.size(); ++h) {
            int u = order[h];
            for (int v : adj[u]) if (!seen[v]) {
                seen[v] = true; tnodes[v].bfsParent = u; tnodes[v].depth = tnodes[u].depth + 1;
                tMaxDepth = (std::max)(tMaxDepth, tnodes[v].depth);
                tnodes[u].kids.push_back(v); order.push_back(v);
            }
        }
        // узлы, до которых от нас не дойти (обрывки) - отдельной веткой на глубине 1
        for (size_t i = 0; i < tnodes.size(); ++i) if (!seen[i]) {
            seen[i] = true; tnodes[i].bfsParent = tSelf; tnodes[i].depth = 1; tnodes[tSelf].kids.push_back((int)i); order.push_back((int)i);
            tMaxDepth = (std::max)(tMaxDepth, 1);
        }
        // раскладка: листья подряд, родитель - посередине детей
        float leaf = 0;
        std::function<void(int)> place = [&](int u) {
            if (tnodes[u].kids.empty()) { tnodes[u].wx = leaf; leaf += 1; return; }
            for (int k : tnodes[u].kids) place(k);
            float a = tnodes[tnodes[u].kids.front()].wx, b = tnodes[tnodes[u].kids.back()].wx;
            tnodes[u].wx = (a + b) / 2;
        };
        place(tSelf);
        tLeaves = (std::max)(1.0f, leaf);
        for (size_t i = 0; i < tnodes.size(); ++i) tnodes[i].rootDepth = -1;
        for (size_t i = 0; i < tnodes.size(); ++i) {
            int d = 0, k = (int)i;
            while (d < 300) {
                auto it = idx.find(tnodes[k].parentKey);
                if (tnodes[k].parentKey == tnodes[k].key) { tnodes[i].rootDepth = d; break; }
                if (it == idx.end()) break;
                k = it->second; ++d;
            }
        }
        if (!selKey.empty()) { auto it = idx.find(selKey); if (it != idx.end()) tSel = it->second; }
    }

    // мировые координаты -> экран
    float TBaseSpacing() const { return (TCW - 60) / (std::max)(1.0f, tLeaves); }
    float TLevel() const { return (TCH - 130) / (float)(std::max)(1, tMaxDepth); }
    D2D1_POINT_2F TPos(const TreeNode& n) const {
        float sp = TBaseSpacing() * tZoom, lv = TLevel() * tZoom;
        float cx = TCX + TCW / 2 + tPanX, by = TCY + TCH - 50 + tPanY;
        return D2D1::Point2F(cx + (n.wx - (tLeaves - 1) / 2) * sp, by - n.depth * lv);
    }

    std::wstring TNodeName(int i) const {
        if (i == tSelf) return L"SELF";
        std::wstring bn = BookName(tnodes[i].address);
        if (!bn.empty()) return bn;
        std::wstring pn = PeerNameByKey(tnodes[i].key);
        if (!pn.empty()) return pn;
        if (i == tRoot) return L"КОРЕНЬ";
        return L"";
    }

    void DrawTree() {
        if (NoCtl(L"ДЕРЕВО МАРШРУТОВ  //  getTree")) return;
        const Theme th = Th();
        BuildTree();
        r.Panel(TCX, TCY, TCW, TCH, th, L"ДЕРЕВО МАРШРУТОВ  //  getTree");
        // масштаб
        float zy = TCY + 32;
        Btn(BTN_TREE_ZOUT, TCX + 16, zy, 30, 26, L"−", th.accent);
        float sx = TCX + 54, sw = 110;
        r.Line(sx, zy + 13, sx + sw, zy + 13, Rgb(th.accent, 0.4f), 2);
        float frac = (float)((log(tZoom) / log(2.0) + 2) / 6);
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        r.Line(sx, zy + 13, sx + sw * frac, zy + 13, Rgb(th.accent), 2);
        r.Disc(sx + sw * frac, zy + 13, 5, Rgb(th.accent));
        wchar_t zb[16]; swprintf(zb, 16, L"%.0f%%", tZoom * 100);
        Btn(BTN_TREE_ZIN, sx + sw + 8, zy, 30, 26, L"+", th.accent);
        Btn(BTN_TREE_FIT, sx + sw + 46, zy, 96, 26, L"ВПИСАТЬ", th.accent);
        r.Text(zb, sx + sw + 152, zy, 60, 26, Rgb(th.muted), 10);

        if (tnodes.empty()) {
            r.Text(L"дерево пусто - видимо, нет ни одного пира", TCX, TCY, TCW, TCH, Rgb(th.muted), 12, false, Center);
        } else {
            r.Clip(TCX + 2, TCY + 64, TCW - 4, TCH - 90);
            // уровни
            for (int d = 1; d <= tMaxDepth; ++d) {
                float ly = TCY + TCH - 50 + tPanY - d * TLevel() * tZoom;
                if (ly < TCY + 64 || ly > TCY + TCH) continue;
                r.Text(L"глубина " + std::to_wstring(d), TCX + 14, ly - 8, 90, 14, Rgb(th.muted, 0.6f), 9);
            }
            // путь до выбранного
            std::set<int> path;
            if (tShowPath && tSel >= 0) for (int k = tSel; k >= 0; k = tnodes[k].bfsParent) path.insert(k);
            // рёбра
            for (size_t i = 0; i < tnodes.size(); ++i) {
                int p = tnodes[i].bfsParent;
                if (p < 0) continue;
                D2D1_POINT_2F a = TPos(tnodes[i]), b = TPos(tnodes[p]);
                bool hl = path.count((int)i) && path.count(p);
                r.Line(a.x, a.y, b.x, b.y, Rgb(hl ? COL_OK : th.accent, hl ? 0.95f : 0.35f), hl ? 2.5f : 1.0f);
            }
            // узлы
            for (size_t i = 0; i < tnodes.size(); ++i) {
                D2D1_POINT_2F a = TPos(tnodes[i]);
                if (a.x < TCX - 10 || a.x > TCX + TCW + 10 || a.y < TCY + 50 || a.y > TCY + TCH + 10) continue;
                bool self = (int)i == tSelf, root = (int)i == tRoot, isPeer = !PeerNameByKey(tnodes[i].key).empty();
                float rad = self ? 7.0f : (isPeer ? 5.0f : (tnodes[i].kids.empty() ? 2.5f : 3.5f));
                unsigned col = root ? COL_WARN : (isPeer ? th.text : th.accent);
                if (self) r.GlowCircle(a.x, a.y, rad + 2, th.accent);
                r.Disc(a.x, a.y, rad, Rgb(col, self ? 1.0f : 0.9f));
                if (root) r.Circle(a.x, a.y, rad + 4, Rgb(COL_WARN, 0.8f), 1.5f);
                if ((int)i == tSel) r.Circle(a.x, a.y, rad + 6, Rgb(COL_OK), 2.0f);
                std::wstring nm = TNodeName((int)i);
                if (!nm.empty()) r.Text(nm, a.x + rad + 5, a.y - 8, 140, 16, Rgb(root ? COL_WARN : th.text, 0.9f), 10);
            }
            r.Unclip();
        }
        r.Text(std::to_wstring(tnodes.size()) + L" узлов  ·  колесо - масштаб  ·  перетащить - двигать  ·  клик - узел",
               TCX + 16, TCY + TCH - 26, TCW - 32, 16, Rgb(th.muted), 9);

        // сведения об узле
        float px = TCX + TCW + 12, pw = MX + MW - px;
        r.Panel(px, TCY, pw, TCH, th, L"УЗЕЛ");
        float x = px + 14, w = pw - 28, y = TCY + 36;
        if (tSel < 0 || tSel >= (int)tnodes.size()) {
            Note(x, y, w, L"Нажми на узел, чтобы увидеть его адрес, глубину и путь до него.\n\n"
                          L"Зелёная линия - путь от тебя.\nЯнтарный - корень сети.\nКрупные точки - твои пиры.");
            return;
        }
        const TreeNode& n = tnodes[tSel];
        auto kv = [&](const std::wstring& k, const std::wstring& v, unsigned c = 0) {
            r.Text(k, x, y, w, 14, Rgb(th.muted), 10); y += 15;
            y += r.TextWrapped(v, x, y, w, Rgb(c ? c : th.text), 11) + 8;
        };
        std::wstring nm = TNodeName(tSel);
        kv(L"имя", nm.empty() ? L"—" : nm);
        kv(L"адрес", n.address);
        kv(L"ключ", util::ShortKey(n.key));
        kv(L"от тебя", std::to_wstring(n.depth) + L" шаг(ов)");
        kv(L"до корня", n.rootDepth >= 0 ? std::to_wstring(n.rootDepth) : L"—");
        kv(L"потомков", std::to_wstring(n.kids.size()));
        std::wstring pt = PingText(n.address);
        if (!pt.empty()) kv(L"пинг", pt, th.accent);
        float by = TCY + TCH - 5 * 40 - 10;
        Btn(BTN_TREE_PATH, x, by, w, 32, tShowPath ? L"ПУТЬ: ПОКАЗАН" : L"ПУТЬ ДО УЗЛА", th.accent, tShowPath); by += 40;
        Btn(BTN_TREE_PING, x, by, w, 32, L"ПИНГ", th.accent, false, tSel != tSelf); by += 40;
        Btn(BTN_TREE_ASK, x, by, w, 32, L"ЗАПРОСИТЬ УЗЕЛ", th.accent, false, tSel != tSelf); by += 40;
        Btn(BTN_TREE_COPY, x, by, w, 32, L"КОПИРОВАТЬ АДРЕС", th.accent); by += 40;
        Btn(BTN_TREE_BOOK, x, by, w, 32, L"В УСТРОЙСТВА", th.accent, false, tSel != tSelf);
    }

    void TreeZoom(float factor, D2D1_POINT_2F at) {
        float nz = tZoom * factor;
        if (nz < 0.25f) nz = 0.25f; if (nz > 16) nz = 16;
        factor = nz / tZoom;
        // точка под курсором остаётся на месте
        float cx = TCX + TCW / 2 + tPanX, by = TCY + TCH - 50 + tPanY;
        tPanX += (at.x - cx) * (1 - factor);
        tPanY += (at.y - by) * (1 - factor);
        tZoom = nz;
        Redraw();
    }

    int TreeHit(D2D1_POINT_2F p) const {
        int best = -1; float bd = 12 * 12;
        for (size_t i = 0; i < tnodes.size(); ++i) {
            D2D1_POINT_2F a = TPos(tnodes[i]);
            float d = (a.x - p.x) * (a.x - p.x) + (a.y - p.y) * (a.y - p.y);
            if (d < bd) { bd = d; best = (int)i; }
        }
        return best;
    }

    // ================== 04 СЕССИИ ==================
    void DrawSessions() {
        if (NoCtl(L"СЕССИИ  //  getSessions")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"СЕССИИ  //  getSessions");
        float x = MX + 16, hy = MY + 34;
        ColHead(x + 8, hy, 260, L"АДРЕС");
        ColHead(x + 270, hy, 110, L"КЛЮЧ");
        ColHead(x + 390, hy, 90, L"↓ RX");
        ColHead(x + 480, hy, 90, L"↑ TX");
        ColHead(x + 570, hy, 80, L"ДЛИТ.");
        ColHead(x + 650, hy, 110, L"ТРАФИК");
        r.Line(x, hy + 18, x + MW - 32, hy + 18, Rgb(th.accent, 0.2f));
        gSessions.x = x; gSessions.y = hy + 22; gSessions.w = MW - 32; gSessions.h = 360; gSessions.rowH = 30;
        gSessions.count = (int)snap.sessions.size();
        gSessions.Clamp();
        r.Clip(gSessions.x, gSessions.y, gSessions.w, gSessions.h);
        for (int i = 0; i < gSessions.Visible() + 1 && gSessions.scroll + i < gSessions.count; ++i) {
            int idx = gSessions.scroll + i;
            const auto& s = snap.sessions[idx];
            float ry = gSessions.y + i * gSessions.rowH;
            if (idx == gSessions.sel) { r.Fill(x, ry, gSessions.w, gSessions.rowH - 2, Rgb(th.accent, 0.13f)); r.Fill(x, ry, 3, gSessions.rowH - 2, Rgb(th.accent)); }
            AddrCell(s[L"address"].str(), x + 8, ry, 258, gSessions.rowH);
            r.Text(util::ShortKey(s[L"key"].str()), x + 270, ry, 110, gSessions.rowH, Rgb(th.muted), 11);
            r.Text(L"↓ " + util::Bytes(s[L"bytes_recvd"].num()), x + 390, ry, 90, gSessions.rowH, Rgb(th.accent), 11);
            r.Text(L"↑ " + util::Bytes(s[L"bytes_sent"].num()), x + 480, ry, 90, gSessions.rowH, Rgb(th.muted), 11);
            r.Text(util::Duration(s[L"uptime"].num()), x + 570, ry, 80, gSessions.rowH, Rgb(th.text), 11);
            auto it = snap.sessionRate.find(s[L"key"].str());
            if (it != snap.sessionRate.end() && it->second.size() > 1) {
                float mx = 1; for (float v : it->second) mx = (std::max)(mx, v);
                float gx = x + 650, gw = 110, gy = ry + 4, gh = gSessions.rowH - 10;
                size_t n = it->second.size();
                for (size_t k = 1; k < n; ++k) {
                    float x1 = gx + gw * (k - 1) / (n - 1), x2 = gx + gw * k / (n - 1);
                    r.Line(x1, gy + gh - gh * it->second[k - 1] / mx, x2, gy + gh - gh * it->second[k] / mx, Rgb(th.accent, 0.9f), 1.2f);
                }
            }
        }
        r.Unclip();
        if (snap.sessions.size() == 0) r.Text(L"активных сессий нет - сессия появляется, когда ты общаешься с другим узлом", x, gSessions.y, gSessions.w, 30, Rgb(th.muted), 12);
        float dy = gSessions.y + gSessions.h + 10;
        bool sel = gSessions.sel >= 0 && gSessions.sel < gSessions.count;
        if (sel) {
            const auto& s = snap.sessions[gSessions.sel];
            r.Text(s[L"address"].str() + L"   " + s[L"key"].str(), x, dy, MW - 32, 18, Rgb(th.text), 10);
            std::wstring pt = PingText(s[L"address"].str());
            if (!pt.empty()) r.Text(pt, x, dy + 20, MW - 32, 18, Rgb(th.accent), 11);
        } else r.Text(std::to_wstring(snap.sessions.size()) + L" активных сессий  ·  трафик в строке - за последние 3 минуты",
                      x, dy, MW - 32, 18, Rgb(th.muted), 11);
        float by = MY + MH - 50;
        Btn(BTN_SES_PING, x, by, 170, 34, L"ПИНГ", th.accent, false, sel);
        Btn(BTN_SES_ASK, x + 182, by, 210, 34, L"ЗАПРОСИТЬ УЗЕЛ", th.accent, false, sel);
        Btn(BTN_SES_COPY, x + 404, by, 210, 34, L"КОПИРОВАТЬ АДРЕС", th.accent, false, sel);
        Btn(BTN_SES_BOOK, x + 626, by, 134, 34, L"В УСТРОЙСТВА", th.accent, false, sel);
    }

    // ================== 05 ТАБЛИЦА ПУТЕЙ ==================
    int pathSort = 0;    // 0 адрес, 1 длина пути, 2 через, 3 seq
    std::vector<int> pathOrder;

    void DrawPaths() {
        if (NoCtl(L"ТАБЛИЦА ПУТЕЙ  //  getPaths")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"ТАБЛИЦА ПУТЕЙ  //  getPaths");
        float x = MX + 16, hy = MY + 34;
        static const wchar_t* heads[] = { L"АДРЕС", L"ПУТЬ ПО ПОРТАМ", L"ЧЕРЕЗ", L"SEQ" };
        float cols[] = { 8, 280, 520, 680 };
        for (int i = 0; i < 4; ++i) {
            std::wstring h = std::wstring(heads[i]) + (pathSort == i ? L" ↓" : L"");
            bool hov = hoverId == BTN_PATH_SORT + i;
            r.Text(h, x + cols[i], hy, 200, 16, Rgb(pathSort == i ? th.accent : th.muted, hov ? 1.0f : 0.9f), 10);
            if (!confirmOpen) hits.push_back({ x + cols[i], hy, r.TextWidth(h, 10) + 6, 16, BTN_PATH_SORT + i });
        }
        r.Line(x, hy + 18, x + MW - 32, hy + 18, Rgb(th.accent, 0.2f));
        // порядок
        const auto& P = snap.paths_;
        pathOrder.resize(P.size());
        for (size_t i = 0; i < P.size(); ++i) pathOrder[i] = (int)i;
        auto via = [&](int i) { const auto& pp = P[i][L"path"]; return pp.size() ? PeerNameByPort(pp[0].i64()) : std::wstring(L"—"); };
        std::stable_sort(pathOrder.begin(), pathOrder.end(), [&](int a, int b) {
            switch (pathSort) {
                case 1: return P[a][L"path"].size() < P[b][L"path"].size();
                case 2: return via(a) < via(b);
                case 3: return P[a][L"sequence"].num() > P[b][L"sequence"].num();
                default: return P[a][L"address"].str() < P[b][L"address"].str();
            }
        });
        gPaths.x = x; gPaths.y = hy + 22; gPaths.w = MW - 32; gPaths.h = 380; gPaths.rowH = 28;
        gPaths.count = (int)P.size();
        gPaths.Clamp();
        r.Clip(gPaths.x, gPaths.y, gPaths.w, gPaths.h);
        for (int i = 0; i < gPaths.Visible() + 1 && gPaths.scroll + i < gPaths.count; ++i) {
            int row = gPaths.scroll + i, idx = pathOrder[row];
            const auto& e = P[idx];
            float ry = gPaths.y + i * gPaths.rowH;
            if (row == gPaths.sel) { r.Fill(x, ry, gPaths.w, gPaths.rowH - 2, Rgb(th.accent, 0.13f)); r.Fill(x, ry, 3, gPaths.rowH - 2, Rgb(th.accent)); }
            std::wstring ports;
            const auto& pp = e[L"path"];
            for (size_t k = 0; k < pp.size(); ++k) ports += (k ? L" " : L"") + std::to_wstring(pp[k].i64());
            AddrCell(e[L"address"].str(), x + 8, ry, 268, gPaths.rowH);
            r.Text(ports.empty() ? L"—" : ports, x + 280, ry, 230, gPaths.rowH, Rgb(th.muted), 11);
            r.Text(via(idx), x + 520, ry, 150, gPaths.rowH, Rgb(th.accent), 11);
            r.Text(e[L"sequence"].isNull() ? L"—" : json::Parser::FromUtf8(json::NumberText(e[L"sequence"].num())), x + 680, ry, 90, gPaths.rowH, Rgb(th.muted), 11);
        }
        r.Unclip();
        if (P.size() == 0) r.Text(L"путей нет", x, gPaths.y, gPaths.w, 30, Rgb(th.muted), 12);
        float dy = gPaths.y + gPaths.h + 8;
        static const wchar_t* sortName[] = { L"адрес", L"длина пути", L"через", L"SEQ" };
        r.Text(std::to_wstring(P.size()) + L" путей  ·  сортировка: " + sortName[pathSort] + L"  ·  клик по заголовку - сменить",
               x, dy, MW - 32, 18, Rgb(th.muted), 10);
        bool sel = gPaths.sel >= 0 && gPaths.sel < gPaths.count;
        if (sel) { std::wstring pt = PingText(P[pathOrder[gPaths.sel]][L"address"].str()); if (!pt.empty()) r.Text(pt, x, dy, MW - 32, 18, Rgb(th.accent), 11, false, Right); }
        float by = MY + MH - 50;
        Btn(BTN_PATH_PING, x, by, 170, 34, L"ПИНГ", th.accent, false, sel);
        Btn(BTN_PATH_ASK, x + 182, by, 210, 34, L"ЗАПРОСИТЬ УЗЕЛ", th.accent, false, sel);
        Btn(BTN_PATH_COPY, x + 404, by, 210, 34, L"КОПИРОВАТЬ АДРЕС", th.accent, false, sel);
        Btn(BTN_PATH_BOOK, x + 626, by, 134, 34, L"В УСТРОЙСТВА", th.accent, false, sel);
    }

    const json::Value* SelectedPath() const {
        if (gPaths.sel < 0 || gPaths.sel >= (int)pathOrder.size()) return nullptr;
        return &snap.paths_[pathOrder[gPaths.sel]];
    }

    // ================== УСТРОЙСТВА (адресная книга) ==================
    // Свои устройства: имя и адрес Yggdrasil.
    // Хранится в addressbook.txt рядом с панелью: имя<TAB>адрес
    // (третий столбец из прежних версий читается и игнорируется).
    // Устройства, которые пингует watchdog, показываются тоже (пометка «watchdog»).
    struct BookEntry { std::wstring name, addr; bool fromWd = false; };
    std::vector<BookEntry> book;         // свои записи
    std::vector<BookEntry> bookView;     // свои + из watchdog (для экрана)
    bool bookLoaded = false;
    int bookSelPrev = -2;

    std::wstring BookPath() const { return util::DirOf(util::SelfExe()) + L"addressbook.txt"; }

    void LoadBook() {
        bookLoaded = true;
        book.clear();
        std::wstringstream ss(util::FromUtf8(util::ReadFileBytes(BookPath())));
        std::wstring ln;
        while (std::getline(ss, ln)) {
            if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
            if (ln.empty() || ln[0] == L'#') continue;
            BookEntry e;
            size_t t1 = ln.find(L'\t');
            e.name = ln.substr(0, t1);
            if (t1 != std::wstring::npos) {
                size_t t2 = ln.find(L'\t', t1 + 1);
                e.addr = ln.substr(t1 + 1, t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1);
            }
            e.name = util::Trim(e.name); e.addr = util::Trim(e.addr);
            if (!e.name.empty() && !e.addr.empty()) book.push_back(e);
        }
    }
    bool SaveBook() {
        std::wstring out = L"# адресная книга ygg-control: имя<TAB>адрес\r\n";
        for (const auto& e : book) out += e.name + L"\t" + e.addr + L"\r\n";
        return util::WriteFileBytes(BookPath(), util::ToUtf8(out));
    }
    static std::wstring NormAddr(const std::wstring& a) {
        in6_addr x;
        if (InetPtonW(AF_INET6, a.c_str(), &x) == 1) {
            wchar_t b[64]; InetNtopW(AF_INET6, &x, b, 64); return b;
        }
        return util::Lower(a);
    }
    // Имя по адресу: своя книга, потом устройства watchdog
    std::wstring BookName(const std::wstring& addr) const {
        if (addr.empty()) return L"";
        std::wstring n = NormAddr(addr);
        for (const auto& e : book) if (NormAddr(e.addr) == n) return e.name;
        const auto& d = snap.wdStatus[L"devices"];
        for (size_t i = 0; i < d.size(); ++i) if (NormAddr(d[i][L"host"].str()) == n) return d[i][L"name"].str();
        return L"";
    }
    void RebuildBookView() {
        if (!bookLoaded) LoadBook();
        bookView = book;
        const auto& d = snap.wdStatus[L"devices"];
        for (size_t i = 0; i < d.size(); ++i) {
            std::wstring h = d[i][L"host"].str();
            bool dup = false;
            for (const auto& e : book) if (NormAddr(e.addr) == NormAddr(h)) dup = true;
            if (!dup) { BookEntry e; e.name = d[i][L"name"].str(); e.addr = h; e.fromWd = true; bookView.push_back(e); }
        }
    }
    // Где этот адрес сейчас виден: сессия / дерево / путь
    std::wstring SeenWhere(const std::wstring& addr) const {
        std::wstring n = NormAddr(addr), r;
        for (size_t i = 0; i < snap.sessions.size(); ++i) if (NormAddr(snap.sessions[i][L"address"].str()) == n) { r += L"сессия "; break; }
        for (size_t i = 0; i < snap.tree.size(); ++i) if (NormAddr(snap.tree[i][L"address"].str()) == n) { r += L"дерево "; break; }
        for (size_t i = 0; i < snap.paths_.size(); ++i) if (NormAddr(snap.paths_[i][L"address"].str()) == n) { r += L"путь"; break; }
        return r;
    }
    // Ячейка "адрес" в таблицах: имя из книги + адрес
    void AddrCell(const std::wstring& addr, float x, float y, float w, float h) {
        const Theme th = Th();
        std::wstring nm = BookName(addr);
        r.Clip(x, y, w, h);
        if (nm.empty()) r.Text(addr, x, y, 2000, h, Rgb(th.text), 11);
        else {
            r.Text(nm, x, y, 2000, h, Rgb(th.accent), 11, true);
            float nw = r.TextWidth(nm, 11, true) + 10;
            r.Text(addr, x + nw, y, 2000, h, Rgb(th.muted), 10);
        }
        r.Unclip();
    }
    // Перейти в книгу с заполненным адресом (из сессий, путей, дерева)
    void ToBook(const std::wstring& addr) {
        if (addr.empty()) return;
        GoTo(SCR_BOOK);
        RebuildBookView();
        gBook.sel = -1; bookSelPrev = -1;
        for (size_t i = 0; i < bookView.size(); ++i) if (NormAddr(bookView[i].addr) == NormAddr(addr)) gBook.sel = (int)i;
        if (gBook.sel < 0) { tbBkName.Set(L""); tbBkAddr.Set(addr); SetTb(&tbBkName); navFocus = false; }
        Redraw();
    }

    Grid gBook;
    TextBox tbBkName, tbBkAddr;

    const BookEntry* BookSel() const {
        return (gBook.sel >= 0 && gBook.sel < (int)bookView.size()) ? &bookView[gBook.sel] : nullptr;
    }

    void DrawBook() {
        const Theme th = Th();
        RebuildBookView();
        r.Panel(MX, MY, MW, MH, th, L"УСТРОЙСТВА  //  адресная книга");
        float x = MX + 16, hy = MY + 34;
        ColHead(x + 8, hy, 170, L"ИМЯ");
        ColHead(x + 180, hy, 300, L"АДРЕС");
        ColHead(x + 490, hy, 90, L"ПИНГ");
        ColHead(x + 590, hy, 170, L"СЕЙЧАС ВИДНО");
        r.Line(x, hy + 18, x + MW - 32, hy + 18, Rgb(th.accent, 0.2f));
        gBook.x = x; gBook.y = hy + 22; gBook.w = MW - 32; gBook.h = 224; gBook.rowH = 28;
        gBook.count = (int)bookView.size();
        gBook.Clamp();
        if (gBook.sel != bookSelPrev) {      // выбрали строку - заполняем поля
            bookSelPrev = gBook.sel;
            if (const BookEntry* e = BookSel()) { tbBkName.Set(e->name); tbBkAddr.Set(e->addr); }
        }
        r.Clip(gBook.x, gBook.y, gBook.w, gBook.h);
        for (int i = 0; i < gBook.Visible() + 1 && gBook.scroll + i < gBook.count; ++i) {
            int idx = gBook.scroll + i;
            const auto& e = bookView[idx];
            float ry = gBook.y + i * gBook.rowH;
            if (idx == gBook.sel) { r.Fill(x, ry, gBook.w, gBook.rowH - 2, Rgb(th.accent, 0.13f)); r.Fill(x, ry, 3, gBook.rowH - 2, Rgb(th.accent)); }
            r.Text(e.name, x + 8, ry, 170, gBook.rowH, Rgb(th.text), 11, true);
            r.Text(e.addr, x + 180, ry, 300, gBook.rowH, Rgb(th.muted), 11);
            auto it = pingMs.find(e.addr);
            std::wstring pt = L"—"; unsigned pc = th.muted;
            if (it != pingMs.end()) {
                if (it->second == -2) { pt = L"…"; pc = th.accent; }
                else if (it->second < 0) { pt = L"нет ответа"; pc = COL_ALERT; }
                else { wchar_t b[32]; swprintf(b, 32, L"%.0f мс", it->second); pt = b; pc = COL_OK; }
            }
            r.Text(pt, x + 490, ry, 90, gBook.rowH, Rgb(pc), 11);
            std::wstring seen = SeenWhere(e.addr);
            if (e.fromWd) seen = (seen.empty() ? L"" : seen + L" · ") + L"watchdog";
            r.Text(seen.empty() ? L"—" : seen, x + 590, ry, 170, gBook.rowH, Rgb(seen.empty() ? th.muted : th.accent, 0.9f), 10);
        }
        r.Unclip();
        if (bookView.empty())
            r.TextWrapped(L"Записей нет. Впиши имя и адрес Yggdrasil ниже и нажми «Сохранить» - или нажми «В устройства» "
                          L"на экранах сессий, путей и дерева. Имена из книги видны везде в панели.",
                          x, gBook.y + 4, gBook.w, Rgb(th.muted), 12);

        float fy = gBook.y + gBook.h + 12;
        Caption(x, fy, 220, L"ИМЯ");
        Caption(x + 232, fy, 360, L"АДРЕС YGGDRASIL");
        Field(tbBkName, x, fy + 18, 220);
        Field(tbBkAddr, x + 232, fy + 18, 360);

        std::wstring addr = util::Trim(tbBkAddr.text);
        bool hasAddr = !addr.empty();
        const BookEntry* sel = BookSel();
        float by = fy + 62;
        Btn(BTN_BK_SAVE, x, by, 180, 34, L"СОХРАНИТЬ", th.accent, true,
            !util::Trim(tbBkName.text).empty() && hasAddr);
        Btn(BTN_BK_DEL, x + 192, by, 180, 34, L"УДАЛИТЬ", COL_ALERT, false, sel && !sel->fromWd);
        Btn(BTN_BK_PING, x + 384, by, 180, 34, L"ПИНГ", th.accent, false, hasAddr);
        Btn(BTN_BK_PINGALL, x + 576, by, 184, 34, L"ПИНГ ВСЕХ", th.accent, false, !bookView.empty());
        by += 44;
        Btn(BTN_BK_COPY, x, by, 180, 34, L"КОПИРОВАТЬ АДРЕС", th.accent, false, hasAddr);
        by += 44;
        std::wstring pt = hasAddr ? PingText(addr) : L"";
        if (!pt.empty()) r.Text(pt, x, by, MW - 32, 18, Rgb(th.accent), 11);
        else Note(x, by, MW - 32, L"Имя из книги показывается в дереве, сессиях и путях рядом с адресом.");
    }

    void BookAction(int id) {
        std::wstring name = util::Trim(tbBkName.text), addr = util::NormalizeIpv6(util::Trim(tbBkAddr.text));
        switch (id) {
        case BTN_BK_SAVE: {
            if (name.empty() || addr.empty()) break;
            if (name.find(L'\t') != std::wstring::npos || addr.find(L'\t') != std::wstring::npos) break;
            in6_addr tmp;
            if (InetPtonW(AF_INET6, addr.c_str(), &tmp) != 1) { Flash(L"Адрес должен быть IPv6-адресом Yggdrasil (например 200:…)", COL_ALERT); break; }
            const BookEntry* sel = BookSel();
            bool updated = false;
            for (auto& e : book) {
                if ((sel && !sel->fromWd && e.name == sel->name && e.addr == sel->addr) || NormAddr(e.addr) == NormAddr(addr)) {
                    e.name = name; e.addr = addr; updated = true; break;
                }
            }
            if (!updated) { BookEntry e; e.name = name; e.addr = addr; book.push_back(e); }
            if (SaveBook()) Flash(updated ? L"Запись обновлена." : L"Устройство добавлено.");
            else Flash(L"Не удалось записать addressbook.txt", COL_ALERT);
            RebuildBookView();
            for (size_t i = 0; i < bookView.size(); ++i) if (NormAddr(bookView[i].addr) == NormAddr(addr)) { gBook.sel = (int)i; bookSelPrev = gBook.sel; }
            break;
        }
        case BTN_BK_DEL: {
            const BookEntry* sel = BookSel();
            if (!sel || sel->fromWd) break;
            std::wstring n = sel->name, a = sel->addr;
            Confirm(L"Удалить из книги «" + n + L"» (" + a + L")?", L"УДАЛИТЬ", [this, n, a] {
                book.erase(std::remove_if(book.begin(), book.end(), [&](const BookEntry& e) { return e.name == n && e.addr == a; }), book.end());
                SaveBook();
                gBook.sel = -1; bookSelPrev = -1;
                tbBkName.Set(L""); tbBkAddr.Set(L"");
                Flash(L"Удалено.");
            }, COL_ALERT);
            break;
        }
        case BTN_BK_PING: PingAsync(addr, true); break;
        case BTN_BK_PINGALL: for (const auto& e : bookView) PingAsync(e.addr, true); break;
        case BTN_BK_COPY: util::CopyToClipboard(hwnd, addr); Flash(L"Адрес скопирован."); break;
        }
    }

    // ================== ИСТОРИЯ ЗАДЕРЖЕК ==================
    // history.log watchdog: unix<TAB>фаза<TAB>интернет<TAB>uri=U|мс;uri=D|-1;...
    struct HistTick { long long ts = 0; std::wstring phase; std::vector<std::pair<int, double>> v; }; // (индекс пира, мс или -1)
    std::vector<HistTick> hist;
    std::vector<std::wstring> histPeers;
    bool histLoading = false;
    long long histLoadedAt = 0;
    std::wstring histError;
    int histRange = 2;          // 0 - 6 ч, 1 - 24 ч, 2 - 48 ч
    Grid gHist;
    std::set<std::wstring> histHidden;   // пиры, снятые с графика галочкой
    bool HistShown(int pi) const { return pi >= 0 && pi < (int)histPeers.size() && !histHidden.count(histPeers[pi]); }
    void ToggleHist(int pi) {
        if (pi < 0 || pi >= (int)histPeers.size()) return;
        if (histHidden.count(histPeers[pi])) histHidden.erase(histPeers[pi]); else histHidden.insert(histPeers[pi]);
    }

    void LoadHistory() {
        if (histLoading) return;
        if (snap.wdExe.empty()) { histError = L"watchdog не найден - историю пишет он (экран WATCHDOG)."; return; }
        histLoading = true;
        histLoadedAt = Now();
        std::wstring path = util::DirOf(snap.wdExe) + L"history.log";
        auto data = std::make_shared<std::string>();
        Async([path, data] {
            if (!util::FileExists(path)) return std::wstring(L"!нет");
            *data = util::ReadFileTail(path, 2 * 1024 * 1024);
            return std::wstring(L"ok");
        }, [this, data](const std::wstring& res) {
            histLoading = false;
            hist.clear(); histPeers.clear(); histError.clear();
            if (res != L"ok") { histError = L"Истории пока нет: watchdog пишет history.log после каждой проверки (раз в 3 минуты)."; Redraw(); return; }
            std::wstringstream ss(util::FromUtf8(*data));
            std::wstring ln;
            std::map<std::wstring, int> idx;
            // Только текущие пиры: удалённые в историю не попадают (иначе она копилась бы)
            std::set<std::wstring> cur;
            for (size_t i = 0; i < snap.peers.size(); ++i) cur.insert(snap.peers[i][L"remote"].str());
            const auto& cp = cfgOrig[L"Peers"];
            for (size_t i = 0; i < cp.size(); ++i) cur.insert(cp[i].str());
            for (const wchar_t* k : { L"main_peers", L"backup_peers", L"regional_peers" }) {
                const auto& a = snap.wdStatus[k];
                for (size_t i = 0; i < a.size(); ++i) cur.insert(a[i].str());
            }
            while (std::getline(ss, ln)) {
                if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
                std::vector<std::wstring> f;
                size_t p = 0;
                for (int k = 0; k < 3; ++k) { size_t t = ln.find(L'\t', p); if (t == std::wstring::npos) break; f.push_back(ln.substr(p, t - p)); p = t + 1; }
                if (f.size() < 3) continue;
                HistTick t;
                try { t.ts = std::stoll(f[0]); } catch (...) { continue; }
                t.phase = f[1];
                std::wstring rest = ln.substr(p);
                size_t q = 0;
                while (q < rest.size()) {
                    size_t sc = rest.find(L';', q);
                    std::wstring item = rest.substr(q, sc == std::wstring::npos ? std::wstring::npos : sc - q);
                    q = sc == std::wstring::npos ? rest.size() : sc + 1;
                    size_t eq = item.rfind(L'=');
                    if (eq == std::wstring::npos) continue;
                    std::wstring uri = item.substr(0, eq), val = item.substr(eq + 1);
                    if (!cur.empty() && !cur.count(uri)) continue;
                    bool up = val.size() > 1 && val[0] == L'U';
                    double ms = up ? _wtof(val.substr(2).c_str()) : -1;
                    auto it = idx.find(uri);
                    int pi;
                    if (it == idx.end()) { pi = (int)histPeers.size(); idx[uri] = pi; histPeers.push_back(uri); } else pi = it->second;
                    t.v.push_back({ pi, ms });
                }
                hist.push_back(t);
            }
            Redraw();
        });
    }

    unsigned HistColor(int i) const {
        static const unsigned pal[] = { 0x6EC8FF, 0x3DFF9A, 0xFFB23D, 0xFF5FD2, 0xB48CFF, 0x4DE1D2, 0xFF8A5C, 0xE6EEF2 };
        return pal[i % 8];
    }

    void DrawHistory() {
        const Theme th = Th();
        if (!histLoading && (histLoadedAt == 0 || Now() - histLoadedAt > 180)) LoadHistory();
        r.Panel(MX, MY, MW, MH, th, L"ИСТОРИЯ ЗАДЕРЖЕК ПИРОВ  //  history.log watchdog");
        float x = MX + 16, w = MW - 32;
        static const wchar_t* rn[] = { L"6 ч", L"24 ч", L"48 ч" };
        for (int i = 0; i < 3; ++i) Chip(BTN_HIST_RANGE + i, x + i * 74, MY + 32, 66, rn[i], histRange == i);
        Chip(BTN_HIST_ALL, x + 244, MY + 32, 96, L"показать все", false);
        Chip(BTN_HIST_NONE, x + 348, MY + 32, 96, L"скрыть все", false);
        Btn(BTN_HIST_RELOAD, x + w - 150, MY + 30, 150, 28, histLoading ? L"ЧИТАЮ…" : L"ОБНОВИТЬ", th.accent, false, !histLoading);

        float cx = x + 44, cy = MY + 90, cw = w - 44, ch = 218;
        long long now = Now(), span = histRange == 0 ? 6 * 3600 : histRange == 1 ? 24 * 3600 : 48 * 3600, t0 = now - span;
        if (hist.empty()) {
            r.Frame(cx, cy, cw, ch, Rgb(th.accent, 0.2f));
            r.TextWrapped(histError.empty() ? (histLoading ? L"читаю history.log…" : L"нет данных") : histError,
                          cx + 16, cy + ch / 2 - 10, cw - 32, Rgb(th.muted), 12);
            return;
        }
        int selPeer = (gHist.sel >= 0 && gHist.sel < (int)histPeers.size()) ? gHist.sel : -1;
        // ось подстраивается под данные: по максимуму среди видимых линий за период берём
        // «круглый» шаг (1, 2 или 5 × 10^k) так, чтобы делений было не больше пяти
        double mx = 0;
        for (const auto& t : hist) if (t.ts >= t0) for (const auto& pv : t.v) if (pv.second > mx && HistShown(pv.first)) mx = pv.second;
        double vstep = 2, top = 10;
        if (mx > 0) {
            double raw = mx * 1.05 / 5;
            double mag = pow(10.0, floor(log10(raw)));
            double f = raw / mag;
            vstep = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * mag;
            top = ceil(mx * 1.05 / vstep - 1e-9) * vstep;
            if (top < vstep) top = vstep;
        }
        int nTicks = (int)floor(top / vstep + 0.5);
        auto X = [&](long long ts) { return cx + cw * (float)(ts - t0) / (float)span; };
        auto Y = [&](double ms) { return cy + ch - ch * (float)((std::min)(ms, top) / top); };
        // сетка
        for (int i = 0; i <= nTicks; ++i) {
            float gy = cy + ch - ch * i / (float)nTicks;
            r.Line(cx, gy, cx + cw, gy, Rgb(th.accent, i == 0 ? 0.35f : 0.10f));
            wchar_t b[32];
            if (vstep < 1) swprintf(b, 32, L"%.1f", vstep * i); else swprintf(b, 32, L"%.0f", vstep * i);
            r.Text(b, x, gy - 8, 40, 16, Rgb(th.muted), 9, false, Right);
        }
        r.Text(L"мс", x, cy - 22, 40, 14, Rgb(th.muted), 9, false, Right);
        long long step = histRange == 0 ? 3600 : histRange == 1 ? 4 * 3600 : 6 * 3600;
        for (long long ts = (t0 / step + 1) * step; ts < now; ts += step) {
            float gx = X(ts);
            r.Line(gx, cy, gx, cy + ch, Rgb(th.accent, 0.06f));
            r.Text(util::TimeText(ts, L"%H:%M"), gx - 30, cy + ch + 18, 60, 14, Rgb(th.muted), 9, false, Center);
        }
        // полоса фаз
        float sy = cy + ch + 4;
        for (size_t i = 0; i < hist.size(); ++i) {
            const auto& t = hist[i];
            if (t.ts < t0) continue;
            long long nx = (i + 1 < hist.size()) ? (std::min)(hist[i + 1].ts, t.ts + 900) : (std::min)(now, t.ts + 180);
            const std::wstring& p = t.phase;
            unsigned c = (p == L"ok" || p == L"backup" || p == L"regional") ? COL_OK :
                         (p == L"down" || p == L"crisis" || p == L"regional_search") ? COL_WARN :
                         (p == L"no_internet" || p == L"battery") ? COL_ALERT : 0x6B8494;
            r.Fill(X(t.ts), sy, (std::max)(1.0f, X(nx) - X(t.ts)), 8, Rgb(c, 0.75f));
        }
        // линии пиров
        r.Clip(cx, cy - 2, cw, ch + 4);
        for (int pi = 0; pi < (int)histPeers.size(); ++pi) {
            if (!HistShown(pi)) continue;
            bool dim = false;
            unsigned col = HistColor(pi);
            long long pts = 0; double pms = -1;
            for (const auto& t : hist) {
                if (t.ts < t0 - 900) continue;
                double ms = -2;
                for (const auto& pv : t.v) if (pv.first == pi) { ms = pv.second; break; }
                if (ms == -2) { pms = -1; pts = 0; continue; }       // пира в этой проверке не было
                if (ms < 0) {                                          // лежал
                    r.Line(X(t.ts), cy + ch - 4, X(t.ts), cy + ch, Rgb(COL_ALERT, dim ? 0.2f : 0.8f), 2);
                    pms = -1; pts = 0; continue;
                }
                if (pms >= 0 && t.ts - pts <= 900)
                    r.Line(X(pts), Y(pms), X(t.ts), Y(ms), Rgb(col, dim ? 0.15f : 0.95f), selPeer == pi ? 2.2f : 1.4f);
                else r.Disc(X(t.ts), Y(ms), 1.5f, Rgb(col, dim ? 0.15f : 0.95f));
                pms = ms; pts = t.ts;
            }
        }
        r.Unclip();
        // подсказка под мышью
        if (mouse.x >= cx && mouse.x <= cx + cw && mouse.y >= cy && mouse.y <= cy + ch && !confirmOpen) {
            long long mt = t0 + (long long)((mouse.x - cx) / cw * span);
            const HistTick* best = nullptr; long long bd = 1LL << 60;
            for (const auto& t : hist) { long long d = llabs(t.ts - mt); if (d < bd) { bd = d; best = &t; } }
            if (best && bd < 900) {
                float gx = X(best->ts);
                r.Line(gx, cy, gx, cy + ch, Rgb(th.text, 0.4f));
                std::vector<std::pair<std::wstring, unsigned>> lines;
                lines.push_back({ util::TimeText(best->ts, L"%d.%m %H:%M") + L"  ·  " + best->phase, th.text });
                for (const auto& pv : best->v) {
                    if (!HistShown(pv.first)) continue;
                    wchar_t b[32];
                    if (pv.second >= 0) swprintf(b, 32, L"%.0f мс", pv.second); else wcscpy_s(b, L"лежал");
                    lines.push_back({ ShortName(histPeers[pv.first]) + L"  " + b, pv.second >= 0 ? HistColor(pv.first) : COL_ALERT });
                }
                float bw = 230, bh = 8 + 16.0f * lines.size(), bx = gx + 12 + bw > cx + cw ? gx - 12 - bw : gx + 12, by = cy + 8;
                r.Fill(bx, by, bw, bh, Rgb(th.bg, 0.95f));
                r.Frame(bx, by, bw, bh, Rgb(th.accent, 0.5f));
                for (size_t i = 0; i < lines.size(); ++i)
                    r.Text(lines[i].first, bx + 8, by + 4 + 16.0f * i, bw - 16, 16, Rgb(lines[i].second), 10);
            }
        }

        // таблица пиров: аптайм и задержки за выбранный период
        float ty = cy + ch + 42;
        ColHead(x + 28, ty, 300, L"ПИР  (галочка - показывать на графике, пробел - переключить)");
        ColHead(x + 340, ty, 90, L"НА СВЯЗИ");
        ColHead(x + 440, ty, 80, L"СРЕДНЯЯ");
        ColHead(x + 530, ty, 70, L"МИН");
        ColHead(x + 610, ty, 70, L"МАКС");
        ColHead(x + 690, ty, 70, L"СЕЙЧАС");
        gHist.x = x; gHist.y = ty + 20; gHist.w = w; gHist.h = MY + MH - 12 - gHist.y; gHist.rowH = 24;
        gHist.count = (int)histPeers.size();
        gHist.Clamp();
        r.Clip(gHist.x, gHist.y, gHist.w, gHist.h);
        for (int i = 0; i < gHist.Visible() + 1 && gHist.scroll + i < gHist.count; ++i) {
            int pi = gHist.scroll + i;
            float ry = gHist.y + i * gHist.rowH;
            int n = 0, up = 0; double sum = 0, mn = 1e18, mxv = 0, last = -2;
            for (const auto& t : hist) {
                if (t.ts < t0) continue;
                for (const auto& pv : t.v) if (pv.first == pi) {
                    ++n; last = pv.second;
                    if (pv.second >= 0) { ++up; sum += pv.second; mn = (std::min)(mn, pv.second); mxv = (std::max)(mxv, pv.second); }
                }
            }
            if (pi == gHist.sel) { r.Fill(x, ry, gHist.w, gHist.rowH - 2, Rgb(th.accent, 0.13f)); r.Fill(x, ry, 3, gHist.rowH - 2, Rgb(th.accent)); }
            {   // галочка: закрашена цветом линии, пустая - пир скрыт
                bool on = HistShown(pi);
                float bx = x + 8, byy = ry + 6;
                r.Frame(bx, byy, 13, 13, Rgb(HistColor(pi), on ? 1.0f : 0.5f));
                if (on) r.Fill(bx + 3, byy + 3, 7, 7, Rgb(HistColor(pi)));
            }
            r.Clip(x + 28, ry, 304, gHist.rowH);
            r.Text(histPeers[pi], x + 28, ry, 2000, gHist.rowH, Rgb(th.text, HistShown(pi) ? 1.0f : 0.45f), 10);
            r.Unclip();
            auto ms = [](double v) { wchar_t b[32]; swprintf(b, 32, L"%.0f мс", v); return std::wstring(b); };
            if (n == 0) { r.Text(L"нет данных за период", x + 340, ry, 300, gHist.rowH, Rgb(th.muted), 10); continue; }
            int pct = (int)(100.0 * up / n + 0.5);
            r.Text(std::to_wstring(pct) + L"%", x + 340, ry, 90, gHist.rowH, Rgb(pct >= 95 ? COL_OK : pct >= 50 ? COL_WARN : COL_ALERT), 10);
            r.Text(up ? ms(sum / up) : L"—", x + 440, ry, 80, gHist.rowH, Rgb(th.text), 10);
            r.Text(up ? ms(mn) : L"—", x + 530, ry, 70, gHist.rowH, Rgb(th.muted), 10);
            r.Text(up ? ms(mxv) : L"—", x + 610, ry, 70, gHist.rowH, Rgb(th.muted), 10);
            r.Text(last >= 0 ? ms(last) : L"лежит", x + 690, ry, 70, gHist.rowH, Rgb(last >= 0 ? COL_OK : COL_ALERT), 10);
        }
        r.Unclip();
    }

    // ================== 06 УДАЛЁННЫЙ УЗЕЛ ==================
    std::wstring remoteTitle;

    void AskRemote(const std::wstring& key) {
        tbRemoteKey.Set(key);
        GoTo(SCR_REMOTE);
    }

    void RunRemote(const std::wstring& cmd, const std::wstring& title) {
        std::wstring key = util::Trim(tbRemoteKey.text);
        if (!IsHexKey(key)) { Flash(L"Нужен публичный ключ узла - 64 hex-символа", COL_ALERT); return; }
        lbRemoteOut.items.clear();
        lbRemoteOut.emptyText = L"жду ответа узла (до 15 с)…";
        remoteTitle = title;
        ygg::Paths p = snap.paths;
        Async([p, cmd, key] {
            json::Value v; std::wstring err;
            if (!ygg::Ctl(p, cmd + L" key=" + key, v, err, 20000)) return L"!" + err;
            return json::DumpW(v, true);
        }, [this](const std::wstring& res) {
            std::vector<ListItem> items;
            if (!res.empty() && res[0] == L'!') {
                lbRemoteOut.emptyText = L"узел не ответил: " + res.substr(1);
            } else {
                for (auto& l : TextLines(res)) {
                    ListItem it; it.left = l; it.key = l;
                    // ключи внутри ответа подсвечиваем - по ним можно спросить дальше
                    size_t q = l.find(L'"');
                    while (q != std::wstring::npos) {
                        size_t e = l.find(L'"', q + 1);
                        if (e == std::wstring::npos) break;
                        if (IsHexKey(l.substr(q + 1, e - q - 1))) { it.color = COL_OK; it.key = l.substr(q + 1, e - q - 1); break; }
                        q = l.find(L'"', e + 1);
                    }
                    items.push_back(it);
                }
            }
            lbRemoteOut.items = items; lbRemoteOut.sel = -1; lbRemoteOut.scroll = 0;
            Redraw();
        });
    }

    void DrawRemote() {
        if (NoCtl(L"УДАЛЁННЫЙ УЗЕЛ")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"УДАЛЁННЫЙ УЗЕЛ  //  запросы по ключу");
        float x = MX + 18, w = MW - 36, y = MY + 34;
        Caption(x, y, w, L"ПУБЛИЧНЫЙ КЛЮЧ УЗЛА  (из дерева, сессий, путей - кнопка «Запросить узел»; или вставь Ctrl+V)"); y += 18;
        Field(tbRemoteKey, x, y, w); y += 42;
        struct B { int id; const wchar_t* label; const wchar_t* cmd; };
        B bs[] = { { BTN_RM_NODEINFO, L"NODEINFO", L"getNodeInfo" }, { BTN_RM_SELF, L"СВЕДЕНИЯ", L"debug_remoteGetSelf" },
                   { BTN_RM_PEERS, L"ЕГО ПИРЫ", L"debug_remoteGetPeers" }, { BTN_RM_TREE, L"ЕГО ДЕРЕВО", L"debug_remoteGetTree" } };
        for (int i = 0; i < 4; ++i) Btn(bs[i].id, x + i * 160, y, 150, 32, bs[i].label, th.accent, i == 0, HasCommand(bs[i].cmd));
        const ListItem* sel = lbRemoteOut.Selected();
        Btn(BTN_RM_USE, x + 4 * 160, y, w - 4 * 160, 32, L"СПРОСИТЬ ЭТОТ", COL_OK, false, sel && IsHexKey(sel->key));
        y += 44;
        if (!remoteTitle.empty()) { Caption(x, y, w, remoteTitle); }
        y += 18;
        List(lbRemoteOut, x, y, w, MY + MH - y - 46);
        Note(x, MY + MH - 40, w, L"Отвечает только узел в сети. NodeInfo - то, что узел сам о себе рассказывает (имя, версия, платформа). "
                                 L"Зелёные строки - ключи других узлов: выбери и нажми «Спросить этот».");
    }

    // ================== 07 ИНТЕРФЕЙСЫ ==================
    void DrawIfaces() {
        if (NoCtl(L"ИНТЕРФЕЙСЫ")) return;
        const Theme th = Th();
        float lw = 320;
        r.Panel(MX, MY, lw, MH, th, L"TUN-АДАПТЕР  //  getTUN");
        float x = MX + 16, y = MY + 36, w = lw - 32;
        if (snap.tun.isObject()) {
            bool en = snap.tun[L"enabled"].boolean(true);
            Row(x, y, w, L"СОСТОЯНИЕ", en ? L"включён" : L"выключен", en ? COL_OK : COL_WARN, 110); y += 26;
            Row(x, y, w, L"ИМЯ", snap.tun[L"name"].str(), 0, 110); y += 26;
            Row(x, y, w, L"MTU", std::to_wstring(snap.tun[L"mtu"].i64()), 0, 110); y += 26;
        } else { r.Text(L"getTUN не ответил", x, y, w, 22, Rgb(th.muted), 11); y += 26; }
        // длинные значения - под подписью, на всю ширину: в строку они не помещаются
        r.Text(L"АДРЕС", x, y, w, 16, Rgb(th.muted), 10);
        r.Text(SelfAddr(), x, y + 16, w, 20, Rgb(th.text), 12); y += 46;
        r.Text(L"ПОДСЕТЬ", x, y, w, 16, Rgb(th.muted), 10);
        r.Text(snap.self[L"subnet"].str(), x, y + 16, w, 20, Rgb(th.text), 12); y += 56;
        Note(x, y, w, L"Подсеть /64 можно раздавать устройствам в локальной сети - у каждого будет свой адрес Yggdrasil без установки Yggdrasil на них.");

        float rx = MX + lw + 12, rw = MW - lw - 12;
        r.Panel(rx, MY, rw, MH, th, L"АВТОПОИСК СОСЕДЕЙ  //  getMulticastInterfaces");
        x = rx + 16; w = rw - 32; y = MY + 36;
        std::vector<ListItem> items;
        json::Value arr = FirstArray(snap.multicast);
        if (arr.size() == 0 && snap.multicast.isObject() && snap.multicast.obj)       // ответ вида {имя: {...}}
            for (const auto& kv : *snap.multicast.obj) { ListItem it; it.left = kv.first; it.right = kv.second.isObject() ? json::DumpW(kv.second) : L""; items.push_back(it); }
        for (size_t i = 0; i < arr.size(); ++i) {
            ListItem it;
            const auto& e = arr[i];
            if (e.isObject()) {
                // имя - первой строкой, остальное - второй (список переносит строки)
                std::wstring nm = e[L"name"].str().empty() ? json::DumpW(e) : e[L"name"].str();
                std::wstring flags;
                auto add = [&](const std::wstring& f) { if (!flags.empty()) flags += L" · "; flags += f; };
                if (e[L"beacon"].boolean()) add(L"beacon");
                if (e[L"listen"].boolean()) add(L"listen");
                if (e[L"port"].i64()) add(L"порт " + std::to_wstring(e[L"port"].i64()));
                if (e[L"priority"].i64()) add(L"приоритет " + std::to_wstring(e[L"priority"].i64()));
                if (e[L"password"].boolean()) add(L"с паролем");
                if (!e[L"address"].isNull() && !e[L"address"].str().empty()) add(e[L"address"].str());
                it.left = flags.empty() ? nm : nm + L"\n" + flags;
            } else it.left = e.str();
            items.push_back(it);
        }
        lbIfMulticast.SetItems(items);
        lbIfMulticast.hts.clear();
        for (const auto& it : items) lbIfMulticast.hts.push_back(r.TextWrapped(it.left, 0, 0, w - 28, Rgb(th.text), 12, false, false) + 12);
        Caption(x, y, w, L"ИНТЕРФЕЙСЫ, НА КОТОРЫХ ИЩЕМ СОСЕДЕЙ В ЛОКАЛЬНОЙ СЕТИ"); y += 18;
        List(lbIfMulticast, x, y, w, 170); y += 182;
        Caption(x, y, w, L"ВХОДЯЩИЕ  //  Listen из конфига"); y += 18;
        std::vector<ListItem> li;
        const auto& L = cfgOrig[L"Listen"];
        for (size_t i = 0; i < L.size(); ++i) { ListItem it; it.left = L[i].str(); li.push_back(it); }
        lbIfListen.SetItems(li);
        lbIfListen.emptyText = cfgLoaded ? L"Listen пуст - входящих пиров не ждём" : L"конфиг не прочитан";
        List(lbIfListen, x, y, w, 120); y += 132;
        float bw = (w - 12) / 2;
        Btn(BTN_IF_CFG, x, MY + MH - 50, bw, 34, L"НАСТРОИТЬ АВТОПОИСК", th.accent);
        Btn(BTN_IF_LISTEN, x + bw + 12, MY + MH - 50, bw, 34, L"НАСТРОИТЬ LISTEN", th.accent);
    }

    // ================== 08 СЛУЖБА ==================
    void DrawService() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"СЛУЖБА  //  Yggdrasil");
        float x = MX + 18, w = MW - 36, y = MY + 36;
        const auto& s = snap.service;
        if (!s.exists) {
            r.Text(L"СЛУЖБА YGGDRASIL НЕ НАЙДЕНА", x, y, w, 30, Rgb(COL_ALERT), 18);
            Note(x, y + 40, w, L"Установи Yggdrasil для Windows (msi с yggdrasil-network.github.io) - он ставит службу «Yggdrasil».");
            return;
        }
        bool run = s.state == SERVICE_RUNNING;
        Row(x, y, w, L"СОСТОЯНИЕ", util::ServiceStateText(s.state), run ? COL_OK : COL_ALERT, 150); y += 26;
        Row(x, y, w, L"РАБОТАЕТ", s.startedAt ? util::Duration((double)(Now() - s.startedAt)) + L"  (с " + util::TimeText(s.startedAt, L"%d.%m %H:%M") + L")" : L"—", 0, 150); y += 26;
        Row(x, y, w, L"PID", s.pid ? std::to_wstring(s.pid) : L"—", 0, 150); y += 26;
        Row(x, y, w, L"ВЕРСИЯ", snap.version.empty() ? L"—" : snap.version, 0, 150); y += 26;
        Row(x, y, w, L"ПРОГРАММА", snap.paths.exe, 0, 150); y += 26;
        Row(x, y, w, L"КОНФИГ", snap.paths.conf, 0, 150); y += 26;
        Row(x, y, w, L"ЛОГ", snap.paths.log.empty() ? L"не в файл (stdout/syslog)" : snap.paths.log, 0, 150); y += 26;
        Row(x, y, w, L"УПРАВЛЕНИЕ", snap.paths.adminListen.empty() ? L"по умолчанию" : snap.paths.adminListen, 0, 150); y += 36;

        Caption(x, y, w, L"УПРАВЛЕНИЕ СЛУЖБОЙ"); y += 20;
        Btn(BTN_SV_START, x, y, 170, 34, L"ЗАПУСТИТЬ", COL_OK, false, !run);
        Btn(BTN_SV_STOP, x + 182, y, 170, 34, L"ОСТАНОВИТЬ", COL_ALERT, false, run);
        Btn(BTN_SV_RESTART, x + 364, y, 190, 34, L"ПЕРЕЗАПУСТИТЬ", COL_WARN, false, run);
        Btn(BTN_SV_LATER, x + 566, y, 190, 34, L"ЧЕРЕЗ 15 С", th.accent, false, run);
        y += 44;
        Caption(x, y, w, L"ЗАПУСК ВМЕСТЕ С WINDOWS"); y += 20;
        Chip(BTN_SV_AUTO, x, y, 150, L"автоматически", s.startType == SERVICE_AUTO_START);
        Chip(BTN_SV_MANUAL, x + 160, y, 150, L"вручную", s.startType == SERVICE_DEMAND_START);
        Chip(BTN_SV_DISABLED, x + 320, y, 150, L"отключена", s.startType == SERVICE_DISABLED);
        y += 36;

        Caption(x, y, w, L"ОБНОВЛЕНИЯ  //  Yggdrasil и эта панель"); y += 20;
        std::wstring ut;
        unsigned uc = th.muted;
        bool newer = upd.ok && !snap.version.empty() && util::VersionNewer(snap.version, upd.tag);
        if (updChecking) ut = L"проверяю…";
        else if (!upd.checked) ut = L"ещё не проверял";
        else if (!upd.ok) { ut = L"не удалось: " + upd.err; uc = COL_WARN; }
        else if (newer) { ut = L"вышла " + upd.tag + L" (у тебя " + snap.version + L")"; uc = COL_OK; }
        else ut = L"у тебя последняя: " + upd.tag + L"   ·   проверено в " + util::TimeText(upd.at, L"%H:%M");
        r.Text(L"YGGDRASIL", x, y, 100, 32, Rgb(th.muted), 10, true);
        r.Text(ut, x + 100, y, w - 520, 32, Rgb(uc), 12);
        Btn(BTN_UPD_CHECK, x + w - 410, y, 150, 32, L"ПРОВЕРИТЬ", th.accent, false, !updChecking);
        Btn(BTN_UPD_INSTALL, x + w - 248, y, 248, 32, newer ? L"УСТАНОВИТЬ " + upd.tag : L"УСТАНОВИТЬ", COL_OK, newer, newer && !upd.msiUrl.empty());
        y += 38;
        {
            bool pnew = pupd.ok && util::VersionNewer(ygg::PANEL_VERSION, pupd.tag);
            std::wstring pt;
            unsigned pc = th.muted;
            if (pupdChecking) pt = L"проверяю…";
            else if (!pupd.checked) pt = L"у тебя v" + std::wstring(ygg::PANEL_VERSION) + L"   ·   ещё не проверял";
            else if (!pupd.ok) { pt = L"у тебя v" + std::wstring(ygg::PANEL_VERSION) + L"   ·   " + pupd.err; if (pupd.err.find(L"нет ни одного") == std::wstring::npos) pc = COL_WARN; }
            else if (pnew) { pt = L"вышла " + pupd.tag + L" (у тебя v" + ygg::PANEL_VERSION + L")"; pc = COL_OK; }
            else pt = L"у тебя последняя: v" + std::wstring(ygg::PANEL_VERSION) + L"   ·   проверено в " + util::TimeText(pupd.at, L"%H:%M");
            r.Text(L"ПАНЕЛЬ", x, y, 100, 32, Rgb(th.muted), 10, true);
            r.Text(pt, x + 100, y, w - 520, 32, Rgb(pc), 12);
            Btn(BTN_PUPD_CHECK, x + w - 410, y, 150, 32, L"ПРОВЕРИТЬ", th.accent, false, !pupdChecking);
            Btn(BTN_PUPD_OPEN, x + w - 248, y, 248, 32, L"СТРАНИЦА РЕЛИЗА", COL_OK, pnew, pupd.ok && !pupd.page.empty());
        }

        float by = MY + MH - 50;
        Btn(BTN_SV_CONFDIR, x, by, 200, 34, L"ПАПКА КОНФИГА", th.accent);
        Btn(BTN_SV_EXEDIR, x + 212, by, 220, 34, L"ПАПКА ПРОГРАММЫ", th.accent);
        Btn(BTN_SV_REREAD, x + 444, by, 220, 34, L"ПЕРЕЧИТАТЬ ПУТИ", th.accent);
    }

    ygg::UpdateInfo upd;
    bool updChecking = false;
    ygg::UpdateInfo pupd;          // обновления самой панели
    bool pupdChecking = false;

    void CheckPanelUpdate(bool quiet) {
        if (pupdChecking) return;
        pupdChecking = true;
        auto holder = std::make_shared<ygg::UpdateInfo>();
        Async([holder] { *holder = ygg::CheckPanelUpdate(); return std::wstring(); }, [this, holder, quiet](const std::wstring&) {
            pupdChecking = false;
            pupd = *holder;
            bool pnew = pupd.ok && util::VersionNewer(ygg::PANEL_VERSION, pupd.tag);
            if (pnew) Flash(L"Вышла новая версия панели " + pupd.tag + L" - на экране «Служба».", COL_OK);
            else if (!quiet) Flash(pupd.ok ? L"Панель последней версии: " + pupd.tag : L"Панель: " + pupd.err, pupd.ok ? 0 : COL_WARN);
            Redraw();
        });
    }

    void CheckUpdate(bool quiet) {
        if (updChecking) return;
        updChecking = true;
        auto holder = std::make_shared<ygg::UpdateInfo>();
        Async([holder] { *holder = ygg::CheckYggUpdate(); return std::wstring(); }, [this, holder, quiet](const std::wstring&) {
            updChecking = false;
            upd = *holder;
            bool newer = upd.ok && !snap.version.empty() && util::VersionNewer(snap.version, upd.tag);
            if (newer) Flash(L"Вышел Yggdrasil " + upd.tag + L" - обновить можно на экране «Служба».", COL_OK);
            else if (!quiet) Flash(upd.ok ? L"Новее нет: " + upd.tag : L"Проверка не удалась: " + upd.err, upd.ok ? 0 : COL_WARN);
            Redraw();
        });
    }

    // Скачать msi и запустить обычный установщик Yggdrasil (конфиг и ключ он не трогает)
    void InstallUpdate() {
        if (upd.msiUrl.empty()) return;
        Confirm(L"Скачать " + upd.msiName + L" и запустить установщик Yggdrasil " + upd.tag + L"?\n"
                L"Конфиг и ключ останутся прежними. Во время установки служба перезапустится - связь через Yggdrasil пропадёт на несколько секунд.",
                L"УСТАНОВИТЬ", [this] {
            std::wstring url = upd.msiUrl, name = upd.msiName;
            Op(L"скачиваю " + name, [url, name] {
                std::string data = util::HttpGet(url, nullptr, 200 * 1024 * 1024);
                if (data.size() < 100000) return std::wstring(L"!не удалось скачать установщик");
                wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
                std::wstring path = std::wstring(tmp) + name;
                if (!util::WriteFileBytes(path, data)) return std::wstring(L"!не удалось сохранить установщик");
                ShellExecuteW(nullptr, L"open", L"msiexec.exe", (L"/i \"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                return L"Установщик запущен: " + name;
            }, L"", [this](bool) { data.ReloadPaths(); });
        }, COL_OK);
    }

    void PickWatchdog() {
        wchar_t buf[MAX_PATH] = L"ygg_watchdog.exe";
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"ygg_watchdog.exe\0ygg_watchdog.exe\0Программы (*.exe)\0*.exe\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Где лежит watchdog (ygg_watchdog.exe)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;
        if (util::RegWriteString(ygg::REG_CONTROL, L"WatchdogPath", buf)) { Flash(L"Watchdog указан, ищу его…"); data.RefreshNow(); }
        else Flash(L"Не удалось запомнить путь", COL_ALERT);
    }

    // Состояние сторожа одним словом и цветом (низ окна и экран «Сторож»)
    void WdPhase(std::wstring& word, unsigned& col) const {
        const auto& st = snap.wdStatus;
        std::wstring ph = st[L"last_phase"].str();
        long long lt = st[L"last_tick_at"].i64();
        bool stale = lt > 0 && Now() - lt > 10 * 60;
        word = L"?"; col = Th().muted;
        if (snap.wdExe.empty()) { word = L"НЕ НАЙДЕН"; }
        else if (!snap.wdStatusOk) { word = L"НЕ ОТВЕЧАЕТ"; col = COL_ALERT; }
        else if (!snap.wdTask.ok) { word = L"АВТОПРОВЕРКА ВЫКЛЮЧЕНА"; col = COL_WARN; }
        else if (stale) { word = L"МОЛЧИТ"; col = COL_ALERT; }
        else if (ph == L"ok") { word = L"ACTIVE"; col = COL_OK; }
        else if (ph == L"backup") { word = L"ЧЕРЕЗ РЕЗЕРВ"; col = COL_OK; }
        else if (ph == L"regional") { word = L"РЕГИОН"; col = COL_OK; }
        else if (ph == L"down" || ph == L"regional_search") { word = L"ИЩЕТ ЗАМЕНУ"; col = COL_WARN; }
        else if (ph == L"crisis") { word = L"КРАЙНИЙ ПЕРЕБОР"; col = COL_WARN; }
        else if (ph == L"no_internet") { word = L"НЕТ ИНТЕРНЕТА"; col = COL_ALERT; }
        else if (ph == L"battery") { word = L"БАТАРЕЯ"; col = COL_ALERT; }
        else if (lt == 0) { word = L"ЕЩЁ НЕ ПРОВЕРЯЛ"; col = COL_WARN; }
    }

    // ================== СТОРОЖ ==================
    void DrawWatchdog() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"WATCHDOG  //  ygg-watchdog");
        float x = MX + 18, w = MW - 36, y = MY + 36;
        if (snap.wdExe.empty()) {
            r.Text(L"WATCHDOG НЕ НАЙДЕН", x, y, w, 30, Rgb(COL_WARN), 18); y += 40;
            Note(x, y, w, L"ygg-watchdog сам следит за пирами: ищет замену упавшим, перебирает страны, шлёт уведомления. "
                          L"Панель находит его по записи в реестре, которую он оставляет при каждом запуске, или по задаче Планировщика. "
                          L"Если он лежит в другом месте - укажи ygg_watchdog.exe вручную.");
            Btn(BTN_WD_PICK, x, y + 70, 260, 34, L"УКАЗАТЬ ygg_watchdog.exe…", th.accent, true);
            return;
        }
        const auto& st = snap.wdStatus;
        std::wstring word; unsigned col;
        WdPhase(word, col);
        r.Text(word, x, y, w, 30, Rgb(col), 20); y += 40;
        Row(x, y, w, L"ПРОГРАММА", snap.wdExe, 0, 170); y += 24;
        Row(x, y, w, L"ВЕРСИЯ", st[L"version"].str().empty() ? L"—" : L"v" + st[L"version"].str(), 0, 170); y += 24;
        long long lt = st[L"last_tick_at"].i64();
        Row(x, y, w, L"ПОСЛЕДНЯЯ ПРОВЕРКА", lt ? util::TimeText(lt, L"%H:%M:%S") + L"  (" + std::to_wstring((Now() - lt) / 60) + L" мин назад)" : L"ещё не было", 0, 170); y += 24;
        std::wstring next;
        if (!snap.wdTask.ok) next = L"задача Планировщика не установлена - сам не проверяет";
        else if (snap.wdTask.state == 1) next = L"задача выключена";
        else if (snap.wdTask.state == 4) next = L"проверяет прямо сейчас";
        else if (snap.wdTask.nextRun) {
            long long left = snap.wdTask.nextRun - Now(); if (left < 0) left = 0;
            wchar_t b[64]; swprintf(b, 64, L"%ls  (через %lld:%02lld)", util::TimeText(snap.wdTask.nextRun, L"%H:%M").c_str(), left / 60, left % 60);
            next = b;
        } else next = L"—";
        Row(x, y, w, L"СЛЕДУЮЩАЯ", next, snap.wdTask.ok ? 0 : COL_WARN, 170); y += 24;
        bool regional = st[L"mode"].str() == L"regional";
        Row(x, y, w, L"РЕЖИМ", regional ? L"региональный" : L"обычный", 0, 170); y += 24;
        const auto& mp = st[L"main_peers"];
        int up = 0;
        for (size_t i = 0; i < mp.size(); ++i)
            for (size_t j = 0; j < snap.peers.size(); ++j)
                if (snap.peers[j][L"remote"].str() == mp[i].str() && snap.peers[j][L"up"].boolean()) { ++up; break; }
        Row(x, y, w, L"ОСНОВНЫЕ ПИРЫ", std::to_wstring(up) + L" / " + std::to_wstring(mp.size()), up ? COL_OK : COL_ALERT, 170); y += 24;
        Row(x, y, w, L"РЕЗЕРВНЫЕ", std::to_wstring(st[L"backup_peers"].size()), 0, 170); y += 30;

        // разбор «почему нет связи» (watchdog считает его в status-json)
        const auto& dg = st[L"diagnosis"];
        bool problem = !dg.isNull() && !dg[L"verdict"].str().empty() &&
                       (!dg[L"any_up"].boolean() || !dg[L"advice"].str().empty());
        if (problem) {
            unsigned dc = dg[L"any_up"].boolean() ? COL_WARN : COL_ALERT;
            Caption(x, y, w, st[L"last_phase"].str() == L"backup" ? L"ОСНОВНЫЕ ПИРЫ: ПОЧЕМУ НЕ РАБОТАЮТ" : L"ПОЧЕМУ НЕТ СВЯЗИ"); y += 18;
            y += r.TextWrapped(dg[L"verdict"].str(), x, y, w, Rgb(dc), 11) + 2;
            const auto& dp = dg[L"peers"];
            for (size_t i = 0; i < dp.size(); ++i) {
                if (dp[i][L"kind"].str() == L"up") continue;
                r.Text(ShortName(dp[i][L"uri"].str()), x + 12, y, 160, 18, Rgb(th.text), 10);
                r.Text(dp[i][L"text"].str(), x + 180, y, w - 180, 18, Rgb(th.muted), 10);
                y += 18;
            }
            if (!dg[L"advice"].str().empty()) y += r.TextWrapped(dg[L"advice"].str(), x, y, w, Rgb(th.muted), 11);
            y += 10;
        }

        Caption(x, y, w, L"ПОСЛЕДНИЕ СОБЫТИЯ"); y += 18;
        float ey = y;
        for (int i = (int)snap.wdEvents.size() - 1; i >= 0 && ey < MY + MH - 70; --i) {
            const std::wstring& e = snap.wdEvents[i];
            size_t t1 = e.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : e.find(L'\t', t1 + 1);
            if (t2 == std::wstring::npos) continue;
            long long ts = _wtoi64(e.substr(0, t1).c_str());
            std::wstring lv = e.substr(t1 + 1, t2 - t1 - 1), tx = e.substr(t2 + 1);
            unsigned c = lv == L"ok" ? COL_OK : lv == L"warn" ? COL_WARN : lv == L"alert" ? COL_ALERT : th.text;
            r.Text(util::TimeText(ts, L"%d.%m %H:%M"), x, ey, 100, 20, Rgb(th.muted), 10);
            r.Text(tx, x + 104, ey, w - 104, 20, Rgb(c), 11);
            ey += 21;
        }
        if (snap.wdEvents.empty()) r.Text(L"событий пока нет", x, ey, w, 20, Rgb(th.muted), 11);

        float by = MY + MH - 50;
        Btn(BTN_WD_OPEN, x, by, 230, 34, L"ОТКРЫТЬ ОКНО WATCHDOG", COL_OK, true);
        Btn(BTN_WD_TICK, x + 242, by, 180, 34, L"ПРОВЕРИТЬ СЕЙЧАС", th.accent, false, !busy);
        if (!snap.wdTask.ok) Btn(BTN_WD_INSTALL, x + 434, by, 200, 34, L"ВКЛЮЧИТЬ АВТОПРОВЕРКУ", COL_OK, false);
        else Btn(BTN_WD_REMOVE, x + 434, by, 200, 34, L"ВЫКЛЮЧИТЬ", COL_ALERT, false);
        Btn(BTN_WD_PICK, x + 646, by, w - 646, 34, L"ДРУГОЙ…", th.accent, false);
    }

    void WatchdogCmd(const std::wstring& cmd, const std::wstring& label, const std::wstring& ok) {
        if (snap.wdExe.empty()) return;
        std::wstring exe = snap.wdExe;
        Op(label, [exe, cmd] {
            util::ProcResult r = util::Run(exe, cmd, 120000);
            if (!r.started) return std::wstring(L"!watchdog не запустился");
            std::wstring out = util::FromUtf8(r.out);
            size_t e = out.find(L"Ошибка");
            if (e != std::wstring::npos) return L"!" + util::Trim(out.substr(e + 7, 160));
            return std::wstring();
        }, ok);
    }

    void ServiceAction(const std::wstring& action, const std::wstring& label, const std::wstring& ok) {
        Op(label, [action] { std::wstring e = util::ControlServiceNamed(ygg::SERVICE_NAME, action); return e.empty() ? std::wstring() : L"!" + e; },
           ok, [this](bool) { data.ReloadPaths(); });
    }

    void RestartLater() {
        std::wstring svc = ygg::SERVICE_NAME;
        std::wstring cmd = L"cmd.exe /c ping -n 16 127.0.0.1 >nul & net stop \"" + svc + L"\" & net start \"" + svc + L"\"";
        if (util::RunDetached(cmd)) Flash(L"Служба перезапустится через 15 секунд.");
        else Flash(L"Не удалось запланировать перезапуск", COL_ALERT);
    }

    // ================== 09 ЛОГ ==================
    int logLevel = 0;          // 0 все, 1 инфо, 2 предупр, 3 ошибки
    bool logFollow = true;
    size_t logShownFor = (size_t)-1;
    std::wstring logFilterFor;

    static int LineLevel(const std::wstring& l) {
        std::wstring s = util::Lower(l);
        if (s.find(L"error") != std::wstring::npos || s.find(L"failed") != std::wstring::npos || s.find(L"panic") != std::wstring::npos) return 3;
        if (s.find(L"warn") != std::wstring::npos || s.find(L"disconnected") != std::wstring::npos) return 2;
        return 1;
    }

    void RebuildLog() {
        std::wstring key = std::to_wstring(logLevel) + L"|" + util::Lower(util::Trim(tbLogSearch.text));
        if (logShownFor == snap.logLines.size() + (size_t)snap.logSize && logFilterFor == key) return;
        logShownFor = snap.logLines.size() + (size_t)snap.logSize;
        logFilterFor = key;
        std::wstring q = util::Lower(util::Trim(tbLogSearch.text));
        std::vector<ListItem> items;
        for (const auto& l : snap.logLines) {
            int lv = LineLevel(l);
            if (logLevel && lv != logLevel) continue;
            if (!q.empty() && util::Lower(l).find(q) == std::wstring::npos) continue;
            ListItem it; it.left = l; it.key = l;
            it.color = lv == 3 ? COL_ALERT : lv == 2 ? COL_WARN : 0;
            items.push_back(it);
        }
        int oldSel = lbLog.sel;
        lbLog.items = items;
        lbLog.sel = oldSel < (int)items.size() ? oldSel : -1;
        if (logFollow) lbLog.scroll = (std::max)(0, (int)items.size() - lbLog.Visible());
    }

    void DrawLog() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"ЛОГ  //  " + (snap.paths.log.empty() ? std::wstring(L"не в файл") : snap.paths.log.substr(snap.paths.log.find_last_of(L"\\/") + 1)));
        float x = MX + 16, w = MW - 32, y = MY + 32;
        static const wchar_t* lv[] = { L"ВСЕ", L"ИНФО", L"ПРЕДУПР", L"ОШИБКИ" };
        for (int i = 0; i < 4; ++i) Chip(BTN_LOG_LEVEL + i, x + i * 92, y, 84, lv[i], logLevel == i);
        Field(tbLogSearch, x + w - 260, y - 3, 260, 28);
        y += 34;
        if (snap.paths.log.empty()) {
            Note(x, y, w, L"Служба пишет лог не в файл (параметр -logto stdout или syslog). Чтобы видеть лог здесь, служба должна запускаться с -logto <файл>.");
            return;
        }
        RebuildLog();
        List(lbLog, x, y, w, MY + MH - y - 52);
        float by = MY + MH - 44;
        wchar_t info[160];
        swprintf(info, 160, L"%d строк  ·  файл %ls  ·  [Пробел] - пауза",
                 (int)lbLog.items.size(), util::Bytes((double)snap.logSize).c_str());
        r.Text(info, x, by, w - 440, 30, Rgb(th.muted), 10);
        Btn(BTN_LOG_FOLLOW, x + w - 430, by, 140, 30, logFollow ? L"ПАУЗА" : L"СЛЕДИТЬ", th.accent);
        Btn(BTN_LOG_NOTEPAD, x + w - 280, by, 130, 30, L"БЛОКНОТ", th.accent);
        Btn(BTN_LOG_DIR, x + w - 140, by, 140, 30, L"ПАПКА", th.accent);
    }

    // ================== 10 КОНФИГ ==================
    std::wstring cfgSelKey;          // выбранное поле
    std::wstring cfgFilledFor;       // для какого поля/строки заполнено поле ввода

    void ReloadConfig() {
        if (cfgLoading) return;
        cfgLoading = true;
        ygg::Paths p = snap.paths;
        if (p.exe.empty()) p = ygg::Discover();
        auto holder = std::make_shared<json::Value>();
        Async([p, holder] {
            std::wstring err;
            if (!ygg::LoadConfig(p, *holder, err)) return L"!" + err;
            return std::wstring();
        }, [this, holder](const std::wstring& res) {
            cfgLoading = false;
            if (!res.empty()) { cfgError = res.substr(1); cfgLoaded = false; Redraw(); return; }
            cfgOrig = *holder;
            cfgEdit = json::Clone(cfgOrig);
            cfgLoaded = true; cfgError.clear(); cfgFilledFor.clear();
            Redraw();
        });
    }

    std::vector<std::wstring> CfgKeys() const {
        std::vector<std::wstring> keys;
        if (!cfgEdit.isObject() || !cfgEdit.obj) return keys;
        for (const auto& h : CFG_HELP) if (cfgEdit.obj->count(h.key)) keys.push_back(h.key);
        for (const auto& kv : *cfgEdit.obj)
            if (!IsHiddenCfgKey(kv.first) && std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
        return keys;
    }

    bool CfgChanged(const std::wstring& k) const { return json::Dump(cfgOrig[k]) != json::Dump(cfgEdit[k]); }
    bool CfgAnyChanged() const {
        if (!cfgLoaded) return false;
        std::set<std::wstring> all;
        if (cfgOrig.obj) for (auto& kv : *cfgOrig.obj) all.insert(kv.first);
        if (cfgEdit.obj) for (auto& kv : *cfgEdit.obj) all.insert(kv.first);
        for (auto& k : all) if (CfgChanged(k)) return true;
        return false;
    }

    std::vector<std::pair<std::wstring, unsigned>> CfgDiff() const {
        std::vector<std::pair<std::wstring, unsigned>> out;
        std::set<std::wstring> all;
        if (cfgOrig.obj) for (auto& kv : *cfgOrig.obj) all.insert(kv.first);
        if (cfgEdit.obj) for (auto& kv : *cfgEdit.obj) all.insert(kv.first);
        for (const auto& k : all) {
            if (!CfgChanged(k)) continue;
            if (IsHiddenCfgKey(k)) { out.push_back({ L"~ " + k + L" (ключ узла)", COL_WARN }); continue; }
            const auto& a = cfgOrig[k]; const auto& b = cfgEdit[k];
            if ((a.isArray() || a.isNull()) && b.isArray()) {
                std::set<std::wstring> sa, sb;
                for (size_t i = 0; i < a.size(); ++i) sa.insert(a[i].isString() ? a[i].str() : json::DumpW(a[i]));
                for (size_t i = 0; i < b.size(); ++i) sb.insert(b[i].isString() ? b[i].str() : json::DumpW(b[i]));
                for (auto& s : sa) if (!sb.count(s)) out.push_back({ L"− " + k + L": " + s, COL_ALERT });
                for (auto& s : sb) if (!sa.count(s)) out.push_back({ L"+ " + k + L": " + s, COL_OK });
            } else {
                out.push_back({ L"~ " + k + L": " + json::DumpW(a) + L"  →  " + json::DumpW(b), COL_WARN });
            }
        }
        return out;
    }

    // Текст из поля -> значение: строка или JSON
    bool ParseCfgValue(const std::wstring& text, bool preferString, json::Value& out) {
        std::wstring t = util::Trim(text);
        if (preferString && !(t.size() && (t[0] == L'{' || t[0] == L'['))) { out = json::MakeString(t); return true; }
        json::Value v;
        if (json::Parse(json::ToUtf8(t), v) && (t.size() == 0 || t[0] != L'"' || v.isString())) { out = v; return true; }
        if (preferString) { out = json::MakeString(t); return true; }
        return false;
    }

    bool ArrayOfStrings(const std::wstring& k, const json::Value& v) const {
        if (v.size() == 0) return k == L"Peers" || k == L"Listen" || k == L"AllowedPublicKeys";
        for (size_t i = 0; i < v.size(); ++i) if (!v[i].isString()) return false;
        return true;
    }

    std::wstring ItemText(const json::Value& v) const { return v.isString() ? v.str() : json::DumpW(v); }

    void DrawConfig() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"КОНФИГ  //  " + snap.paths.conf.substr(snap.paths.conf.find_last_of(L"\\/") + 1));
        float x = MX + 16;
        if (!cfgLoaded) {
            r.Text(cfgLoading ? L"читаю конфиг…" : L"Конфиг не прочитан: " + cfgError, x, MY + 40, MW - 32, 24,
                   Rgb(cfgLoading ? th.muted : COL_ALERT), 12);
            if (!cfgLoading) Btn(BTN_REFRESH, x, MY + 80, 200, 34, L"ПРОЧИТАТЬ СНОВА", th.accent);
            return;
        }
        // поля
        std::vector<ListItem> keys;
        for (auto& k : CfgKeys()) {
            ListItem it; it.left = k; it.key = k;
            if (CfgChanged(k)) { it.right = L"изм."; it.rightColor = COL_WARN; }
            keys.push_back(it);
        }
        lbCfgKeys.SetItems(keys);
        if (lbCfgKeys.sel < 0 && !keys.empty()) lbCfgKeys.sel = 0;
        List(lbCfgKeys, x, MY + 34, 210, 288);
        r.Text(L"PrivateKey скрыт  →  «Ключ узла»", x, MY + 326, 210, 16, Rgb(th.muted), 9);

        const ListItem* ks = lbCfgKeys.Selected();
        std::wstring K = ks ? ks->key : L"";
        if (K != cfgSelKey) { cfgSelKey = K; cfgFilledFor.clear(); lbCfgItems.sel = -1; lbCfgItems.scroll = 0; tbCfgItem.Set(L""); tbCfgKey.Set(L""); }
        float ex = MX + 240, ew = MW - 256, y = MY + 34;
        if (!K.empty()) {
            const json::Value& V = cfgEdit[K];
            r.Text(K, ex, y, ew, 20, Rgb(th.text), 14, true); y += 22;
            y += r.TextWrapped(CfgHelpFor(K), ex, y, ew, Rgb(th.muted), 10) + 8;
            if (V.isArray()) {
                std::vector<ListItem> items;
                for (size_t i = 0; i < V.size(); ++i) { ListItem it; it.left = ItemText(V[i]); it.key = std::to_wstring(i); items.push_back(it); }
                lbCfgItems.items = items; lbCfgItems.Clamp();
                lbCfgItems.emptyText = L"пусто - добавь строку ниже";
                List(lbCfgItems, ex, y, ew, 150); y += 158;
                const ListItem* sel = lbCfgItems.Selected();
                std::wstring fillKey = K + L"#" + (sel ? sel->key : L"-");
                if (cfgFilledFor != fillKey) { cfgFilledFor = fillKey; tbCfgItem.Set(sel ? sel->left : L""); }
                tbCfgItem.placeholder = ArrayOfStrings(K, V) ? L"новая строка" : L"JSON-объект";
                Field(tbCfgItem, ex, y, ew); y += 38;
                bool has = !util::Trim(tbCfgItem.text).empty();
                Btn(BTN_CF_ADD, ex, y, 150, 30, L"+ СТРОКА", th.accent, false, has);
                Btn(BTN_CF_REPLACE, ex + 160, y, 150, 30, L"ЗАМЕНИТЬ", th.accent, false, has && sel);
                Btn(BTN_CF_DEL, ex + 320, y, 150, 30, L"УДАЛИТЬ", COL_ALERT, false, sel != nullptr);
            } else if (V.isObject()) {
                std::vector<ListItem> items;
                if (V.obj) for (const auto& kv : *V.obj) { ListItem it; it.left = kv.first + L": " + ItemText(kv.second); it.key = kv.first; items.push_back(it); }
                lbCfgItems.items = items; lbCfgItems.Clamp();
                lbCfgItems.emptyText = L"пусто - добавь пару ключ/значение ниже";
                List(lbCfgItems, ex, y, ew, 150); y += 158;
                const ListItem* sel = lbCfgItems.Selected();
                std::wstring fillKey = K + L"#" + (sel ? sel->key : L"-");
                if (cfgFilledFor != fillKey) {
                    cfgFilledFor = fillKey;
                    tbCfgKey.Set(sel ? sel->key : L"");
                    tbCfgItem.Set(sel ? ItemText(V[sel->key]) : L"");
                }
                tbCfgItem.placeholder = L"значение (строка или JSON)";
                Field(tbCfgKey, ex, y, 160);
                Field(tbCfgItem, ex + 170, y, ew - 170); y += 38;
                bool has = !util::Trim(tbCfgKey.text).empty();
                Btn(BTN_CF_SET, ex, y, 150, 30, L"ЗАДАТЬ", th.accent, false, has);
                Btn(BTN_CF_DEL, ex + 160, y, 150, 30, L"УДАЛИТЬ", COL_ALERT, false, sel != nullptr);
            } else if (V.type == json::Value::Bool) {
                Chip(BTN_CF_TRUE, ex, y, 120, L"true", V.boolean());
                Chip(BTN_CF_FALSE, ex + 130, y, 120, L"false", !V.boolean());
                y += 36;
            } else {
                std::wstring fillKey = K + L"#scalar";
                if (cfgFilledFor != fillKey) { cfgFilledFor = fillKey; tbCfgItem.Set(V.isString() ? V.str() : json::DumpW(V)); }
                tbCfgItem.placeholder = L"значение";
                Field(tbCfgItem, ex, y, ew); y += 38;
                Btn(BTN_CF_SET, ex, y, 150, 30, L"ЗАДАТЬ", th.accent);
            }
        }

        // изменения
        float cy = MY + 350, ch = 120;
        r.Brackets(x, cy, MW - 32, ch, Rgb(th.accent, 0.6f), 10, 1.2f);
        r.Text(L"ИЗМЕНЕНИЯ", x + 12, cy + 6, 200, 16, Rgb(COL_WARN), 10);
        auto diff = CfgDiff();
        float ly = cy + 26;
        if (diff.empty()) r.Text(L"нет", x + 12, ly, 300, 18, Rgb(th.muted), 11);
        for (size_t i = 0; i < diff.size() && i < 4; ++i) { r.Text(diff[i].first, x + 12, ly, 440, 18, Rgb(diff[i].second), 11); ly += 20; }
        if (diff.size() > 4) r.Text(L"и ещё " + std::to_wstring(diff.size() - 4), x + 12, ly, 300, 18, Rgb(th.muted), 10);
        r.TextWrapped(L"Перед сохранением конфиг проверяет сам yggdrasil.\nСтарый остаётся копией: yggdrasil.conf.bak-дата.\n"
                      L"Файл пишется в JSON - комментарии из него пропадут.",
                      x + 470, cy + 26, MW - 32 - 482, Rgb(th.muted), 10);

        bool any = CfgAnyChanged();
        float by = MY + MH - 50;
        Btn(BTN_CF_APPLY, x, by, 300, 34, L"ПРИМЕНИТЬ И ПЕРЕЗАПУСТИТЬ", COL_OK, true, any);
        Btn(BTN_CF_SAVE, x + 312, by, 180, 34, L"ТОЛЬКО СОХРАНИТЬ", th.accent, false, any);
        Btn(BTN_CF_REVERT, x + 504, by, 160, 34, L"ОТМЕНИТЬ", th.accent, false, any);
    }

    void CfgEditAction(int id) {
        if (!cfgLoaded || cfgSelKey.empty() || !cfgEdit.obj) return;
        json::Value& V = (*cfgEdit.obj)[cfgSelKey];
        const ListItem* sel = lbCfgItems.Selected();
        if (V.isArray()) {
            json::Value item;
            if (id == BTN_CF_ADD || id == BTN_CF_REPLACE) {
                if (!ParseCfgValue(tbCfgItem.text, ArrayOfStrings(cfgSelKey, V), item)) { Flash(L"Это не JSON - проверь скобки и кавычки", COL_ALERT); return; }
                if (!V.arr) V = json::MakeArray();
                if (id == BTN_CF_ADD) { V.arr->push_back(item); lbCfgItems.sel = (int)V.arr->size() - 1; }
                else if (sel) (*V.arr)[std::stoi(sel->key)] = item;
            } else if (id == BTN_CF_DEL && sel && V.arr) {
                V.arr->erase(V.arr->begin() + std::stoi(sel->key));
                lbCfgItems.sel = -1;
            }
        } else if (V.isObject()) {
            if (id == BTN_CF_SET) {
                json::Value item;
                ParseCfgValue(tbCfgItem.text, true, item);
                std::wstring k = util::Trim(tbCfgKey.text);
                if (!V.obj) V = json::MakeObject();
                (*V.obj)[k] = item;
            } else if (id == BTN_CF_DEL && sel && V.obj) { V.obj->erase(sel->key); lbCfgItems.sel = -1; }
        } else if (id == BTN_CF_TRUE || id == BTN_CF_FALSE) {
            V.type = json::Value::Bool; V.b = id == BTN_CF_TRUE;
        } else if (id == BTN_CF_SET) {
            json::Value item;
            bool wantString = V.isString() || V.isNull();
            if (!ParseCfgValue(tbCfgItem.text, wantString, item) || (V.type == json::Value::Number && item.type != json::Value::Number)) {
                Flash(L"Здесь нужно число", COL_ALERT); return;
            }
            V = item;
        }
        cfgFilledFor.clear();
    }

    void SaveConfigUI(bool restart) {
        ygg::Paths p = snap.paths;
        json::Value cfg = json::Clone(cfgEdit);
        Op(restart ? L"сохраняю конфиг и перезапускаю службу" : L"сохраняю конфиг", [p, cfg, restart] {
            std::wstring backup;
            std::wstring e = ygg::SaveConfig(p, cfg, &backup);
            if (!e.empty()) return L"!" + e;
            if (restart) {
                e = util::ControlServiceNamed(ygg::SERVICE_NAME, L"restart");
                if (!e.empty()) return L"!конфиг сохранён, но служба: " + e;
                return L"Конфиг сохранён, служба перезапущена. Копия: " + backup.substr(backup.find_last_of(L"\\/") + 1);
            }
            return L"Конфиг сохранён (заработает после перезапуска службы). Копия: " + backup.substr(backup.find_last_of(L"\\/") + 1);
        }, L"", [this](bool) { data.ReloadPaths(); ReloadConfig(); });
    }

    // ================== 11 КЛЮЧ УЗЛА ==================
    void DrawKey() {
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"КЛЮЧ УЗЛА");
        float x = MX + 18, w = MW - 36, y = MY + 36;
        Row(x, y, w, L"АДРЕС", SelfAddr().empty() ? L"—" : SelfAddr(), 0, 150); y += 26;
        Row(x, y, w, L"ПОДСЕТЬ", snap.self[L"subnet"].str().empty() ? L"—" : snap.self[L"subnet"].str(), 0, 150); y += 26;
        r.Text(L"ПУБЛИЧНЫЙ КЛЮЧ", x, y, 150, 22, Rgb(th.muted), 11);
        r.Text(SelfKey().empty() ? L"—" : SelfKey(), x + 150, y, w - 150, 22, Rgb(th.text), 11); y += 26;
        std::wstring src = !cfgLoaded ? L"конфиг не прочитан" :
            (!cfgOrig[L"PrivateKeyPath"].str().empty() ? L"файл PEM: " + cfgOrig[L"PrivateKeyPath"].str() : L"PrivateKey внутри конфига");
        Row(x, y, w, L"ЗАКРЫТЫЙ КЛЮЧ", src, 0, 150); y += 40;
        Note(x, y, w, L"Адрес в Yggdrasil вычисляется из ключа. Закрытый ключ - это и есть твой адрес: кто его знает, тот может выдать себя за этот узел. "
                      L"Панель его не показывает.");
        y += 52;
        Btn(BTN_KEY_COPY, x, y, 240, 34, L"КОПИРОВАТЬ ПУБЛИЧНЫЙ", th.accent, false, !SelfKey().empty());
        y += 56;
        r.Text(L"ЭКСПОРТ КЛЮЧА", x, y, w, 18, Rgb(th.accent), 11); y += 20;
        Note(x, y, w, L"Сохраняет закрытый ключ в файл PEM (yggdrasil -exportkey). Пригодится, чтобы перенести этот адрес на другую машину "
                      L"или хранить ключ отдельно от конфига (PrivateKeyPath). Храни файл как пароль.");
        y += 40;
        Btn(BTN_KEY_EXPORT, x, y, 240, 34, L"ЭКСПОРТ В PEM…", th.accent, false, cfgLoaded);
        y += 56;
        r.Text(L"НОВЫЙ КЛЮЧ = НОВЫЙ АДРЕС", x, y, w, 18, Rgb(COL_ALERT), 11); y += 20;
        Note(x, y, w, L"Создаёт новый ключ (yggdrasil -genconf), записывает его в конфиг и перезапускает службу. Адрес сменится: всё, что "
                      L"знает старый адрес (RDP, SSH, правила файрвола, AllowedPublicKeys у друзей, watchdog на других машинах), придётся поправить. "
                      L"Старый конфиг со старым ключом останется копией - вернуть можно.");
        y += 58;
        Btn(BTN_KEY_NEW, x, y, 240, 34, L"НОВЫЙ КЛЮЧ", COL_ALERT, false, cfgLoaded);
    }

    void ExportKey() {
        wchar_t buf[MAX_PATH] = L"yggdrasil-key.pem";
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Ключ PEM (*.pem)\0*.pem\0Все файлы\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"pem";
        ofn.lpstrTitle = L"Куда сохранить закрытый ключ";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&ofn)) return;
        std::wstring path = buf;
        ygg::Paths p = snap.paths;
        Op(L"экспорт ключа", [p, path] {
            util::ProcResult r = util::Run(p.exe, L"-useconffile \"" + p.conf + L"\" -exportkey", 15000);
            if (!r.started || r.code != 0 || r.out.find("-----BEGIN") == std::string::npos)
                return L"!yggdrasil не выдал ключ: " + util::Trim(util::FromUtf8(r.out)).substr(0, 160);
            if (!util::WriteFileBytes(path, r.out)) return std::wstring(L"!не удалось записать файл");
            return L"Ключ сохранён: " + path;
        }, L"");
    }

    void NewKey() {
        Confirm(L"Создать НОВЫЙ ключ? Адрес этого узла сменится навсегда.\n\nВсё, что знает старый адрес (RDP, SSH, правила файрвола, "
                L"AllowedPublicKeys у друзей), перестанет его находить. Старый конфиг сохранится копией.", L"СМЕНИТЬ КЛЮЧ", [this] {
            ygg::Paths p = snap.paths;
            json::Value cfg = json::Clone(cfgOrig);
            Op(L"меняю ключ", [p, cfg]() mutable -> std::wstring {
                util::ProcResult r = util::Run(p.exe, L"-genconf -json", 15000);
                json::Value gen;
                if (!json::Parse(r.out, gen) || gen[L"PrivateKey"].str().empty()) return L"!yggdrasil -genconf не выдал ключ";
                if (!cfg.obj) return L"!конфиг пуст";
                (*cfg.obj)[L"PrivateKey"] = gen[L"PrivateKey"];
                cfg.obj->erase(L"PrivateKeyPath");
                std::wstring backup;
                std::wstring e = ygg::SaveConfig(p, cfg, &backup);
                if (!e.empty()) return L"!" + e;
                e = util::ControlServiceNamed(ygg::SERVICE_NAME, L"restart");
                if (!e.empty()) return L"!ключ записан, но служба: " + e;
                return L"Новый ключ записан, служба перезапущена. Старый конфиг: " + backup.substr(backup.find_last_of(L"\\/") + 1);
            }, L"", [this](bool) { data.ReloadPaths(); ReloadConfig(); });
        }, COL_ALERT);
    }

    // ================== 12 ВСЕ КОМАНДЫ ==================
    std::wstring cmdSelFor;

    void DrawCommands() {
        if (NoCtl(L"ВСЕ КОМАНДЫ  //  list")) return;
        const Theme th = Th();
        r.Panel(MX, MY, MW, MH, th, L"ВСЕ КОМАНДЫ  //  list");
        float x = MX + 16;
        json::Value list = FirstArray(snap.commands);
        std::vector<ListItem> items;
        for (size_t i = 0; i < list.size(); ++i) {
            ListItem it;
            it.key = it.left = list[i].isObject() ? list[i][L"command"].str() : list[i].str();
            const auto& f = list[i][L"fields"];
            for (size_t k = 0; k < f.size(); ++k) it.right += (k ? L" " : L"") + f[k].str();
            if (!it.key.empty()) items.push_back(it);
        }
        lbCmds.SetItems(items);
        List(lbCmds, x, MY + 34, 300, MH - 50);
        float ex = x + 316, ew = MW - 32 - 316, y = MY + 34;
        const ListItem* sel = lbCmds.Selected();
        if (!sel) { Note(ex, y, ew, L"Все команды, которые понимает эта версия Yggdrasil. Выбери слева - справа появятся её параметры. "
                                    L"Ответ показывается как есть, в JSON - так можно сделать то, для чего в панели нет отдельной кнопки."); return; }
        if (cmdSelFor != sel->key) { cmdSelFor = sel->key; for (auto& t : tbCmdArgs) t.Set(L""); }
        r.Text(sel->key, ex, y, ew, 22, Rgb(th.text), 14, true); y += 24;
        std::wstring desc;
        std::vector<std::wstring> fields;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].isObject() && list[i][L"command"].str() == sel->key) {
                desc = list[i][L"description"].str();
                for (size_t k = 0; k < list[i][L"fields"].size(); ++k) fields.push_back(list[i][L"fields"][k].str());
            }
        if (!desc.empty()) y += r.TextWrapped(desc, ex, y, ew, Rgb(th.muted), 11) + 8;
        for (size_t k = 0; k < fields.size() && k < tbCmdArgs.size(); ++k) {
            r.Text(fields[k], ex, y, 120, 28, Rgb(th.accent), 11);
            tbCmdArgs[k].placeholder = L"значение";
            Field(tbCmdArgs[k], ex + 120, y, ew - 120, 28);
            y += 34;
        }
        Btn(BTN_CMD_RUN, ex, y, 180, 32, L"ВЫПОЛНИТЬ", th.accent, true);
        y += 42;
        List(lbCmdOut, ex, y, ew, MY + MH - 16 - y);
    }

    void RunCommand() {
        const ListItem* sel = lbCmds.Selected();
        if (!sel) return;
        json::Value list = FirstArray(snap.commands);
        std::vector<std::wstring> fields;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].isObject() && list[i][L"command"].str() == sel->key)
                for (size_t k = 0; k < list[i][L"fields"].size(); ++k) fields.push_back(list[i][L"fields"][k].str());
        std::wstring args = sel->key;
        for (size_t k = 0; k < fields.size() && k < tbCmdArgs.size(); ++k) {
            std::wstring v = util::Trim(tbCmdArgs[k].text);
            if (v.empty()) continue;
            args += L" " + fields[k] + L"=" + (v.find(L' ') != std::wstring::npos ? L"\"" + v + L"\"" : v);
        }
        lbCmdOut.items.clear();
        lbCmdOut.emptyText = L"выполняю " + sel->key + L"…";
        ygg::Paths p = snap.paths;
        Async([p, args] {
            util::ProcResult r = util::Run(p.ctl, p.EndpointArg() + L"-json " + args, 30000);
            json::Value v;
            if (json::Parse(r.out, v)) return json::DumpW(v, true);
            return util::FromUtf8(r.out);
        }, [this](const std::wstring& res) {
            std::vector<ListItem> items;
            for (auto& l : TextLines(res)) { ListItem it; it.left = l; items.push_back(it); }
            lbCmdOut.items = items; lbCmdOut.sel = -1; lbCmdOut.scroll = 0;
            lbCmdOut.emptyText = L"пустой ответ";
            data.RefreshNow();
            Redraw();
        });
    }

    // ================== действия ==================
    void GoTo(int s) {
        screen = s;
        SetTb(nullptr);
        focusLb = nullptr;
        gridFocus = false;
        focusBtn = 0;
        if ((s == SCR_CONFIG || s == SCR_KEY || s == SCR_PEERS || s == SCR_IFACES) && !cfgLoaded) ReloadConfig();
        if (s == SCR_LOG) logShownFor = (size_t)-1;
        Redraw();
    }

    void OpenWatchdog() {
        if (snap.wdExe.empty()) return;
        ShellExecuteW(hwnd, L"open", snap.wdExe.c_str(), nullptr, util::DirOf(snap.wdExe).c_str(), SW_SHOWNORMAL);
    }

    void WatchdogTick() {
        if (snap.wdExe.empty()) return;
        std::wstring exe = snap.wdExe;
        Op(L"watchdog проверяет", [exe] {
            util::ProcResult r = util::Run(exe, L"tick", 10 * 60 * 1000);
            return r.started ? std::wstring() : std::wstring(L"!watchdog не запустился");
        }, L"Watchdog проверил связь.");
    }

    void OnButton(int id) {
        if (id >= NAV_FIRST && id < NAV_FIRST + SCR_COUNT) { navFocus = true; GoTo(id - NAV_FIRST); return; }
        const Theme th = Th();
        (void)th;
        switch (id) {
        case BTN_CONFIRM_YES: { confirmOpen = false; auto a = confirmAction; confirmAction = nullptr; if (a) a(); break; }
        case BTN_CONFIRM_NO: confirmOpen = false; confirmAction = nullptr; break;
        case BTN_WD_OPEN: OpenWatchdog(); break;
        case BTN_WD_TICK: WatchdogTick(); break;
        case BTN_WD_PICK: PickWatchdog(); break;
        case BTN_WD_INSTALL: WatchdogCmd(L"install-task", L"включаю автопроверку", L"Автопроверка включена: watchdog проверяет каждые 3 минуты."); break;
        case BTN_WD_REMOVE:
            Confirm(L"Выключить автопроверку watchdog? Задача YggWatchdogTick будет удалена из Планировщика - watchdog перестанет сам следить за пирами.",
                    L"ВЫКЛЮЧИТЬ", [this] { WatchdogCmd(L"uninstall-task", L"выключаю автопроверку", L"Автопроверка выключена."); }, COL_ALERT);
            break;
        case BTN_UPD_CHECK: CheckUpdate(false); break;
        case BTN_UPD_INSTALL: InstallUpdate(); break;
        case BTN_PUPD_CHECK: CheckPanelUpdate(false); break;
        case BTN_PUPD_OPEN: if (!pupd.page.empty()) ShellExecuteW(nullptr, L"open", pupd.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case BTN_REFRESH: data.RefreshNow(); if (screen == SCR_CONFIG && !cfgLoaded) ReloadConfig(); Flash(L"Обновляю…"); break;
        case BTN_START_SVC: ServiceAction(L"start", L"запускаю службу", L"Служба запущена."); break;
        case BTN_COPY_ADDR: util::CopyToClipboard(hwnd, SelfAddr()); Flash(L"Адрес скопирован."); break;
        case BTN_COPY_KEY: case BTN_KEY_COPY: util::CopyToClipboard(hwnd, SelfKey()); Flash(L"Публичный ключ скопирован."); break;

        // пиры
        case BTN_PEER_ADD:
            for (auto& c : apCheck) c = Check();
            tbApUri.Set(L""); tbApPass.Set(L""); tbApKey.Set(L"");
            GoTo(SCR_ADDPEER); SetTb(&tbApUri); break;
        case BTN_PEER_DEL: RemoveSelectedPeer(); break;
        case BTN_PEER_TOCFG: SelectedPeerToConfig(); break;
        case BTN_PEER_PING:
            for (size_t i = 0; i < snap.peers.size(); ++i)
                if (snap.peers[i][L"up"].boolean()) PingAsync(snap.peers[i][L"address"].str());
            Flash(L"Пингую пиров по их адресам Yggdrasil…");
            break;
        case BTN_AP_ADD: RunAddPeer(true); break;
        case BTN_AP_CHECK: RunAddPeer(false); break;
        case BTN_AP_CANCEL: GoTo(SCR_PEERS); break;

        // дерево
        case BTN_TREE_ZIN: TreeZoom(1.5f, D2D1::Point2F(TCX + TCW / 2, TCY + TCH / 2)); break;
        case BTN_TREE_ZOUT: TreeZoom(1 / 1.5f, D2D1::Point2F(TCX + TCW / 2, TCY + TCH / 2)); break;
        case BTN_TREE_FIT: tZoom = 1; tPanX = tPanY = 0; break;
        case BTN_TREE_PATH: tShowPath = !tShowPath; break;
        case BTN_TREE_PING: if (tSel >= 0) PingAsync(tnodes[tSel].address); break;
        case BTN_TREE_ASK: if (tSel >= 0) AskRemote(tnodes[tSel].key); break;
        case BTN_BK_SAVE: case BTN_BK_DEL: case BTN_BK_PING: case BTN_BK_PINGALL: case BTN_BK_COPY: BookAction(id); break;
        case BTN_HIST_ALL: histHidden.clear(); break;
        case BTN_HIST_NONE: for (const auto& u : histPeers) histHidden.insert(u); break;
        case BTN_HIST_RELOAD: histLoadedAt = 0; LoadHistory(); break;
        case BTN_SES_BOOK: if (gSessions.sel >= 0 && gSessions.sel < gSessions.count) ToBook(snap.sessions[gSessions.sel][L"address"].str()); break;
        case BTN_PATH_BOOK: if (auto* p = SelectedPath()) ToBook((*p)[L"address"].str()); break;
        case BTN_TREE_BOOK: if (tSel >= 0 && tSel != tSelf) ToBook(tnodes[tSel].address); break;
        case BTN_TREE_COPY: if (tSel >= 0) { util::CopyToClipboard(hwnd, tnodes[tSel].address); Flash(L"Адрес скопирован."); } break;

        // сессии
        case BTN_SES_PING: if (gSessions.sel >= 0 && gSessions.sel < gSessions.count) PingAsync(snap.sessions[gSessions.sel][L"address"].str()); break;
        case BTN_SES_ASK: if (gSessions.sel >= 0 && gSessions.sel < gSessions.count) AskRemote(snap.sessions[gSessions.sel][L"key"].str()); break;
        case BTN_SES_COPY: if (gSessions.sel >= 0 && gSessions.sel < gSessions.count) { util::CopyToClipboard(hwnd, snap.sessions[gSessions.sel][L"address"].str()); Flash(L"Адрес скопирован."); } break;

        // пути
        case BTN_PATH_PING: if (auto* p = SelectedPath()) PingAsync((*p)[L"address"].str()); break;
        case BTN_PATH_ASK: if (auto* p = SelectedPath()) AskRemote((*p)[L"key"].str()); break;
        case BTN_PATH_COPY: if (auto* p = SelectedPath()) { util::CopyToClipboard(hwnd, (*p)[L"address"].str()); Flash(L"Адрес скопирован."); } break;

        // удалённый узел
        case BTN_RM_NODEINFO: RunRemote(L"getNodeInfo", L"NODEINFO  //  getNodeInfo"); break;
        case BTN_RM_SELF: RunRemote(L"debug_remoteGetSelf", L"СВЕДЕНИЯ  //  debug_remoteGetSelf"); break;
        case BTN_RM_PEERS: RunRemote(L"debug_remoteGetPeers", L"ЕГО ПИРЫ  //  debug_remoteGetPeers"); break;
        case BTN_RM_TREE: RunRemote(L"debug_remoteGetTree", L"ЕГО ДЕРЕВО  //  debug_remoteGetTree"); break;
        case BTN_RM_USE: if (const ListItem* s = lbRemoteOut.Selected()) if (IsHexKey(s->key)) { tbRemoteKey.Set(s->key); RunRemote(L"getNodeInfo", L"NODEINFO  //  getNodeInfo"); } break;

        // интерфейсы
        case BTN_IF_CFG: GoTo(SCR_CONFIG); SelectCfgKey(L"MulticastInterfaces"); break;
        case BTN_IF_LISTEN: GoTo(SCR_CONFIG); SelectCfgKey(L"Listen"); break;

        // служба
        case BTN_SV_START: ServiceAction(L"start", L"запускаю службу", L"Служба запущена."); break;
        case BTN_SV_STOP:
            Confirm(L"Остановить Yggdrasil? Все соединения через Yggdrasil оборвутся, пока не запустишь снова.", L"ОСТАНОВИТЬ",
                    [this] { ServiceAction(L"stop", L"останавливаю службу", L"Служба остановлена."); }, COL_ALERT);
            break;
        case BTN_SV_RESTART:
            Confirm(L"Перезапустить Yggdrasil сейчас? Соединения через Yggdrasil оборвутся на несколько секунд. "
                    L"Если ты подключён к этому компьютеру через сам Yggdrasil - выбери «через 15 с».", L"ПЕРЕЗАПУСТИТЬ",
                    [this] { ServiceAction(L"restart", L"перезапускаю службу", L"Служба перезапущена."); });
            break;
        case BTN_SV_LATER: RestartLater(); break;
        case BTN_SV_AUTO: case BTN_SV_MANUAL: case BTN_SV_DISABLED: {
            DWORD t = id == BTN_SV_AUTO ? SERVICE_AUTO_START : id == BTN_SV_MANUAL ? SERVICE_DEMAND_START : SERVICE_DISABLED;
            auto apply = [this, t] {
                Op(L"меняю тип запуска", [t] { std::wstring e = util::SetServiceStartType(ygg::SERVICE_NAME, t); return e.empty() ? std::wstring() : L"!" + e; },
                   L"Тип запуска службы изменён.");
            };
            if (t == SERVICE_DISABLED) Confirm(L"Отключить службу? Yggdrasil не запустится ни сам, ни вручную, пока не вернёшь тип запуска.", L"ОТКЛЮЧИТЬ", apply, COL_ALERT);
            else apply();
            break;
        }
        case BTN_SV_CONFDIR: ShellExecuteW(hwnd, L"open", util::DirOf(snap.paths.conf).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case BTN_SV_EXEDIR: ShellExecuteW(hwnd, L"open", util::DirOf(snap.paths.exe).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case BTN_SV_REREAD: data.ReloadPaths(); ReloadConfig(); Flash(L"Перечитываю пути и конфиг…"); break;

        // лог
        case BTN_LOG_FOLLOW: logFollow = !logFollow; logShownFor = (size_t)-1; break;
        case BTN_LOG_NOTEPAD: if (!snap.paths.log.empty()) ShellExecuteW(hwnd, L"open", L"notepad.exe", (L"\"" + snap.paths.log + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
        case BTN_LOG_DIR: if (!snap.paths.log.empty()) ShellExecuteW(hwnd, L"open", util::DirOf(snap.paths.log).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;

        // конфиг
        case BTN_CF_ADD: case BTN_CF_REPLACE: case BTN_CF_DEL: case BTN_CF_SET: case BTN_CF_TRUE: case BTN_CF_FALSE:
            CfgEditAction(id); break;
        case BTN_CF_APPLY: SaveConfigUI(true); break;
        case BTN_CF_SAVE: SaveConfigUI(false); break;
        case BTN_CF_REVERT: cfgEdit = json::Clone(cfgOrig); cfgFilledFor.clear(); Flash(L"Правки отменены."); break;

        // ключ
        case BTN_KEY_EXPORT: ExportKey(); break;
        case BTN_KEY_NEW: NewKey(); break;

        case BTN_CMD_RUN: RunCommand(); break;
        default:
            if (id >= BTN_AP_PROTO && id < BTN_AP_PROTO + 5) {
                static const wchar_t* protos[] = { L"tcp", L"tls", L"quic", L"ws", L"wss" };
                SetScheme(protos[id - BTN_AP_PROTO]);
            } else if (id >= BTN_AP_WHERE && id < BTN_AP_WHERE + 3) apWhere = id - BTN_AP_WHERE;
            else if (id >= BTN_PATH_SORT && id < BTN_PATH_SORT + 4) { pathSort = id - BTN_PATH_SORT; gPaths.sel = -1; }
            else if (id >= BTN_HIST_RANGE && id < BTN_HIST_RANGE + 3) histRange = id - BTN_HIST_RANGE;
            else if (id >= BTN_LOG_LEVEL && id < BTN_LOG_LEVEL + 4) { logLevel = id - BTN_LOG_LEVEL; logShownFor = (size_t)-1; }
        }
        Redraw();
    }

    void SelectCfgKey(const std::wstring& k) {
        if (!cfgLoaded || !cfgEdit.obj) return;
        if (!cfgEdit.obj->count(k)) (*cfgEdit.obj)[k] = json::MakeArray();
        auto keys = CfgKeys();
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) {
            std::vector<ListItem> items;
            for (auto& kk : keys) { ListItem it; it.left = it.key = kk; items.push_back(it); }
            lbCfgKeys.items = items; lbCfgKeys.sel = (int)i;
        }
    }

    // ---------- ввод ----------
    D2D1_POINT_2F Pt(LPARAM lp) { return r.ToDesign((short)LOWORD(lp), (short)HIWORD(lp)); }
    int HitAt(LPARAM lp) {
        D2D1_POINT_2F p = Pt(lp);
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) if (it->contains(p)) return it->id;
        return 0;
    }

    std::vector<TextBox*> ScreenFields() {
        switch (screen) {
            case SCR_ADDPEER: return { &tbApUri, &tbApPass, &tbApKey };
            case SCR_REMOTE: return { &tbRemoteKey };
            case SCR_LOG: return { &tbLogSearch };
            case SCR_CONFIG: {
                if (!cfgLoaded || cfgSelKey.empty()) return {};
                const json::Value& V = cfgEdit[cfgSelKey];
                if (V.isObject()) return { &tbCfgKey, &tbCfgItem };
                if (V.type == json::Value::Bool) return {};
                return { &tbCfgItem };
            }
            case SCR_BOOK: return { &tbBkName, &tbBkAddr };
            case SCR_COMMANDS: {
                std::vector<TextBox*> v;
                for (auto& t : tbCmdArgs) if (t.w > 0 && lbCmds.Selected()) v.push_back(&t);
                return v;
            }
            default: return {};
        }
    }
    std::vector<ListBox*> ScreenLists() {
        switch (screen) {
            case SCR_REMOTE: return { &lbRemoteOut };
            case SCR_IFACES: return { &lbIfMulticast, &lbIfListen };
            case SCR_LOG: return { &lbLog };
            case SCR_CONFIG: return { &lbCfgKeys, &lbCfgItems };
            case SCR_COMMANDS: return { &lbCmds, &lbCmdOut };
            default: return {};
        }
    }
    Grid* ScreenGrid() {
        if (screen == SCR_PEERS) return &gPeers;
        if (screen == SCR_SESSIONS) return &gSessions;
        if (screen == SCR_PATHS) return &gPaths;
        if (screen == SCR_BOOK) return &gBook;
        if (screen == SCR_HISTORY) return &gHist;
        return nullptr;
    }

    void OnMouseDown(LPARAM lp) {
        pressId = HitAt(lp);
        focusBtn = 0;
        if (pressId || confirmOpen) return;
        D2D1_POINT_2F p = Pt(lp);
        for (TextBox* t : ScreenFields()) if (t->Hit().contains(p)) { SetTb(t); navFocus = gridFocus = false; t->caret = t->text.size(); return; }
        SetTb(nullptr);
        for (ListBox* l : ScreenLists()) if (l->ClickAt(p)) { focusLb = l; navFocus = gridFocus = false; if (l == &lbLog) logFollow = false; return; }
        if (screen == SCR_HISTORY && gHist.Inside(p) && p.x < gHist.x + 30) {      // галочка
            int i = gHist.scroll + (int)((p.y - gHist.y) / gHist.rowH);
            if (i >= 0 && i < (int)histPeers.size()) {
                ToggleHist(i); gHist.sel = i;
                focusLb = nullptr; navFocus = false; gridFocus = true; Redraw(); return;
            }
        }
        if (Grid* g = ScreenGrid()) if (g->Click(p)) { focusLb = nullptr; navFocus = false; gridFocus = true; return; }
        if (screen == SCR_TREE && p.x >= TCX && p.x <= TCX + TCW && p.y >= TCY + 62 && p.y <= TCY + TCH) {
            tDragging = true; tDragMoved = false; tDragStart = p; tPanStartX = tPanX; tPanStartY = tPanY;
        }
    }

    void OnMouseMove(LPARAM lp) {
        D2D1_POINT_2F p = Pt(lp);
        mouse = p;
        if (tDragging) {
            float dx = p.x - tDragStart.x, dy = p.y - tDragStart.y;
            if (fabsf(dx) + fabsf(dy) > 4) tDragMoved = true;
            if (tDragMoved) { tPanX = tPanStartX + dx; tPanY = tPanStartY + dy; Redraw(); }
            return;
        }
        int id = HitAt(lp);
        if (id != hoverId) { hoverId = id; Redraw(); }
        SetCursor(LoadCursor(nullptr, id ? IDC_HAND : IDC_ARROW));
    }

    void OnMouseUp(LPARAM lp) {
        if (tDragging) {
            tDragging = false;
            if (!tDragMoved) { tSel = TreeHit(Pt(lp)); }
            Redraw();
            return;
        }
        int id = HitAt(lp), pressed = pressId;
        pressId = 0;
        if (id && id == pressed) OnButton(id);
        Redraw();
    }

    void OnWheel(WPARAM wp, LPARAM lp) {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        D2D1_POINT_2F p = r.ToDesign(pt.x, pt.y);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (screen == SCR_TREE && p.x >= TCX && p.x <= TCX + TCW && p.y >= TCY && p.y <= TCY + TCH) {
            TreeZoom(delta > 0 ? 1.2f : 1 / 1.2f, p);
            return;
        }
        for (ListBox* l : ScreenLists())
            if (p.x >= l->x && p.x <= l->x + l->w && p.y >= l->y && p.y <= l->y + l->h) {
                l->Wheel(delta);
                if (l == &lbLog) logFollow = false;
                Redraw(); return;
            }
        if (Grid* g = ScreenGrid()) if (g->Inside(p)) { g->Wheel(delta); Redraw(); }
    }

    void SubmitField(TextBox* t) {
        if (screen == SCR_ADDPEER) { RunAddPeer(true); return; }
        if (t == &tbRemoteKey) { RunRemote(L"getNodeInfo", L"NODEINFO  //  getNodeInfo"); return; }
        if (screen == SCR_CONFIG) {
            const json::Value& V = cfgEdit[cfgSelKey];
            if (V.isArray()) OnButton(lbCfgItems.Selected() ? BTN_CF_REPLACE : BTN_CF_ADD);
            else OnButton(BTN_CF_SET);
            return;
        }
        if (screen == SCR_COMMANDS) { RunCommand(); return; }
        if (screen == SCR_BOOK) {
            if (t == &tbBkName) SetTb(&tbBkAddr);
            else BookAction(BTN_BK_SAVE);
            return;
        }
    }

    void OnKey(WPARAM vk) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (confirmOpen) {
            if (vk == VK_RETURN) OnButton(BTN_CONFIRM_YES);
            else if (vk == VK_ESCAPE) OnButton(BTN_CONFIRM_NO);
            return;
        }
        if (vk == VK_TAB) { CycleFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0); return; }
        if (focusBtn && !focusTb && (vk == VK_RETURN || vk == VK_SPACE)) {
            int id = focusBtn;
            if (std::find(frameButtons.begin(), frameButtons.end(), id) != frameButtons.end()) OnButton(id);
            return;
        }
        if (focusTb) {
            if (vk == VK_ESCAPE) { SetTb(nullptr); navFocus = true; }
            else if (vk == VK_RETURN) SubmitField(focusTb);
            else focusTb->OnKey(vk, ctrl);
            Redraw();
            return;
        }
        int cur = screen == SCR_ADDPEER ? SCR_PEERS : screen;
        if (navFocus && (vk == VK_UP || vk == VK_DOWN)) {
            GoTo((cur + (vk == VK_UP ? SCR_COUNT - 1 : 1)) % SCR_COUNT);
            return;
        }
        if (vk == 'W') { OpenWatchdog(); return; }
        if (vk == 'T') { WatchdogTick(); return; }
        if (vk == VK_SPACE && screen == SCR_HISTORY && gridFocus && gHist.sel >= 0) { ToggleHist(gHist.sel); Redraw(); return; }
        if (vk == VK_SPACE && screen == SCR_LOG) { logFollow = !logFollow; logShownFor = (size_t)-1; Redraw(); return; }
        if (vk == VK_ESCAPE) {
            if (screen == SCR_ADDPEER) GoTo(SCR_PEERS);
            else if (!navFocus) { navFocus = true; focusLb = nullptr; gridFocus = false; focusBtn = 0; Redraw(); }
            else ShowWindow(hwnd, SW_MINIMIZE);
            return;
        }
        if (vk == VK_DELETE && screen == SCR_PEERS) { RemoveSelectedPeer(); return; }
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT) {
            if (focusLb) {
                int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -focusLb->Visible() : focusLb->Visible();
                focusLb->Move(d);
            } else if (Grid* g = ScreenGrid()) {
                int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -g->Visible() : g->Visible();
                g->Move(d);
            }
            Redraw();
        }
    }

    // Tab: команды слева -> поля ввода -> списки -> таблица экрана -> снова команды
    // (Shift+Tab - в обратную сторону). Стрелки работают там, где фокус.
    void CycleFocus(bool back) {
        struct Slot { int kind; void* p; };   // 0 команды, 1 поле, 2 список, 3 таблица
        std::vector<Slot> ring = { { 0, nullptr } };
        for (TextBox* t : ScreenFields()) if (t->w > 0) ring.push_back({ 1, t });
        for (ListBox* l : ScreenLists()) if (l->w > 0) ring.push_back({ 2, l });
        if (Grid* g = ScreenGrid()) if (g->count > 0) ring.push_back({ 3, g });
        for (int id : frameButtons) ring.push_back({ 4, (void*)(intptr_t)id });
        int cur = 0;
        for (size_t i = 0; i < ring.size(); ++i)
            if ((ring[i].kind == 1 && ring[i].p == focusTb) || (ring[i].kind == 2 && ring[i].p == focusLb && !focusTb) ||
                (ring[i].kind == 3 && gridFocus && !focusTb && !focusLb) ||
                (ring[i].kind == 4 && focusBtn && (int)(intptr_t)ring[i].p == focusBtn)) cur = (int)i;
        if (navFocus) cur = 0;
        int n = (int)ring.size();
        Slot s = ring[(cur + (back ? n - 1 : 1)) % n];
        navFocus = s.kind == 0;
        focusBtn = s.kind == 4 ? (int)(intptr_t)s.p : 0;
        SetTb(s.kind == 1 ? (TextBox*)s.p : nullptr);
        focusLb = s.kind == 2 ? (ListBox*)s.p : nullptr;
        gridFocus = s.kind == 3;
        if (gridFocus) { Grid* g = (Grid*)s.p; if (g->sel < 0) g->Move(1); }
        if (focusLb && focusLb->sel < 0 && !focusLb->items.empty()) focusLb->Move(1);
        if (focusTb) focusTb->caret = focusTb->text.size();
        Redraw();
    }

    void OnChar(wchar_t c) {
        if (c == L'\t') return;   // Tab обрабатывается в OnKey
        if (!focusTb || confirmOpen) return;
        focusTb->OnChar(c);
        Redraw();
    }
};

App g_app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& a = g_app;
    switch (msg) {
    case WM_CREATE:
        a.hwnd = hwnd;
        a.r.designW = DW; a.r.designH = DH;
        a.r.Init(hwnd);
        a.r.bgColor = LoadColorPngResource(101);   // фон-древо, вшит в exe
        a.data.Start(hwnd);
        a.ReloadConfig();
        a.CheckUpdate(true);
        a.CheckPanelUpdate(true);
        util::RegWriteString(ygg::REG_CONTROL, L"Path", util::SelfExe());   // чтобы сторож мог найти панель
        SetTimer(hwnd, TIMER_CLOCK, 1000, nullptr);
        SetTimer(hwnd, TIMER_BLINK, 500, nullptr);
        return 0;
    case WM_SIZE: a.r.Resize(); a.Redraw(); return 0;
    case WM_DPICHANGED: {
        RECT* rc = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        a.r.Resize();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_BLINK && !a.focusTb) return 0;   // курсор в поле мигает
        if (!IsIconic(hwnd)) a.Redraw();
        return 0;
    case ygg::WM_APP_DATA:
        a.snap = a.data.Get();
        a.Redraw();
        return 0;
    case WM_APP_TASK: {
        AsyncTask* t = (AsyncTask*)lp;
        if (t->done) t->done(t->result);
        delete t;
        a.Redraw();
        return 0;
    }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); a.Paint(); EndPaint(hwnd, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: a.OnMouseMove(lp); return 0;
    case WM_LBUTTONDOWN: a.OnMouseDown(lp); SetCapture(hwnd); a.Redraw(); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); a.OnMouseUp(lp); return 0;
    case WM_MOUSEWHEEL: a.OnWheel(wp, lp); return 0;
    case WM_KEYDOWN:
        a.OnKey(wp);
        return 0;
    case WM_CHAR: a.OnChar((wchar_t)wp); return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) return TRUE; break;
    case WM_DESTROY:
        a.data.Stop();
        a.r.DiscardTarget();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool IsElevated() {
    BOOL isAdmin = FALSE;
    PSID g = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g)) {
        CheckTokenMembership(nullptr, g, &isAdmin);
        FreeSid(g);
    }
    return isAdmin == TRUE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Служба, конфиг в ProgramData и yggdrasilctl - только с правами администратора
    if (!IsElevated()) {
        ShellExecuteW(nullptr, L"runas", util::SelfExe().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"YggControlSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND ex = FindWindowW(L"YggControl", nullptr);
        if (ex) { ShowWindow(ex, SW_RESTORE); SetForegroundWindow(ex); }
        return 0;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"YggControl";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT rc = { 0, 0, MulDiv((int)DW, dpi, 96), MulDiv((int)DH, dpi, 96) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ygg-control", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (mutex) CloseHandle(mutex);
    return 0;
}
