//
// @file tests/prro_fs_emulator.cpp
// @brief Імітація фіскального сервера ДПС для наскрізних тестів ПРРО
//        (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md).
//        Ендпоінти: GET /ping; POST /fs/doc; POST /fs/cmd; POST /control; GET /control/state.
//        Базова адреса для споживача — http://127.0.0.1:<port>/fs.
// CLI: prro_fs_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>] [--data <dir>] [--self-test]
// Коди повернення: 0 — штатно; 1 — не піднявся порт / провал self-test; 2 — CLI/bootstrap.
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/PrroFsService.h"
#include "support/UapkiOracle.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#pragma comment(lib, "shell32.lib")

#ifndef HOST_DATA_DIR
#  define HOST_DATA_DIR ""
#endif

int RunPrroFsSelfTest();   // tests/prro_fs_emulator_selftest.cpp

namespace {

struct Config {
    int          port = 8099;
    std::wstring keyPath, providersDir, dataDir;
    std::string  pass = "testpassword";
    bool         selfTest = false;
};

std::mutex              g_stopMx;
std::condition_variable g_stopCv;
bool                    g_stopRequested = false;   // guarded by g_stopMx

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        { std::lock_guard<std::mutex> lk(g_stopMx); g_stopRequested = true; }
        g_stopCv.notify_all();
        oracle::Shutdown();
    }
    return FALSE;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring p(buf, n);
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos);
}

void Usage() {
    std::printf(
        "prro_fs_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>] [--data <dir>] [--self-test]\n"
        "  port        порт (деф. 8099; зайнятий і не заданий явно — наступний вільний до +10)\n"
        "  --key       PKCS#12 ключа «сервера» (деф. <data>\\test-diia.p12)\n"
        "  --pass      пароль контейнера (деф. testpassword)\n"
        "  --providers каталог із cm-pkcs12_*.dll (деф. каталог цього exe)\n"
        "  --data      каталог тест-даних certs/ + crls/ (деф. compile-time tests/data)\n"
        "  --self-test сценарії протоколу in-process і вихід\n"
        "Керування: POST /control {\"action\":\"reset\"|\"fault\"|\"set\",…}, GET /control/state.\n");
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) { std::printf("FAIL: CommandLineToArgvW\n"); return 2; }

    Config cfg;
    bool bad = false, portSet = false;
    for (int i = 1; i < argc && !bad; ++i) {
        const std::wstring a = wargv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 < argc) return std::wstring(wargv[++i]);
            std::printf("Аргумент %s потребує значення\n", oracle::w2u8(a).c_str());
            bad = true;
            return std::wstring();
        };
        if      (a == L"--key")       cfg.keyPath      = next();
        else if (a == L"--pass")      cfg.pass         = oracle::w2u8(next());
        else if (a == L"--providers") cfg.providersDir = next();
        else if (a == L"--data")      cfg.dataDir      = next();
        else if (a == L"--self-test") cfg.selfTest     = true;
        else if (!a.empty() && a[0] != L'-' && !portSet) { cfg.port = _wtoi(a.c_str()); portSet = true; }
        else { std::printf("Невідомий аргумент: %s\n", oracle::w2u8(a).c_str()); bad = true; }
    }
    LocalFree(wargv);
    if (bad) { Usage(); return 2; }
    if (cfg.port <= 0 || cfg.port > 65535) { std::printf("Некоректний порт: %d\n", cfg.port); Usage(); return 2; }

    if (cfg.dataDir.empty())      cfg.dataDir      = oracle::u8to16(HOST_DATA_DIR);
    if (cfg.keyPath.empty())      cfg.keyPath      = cfg.dataDir + L"\\test-diia.p12";
    if (cfg.providersDir.empty()) cfg.providersDir = ExeDir();

    std::printf("prro_fs_emulator\n  port=%d\n  data=%s\n  key=%s\n  providers=%s\n", cfg.port,
                oracle::w2u8(cfg.dataDir).c_str(), oracle::w2u8(cfg.keyPath).c_str(),
                oracle::w2u8(cfg.providersDir).c_str());
    std::fflush(stdout);

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (!oracle::Init(oracle::OracleConfig{ cfg.providersDir, cfg.dataDir, cfg.keyPath, cfg.pass })) {
        std::printf("Bootstrap не вдався — вихід\n");
        oracle::Shutdown();
        return 2;
    }

    if (cfg.selfTest) {
        const int rc = RunPrroFsSelfTest();
        oracle::Shutdown();
        return rc;
    }

    if (!minihttp::InitNetwork()) { std::printf("Не вдалося ініціалізувати Winsock\n"); oracle::Shutdown(); return 1; }

    prrofs::PrroFsService service(cfg.port);
    std::unique_ptr<minihttp::Server> server;
    int chosenPort = 0;
    std::string listenErr;
    const int attempts = portSet ? 1 : 11;
    for (int i = 0; i < attempts; ++i) {
        const int tryPort = cfg.port + i;
        if (tryPort > 65535) break;
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(tryPort, "127.0.0.1"));
        s->SetHandler([&service](const minihttp::Request& r) { return service.Handle(r); });
        s->SetCommonHeaders([&service]() { return service.CommonHeaders(); });
        const std::pair<bool, std::string> res = s->Listen();
        if (res.first) { server = std::move(s); chosenPort = tryPort; break; }
        listenErr = res.second;
        if (i == 0) std::printf("Порт %d зайнятий (%s)%s\n", tryPort, listenErr.c_str(),
                                attempts > 1 ? " — шукаю вільний" : "");
    }
    if (!server) {
        std::printf("Не вдалося зайняти порт %d: %s\n", cfg.port, listenErr.c_str());
        minihttp::ShutdownNetwork();
        oracle::Shutdown();
        return 1;
    }
    service.SetPort(chosenPort);
    server->Start();
    std::printf("\nprro_fs_emulator слухає 127.0.0.1:%d — базова адреса http://127.0.0.1:%d/fs — Ctrl-C для виходу\n",
                chosenPort, chosenPort);
    if (chosenPort != cfg.port)
        std::printf("УВАГА: порт %d був зайнятий. У параметрі АдресСервераДПС вкажіть http://127.0.0.1:%d/fs\n",
                    cfg.port, chosenPort);
    std::fflush(stdout);

    {
        std::unique_lock<std::mutex> lk(g_stopMx);
        g_stopCv.wait(lk, [] { return g_stopRequested; });
    }
    server->Stop();
    minihttp::ShutdownNetwork();
    oracle::Shutdown();
    return 0;
}
