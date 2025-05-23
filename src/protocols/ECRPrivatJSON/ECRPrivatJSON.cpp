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
}

ECRPrivatJSONProtocol::~ECRPrivatJSONProtocol() {
    // Отключение от терминала при уничтожении объекта
    Disconnect();
}

void ECRPrivatJSONProtocol::SetParentComponent(AddInNative* component) {
    parentComponent_ = component;
    
    if (parentComponent_) {
        componentName_ = "ECRPrivatJSON";
    }
}

} // namespace ECRPrivatJSON
