#include "core/pch.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONHelper::ECRPrivatJSONHelper(ITransport* transport, const std::string& componentName)
    : transport_(transport)
    , componentName_(componentName)
    , responseReceived_(false) {
}

ECRPrivatJSONHelper::~ECRPrivatJSONHelper() {
    // Очистка ресурсов
    transport_ = nullptr;
}

// ===========================
// Методы парсинга и форматирования JSON
// ===========================

std::string ECRPrivatJSONHelper::BuildRequest(const std::string& method, const std::map<std::string, std::string>& params) {
    try {
        // Создаем базовый JSON-объект
        json requestJson;
        requestJson["method"] = method;
        requestJson["step"] = 0;
        
        // Добавляем параметры, если они есть
        if (!params.empty()) {
            json paramsJson;
            for (const auto& param : params) {
                paramsJson[param.first] = param.second;
            }
            requestJson["params"] = paramsJson;
        }
        
        // Преобразуем в строку
        return requestJson.dump();
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка формирования JSON-запроса: " + std::string(e.what()));
        return "{}";
    }
}

// ===========================
// Методы для работы с транспортным слоем
// ===========================

std::string ECRPrivatJSONHelper::SendReceive(const std::string& request, int timeout) {
    if (!transport_) {
        NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не инициализирован");
        return "";
    }
    
    if (!transport_->IsOpen()) {
        NEUTRAL_REPORT_ERROR(componentName_, "Транспортный слой не открыт");
        return "";
    }
    
    try {
        NEUTRAL_REPORT_DEBUG(componentName_, "Отправка запроса: " + request);
        
        // Добавляем нулевой терминатор в конец
        std::vector<uint8_t> requestData(request.begin(), request.end());
        requestData.push_back(0);
        
        // Сбрасываем состояние получения ответа
        ResetResponseState();
        
        // Отправляем запрос
        if (transport_->Send(requestData) <= 0) {
            NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки запроса");
            return "";
        }
        
        // Ожидаем ответ
        if (!WaitForResponse(timeout)) {
            NEUTRAL_REPORT_ERROR(componentName_, "Таймаут ожидания ответа");
            return "";
        }
        
        // Получаем ответ
        std::string response = GetResponse();
        NEUTRAL_REPORT_DEBUG(componentName_, "Получен ответ: " + response);
        
        return response;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка отправки/получения данных: " + std::string(e.what()));
        return "";
    }
    catch (...) {
        NEUTRAL_REPORT_ERROR(componentName_, "Неизвестная ошибка отправки/получения данных");
        return "";
    }
}

std::string ECRPrivatJSONHelper::AddNullTerminator(const std::string& json, bool addLeadingNull) {
    std::string result;
    
    // Добавляем начальный нулевой терминатор для хендшейка, если требуется
    if (addLeadingNull) {
        result.push_back('\0');
    }
    
    // Копируем исходную строку
    result.append(json);
    
    // Добавляем конечный нулевой терминатор
    result.push_back('\0');
    
    return result;
}

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

bool ECRPrivatJSONHelper::WaitForResponse(int timeout) {
    std::unique_lock<std::mutex> lock(bufferMutex_);
    return dataCondition_.wait_for(lock, std::chrono::milliseconds(timeout),
        [this] { return responseReceived_; });
}

std::string ECRPrivatJSONHelper::GetResponse() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return response_;
}

void ECRPrivatJSONHelper::ResetResponseState() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    responseReceived_ = false;
    response_.clear();
}

// ===========================
// Вспомогательные методы
// ===========================

int ECRPrivatJSONHelper::FindNullTerminator() const {
    auto it = std::find(dataBuffer_.begin(), dataBuffer_.end(), 0);
    if (it != dataBuffer_.end()) {
        return static_cast<int>(std::distance(dataBuffer_.begin(), it));
    }
    return -1;
}

std::string ECRPrivatJSONHelper::FormatAmount(double amount, int precision) const {
    std::ostringstream amountStream;
    amountStream << std::fixed << std::setprecision(precision) << amount;
    return amountStream.str();
}

std::string ECRPrivatJSONHelper::ExtractValueByKey(const std::string& jsonResponse, const std::string& key, const std::string& defaultValue) const {
    try {
        json responseJson = json::parse(jsonResponse);
        
        // Проверяем наличие ключа в корне JSON
        if (responseJson.contains(key) && !responseJson[key].is_null()) {
            if (responseJson[key].is_string()) {
                return responseJson[key].get<std::string>();
            }
            return responseJson[key].dump();
        }
        
        // Проверяем наличие ключа в параметрах
        if (responseJson.contains("params") && responseJson["params"].is_object() && 
            responseJson["params"].contains(key) && !responseJson["params"][key].is_null()) {
            if (responseJson["params"][key].is_string()) {
                return responseJson["params"][key].get<std::string>();
            }
            return responseJson["params"][key].dump();
        }
        
        // Ключ не найден
        return defaultValue;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка извлечения значения по ключу '" + key + "': " + std::string(e.what()));
        return defaultValue;
    }
}

bool ECRPrivatJSONHelper::IsSuccess(const std::string& jsonResponse) const {
    try {
        json j = ParseJSON(jsonResponse);
        
        // Проверка успешности на основе кода ответа
        if (j.contains("params") && j["params"].is_object()) {
            auto params = j["params"];
            
            if (params.contains("responseCode")) {
                std::string responseCode = params["responseCode"].get<std::string>();
                
                // Успешными считаем коды из документации
                bool isSuccess = (responseCode == ResponseCodes::SUCCESS || 
                                 responseCode == ResponseCodes::SUCCESS_SHORT || 
                                 responseCode == ResponseCodes::PARTIAL_APPROVAL);
                
                // Специальный случай для метода "GetReceiptInfo" и для частичного одобрения (10) 
                // согласно документации: "Виключенням є RC=10, який може бути при Partial approval 
                // або при відміні операції Cashback, але успішному проведені оплати"
                if (!isSuccess && responseCode == "10") {
                    // Для GetReceiptInfo код 10 считается успешным
                    if (j.contains("method") && j["method"].get<std::string>() == "GetReceiptInfo") {
                        isSuccess = true;
                    }
                    // Для других операций проверяем поле error
                    else if (j.contains("error") && j["error"].is_boolean()) {
                        isSuccess = !j["error"].get<bool>();
                    }
                }
                
                return isSuccess;
            }
        }
        
        // Если нет кода ответа, то проверяем поле error
        if (j.contains("error") && j["error"].is_boolean()) {
            return !j["error"].get<bool>();
        }
        
        // По умолчанию считаем неуспешным, если не удалось однозначно определить
        return false;
    }
    catch (const json::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга JSON: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка при проверке успешности операции: " + std::string(e.what()));
        return false;
    }
}

bool ECRPrivatJSONHelper::ParseTerminalResponse(const std::string& jsonResponse, TerminalResponse& response) const {
    try {
        // Очищаем структуру перед заполнением
        response.Clear();
        
        // Парсим JSON
        json responseJson = json::parse(jsonResponse);
        
        // Извлекаем метод операции
        if (responseJson.contains("method") && responseJson["method"].is_string()) {
            response.operationName = responseJson["method"].get<std::string>();
        }
        
        // Проверяем наличие поля params
        if (responseJson.contains("params") && responseJson["params"].is_object()) {
            auto params = responseJson["params"];
            
            // Извлекаем код ответа
            if (params.contains("responseCode")) {
                response.responseCode = params["responseCode"].get<std::string>();
            } else if (params.contains("code")) {
                response.responseCode = params["code"].get<std::string>();
            }
            
            // Извлекаем описание ответа
            if (params.contains("responseDescription")) {
                response.responseDescription = params["responseDescription"].get<std::string>();
            } else if (params.contains("description")) {
                response.responseDescription = params["description"].get<std::string>();
            }
            
            // Извлекаем чек
            if (params.contains("receipt") && params["receipt"].is_string()) {
                response.receipt = params["receipt"].get<std::string>();
            }
            
            // Извлекаем RRN
            if (params.contains("rrn") && params["rrn"].is_string()) {
                response.rrn = params["rrn"].get<std::string>();
            }
            
            // Извлекаем код авторизации
            if (params.contains("approvalCode") && params["approvalCode"].is_string()) {
                response.approvalCode = params["approvalCode"].get<std::string>();
            }
            
            // Извлекаем PAN карты
            if (params.contains("cardPan") && params["cardPan"].is_string()) {
                response.cardPAN = params["cardPan"].get<std::string>();
            }
            
            // Извлекаем срок действия карты
            if (params.contains("cardExpDate") && params["cardExpDate"].is_string()) {
                response.cardExpDate = params["cardExpDate"].get<std::string>();
            }
            
            // Извлекаем EMV AID карты
            if (params.contains("cardEmvAid") && params["cardEmvAid"].is_string()) {
                response.cardEmvAid = params["cardEmvAid"].get<std::string>();
            }
            
            // Извлекаем тип карты
            if (params.contains("cardType") && params["cardType"].is_string()) {
                response.cardType = params["cardType"].get<std::string>();
            }
            
            // Извлекаем сумму операции
            if (params.contains("amount")) {
                if (params["amount"].is_string()) {
                    response.amount = params["amount"].get<std::string>();
                } else if (params["amount"].is_number()) {
                    response.amount = std::to_string(params["amount"].get<double>());
                }
            }
            
            // Извлекаем ID терминала
            if (params.contains("terminalId") && params["terminalId"].is_string()) {
                response.terminalID = params["terminalId"].get<std::string>();
            }
            
            // Извлекаем ID мерчанта
            if (params.contains("merchantId") && params["merchantId"].is_string()) {
                response.merchantID = params["merchantId"].get<std::string>();
            }
            
            // Извлекаем ID транзакции
            if (params.contains("transactionId") && params["transactionId"].is_string()) {
                response.transactionID = params["transactionId"].get<std::string>();
            }
            
            // Извлекаем дату транзакции
            if (params.contains("transactionDate") && params["transactionDate"].is_string()) {
                response.transactionDate = params["transactionDate"].get<std::string>();
            }
            
            // Извлекаем время транзакции
            if (params.contains("transactionTime") && params["transactionTime"].is_string()) {
                response.transactionTime = params["transactionTime"].get<std::string>();
            }
        }
          // Определяем успешность операции
        response.isSuccess = IsSuccess(jsonResponse);
        
        // Для совместимости с разными версиями структуры
        response.success = response.isSuccess;
        
        return true;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(componentName_, "Ошибка парсинга ответа терминала: " + std::string(e.what()));
        return false;
    }
}

void ECRPrivatJSONHelper::SetTransport(ITransport* transport) {
    transport_ = transport;
}

} // namespace ECRPrivatJSON
