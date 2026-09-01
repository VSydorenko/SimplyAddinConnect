#include "../core/pch.h"
#include "AddinEcrBpo4000.h"
#include "../helpers/ServiceTools.h"
#include <cstdlib>

// Клас у 1С: "AddIn.<символьне ім'я>.ECRPrivatBPO4000".
REGISTER_COMPONENT(u"ECRPrivatBPO4000", AddinEcrBpo4000)

namespace {

/// Розбір суми з payload термінала. Локаль процесу — "C", тож strtod чекає крапку.
bool TryParseAmount(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || (end && *end != '\0')) return false;
    out = v;
    return true;
}

} // namespace

AddinEcrBpo4000::AddinEcrBpo4000() {
    REPORT_INFO("Ініціалізація БПО-фасаду еквайрингу, ревізія 4000");
    RegisterSystemMethods();
    RegisterAcquiringMethods();
    RegisterPaymentMethods();
}

void AddinEcrBpo4000::RegisterPaymentMethods() {

    // Дев'ятка оплати/повернення:
    //   (ИДУстройства, НомерМерчанта, РеквизитыКартыQR, СуммаОперации,
    //    НомерКарты, НомерЧека, СсылочныйНомер, КодАвторизации, ТекстСлипЧека)
    // Читаються назад Параметры[1..8], тобто ВСЕ після ИДУстройства — IN/OUT.
    const std::vector<ParamSpec> nine{
        ParamSpec{ u"DeviceID",      u"ИДУстройства",     false, {} },
        ParamSpec{ u"MerchantID",    u"НомерМерчанта",    false, {} },
        ParamSpec{ u"CardQRData",    u"РеквизитыКартыQR", false, {} },
        ParamSpec{ u"Amount",        u"СуммаОперации",    false, {} },
        ParamSpec{ u"CardNumber",    u"НомерКарты",       false, {} },
        ParamSpec{ u"ReceiptNumber", u"НомерЧека",        false, {} },
        ParamSpec{ u"RRN",           u"СсылочныйНомер",   false, {} },
        ParamSpec{ u"AuthCode",      u"КодАвторизации",   false, {} },
        ParamSpec{ u"SlipText",      u"ТекстСлипЧека",    false, {} },
    };

    // Хвіст результату однаковий у всіх чотирьох методів, різниться лише зсув:
    // у дев'ятці він починається з позиції 4, у десятках — з позиції 5.
    auto fillTail = [](const ResultEnvelope& env,
                       VH cardNo, VH receiptNo, VH rrn, VH authCode, VH slip) {
        const std::string pan = PayloadStr(env, "cardPAN");
        if (!pan.empty()) cardNo = pan;
        const std::string invoice = PayloadStr(env, "invoiceNumber");
        if (!invoice.empty()) receiptNo = invoice;
        const std::string rrnValue = PayloadStr(env, "rrn");
        if (!rrnValue.empty()) rrn = rrnValue;
        const std::string auth = PayloadStr(env, "approvalCode");
        if (!auth.empty()) authCode = auth;
        slip = PayloadStr(env, "receiptText");
    };

    // СуммаОперации читається назад у всіх чотирьох (Параметры[3]) — при частковому
    // схваленні вона відрізняється від запитаної. Перетираємо лише розбірне значення.
    auto fillAmount = [](const ResultEnvelope& env, VH amount) {
        double actual = 0.0;
        if (TryParseAmount(PayloadStr(env, "amount"), actual)) amount = actual;
    };

    AddFunction(u"PayByPaymentCard", u"ОплатитьПлатежнойКартой",
        Ret([this, fillTail, fillAmount](VH deviceId, VH, VH, VH amount,
                                         VH cardNo, VH receiptNo, VH rrn,
                                         VH authCode, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            // НомерМерчанта і РеквизитыКартыQR наш драйвер не використовує:
            // ПриватБанк-протокол мерчанта бере зі своїх налаштувань, а QR ми не
            // вміємо (прапорець ConsumerPresentedQR знято, тож 1С його й не надішле).
            const ResultEnvelope env = RunPurchase(VariantToDouble(amount));
            if (!MapEnvToBool(env)) return false;
            fillAmount(env, amount);
            fillTail(env, cardNo, receiptNo, rrn, authCode, slip);
            return true;
        }),
        nine);

    AddFunction(u"ReturnPaymentByPaymentCard", u"ВернутьПлатежПоПлатежнойКарте",
        Ret([this, fillTail, fillAmount](VH deviceId, VH, VH, VH amount,
                                         VH cardNo, VH receiptNo, VH rrn,
                                         VH authCode, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            const std::string originalRrn = VariantToString(rrn);
            const ResultEnvelope env = RunRefund(VariantToDouble(amount), originalRrn);
            if (!MapEnvToBool(env)) return false;
            fillAmount(env, amount);
            fillTail(env, cardNo, receiptNo, rrn, authCode, slip);
            return true;
        }),
        nine);

    // Скасування — ДЕСЯТКА: на позиції 4 додається СуммаОригинальнойОперации,
    // решта зсунута на одну.
    //
    // ⚠️⚠️ ПАСТКА: позицію 4 конфігурація НАЗАД НЕ ЧИТАЄ (читання стрибає з
    // Параметры[3] одразу на [5]). Це єдиний IN-only параметр на цій ревізії серед
    // сусідів-IN/OUT — записане туди зникне без винятку й без сліду. Тому параметр
    // приймаємо, використовуємо для рішення про часткове скасування, і НЕ пишемо.
    AddFunction(u"CancelPaymentByPaymentCard", u"ОтменитьПлатежПоПлатежнойКарте",
        Ret([this, fillTail, fillAmount](VH deviceId, VH, VH, VH amount,
                                         VH originalAmount, VH cardNo, VH receiptNo,
                                         VH rrn, VH authCode, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;

            // Часткове скасування дозволено лише коли драйвер його задекларував.
            // Конфігурація мала б відсіяти виклик ДО драйвера за прапорцем
            // PartialCancellation, але якщо він усе-таки дійшов (інша гілка, інша
            // збірка) — відмовляємо явно, а не мовчки скасовуємо на іншу суму.
            const double original = VariantToDouble(originalAmount);
            if (original > 0.0 && !Capabilities().partialCancellation) {
                return MapEnvToBool(Unsupported("Часткове скасування"));
            }

            const ResultEnvelope env = RunVoid(VariantToDouble(amount), VariantToString(rrn));
            if (!MapEnvToBool(env)) return false;
            fillAmount(env, amount);
            fillTail(env, cardNo, receiptNo, rrn, authCode, slip);
            return true;
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID",       u"ИДУстройства",              false, {} },
            ParamSpec{ u"MerchantID",     u"НомерМерчанта",             false, {} },
            ParamSpec{ u"CardQRData",     u"РеквизитыКартыQR",          false, {} },
            ParamSpec{ u"Amount",         u"СуммаОперации",             false, {} },
            ParamSpec{ u"OriginalAmount", u"СуммаОригинальнойОперации", false, {} },
            ParamSpec{ u"CardNumber",     u"НомерКарты",                false, {} },
            ParamSpec{ u"ReceiptNumber",  u"НомерЧека",                 false, {} },
            ParamSpec{ u"RRN",            u"СсылочныйНомер",            false, {} },
            ParamSpec{ u"AuthCode",       u"КодАвторизации",            false, {} },
            ParamSpec{ u"SlipText",       u"ТекстСлипЧека",             false, {} } });

    // Видача готівки — друга ДЕСЯТКА, з іншим змістом позиції 4 (СуммаНаличных).
    // Драйвер операції не має; прапорець CashWithdrawal знято, тож конфігурація
    // відсіє виклик раніше. Метод усе одно зареєстровано — вимога ІТС §1.3.
    AddFunction(u"PayByPaymentCardWithCashWithdrawal", u"ОплатитьПлатежнойКартойCВыдачейНаличных",
        Ret([this](VH deviceId, VH, VH, VH, VH, VH, VH, VH, VH, VH) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            return MapEnvToBool(Unsupported("ОплатитьПлатежнойКартойCВыдачейНаличных"));
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID",       u"ИДУстройства",     false, {} },
            ParamSpec{ u"MerchantID",     u"НомерМерчанта",    false, {} },
            ParamSpec{ u"CardQRData",     u"РеквизитыКартыQR", false, {} },
            ParamSpec{ u"Amount",         u"СуммаОперации",    false, {} },
            ParamSpec{ u"CashAmount",     u"СуммаНаличных",    false, {} },
            ParamSpec{ u"CardNumber",     u"НомерКарты",       false, {} },
            ParamSpec{ u"ReceiptNumber",  u"НомерЧека",        false, {} },
            ParamSpec{ u"RRN",            u"СсылочныйНомер",   false, {} },
            ParamSpec{ u"AuthCode",       u"КодАвторизации",   false, {} },
            ParamSpec{ u"SlipText",       u"ТекстСлипЧека",    false, {} } });

    // Досяжний лише з ревізії 4000 (на 3004 конфігурація відмовляє сама, не
    // доходячи до драйвера). Термінал списку операцій не віддає; прапорець
    // ListCardTransactions знято.
    AddFunction(u"GetCardTransactions", u"ПолучитьОперацииПоКартам",
        Ret([this](VH deviceId, VH) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            return MapEnvToBool(Unsupported("ПолучитьОперацииПоКартам"));
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID",     u"ИДУстройства", false, {} },
                                ParamSpec{ u"Transactions", u"XMLОпераций",  false, {} } });

    REPORT_INFO("Реєстрація платіжних методів ревізії 4000 завершена");
}
