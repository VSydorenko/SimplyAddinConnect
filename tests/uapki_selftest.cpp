/*
 * uapki_selftest — L1-харнес крипто-ядра UAPKI.
 *
 * Проганяє JSON-сценарій напряму через статично злінковані process()/json_free()
 * (без завантаження DLL і без 1С). Для кожного task будує запит {method,parameters},
 * викликає ядро, перевіряє відповідь проти "полів очікувань" (siblings до method).
 *
 * Це окремий консольний exe: НЕ підключає src/core/pch.h головного проєкту й
 * НЕ використовує макроси REPORT_* — звичайний printf тут дозволено.
 *
 * CLI:  uapki_selftest <шлях-до-сценарію.json>
 * Код повернення: 0 — усі task пройшли; 1 — є провали; 2 — фатальна помилка/виняток.
 *
 * КОНТРАКТ формату сценарію (які поля читає selftest) — див. блок наприкінці файлу.
 */

#include <cstdio>
#include <cstdint>
#include <cctype>
#include <string>
#include "parson-helper.h"

//  Символи крипто-ядра (C-лінкування), статично злінковані через uapki_bundle.
extern "C" char* process (const char* request);
extern "C" void json_free (char* buf);


//  ---- Читання файлу цілком (як в upstream test.cpp) --------------------------
static std::string readFile (const char* fileName)
{
    std::string rv_s;
    FILE* f = fopen(fileName, "rb");
    if (f == nullptr) return rv_s;

    fseek(f, 0, SEEK_END);
    const size_t file_size = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    rv_s.resize(file_size);
    if (fread((void*)rv_s.data(), sizeof(char), file_size, f) != file_size) {
        rv_s.clear();
    }
    fclose(f);
    return rv_s;
}

//  ---- base64 -> hex (для звірки DIGEST.result.bytes із expectHashHex) --------
static bool base64ToBytes (const std::string& b64, std::string& out)
{
    static const int8_t T[256] = {
        //  ініціалізуємо -1 усюди, крім валідних символів (задаємо нижче)
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    out.clear();
    int acc = 0, nbits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int8_t v = T[(uint8_t)c];
        if (v < 0) return false;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back((char)((acc >> nbits) & 0xFF));
        }
    }
    return true;
}

static std::string bytesToHexLower (const std::string& bytes)
{
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        s.push_back(H[b >> 4]);
        s.push_back(H[b & 0x0F]);
    }
    return s;
}

static std::string toLower (const std::string& in)
{
    std::string s = in;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}


int main (int argc, char* argv[])
{
    try {
        ParsonHelper::setEscapeSlashes(0);

        if (argc < 2) {
            puts("Usage: uapki_selftest <scenario.json>");
            return 2;
        }

        const char* fn_scenario = argv[1];
        std::string s_scenario = readFile(fn_scenario);
        if (s_scenario.empty()) {
            printf("[FATAL] Cannot read scenario file: %s\n", fn_scenario);
            return 2;
        }

        //  Знімаємо провідний UTF-8 BOM, якщо є: parson не парсить JSON, що
        //  починається з BOM (деякі редактори/копіювальники його додають).
        if (s_scenario.size() >= 3 &&
            (unsigned char)s_scenario[0] == 0xEF &&
            (unsigned char)s_scenario[1] == 0xBB &&
            (unsigned char)s_scenario[2] == 0xBF) {
            s_scenario.erase(0, 3);
        }

        ParsonHelper json;
        //  true = дозволити //-коментарі у сценарії
        if (!json.parse(s_scenario.c_str(), true)) {
            puts("[FATAL] Invalid scenario JSON");
            return 2;
        }

        JSON_Array* ja_tasks = json.getArray("tasks");
        const size_t cnt_tasks = json_array_get_count(ja_tasks);
        if (cnt_tasks == 0) {
            puts("[FATAL] Scenario has no 'tasks' array or it is empty");
            return 2;
        }

        int failures = 0;
        int executed = 0;

        for (size_t i = 0; i < cnt_tasks; i++) {
            JSON_Object* jo_task = json_array_get_object(ja_tasks, i);
            if (!jo_task) { failures++; printf("[FAIL] task #%zu: not an object\n", i); continue; }

            const std::string method = ParsonHelper::jsonObjectGetString(jo_task, "method");
            const bool skip = (json_object_get_boolean(jo_task, "skip") > 0);
            const char* comment = json_object_get_string(jo_task, "comment");

            //  Пропускаємо: явний skip / порожній метод / спец-методи upstream (_DIGEST, _NEW_THREAD, ...)
            if (skip || method.empty() || method[0] == '_') {
                printf("[SKIP] task #%zu method='%s'%s%s\n", i + 1, method.c_str(),
                       comment ? " : " : "", comment ? comment : "");
                continue;
            }

            //  --- Поля очікувань (читаємо ДО побудови запиту; у parameters НЕ копіюємо) ---
            const bool expect_error         = (json_object_get_boolean(jo_task, "expectError") > 0);
            const bool has_expect_count      = (json_object_has_value(jo_task, "expectCountCmProviders") != 0);
            const int  expect_count_prov     = (int)json_object_get_number(jo_task, "expectCountCmProviders");
            const bool has_expect_hash       = (json_object_get_string(jo_task, "expectHashHex") != nullptr);
            const std::string expect_hash_hex = ParsonHelper::jsonObjectGetString(jo_task, "expectHashHex");
            const bool has_expect_sigvalid    = (json_object_has_value(jo_task, "expectSignatureValid") != 0);
            const bool expect_sig_valid       = (json_object_get_boolean(jo_task, "expectSignatureValid") > 0);

            //  --- Будуємо запит: {method, parameters} ---
            ParsonHelper req;
            req.create();
            req.setString("method", method);
            JSON_Object* jo_params_src = json_object_get_object(jo_task, "parameters");
            if (jo_params_src) {
                JSON_Object* jo_params_dst = req.setObject("parameters");
                json_object_copy_all_items(jo_params_dst, jo_params_src);
            }
            std::string s_request;
            req.serialize(s_request);
            if (s_request.empty()) {
                failures++;
                printf("[FAIL] task #%zu method='%s': cannot serialize request\n", i + 1, method.c_str());
                continue;
            }

            //  --- Виклик крипто-ядра ---
            char* res = process(s_request.c_str());
            if (!res) {
                failures++;
                printf("[FAIL] task #%zu method='%s': process() returned null\n", i + 1, method.c_str());
                continue;
            }

            executed++;
            bool task_ok = true;
            std::string detail;

            ParsonHelper resp;
            if (!resp.parse(res, false)) {
                task_ok = false;
                detail += " [response is not valid JSON]";
            }

            //  errorCode — на КОРЕНІ відповіді
            const int error_code = resp.rootObject()
                ? (int)json_object_get_number(resp.rootObject(), "errorCode")
                : -1;
            JSON_Object* jo_result = resp.rootObject()
                ? json_object_get_object(resp.rootObject(), "result")
                : nullptr;

            //  1) Перевірка errorCode
            if (expect_error) {
                if (error_code == 0) { task_ok = false; detail += " [expected errorCode!=0 but got 0]"; }
            }
            else {
                if (error_code != 0) {
                    task_ok = false;
                    const char* err = resp.rootObject() ? json_object_get_string(resp.rootObject(), "error") : nullptr;
                    detail += " [unexpected errorCode=" + std::to_string(error_code) + "]";
                    if (err) detail += std::string(" error='") + err + "'";
                }
            }

            //  2) INIT: result.countCmProviders
            if (task_ok && has_expect_count) {
                const int got = jo_result ? (int)json_object_get_number(jo_result, "countCmProviders") : -1;
                if (got != expect_count_prov) {
                    task_ok = false;
                    detail += " [countCmProviders expected " + std::to_string(expect_count_prov)
                            + " got " + std::to_string(got) + "]";
                }
            }

            //  3) DIGEST: result.bytes (base64) -> hex, звірка з expectHashHex
            if (task_ok && has_expect_hash) {
                const char* b64 = jo_result ? json_object_get_string(jo_result, "bytes") : nullptr;
                std::string raw, got_hex;
                if (!b64 || !base64ToBytes(b64, raw)) {
                    task_ok = false;
                    detail += " [DIGEST: no/invalid result.bytes]";
                }
                else {
                    got_hex = bytesToHexLower(raw);
                    if (got_hex != toLower(expect_hash_hex)) {
                        task_ok = false;
                        detail += " [hash expected " + toLower(expect_hash_hex) + " got " + got_hex + "]";
                    }
                }
            }

            //  4) VERIFY: вердикт підпису.
            //     Джерело вердикту:
            //       P7S/CAdES -> signatureInfos[0].status ("TOTAL-VALID" == валідно).
            //                    Саме цей ХОЛІСТИЧНИЙ статус, а НЕ statusSignature, ловить
            //                    невідповідність messageDigest у detached-даних: при
            //                    пошкодженому content statusSignature лишається "VALID"
            //                    (підпис над signedAttributes коректний), а status стає
            //                    "TOTAL-FAILED" (verify.cpp: joSignInfo.status/statusMessageDigest).
            //                    Якщо поля "status" немає — резерв: statusSignature.
            //       RAW       -> result.statusSignature ("VALID..." == валідно).
            if (task_ok && has_expect_sigvalid) {
                const char* verdict    = nullptr;   //  холістичний "status" ("TOTAL-VALID" => валідно)
                const char* status_sig = nullptr;   //  резерв / RAW ("VALID..." => валідно)
                if (jo_result) {
                    JSON_Array* ja_sinfos = json_object_get_array(jo_result, "signatureInfos");
                    if (ja_sinfos && json_array_get_count(ja_sinfos) > 0) {
                        JSON_Object* jo_si0 = json_array_get_object(ja_sinfos, 0);
                        if (jo_si0) {
                            verdict    = json_object_get_string(jo_si0, "status");
                            status_sig = json_object_get_string(jo_si0, "statusSignature");
                        }
                    }
                    else {
                        status_sig = json_object_get_string(jo_result, "statusSignature");
                    }
                }

                bool is_valid = false;
                std::string got;
                if (verdict) {
                    is_valid = (std::string(verdict) == "TOTAL-VALID");
                    got = std::string("status='") + verdict + "'";
                }
                else if (status_sig) {
                    is_valid = (std::string(status_sig).rfind("VALID", 0) == 0);
                    got = std::string("statusSignature='") + status_sig + "'";
                }
                else {
                    task_ok = false;
                    detail += " [VERIFY: no status/statusSignature found]";
                }

                if (task_ok && (verdict || status_sig) && (is_valid != expect_sig_valid)) {
                    task_ok = false;
                    detail += " [signature valid expected " + std::string(expect_sig_valid ? "true" : "false")
                            + " got " + got + "]";
                }
            }

            json_free(res);

            if (task_ok) {
                printf("[PASS] task #%zu method='%s' errorCode=%d%s%s\n",
                       i + 1, method.c_str(), error_code,
                       comment ? " : " : "", comment ? comment : "");
            }
            else {
                failures++;
                printf("[FAIL] task #%zu method='%s' errorCode=%d%s%s\n",
                       i + 1, method.c_str(), error_code, detail.c_str(),
                       comment ? " | " : "");
                if (comment) printf("       comment: %s\n", comment);
            }
        }

        printf("\n==== uapki_selftest: executed=%d, failures=%d ====\n", executed, failures);
        return (failures > 0) ? 1 : 0;
    }
    catch (const std::exception& e) {
        printf("[FATAL] exception: %s\n", e.what());
        return 2;
    }
    catch (...) {
        puts("[FATAL] unknown exception");
        return 2;
    }
}


/*
 * =====================  КОНТРАКТ ФОРМАТУ СЦЕНАРІЮ  ============================
 *
 * Сценарій — JSON-об'єкт з масивом "tasks". Дозволено //-коментарі (парситься
 * з withComments=true). Кожен task — об'єкт із такими полями:
 *
 *   Стандартні (сумісні з upstream test.cpp):
 *     "method"      String  — метод ядра ("INIT","DIGEST","VERIFY","DEINIT",...).
 *                             Порожній / такий, що починається з '_' — task пропускається
 *                             (спец-методи upstream _DIGEST/_NEW_THREAD/... НЕ підтримуються).
 *     "skip"        Bool    — true => task пропускається. Опц., default false.
 *     "comment"     String  — довільний коментар для логів. Опц.
 *     "parameters"  Object  — параметри методу; копіюються as-is у запит.
 *                             ПОЛЯ ОЧІКУВАНЬ (нижче) сюди НЕ потрапляють. Опц.
 *
 *   Поля очікувань (siblings до "method"/"parameters", читаються ДО побудови запиту):
 *     "expectError"             Bool  — очікуємо errorCode != 0 у відповіді.
 *                                       Опц., default false (=> очікуємо errorCode == 0).
 *                                       Застосовне до будь-якого методу.
 *     "expectCountCmProviders"  Int   — для INIT: result.countCmProviders МАЄ дорівнювати цьому.
 *                                       Опц.
 *     "expectHashHex"           String— для DIGEST: result.bytes (base64) декодується й
 *                                       hex-кодується; порівняння регістронезалежне. Опц.
 *     "expectSignatureValid"    Bool  — для VERIFY: вердикт підпису МАЄ бути валідним(true)/
 *                                       невалідним(false). Джерело вердикту:
 *                                         P7S/CAdES -> result.signatureInfos[0].status
 *                                                      (валідно == "TOTAL-VALID"). Цей холістичний
 *                                                      статус ловить і пошкоджений detached-content
 *                                                      (тоді statusSignature="VALID", а status=
 *                                                      "TOTAL-FAILED"). Резерв — statusSignature.
 *                                         RAW       -> result.statusSignature ("VALID..." == валідно).
 *                                       Опц.
 *
 *   Кілька полів очікувань в одному task допускаються (усі перевіряються).
 *
 * Код повернення exe: 0 — усі виконані task пройшли; 1 — є провали; 2 — фатальна помилка.
 * =============================================================================
 */
