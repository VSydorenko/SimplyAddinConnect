#include "../../core/pch.h"
#include "ECRPrivatJSON.h"
#include "../../helpers/ECRPrivatJSON/ECRPrivatJSONHelper.h"
#include "../../helpers/ServiceTools.h"

namespace ECRPrivatJSON {

// ===========================
// Конструктор и деструктор
// ===========================

ECRPrivatJSONProtocol::ECRPrivatJSONProtocol()
    : connected_(false)
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

} // namespace ECRPrivatJSON
