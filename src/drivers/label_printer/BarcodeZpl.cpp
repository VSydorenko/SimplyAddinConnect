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

BarcodeEmit BarcodeZpl::Emit(const BarcodeField& f, const std::string& value, int dotsPerMm) {
    BarcodeEmit e;
    const long x = mmToDots(f.geom.left, dotsPerMm), y = mmToDots(f.geom.top, dotsPerMm);
    const long h = mmToDots(f.geom.height, dotsPerMm);
    const char hri = f.printHRI ? 'Y' : 'N';
    char buf[256];
    const std::string& t = f.type;
    std::string cmd, data = value;

    if (t=="EAN13") {                       // ^BE приймає 12 цифр
        std::string d = digitsOnly(value); if (d.size()==13) d = d.substr(0,12);
        std::snprintf(buf,sizeof(buf),"^BEN,%ld,%c,N", h, hri); cmd=buf; data=d;
    } else if (t=="EAN8") {
        std::string d = digitsOnly(value); if (d.size()==8) d=d.substr(0,7);
        std::snprintf(buf,sizeof(buf),"^B8N,%ld,%c,N", h, hri); cmd=buf; data=d;
    } else if (t=="Code128"||t=="EAN128") {
        std::snprintf(buf,sizeof(buf),"^BCN,%ld,%c,N,N", h, hri); cmd=buf;
        if (t=="EAN128") data = ">;>8" + value;  // FNC1 (GS1-128); повний GS1-препроцесор — окремо
    } else if (t=="Code39") { std::snprintf(buf,sizeof(buf),"^B3N,N,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="Code93")  { std::snprintf(buf,sizeof(buf),"^BAN,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="ITF14")   { std::snprintf(buf,sizeof(buf),"^B2N,%ld,%c,N",h,hri); cmd=buf; }
    else if (t=="QRCode")  { cmd="^BQN,2,5"; }
    else if (t=="DataMatrix"){ cmd="^BXN,5,200"; }
    else if (t=="PDF417")  { cmd="^B7N,5,5"; }
    else if (t=="GS1DataBarExpandedStacked"){ std::snprintf(buf,sizeof(buf),"^BRN,6,5,2,,%ld",h); cmd=buf; }
    else { e.ok=false; e.errCode="UNSUPPORTED_BARCODE"; e.errDesc="Тип штрихкоду не підтримується нативно: "+t; return e; }

    std::snprintf(buf,sizeof(buf),"^FO%ld,%ld^BY2", x, y);
    e.zpl = std::string(buf) + cmd + EscapeFd(data) + "^FS";
    e.ok = true; return e;
}
} // namespace labelprinter
