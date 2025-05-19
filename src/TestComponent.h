#pragma once

#include "core/AddInNative.h"

class TestComponent:
    public AddInNative
{
private:
    static std::vector<std::u16string> names;
    TestComponent();
    int64_t value;
private:
    std::u16string text;
    std::u16string getTestString();
    void setTestString(const std::u16string &text);
};
