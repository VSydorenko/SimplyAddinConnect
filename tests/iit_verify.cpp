///
/// @file tests/iit_verify.cpp
/// @brief iit_verify — НЕЗАЛЕЖНИЙ арбітр підпису на нативній бібліотеці АТ «ІІТ».
///
/// НАВІЩО. Рівні native_host перевіряють наш підпис НАШИМ ЖЕ VERIFY: підписали
/// UAPKI — перевірили UAPKI. При системній помилці (спільній для підпису й
/// перевірки) обидва рівні пройдуть зелено, нічого не помітивши. Потрібен двигун
/// ІНШОГО ПОХОДЖЕННЯ. UAPKI є форком Cryptonite (extern/uapki/README.md:4), тож
/// жодна Cryptonite-похідна бібліотека арбітром бути не може — спільний предок дає
/// спільні системні помилки. АТ «ІІТ» має власну кодову базу, тому її EUSignCP.dll
/// і є другою, незалежною думкою про наш підпис.
///
/// ЛІЦЕНЗІЯ. EUSignCP.dll у репозиторій НЕ копіюється — вантажиться з місця
/// встановлення через LoadLibraryW+GetProcAddress. Ліцензія ІІТ §2.2: складові
/// неподільного продукту не можна поділяти для використання на кількох комп'ютерах.
///
/// ТІЛЬКИ x86. EUSignCP.dll 1.3.1.224 — 32-бітна (dumpbin /headers → 14C machine),
/// x64-процес її не завантажить. Ціль оголошена в tests/CMakeLists.txt ДО гейта
/// BUILD_WITH_UAPKI: UAPKI цьому арбітру не потрібен — у незалежності від нашого
/// крипто-стека весь його сенс.
///
/// СТАН. Задача 5 — це КАРКАС: пошук бібліотеки, прив'язка експортів і чесний SKIP.
/// Власне перевірка підпису — задача 6, сховище довіри — задача 7.
///
/// КОДИ ВИХОДУ (контракт для задач 6-11, не міняти):
///   0 — підпис валідний | 1 — підпис невалідний | 2 — помилка використання/внутрішня
///   3 — SKIP (немає ІІТ або сховища довіри).
/// stdout — РІВНО ОДИН рядок JSON: {"status":...,"detail":...,"code":...}.
///

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW (потрібен явно)
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")  // CommandLineToArgvW

// ---------------------------------------------------------------------------
// Прототипи потрібних експортів EUSignCP.dll.
//
// EULoad/EUUnload/EUGetInterface у DLL НЕ експортуються — це glue із SDK ІІТ,
// якого в інсталяції кінцевого користувача немає. Тому зв'язуємось виключно через
// GetProcAddress по іменах (звірено dumpbin /exports, 630 експортів, імена
// недекоровані).
//
// ДЖЕРЕЛО СИГНАТУР — настанова АТ «ІІТ» (додаток з описом інтерфейсу бібліотеки
// підпису, EUSignMSWCPPAppendixA): звірено поле за полем, аргумент за аргументом.
// Заголовка EUSignCP.h у постачанні кінцевого користувача НЕМАЄ, тож перевірити
// прототип «на місці» неможливо — саме тому кожен нижче має посилання на джерело.
//
// Угода виклику — __stdcall (WINAPI), як і в решті API ІІТ. УВАГА: при __stdcall
// стек чистить ВИКЛИКАНА сторона, тож помилка в КІЛЬКОСТІ параметрів псує стек
// МОВЧКИ — ні помилки збірки, ні винятку, лише зіпсовані дані десь далі.
// НЕ «спрощуй» ці прототипи, прибираючи «зайвий» аргумент: два з них уже були
// коротшими на один параметр (EUSetFileStoreSettings, EUGetErrorLangDesc), і саме
// настанова показала, що це помилка. Правити — лише звірившись із настановою ІІТ.
// PSTR у настанові = char* (не const): рядки передаються модифікованими буферами.
// ---------------------------------------------------------------------------

/// Відомості про підпис, які повертає EUVerifyData*; звільняється EUFreeSignInfo.
/// Настанова ІІТ (EUSignMSWCPPAppendixA, EU_SIGN_INFO): BOOL bFilled, рівно 17 PSTR
/// у цьому порядку, BOOL bTimeAvail, BOOL bTimeStamp, SYSTEMTIME Time.
typedef struct {
    BOOL       bFilled;
    char*      pszIssuer;         char* pszIssuerCN;       char* pszSerial;
    char*      pszSubject;        char* pszSubjCN;         char* pszSubjOrg;
    char*      pszSubjOrgUnit;    char* pszSubjTitle;      char* pszSubjState;
    char*      pszSubjLocality;   char* pszSubjFullName;   char* pszSubjAddress;
    char*      pszSubjPhone;      char* pszSubjEMail;      char* pszSubjDNS;
    char*      pszSubjEDRPOUCode; char* pszSubjDRFOCode;
    BOOL       bTimeAvail;
    BOOL       bTimeStamp;        // позначка часу з TSP-сервера
    SYSTEMTIME Time;
} EU_SIGN_INFO;

// Настанова ІІТ: EUSetSettingsRegPath(DWORD dwRootKey, PSTR pszRegPath).
// dwRootKey: 0 = DEFAULT, 1 = HKLM, 2 = HKCU, 3 = CURRENT.
// Ізоляція налаштувань у власну гілку реєстру КРИТИЧНА й робиться ДО EUInitialize:
// коли шлях налаштувань порожній, бібліотека на Windows бере налаштування з реєстру
// КОРИСТУВАЧА — і EUSetModeSettings(TRUE) мовчки перемкнув би інсталяцію ІІТ
// користувача в офлайн, тобто наш тест зіпсував би чужий софт побічним ефектом.
typedef DWORD (WINAPI *PFN_SetSettingsRegPath)(DWORD, char*);
typedef void  (WINAPI *PFN_SetUIMode)(BOOL);        // настанова ІІТ: VOID; FALSE = без діалогів
typedef DWORD (WINAPI *PFN_Initialize)(void);       // настанова ІІТ: DWORD, без аргументів
typedef DWORD (WINAPI *PFN_SetModeSettings)(BOOL);  // настанова ІІТ: DWORD; TRUE = офлайн
// Настанова ІІТ: EUSetFileStoreSettings(PSTR pszPath, BOOL bCheckCRLs,
//   BOOL bAutoRefresh, BOOL bOwnCRLsOnly, BOOL bFullAndDeltaCRLs,
//   BOOL bAutoDownloadCRLs, BOOL bSaveLoadedCerts, DWORD dwExpireTime).
// ВІСІМ аргументів. Восьмий (час зберігання стану перевіреного сертифіката, секунди)
// легко не помітити: із сімома викликана сторона зняла б зі стека 32 байти проти
// покладених 28. Задача 7 має ставити dwExpireTime = 30 (аналог ocspResponseExpireTime
// у віджеті ЦЗО); при bCheckCRLs=FALSE та офлайні значення ні на що не впливає, але
// випадкове число тут неприйнятне.
typedef DWORD (WINAPI *PFN_SetFileStoreSettings)(char*, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL, DWORD);
// Настанова ІІТ: EUSaveCertificates(PBYTE pbCertificates, DWORD dwCertificatesLength).
// Імпорт бандла сертифікатів (P7B) у файлове сховище — задача 7.
typedef DWORD (WINAPI *PFN_SaveCertificates)(BYTE*, DWORD);
// Настанова ІІТ: EUVerifyDataInternal(PSTR pszSignedData, PBYTE pbSignedData,
//   DWORD dwSignedDataLength, PBYTE* ppbData, PDWORD pdwDataLength, PEU_SIGN_INFO).
// Провідний PSTR — той самий підпис у base64; «якщо параметр == 0, перевіряються
// дані з масиву байт», тож nullptr + байти — ШТАТНИЙ задокументований шлях.
typedef DWORD (WINAPI *PFN_VerifyDataInternal)(char*, BYTE*, DWORD, BYTE**, DWORD*, EU_SIGN_INFO*);
// Настанова ІІТ: PSTR EUGetErrorLangDesc(DWORD dwError, DWORD dwLang) — ПОВЕРТАЄ
// рядок, а не заповнює буфер. dwLang = 0 (EU_DEFAULT_LANG; інших мовних констант
// настанова не наводить). EUFreeMemory на результат НЕ кликати: настанова цього не
// приписує, а зайве звільнення чужого статичного буфера гірше за витік кількох байтів.
// Потрібен, щоб «відхилив підпис» і «не зміг перевірити» не зливались у безликий exit 1.
typedef char* (WINAPI *PFN_GetErrorLangDesc)(DWORD, DWORD);
typedef void  (WINAPI *PFN_FreeMemory)(BYTE*);            // настанова ІІТ: VOID
typedef void  (WINAPI *PFN_FreeSignInfo)(EU_SIGN_INFO*);  // настанова ІІТ: VOID
typedef void  (WINAPI *PFN_Finalize)(void);               // настанова ІІТ: VOID, не DWORD

/// Прив'язана бібліотека ІІТ. Перелік експортів живе РІВНО ТУТ: задачі 6 і 7
/// беруть уже прив'язані вказівники й bindEu більше не правлять.
struct Eu {
    HMODULE      h = nullptr;
    std::wstring dir;                    // каталог, з якого завантажено DLL (для діагностики)
    PFN_SetSettingsRegPath   SetSettingsRegPath   = nullptr;
    PFN_SetUIMode            SetUIMode            = nullptr;
    PFN_Initialize           Initialize           = nullptr;
    PFN_SetModeSettings      SetModeSettings      = nullptr;
    PFN_SetFileStoreSettings SetFileStoreSettings = nullptr;
    PFN_SaveCertificates     SaveCertificates     = nullptr;
    PFN_VerifyDataInternal   VerifyDataInternal   = nullptr;
    PFN_GetErrorLangDesc     GetErrorLangDesc     = nullptr;
    PFN_FreeMemory           FreeMemory           = nullptr;
    PFN_FreeSignInfo         FreeSignInfo         = nullptr;
    PFN_Finalize             Finalize             = nullptr;
};

// ---------------------------------------------------------------------------
// Дрібний інструментарій
// ---------------------------------------------------------------------------

/// Широкий рядок -> UTF-8 (шляхи можуть містити кирилицю).
static std::string w2u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

/// Екранування рядка для JSON: шляхи Windows містять зворотні слеші, а вони
/// без екранування зробили б вивід невалідним JSON — і гейт (задача 9) мовчки
/// не розібрав би вердикт арбітра.
static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

/// Єдиний рядок результату на stdout. status — завжди літерал, тож не екранується.
static void jsonOut(const char* status, const std::string& detail, long code) {
    std::printf("{\"status\":\"%s\",\"detail\":\"%s\",\"code\":%ld}\n",
                status, jsonEscape(detail).c_str(), code);
    std::fflush(stdout);
}

/// Тимчасове перемикання кодової сторінки КОНСОЛІ на UTF-8, щоб кириличні деталі
/// читалися при ручному запуску. Консоль спільна з батьківською оболонкою, тож
/// стару сторінку обов'язково повертаємо. На перенаправленому stdout не робить
/// нічого: туди й так ідуть UTF-8 байти — це і є контракт виводу.
struct ConsoleUtf8Guard {
    UINT prev = 0;
    ConsoleUtf8Guard() {
        DWORD mode = 0;
        if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
            const UINT cur = GetConsoleOutputCP();
            if (cur != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) prev = cur;
        }
    }
    ~ConsoleUtf8Guard() { if (prev) SetConsoleOutputCP(prev); }
};

/// Читання файла ЦІЛКОМ широким іменем. Саме CreateFileW, а не CreateFileA:
/// ANSI-шлях спотворив би кириличні імена (та сама причина, що й для
/// CommandLineToArgvW у native_host.cpp).
static bool readFileBytes(const std::wstring& path, std::vector<BYTE>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart >= 0 && sz.QuadPart <= 64LL * 1024 * 1024;
    if (ok && sz.QuadPart > 0) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) != 0 && got == out.size();
        if (!ok) out.clear();
    }
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Пошук і прив'язка бібліотеки ІІТ
// ---------------------------------------------------------------------------

/// Пошук каталогу ІІТ: спершу перекриття змінною середовища IIT_EU_DIR (нею ж
/// тест перевіряє гілку SKIP), потім стандартне місце встановлення.
/// Критерій добору — НЕ ім'я підкаталогу, а наявність у ньому «End User\EUSignCP.dll»:
/// перебираємо БУДЬ-ЯКУ підтеку «Institute of Informational Technologies» і беремо
/// першу, де така DLL реально лежить. Тому ані версія («Certificate Authority-1.3»),
/// ані сама схема іменування ЦСК у код не впаяні й переживуть оновлення ІІТ.
static bool findIitDir(std::wstring& out) {
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"IIT_EU_DIR", env, MAX_PATH) > 0) {
        out = env;
        return GetFileAttributesW((out + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    // Процес 32-бітний, тож ProgramFiles(x86) вказує саме туди, де стоїть ІІТ.
    wchar_t pf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return false;
    const std::wstring base = std::wstring(pf) + L"\\Institute of Informational Technologies";
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring cand = base + L"\\" + n + L"\\End User";
        if (GetFileAttributesW((cand + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = cand;
            found = true;
            break;
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return found;
}

/// Завантаження EUSignCP.dll і прив'язка ВСІХ потрібних експортів (11 штук —
/// разом із тими, що знадобляться задачам 6 і 7). Перелік навмисно повний саме
/// тут: розділити його на три правки означало б ризикувати розбіжністю.
/// Будь-який відсутній експорт — це не «трохи менше можливостей», а неробочий
/// арбітр, тож повертаємо false з названим іменем -> SKIP, ніколи не PASS.
static bool bindEu(Eu& eu, std::string& err) {
    std::wstring dir;
    if (!findIitDir(dir)) {
        err = "EUSignCP.dll не знайдено (ІІТ не встановлено або IIT_EU_DIR вказує не туди)";
        return false;
    }
    eu.dir = dir;
    SetDllDirectoryW(dir.c_str());                       // залежності EUSignCP.dll лежать поруч із нею
    eu.h = LoadLibraryW((dir + L"\\EUSignCP.dll").c_str());
    if (!eu.h) {
        err = "LoadLibraryW не змогла завантажити EUSignCP.dll із " + w2u8(dir)
            + ", GetLastError=" + std::to_string((unsigned long)GetLastError());
        return false;
    }

    // FreeLibrary перед виходом: інакше частковий провал прив'язки лишав би
    // завантажений HMODULE — асиметрія з успішним шляхом, де модуль звільняє main.
    #define BIND(field, name)                                                    \
        eu.field = (decltype(eu.field))GetProcAddress(eu.h, name);               \
        if (!eu.field) {                                                         \
            err = std::string("Немає експорту ") + name;                         \
            FreeLibrary(eu.h); eu.h = nullptr;                                   \
            return false;                                                        \
        }
    BIND(SetSettingsRegPath,   "EUSetSettingsRegPath")   // ізоляція налаштувань (задача 6)
    BIND(SetUIMode,            "EUSetUIMode")
    BIND(Initialize,           "EUInitialize")
    BIND(SetModeSettings,      "EUSetModeSettings")
    BIND(SetFileStoreSettings, "EUSetFileStoreSettings")
    BIND(SaveCertificates,     "EUSaveCertificates")     // імпорт бандла довіри (задача 7)
    BIND(VerifyDataInternal,   "EUVerifyDataInternal")
    BIND(GetErrorLangDesc,     "EUGetErrorLangDesc")     // людський опис коду помилки (задача 6)
    BIND(FreeMemory,           "EUFreeMemory")
    BIND(FreeSignInfo,         "EUFreeSignInfo")
    BIND(Finalize,             "EUFinalize")
    #undef BIND
    return true;
}

// ---------------------------------------------------------------------------

int main() {
    ConsoleUtf8Guard cpGuard;

    // char** argv на Windows приходить у системному ANSI-кодуванні, тож кириличні
    // шляхи (а задача 9 подає сюди шляхи з Get-ChildItem) спотворювались би.
    // Беремо широкі аргументи напряму — як у native_host.cpp.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) {
        jsonOut("ERROR", "CommandLineToArgvW не повернула аргументів", 0);
        return 2;
    }
    if (argc < 2) {
        jsonOut("ERROR", "usage: iit_verify_x86 <file.p7s>", 0);
        LocalFree(wargv);
        return 2;
    }
    const std::wstring sigPath = wargv[1];
    LocalFree(wargv);

    Eu eu;
    std::string err;
    if (!bindEu(eu, err)) {
        jsonOut("SKIP", err, 0);      // немає ІІТ -> явний SKIP із названою причиною, НІКОЛИ не PASS
        return 3;
    }

    // Вхідний файл читаємо вже тут, щоб широкий шлях був під навантаженням із
    // першого дня. У КАРКАСІ результат читання на код виходу НЕ впливає:
    // перевірки підпису ще немає, а верифікація задачі 5 навмисно кличе арбітра з
    // неіснуючим ім'ям. Задача 6 зробить недоступний вхід твердою помилкою (exit 2).
    std::vector<BYTE> sig;
    const bool readOk = readFileBytes(sigPath, sig);

    std::string detail = "bind ok (11 експортів); ІІТ: " + w2u8(eu.dir)
                       + "; вхід: " + w2u8(sigPath) + " — "
                       + (readOk ? (std::to_string(sig.size()) + " байт")
                                 : std::string("не прочитано (у каркасі не критично)"));
    jsonOut("OK", detail, 0);

    if (eu.h) FreeLibrary(eu.h);
    return 0;
}
