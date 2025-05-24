#include "../../core/pch.h"
#include "ECRPrivatJSON.h"
#include "../../helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "../../helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONProtocol::ECRPrivatJSONProtocol()
    : parentComponent_(nullptr)
    , connected_(false)
    , waitingForResponse_(false)
    , responseReceived_(false) {
    // Инициализация последнего ответа
    lastResponse_ = TerminalResponse();
    NEUTRAL_REPORT_DEBUG("ECRPrivatJSONProtocol", "Создан экземпляр протокола PrivatJSON");
}

ECRPrivatJSONProtocol::~ECRPrivatJSONProtocol() {
    // Отключение от терминала при уничтожении объекта
    NEUTRAL_REPORT_INFO("ECRPrivatJSONProtocol", "Завершение работы протокола PrivatJSON");
    Disconnect();
}

void ECRPrivatJSONProtocol::SetParentComponent(AddInNative* component) {
    parentComponent_ = component;
    
    if (parentComponent_) {
        "ECRPrivatJSONProtocol" = "ECRPrivatJSON";
        NEUTRAL_REPORT_INFO("ECRPrivatJSONProtocol", "Установлен родительский компонент " + "ECRPrivatJSONProtocol");
    } else {
        NEUTRAL_REPORT_WARN("ECRPrivatJSONProtocol", "Передан нулевой указатель на родительский компонент");
    }
}

} // namespace ECRPrivatJSON
