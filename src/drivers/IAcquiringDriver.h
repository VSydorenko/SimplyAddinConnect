#pragma once
#include <map>
#include <string>
#include "../platform/ResultEnvelope.h"

/// Що термінал і протокол РЕАЛЬНО вміють. Це властивість ОБЛАДНАННЯ, а не фасаду:
/// саме тому прапорці живуть тут, а не захардкожені в ПараметрыТерминала.
/// Термінал, що вміє часткове скасування, вмикає його одним рядком у своєму драйвері.
///
/// Дефолти — усе вимкнено СВІДОМО: непідтримуване має бути станом за замовчуванням,
/// щоб забути ввімкнути було безпечно, а забути вимкнути — неможливо.
struct AcquiringCapabilities {
    std::string terminalId;               ///< TerminalParameters@TerminalID
    bool printSlipOnTerminal = false;     ///< термінал друкує квитанції сам
    bool shortSlip = false;
    bool cashWithdrawal = false;
    bool electronicCertificates = false;
    bool partialCancellation = false;
    bool consumerPresentedQR = false;
    bool listCardTransactions = false;
};

/// Чесна відмова у формі, якої вимагає ІТС §1.3: в описі помилки має бути прямо
/// сказано, що функція обладнанням не підтримується.
inline ResultEnvelope AcquiringUnsupported(const std::string& method) {
    return ResultEnvelope::Fail("UNSUPPORTED",
        "Операція \"" + method + "\" не підтримується обладнанням");
}

/// Межа, за якою ховається протокол еквайрингу. Фасад працює ВИКЛЮЧНО через неї.
///
/// ⚠️ Інтерфейс спроєктовано на ОДНОМУ відомому протоколі (ПриватБанк JSON), тож
/// ризик, що для BPOS1 він виявиться не зовсім тим, реальний і прийнятий свідомо.
/// Коли з'явиться друга реалізація — уточнюємо ЗА ФАКТОМ, а не вгадуємо наперед.
///
/// Операції мають ДЕФОЛТНІ реалізації з чесною відмовою: непідтримуване лишається
/// поведінкою за замовчуванням, а не обов'язком кожного драйвера.
class IAcquiringDriver {
public:
    virtual ~IAcquiringDriver() = default;

    // ---- з'єднання ----
    /// Параметри — сирою мапою з УстановитьПараметр: набір імен у кожного протоколу
    /// свій (він же й описує їх у SettingsXml), тож фасад у них не заглядає.
    virtual ResultEnvelope Open(const std::map<std::string, std::string>& params) = 0;
    virtual void Close() = 0;
    virtual bool IsConnected() const = 0;

    // ---- ідентичність і опис ----
    virtual std::string Vendor() const = 0;
    virtual std::string Model() const = 0;
    virtual std::string DriverName() const = 0;          ///< DriverDescription@Name
    virtual std::string DriverDescription() const = 0;   ///< @Description
    virtual std::string SettingsXml() const = 0;         ///< форма налаштувань цього протоколу
    virtual AcquiringCapabilities Capabilities() const = 0;
    /// Стабільний ключ ЦІЛІ підключення для цих параметрів (напр. "tcp://10.0.0.5:2000").
    /// Потрібен фасаду, щоб зрозуміти, чи веде нова конфігурація на той самий канал,
    /// що й активне з'єднання. Підключення не робить і стану не змінює.
    ///
    /// ⚠️ КРИТЕРІЙ СКЛАДУ: у ключ входить рівно те, зміна чого робить НАЯВНИЙ КАНАЛ
    /// НЕПРИДАТНИМ, — а не те, що «ідентифікує пристрій». Різниця не теоретична: Baud
    /// для COM лишає фізичну ціль тією самою, але вимагає перевідкриття каналу, тож у
    /// ключ входить. І навпаки — параметри поведінки операцій, таймаути й журналювання
    /// у ключ НЕ входять: їх зміна не робить перевірене підключення неперевіреним.
    ///
    /// ⚠️ НЕ КОНКАТЕНУЙ УСЮ МАПУ params. Це очевидна лінива реалізація, і вона тиха:
    /// зміна будь-якого стороннього параметра (VoidAsRefund, таймаут з'єднання, рівень
    /// журналювання — жоден з них ціллю не є) дасть хибне «ціль змінилась». При живому
    /// каналі це не «просто помилка»: фасад відкриє ДРУГУ сесію до ТОГО САМОГО
    /// монопольного термінала, поки перша ще жива, — результат: хибне «недоступно» на
    /// справному обладнанні плюс зайвий стук у зайнятий термінал. Компілятор цього не
    /// спіймає, тест сусіднього драйвера — теж.
    ///
    /// ⚠️ КЛЮЧ МАЄ БУТИ ДЕТЕРМІНОВАНИМ: еквівалентні написання зводь до одного вигляду.
    /// Порожній Host і Host="0.0.0.0", або TERM01 і term01 — якщо для Open() це та сама
    /// адреса, ключ зобов'язаний це показати: підставляй ті самі дефолти, що й при
    /// підключенні, і нормалізуй регістр там, де він не значущий. Інакше — той самий
    /// дефект іншим шляхом: хибне «ціль змінилась» на насправді незмінному каналі, друга
    /// сесія в монопольний термінал, хибне «недоступно» про справне обладнання.
    /// Реалізація ПриватБанку (EcrPrivatJsonAcquiring::BuildConnectionString) це вже
    /// робить — дефолти Port/Baud, up-case TransportKind — можна брати за зразок.
    ///
    /// ⚠️ ЗНАЧЕННЯ ВИДИМЕ: воно потрапляє в текст результату ТестУстройства (на екран
    /// каси) і в журнал. Тому НЕ включай у нього облікові дані — паролі, токени, ключі
    /// мерчанта. Якщо таке поле справді потрібне для порівняння, включай його у
    /// вигляді, непридатному для відновлення (напр. хеш), а не відкритим текстом.
    ///
    /// ⚠️ Свідомо ЧИСТО ВІРТУАЛЬНИЙ, без дефолту: ідентичність каналу зобов'язаний
    /// уміти кожен драйвер — це така сама його властивість, як Vendor()/SettingsXml().
    /// Дефолт тут був би небезпечний: драйвер, що забув його перевизначити, віддавав би
    /// однаковий ключ для різних терміналів, і ТестУстройства мовчки перевіряло б не ту
    /// адресу — рівно той дефект, проти якого цей метод і заведено.
    virtual std::string TargetKey(const std::map<std::string, std::string>& params) const = 0;
    /// Коротка перевірка зв'язку для ТестУстройства. Фінансових наслідків не має.
    virtual ResultEnvelope Probe() = 0;

    // ---- операції ----
    virtual ResultEnvelope Purchase(double amount) {
        (void)amount; return AcquiringUnsupported("Оплата");
    }
    virtual ResultEnvelope Refund(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return AcquiringUnsupported("Повернення");
    }
    /// Власна операція скасування протоколу. Немає — фасад піде шляхом VoidAsRefund.
    virtual ResultEnvelope Void(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return AcquiringUnsupported("Скасування");
    }
    virtual ResultEnvelope DayTotals()     { return AcquiringUnsupported("Підсумки дня по картах"); }
    virtual ResultEnvelope Audit()         { return AcquiringUnsupported("X-звіт"); }
    virtual ResultEnvelope EmergencyVoid() { return AcquiringUnsupported("Аварійне скасування"); }

    // ---- асинхронне розширення (не БПО; його бере розширення 1С із точки РМК) ----
    virtual bool StartPurchase(double amount) { (void)amount; return false; }
    virtual bool StartRefund(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return false;
    }
    /// 0 Idle, 1 Running, 2 Interrupting, 3 Done, 4 Error (значення JobState).
    virtual int  OperationState() const { return 0; }
    virtual bool TryGetOperationResult(ResultEnvelope& out) const { (void)out; return false; }
    /// Онлайн-статус термінала, -1..11; -1 = ще не було.
    virtual int  LastStatus() const { return -1; }
    virtual void CancelOperation() {}
};
