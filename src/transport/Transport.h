#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

/**
 * @class ITransport
 * @brief Базовый интерфейс транспортного слоя
 * 
 * Определяет общий интерфейс для всех типов транспортного соединения,
 * включая COM-порты, TCP-соединения и WebSocket
 */
class ITransport {
public:
    // Типы для обратных вызовов
    using DataReceivedCallback = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback = std::function<void(const std::string&, int)>;
    using ConnectionStateCallback = std::function<void(bool)>;
    
    /**
     * @brief Виртуальный деструктор
     */
    virtual ~ITransport() = default;
    
    /**
     * @brief Открыть соединение
     * @return true если соединение успешно открыто, false в противном случае
     */
    virtual bool Open() = 0;
    
    /**
     * @brief Закрыть соединение
     * @return true если соединение успешно закрыто, false в противном случае
     */
    virtual bool Close() = 0;
    
    /**
     * @brief Проверить, открыто ли соединение
     * @return true если соединение открыто, false в противном случае
     */
    virtual bool IsOpen() const = 0;
    
    /**
     * @brief Отправить данные через соединение
     * @param data Данные для отправки
     * @return Количество отправленных байт или -1 в случае ошибки
     */
    virtual int Send(const std::vector<uint8_t>& data) = 0;
    
    /**
     * @brief Установить обработчик получения данных
     * @param callback Функция обратного вызова
     */
    virtual void SetDataReceivedCallback(DataReceivedCallback callback) = 0;
    
    /**
     * @brief Установить обработчик ошибок
     * @param callback Функция обратного вызова
     */
    virtual void SetErrorCallback(ErrorCallback callback) = 0;
    
    /**
     * @brief Установить обработчик изменения состояния соединения
     * @param callback Функция обратного вызова
     */
    virtual void SetConnectionStateCallback(ConnectionStateCallback callback) = 0;
};