#pragma once
// UapkiOracle — крипто-частина тестових HTTP-оракулів (uapki_fiscal_emulator,
// prro_fs_emulator): статично злінковане ядро UAPKI через process()/json_free().
// Винесено з tests/uapki_fiscal_emulator.cpp БЕЗ зміни логіки (спека
// docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §9).
// UAPKI — процесний singleton (одне сховище, один обраний ключ), тому модуль —
// набір вільних функцій над внутрішнім станом, а не клас з екземплярами.
// Усі функції потокобезпечні: process() серіалізується внутрішнім м'ютексом.
#include <string>

namespace oracle {

struct OracleConfig {
    std::wstring providersDir;   // каталог із cm-pkcs12_*.dll
    std::wstring dataDir;        // certs/ + crls/ (копіюються у %TEMP%\uapki_oracle_<pid>)
    std::wstring keyPath;        // PKCS#12-контейнер тест-ключа
    std::string  pass;
};

struct VerifyOutcome {
    bool        accepted = false;   // критерій прийняття — РІВНО 4 умови (див. Verify)
    std::string status, statusSig, statusMd, signerCertId, contentB64;
    bool        validSig = false, validDig = false;
    // Діагностичний крос-чек: сертифікат підписувача реально ВКЛАДЕНО в CMS
    // (certIds непорожній І містить signerCertId). У критерій прийняття НЕ входить.
    bool        certEmbedded = false;
    // Причина відмови від крипто-ядра (errorCode != 0).
    long        errorCode = 0;
    std::string errorText;
};

// Bootstrap: writable-кеш у %TEMP% -> INIT -> OPEN -> KEYS -> SELECT_KEY.
bool          Init(const OracleConfig& cfg);
// STRUCT-перевірка CMS; accepted = errorCode==0 && status=="TOTAL-VALID" && validSignatures && validDigests.
VerifyOutcome Verify(const std::string& derBytes);
// CAdES-BES enveloping тест-ключем. Сирі DER-байти або порожній рядок.
std::string   Sign(const std::string& rawBytes);
// Прибрати temp-каталог. Ідемпотентний; безпечний з потоку ctrl-хендлера.
void          Shutdown();

std::wstring u8to16(const std::string& s);
std::string  w2u8(const std::wstring& w);
std::string  b64encode(const std::string& in);
bool         b64decode(const std::string& b64, std::string& out);

}  // namespace oracle
