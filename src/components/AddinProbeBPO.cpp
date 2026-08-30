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

    // ---- Зонд 0.5: дев'ять параметрів, чотири з них IN/OUT ----
    // Розкладка повторює ОплатитьПлатежнойКартой: 0-2 — вхідні, 3-6 — IN/OUT
    // (НомерКарты/НомерЧека/СсылочныйНомер/КодАвторизации), 7-8 — вихідні.
    // Повертає JSON із тим, що ПРОЧИТАЛО, — щоб 1С звірила обидва напрямки.
    AddFunction(u"InOut9", u"ВходВыход9",
        Ret([](VH p0, VH p1, VH p2, VH p3, VH p4, VH p5, VH p6, VH p7, VH p8) -> std::string {
            const std::string in0 = p0, in1 = p1, in2 = p2, in3 = p3,
                              in4 = p4, in5 = p5, in6 = p6, in7 = p7, in8 = p8;
            // IN/OUT: читаємо й перезаписуємо ТІ САМІ слоти.
            p3 = std::string("<") + in3 + ">";
            p4 = std::string("<") + in4 + ">";
            p5 = std::string("<") + in5 + ">";
            p6 = std::string("<") + in6 + ">";
            // Чисті OUT.
            p7 = std::string("OUT7");
            p8 = std::string("OUT8");
            return std::string("{\"read\":[\"")
                + JsonEscape(in0) + "\",\"" + JsonEscape(in1) + "\",\"" + JsonEscape(in2)
                + "\",\"" + JsonEscape(in3) + "\",\"" + JsonEscape(in4) + "\",\"" + JsonEscape(in5)
                + "\",\"" + JsonEscape(in6) + "\",\"" + JsonEscape(in7) + "\",\"" + JsonEscape(in8)
                + "\"]}";
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"p0", u"П0", false, {} }, ParamSpec{ u"p1", u"П1", false, {} },
            ParamSpec{ u"p2", u"П2", false, {} }, ParamSpec{ u"p3", u"П3", false, {} },
            ParamSpec{ u"p4", u"П4", false, {} }, ParamSpec{ u"p5", u"П5", false, {} },
            ParamSpec{ u"p6", u"П6", false, {} }, ParamSpec{ u"p7", u"П7", false, {} },
            ParamSpec{ u"p8", u"П8", false, {} } });

    REPORT_INFO("Реєстрація методів ProbeBPO завершена");
}
