#pragma once
// PrroFsService — обробник HTTP-запитів імітації фіскального сервера ДПС
// (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §5, §6, §8).
// Крипто — oracle::* (UapkiOracle), стан — FiscalServerState, збої — FaultPlan.
// Потокобезпечний: MiniHttpServer кличе Handle з потоку кожного з'єднання.
// УВАГА: тягне MiniHttpServer.h (winsock2.h) — включати ДО windows.h.
#include <ctime>
#include <mutex>
#include <string>

#include "FaultPlan.h"
#include "FiscalServerState.h"
#include "MiniHttpServer.h"

namespace prrofs {

// Годинник сервера = системний час + зсув (§5, §8.3).
std::time_t ServerNow(int skewSeconds);
std::string HttpDate(std::time_t t);    // RFC 1123, GMT: "Thu, 24 Sep 2026 09:00:00 GMT"
std::string IsoLocal(std::time_t t);    // ISO 8601 з місцевим зсувом: "2026-09-24T12:00:00+03:00"
std::string OrderDate(std::time_t t);   // ddmmyyyy, місцевий час
std::string OrderTime(std::time_t t);   // hhmmss, місцевий час
// ISO 8601 ("Z", "±hh:mm" або без зсуву = місцевий) чи "/Date(ms)/" -> epoch. false — не розібрано.
bool        ParseDateTime(const std::string& s, long long& epoch);
// "Код помилки: <n> <Назва>\r\n<опис>" ([Опис] ~899-901).
std::string ErrorBody(int code, const std::string& description);

// Розгортання тіла /fs/cmd (JSON або CMS) — ОДНОРАЗОВЕ й ЛІНИВЕ: обчислюється лише
// коли знадобиться (CommandNameOf для "cmd:<Команда>" або HandleCmd), інакше тіло
// лишається неторканим (Task 6, amendment A). Повне визначення — у .cpp (несе json).
struct UnwrappedCmd;

class PrroFsService {
public:
    explicit PrroFsService(int port) : port_(port) {}
    void SetPort(int port) { port_ = port; }
    void SetTrace(bool on) { trace_ = on; }

    minihttp::Response   Handle(const minihttp::Request& req);
    minihttp::HeaderList CommonHeaders();             // Date за годинником сервера

private:
    minihttp::Response HandleDoc(const std::string& body);
    minihttp::Response HandleCmd(const std::string& body, UnwrappedCmd& memo);
    minihttp::Response HandleControl(const std::string& body);
    minihttp::Response StateJson();
    minihttp::Response Reject(int code, const std::string& text, const ParsedDoc* doc);
    std::string        BuildTicket(const ParsedDoc* doc, const std::string& taxNum, std::time_t now,
                                   int errorCode, const std::string& errorTextUtf8);
    // Збої (Task 6):
    std::string        CommandNameOf(const std::string& body, UnwrappedCmd& memo);
    minihttp::Response FaultStatus(const Fault& f);
    static minihttp::Response FaultDrop(const Fault& f);

    std::mutex        mx_;          // state_ + faults_
    FiscalServerState state_;
    FaultPlan         faults_;
    int               port_  = 0;
    bool              trace_ = true;
};

}  // namespace prrofs
