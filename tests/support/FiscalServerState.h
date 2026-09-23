#pragma once
// FiscalServerState — стан імітації фіскального сервера ДПС: ПРРО, зміни, локальна й
// фіскальна нумерація, збережені документи, підсумки змін; розбір XML документа.
// Чиста логіка: без HTTP і без крипто (спека
// docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §4).
// НЕ потокобезпечний: викликач (PrroFsService) тримає власний м'ютекс.
#include <cstdint>
#include <string>
#include <vector>

namespace prrofs {

// Коди помилок ДПС ([Опис] ~755-875) — лише ті, що породжує імітація.
enum ErrorCode : int {
    kOk                          = 0,
    kTransactionsRegistrarAbsent = 1,
    kShiftAlreadyOpened          = 4,
    kShiftNotOpened              = 5,
    kLastDocumentMustBeZRep      = 6,
    kCheckLocalNumberInvalid     = 7,
    kZRepAlreadyRegistered       = 8,
    kDocumentValidationError     = 9,
    kInvalidQueryParameter       = 11,
};
const char* ErrorCodeName(int code);   // "CheckLocalNumberInvalid"…; невідомий — "Unknown"

enum class DocClass { Check, ZRep };

// Суми й відсотки — цілими копійками / сотими (150.00 -> 15000, 20.00 % -> 2000).
struct PayFormTotal {
    int         code = 0;
    std::string name;          // UTF-8
    int64_t     sum = 0;
};

struct TaxTotal {
    int         type = 0;
    std::string name;          // UTF-8
    std::string letter;        // UTF-8
    int64_t     prc = 0;
    bool        sign = false;
    int64_t     turnover = 0, turnoverDiscount = 0, sourceSum = 0, sum = 0;
};

struct OrderTypeTotals {
    int64_t sum = 0, rndSum = 0, noRndSum = 0;
    int     ordersCount = 0;
    std::vector<PayFormTotal> payForms;
    std::vector<TaxTotal>     taxes;
};

struct ShiftTotals {
    OrderTypeTotals real, ret;
    int64_t serviceInput = 0, serviceOutput = 0;
};

// Розібраний документ: заголовок + суми, потрібні для підсумків (§4.7).
struct ParsedDoc {
    DocClass    klass = DocClass::Check;
    int         docType = 0;        // DOCTYPE (CHECK)
    int         docSubType = 0;     // DOCSUBTYPE; відсутній = 0
    long long   orderNum = 0;       // ORDERNUM
    std::string cashRegisterNum;    // CASHREGISTERNUM
    std::string uid;                // UID
    std::string cashier;            // CASHIER, UTF-8
    bool        testing = false;    // TESTING
    int64_t     totalSum = 0, rndSum = 0, noRndSum = 0;   // CHECKTOTAL
    std::vector<PayFormTotal> pays;                       // CHECKPAY/ROW
    std::vector<TaxTotal>     taxes;                      // CHECKTAX/ROW
};

// Розбір XML документа ПРРО; кодування — з XML-декларації (windows-1251 або UTF-8,
// BOM UTF-8 допустимий). false + err — не розібрано.
bool ParseDocument(const std::string& xmlBytes, ParsedDoc& out, std::string& err);

// Вставити ORDERTAXNUM останнім елементом CHECKHEAD (наявний — замінити на місці).
// Робота на байтах (вставка ASCII), тож оголошене кодування документа не змінюється.
bool InsertOrderTaxNum(const std::string& xmlBytes, const std::string& taxNum, std::string& out);

// "150.00" -> 15000; "20" -> 2000; "-1.5" -> -150. Більше двох знаків після крапки,
// порожньо чи не число -> false.
bool        ParseMoney(const std::string& text, int64_t& out);
std::string FormatMoney(int64_t v);   // 15000 -> "150.00", -5 -> "-0.05"

std::string Cp1251ToUtf8(const std::string& s);
std::string Utf8ToCp1251(const std::string& s);

struct RegistrarSeed {
    std::string numFiscal;
    long long   nextLocalNum = 1;
};

struct ShiftRecord {
    long long   shiftId = 0;
    std::string opened, closed;             // ISO 8601; closed порожній — зміна не закрита
    long long   openedEpoch = 0;            // для фільтра Shifts From/To
    std::string openName, closeName;
    std::string openShiftFiscalNum, closeShiftFiscalNum, zRepFiscalNum;
    bool        testing = false;
};

struct RegistrarState {
    std::string numFiscal;
    int         numLocal = 0;               // порядковий номер ПРРО (1, 2, …) для Objects
    bool        shiftOpen = false;
    long long   shiftId = 0;                // поточна/остання зміна; 0 — змін не було
    std::string openShiftFiscalNum;
    bool        zRepPresent = false;
    bool        testing = false;
    std::string name;                       // CASHIER документа відкриття, UTF-8
    long long   firstLocalNum = 0;
    long long   nextLocalNum = 1;
    std::string lastFiscalNum;              // у відкритій зміні; порожньо = null
    ShiftTotals totals;
    std::vector<ShiftRecord> shifts;        // історія; поточна/остання — back()
};

struct StoredDoc {
    std::string registrar;
    long long   localNum = 0;
    std::string fiscalNum;
    DocClass    klass = DocClass::Check;
    int         docType = 0, docSubType = 0;
    long long   shiftId = 0;
    std::string registeredAt;               // ISO 8601
    std::string originalXml;                // побайтово, як прийшло всередині CMS
};

struct SubmitResult {
    int         errorCode = kOk;
    std::string errorText;                  // UTF-8; для коду 7 закінчується числом N
    std::string fiscalNum;                  // виданий, якщо errorCode == kOk
};

class FiscalServerState {
public:
    static constexpr long long kFirstFiscalNum = 100000001;

    void Reset(const std::vector<RegistrarSeed>& seeds);
    // Рішення по документу (§4.4) і, якщо прийнято, реєстрація (§4.5).
    SubmitResult Submit(const ParsedDoc& doc, const std::string& originalXml,
                        const std::string& nowIso, long long nowEpoch);

    const RegistrarState* Find(const std::string& numFiscal) const;
    const StoredDoc*      FindDoc(const std::string& registrar, long long localNum) const;
    const StoredDoc*      FindDocByFiscal(const std::string& registrar, const std::string& fiscalNum) const;
    std::vector<const RegistrarState*> Registrars() const;     // у порядку reset
    const std::vector<StoredDoc>&      Docs() const { return docs_; }

private:
    RegistrarState* FindMut(const std::string& numFiscal);
    static void AddToTotals(ShiftTotals& t, const ParsedDoc& d);

    std::vector<RegistrarState> regs_;
    std::vector<StoredDoc>      docs_;
    long long nextFiscal_  = kFirstFiscalNum;
    long long nextShiftId_ = 1;
};

}  // namespace prrofs
