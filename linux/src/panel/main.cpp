// main.cpp - ygg-control для Linux: панель управления Yggdrasil в терминале.
// Сборка: build.sh (g++, OpenSSL). Подробно - README_LINUX.md.
#include "app.cpp"
#include "screens1.cpp"
#include "screens2.cpp"
#include "screens3.cpp"

namespace pnl {

int App::Main() {
    if (!term.Start()) { fprintf(stderr, "Панели нужен терминал.\n"); return 1; }
    data.Start();
    stack.emplace_back(new MainScreen());
    // при запуске - тихая проверка обновлений и чтение конфига
    CheckPanelUpdate(true);
    CheckYggUpdate(true);
    ReloadConfig();
    while (!quit) {
        FinishJobs();
        s = data.Get();
        term.UpdateSize();
        Canvas& c = term.canvas;
        if (c.w < 80 || c.h < 24) {
            c.Put(1, 1, "Окно слишком маленькое: нужно хотя бы 80×24 (сейчас " + std::to_string(c.w) + "×" + std::to_string(c.h) + ")", OR);
        } else {
            stack.back()->Draw(c, *this);
            if (modal.open) DrawModal(c);
        }
        term.Present();
        tui::Key k = term.Read(Busy() || !jobs.empty() ? 150 : 500);
        ++spin;
        if (k.k == K::None || k.k == K::Resize) continue;
        if (k.k == K::CtrlC) break;
        if (modal.open) { ModalKey(k); continue; }
        Screen* top = stack.back().get();
        if (top->Key(k, *this)) continue;
        if (k.k == K::Esc || (k.k == K::Backspace && !top->Typing())) {
            if (stack.size() > 1) { if (top->CanLeave(*this)) Pop(); }
            else quit = true;
            continue;
        }
        if (!top->Typing() && (k.is('q') || k.text == "й")) { if (stack.size() == 1 || top->CanLeave(*this)) quit = true; continue; }
    }
    data.Stop();
    term.Stop();
    return 0;
}

} // namespace pnl

#ifndef YGG_CONTROL_NO_MAIN
static void PrintHelp() {
    printf("ygg-control %s - панель управления Yggdrasil в терминале\n\n", PANEL_VERSION);
    printf("  yggdrasil  или  ygg-control   открыть панель (попросит пароль sudo)\n");
    printf("  yggdrasil <аргументы>         команда самому Yggdrasil (-genconf, -version...) - передаётся ему\n");
    printf("  ygg-control version      версия\n");
    printf("  ygg-control --selftest   самопроверка (печатает версию - нужна обновлению)\n");
    printf("  ygg-control update       поставить новую версию из репозитория\n\n");
    printf("Сторож пиров ygg-watchdog - отдельная программа, панель показывает его состояние.\n");
}

// yggdrasilctl, конфиг и systemctl требуют root - перезапуск через sudo
static void ElevateIfNeeded(int argc, char** argv) {
    if (util::IsRoot()) return;
    if (util::Which("sudo").empty()) { fprintf(stderr, "Нужны права root, а sudo не найден. Запусти от root.\n"); exit(1); }
    std::vector<char*> a;
    std::string sudo = "sudo", self = util::SelfExe();
    a.push_back(&sudo[0]);
    a.push_back(&self[0]);
    for (int i = 1; i < argc; ++i) a.push_back(argv[i]);
    a.push_back(nullptr);
    execvp("sudo", a.data());
    perror("sudo");
    exit(1);
}

// Настоящий yggdrasil (не эта панель): по пути из службы, потом стандартные места
static std::string RealYggdrasil() {
    char self[4096] = "";
    if (!realpath(util::SelfExe().c_str(), self)) self[0] = 0;
    std::vector<std::string> cand;
    util::ProcResult r = util::Run({ "systemctl", "show", "yggdrasil", "-p", "ExecStart", "--value" }, 5000);
    size_t a = r.out.find("argv[]=");
    if (a != std::string::npos) { auto args = ygg::SplitCmd(r.out.substr(a + 7)); if (!args.empty()) cand.push_back(args[0]); }
    for (const char* c : { "/usr/bin/yggdrasil", "/usr/sbin/yggdrasil", "/opt/yggdrasil/yggdrasil", "/usr/local/sbin/yggdrasil", "/usr/local/bin/yggdrasil" })
        cand.push_back(c);
    for (const auto& c : cand) {
        char rp[4096];
        if (access(c.c_str(), X_OK) == 0 && realpath(c.c_str(), rp) && strcmp(rp, self) != 0) return c;
    }
    return "";
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    // Вызов командой «yggdrasil»: без аргументов - панель, с аргументами
    // (-genconf, -version, -useconffile ...) - это команда самому Yggdrasil, передаём ему
    if (util::BaseName(argv[0]) == "yggdrasil" && argc > 1) {
        std::string real = RealYggdrasil();
        if (real.empty()) { fprintf(stderr, "Настоящий yggdrasil не найден (эта команда - панель ygg-control).\n"); return 127; }
        argv[0] = const_cast<char*>(real.c_str());
        execv(real.c_str(), argv);
        perror(real.c_str());
        return 127;
    }
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "--selftest" || cmd == "selftest") {
        if (!net::TlsCtx()) return 1;
        printf("%s\n", PANEL_VERSION);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") { printf("ygg-control %s\n", PANEL_VERSION); return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { PrintHelp(); return 0; }
    ElevateIfNeeded(argc, argv);
    net::Cfg().userAgent = std::string("ygg-control/") + PANEL_VERSION;
    net::Cfg().stateDir = util::SelfDir();
    if (cmd == "update") {
        pnl::PanelUpdate u = pnl::CheckPanelUpdate();
        if (!u.ok) { fprintf(stderr, "Не удалось проверить: %s\n", u.err.c_str()); return 1; }
        if (!util::VersionNewer(PANEL_VERSION, u.tag)) { printf("В репозитории та же версия или старше (у тебя %s).\n", PANEL_VERSION); return 0; }
        std::string e = pnl::ApplyPanelUpdate(u.tag);
        if (!e.empty()) { fprintf(stderr, "Ошибка: %s\n", e.c_str()); return 1; }
        printf("Обновлено до %s.\n", u.tag.c_str());
        return 0;
    }
    if (!cmd.empty()) { fprintf(stderr, "неизвестная команда: %s\n\n", cmd.c_str()); PrintHelp(); return 1; }
    pnl::App app;
    return app.Main();
}
#endif
