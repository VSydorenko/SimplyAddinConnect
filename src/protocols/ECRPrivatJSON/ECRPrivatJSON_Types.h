#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>

namespace ECRPrivatJSON {

/**
 * @brief Константы кодов ответа для протокола ECR Privat JSON
 */
namespace ResponseCodes {
    // Успешные коды
    const std::string SUCCESS = "0000";                         // Успешная операция
    const std::string SUCCESS_SHORT = "00";                     // Успешная операция (краткий код)
    const std::string PARTIAL_APPROVAL = "0010";                // Частичное одобрение
    
    // Коды ошибок
    const std::string GENERAL_ERROR = "9999";                   // Общая ошибка
    const std::string TIMEOUT = "0908";                         // Таймаут операции
    const std::string CANCELLED = "0999";                       // Операция отменена пользователем
    
    // Дополнительные коды ошибок из документации
    const std::string GENERAL_ERROR_1000 = "1000";              // Загальна помилка
    const std::string CANCELLED_BY_USER = "1001";               // Транзакція відмінена користувачем
    const std::string EMV_DECLINE = "1002";                     // EMV Decline
    const std::string TRANSACTION_LOG_FULL = "1003";            // Transaction log is full
    const std::string NO_CONNECTION = "1004";                   // No connection with host
    const std::string NO_PAPER = "1005";                        // No paper in printer
    const std::string CRYPTO_ERROR = "1006";                    // Error Crypto keys
    const std::string CARD_READER_NOT_CONNECTED = "1007";       // Card reader is not connected
    const std::string TRANSACTION_ALREADY_COMPLETE = "1008";    // Transaction is already complete
}

/**
 * @brief Типы операций протокола ECR Privat JSON
 */
enum class OperationType {
    Purchase,                   // Оплата
    Refund,                     // Возврат
    Withdrawal,                 // Отмена
    WithdrawalPartly,           // Частичная отмена
    Verify,                     // Сверка итогов (дневной отчет)
    CheckConnection,            // Проверка соединения
    PrintReceiptNum,            // Печать чека
    ServiceMessage,             // Служебное сообщение
    ReadCardBank,               // Чтение банковской карты
    ReadCardDiscount,           // Чтение дисконтной карты
    GetTerminalInfo,            // Получение информации о терминале
    PingDevice,                 // Проверка доступности терминала
    GetBalance,                 // Получение баланса
    ServiceRefund,              // Сервис возврата
    ServicePbP,                 // Сервис оплата частями
    ServiceRefPbP,              // Сервис возврат оплаты частями
    ServicePartlyRefPbP,        // Сервис частичный возврат оплаты частями
    ServicePbPperiod,           // Сервис оплата частями в периоде
    ServiceRefPbPperiod,        // Сервис возврат оплаты частями в периоде
    ServicePartlyRefPbPperiod,  // Сервис частичный возврат оплаты частями в периоде
    ServiceInstantPbI,          // Сервис мгновенная рассрочка
    ServiceRefPbI,              // Сервис возврат мгновенной рассрочки
    ServicePartlyRefPbI,        // Сервис частичный возврат мгновенной рассрочки
    ServicePbIAct,              // Сервис мгновенная рассрочка акция
    ServiceRefPbIAct,           // Сервис возврат мгновенной рассрочки акция
    ServicePartlyRefPbIAct,     // Сервис частичный возврат мгновенной рассрочки акция
    ServicePbPAct,              // Акционная оплата частями
    ServiceRefPbPAct,           // Возврат акционной оплаты частями
    ServicePartlyRefPbPAct,     // Сервис частичный возврат акционной оплаты частями
    ServiceGeneric,             // Универсальный сервис
    Cashback,                   // Кэшбэк
    Audit,                      // Аудит (X-баланс)
    VerifyCopy,                 // Копия чека сверки итогов
    PrintBatchJournal,          // Печать информации о всех транзакциях в пакете
    ReadBonusCard,              // Чтение бонусной карты
    GetPinBonusCard,            // Получение PIN бонусной карты
    PinChangeBonusCard,         // Изменение PIN бонусной карты
    GetPhoneNumber,             // Ввод номера телефона
    Preauthorization,           // Предавторизация
    SaleCompletion,             // Завершение предавторизации
    GetOTPpassword,             // Ввод OTP пароля
    GetReceiptInfo              // Получение данных по чеку
};

/**
 * @brief Типы служебных сообщений протокола ECR Privat JSON
 */
enum class ServiceMessageType {
    Identify,                   // Идентификация терминала
    DeviceBusy,                 // Устройство занято
    Interrupt,                  // Прерывание операции
    InterruptTransmitted,       // Подтверждение прерывания
    MethodNotImplemented,       // Метод не реализован
    GetMerchantList,            // Получение списка мерчантов
    GetMaskList,                // Получение списка масок мерчантов
    Debug,                      // Отладка
    DebugOn,                    // Отладка включена
    DebugOff,                   // Отладка отключена
    GetLastResult,              // Получение результата последней операции
    GetLastStatMsgCode,         // Получение кода статусного сообщения
    GetLastStatMsgDescription,  // Получение описания статусного сообщения
    GetDiscountName,            // Получение имени группы скидки
    GetVersion                  // Получение версии протокола
};

/**
 * @brief Структура ответа терминала
 */
struct TerminalResponse {
    std::string responseCode;           // Код ответа
    std::string responseDescription;    // Описание ответа
    std::string receipt;                // Текст чека (если есть)
    std::string rrn;                    // RRN транзакции
    std::string approvalCode;           // Код авторизации
    std::string cardPAN;                // Замаскированный PAN карты
    std::string cardExpDate;            // Срок действия карты
    std::string cardEmvAid;             // EMV AID карты
    std::string cardType;               // Тип карты
    std::string amount;                 // Сумма операции
    std::string terminalID;             // Идентификатор терминала
    std::string merchantID;             // Идентификатор мерчанта
    std::string transactionID;          // Идентификатор транзакции
    std::string jsonResponse;           // Исходный JSON ответ
    std::string method;                 // Метод операции
    std::string errorMessage;           // Сообщение об ошибке
    std::string receiptText;            // Текст чека в форматированном виде
    std::string operationStatus;        // Статус операции
    std::string totalAmount;            // Общая сумма
    std::string currency;               // Валюта операции
    std::string cardHolder;             // Держатель карты
    std::string aid;                    // Application Identifier
    std::string paymentStatus;          // Статус платежа
    std::string bankName;               // Название банка
    std::string refundNDSPerc;          // Процент НДС для возврата
    std::string refundNDSAmount;        // Сумма НДС для возврата
    bool success;                       // Успех операции
    std::string transactionDate;        // Дата транзакции
    std::string transactionTime;        // Время транзакции
    std::string operationName;          // Название операции
    bool isSuccess;                     // Признак успешности операции
    
    // Конструктор по умолчанию
    TerminalResponse() : isSuccess(false) {}

    // Очистка структуры
    void Clear() {
        responseCode.clear();
        responseDescription.clear();
        receipt.clear();
        rrn.clear();
        approvalCode.clear();
        cardPAN.clear();
        cardExpDate.clear();
        cardEmvAid.clear();
        cardType.clear();
        amount.clear();
        terminalID.clear();
        merchantID.clear();
        transactionID.clear();
        transactionDate.clear();
        transactionTime.clear();
        operationName.clear();
        isSuccess = false;
        jsonResponse.clear();
        method.clear();
        errorMessage.clear();
        receiptText.clear();
        operationStatus.clear();
        totalAmount.clear();
        currency.clear();
        cardHolder.clear();
        aid.clear();
        paymentStatus.clear();
        bankName.clear();
        refundNDSPerc.clear();
        refundNDSAmount.clear();
        success = false;
    }
};

} // namespace ECRPrivatJSON
