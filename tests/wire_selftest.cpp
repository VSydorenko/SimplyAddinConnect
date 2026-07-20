// wire_selftest — харнес device-facing ядра (framer/classifier/session) без 1С і без UAPKI.
// Лінкує wire_component+helpers+base напряму; свій хенд-ролед раннер (як core_selftest).
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/transport/NullTerminatedFramer.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(c,n) do{ if(c){std::printf("[PASS] %s\n",n);} else {std::printf("[FAIL] %s\n",n);++g_failed;} }while(0)

static std::vector<uint8_t> B(const std::string& s){ return {s.begin(), s.end()}; }

static void TestNullTerminatedFramer() {
    NullTerminatedFramer f;
    std::vector<std::vector<uint8_t>> out;

    // 1 кадр
    f.Feed([]{ auto v=B("{\"m\":1}"); v.push_back(0); return v; }(), out);
    CHECK(out.size()==1 && out[0]==B("{\"m\":1}"), "single frame");

    // split посередині кадру між Feed
    out.clear(); f.Reset();
    f.Feed(B("{\"a\""), out);            CHECK(out.empty(), "partial: no frame yet");
    f.Feed([]{ auto v=B(":2}"); v.push_back(0); return v; }(), out);
    CHECK(out.size()==1 && out[0]==B("{\"a\":2}"), "split frame reassembled");

    // два кадри в одному чанку + провідний/подвійний 0x00 ігнорується
    out.clear(); f.Reset();
    std::vector<uint8_t> c; c.push_back(0);            // провідний 0x00 → порожній кадр, ігнор
    for(char ch:std::string("A")) c.push_back(ch); c.push_back(0);
    for(char ch:std::string("B")) c.push_back(ch); c.push_back(0);
    f.Feed(c, out);
    CHECK(out.size()==2 && out[0]==B("A") && out[1]==B("B"), "two frames, leading 0x00 ignored");

    // Wrap
    CHECK(f.Wrap(B("X")) == ([]{ auto v=B("X"); v.push_back(0); return v; }()), "Wrap adds trailing 0x00");
    auto hs = f.Wrap(B("X"), FrameOptions{true});
    CHECK(hs.size()==3 && hs[0]==0 && hs[2]==0, "Wrap leadingDelimiter for handshake");

    // переповнення: буфер без 0x00 понад ліміт → framing-error, буфер очищено, наступний кадр ОК
    { NullTerminatedFramer fo(4); std::vector<std::vector<uint8_t>> o2;
      fo.Feed(B("12345"), o2);                 // >4 без термінатора
      CHECK(o2.empty(), "overflow: no frame emitted");
      fo.Feed([]{ auto v=B("Z"); v.push_back(0); return v; }(), o2);
      CHECK(o2.size()==1 && o2[0]==B("Z"), "overflow: recovers on next valid frame"); }
}
int main(){ std::printf("=== wire_selftest ===\n"); TestNullTerminatedFramer();
    std::printf("=== %s (failed:%d) ===\n", g_failed?"FAIL":"OK", g_failed); return g_failed?1:0; }
