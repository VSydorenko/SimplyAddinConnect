#include "core/pch.h"
#include "protocols/ECRPrivatJSON/ECRPrivatJSON.h"
#include "helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONProtocol::ECRPrivatJSONProtocol()
    : parentComponent_(nullptr)
    , componentName_("ECRPrivatJSONProtocol")
    , connected_(false)
    , waitingForResponse_(false)
    , responseReceived_(false) {
    // Инициализация последнего ответа
    lastResponse_ = TerminalResponse();
    NEUTRAL_REPORT_DEBUG(componentName_, "Создан экземпляр протокола PrivatJSON");
}

ECRPrivatJSONProtocol::~ECRPrivatJSONProtocol() {
    // Отключение от терминала при уничтожении объекта
    NEUTRAL_REPORT_INFO(componentName_, "Завершение работы протокола PrivatJSON");
    Disconnect();
}

void ECRPrivatJSONProtocol::SetParentComponent(AddInNative* component) {
    parentComponent_ = component;
    
    if (parentComponent_) {
        componentName_ = "ECRPrivatJSON";
        NEUTRAL_REPORT_INFO(componentName_, "Установлен родительский компонент " + componentName_);
    } else {
        NEUTRAL_REPORT_WARN(componentName_, "Передан нулевой указатель на родительский компонент");
    }
}

} // namespace ECRPrivatJSON
