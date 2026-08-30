#include "../core/pch.h"
#include "AddinProbeBPO.h"
#include "../helpers/ServiceTools.h"
#include <chrono>
#include <thread>

// ТИМЧАСОВА зонд-компонента (див. AddinProbeBPO.h). Прибрати перед merge у main.
REGISTER_COMPONENT(u"ProbeBPO", AddinProbeBPO)

namespace {
// Екранування рядка для вкладення в JSON-відповідь зонда (без залежності від nlohmann).
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
    }
    return out;
}
} // namespace

AddinProbeBPO::AddinProbeBPO() {
    REPORT_INFO("Ініціалізація зонд-компоненти ProbeBPO (тимчасова, не для продакшену)");
    RegisterMethods();
}

AddinProbeBPO::~AddinProbeBPO() {
    REPORT_INFO("Завершення роботи зонд-компоненти ProbeBPO");
    ServiceTools::DisableComponentLogging(this);
}

void AddinProbeBPO::RegisterMethods() {
    // ---- Зонд 0.7: блокуючий метод, що паралельно шле події й рухає лічильник ----
    // Модель реального Purchase: потік виклику стоїть у RequestPrimary, а окремий
    // poller оновлює статус і пушить події. Питання зонда — чи бачить це 1С ПІД ЧАС
    // виклику, чи лише після повернення.
    AddFunction(u"BlockAndEmit", u"БлокироватьИСобытия",
        Ret([this](VH seconds, VH intervalMs) -> std::string {
            if (running_.exchange(true)) {
                return std::string("{\"error\":\"already running\"}");
            }
            const int secs = static_cast<int>(static_cast<int64_t>(seconds));
            const int step = static_cast<int>(static_cast<int64_t>(intervalMs));
            const int totalMs = (secs > 0 ? secs : 5) * 1000;
            const int stepMs = (step > 0 ? step : 500);

            tick_.store(0);
            emitted_.store(0);

            // Емітер — окремий потік (як poller драйвера ECR).
            std::thread emitter([this, totalMs, stepMs]() {
                int elapsed = 0;
                while (elapsed < totalMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
                    elapsed += stepMs;
                    const int n = tick_.fetch_add(1) + 1;
                    const std::string data =
                        "{\"tick\":" + std::to_string(n) +
                        ",\"elapsedMs\":" + std::to_string(elapsed) + "}";
                    if (this->PostExternalEvent(AddInNative::MB2WCHAR("tick"),
                                                AddInNative::MB2WCHAR(data)))
                        emitted_.fetch_add(1);
                }
            });

            // Потік виклику блокується — саме як на реальній оплаті.
            std::this_thread::sleep_for(std::chrono::milliseconds(totalMs));
            emitter.join();
            running_.store(false);

            const std::string out =
                "{\"blockedMs\":" + std::to_string(totalMs) +
                ",\"stepMs\":" + std::to_string(stepMs) +
                ",\"ticks\":" + std::to_string(tick_.load()) +
                ",\"emitted\":" + std::to_string(emitted_.load()) + "}";
            REPORT_INFO("BlockAndEmit завершено: " + out);
            return out;
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"seconds",    u"Секунды",    /*required*/false, DefaultHelper(static_cast<int64_t>(5)) },
            ParamSpec{ u"intervalMs", u"ИнтервалМс", /*required*/false, DefaultHelper(static_cast<int64_t>(500)) } });

    // Дешеве читання атоміка — аналог СтатусТерминала(). Саме його 1С має опитувати
    // з ПодключитьОбработчикОжидания під час BlockAndEmit.
    AddFunction(u"Ticks", u"Тики", Ret([this]() -> int { return tick_.load(); }));

    AddProcedure(u"Reset", u"Сбросить",
        MethFunction(std::function<void()>([this]() { tick_.store(0); emitted_.store(0); })));

    // ---- Зонд 0.5: репетиція СПРАВЖНЬОЇ сигнатури ОплатитьПлатежнойКартой ----
    // На ревізії 3004 (єдиній, що працює і в УНФ ru, і в BAS УНФ ua) метод має рівно сім
    // параметрів, і всі шість після ИДУстройства — IN/OUT:
    //   (ИДУстройства, НомерКарты, СуммаОперации, НомерЧека, СсылочныйНомер,
    //    КодАвторизации, ТекстСлипЧека)
    // Позиція 2 — ЧИСЛО, не рядок. Це окремий шлях у VariantHelper::clear()
    // (VTYPE_R8/VTYPE_I4 не викликають FreeMemory, на відміну від VTYPE_PWSTR),
    // і окремий ризик втрати дробової частини суми — тому читаємо/пишемо саме double.
    AddFunction(u"InOut7", u"ВходВыход7",
        Ret([](VH deviceId, VH cardNo, VH amount, VH receiptNo,
               VH rrn, VH authCode, VH slip) -> std::string {
            const std::string inDevice = deviceId;
            const std::string inCard = cardNo, inReceipt = receiptNo,
                              inRrn = rrn, inAuth = authCode, inSlip = slip;

            // ЯКИЙ САМЕ tVariant-тип 1С прислала для числа — це і є головне питання зонда.
            // VariantHelper::operator double() приймає лише I2/I4/UI1/ERROR/R4/R8 і кидає
            // на решті (I8, UI4, INT, …). Якщо платформа передає суму іншим типом, конверсію
            // в ядрі доведеться розширювати — але вгадувати не будемо, хай зонд покаже.
            const int amountVt = static_cast<int>(amount.type());
            double inAmount = 0.0;
            bool amountReadOk = true;
            try { inAmount = amount; }
            catch (...) { amountReadOk = false; }
            if (!amountReadOk) {
                return std::string("{\"error\":\"amount: непідтримуваний tVariant-тип\",\"amountVt\":")
                    + std::to_string(amountVt) + "}";
            }

            // IN/OUT: читаємо й перезаписуємо ТІ САМІ слоти (позицію 0 не чіпаємо — вона IN).
            cardNo    = std::string("<") + inCard + ">";
            amount    = inAmount + 0.01;      // видима зміна, що переживає лише double
            receiptNo = std::string("<") + inReceipt + ">";
            rrn       = std::string("<") + inRrn + ">";
            authCode  = std::string("<") + inAuth + ">";
            slip      = std::string("СЛІП\nрядок 2");

            return std::string("{\"deviceId\":\"") + JsonEscape(inDevice)
                + "\",\"cardNo\":\"" + JsonEscape(inCard)
                + "\",\"amountVt\":" + std::to_string(amountVt)
                + ",\"amount\":" + std::to_string(inAmount)
                + ",\"receiptNo\":\"" + JsonEscape(inReceipt)
                + "\",\"rrn\":\"" + JsonEscape(inRrn)
                + "\",\"authCode\":\"" + JsonEscape(inAuth)
                + "\",\"slip\":\"" + JsonEscape(inSlip) + "\"}";
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"deviceId",  u"ИДУстройства",   false, {} },
            ParamSpec{ u"cardNo",    u"НомерКарты",     false, {} },
            ParamSpec{ u"amount",    u"СуммаОперации",  false, {} },
            ParamSpec{ u"receiptNo", u"НомерЧека",      false, {} },
            ParamSpec{ u"rrn",       u"СсылочныйНомер", false, {} },
            ParamSpec{ u"authCode",  u"КодАвторизации", false, {} },
            ParamSpec{ u"slip",      u"ТекстСлипЧека",  false, {} } });

    REPORT_INFO("Реєстрація методів ProbeBPO завершена");
}
