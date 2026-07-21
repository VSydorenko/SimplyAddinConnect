#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <chrono>
#include <atomic>
#include <nlohmann/json.hpp>
#include "../../platform/ResultEnvelope.h"
#include "../../platform/JobEngine.h"

class DeviceSession;
class ITransport;
struct RequestResult;

/// Розібраний рядок підключення.
struct EcrConnParams {
    enum class Kind { Tcp, Com } kind = Kind::Tcp;
    std::string host;        ///< для Tcp
    int tcpPort = 2000;      ///< для Tcp
    std::string comPort;     ///< для Com, напр. "COM3"
    int baud = 115200;       ///< для Com
};

/// Пілотний драйвер ECRPrivatJSON: розбір підключення, фабрика транспорту, життєвий
/// цикл за еталонною схемою (спека §6). Операції — Частина 2.
class EcrPrivatJsonDriver {
public:
    // Конструктор/деструктор — поза класом (у .cpp): unique_ptr<DeviceSession> над
    // forward-оголошеним типом вимагає повного типу при інстанціюванні спецчленів;
    // out-of-line визначення тримає цю вимогу в TU драйвера (не протікає у фасад).
    EcrPrivatJsonDriver();
    ~EcrPrivatJsonDriver();

    /// Розбір рядка підключення → out. false, якщо не розібрано:
    ///   tcp://host:port — host як IPv4/hostname (IPv6-літерали не підтримуються);
    ///   COMn[:baud[,8,N,1]] — з рядка береться лише baud (дефолт 115200); формат кадру
    ///   завжди 8N1 (спека §3.1), поля після baud ігноруються.
    static bool ParseConnString(const std::string& s, EcrConnParams& out);

    /// Еталонна схема: Ping(+dc) → Identify(+dc) → постійний конект. true — на зв'язку.
    bool Connect(const std::string& connString);
    void Disconnect();
    bool IsConnected() const;

    std::string Vendor() const;
    std::string Model() const;

    // Синхронні операції (start+wait). Повертають ResultEnvelope (ok/code/description/payload).
    ResultEnvelope Execute(const std::string& method, const nlohmann::json& params, int timeoutMs);

    /// Увімкнути/вимкнути wire-трасування (діє з наступного Connect).
    void SetTrace(bool on) { traceEnabled_.store(on); }
    ResultEnvelope Purchase(const std::string& amount, const nlohmann::json& extra = {});
    ResultEnvelope Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    ResultEnvelope CheckConnection();
    ResultEnvelope GetReceiptInfo(const std::string& invoiceNumber);

    /// Останній прочитаний getLastStatMsgCode (-1, якщо ще не було).
    int LastStatus() const;

    /// Запит на скасування активної операції: poller надішле interrupt на service-доріжці.
    void RequestInterrupt();

    // Асинхронний API поверх JobEngine (неблокуючий; результат — TryGetOperationResult).
    bool StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs);
    bool StartPurchase(const std::string& amount, const nlohmann::json& extra = {});
    bool StartRefund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    JobState OperationState() const;
    bool TryGetOperationResult(ResultEnvelope& out) const;
    void CancelOperation();   ///< RequestInterrupt() + JobEngine → Interrupting

    // Send-арбітр (спека §7): мін. інтервал 0.1с між ФАКТИЧНИМИ відправленнями.
    // Використовуватиметься в Частині 2; тут — інфраструктура.
    void GateSend();   // блокує до дозволеного моменту, оновлює мітку

private:
    std::unique_ptr<ITransport> MakeTransport(const EcrConnParams& p) const;
    /// Зібрати нову DeviceSession з колбеками (ставляться ДО Start()).
    std::unique_ptr<DeviceSession> MakeSession(const EcrConnParams& p);

    EcrConnParams params_{};
    std::unique_ptr<DeviceSession> session_;   ///< постійна сесія (після Connect)
    std::string vendor_, model_;
    JobEngine job_;                            ///< одне активне асинхронне завдання

    static ResultEnvelope MapResult(const RequestResult& r);
    static constexpr int kOperationTimeoutMs = 120000;   ///< до 120с на фінансову операцію

    /// Poller-цикл: під час активної primary-операції полить getLastStatMsgCode на
    /// service-доріжці й оновлює lastStatus_. Єдиний власник service-доріжки.
    void PollerLoop(std::atomic<bool>& stop);
    std::atomic<int> lastStatus_{ -1 };
    static constexpr int kPollIntervalMs = 500;    ///< 0.5с полінг статусу
    static constexpr int kServiceTimeoutMs = 3000;

    // Скасування: 1С ставить interruptRequested_, poller шле interrupt один раз (interruptSent_).
    std::atomic<bool> interruptRequested_{ false };
    std::atomic<bool> interruptSent_{ false };

    /// Внутрішній виконавець (без ре-ресету interruptRequested_): sync-шлях і worker
    /// асинхронного завдання кличуть саме його. Публічний Execute робить reset ПЕРЕД цим.
    ResultEnvelope ExecuteInternal(const std::string& method, const nlohmann::json& params, int timeoutMs);

    /// Best-effort відновлення після desync (Timeout+IsDesynchronized): полінг статусу до
    /// спокою → MarkSynchronized() → GetReceiptInfo. Викликати ПІСЛЯ зупинки poller-а.
    ResultEnvelope RecoverAfterDesync();
    static constexpr int kRecoverPollTries = 5;

    /// Захист від рекурсивного відновлення (RecoverAfterDesync → ExecuteInternal → …).
    std::atomic<bool> inRecovery_{ false };
    /// Wire-трасування: якщо true — MakeSession чіпляє SetWireTraceHandler (діє з наступного Connect).
    std::atomic<bool> traceEnabled_{ false };

    mutable std::mutex sendGateMutex_;
    std::chrono::steady_clock::time_point lastSend_{};
    static constexpr int kSendGapMs = 100;     ///< 0.1с між командами (спека)
};
