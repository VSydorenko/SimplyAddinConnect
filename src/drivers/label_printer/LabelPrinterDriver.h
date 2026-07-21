#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include "../../platform/ResultEnvelope.h"
#include "LabelModel.h"
#include "LabelRaster.h"

class ITransport;

namespace labelprinter {

// Оркестратор друку етикеток: мульти-DeviceID, batch state machine, concurrency.
// Друк односпрямований і синхронний, тож DeviceSession/JobEngine НЕ використовуються —
// драйвер сам володіє ITransport на кожен зареєстрований пристрій.
//
// Життєвий цикл: Connect(profile) -> DeviceID; InitializePrinter/PrintLabels за DeviceID;
// Disconnect. Пакетний друк іде трьома станами packageStatus:
//   "first"          — скидає старий кеш формату, зберігає новий і друкує;
//   "regular"/"last" — вимагають активного кешу (інакше Fail "BAD_INPUT");
//   "last"           — після друку очищає кеш формату.
class LabelPrinterDriver {
public:
    LabelPrinterDriver();
    ~LabelPrinterDriver();

    LabelPrinterDriver(const LabelPrinterDriver&) = delete;
    LabelPrinterDriver& operator=(const LabelPrinterDriver&) = delete;

    // Реєструє пристрій за профілем підключення й повертає детермінований DeviceID
    // (лічильник рядком). Транспорт створюється одразу (лінива Open — при першому Send).
    std::string Connect(const DeviceProfile& profile);

    // Надсилає ініціалізаційний блок ZPL (темність/швидкість/розмір) на пристрій.
    ResultEnvelope InitializePrinter(const std::string& deviceId);

    // Друкує пакет етикеток. packageStatus керує кешем формату (див. опис класу).
    // Успіх = усі етикетки пакета прийнято транспортом (у чергу / у сокет), НЕ фізичний друк.
    // Payload при успіху: {acceptedInstances, acceptedCopies}.
    ResultEnvelope PrintLabels(const std::string& deviceId, const LabelBatch& batch, const std::string& packageStatus);

    // Знімає реєстрацію пристрою й закриває його транспорт (best-effort).
    void Disconnect(const std::string& deviceId);

    // true, якщо DeviceID зареєстровано.
    bool IsConnected(const std::string& deviceId) const;

    // Тест-хук: підмінити фабрику транспорту (напр. фейковий ITransport-захоплювач).
    // Задавати ДО Connect. Порожня фабрика → реальний MakeTransport (spooler/tcp).
    using TransportFactory = std::function<std::unique_ptr<ITransport>(const DeviceProfile&)>;
    void SetTransportFactoryForTest(TransportFactory factory);

private:
    // Контекст одного пристрою. Власний m серіалізує генерацію+Send цього пристрою,
    // не блокуючи інші пристрої (registryMutex_ тримається лише навколо map).
    struct DeviceContext {
        std::unique_ptr<ITransport> transport;
        std::optional<LabelFormatting> cachedFormatting;
        DeviceProfile profile;
        std::mutex m;
    };

    std::unique_ptr<ITransport> MakeTransport(const DeviceProfile& profile) const;

    // Пошук контексту під registryMutex_; повертає сирий вказівник (валідний, доки
    // виклик тримає ctx->m і немає паралельного Disconnect саме цього DeviceID).
    DeviceContext* Lookup(const std::string& deviceId) const;

    // Забезпечує відкритий транспорт і надсилає всі байти (all-or-error).
    static ResultEnvelope SendBytes(DeviceContext& ctx, const std::vector<uint8_t>& bytes);

    // GDI+ живе весь час життя драйвера (= час життя компоненти, конструюється поза DllMain),
    // тож растровий шар LabelRaster::Render у продакшн-шляху BuildLabel завжди має ініціалізований
    // GDI+ — без цього члена растр (текст/картинки/рамки) тихо випадав би, лишаючи самі штрихкоди.
    // Перший член: конструюється до реєстру, руйнується після нього.
    GdiplusRuntime gdiplus_;

    mutable std::mutex registryMutex_;                            // лише навколо devices_/nextId_
    std::map<std::string, std::unique_ptr<DeviceContext>> devices_;
    unsigned long long nextId_ = 1;                              // джерело детермінованих DeviceID
    TransportFactory transportFactory_;                          // тест-фабрика; порожня → MakeTransport
};

} // namespace labelprinter
