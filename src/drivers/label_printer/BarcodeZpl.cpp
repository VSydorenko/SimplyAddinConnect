#include "../../core/pch.h"
#include "BarcodeZpl.h"
#include "LabelUnits.h"
#include <cstdio>
#include <cctype>
namespace labelprinter {

std::string BarcodeZpl::EscapeFd(const std::string& raw) {
    bool need=false; for (unsigned char c: raw) if (c=='^'||c=='~'||c=='>'||c<0x20){need=true;break;}
    if (!need) return "^FD" + raw;
    static const char* H="0123456789ABCDEF";
    std::string out="^FH_^FD";
    for (unsigned char c: raw) {
        if (c=='^'||c=='~'||c=='>'||c<0x20||c=='_') { out.push_back('_'); out.push_back(H[c>>4]); out.push_back(H[c&0xF]); }
        else out.push_back((char)c);
    }
    return out;
}

// повертає рядок цифр без нецифрових символів
static std::string digitsOnly(const std::string& s){ std::string o; for(char c:s) if(std::isdigit((unsigned char)c)) o.push_back(c); return o; }

// Мапінг orientation (0/90/180/270) -> ZPL-орієнтація поля (перший параметр баркод-команди).
static char orientChar(int deg){
    switch (deg) { case 90: return 'R'; case 180: return 'I'; case 270: return 'B'; default: return 'N'; }
}

// Оцінка мінімальної ширини лінійного штрихкоду у модулях (ширина модуля = 1 dot),
// з врахуванням quiet-zone. Для 2D/нелінійних символік повертає 0 (ширина модуля
// через ^BY до них не застосовується — лишаємо дефолт без перевірки влізання).
static long estBarcodeModules(const std::string& type, const std::string& data){
    const long q = 20;                      // quiet-zone разом (~10 модулів з кожного боку)
    const long n = (long)data.size();
    if (type=="EAN13")                       return 95 + q;                 // фіксовано 95 модулів
    if (type=="EAN8")                        return 67 + q;                 // фіксовано 67 модулів
    if (type=="Code128"||type=="EAN128")     return 11*(n + 3) + 13 + q;    // start+data+check +stop
    if (type=="Code39")                      return 16*(n + 2) + q;         // ~16 мод/символ + '*' start/stop
    if (type=="Code93")                      return 9*(n + 4) + q;          // start+data+2check+stop, 9 мод/символ
    if (type=="ITF14")                       return 9*n + 7 + q;            // ~9 модулів на цифру + start/stop
    return 0;                                                              // 2D / нелінійні — без ^BY-розрахунку
}

BarcodeEmit BarcodeZpl::Emit(const BarcodeField& f, const std::string& value, int dotsPerMm) {
    BarcodeEmit e;
    const long x = mmToDots(f.geom.left, dotsPerMm), y = mmToDots(f.geom.top, dotsPerMm);
    const long h = mmToDots(f.geom.height, dotsPerMm);
    const char hri = f.printHRI ? 'Y' : 'N';
    const char ori = orientChar(f.geom.orientation);
    char buf[256];
    const std::string& t = f.type;
    std::string cmd, data = value;
    bool ean128 = false;

    // §6.1: розширення (add-on 2/5) і UPC/EAN extensions відкладені до v2 -> явний UNSUPPORTED.
    if (t.find("Addon") != std::string::npos || t.find("Extension") != std::string::npos) {
        e.ok=false; e.errCode="UNSUPPORTED_BARCODE";
        e.errDesc="Штрихкод-розширення (add-on/extension) відкладено до v2: "+t; return e;
    }

    if (t=="EAN13") {                       // ^BE приймає 12 цифр
        std::string d = digitsOnly(value); if (d.size()==13) d = d.substr(0,12);
        std::snprintf(buf,sizeof(buf),"^BE%c,%ld,%c,N", ori, h, hri); cmd=buf; data=d;
    } else if (t=="EAN8") {
        std::string d = digitsOnly(value); if (d.size()==8) d=d.substr(0,7);
        std::snprintf(buf,sizeof(buf),"^B8%c,%ld,%c,N", ori, h, hri); cmd=buf; data=d;
    } else if (t=="Code128"||t=="EAN128") {
        std::snprintf(buf,sizeof(buf),"^BC%c,%ld,%c,N,N", ori, h, hri); cmd=buf;
        if (t=="EAN128") ean128 = true;          // FNC1 (GS1-128) додаємо ПОЗА escaping (див. нижче)
    } else if (t=="Code39") { std::snprintf(buf,sizeof(buf),"^B3%c,N,%ld,%c,N",ori,h,hri); cmd=buf; }
    else if (t=="Code93")  { std::snprintf(buf,sizeof(buf),"^BA%c,%ld,%c,N",ori,h,hri); cmd=buf; }
    else if (t=="ITF14")   { std::snprintf(buf,sizeof(buf),"^B2%c,%ld,%c,N",ori,h,hri); cmd=buf; }
    else if (t=="QRCode")  { std::snprintf(buf,sizeof(buf),"^BQ%c,2,5",ori); cmd=buf; }
    else if (t=="DataMatrix"){ std::snprintf(buf,sizeof(buf),"^BX%c,5,200",ori); cmd=buf; }
    else if (t=="PDF417")  { std::snprintf(buf,sizeof(buf),"^B7%c,5,5",ori); cmd=buf; }
    else if (t=="GS1DataBarExpandedStacked"){ std::snprintf(buf,sizeof(buf),"^BR%c,6,5,2,,%ld",ori,h); cmd=buf; }
    else { e.ok=false; e.errCode="UNSUPPORTED_BARCODE"; e.errDesc="Тип штрихкоду не підтримується нативно: "+t; return e; }

    // Ширина модуля ^BY з геометрії поля + перевірка влізання (з quiet-zone) для лінійних символік.
    long moduleW = 2;                             // дефолт, коли ширину поля не задано
    if (f.geom.width > 0) {
        long estM = estBarcodeModules(t, data);
        if (estM > 0) {
            const long avail = mmToDots(f.geom.width, dotsPerMm);
            moduleW = avail / estM;               // ціла ширина модуля в dots
            if (moduleW < 1) {
                e.ok=false; e.errCode="BARCODE_TOO_WIDE";
                e.errDesc="Штрихкод не влазить у задану ширину поля (з quiet-zone): "+t; return e;
            }
            if (moduleW > 10) moduleW = 10;       // клампимо 1..10 dots
        }
    }

    // ^FD: для EAN128 FNC1-послідовність (>;>8) має лишитись КЕРУЮЧОЮ (не hex-екранованою під ^FH),
    // тож екрануємо лише корисне навантаження, а FNC1 вставляємо ПОЗА escaping — одразу після ^FD.
    std::string fd = EscapeFd(data);
    if (ean128) {
        const std::string pfx = ">;>8";           // start subset C + FNC1 (GS1-128)
        size_t p = fd.find("^FD");
        if (p != std::string::npos) fd.insert(p + 3, pfx);
    }

    std::snprintf(buf,sizeof(buf),"^FO%ld,%ld^BY%ld", x, y, moduleW);
    e.zpl = std::string(buf) + cmd + fd + "^FS";
    e.ok = true; return e;
}
} // namespace labelprinter
