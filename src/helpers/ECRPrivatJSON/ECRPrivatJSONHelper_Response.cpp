#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// Обработка полученных данных
void ECRPrivatJSONHelper::ProcessReceivedData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    
    // Добавляем полученные данные в буфер
    dataBuffer_.insert(dataBuffer_.end(), data.begin(), data.end());
    
    // Проверяем наличие нулевого терминатора в буфере
    int nullPos = FindNullTerminator();
    if (nullPos >= 0) {
        // Найден нулевой терминатор, извлекаем сообщение
        response_ = std::string(dataBuffer_.begin(), dataBuffer_.begin() + nullPos);
        responseReceived_ = true;
        
        // Удаляем обработанные данные из буфера
        dataBuffer_.erase(dataBuffer_.begin(), dataBuffer_.begin() + nullPos + 1);
        
        // Уведомляем о получении ответа
        dataCondition_.notify_one();
    }
}

// Ожидание получения ответа
bool ECRPrivatJSONHelper::WaitForResponse(int timeout) {
    std::unique_lock<std::mutex> lock(bufferMutex_);
    return dataCondition_.wait_for(lock, std::chrono::milliseconds(timeout),
        [this] { return responseReceived_; });
}

// Получение накопленного ответа
std::string ECRPrivatJSONHelper::GetResponse() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return response_;
}

// Сброс состояния ожидания ответа
void ECRPrivatJSONHelper::ResetResponseState() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    responseReceived_ = false;
    response_.clear();
}

// Поиск нулевого терминатора в буфере
int ECRPrivatJSONHelper::FindNullTerminator() const {
    auto it = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
    if (it != dataBuffer_.end()) {
        return static_cast<int>(std::distance(dataBuffer_.begin(), it));
    }
    return -1;
}

} // namespace ECRPrivatJSON
