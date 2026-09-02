#include "../../core/pch.h"
#include "LabelPrinterDriver.h"
#include "LabelZplGenerator.h"
#include "../../transport/Transport.h"
#include "../../transport/Transport_TCP.h"
#include "../../transport/Transport_SpoolerRaw.h"
#include "../../helpers/ServiceTools.h"
#include <exception>

namespace labelprinter {

namespace {
constexpr const char* kTag = "LabelPrinterDriver";
}

// Конструктор/деструктор — поза класом: unique_ptr<ITransport> у DeviceContext вимагає
// повного типу ITransport при знищенні map, а він відомий лише в цьому TU.
LabelPrinterDriver::LabelPrinterDriver() = default;

LabelPrinterDriver::~LabelPrinterDriver() {
    // Закриваємо всі транспорти, що лишилися. Реєстр більше ніхто не чіпає (dtor).
    std::lock_guard<std::mutex> lk(registryMutex_);
    for (auto& kv : devices_) {
        auto& ctx = kv.second;
        if (ctx && ctx->transport && ctx->transport->IsOpen())
            ctx->transport->Close();
    }
    devices_.clear();
}

void LabelPrinterDriver::SetTransportFactoryForTest(TransportFactory factory) {
    transportFactory_ = std::move(factory);
}

std::unique_ptr<ITransport> LabelPrinterDriver::MakeTransport(const DeviceProfile& profile) const {
    if (profile.transport == DeviceProfile::Transport::Tcp)
        return std::make_unique<TransportTCP>(profile.host, profile.port);
    return std::make_unique<TransportSpoolerRaw>(profile.printerName);
}

std::shared_ptr<LabelPrinterDriver::DeviceContext>
LabelPrinterDriver::Lookup(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(registryMutex_);
    auto it = devices_.find(deviceId);
    return it == devices_.end() ? nullptr : it->second;   // shared-копія під замком
}

std::string LabelPrinterDriver::Connect(const DeviceProfile& profile) {
    auto ctx = std::make_shared<DeviceContext>();
    ctx->profile = profile;
    // Фабрику читаємо поза registryMutex_ (тест-хук ставиться до Connect). Створення
    // транспорту не тримає реєстр — щоб паралельні Connect не серіалізувалися на I/O-конструкторах.
    ctx->transport = transportFactory_ ? transportFactory_(profile) : MakeTransport(profile);

    std::lock_guard<std::mutex> lk(registryMutex_);
    std::string id = std::to_string(nextId_++);
    devices_.emplace(id, std::move(ctx));
    NEUTRAL_REPORT_INFO(kTag, "Підключено пристрій: " + id);
    return id;
}

void LabelPrinterDriver::Disconnect(const std::string& deviceId) {
    std::shared_ptr<DeviceContext> ctx;
    {
        std::lock_guard<std::mutex> lk(registryMutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) return;
        ctx = std::move(it->second);   // забираємо shared-власника з реєстру під registryMutex_
        devices_.erase(it);
    }
    // Закриття транспорту — поза registryMutex_ (I/O). Контекст уже вилучено з мапи, але
    // shared_ptr тримає його живим; in-flight операція зі своєю shared-копією теж
    // лишається валідною. lock(ctx->m) серіалізує закриття з будь-яким активним Send.
    if (ctx && ctx->transport) {
        std::lock_guard<std::mutex> lk(ctx->m);
        if (ctx->transport->IsOpen()) ctx->transport->Close();
    }
    NEUTRAL_REPORT_INFO(kTag, "Відключено пристрій: " + deviceId);
}

bool LabelPrinterDriver::IsConnected(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(registryMutex_);
    return devices_.count(deviceId) > 0;
}

ResultEnvelope LabelPrinterDriver::SendBytes(DeviceContext& ctx, const std::vector<uint8_t>& bytes) {
    // Усі транспортні відмови зводимо до єдиного коду TRANSPORT_ERROR (таксономія §9);
    // конкретику лишаємо в description.
    if (!ctx.transport)
        return ResultEnvelope::Fail("TRANSPORT_ERROR", "Транспорт пристрою не ініціалізовано");
    if (!ctx.transport->IsOpen()) {
        if (!ctx.transport->Open())
            return ResultEnvelope::Fail("TRANSPORT_ERROR", "Не вдалося відкрити канал до принтера");
    }
    const int sent = ctx.transport->Send(bytes);
    if (sent != static_cast<int>(bytes.size()))
        return ResultEnvelope::Fail("TRANSPORT_ERROR",
            "Транспорт прийняв " + std::to_string(sent) + " з " + std::to_string(bytes.size()) + " байтів");
    return ResultEnvelope::Ok();
}

ResultEnvelope LabelPrinterDriver::Probe(const std::string& deviceId) {
    std::shared_ptr<DeviceContext> ctx = Lookup(deviceId);
    if (!ctx)
        return ResultEnvelope::Fail("NOT_CONNECTED", "Пристрій не підключено: " + deviceId);

    std::lock_guard<std::mutex> lk(ctx->m);
    if (!ctx->transport)
        return ResultEnvelope::Fail("TRANSPORT_ERROR", "Транспорт пристрою не ініціалізовано");
    if (ctx->transport->IsOpen()) {
        NEUTRAL_REPORT_INFO(kTag, "Канал до пристрою вже відкритий: " + deviceId);
        return ResultEnvelope::Ok();
    }
    if (!ctx->transport->Open())
        return ResultEnvelope::Fail("TRANSPORT_ERROR", "Не вдалося відкрити канал до принтера");
    ctx->transport->Close();   // повертаємо ліниво-закритий стан
    NEUTRAL_REPORT_INFO(kTag, "Канал до пристрою доступний: " + deviceId);
    return ResultEnvelope::Ok();
}

ResultEnvelope LabelPrinterDriver::InitializePrinter(const std::string& deviceId) {
    std::shared_ptr<DeviceContext> ctx = Lookup(deviceId);
    if (!ctx)
        return ResultEnvelope::Fail("NOT_CONNECTED", "Пристрій не підключено: " + deviceId);

    std::lock_guard<std::mutex> lk(ctx->m);
    try {
        const std::vector<uint8_t> zpl = LabelZplGenerator::BuildInit(ctx->profile);
        ResultEnvelope res = SendBytes(*ctx, zpl);
        if (!res.ok) {
            NEUTRAL_REPORT_ERROR(kTag, "Помилка ініціалізації принтера " + deviceId + ": " + res.description);
            return res;
        }
        NEUTRAL_REPORT_INFO(kTag, "Ініціалізовано принтер: " + deviceId);
        return ResultEnvelope::Ok();
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, "Виняток під час ініціалізації " + deviceId + ": " + std::string(e.what()));
        return ResultEnvelope::Fail("EXCEPTION", std::string("Виняток під час ініціалізації: ") + e.what());
    }
}

ResultEnvelope LabelPrinterDriver::PrintLabels(const std::string& deviceId,
                                               const LabelBatch& batch,
                                               const std::string& packageStatus) {
    std::shared_ptr<DeviceContext> ctx = Lookup(deviceId);
    if (!ctx)
        return ResultEnvelope::Fail("NOT_CONNECTED", "Пристрій не підключено: " + deviceId);

    std::lock_guard<std::mutex> lk(ctx->m);

    const bool isFirst = (packageStatus == "first");

    // State machine кешу формату.
    if (isFirst) {
        if (!batch.formatting)
            return ResultEnvelope::Fail("BAD_INPUT", "Пакет 'first' без секції форматування");
        ctx->cachedFormatting = batch.formatting;   // reset старого стану + set нового
    } else {
        // Кеш формату встановлює ЛИШЕ 'first'. Без активного кешу 'regular'/'last' —
        // помилка вхідних даних, навіть якщо пакет несе власне форматування.
        if (!ctx->cachedFormatting)
            return ResultEnvelope::Fail("BAD_INPUT",
                "Пакет '" + packageStatus + "' без активного форматування (немає попереднього 'first')");
    }

    const LabelFormatting& fmt = ctx->cachedFormatting.value();

    long acceptedInstances = 0;
    long acceptedCopies = 0;
    try {
        for (size_t i = 0; i < batch.labels.size(); ++i) {
            const LabelInstance& inst = batch.labels[i];

            GenResult gr = LabelZplGenerator::BuildLabel(fmt, inst, ctx->profile);
            if (!gr.ok) {
                ResultEnvelope r = ResultEnvelope::Fail(gr.errCode, gr.errDesc);
                r.payload["failedIndex"] = static_cast<int>(i);
                r.payload["acceptedInstances"] = acceptedInstances;
                r.payload["acceptedCopies"] = acceptedCopies;
                r.description += " (прийнято " + std::to_string(acceptedInstances) + " з " +
                                 std::to_string(batch.labels.size()) + " екземплярів; збій на #" + std::to_string(i) + ")";
                NEUTRAL_REPORT_ERROR(kTag, "Помилка генерації етикетки #" + std::to_string(i) +
                                           " на " + deviceId + ": " + gr.errCode + " " + gr.errDesc);
                return r;
            }

            ResultEnvelope sres = SendBytes(*ctx, gr.zpl);
            if (!sres.ok) {
                sres.payload["failedIndex"] = static_cast<int>(i);
                sres.payload["acceptedInstances"] = acceptedInstances;
                sres.payload["acceptedCopies"] = acceptedCopies;
                sres.description += " (прийнято " + std::to_string(acceptedInstances) + " з " +
                                    std::to_string(batch.labels.size()) + " екземплярів; збій на #" + std::to_string(i) + ")";
                NEUTRAL_REPORT_ERROR(kTag, "Помилка відправлення етикетки #" + std::to_string(i) +
                                           " на " + deviceId + ": " + sres.description);
                return sres;
            }

            acceptedInstances += 1;
            acceptedCopies += (inst.quantity > 0 ? inst.quantity : 1);
        }
    } catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, "Виняток під час друку на " + deviceId + ": " + std::string(e.what()));
        return ResultEnvelope::Fail("EXCEPTION", std::string("Виняток під час друку: ") + e.what());
    }

    // "last" завершує пакет — скидаємо кеш формату.
    if (packageStatus == "last")
        ctx->cachedFormatting.reset();

    // MapResult: успіх → Ok({acceptedInstances, acceptedCopies}).
    nlohmann::json payload;
    payload["acceptedInstances"] = acceptedInstances;
    payload["acceptedCopies"] = acceptedCopies;
    NEUTRAL_REPORT_INFO(kTag, "Пакет '" + packageStatus + "' прийнято на " + deviceId +
                              ": екземплярів " + std::to_string(acceptedInstances) +
                              ", копій " + std::to_string(acceptedCopies));
    return ResultEnvelope::Ok(payload);
}

} // namespace labelprinter
