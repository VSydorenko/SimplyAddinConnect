// core_selftest — L1-харнес ядра AddInNative без 1С і без UAPKI.
// Лінкує OBJECT-бібліотеки ядра напряму; емулює платформу моками.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/core/AddInNative.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, name) do { \
    if (cond) { std::printf("[PASS] %s\n", name); } \
    else { std::printf("[FAIL] %s\n", name); ++g_failed; } \
} while (0)

// ---- Мок платформи 1С ----
struct MockConnect : public IAddInDefBase {
    std::vector<std::u16string> errors;      // тексти AddError
    std::vector<std::u16string> events;      // "source|message|data" з ExternalEvent
    bool ADDIN_API AddError(unsigned short, const WCHAR_T*,
                            const WCHAR_T* descr, long) override {
        errors.push_back(reinterpret_cast<const char16_t*>(descr));
        return true;
    }
    bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
    bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return false; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 100; }
    bool ADDIN_API ExternalEvent(WCHAR_T* src, WCHAR_T* msg, WCHAR_T* data) override {
        std::u16string s = reinterpret_cast<char16_t*>(src);
        s += u"|"; s += reinterpret_cast<char16_t*>(msg);
        s += u"|"; s += reinterpret_cast<char16_t*>(data);
        events.push_back(s);
        return true;
    }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
};

struct MockMemory : public IMemoryManager {
    bool ADDIN_API AllocMemory(void** p, unsigned long n) override {
        *p = std::malloc(n); return *p != nullptr;
    }
    void ADDIN_API FreeMemory(void** p) override {
        std::free(*p); *p = nullptr;
    }
};

// ---- Смоук: реєстрація компоненти, життєвий цикл, властивість Version ----
static void TestSmokeLifecycle() {
    // Пробна компонента реєструється прямо в тесті через публічний AddComponent
    struct CoreProbe : public AddInNative {};
    AddInNative::AddComponent(u"CoreProbe", []() -> AddInNative* { return new CoreProbe; });

    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    CHECK(comp != nullptr, "CreateObject(CoreProbe)");
    if (!comp) return;

    MockConnect connect; MockMemory memory;
    CHECK(comp->Init(&connect), "Init");
    CHECK(comp->setMemManager(&memory), "setMemManager");

    long propNum = comp->FindProp((WCHAR_T*)u"Version");
    CHECK(propNum >= 0, "FindProp(Version)");

    tVariant val{}; tVarInit(&val);
    CHECK(comp->GetPropVal(propNum, &val), "GetPropVal(Version)");
    CHECK(val.vt == VTYPE_PWSTR && val.wstrLen > 0, "Version is non-empty string");

    comp->Done();
    delete comp;
}

int main() {
    std::printf("=== core_selftest ===\n");
    TestSmokeLifecycle();
    std::printf("=== %s (failed: %d) ===\n", g_failed ? "FAIL" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
