#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// Обработка полученных данных
void ECRPrivatJSONHelper::ProcessReceivedData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    
    // Добавляем полученные данные в буфер
    dataBuffer_.insert(dataBuffer_.end(), data.begin(), data.end());
    
    // Проверяем, есть ли нулевой терминатор
    int terminatorPos = FindNullTerminator();
    
    if (terminatorPos >= 0) {
        // Создаем строку с JSON-ответом
        std::string jsonResponse(dataBuffer_.begin(), dataBuffer_.begin() + terminatorPos);
        
        // Нормализуем JSON-строку
        std::string normalizedJson = NormalizeResponseJson(jsonResponse);
        
        // Сохраняем ответ
        receivedResponse_ = normalizedJson;
        responseReceived_ = true;
        
        // Удаляем обработанные данные из буфера (включая нулевой терминатор)
        dataBuffer_.erase(dataBuffer_.begin(), dataBuffer_.begin() + terminatorPos + 1);
        
        // Уведомляем ожидающий поток
        dataCondition_.notify_all();
    }
}

// Ожидание получения ответа
bool ECRPrivatJSONHelper::WaitForResponse(int timeout) {
    std::unique_lock<std::mutex> lock(bufferMutex_);
    
    // Ждем, пока не получим ответ или не истечет таймаут
    return dataCondition_.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
        return responseReceived_;
    });
}

// Получение накопленного ответа
std::string ECRPrivatJSONHelper::GetResponse() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return receivedResponse_;
}

// Сброс состояния ожидания ответа
void ECRPrivatJSONHelper::ResetResponseState() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    responseReceived_ = false;
    receivedResponse_.clear();
}

// Поиск нулевого терминатора в буфере
int ECRPrivatJSONHelper::FindNullTerminator() const {
    for (size_t i = 0; i < dataBuffer_.size(); i++) {
        if (dataBuffer_[i] == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Установка транспортного объекта
void ECRPrivatJSONHelper::SetTransport(ITransport* transport) {
    transport_ = transport;
}

} // namespace ECRPrivatJSON
