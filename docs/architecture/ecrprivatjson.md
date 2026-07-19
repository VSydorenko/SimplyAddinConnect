# Архітектура стеку ECRPrivatJSON (платіжний термінал)

Документ описує підсистему взаємодії з платіжним терміналом ПриватБанку — від
компоненти 1С `AddinECRPrivatJSON` до конкретного каналу зв'язку (COM/TCP/WebSocket).
Це найзріліша підсистема проєкту SimplyAddinConnect. Наратив звірено з кодом гілки
`add_UAPKI`; специфікація самого протоколу — у `docs/ECR_Privat_JSON_Protokol.md`.

---

## 1. Трирівнева структура

Підсистема побудована як три рівні плюс змінний транспорт:

```
AddinECRPrivatJSON  (компонента, методи для 1С)
        │  std::unique_ptr<ECRPrivatJSONProtocol>
        ▼
ECRPrivatJSONProtocol  (логіка: підключення, хендшейк, фін./сервісні операції)
        │  std::unique_ptr
        ├──────────────► ECRPrivatJSONHelper  (JSON, буфер, синхронізація очікування)
        └──────────────► ITransport            (канал зв'язку: COM/TCP/WS)
```

- `AddinECRPrivatJSON` (компонента 1С) успадковує `AddInNative`
  (`src/components/AddinECRPrivatJSON.h`). Вона **тонка**: транслює виклики 1С у
  протокол і формує відповідь.
- Компонента володіє `std::unique_ptr<ECRPrivatJSON::ECRPrivatJSONProtocol> protocol_`
  (`src/components/AddinECRPrivatJSON.h`). Конструктор створює `protocol_` і викликає
  `RegisterMethods()`; деструктор викликає `protocol_->Disconnect()` та
  `ServiceTools::DisableComponentLogging(this)`.
- `ECRPrivatJSONProtocol` володіє `std::unique_ptr<ITransport> transport_` і
  `std::unique_ptr<ECRPrivatJSONHelper> helper_`
  (`src/protocols/ECRPrivatJSON/ECRPrivatJSON.h`).

Кожен верхній рівень не знає деталей нижнього: компонента бачить лише протокол,
протокол — лише інтерфейси `ECRPrivatJSONHelper` та `ITransport`.

### 1.1. Методи компоненти для 1С

Реєстрація відбувається в `AddinECRPrivatJSON::RegisterMethods()`
(`src/components/AddinECRPrivatJSON.cpp`). Кожен метод має два імені (англ. + укр.):

| Метод (EN / RU) | Призначення |
|---|---|
| `EnableLogging` / `ИспользоватьЛогирование` | Увімкнення логування |
| `ConnectCOM` / `ПодключитьCOM` | Підключення по COM-порту (baudRate за замовч. 115200) |
| `ConnectTCP` / `ПодключитьTCP` | Підключення по TCP (порт за замовч. 2000) |
| `ConnectWebSocket` / `ПодключитьWebSocket` | Підключення по WebSocket (авто-префікс `ws://`) |
| `Connect` / `Подключить` | Автовизначення транспорту за рядком підключення (`DetermineTransportType`) |
| `Disconnect` / `Отключить` | Відключення |
| `IsConnected` / `ПроверитьПодключение` | Перевірка стану підключення |
| `Payment` / `Оплата` | Оплата (amount, discount, merchantId, subMerchant) |
| `Refund` / `Возврат` | Повернення (amount, rrn, discount, merchantId, subMerchant) |
| `Settlement` / `СверкаИтогов` | Звірка підсумків (merchantId) |
| `GetTerminalInfo` / `ИнформацияОТерминале` | Інформація про термінал |
| `GetLastResponseCode` / `ПолучитьКодОтвета` | Код відповіді останньої операції |
| `GetLastResponseDescription` / `ПолучитьОписаниеОтвета` | Опис відповіді |
| `GetLastReceipt` / `ПолучитьТекстЧека` | Текст чека |
| `GetLastRRN` / `ПолучитьRRN` | RRN операції |
| `GetLastApprovalCode` / `ПолучитьКодАвторизации` | Код авторизації |
| `GetLastCardPAN` / `ПолучитьНомерКарты` | Номер картки |
| `GetLastAmount` / `ПолучитьСуммуОперации` | Сума операції |
| `IsLastOperationSuccess` / `УспешнаЛиОперация` | Ознака успіху |

Геттери `GetLast*` читають поля збереженого `lastTerminalResponse_` компоненти.
`DetermineTransportType` (`src/components/AddinECRPrivatJSON.cpp`) розбирає рядок
підключення і обирає тип каналу. Перевірка успіху коду —
`IsSuccessCode(responseCode)` порівнює з `SUCCESS` ("0000"), `SUCCESS_SHORT` ("00"),
`PARTIAL_APPROVAL` ("0010").

> Примітка: методи звітів (`GetDailyReport`, `GetXReport`, `GetZReport`) та `GetReceipt`
> існують на рівні `ECRPrivatJSONProtocol` (див. §2.4), але **не** виведені як
> окремі 1С-методи компоненти — у `RegisterMethods()` для них немає `AddFunction`.

---

## 2. `ECRPrivatJSONProtocol` — логіка протоколу

`src/protocols/ECRPrivatJSON/` (заголовок `ECRPrivatJSON.h`). Реалізація навмисно
розбита по файлах відповідно до груп операцій:

| Файл | Вміст |
|---|---|
| `ECRPrivatJSON_Connection.cpp` | `ConnectCOM` / `ConnectTCP` / `ConnectWebSocket` / `Disconnect` / `IsConnected` |
| `ECRPrivatJSON_Handshake.cpp` | `PerformHandshake` / `IdentifyTerminal` / `GetTerminalInfo` |
| `ECRPrivatJSON_FinancialOperations.cpp` | `Payment` / `Refund` / `Settlement` / `GetReceipt` |
| `ECRPrivatJSON_ServiceOperations.cpp` | `GetDailyReport` / `GetXReport` / `GetZReport` / `GetLastResponse` |
| `ECRPrivatJSON_Internal.cpp` | `OnDataReceived` / `OnError` / `OnConnectionStateChanged` |

### 2.1. Підключення

Три методи підключення — `ConnectCOM`, `ConnectTCP`, `ConnectWebSocket`
(`ECRPrivatJSON_Connection.cpp`) — виконують спільну послідовність:

1. створюють `transport_` потрібного типу і `helper_`;
2. прив'язують колбеки транспорту до методів протоколу (`OnDataReceived` /
   `OnError` / `OnConnectionStateChanged`) через лямбди виду
   `[this](...){ this->OnDataReceived(data); }`;
3. відкривають канал;
4. виконують хендшейк (`PerformHandshake`) — до 3 спроб;
5. виконують ідентифікацію (`IdentifyTerminal`);
6. на будь-якій невдачі — `Disconnect()` і повернення `false`.

> Оголошений приватний метод-фабрика `CreateTransport` (`ECRPrivatJSON.h`) у
> прочитаних `.cpp`-файлах реалізації не має — транспорт фактично створюється
> прямо в кожному з трьох `Connect*`-методів. `CreateTransport` виглядає як
> мертвий/нереалізований код (потребує підтвердження — див. кінець документа).

### 2.2. Хендшейк (`PerformHandshake` + `IdentifyTerminal`)

Хендшейк виконується під час підключення **після** створення `transport_` + `helper_`.

- `PerformHandshake()` (`ECRPrivatJSON_Handshake.cpp`) формує запит методом
  `"PingDevice"` через `helper_->BuildRequest("PingDevice", params, true)` — третій
  аргумент `true` означає режим хендшейку і додає **провідний нуль-байт** `0x00`
  перед JSON. Запит надсилається напряму через `transport_->Send`; перед відправкою
  виставляються `waitingForResponse_ = true; responseReceived_ = false;`. Відповідь
  очікується через `helper_->WaitForResponse(5000)` (таймаут 5 с), успіх
  перевіряється через `helper_->IsSuccess(response)`. Логіка з до 3 спроб і паузою
  `500 * attempt` мс між ними реалізована у самих `Connect*`-методах
  (`ECRPrivatJSON_Connection.cpp`).
- `IdentifyTerminal()` формує запит методом `"ServiceMessage"` з
  `params["msgType"] = "identify"`, надсилає, чекає `helper_->WaitForResponse(5000)`,
  і **вручну** парсить JSON відповіді (через `json::parse`, читаючи
  `params.terminalName` / `params.terminalSerialNum`), а не через
  `ParseTerminalResponse`. Формує `terminalInfo = terminalName + " " + terminalSerialNum`.

Специфікація протоколу підтверджує: хендшейк — це повідомлення Ping з
нуль-термінатором `0x00` у кінці JSON (`docs/ECR_Privat_JSON_Protokol.md`).

> Розбіжність із «еталонною схемою» документації: `docs/ECR_Privat_JSON_Protokol.md`
> описує послідовність конект → хендшейк → дисконект → Identify → дисконект →
> основний режим. Реалізація в коді **не** робить проміжних дисконектів: хендшейк і
> `IdentifyTerminal` виконуються послідовно на одному відкритому з'єднанні
> (`ECRPrivatJSON_Connection.cpp`).

### 2.3. Фінансові операції

`Payment`, `Refund`, `Settlement`, `GetReceipt` (`ECRPrivatJSON_FinancialOperations.cpp`)
поділяють спільний патерн: валідація → формування `params` (суми через
`helper_->FormatAmount`) → визначення методу через
`helper_->OperationTypeToString(...)` → `helper_->BuildRequest` →
`helper_->SendReceive(request, timeout)` → `helper_->ParseTerminalResponse` у
`lastResponse_`.

| Операція | Тип операції (рядок) | Таймаут | Особливості |
|---|---|---|---|
| `Payment` | `Purchase` (`OperationType::Purchase`) | 120000 мс | валідація `amount > 0` |
| `Refund` | `Refund` | 120000 мс | додає `rrn` у `params` |
| `Settlement` | `Verify` | 120000 мс | — |
| `GetReceipt` | `PrintReceiptNum` | 60000 мс | вимагає непорожній `transactionId` |

### 2.4. Сервісні операції (звіти)

`GetDailyReport`, `GetXReport`, `GetZReport` (`ECRPrivatJSON_ServiceOperations.cpp`)
спершу перевіряють `IsConnected()` (повертають `false`, якщо не підключено),
формують запит і викликають `helper_->SendReceive(request)` з дефолтним таймаутом,
результат кладуть у `lastResponse_.jsonResponse` і парсять через `ParseTerminalResponse`.

| Звіт | Тип операції (рядок) |
|---|---|
| `GetDailyReport` | `Verify` |
| `GetXReport` | `XReport` |
| `GetZReport` | `ZReport` |

### 2.5. Асинхронний прийом (колбеки + примітиви синхронізації)

Протокол тримає власний набір полів синхронізації (`ECRPrivatJSON.h`):
`std::mutex bufferMutex_`, `std::condition_variable dataCondition_`,
`std::atomic<bool> connected_`, `std::atomic<bool> waitingForResponse_`,
`std::atomic<bool> responseReceived_`, `std::string receivedResponse_`,
`std::vector<uint8_t> dataBuffer_`.

- `OnDataReceived(data)` (`ECRPrivatJSON_Internal.cpp`) під
  `std::lock_guard<std::mutex> lock(bufferMutex_)` додає дані у `dataBuffer_`, шукає
  нуль-байт-термінатор (`std::find(..., 0)`); при знаходженні вирізає JSON-повідомлення
  до термінатора. Якщо `waitingForResponse_` було `true` — зберігає повідомлення у
  `receivedResponse_`, ставить `responseReceived_ = true`, `waitingForResponse_ = false`
  і викликає `dataCondition_.notify_all()`; інакше логує як «неочікуване повідомлення
  від терміналу» (з TODO про обробку ініціативних повідомлень). Далі видаляє оброблене
  повідомлення з буфера і повторює цикл, доки в пакеті лишаються нуль-термінатори
  (підтримка кількох повідомлень в одному пакеті).
- `OnError(errorMessage, errorCode)` — лише логує помилку транспортного рівня.
- `OnConnectionStateChanged(connected)` — оновлює `connected_` і логує зміну стану
  (лише якщо стан справді змінився).

> **Важлива розбіжність (спостереження з коду, не запуску).** Транспортні колбеки
> прив'язані **лише** до `Protocol::OnDataReceived`, який заповнює *власні*
> `dataBuffer_` / `dataCondition_` / `responseReceived_` протоколу. Натомість
> фінансові й звітні операції викликають `helper_->SendReceive(...)`, який усередині
> чекає `WaitForResponse` на *власних* `bufferMutex_` / `dataCondition_` /
> `responseReceived_` **хелпера**. Єдиний метод, що сигналить примітиви хелпера, —
> `ECRPrivatJSONHelper::ProcessReceivedData`, і він **ніде в кодовій базі не
> викликається** (перевірено грепом — єдине входження, крім оголошення, це саме
> визначення методу). Тобто в коді існують дві паралельні, не з'єднані між собою
> буферно-синхронізаційні підсистеми — у `Protocol` і в `Helper`. Це задокументований
> факт із коду. Функціональний наслідок (наприклад, що `SendReceive` завжди
> завершується таймаутом) — логічний висновок, який варто перевірити збіркою/тестом.

---

## 3. `ECRPrivatJSONHelper` — формат, буфери, синхронізація

`src/helpers/ECRPrivatJSON/` (заголовок `ECRPrivatJSONHelper.h`). Розбитий на файли
`_Request`, `_Response`, `_Parsing`, `_Terminal`, `_Utils`, `_Service`. Відповідає за
низькорівневу роботу з форматом протоколу.

### 3.1. Формування запиту й нуль-термінатори

- `BuildRequest(method, params = {}, isHandshake = false)`
  (`ECRPrivatJSONHelper_Request.cpp`) створює JSON виду
  `{"method": ..., "step": 0[, "params": {...}]}` через `nlohmann::json`, серіалізує
  (`dump()`), потім викликає `AddNullTerminator(jsonString, isHandshake)`.
- `AddNullTerminator(json, isHandshake)`: якщо `isHandshake == true` — додає провідний
  байт `0x00` перед JSON; завжди додає завершальний `0x00` у кінці. Це відповідає
  специфікації (`docs/ECR_Privat_JSON_Protokol.md`): JSON без пробілів, роздільник
  повідомлень — `0x00`, для хендшейку — додатковий провідний `0x00`.

### 3.2. Відправка й очікування

- `SendReceive(request, timeout = 30000)` (`ECRPrivatJSONHelper_Request.cpp`): перевіряє
  `transport_` та `IsOpen()`, конвертує рядок у `std::vector<uint8_t>`, під
  `bufferMutex_` очищає `dataBuffer_` / `responseReceived_`, викликає
  `transport_->Send(requestData)`, далі `WaitForResponse(timeout)`, `GetResponse()`,
  `ResetResponseState()`. Дефолтний таймаут — 30000 мс (`ECRPrivatJSONHelper.h`).
- `WaitForResponse(timeout)` (`ECRPrivatJSONHelper_Response.cpp`): під
  `std::unique_lock<std::mutex> lock(bufferMutex_)` викликає
  `dataCondition_.wait_for(lock, std::chrono::milliseconds(timeout), [this]{ return responseReceived_; })`.
- `GetResponse()` — повертає `response_` під локом; `ResetResponseState()` — скидає
  `responseReceived_ = false` і очищає `response_`.
- `ProcessReceivedData(data)` (`ECRPrivatJSONHelper_Response.cpp`) — накопичує дані у
  `dataBuffer_` під локом, шукає нуль-термінатор через приватний `FindNullTerminator()`,
  при знаходженні заповнює `response_`, ставить `responseReceived_ = true`, викликає
  `dataCondition_.notify_one()`. **Цей метод не викликається жодним колбеком транспорту**
  (див. розбіжність у §2.5).

> Задокументована в `ARCHITECTURE.md` схема `SendReceive → ProcessReceivedData →
> WaitForResponse → GetResponse` описує архітектурний *намір*: у дослівному коді
> `SendReceive` не викликає `ProcessReceivedData` (лише очищає буфер, шле дані, чекає,
> отримує, скидає стан).

### 3.3. Парсери відповіді

- `ParseJSON(jsonString)` (`ECRPrivatJSONHelper_Parsing.cpp`) — нормалізує через
  `NormalizeResponseJson`, валідує через `IsJsonValid`, парсить через
  `nlohmann::json::parse`.
- `NormalizeResponseJson(jsonString)` — прибирає провідні/кінцеві нуль-байти й
  непечатні символи (< 32), внутрішні `\0` замінює на пробіл.
- `ParseTerminalResponse(jsonResponse, response)` — нормалізує, валідує, парсить JSON,
  очищає `response`, зберігає нормалізований JSON у `response.jsonResponse`, виставляє
  `response.success = IsSuccess(...)`, потім мапить поле `method` і поля з `params`:
  `responseCode`, `errorMessage` (за наявності `success = false`), `receiptText`,
  `operationStatus`, `totalAmount`, `currency`, `transactionId → transactionID`,
  `terminalId → terminalID`, `approvalCode`, `rrn`, `cardPAN`, `cardExpDate`,
  `cardHolder`, `merchantId → merchantID`, `AID → aid`, `paymentStatus`, `bankName`,
  `refundNDSPerc`, `refundNDSAmount`.
- `IsSuccess(jsonResponse)` (`ECRPrivatJSONHelper.cpp`) — успіх визначається кодом
  `params.responseCode` ∈ { `SUCCESS` "0000", `SUCCESS_SHORT` "00",
  `PARTIAL_APPROVAL` "0010" }. Спецвипадок коду "10": для методу `"GetReceiptInfo"` —
  завжди успіх; для інших методів — успіх якщо `error == false`. Якщо `responseCode`
  відсутній — fallback на поле `error` (успіх = `!error`). Якщо нічого не визначено —
  неуспіх.
- Точкові екстрактори (усі читають `params.<key>`, повертають default/""):
  `ExtractValueByKey`, `ExtractReceiptText` (`params.receiptText`),
  `ExtractResponseCode` (`params.responseCode`), `ExtractErrorMessage`
  (`params.errorMessage`), `ExtractTransactionId` (`params.transactionId`),
  `ExtractTerminalInfo` (лише якщо `method == "GetTerminalInfo"`: `vendor/model/
  serialNumber/firmware`, дефолт "Unknown"), `CheckHandshakeResult` (успіх якщо
  `method == "PingDevice"`).
- `FormatAmount(amount, precision = 2)` (`ECRPrivatJSONHelper_Utils.cpp`) — обмежує
  суму в `[0, 999999.99]` (від'ємні → 0, надто великі → 999999.99), форматує через
  `std::ostringstream` з `std::fixed` + `setprecision`; при виявленні наукової нотації
  форматує вручну.

### 3.4. Типи (`ECRPrivatJSON_Types.h`)

- `namespace ResponseCodes` — рядкові константи кодів: `SUCCESS` "0000",
  `SUCCESS_SHORT` "00", `PARTIAL_APPROVAL` "0010", `GENERAL_ERROR` "9999",
  `TIMEOUT` "0908", `CANCELLED` "0999", плюс коди "1000"–"1008".
- `enum class OperationType` (44 значення) і `OperationTypeToString(...)`
  (`ECRPrivatJSONHelper_Utils.cpp`). Практично всі значення enum мають відповідний
  `case`, окрім `OperationType::Payment` (це псевдонім `Purchase` в enum) — для нього
  спрацьовує `default:` → повертає `"Unknown"` і логує попередження.
- `enum class ServiceMessageType` (18 значень) і `ServiceMessageTypeToString(...)` —
  для більшості значень рядок у нижньому camelCase (напр. "identify"), крім перших
  трьох у PascalCase ("GetTerminalInfo", "PingDevice", "RunCommand").
- `struct TerminalResponse` — поля відповіді терміналу (переважно `std::string`), плюс
  дубльовані булеві `success` та `isSuccess`; `Clear()` скидає поля.

> Дослівний факт із коду: `ECRPrivatJSONHelper_Parsing.cpp` містить рядок
> `response.totalAmount = std::stod(amountStr);`, тоді як `TerminalResponse::totalAmount`
> оголошено як `std::string` (`ECRPrivatJSON_Types.h`). `std::stod` повертає `double` —
> тип не збігається з полем. Чи це компілюється — не перевірялося (див. кінець документа).

---

## 4. Транспортний шар

`src/transport/`. Єдиний інтерфейс `ITransport` (`Transport.h`) — усі канали
виглядають однаково для протоколу:

```cpp
bool Open();  bool Close();  bool IsOpen() const;
int  Send(const std::vector<uint8_t>& data);
void SetDataReceivedCallback(DataReceivedCallback);   // асинхронний прийом
void SetErrorCallback(ErrorCallback);
void SetConnectionStateCallback(ConnectionStateCallback);
```

Типи колбеків (`Transport.h`):
`DataReceivedCallback = std::function<void(const std::vector<uint8_t>&)>`,
`ErrorCallback = std::function<void(const std::string&, int)>`,
`ConnectionStateCallback = std::function<void(bool)>`.

### 4.1. Реалізації

| Клас | Файл | Канал |
|---|---|---|
| `TransportCOM` | `Transport_COM.*` | Послідовний порт |
| `TransportTCP` | `Transport_TCP.*` | TCP-сокет (клієнт або сервер) |
| `TransportWSClient` | `Transport_WSClient.*` | WebSocket-клієнт (через `ixwebsocket`) |
| `TransportWSServer` | `Transport_WSServer.*` | WebSocket-сервер (через `ixwebsocket`) |

- **`TransportCOM`** — конструктор `(portName, baudRate = 9600, dataBits = 8,
  parity = 'N', stopBits = 1.0f)`; має `ConfigurePort`, `SetTimeouts` (5 параметрів
  `DWORD`), `GetPortName`. Читання — в окремому потоці (`m_readThread` /
  `ReadThreadFunction` / `StartReadThread` / `StopReadThread`), стан —
  `std::atomic<bool> m_isOpen` / `m_threadRunning`.
- **`TransportTCP`** — два конструктори: клієнтський `(host, port)` і серверний
  `explicit (int port, int maxConnections = 5)`; `SetTimeout(timeoutMs)`; внутрішньо
  `ConnectAsClient` / `StartServer`; окремі потоки читання й `accept` (`m_readThread`,
  `m_acceptThread`); `SOCKET m_socket`, `m_serverSocket`.
- **`TransportWSClient`** — конструктор `(url, protocols = {})`; використовує
  `ix::WebSocket`; `SetTimeout(timeoutSecs)`, `SetPingInterval`, `SetExtraHeaders`;
  приватний `OnMessageCallback(ix::WebSocketMessagePtr)`.
- **`TransportWSServer`** — конструктор `(port, host = "127.0.0.1",
  maxConnections = 32)`; використовує `ix::WebSocketServer`; має
  `SendToClient(clientId, data)`, `SetPingInterval`, `EnableCompression`; тримає мапу
  клієнтів `std::map<std::shared_ptr<ix::WebSocket>, std::string> m_clients`.

### 4.2. Подієвий прийом і синхронний виклик поверх асинхронного каналу

Прийом даних — **подієвий**: транспорт викликає `DataReceivedCallback`, протокол
складає байти у свій буфер і сигналить очікувальнику через умовну змінну. Це дозволяє
для 1С виглядати як синхронний виклик («надіслати й дочекатися відповіді») поверх
принципово асинхронного каналу.

Механізм sync-over-async:

1. 1С викликає метод компоненти (напр. `Payment`) **синхронно** — метод повертає
   результат одразу (`src/components/AddinECRPrivatJSON.cpp`).
2. Усередині `ECRPrivatJSONProtocol::Payment` виклик `helper_->SendReceive(request, 120000)`
   **блокує** потік 1С-виклику в `WaitForResponse` /
   `dataCondition_.wait_for(...)` до сигналу або таймауту.
3. Дані від терміналу надходять **асинхронно** з фонового потоку читання транспорту
   (`ReadThreadFunction` у `TransportCOM` / `TransportTCP`, колбек `ix::WebSocket` у
   `TransportWSClient`) і потрапляють через `DataReceivedCallback` у
   `Protocol::OnDataReceived`, звідки нотифікується умовна змінна.

Формально це шаблон «блокуючий очікувач (`condition_variable::wait_for` з таймаутом) на
потоці виклику 1С + callback-нотифікатор на фоновому потоці транспорту».

> Застереження: як описано в §2.5, фактичну нотифікацію отримує умовна змінна
> *протоколу* (`Protocol::OnDataReceived → dataCondition_.notify_all()`), тоді як
> `helper_->SendReceive` очікує на умовній змінній *хелпера*. Опис вище відображає
> задуману схему sync-over-async; звірку фактичного проходження сигналу до
> `SendReceive` слід виконати окремо.

---

## Пов'язані документи

- `docs/ECR_Privat_JSON_Protokol.md` — специфікація JSON-протоколу терміналу
  (формат повідомлень, нуль-термінатори, коди відповіді, схема підключення).
- `docs/ARCHITECTURE.md` — загальна архітектура DLL (цей документ деталізує її §4–§5).
- `CMake/components.cmake` — джерело правди щодо складу й залежностей компонент.

---

## Потребує окремої перевірки (для верифікатора)

- Реалізацію `ECRPrivatJSONProtocol::CreateTransport` не знайдено серед прочитаних
  `.cpp` — ймовірно мертвий код; не шукалося по всьому дереву `src`.
- Розбіжність двох буферно-синхронізаційних підсистем (§2.5): `ProcessReceivedData`
  хелпера не викликається жодним колбеком; функціональний наслідок (таймаут
  `SendReceive`) — логічний висновок, не підтверджений збіркою/тестом.
- Рядок `response.totalAmount = std::stod(amountStr);` технічно несумісний із типом
  поля `std::string` — компільованість не перевірялася.
