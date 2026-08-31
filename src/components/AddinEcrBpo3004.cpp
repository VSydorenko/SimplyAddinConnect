#include "../core/pch.h"
#include "AddinEcrBpo3004.h"
#include "../helpers/ServiceTools.h"
#include <cstdlib>

// Клас у 1С: "AddIn.<символьне ім'я>.ECRPrivatBPO3004". Адміністратор обирає його
// записом довідника драйверів — автовизначення ревізії в БПО немає (§2.3 доку).
REGISTER_COMPONENT(u"ECRPrivatBPO3004", AddinEcrBpo3004)

namespace {

/// Розбір суми з payload термінала. Локаль у процесі — "C", тож strtod чекає крапку.
/// Не вдалося розібрати → false, і тоді вхідне значення НЕ перетирається.
bool TryParseAmount(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || (end && *end != '\0')) return false;
    out = v;
    return true;
}

} // namespace

AddinEcrBpo3004::AddinEcrBpo3004() {
    REPORT_INFO("Ініціалізація БПО-фасаду еквайрингу, ревізія 3004");
    RegisterSystemMethods();
    RegisterPaymentMethods();
}

void AddinEcrBpo3004::RegisterPaymentMethods() {

    // Спільна сімка ревізії 3004 для оплати/повернення/скасування:
    //   (ИДУстройства, НомерКарты, СуммаОперации, НомерЧека, СсылочныйНомер,
    //    КодАвторизации, ТекстСлипЧека)
    // Усі ШІСТЬ параметрів після ИДУстройства — IN/OUT: конфігурація читає їх назад
    // (Параметры[1..6]). Позиція 0 — тільки IN.
    const std::vector<ParamSpec> seven{
        ParamSpec{ u"DeviceID",      u"ИДУстройства",   false, {} },
        ParamSpec{ u"CardNumber",    u"НомерКарты",     false, {} },
        ParamSpec{ u"Amount",        u"СуммаОперации",  false, {} },
        ParamSpec{ u"ReceiptNumber", u"НомерЧека",      false, {} },
        ParamSpec{ u"RRN",           u"СсылочныйНомер", false, {} },
        ParamSpec{ u"AuthCode",      u"КодАвторизации", false, {} },
        ParamSpec{ u"SlipText",      u"ТекстСлипЧека",  false, {} },
    };

    // Заповнення OUT-частини після успішної операції. Суму перетираємо ЛИШЕ якщо
    // термінал повернув розбірне значення: при частковому схваленні воно відрізняється
    // від запитаного, і саме тому конфігурація читає її назад.
    auto fillOut = [](const ResultEnvelope& env,
                      VH cardNo, VH amount, VH receiptNo, VH rrn, VH authCode, VH slip) {
        const std::string pan = PayloadStr(env, "cardPAN");
        if (!pan.empty()) cardNo = pan;

        double actual = 0.0;
        if (TryParseAmount(PayloadStr(env, "amount"), actual)) amount = actual;

        const std::string invoice = PayloadStr(env, "invoiceNumber");
        if (!invoice.empty()) receiptNo = invoice;

        const std::string rrnValue = PayloadStr(env, "rrn");
        if (!rrnValue.empty()) rrn = rrnValue;

        const std::string auth = PayloadStr(env, "approvalCode");
        if (!auth.empty()) authCode = auth;

        // ТекстСлипЧека додатково лягає в ПодключаемоеОборудование.ПоследнийСлипЧек —
        // звідти працює команда «Надрукувати останній сліп-чек».
        slip = PayloadStr(env, "receiptText");
    };

    AddFunction(u"PayByPaymentCard", u"ОплатитьПлатежнойКартой",
        Ret([this, fillOut](VH deviceId, VH cardNo, VH amount, VH receiptNo,
                            VH rrn, VH authCode, VH slip) -> bool {
            const std::string id = deviceId;
            if (!CheckDeviceId(id)) return false;
            const ResultEnvelope env = RunPurchase(static_cast<double>(amount));
            if (!MapEnvToBool(env)) return false;
            fillOut(env, cardNo, amount, receiptNo, rrn, authCode, slip);
            return true;
        }),
        seven);

    AddFunction(u"ReturnPaymentByPaymentCard", u"ВернутьПлатежПоПлатежнойКарте",
        Ret([this, fillOut](VH deviceId, VH cardNo, VH amount, VH receiptNo,
                            VH rrn, VH authCode, VH slip) -> bool {
            const std::string id = deviceId;
            if (!CheckDeviceId(id)) return false;
            // Повернення робиться за RRN вихідної операції — він приходить у
            // СсылочныйНомер і на цій позиції ж повертається новим значенням.
            const std::string originalRrn = rrn;
            const ResultEnvelope env = RunRefund(static_cast<double>(amount), originalRrn);
            if (!MapEnvToBool(env)) return false;
            fillOut(env, cardNo, amount, receiptNo, rrn, authCode, slip);
            return true;
        }),
        seven);

    // Скасування драйвер не реалізує. Реєструємо метод і чесно відмовляємо —
    // вимога ІТС §1.3; мовчазний успіх тут був би найгіршим варіантом.
    // На 3004 прапорця ЧастичнаяОтмена перед викликом немає, тож ця гілка ДОСЯЖНА.
    AddFunction(u"CancelPaymentByPaymentCard", u"ОтменитьПлатежПоПлатежнойКарте",
        Ret([this](VH deviceId, VH, VH, VH, VH, VH, VH) -> bool {
            const std::string id = deviceId;
            if (!CheckDeviceId(id)) return false;
            return MapEnvToBool(RunVoid());
        }),
        seven);

    AddFunction(u"CardDayTotals", u"ИтогиДняПоКартам",
        Ret([this](VH deviceId, VH slip) -> bool {
            const std::string id = deviceId;
            if (!CheckDeviceId(id)) return false;
            const ResultEnvelope env = RunDayTotals();
            if (!MapEnvToBool(env)) return false;
            slip = PayloadStr(env, "receipt");
            return true;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства",  false, {} },
                                ParamSpec{ u"SlipText", u"ТекстСлипЧека", false, {} } });

    AddFunction(u"EmergencyCancelOperation", u"АварийнаяОтменаОперации",
        Ret([this](VH deviceId) -> bool {
            const std::string id = deviceId;
            if (!CheckDeviceId(id)) return false;
            return MapEnvToBool(RunEmergencyVoid());
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    REPORT_INFO("Реєстрація платіжних методів ревізії 3004 завершена");
}
