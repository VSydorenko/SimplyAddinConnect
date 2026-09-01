#pragma once
#include "../IAcquiringDriver.h"
#include "EcrPrivatJsonDriver.h"

/// Адаптер: протокол ПриватБанк JSON у контракті IAcquiringDriver.
/// Уся Privat-специфіка (рядок підключення, форма налаштувань, можливості
/// термінала) — тут; фасад про неї не знає.
class EcrPrivatJsonAcquiring : public IAcquiringDriver {
public:
    ResultEnvelope Open(const std::map<std::string, std::string>& params) override;
    void Close() override;
    bool IsConnected() const override;

    std::string Vendor() const override;
    std::string Model() const override;
    std::string DriverName() const override;
    std::string DriverDescription() const override;
    std::string SettingsXml() const override;
    AcquiringCapabilities Capabilities() const override;
    ResultEnvelope Probe() override;

    ResultEnvelope Purchase(double amount) override;
    ResultEnvelope Refund(double amount, const std::string& rrn) override;
    ResultEnvelope DayTotals() override;
    ResultEnvelope Audit() override;
    // Void/EmergencyVoid НЕ перевизначені: власних операцій протокол не має,
    // працює дефолтна чесна відмова (скасування фасад робить поверненням за RRN).

    bool StartPurchase(double amount) override;
    bool StartRefund(double amount, const std::string& rrn) override;
    int  OperationState() const override;
    bool TryGetOperationResult(ResultEnvelope& out) const override;
    int  LastStatus() const override;
    void CancelOperation() override;

private:
    static std::string BuildConnectionString(const std::map<std::string, std::string>& params);
    EcrPrivatJsonDriver drv_;
};
