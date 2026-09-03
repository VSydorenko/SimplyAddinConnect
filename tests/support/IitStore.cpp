#include "IitStore.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Каталог кешу — поруч із уже наявним ...\providers\<версія>\, за тим самим
// патерном, що використовує UAPKIConnectHelper::EnsureProviderDeployed:
// %LOCALAPPDATA%\SimplyAddinConnect\<підкаталог>.
// ---------------------------------------------------------------------------
static bool cacheDir(std::wstring& out) {
    wchar_t lad[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    out = std::wstring(lad) + L"\\SimplyAddinConnect\\iit-store";
    return true;
}

static bool fileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool fileSize64(const std::wstring& path, ULONGLONG& outSize) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    ULARGE_INTEGER u{};
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    outSize = u.QuadPart;
    return true;
}

static void makeDirs(const std::wstring& path) {
    std::wstring acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (!acc.empty()) CreateDirectoryW(acc.c_str(), nullptr);
        }
        if (i < path.size()) acc += path[i];
    }
}

// FNV-1a 64-біт по вмісту файлу. Не про криптостійкість — лише щоб маркер
// ідемпотентності ловив підміну бандла файлом того самого розміру.
static bool fnv1aFile(const std::wstring& path, ULONGLONG& outHash) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ULONGLONG hash = 1469598103934665603ULL;   // FNV offset basis (64-біт)
    const ULONGLONG prime = 1099511628211ULL;  // FNV prime (64-біт)
    std::vector<BYTE> buf(64 * 1024);
    DWORD got = 0;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            hash ^= buf[i];
            hash *= prime;
        }
    }
    CloseHandle(h);
    outHash = hash;
    return true;
}

static std::wstring markerPath(const std::wstring& bundlePath) {
    return bundlePath + L".imported";
}

// Маркер — текстовий рядок "<розмір>:<хеш-hex>". Формат довільний: файл
// приватний для цього кешу, ніхто інший його не читає.
static bool readMarker(const std::wstring& path, ULONGLONG& size, ULONGLONG& hash) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[128]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return false;
    buf[got] = '\0';
    unsigned long long s = 0, hh = 0;
    if (std::sscanf(buf, "%llu:%llx", &s, &hh) != 2) return false;
    size = s;
    hash = hh;
    return true;
}

// Атомарний запис маленького текстового файлу: тимчасове ім'я + MoveFileExW,
// той самий патерн, що й для бандла нижче. Програш гонки (інший процес уже
// поставив свіжий маркер) трактуємо як успіх — файл на місці, і досить.
static bool writeFileAtomic(const std::wstring& dst, const char* data, size_t len) {
    const std::wstring tmp = dst + L".tmp_" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, (DWORD)len, &written, nullptr) && written == len;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }

    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return fileExists(dst);   // встиг інший процес — програш гонки трактуємо як успіх
    }
    return true;
}

// Екранування одинарної лапки для підстановки в одинарний PowerShell-рядок:
// ' -> ''. Без цього шлях з апострофом (валідний символ у профілі Windows,
// напр. C:\Users\O'Brien\..., і звичайний орфографічний знак української
// мови) обриває -OutFile посеред рядка й ламає синтаксис команди —
// ДЕТЕРМІНОВАНО на кожному такому профілі, а не зрідка.
static std::wstring psSingleQuoteEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        out += c;
        if (c == L'\'') out += L'\'';
    }
    return out;
}

// Мінімальна перевірка вмісту завантаженого: captive portal чи заглушка CDN
// віддають HTTP 200 з HTML/XML замість бандла, і без цієї перевірки зіпсований
// файл закешувався б НАЗАВЖДИ (повторне завантаження — лише за відсутності
// цільового файлу). Формат DER/PEM не вгадуємо — відкидаємо тільки очевидно
// НЕ бандл: перший непробільний байт '<' або розмір менший за 1024 Б
// (реальний бандл ЦЗО — 1 569 840 Б, запас із великим кратним запасом).
static bool looksLikeBundle(const std::wstring& path) {
    ULONGLONG size = 0;
    if (!fileSize64(path, size) || size < 1024) return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE buf[64]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;

    for (DWORD i = 0; i < got; ++i) {
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') continue;
        return buf[i] != '<';
    }
    return true;   // самі пробіли на початку — нетиповий, але не captive-portal випадок
}

// Завантаження бандла ЦСК. Запис АТОМАРНИЙ: тимчасове ім'я + MoveFileExW.
// Атомарність не косметика — run_tests.ps1 ганяє дві архітектури, прогони
// можуть перетнутися на одному кеші. Програш гонки трактується як успіх,
// якщо цільовий файл уже на місці.
static bool downloadBundle(const std::wstring& dir, const std::wstring& dst) {
    makeDirs(dir);
    const std::wstring tmp = dst + L".tmp_" + std::to_wstring(GetCurrentProcessId());

    std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command "
                       L"\"try { Invoke-WebRequest "
                       L"'https://czo.gov.ua/download/certificates/CACertificates.p7b' "
                       L"-OutFile '" + psSingleQuoteEscape(tmp) + L"' -UseBasicParsing; exit 0 } catch { exit 1 }\"";

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;

    // Таймаут 120 с — не лише межа очікування, а й тригер: якщо процес не
    // вклався, TerminateProcess обов'язковий. Інакше зависла powershell.exe
    // лишається тримати tmp відкритим (DeleteFileW нижче мовчки провалиться)
    // або дописує файл уже ПІСЛЯ повернення функції — єдина гілка, де тимчасовий
    // файл раніше міг пережити функцію.
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);   // дочекатись реального завершення й звільнення tmp
    }
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (rc != 0) { DeleteFileW(tmp.c_str()); return false; }

    if (!looksLikeBundle(tmp)) { DeleteFileW(tmp.c_str()); return false; }

    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return fileExists(dst);   // встиг інший процес — програш гонки трактуємо як успіх
    }
    return true;
}

static bool wideToUtf8(const std::wstring& w, std::string& out) {
    if (w.empty()) { out.clear(); return true; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return true;
}

bool ensureIitStore(std::string& outDirUtf8, std::wstring& outBundlePath, bool& outNeedImport) {
    outDirUtf8.clear();
    outBundlePath.clear();
    outNeedImport = false;

    std::wstring dir;
    if (!cacheDir(dir)) return false;

    const std::wstring dst = dir + L"\\CACertificates.p7b";
    if (!fileExists(dst) && !downloadBundle(dir, dst)) return false;
    if (!fileExists(dst)) return false;

    if (!wideToUtf8(dir, outDirUtf8)) return false;

    // Ідемпотентність: маркер поруч із бандлом фіксує розмір+хеш уже
    // імпортованого файлу. EUSaveCertificates дорогий (бандл ЦЗО великий) і
    // марний на вже наповненому сховищі, тож кличемо його лише коли маркера
    // немає або бандл змінився. Не змогли порахувати хеш поточного бандла —
    // безпечніше вважати імпорт потрібним, ніж мовчки пропустити наповнення.
    ULONGLONG curSize = 0, curHash = 0;
    bool needImport = true;
    if (fileSize64(dst, curSize) && fnv1aFile(dst, curHash)) {
        ULONGLONG mSize = 0, mHash = 0;
        needImport = !readMarker(markerPath(dst), mSize, mHash) || mSize != curSize || mHash != curHash;
    }

    outNeedImport = needImport;
    if (needImport) outBundlePath = dst;   // потрібен лише тоді, коли задача 6 читатиме байти для SaveCertificates
    return true;
}

bool markIitStoreImported(const std::wstring& bundlePath) {
    ULONGLONG size = 0, hash = 0;
    if (!fileSize64(bundlePath, size) || !fnv1aFile(bundlePath, hash)) return false;

    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%llu:%llx", (unsigned long long)size, (unsigned long long)hash);
    return writeFileAtomic(markerPath(bundlePath), buf, std::strlen(buf));
}
