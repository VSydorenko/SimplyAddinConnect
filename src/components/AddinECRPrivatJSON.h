#pragma once
#include "../core/AddInNative.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonDriver.h"
#include <vector>
#include <string>

/// Компонента 1С над пілотним драйвером ECRPrivatJSON. Реєструє методи прямо
/// (як AddinUAPKIConnect); делегує EcrPrivatJsonDriver; результати — через
/// this->result / РезультатОперацииJSON.
class AddinECRPrivatJSON : public AddInNative {
public:
    static std::vector<std::u16string> names;
    AddinECRPrivatJSON();
    virtual ~AddinECRPrivatJSON();

private:
    void RegisterMethods();
    EcrPrivatJsonDriver driver_;
    std::string lastResultJson_;   ///< останній ResultEnvelope у JSON (для РезультатОперацииJSON)
};
