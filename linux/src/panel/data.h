// data.h - всё, что знает панель: снимок узла (yggdrasilctl -json), служба,
// трафик, сторож. Собирается в фоновом потоке раз в 3 секунды.
#pragma once
#include "../common/ygg.h"
#include "version.h"
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace pnl {

// ------------------------------------------------------------------ сторож (ygg-watchdog)
// Где он: путь, который он сам записал при установке, затем команда в PATH,
// затем ~/ygg-watchdog того, кто запустил sudo.
inline std::string FindWatchdog() {
    std::string p = util::Trim(util::ReadFile("/etc/yozmor/watchdog_path"));
    if (!p.empty() && access(p.c_str(), X_OK) == 0) return p;
    std::string w = util::Which("ygg-watchdog");
    if (!w.empty()) {
        char buf[4096];
        if (realpath(w.c_str(), buf)) return buf;
        return w;
    }
    const char* su = getenv("SUDO_USER");
    std::string home = su && *su ? "/home/" + std::string(su) : (getenv("HOME") ? getenv("HOME") : "");
    std::string guess = home + "/ygg-watchdog/ygg-watchdog";
    if (!home.empty() && access(guess.c_str(), X_OK) == 0) return guess;
    return "";
}

// ------------------------------------------------------------------ снимок
struct TrafficPoint { long long t = 0; double rx = 0, tx = 0; };   // байт/с

struct Snap {
    ygg::Paths paths;
    std::string version;                 // yggdrasil -version
    bool ctlOk = false;
    std::string ctlErr;
    json::Value self, peers, sessions, paths_, tree, tun, multicast, commands;
    long long treeAt = 0;
    ygg::ServiceInfo svc;
    std::deque<TrafficPoint> traffic;    // 60 минут
    double totalRx = 0, totalTx = 0;
    std::map<std::string, std::deque<float>> sessRate;   // ключ сессии -> скорость (байт/с)
    std::string tunIfName;               // имя TUN из getTUN или ip
    std::vector<std::string> tunAddrs;   // адреса на интерфейсе (ip -6 addr)
    // сторож
    std::string wdExe;
    bool wdOk = false;
    json::Value wd;                      // status-json сторожа
    std::vector<std::string> wdEvents;   // "unix\tуровень\tтекст"
    long long at = 0;
};

class Data {
public:
    std::mutex mu;
    Snap snap;
    std::atomic<bool> stop{ false }, wake{ false }, reloadPaths{ true };
    std::thread th;

    void Start() { th = std::thread([this] { Loop(); }); }
    void Stop() { stop = true; if (th.joinable()) th.join(); }
    void RefreshNow() { wake = true; }
    void ReloadPaths() { reloadPaths = true; wake = true; }
    Snap Get() { std::lock_guard<std::mutex> lk(mu); return snap; }

private:
    void Loop() {
        Snap s;
        int tick = 0;
        double lastRx = -1, lastTx = -1;
        long long lastT = 0;
        std::map<std::string, std::pair<double, long long>> lastSess;
        while (!stop) {
            long long now = util::Now();
            s.at = now;
            if (reloadPaths.exchange(false)) {
                s.paths = ygg::CtlPaths();
                s.version = ygg::InstalledVersion(s.paths);
            }
            s.svc = ygg::QueryService();
            json::Value v;
            std::string err;
            s.ctlOk = ygg::Ctl("getSelf", v, err, 6000);
            s.ctlErr = s.ctlOk ? "" : err;
            if (s.ctlOk) {
                s.self = v;
                if (ygg::Ctl("getPeers", v, err, 8000)) s.peers = v["peers"];
                if (ygg::Ctl("getSessions", v, err, 8000)) s.sessions = v["sessions"];
                if (tick % 2 == 0) {
                    if (ygg::Ctl("getPaths", v, err, 8000)) s.paths_ = v["paths"];
                    if (ygg::Ctl("getTree", v, err, 8000)) { s.tree = v["tree"]; s.treeAt = now; }
                }
                if (tick % 5 == 0) {
                    s.multicast = ygg::Ctl("getMulticastInterfaces", v, err, 6000) ? v : json::Value();
                    s.tun = ygg::Ctl("getTUN", v, err, 6000) ? v : json::Value();
                    if (ygg::Ctl("list", v, err, 6000)) s.commands = v;
                    s.tunIfName = s.tun["name"].str();
                    s.tunAddrs.clear();
                    if (!s.tunIfName.empty()) {
                        util::ProcResult r = util::Run({ "ip", "-6", "-o", "addr", "show", "dev", s.tunIfName }, 4000);
                        for (const auto& l : util::Lines(r.out)) {
                            size_t a = l.find("inet6 ");
                            if (a == std::string::npos) continue;
                            size_t e = l.find(' ', a + 6);
                            s.tunAddrs.push_back(l.substr(a + 6, e == std::string::npos ? std::string::npos : e - a - 6));
                        }
                    }
                }
                // трафик: сумма по пирам, скорость = разница за время между опросами
                double rx = 0, tx = 0;
                for (size_t i = 0; i < s.peers.size(); ++i) { rx += s.peers[i]["bytes_recvd"].num(); tx += s.peers[i]["bytes_sent"].num(); }
                s.totalRx = rx; s.totalTx = tx;
                if (lastT > 0 && now > lastT && rx >= lastRx && tx >= lastTx) {
                    TrafficPoint p; p.t = now;
                    p.rx = (rx - lastRx) / (double)(now - lastT);
                    p.tx = (tx - lastTx) / (double)(now - lastT);
                    s.traffic.push_back(p);
                    while (!s.traffic.empty() && s.traffic.front().t < now - 3600) s.traffic.pop_front();
                }
                lastRx = rx; lastTx = tx; lastT = now;
                std::map<std::string, std::pair<double, long long>> cur;
                for (size_t i = 0; i < s.sessions.size(); ++i) {
                    std::string k = s.sessions[i]["key"].str();
                    double b = s.sessions[i]["bytes_recvd"].num() + s.sessions[i]["bytes_sent"].num();
                    cur[k] = { b, now };
                    auto it = lastSess.find(k);
                    if (it != lastSess.end() && now > it->second.second && b >= it->second.first) {
                        auto& h = s.sessRate[k];
                        h.push_back((float)((b - it->second.first) / (double)(now - it->second.second)));
                        while (h.size() > 60) h.pop_front();
                    }
                }
                for (auto it = s.sessRate.begin(); it != s.sessRate.end();) it = cur.count(it->first) ? std::next(it) : s.sessRate.erase(it);
                lastSess = cur;
            }
            // сторож - раз в 6 секунд
            if (tick % 2 == 0) {
                s.wdExe = FindWatchdog();
                s.wdOk = false;
                if (!s.wdExe.empty()) {
                    util::ProcResult r = util::Run({ s.wdExe, "status-json" }, 15000);
                    json::Value st;
                    if (json::ParseLoose(r.out, st) && st.isObject()) { s.wd = st; s.wdOk = true; }
                    s.wdEvents.clear();
                    for (const auto& l : util::Lines(util::ReadFileTail(util::DirOf(s.wdExe) + "events.log", 64 * 1024)))
                        if (!util::Trim(l).empty()) s.wdEvents.push_back(l);
                    if (s.wdEvents.size() > 300) s.wdEvents.erase(s.wdEvents.begin(), s.wdEvents.end() - 300);
                }
            }
            { std::lock_guard<std::mutex> lk(mu); snap = s; }
            ++tick;
            for (int i = 0; i < 30 && !stop && !wake; ++i) util::SleepMs(100);
            wake = false;
        }
    }
};

// ------------------------------------------------------------------ мелочи
// Короткое имя пира из адреса: tls://ygg-msk-1.example.ru:443 -> ygg-msk-1
inline std::string ShortName(const std::string& uri) {
    util::PeerUri u = util::ParseUri(uri);
    std::string h = u.ok ? u.host : uri;
    if (h.find(':') != std::string::npos) return h;                    // IPv6
    auto parts = util::Split(h, '.');
    if (parts.empty()) return h;
    bool numeric = !parts[0].empty() && isdigit((unsigned char)parts[0][0]);
    if (numeric) return h;
    std::string s = parts[0];
    for (const char* g : { "ygg", "yggdrasil", "peer", "peers", "node", "public", "y" })
        if (util::IEquals(s, g) && parts.size() > 2) { s = parts[1]; break; }
    return util::U8Left(s, 18);
}
inline std::string ShortKey(const std::string& k) { return k.size() > 12 ? k.substr(0, 4) + "…" + k.substr(k.size() - 4) : k; }
inline std::string ShortAddr(const std::string& a, size_t n = 22) {
    if (util::U8Len(a) <= n) return a;
    return a.substr(0, n - 6) + "…" + a.substr(a.size() - 5);
}
inline bool IsHexKey(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
inline std::string NormAddr(const std::string& a) {
    in6_addr x;
    if (inet_pton(AF_INET6, util::Trim(a).c_str(), &x) == 1) {
        char b[64];
        inet_ntop(AF_INET6, &x, b, sizeof(b));
        return b;
    }
    return util::Lower(util::Trim(a));
}
inline std::string Bytes(double b) {
    if (b < 0) return "—";
    const char* u[] = { "Б", "КБ", "МБ", "ГБ", "ТБ" };
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; ++i; }
    return (i == 0 ? std::to_string((long long)b) : util::FixedRu(b, b < 10 ? 1 : 0)) + " " + u[i];
}
inline std::string Rate(double b) { return b < 0 ? "—" : Bytes(b) + "/с"; }
inline std::string Dur(double sec) {
    if (sec < 0) return "—";
    long long s = (long long)sec;
    if (s < 60) return std::to_string(s) + " с";
    if (s < 3600) return std::to_string(s / 60) + " мин";
    if (s < 86400) return std::to_string(s / 3600) + " ч " + std::to_string(s % 3600 / 60) + " мин";
    return std::to_string(s / 86400) + " д " + std::to_string(s % 86400 / 3600) + " ч";
}
// Коротко для таблиц: «1 ч 30 м», «2 д 3 ч»
inline std::string DurShort(double sec) {
    if (sec < 0) return "—";
    long long s = (long long)sec;
    if (s < 60) return std::to_string(s) + " с";
    if (s < 3600) return std::to_string(s / 60) + " мин";
    if (s < 86400) return std::to_string(s / 3600) + " ч " + std::to_string(s % 3600 / 60) + " м";
    return std::to_string(s / 86400) + " д " + std::to_string(s % 86400 / 3600) + " ч";
}
// Первый массив в ответе (ответы разных версий обёрнуты по-разному)
inline json::Value FirstArray(const json::Value& v) {
    if (v.isArray()) return v;
    if (v.isObject()) for (const auto& kv : v.obj) if (kv.second.isArray()) return kv.second;
    return json::Value();
}
inline bool HasCommand(const Snap& s, const std::string& name) {
    json::Value list = FirstArray(s.commands);
    if (list.size() == 0) return true;   // список неизвестен - не мешаем
    for (size_t i = 0; i < list.size(); ++i) {
        std::string c = list[i].isObject() ? list[i]["command"].str() : list[i].str();
        if (util::IEquals(c, name)) return true;
    }
    return false;
}
// Глубина узла в дереве (шагов до корня)
inline int RootDepth(const Snap& s, const std::string& key) {
    std::map<std::string, std::string> parent;
    for (size_t i = 0; i < s.tree.size(); ++i) parent[s.tree[i]["key"].str()] = s.tree[i]["parent"].str();
    std::string k = key;
    for (int d = 0; d < 256; ++d) {
        auto it = parent.find(k);
        if (it == parent.end()) return -1;
        if (it->second == k) return d;
        k = it->second;
    }
    return -1;
}
inline int PeersUp(const Snap& s) { int n = 0; for (size_t i = 0; i < s.peers.size(); ++i) if (s.peers[i]["up"].boolean()) ++n; return n; }
inline std::string PeerNameByKey(const Snap& s, const std::string& key) {
    for (size_t i = 0; i < s.peers.size(); ++i) if (s.peers[i]["key"].str() == key) return ShortName(s.peers[i]["remote"].str());
    return "";
}
inline std::string PeerNameByPort(const Snap& s, long long port) {
    for (size_t i = 0; i < s.peers.size(); ++i) if (s.peers[i]["port"].i64(-1) == port) return ShortName(s.peers[i]["remote"].str());
    return "порт " + std::to_string(port);
}

// ------------------------------------------------------------------ адресная книга
// addressbook.txt рядом с панелью: имя<TAB>адрес. Устройства, которые
// проверяет сторож, показываются тоже (пометка «сторож»).
struct BookEntry { std::string name, addr; bool fromWd = false; };
inline std::string BookPath() { return util::SelfDir() + "addressbook.txt"; }
inline std::vector<BookEntry> LoadBook() {
    std::vector<BookEntry> out;
    for (const auto& l : util::Lines(util::ReadFile(BookPath()))) {
        std::string t = util::Trim(l);
        if (t.empty() || t[0] == '#') continue;
        auto f = util::SplitKeep(l, '\t');
        if (f.size() < 2) continue;
        BookEntry e{ util::Trim(f[0]), util::Trim(f[1]), false };
        if (!e.name.empty() && !e.addr.empty()) out.push_back(e);
    }
    return out;
}
inline bool SaveBook(const std::vector<BookEntry>& b) {
    std::string out = "# адресная книга ygg-control: имя<TAB>адрес\n";
    for (const auto& e : b) if (!e.fromWd) out += e.name + "\t" + e.addr + "\n";
    return util::WriteFileAtomic(BookPath(), out);
}

// ------------------------------------------------------------------ обновление самой панели
// Релизов нет: версия - PANEL_VERSION в linux/src/panel/version.h репозитория
// Yozmor/ygg-control, программа - linux/ygg-control. Скачанный файл
// сверяется с git-отпечатком и запускается с --selftest: должен назвать
// ту версию, что в исходнике.
static const char* PANEL_REPO_API = "https://api.github.com/repos/Yozmor/ygg-control/contents/";
static const char* PANEL_REPO_PAGE = "https://github.com/Yozmor/ygg-control";
struct PanelUpdate { bool checked = false, ok = false; std::string tag, err; long long at = 0; };

struct RepoFile { bool ok = false; std::string url, sha, err; long long size = -1; };
inline RepoFile GetRepoFile(const std::string& path) {
    RepoFile f;
    net::HttpResult r = net::Get(PANEL_REPO_API + path, net::RequestOpts(), "Accept: application/vnd.github+json\r\n");
    json::Value v;
    if (!r.transportOk) { f.err = "GitHub недоступен"; return f; }
    if (r.status == 404) { f.err = "в репозитории нет " + path; return f; }
    if (r.status != 200 || !json::Parse(r.body, v) || v["download_url"].str().empty()) { f.err = "GitHub ответил кодом " + std::to_string(r.status); return f; }
    f.ok = true;
    f.url = v["download_url"].str();
    f.sha = util::Lower(v["sha"].str());
    f.size = v["size"].i64(-1);
    return f;
}
inline PanelUpdate CheckPanelUpdate() {
    PanelUpdate u;
    u.checked = true;
    u.at = util::Now();
    RepoFile f = GetRepoFile("linux/src/panel/version.h");
    if (!f.ok) { u.err = f.err; return u; }
    net::HttpResult r = net::Get(f.url);
    size_t k = r.body.find("PANEL_VERSION");
    size_t q1 = k == std::string::npos ? k : r.body.find('"', k);
    size_t q2 = q1 == std::string::npos ? q1 : r.body.find('"', q1 + 1);
    if (!r.transportOk || q2 == std::string::npos || q2 - q1 > 20) { u.err = "не удалось прочитать версию в репозитории"; return u; }
    u.ok = true;
    u.tag = r.body.substr(q1 + 1, q2 - q1 - 1);
    return u;
}
inline std::string SelfTestVersion(const std::string& exe) {
    util::ProcResult r = util::Run({ exe, "--selftest" }, 30000);
    if (!r.started || r.timedOut || r.code != 0) return "";
    return util::Trim(r.out);
}
// Скачать, проверить и поставить новую панель. Пусто - получилось.
// Работающий файл не перезаписывается, а заменяется переименованием -
// эта копия панели доработает на старом, следующий запуск возьмёт новый.
inline std::string ApplyPanelUpdate(const std::string& expect) {
    std::string self = util::SelfExe(), fresh = self + ".new", backup = self + ".backup";
    RepoFile f = GetRepoFile("linux/ygg-control");
    if (!f.ok) return f.err;
    net::RequestOpts o;
    o.maxBody = 200 * 1024 * 1024;
    o.timeoutMs = 120000;
    net::HttpResult r = net::Get(f.url, o);
    if (!r.transportOk || r.status != 200 || !r.complete) return "файл не скачался";
    if (f.size >= 0 && (long long)r.body.size() != f.size) return "файл скачался не целиком";
    if (f.sha.size() == 40 && util::GitBlobSha1(r.body) != f.sha) return "отпечаток скачанного файла не совпал с репозиторием";
    if (!util::LooksLikeElf(r.body)) return "скачанный файл не похож на программу Linux";
    if (!util::WriteFile(fresh, r.body, 0755)) return "не удалось записать " + fresh + " (нужен root)";
    chmod(fresh.c_str(), 0755);
    std::string got = SelfTestVersion(fresh);
    if (got != expect) {
        unlink(fresh.c_str());
        return got.empty() ? "скачанная панель не запускается" : "в репозитории номер версии " + expect + ", а linux/ygg-control внутри " + got +
                                                                    " - похоже, свежую сборку забыли закоммитить";
    }
    util::CopyFile(self, backup);
    chmod(backup.c_str(), 0755);
    if (rename(fresh.c_str(), self.c_str()) != 0) { unlink(fresh.c_str()); return "не удалось заменить файл"; }
    if (SelfTestVersion(self) != expect) {
        rename(backup.c_str(), self.c_str());
        return "новая версия не прошла самопроверку на месте - возвращена прежняя";
    }
    return "";
}

} // namespace pnl
