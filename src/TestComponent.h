#pragma once

#include "core/AddInNative.h"
#include "helpers/ServiceTools.h"
#include "transport/Transport_COM.h"

class TestComponent:
    public AddInNative
{
public:
    static std::vector<std::u16string> names;
private:
    TestComponent();
    ~TestComponent();
    int64_t value;
private:
    std::u16string text;
    std::u16string getTestString();
    void setTestString(const std::u16string &text);
    void GenerateTestError();
    
    // Методы для работы с COM-портами
    std::vector<std::u16string> GetAvailablePorts();
    bool CheckPortExists(const std::u16string &portName);
    bool IsPortAvailable(const std::u16string &portName);
    bool OpenPort(const std::u16string &portName, const std::u16string &baudRate);
    bool ClosePort(const std::u16string &portName);
    
    // Транспортный контур COM
    std::unique_ptr<TransportCOM> comTransport;
    bool isPortOpen = false;
};
