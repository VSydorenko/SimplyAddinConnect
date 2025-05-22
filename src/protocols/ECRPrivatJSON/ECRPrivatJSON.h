#pragma once

#include "ECRPrivatJSON_Types.h"
#include "../../core/AddInNative.h"
#include "../../helpers/ServiceTools.h"
#include "../../transport/Transport.h"
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>

// Предварительное объявление для избежания циклических зависимостей
namespace SimplyConnect {
    class ECRPrivatJSONHelper;
}

namespace SimplyConnect {

/**
 * @class ECRPrivatJSONProtocol
 * @brief Реализация протокола ECR Privat JSON
 * 
 * Этот класс обеспечивает полную реализацию протокола ECR Privat JSON
 * и отвечает за управление соединением, выполнение финансовых и служебных операций,
 * и обработку ответов терминала.
 */
class ECRPrivatJSONProtocol {
public:
    /**
     * @brief Конструктор класса
     */
    ECRPrivatJSONProtocol();
    
    /**
     * @brief Деструктор класса
     */
    ~ECRPrivatJSONProtocol();
    
    /**
     * @brief Установка родительского компонента для организации логирования
     * @param component Указатель на родительский компонент
     */
    void SetParentComponent(AddInNative* component);
    
    //
    // Методы подключения
    //
    
    /**
     * @brief Подключение к терминалу через COM-порт
     * @param portName Имя COM-порта
     * @param baudRate Скорость передачи данных
     * @return true, если подключение успешно
     */
    bool ConnectCOM(const std::u16string& portName, int baudRate);
    
    /**
     * @brief Подключение к терминалу через TCP
     * @param address IP-адрес терминала
     * @param port Порт терминала
     * @return true, если подключение успешно
     */
    bool ConnectTCP(const std::string& address, int port);

    /**
     * @brief Подключение к терминалу через WebSocket
     * @param url URL WebSocket (ws://host:port)
     * @return true, если подключение успешно
     */
    bool ConnectWebSocket(const std::string& url);
    
    /**
     * @brief Отключение от терминала
     */
    void Disconnect();
    
    /**
     * @brief Проверка активности соединения
     * @return true, если соединение активно
     */
    bool IsConnected() const;
    
    //
    // Финансовые операции
    //
    
    /**
     * @brief Выполнение операции оплаты
     * @param amount Сумма оплаты
     * @param discount Скидка
     * @param merchantId Идентификатор мерчанта
     * @param subMerchant Идентификатор суб-мерчанта
     * @return true, если операция успешна
     */
    bool Payment(double amount, const std::string& discount = "", 
                const std::string& merchantId = "0", const std::string& subMerchant = "");
    
    /**
     * @brief Выполнение операции возврата
     * @param amount Сумма возврата
     * @param rrn Номер ссылки ретрансляции
     * @param discount Скидка
     * @param merchantId Идентификатор мерчанта
     * @param subMerchant Идентификатор суб-мерчанта
     * @return true, если операция успешна
     */
    bool Refund(double amount, const std::string& rrn = "", const std::string& discount = "", 
                const std::string& merchantId = "0", const std::string& subMerchant = "");
    
    /**
     * @brief Выполнение сверки итогов
     * @param merchantId Идентификатор мерчанта
     * @return true, если операция успешна
     */
    bool Settlement(const std::string& merchantId = "0");
    
    /**
     * @brief Получение копии чека
     * @param transactionId Идентификатор транзакции
     * @return true, если операция успешна
     */
    bool GetReceipt(const std::string& transactionId);
    
    /**
     * @brief Получение информации о терминале
     * @return Строка с информацией о терминале
     */
    std::string GetTerminalInfo();
    
    /**
     * @brief Получение последнего ответа от терминала
     * @return Структура с данными последнего ответа
     */
    TerminalResponse GetLastResponse() const;

private:
    /**
     * @brief Создание транспортного объекта на основе строки подключения
     * @param connectionString Строка подключения (COM-порт, TCP, WebSocket)
     * @return Указатель на созданный транспортный объект или nullptr в случае ошибки
     */
    std::unique_ptr<ITransport> CreateTransport(const std::string& connectionString);
    
    /**
     * @brief Выполнение хендшейка с терминалом
     * @return true, если хендшейк успешен
     */
    bool PerformHandshake();
    
    /**
     * @brief Идентификация терминала
     * @param terminalInfo Строка для сохранения информации о терминале
     * @return true, если идентификация успешна
     */
    bool IdentifyTerminal(std::string& terminalInfo);
    
    /**
     * @brief Обработчик получения данных от транспортного слоя
     * @param data Полученные данные
     */
    void OnDataReceived(const std::vector<uint8_t>& data);
    
    /**
     * @brief Обработчик ошибок транспортного слоя
     * @param errorMessage Сообщение об ошибке
     * @param errorCode Код ошибки
     */
    void OnError(const std::string& errorMessage, int errorCode);
    
    /**
     * @brief Обработчик изменения состояния соединения
     * @param connected Состояние соединения (true - подключено, false - отключено)
     */
    void OnConnectionStateChanged(bool connected);

    // Указатель на транспортный слой
    std::unique_ptr<ITransport> transport_;
    
    // Указатель на родительский компонент для логирования
    AddInNative* parentComponent_;
    
    // Указатель на вспомогательный класс для работы с протоколом
    std::unique_ptr<ECRPrivatJSONHelper> helper_;
    
    // Последний ответ терминала
    TerminalResponse lastResponse_;
    
    // Имя компонента для нейтральных сообщений логирования
    std::string componentName_;
    
    // Буфер для накопления данных
    std::vector<uint8_t> dataBuffer_;
    
    // Мьютекс для синхронизации доступа к буферу данных
    std::mutex bufferMutex_;
    
    // Условная переменная для уведомления о получении данных
    std::condition_variable dataCondition_;
    
    // Флаг состояния соединения
    std::atomic<bool> connected_;
    
    // Флаг, указывающий, что ожидается ответ от терминала
    std::atomic<bool> waitingForResponse_;
    
    // Флаг, указывающий, что ответ получен
    std::atomic<bool> responseReceived_;
    
    // Полученный ответ от терминала
    std::string receivedResponse_;
};

} // namespace SimplyConnect
