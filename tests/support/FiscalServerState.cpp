// FiscalServerState — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "FiscalServerState.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pugixml.hpp"

#include <windows.h>

namespace prrofs {
namespace {

std::string Recode(const std::string& in, UINT fromCp, UINT toCp) {
    if (in.empty()) return in;
    const int wn = MultiByteToWideChar(fromCp, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (wn <= 0) return std::string();
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(fromCp, 0, in.data(), static_cast<int>(in.size()), &w[0], wn);
    const int n = WideCharToMultiByte(toCp, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(toCp, 0, w.data(), wn, &out[0], n, nullptr, nullptr);
    return out;
}

// Кодування з XML-декларації (нижній регістр). Немає декларації/атрибута — "utf-8".
std::string DeclaredEncoding(const std::string& x) {
    size_t start = 0;
    if (x.size() >= 3 && static_cast<unsigned char>(x[0]) == 0xEF
        && static_cast<unsigned char>(x[1]) == 0xBB && static_cast<unsigned char>(x[2]) == 0xBF) start = 3;
    if (x.compare(start, 5, "<?xml") != 0) return "utf-8";
    const size_t end = x.find("?>", start);
    const size_t enc = x.find("encoding", start);
    if (end == std::string::npos || enc == std::string::npos || enc > end) return "utf-8";
    const size_t q = x.find_first_of("\"'", enc);
    if (q == std::string::npos || q > end) return "utf-8";
    const size_t q2 = x.find(x[q], q + 1);
    if (q2 == std::string::npos || q2 > end) return "utf-8";
    std::string v = x.substr(q + 1, q2 - q - 1);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string ChildText(const pugi::xml_node& n, const char* name) {
    return n.child(name).text().as_string();
}

// Необов'язкове грошове поле: відсутнє = 0; присутнє нечислове -> false.
bool OptMoney(const pugi::xml_node& n, const char* name, int64_t& dst) {
    const std::string t = ChildText(n, name);
    if (t.empty()) { dst = 0; return true; }
    return ParseMoney(t, dst);
}

bool IsTrue(const std::string& s) { return s == "true" || s == "1"; }

void AddOrder(OrderTypeTotals& o, const ParsedDoc& d) {
    o.sum      += d.totalSum;
    o.rndSum   += d.rndSum;
    o.noRndSum += d.noRndSum;
    o.ordersCount += 1;
    for (const PayFormTotal& p : d.pays) {
        auto it = std::find_if(o.payForms.begin(), o.payForms.end(),
                               [&p](const PayFormTotal& x) { return x.code == p.code; });
        if (it == o.payForms.end()) o.payForms.push_back(p);
        else                        it->sum += p.sum;
    }
    for (const TaxTotal& t : d.taxes) {
        auto it = std::find_if(o.taxes.begin(), o.taxes.end(), [&t](const TaxTotal& x) {
            return x.type == t.type && x.letter == t.letter && x.prc == t.prc && x.sign == t.sign;
        });
        if (it == o.taxes.end()) { o.taxes.push_back(t); continue; }
        it->turnover         += t.turnover;
        it->turnoverDiscount += t.turnoverDiscount;
        it->sourceSum        += t.sourceSum;
        it->sum              += t.sum;
    }
}

}  // namespace

const char* ErrorCodeName(int code) {
    switch (code) {
        case kOk:                          return "Ok";
        case kTransactionsRegistrarAbsent: return "TransactionsRegistrarAbsent";
        case kShiftAlreadyOpened:          return "ShiftAlreadyOpened";
        case kShiftNotOpened:              return "ShiftNotOpened";
        case kLastDocumentMustBeZRep:      return "LastDocumentMustBeZRep";
        case kCheckLocalNumberInvalid:     return "CheckLocalNumberInvalid";
        case kZRepAlreadyRegistered:       return "ZRepAlreadyRegistered";
        case kDocumentValidationError:     return "DocumentValidationError";
        case kInvalidQueryParameter:       return "InvalidQueryParameter";
        default:                           return "Unknown";
    }
}

std::string Cp1251ToUtf8(const std::string& s) { return Recode(s, 1251, CP_UTF8); }
std::string Utf8ToCp1251(const std::string& s) { return Recode(s, CP_UTF8, 1251); }

bool ParseMoney(const std::string& text, int64_t& out) {
    if (text.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (text[0] == '-') { neg = true; i = 1; }
    if (i >= text.size()) return false;
    int64_t whole = 0;
    size_t digits = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) whole = whole * 10 + (text[i] - '0');
    if (digits == 0) return false;
    int64_t frac = 0;
    size_t fracDigits = 0;
    if (i < text.size() && text[i] == '.') {
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++fracDigits) frac = frac * 10 + (text[i] - '0');
        if (fracDigits == 0 || fracDigits > 2) return false;
        if (fracDigits == 1) frac *= 10;
    }
    if (i != text.size()) return false;
    out = whole * 100 + frac;
    if (neg) out = -out;
    return true;
}

std::string FormatMoney(int64_t v) {
    const bool neg = v < 0;
    const int64_t a = neg ? -v : v;
    char b[32];
    std::snprintf(b, sizeof(b), "%s%lld.%02lld", neg ? "-" : "",
                  static_cast<long long>(a / 100), static_cast<long long>(a % 100));
    return b;
}

bool ParseDocument(const std::string& xmlBytes, ParsedDoc& out, std::string& err) {
    out = ParsedDoc();
    const std::string enc = DeclaredEncoding(xmlBytes);
    std::string utf8;
    if (enc == "windows-1251" || enc == "cp1251")  utf8 = Cp1251ToUtf8(xmlBytes);
    else if (enc == "utf-8" || enc == "utf8")      utf8 = xmlBytes;
    else { err = "непідтримуване кодування: " + enc; return false; }
    // BOM UTF-8 (так лежать зразки ДПС) знімаємо самі — не покладаючись на поведінку парсера.
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF
        && static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) utf8.erase(0, 3);

    pugi::xml_document doc;
    const pugi::xml_parse_result pr =
        doc.load_buffer(utf8.data(), utf8.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!pr) { err = std::string("XML не розбирається: ") + pr.description(); return false; }

    const pugi::xml_node root = doc.document_element();
    const std::string rootName = root.name();
    pugi::xml_node head;
    if (rootName == "CHECK")     { out.klass = DocClass::Check; head = root.child("CHECKHEAD"); }
    else if (rootName == "ZREP") { out.klass = DocClass::ZRep;  head = root.child("ZREPHEAD"); }
    else { err = "невідомий корінь документа: " + rootName; return false; }
    if (!head) { err = "немає заголовка документа"; return false; }

    const std::string orderNum = ChildText(head, "ORDERNUM");
    out.cashRegisterNum = ChildText(head, "CASHREGISTERNUM");
    if (orderNum.empty() || out.cashRegisterNum.empty()) { err = "немає ORDERNUM або CASHREGISTERNUM"; return false; }
    char* e = nullptr;
    out.orderNum = std::strtoll(orderNum.c_str(), &e, 10);
    if (*e != '\0' || out.orderNum <= 0) { err = "ORDERNUM не є додатним числом: " + orderNum; return false; }
    out.uid     = ChildText(head, "UID");
    out.cashier = ChildText(head, "CASHIER");
    out.testing = IsTrue(ChildText(head, "TESTING"));
    if (out.klass == DocClass::ZRep) return true;

    out.docType    = head.child("DOCTYPE").text().as_int(0);
    out.docSubType = head.child("DOCSUBTYPE").text().as_int(0);
    const pugi::xml_node total = root.child("CHECKTOTAL");
    if (!OptMoney(total, "SUM", out.totalSum) || !OptMoney(total, "RNDSUM", out.rndSum)
        || !OptMoney(total, "NORNDSUM", out.noRndSum)) { err = "CHECKTOTAL: нечислове поле"; return false; }
    for (pugi::xml_node row : root.child("CHECKPAY").children("ROW")) {
        PayFormTotal p;
        p.code = row.child("PAYFORMCD").text().as_int(0);
        p.name = ChildText(row, "PAYFORMNM");
        if (!OptMoney(row, "SUM", p.sum)) { err = "CHECKPAY/ROW/SUM: не число"; return false; }
        out.pays.push_back(p);
    }
    for (pugi::xml_node row : root.child("CHECKTAX").children("ROW")) {
        TaxTotal t;
        t.type   = row.child("TYPE").text().as_int(0);
        t.name   = ChildText(row, "NAME");
        t.letter = ChildText(row, "LETTER");
        t.sign   = IsTrue(ChildText(row, "SIGN"));            // відсутній = false (§4.7)
        if (!OptMoney(row, "PRC", t.prc) || !OptMoney(row, "TURNOVER", t.turnover)
            || !OptMoney(row, "SOURCESUM", t.sourceSum) || !OptMoney(row, "SUM", t.sum)) {
            err = "CHECKTAX/ROW: нечислове поле";
            return false;
        }
        if (ChildText(row, "TURNOVERDISCOUNT").empty()) t.turnoverDiscount = t.turnover;   // §4.7
        else if (!OptMoney(row, "TURNOVERDISCOUNT", t.turnoverDiscount)) { err = "TURNOVERDISCOUNT: не число"; return false; }
        out.taxes.push_back(t);
    }
    return true;
}

bool InsertOrderTaxNum(const std::string& x, const std::string& taxNum, std::string& out) {
    static const char* kOpen  = "<ORDERTAXNUM>";
    static const char* kClose = "</ORDERTAXNUM>";
    const size_t headOpen  = x.find("<CHECKHEAD>");
    const size_t headClose = x.find("</CHECKHEAD>");
    if (headOpen == std::string::npos || headClose == std::string::npos || headClose < headOpen) return false;
    const std::string elem = std::string(kOpen) + taxNum + kClose;
    const size_t oldOpen = x.find(kOpen, headOpen);
    if (oldOpen != std::string::npos && oldOpen < headClose) {
        const size_t oldClose = x.find(kClose, oldOpen);
        if (oldClose == std::string::npos || oldClose > headClose) return false;
        out = x.substr(0, oldOpen) + elem + x.substr(oldClose + std::strlen(kClose));
        return true;
    }
    out = x.substr(0, headClose) + elem + x.substr(headClose);
    return true;
}

void FiscalServerState::Reset(const std::vector<RegistrarSeed>& seeds) {
    regs_.clear();
    docs_.clear();
    nextFiscal_  = kFirstFiscalNum;
    nextShiftId_ = 1;
    int i = 1;
    for (const RegistrarSeed& s : seeds) {
        RegistrarState r;
        r.numFiscal    = s.numFiscal;
        r.numLocal     = i++;
        r.nextLocalNum = s.nextLocalNum;
        regs_.push_back(r);
    }
}

RegistrarState* FiscalServerState::FindMut(const std::string& numFiscal) {
    for (RegistrarState& r : regs_) if (r.numFiscal == numFiscal) return &r;
    return nullptr;
}

const RegistrarState* FiscalServerState::Find(const std::string& numFiscal) const {
    for (const RegistrarState& r : regs_) if (r.numFiscal == numFiscal) return &r;
    return nullptr;
}

const StoredDoc* FiscalServerState::FindDoc(const std::string& registrar, long long localNum) const {
    for (const StoredDoc& d : docs_) if (d.registrar == registrar && d.localNum == localNum) return &d;
    return nullptr;
}

const StoredDoc* FiscalServerState::FindDocByFiscal(const std::string& registrar,
                                                    const std::string& fiscalNum) const {
    for (const StoredDoc& d : docs_) if (d.registrar == registrar && d.fiscalNum == fiscalNum) return &d;
    return nullptr;
}

std::vector<const RegistrarState*> FiscalServerState::Registrars() const {
    std::vector<const RegistrarState*> out;
    for (const RegistrarState& r : regs_) out.push_back(&r);
    return out;
}

void FiscalServerState::AddToTotals(ShiftTotals& t, const ParsedDoc& d) {
    switch (d.docSubType) {
        case 0:          AddOrder(t.real, d);              break;
        case 1:          AddOrder(t.ret, d);               break;
        case 2: case 3:  t.serviceInput  += d.totalSum;    break;
        case 4:          t.serviceOutput += d.totalSum;    break;
        default:                                           break;   // 5 сторно — поза обсягом (§12)
    }
}

SubmitResult FiscalServerState::Submit(const ParsedDoc& d, const std::string& originalXml,
                                       const std::string& nowIso, long long nowEpoch) {
    SubmitResult r;
    RegistrarState* reg = FindMut(d.cashRegisterNum);
    if (!reg) {
        r.errorCode = kTransactionsRegistrarAbsent;
        r.errorText = "ПРРО з фіскальним номером " + d.cashRegisterNum + " не зареєстрований";
        return r;
    }
    // Порядок перевірок — §4.4; номер раніше за стан — [припущення] спеки.
    if (d.orderNum != reg->nextLocalNum) {
        r.errorCode = kCheckLocalNumberInvalid;
        r.errorText = "Номер документа повинен дорівнювати " + std::to_string(reg->nextLocalNum);
        return r;
    }
    const bool isCheck = (d.klass == DocClass::Check);
    const bool isOpen  = isCheck && d.docType == 100;
    const bool isClose = isCheck && d.docType == 101;
    if (isOpen && reg->shiftOpen)   { r.errorCode = kShiftAlreadyOpened;     r.errorText = "Зміну вже відкрито";                       return r; }
    if (!isOpen && !reg->shiftOpen) { r.errorCode = kShiftNotOpened;         r.errorText = "Зміну не відкрито";                        return r; }
    if (isClose && !reg->zRepPresent) { r.errorCode = kLastDocumentMustBeZRep; r.errorText = "Останнім документом зміни має бути Z-звіт"; return r; }
    if (d.klass == DocClass::ZRep && reg->zRepPresent) { r.errorCode = kZRepAlreadyRegistered; r.errorText = "Z-звіт уже зареєстровано"; return r; }

    r.fiscalNum = std::to_string(nextFiscal_++);
    if (isOpen) {
        reg->shiftOpen          = true;
        reg->shiftId            = nextShiftId_++;
        reg->openShiftFiscalNum = r.fiscalNum;
        reg->zRepPresent        = false;
        reg->testing            = d.testing;
        reg->name               = d.cashier.empty() ? std::string("Тестовий касир") : d.cashier;
        reg->firstLocalNum      = d.orderNum;
        reg->totals             = ShiftTotals();
        ShiftRecord s;
        s.shiftId            = reg->shiftId;
        s.opened             = nowIso;
        s.openedEpoch        = nowEpoch;
        s.openName           = reg->name;
        s.openShiftFiscalNum = r.fiscalNum;
        s.testing            = d.testing;
        reg->shifts.push_back(s);
    } else if (d.klass == DocClass::ZRep) {
        reg->zRepPresent = true;
        reg->shifts.back().zRepFiscalNum = r.fiscalNum;
    } else if (isClose) {
        reg->shiftOpen = false;
        ShiftRecord& s = reg->shifts.back();
        s.closed              = nowIso;
        s.closeName           = d.cashier.empty() ? reg->name : d.cashier;
        s.closeShiftFiscalNum = r.fiscalNum;
    } else if (isCheck && d.docType == 0) {
        AddToTotals(reg->totals, d);
    }
    reg->nextLocalNum += 1;
    reg->lastFiscalNum = reg->shiftOpen ? r.fiscalNum : std::string();

    StoredDoc sd;
    sd.registrar    = reg->numFiscal;
    sd.localNum     = d.orderNum;
    sd.fiscalNum    = r.fiscalNum;
    sd.klass        = d.klass;
    sd.docType      = d.docType;
    sd.docSubType   = d.docSubType;
    sd.shiftId      = reg->shiftId;
    sd.registeredAt = nowIso;
    sd.originalXml  = originalXml;
    docs_.push_back(std::move(sd));
    return r;
}

}  // namespace prrofs
