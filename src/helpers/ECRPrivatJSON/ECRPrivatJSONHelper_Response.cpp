#include "../../core/pch.h"
#include "ECRPrivatJSONHelper.h"
#include "../ServiceTools.h"

namespace ECRPrivatJSON {

// Обработка полученных данных
void ECRPrivatJSONHelper::ProcessReceivedData(const std::vector<uint8_t>& data) {
    try {
        if (data.empty()) {
            NEUTRAL_REPORT_WARN(componentName_, "Получены пустые данные для обработки");
            return;
        }
        
        NEUTRAL_REPORT_DEBUG(componentName_, "Получены данные размером " + std::to_string(data.size()) + " байт");
        
        std::lock_guard<std::mutex> lock(bufferMutex_);
        
        // Добавляем полученные данные в буфер
        dataBuffer_.insert(dataBuffer_.end(), data.begin(), data.end());
        NEUTRAL_REPORT_TRACE(componentName_, "Данные добавлены в буфер, текущий размер буфера: " + std::to_string(dataBuffer_.size()) + " байт");
        
        // Проверяем наличие нулевого терминатора в буфере
        int nullPos = FindNullTerminator();
        if (nullPos >= 0) {
            // Найден нулевой терминатор, извлекаем сообщение
            response_ = std::string(dataBuffer_.begin(), dataBuffer_.begin() + nullPos);
            responseReceived_ = true;
            
            NEUTRAL_REPORT_DEBUG(componentName_, "Найден нулевой терминатор на позиции " + std::to_string(nullPos) + ", сообщение извлечено");
            
            // Удаляем обработанные данные из буфера
            dataBuffer_.erase(dataBuffer_.begin(), dataBuffer_.begin() + nullPos + 1);
            NEUTRAL_REPORT_TRACE(componentName_, "Обработанные данные удалены из буфера, остаток: " + std::to_string(dataBuffer_.size()) + " байт");
            
            // Уведомляем о получении ответа
            dataCondition_.notify_one();
            NEUTRAL_REPORT_INFO(componentName_, "Получен полный ответ, отправлено уведомление");
        } else {
            NEUTRAL_REPORT_TRACE(componentName_, "Нулевой терминатор не найден, ожидание дополнительных данных");
        }
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при обработке полученных данных: " + std::string(e.what()));
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при обработке полученных данных");
    }
}

// Ожидание получения ответа
bool ECRPrivatJSONHelper::WaitForResponse(int timeout) {
    try {
        NEUTRAL_REPORT_DEBUG(componentName_, "Начало ожидания ответа с таймаутом " + std::to_string(timeout) + " мс");
        
        std::unique_lock<std::mutex> lock(bufferMutex_);
        bool result = dataCondition_.wait_for(lock, std::chrono::milliseconds(timeout),
            [this] { return responseReceived_; });
        
        if (result) {
            NEUTRAL_REPORT_DEBUG(componentName_, "Ответ успешно получен в пределах таймаута");
        } else {
            NEUTRAL_REPORT_WARN(componentName_, "Истек таймаут ожидания ответа (" + std::to_string(timeout) + " мс)");
        }
        
        return result;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при ожидании ответа: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при ожидании ответа");
        return false;
    }
}

// Получение накопленного ответа
std::string ECRPrivatJSONHelper::GetResponse() {
    try {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        
        if (response_.empty()) {
            NEUTRAL_REPORT_WARN(componentName_, "Получение пустого ответа");
        } else {
            NEUTRAL_REPORT_DEBUG(componentName_, "Получение ответа размером " + std::to_string(response_.size()) + " байт");
        }
        
        return response_;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при получении ответа: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при получении ответа");
        return "";
    }
}

// Сброс состояния ожидания ответа
void ECRPrivatJSONHelper::ResetResponseState() {
    try {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        responseReceived_ = false;
        response_.clear();
        NEUTRAL_REPORT_TRACE(componentName_, "Состояние ожидания ответа сброшено");
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при сбросе состояния ответа: " + std::string(e.what()));
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при сбросе состояния ответа");
    }
}

// Поиск нулевого терминатора в буфере
int ECRPrivatJSONHelper::FindNullTerminator() const {
    try {
        if (dataBuffer_.empty()) {
            NEUTRAL_REPORT_TRACE(componentName_, "Попытка поиска нулевого терминатора в пустом буфере");
            return -1;
        }
        
        auto it = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
        if (it != dataBuffer_.end()) {
            int position = static_cast<int>(std::distance(dataBuffer_.begin(), it));
            NEUTRAL_REPORT_TRACE(componentName_, "Нулевой терминатор найден на позиции " + std::to_string(position));
            return position;
        }
        
        NEUTRAL_REPORT_TRACE(componentName_, "Нулевой терминатор не найден в буфере размером " + std::to_string(dataBuffer_.size()) + " байт");
        return -1;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при поиске нулевого терминатора: " + std::string(e.what()));
        return -1;
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка при поиске нулевого терминатора");
        return -1;
    }
}

} // namespace ECRPrivatJSON
