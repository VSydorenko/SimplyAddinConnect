//
// @file tests/provider_contract_selftest.cpp
// @brief L1.5 — контракт ініціалізації провайдера НКІ.
//        Вантажить cm-pkcs12_xNN.dll напряму (LoadLibraryW + GetProcAddress) і
//        перевіряє, що provider_init ідемпотентний і веде облік посилань.
//        UAPKI НЕ лінкується: cm-api.h самодостатній.
//
// Це окремий консольний exe; PCH головного проєкту НЕ підключається, дозволено printf.
//

#ifndef _WIN32

#include <cstdio>
int main() {
    printf("provider_contract_selftest: Windows only\n");
    return 0;
}

#else // _WIN32

#include <windows.h>
#include <cstdio>
#include <string>

#include "cm-api.h"
#include "cm-errors.h"

#ifdef _WIN64
#  define ARCH_W L"_x64"
#else
#  define ARCH_W L"_x86"
#endif

#ifndef PCS_BIN_DIR
#  define PCS_BIN_DIR ""
#endif

static int g_fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", (msg)); ++g_fails; } \
    else         { printf("[PASS] %s\n", (msg)); } \
} while (0)

// Перетворює UTF-8 у широкий рядок (шлях до провайдера приходить із CMake як char*).
static std::wstring u8to16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct ProviderApi {
    HMODULE                 h      = nullptr;
    cm_provider_info_f      info   = nullptr;
    cm_provider_init_f      init   = nullptr;
    cm_provider_deinit_f    deinit = nullptr;
    cm_provider_open_f      open   = nullptr;

    bool load(const std::wstring& path) {
        h = LoadLibraryW(path.c_str());
        if (!h) {
            printf("  LoadLibraryW('%ls') err=%lu\n", path.c_str(), GetLastError());
            return false;
        }
        info   = (cm_provider_info_f)   GetProcAddress(h, "provider_info");
        init   = (cm_provider_init_f)   GetProcAddress(h, "provider_init");
        deinit = (cm_provider_deinit_f) GetProcAddress(h, "provider_deinit");
        open   = (cm_provider_open_f)   GetProcAddress(h, "provider_open");
        return info && init && deinit && open;
    }
    ~ProviderApi() { if (h) FreeLibrary(h); }
};

// Пара предикатів, якою код розрізняє «об'єкт живий» від «об'єкта немає»
// (testing-rules, правило 2). provider_open іде через глобал: якщо глобала немає,
// відповідь РІВНО RET_CM_NOT_INITIALIZED; якщо є — будь-яка інша (файла свідомо
// не існує, тож це помилка відкриття, а не ініціалізації).
// Контейнер і пароль не потрібні — саме тому перевірка стабільна.
static bool providerAlive(ProviderApi& api) {
    CM_SESSION_API* session = nullptr;
    CM_ERROR err = api.open("Z:\\nonexistent-by-design.p12", OPEN_MODE_RO, nullptr, &session);
    printf("  [probe] provider_open -> 0x%04X\n", (unsigned)err);
    return err != RET_CM_NOT_INITIALIZED;
}

int wmain(int argc, wchar_t** argv) {
    // argv[1] — повний шлях до провайдера (для разової перевірки cm-pkcs11);
    // без аргументу — cm-pkcs12 з bin/Release, як у гейті.
    std::wstring binDir = u8to16(PCS_BIN_DIR);
    std::wstring path   = (argc > 1) ? std::wstring(argv[1])
                                     : binDir + L"\\cm-pkcs12" + ARCH_W + L".dll";

    printf("provider_contract_selftest\n  provider=%ls\n", path.c_str());

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("SKIP: немає файлу провайдера (зібрано без -WithUAPKI)\n");
        return 3;
    }

    ProviderApi api;
    if (!api.load(path)) { printf("[FAIL] завантаження провайдера й експортів\n"); return 1; }
    printf("[PASS] провайдер завантажено, усі потрібні експорти на місці\n");

    // 1. Перший init
    CHECK(api.init(nullptr) == RET_OK, "init #1 -> RET_OK");
    CHECK(providerAlive(api), "після init #1 провайдер живий");

    // 2. Повторний init — СЕРЦЕ ДЕФЕКТУ.
    CM_ERROR e2 = api.init(nullptr);
    printf("  init #2 -> 0x%04X\n", (unsigned)e2);
    CHECK(e2 == RET_OK, "init #2 -> RET_OK (ідемпотентність)");
    CHECK(providerAlive(api), "після init #2 провайдер живий");

    // 3. deinit при лічильнику 2 — об'єкт мусить ВИЖИТИ.
    CHECK(api.deinit() == RET_OK, "deinit #1 -> RET_OK");
    CHECK(providerAlive(api), "після deinit #1 провайдер ЖИВИЙ (лічильник 2 -> 1)");

    // 4. deinit до нуля — об'єкт зникає.
    CHECK(api.deinit() == RET_OK, "deinit #2 -> RET_OK");
    CHECK(!providerAlive(api), "після deinit #2 провайдера НЕМАЄ (лічильник 0)");

    // 5. deinit без init — поведінка не змінюється.
    CHECK(api.deinit() == RET_CM_NOT_INITIALIZED, "deinit #3 -> RET_CM_NOT_INITIALIZED");

    // 6. Повний цикл заново — провайдер має підніматися після повного звільнення.
    CHECK(api.init(nullptr) == RET_OK, "init #3 після повного deinit -> RET_OK");
    CHECK(providerAlive(api), "після init #3 провайдер живий");
    CHECK(api.deinit() == RET_OK, "фінальний deinit -> RET_OK");

    // 7. Інша конфігурація — НЕ та сама операція. Провайдер мусить відмовити
    //    ГУЧНО й не чіпати ні стану, ні лічильника (контракт cm-api.h).
    CHECK(api.init(nullptr) == RET_OK, "init з конфігурацією A (null) -> RET_OK");
    static const char CFG_B[] = "{\"differentConfig\":true}";
    CM_ERROR eB = api.init((CM_JSON_PCHAR)CFG_B);
    printf("  init з конфігурацією B -> 0x%04X\n", (unsigned)eB);
    CHECK(eB == RET_CM_ALREADY_INITIALIZED, "init з ІНШОЮ конфігурацією -> RET_CM_ALREADY_INITIALIZED");
    // Доказ, що відмова не збільшила лічильник: ОДИН deinit мусить звільнити об'єкт.
    CHECK(api.deinit() == RET_OK, "deinit після відмови -> RET_OK");
    CHECK(!providerAlive(api), "після ОДНОГО deinit провайдера немає (відмова не рахувалась)");

    printf("\n=== provider_contract_selftest: %s (FAIL: %d) ===\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif // _WIN32
