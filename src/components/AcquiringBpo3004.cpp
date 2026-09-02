#include "../core/pch.h"
#include "AcquiringBpo3004.h"
#include "../platform/MoneyFormat.h"   // TryMoneyFromString — пара до MoneyToString
#include "../helpers/ServiceTools.h"

AcquiringBpo3004::AcquiringBpo3004() {
    RegisterSystemMethods();
    RegisterAcquiringMethods();
    RegisterPaymentMethods();
}

void AcquiringBpo3004::RegisterPaymentMethods() {

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
        if (TryMoneyFromString(PayloadStr(env, "amount"), actual)) amount = actual;

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
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            const ResultEnvelope env = RunPurchase(VariantToDouble(amount));
            if (!MapEnvToBool(env)) return false;
            fillOut(env, cardNo, amount, receiptNo, rrn, authCode, slip);
            return true;
        }),
        seven);

    AddFunction(u"ReturnPaymentByPaymentCard", u"ВернутьПлатежПоПлатежнойКарте",
        Ret([this, fillOut](VH deviceId, VH cardNo, VH amount, VH receiptNo,
                            VH rrn, VH authCode, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            // Повернення робиться за RRN вихідної операції — він приходить у
            // СсылочныйНомер і на цій позиції ж повертається новим значенням.
            const std::string originalRrn = VariantToString(rrn);
            const ResultEnvelope env = RunRefund(VariantToDouble(amount), originalRrn);
            if (!MapEnvToBool(env)) return false;
            fillOut(env, cardNo, amount, receiptNo, rrn, authCode, slip);
            return true;
        }),
        seven);

    // Скасування (сторно) — сценарій «картку списано, а чек не пробився»; РМК кличе
    // його з ВыполнитьСторноОплатыПоКарте. Механізм (RunVoid): спершу питаємо
    // ВЛАСНУ операцію скасування драйвера; якщо в драйвера її немає (код
    // UNSUPPORTED) — за параметром `VoidAsRefund` (дефолт увімкнено) скасування
    // виконується ПОВЕРНЕННЯМ за RRN, а зі знятим параметром — чесна відмова
    // (§2.3.1 доку). Будь-яку ІНШУ помилку драйвера повертаємо як є: відкат на
    // повернення дозволений лише при UNSUPPORTED, бо при невизначеній відповіді
    // (напр. TIMEOUT) термінал міг операцію вже виконати, і повернення зрушило б
    // гроші вдруге.
    // На 3004 прапорця перед викликом немає, тож обидві гілки досяжні.
    AddFunction(u"CancelPaymentByPaymentCard", u"ОтменитьПлатежПоПлатежнойКарте",
        Ret([this, fillOut](VH deviceId, VH cardNo, VH amount, VH receiptNo,
                            VH rrn, VH authCode, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            const ResultEnvelope env = RunVoid(VariantToDouble(amount), VariantToString(rrn));
            if (!MapEnvToBool(env)) return false;
            fillOut(env, cardNo, amount, receiptNo, rrn, authCode, slip);
            return true;
        }),
        seven);

    // ИтогиДняПоКартам і АварийнаяОтменаОперации гілок за ревізією не мають —
    // вони зареєстровані в базі й однакові для всіх фасадів.

    REPORT_INFO("Реєстрація платіжних методів ревізії 3004 завершена");
}
