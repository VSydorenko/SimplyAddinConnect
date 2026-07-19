**ECRCommX**

**for connecting to Ingenico POS-terminals using** **B-POS1 protocol**

(ActiveX-component)

ECRCommX B-POS1. Doc. Version 3.2.3 (2021-02-02)

# General information

The component realizes a high level application interface between Windows personal computer (PC) or Windows based POS systems for interaction with Ingenico EFT POS terminals using ECR protocol by RS232 or USB connections.

# Interface description

The component realizes the interface BPOS1Lib, with the help of which all the actions

are being fulfilled.

Running transactions for fulfilling, and also setting the initial parameters, are done with the help of methods of the interface BPOS1Lib. The methods are immediately returning control to the calling side because transactions are performing in a separate thread.

# System requirements and installation

Library is x86 based and should work under x86 application environment

Supported OS:

Windows 8 (x86 / x64), Windows XP (x86 / x64), Windows 7 (x86 /x64), Windows 2003, Windows 2008

Register x32 library in Windows x86:

Installation must be done under Administrator user rights. ECRCommX.dll can be copied to any location on local drive.

To register ActiveX use **regsvr32 ECRCommX.dll**

After successful registration of component, a corresponding message should appear.

Register in x64 library Windows x64:

Installation must be done under Administrator user rights. ECRCommX.dll can be copied to any location on local drive.

To register ActiveX use **regsvr32 ECRCommX.dll**

After successful registration of component, a corresponding message should appear.

How to register x32 library in Windows x64:

Installation must be done under Administrator user rights.

ECRCommX.dll can be copied to any location on local drive.

To register ActiveX use **C:\\windows\\syswow64\\regsvr32 ECRCommX.dll** After successful registration of component, a corresponding message should appear.

**Additional Requirements:**

**Visual Studio C++ Redistributable 2012 (from library version 1.7.0.0) Visual Studio C++ Redistributable 2005 (before library version 1.7.0.0)**

When initializing transaction, the _BPOS1Lib_ class creates a thread for connecting with terminal and returns the control. To get the results of transaction fulfilling there is used the property _LastResult_ (or method \*get_LastResult(**_pVal_**)*. If function returns 0 in _pVal_ value, then the transaction was done successfully, otherwise there was an error, the code of which can be obtained with the property _LastErrorCode_ (method _get_ErrorCode_), and the description with Error Description (method*get_ErrorDescription\*). See the list of error codes below.

**Note.** Before getting properties **CardNumber, ResponseCode** it is necessary to ensure that **LastResult** is not equal 2, i.e. the control thread has completed its fulfilling, otherwise values will not be defined.

# Error processing

For all functions two possible return values are available **– S_OK** in case of success, and **S_FALSE** in case of error.

If function returns **S_FALSE**, application should call _get_LastErrorCode()_ function for retrieving detailed information about the error occurred.

# Status messages processing

While terminal performs some transaction it can inform you about its current state. For example, about authorization on banking host or card reading process. To check this information use _LastStatMsgCode and LastStatMsgDescription properties_ to get the last status of terminal.

# Timeouts

Method CommOpen has no timeout, just trying to open port, that you put into this method;

Method CommOpenAuto throughout all active COM-ports on PC, performs ping to everyone with 500ms timeout; All other methods, like Purchase, Refund etc. has timeout 6 seconds.

# Interface methods

## CommOpen(BYTE bPort, LONG lBaudRate)

Opens communication port.

\*[_**IN**_]\* **bPort** – COM port number, 1– “COM1”, 2 – "COM2" etc.

\*[_**IN**_]\* **lBaudRate** – data transmission speed, e.g. 19200, 38400 etc.

## CommOpenTCP(BSTR bsIP, BSTR bsPort)

Opens communication port.

\*[_**IN**_]\* **bsIP** – IP address, e.g. “192.168.2.33”, “12.25.1.22” etc.

\*[_**IN**_]\* **bsPort** – IP-port, e.g. 1255, 2050 etc.

## CommOpenAuto(LONG lBaudRate)

The same as **CommOpen** except there is performed an automatic search of connected terminal on ports 1…255.

\*[_**IN**_]\* **lBaudRate** – data transmission speed, e.g. 19200, 38400 etc.

## CommClose(void)

Closes communication port opened with **CommOpen.**

_No parameters._

## CheckConnection(BYTE bMerchIdx)

Terminal will check connection to authorization host.

**[IN] bMerchIdx** – index of merchant, which will be used for this transaction. Starts from 1.

## Purchase(ULONG ulAmount, ULONG ulAddAmount, BYTE bMerchIdx)

Performs “Purchase” transaction.

_Purchase is withdrawal from customer's account to pay for goods / services._

**[IN] ulAmount** _–_ amount of purchase

**[IN] ulAddAmount** – additional amount (discount). May be used to set purchase discount. In this case ulAmount is original amount and ulAddAmount is discount. Thus, the final amount will be the following: ulAmount – ulAddAmount.

**[IN] bMerchIdx** – index of merchant, which will be used for purchase transaction.

Starts from 1.

_Confirmation to POS is required._

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## Refund(ULONG ulAmount, ULONG ulAddAmount, BYTE bMerchIdx, BSTR bsRRN)

Performs “Refund” transaction.

_Refund to customer’s account when he wants to return goods. Refund is allowed to be done at any time, regardless of time when the original purchase transaction was performed._ **[IN] ulAmount** _–_ amount of purchase

**[IN] ulAddAmount** – _reserved for future use_.

**[IN] bMerchIdx** – index of merchant which will be used for refund transaction. Starts from 1.

**[IN] bsRRN** – Retrieval reference number of original purchase transaction.

_Confirmation to POS is required for some financial applications._

**Please note, that for the financial application, where Refund can't be cancelled, the confirmation is optional. Please contact application vendor for clarifications.**

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## Void(ULONG ulInvoiceNum, BYTE bMerchIdx)

Performs “Void” transaction.

_Cancel the transaction (purchase or refund). Cancellation of the transaction can be carried out only until the end of settlement day, when the original transaction was performed, because funds that were debited from the customer's account will be credited to the merchant not earlier than the next settlement day._

\*[_**IN**_]_**ulInvoiceNum**_–\* invoice number of original transaction

**[IN] bMerchIdx** – index of merchant, which will be used for refund transaction. Starts from 1.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## Balance(BYTE bMerchIdx, BSTR bsCurrCode, BYTE bAccNumber)

Performs “Balance Inquiry” transaction.

Return information about balance account of customer\`s card.

**[IN] bMerchIdx** – index of merchant, which will be used for balance transaction. Starts from 1.

**[IN] bsCurrCode** _–_ currency code of transaction. Not mandatory. **[IN] bAccNumber** – account number. Not mandatory. **bAccNumber** has one of following values:

1. – DEFAULT
2. – SAVINGS
3. – CHECKING
4. – CREDIT
5. – UNIVERSAL

    _Confirmation to POS is required._

    _Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## Deposit(BYTE bMerchIdx, ULONG ulAmount, BSTR bsCurrCode, BYTE bAccNumber)

Performs “Deposit” transaction.

Deposit to customer's account.

**[IN] bMerchIdx** – index of merchant of original transaction. Starts from 1.

**[IN] ulAmount** _–_ amount of deposit.

**[IN] bsCurrCode** _–_ currency code of transaction. Not mandatory. **[IN] bAccNumber** – account number. Not mandatory. **bAccNumber** has one of following values:

1. – DEFAULT
2. – SAVINGS
3. – CHECKING
4. – CREDIT
5. – UNIVERSAL

    _Confirmation to POS is required._

    _Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## CashAdvance(BYTE bMerchIdx, ULONG ulAmount, BSTR bsCurrCode, BYTE bAccNumber)

Performs “Cash Advance” transaction.

Cash withdrawal and debiting of the card account by the specified amount.

**[IN] bMerchIdx** – index of merchant of original transaction. Starts from 1.

**[IN] ulAmount** _–_ amount of deposit.

**[IN] bsCurrCode** _–_ currency code of transaction. Not mandatory. **[IN] bAccNumber** – account number. Not mandatory. **bAccNumber** has one of following values:

1. – DEFAULT
2. – SAVINGS
3. –CHECKING
4. – CREDIT
5. – UNIVERSAL

    _Confirmation to POS is required._

    _Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## Completion(BYTE bMerchIdx, ULONG ulAmount, BSTR bsRRN, ULONG ulInvoiceNum)

Performs completion of previously performed transaction. Completion _is allowed to be_ _done after the original purchase transaction. Completion_ amount can\`t be bigger than amount of t*he original purchase transaction.*

**[IN] bMerchIdx** – index of merchant of original purchase transaction. Starts from 1.

**[IN] ulAmount** _–_ amount of completion

**[IN] bsRRN** – Retrieval reference number of original purchase transaction.

**[IN] ulInvoiceNum** – invoice number of original preauthorization _Confirmation to POS is required._

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## ReadCard(void)

This function is used to read only discount card

## ReadBankCard(void)

This function is used to read any card

## PurchaseService(BYTE bMerchIdx, ULONG ulAmount, BSTR bsServiceParams)

Performs “Service” transaction.

_Special operation, which sends additional data with parameters to the host using 63.89 field. Operation is supported only by TITP financial protocol ("Compass+" processing)._ **[IN] bMerchIdx** – index of merchant, which will be used for the service transaction. Starts from 1.

**[IN] ulAmount** _–_ amount of service operation.

**[IN] bsServiceParams** – string with "serv_num/nominal/parameters) format. "/" symbol must separate all the necessary values. For example, "0109//1/5". The first component is 4-digit substring (zero padding from left side must be performed), terminal will perform service operation using 109 service without nominal, but will include parameters "1" and "5".

_Confirmation to POS is required._

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## IdentifyCard(BYTE bMerchIdx, BSTR bsCurrCode, BYTE bAccNumber)

Performs “Identify card” transaction.

Return customer\`s RNK if identify card was successful.

**[IN] bMerchIdx** – index of merchant, which will be used for balance transaction. Starts from 1.

**[IN] bsCurrCode** _–_ currency code of transaction. Not mandatory. **[IN] bAccNumber** – account number. Not mandatory. **bAccNumber** has one of following values:

0 – DEFAULT 1 – SAVINGS

2 – CHECKING 3 – CREDIT

4 – UNIVERSAL

_Confirmation to POS is required._

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## POSGetInfo(void)

Returns POS information (software version, profile ID and the list of acquirers will be returned by _TerminalInfo_ property). _No parameters._

## POSExTransaction (void)

Performs InfoCall operation.

_InfoCall is the operation that sends service information to host (operation is supported only by TITP financial protocol and "Compass+" processing)._ _No parameters._

## Settlement(BYTE bMerchIdx)

Performs Settlement transaction.

_Settlement is the calculation of totals amount according to transaction data and sending them to authorization host. After Settlement transaction all transaction data will be deleted. Should be performed at the end of the settlement day._

Terminal will print a receipt (if printer is available) with totals of current batch and return these totals to component. To get them use _Totals properties (see description below)._

**[IN] bMerchIdx** – index of merchant, which will be used for refund transaction. Starts from 1. If equals 0, terminal will perform settlement and return totals for all available merchants.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## PrintBatchTotals(BYTE bMerchIdx)

Terminal will print receipt (if it’s possible) with totals of current batch\* and return these totals to component. To get them use _Totals properties (see description below)._

Does not perform Settlement transaction unlike Settlement() method.

_\*Batch – is the transaction data within current settlement day._

It prints the same receipt as after settlement.

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1. If equals 0, terminal will print receipts and return totals for all available merchants.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## PrintLastSettleCopy(BYTE bMerchIdx)

Terminal will print receipt of previous settlement transaction.

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1. If equals 0, terminal will print receipts and return totals for all available merchants.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## PrintBatchJournal(BYTE bMerchIdx)

Terminal will print receipt with info about all transactions of current batch.

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1. If equals 0, terminal will print receipts and return totals for all available merchants.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

## GetBatchTotals(BYTE bMerchIdx)

Terminal will return totals of current batch\* to component. To get them use _Totals_ _properties (see description below)._

Method performs same as PrintBatchTotals but terminal does not print receipt.

_\*Batch – is the transaction data within current settlement day._

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1. If equals 0, terminal will return totals for all available merchants.

## GetTxnDataByInv(ULONG ulInvoiceNum, BYTE bMerchIdx)

Terminal will return transaction data (Response code, PAN... etc.) of transaction with given Invoice Number.

**[IN] ulInvoiceNum** _–_ invoice number of required transaction

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1.

## GetTxnNum(void)

Terminal will return a number of transactions into transaction journal of terminal. To get number of transactions use TxnNum property.

## GetTxnDataByOrder(ULONG ulOrderNum)

Terminal will return transaction data (Response code, PAN... etc.) of transaction with given Order Number.

**[IN] ulOrderNum** _–_ order number of transaction in transaction journal. Starts from 1 and should not be greater than number of transactions (see GetTxnNum() method).

# ReqCurrReceipt(void)

This function is used to send request to POS for the content of current transaction receipt. (Approved / Decline)

In case of successful command receipt will be received by **get_Receipt**

# ReqReceiptByInv(ULONG ulInvoiceNum, BYTE bMerchIdx)

This function is used to get receipt of transaction with current invoice number.

In case of successful command receipt will be received by **get_Receipt**

**[IN] ulInvoiceNum** _–_ invoice number of required transaction.

**[IN] bMerchIdx** – index of merchant which will be used for current transaction. Starts from 1.

_Receipt can be received by calling ReqCurrReceipt function in case of successful response. Please see how to retrieve the receipt._

# Confirm(void)

This function is used to confirm the transaction for terminal. It should be used after terminal has finished the financial transaction/scenario (_Purchase, Refund, except Void_) and LastResult is eq. 0. **Otherwise terminal will cancel it.**

# SelectApp(BSTR bsAppName, ULONG ulAppIdx)

This function is used to select application on POS to proceed operations in the chosen app. Usage: after communication open, and before operation is needed to perform.

**[IN] bsAppName** – the name of application on POS, e.g. “TE7E”, “TE75” etc. Max size is 15 bytes.

**[IN] ulAppIdx** – index of application on POS, e.g. 12555, 3458 etc.

# CloseApp(void)

This function is used to close application on POS to finish all operation with app.

# StartScenario(ULONG ulScenarioID, BSTR bsScenarioData)

This function is used to run scenario on POS.

**[IN] ulScenarioID** – index of scenario on POS, e.g. 1, 17 etc.

**[IN] bsScenarioData** – the xml-buffer with scenario specification. Max size is 1200 bytes.

# SetExtraPrintData(BSTR bsExtraPrintData)

This function is used to set additional data before the financial transaction (_Purchase,_ _Refund, etc., except Void_). Data will be cleaned after each transaction.

**[IN] bsExtraPrintData** – the additional data which can be printed on receipt. Max size is 1024 bytes.

# SetExtraXmlData(BSTR bsExtraXmlData)

This function is used to set additional data before the financial transaction (_Purchase,_ _Refund, etc., except Void_). Data will be cleaned after each transaction.

**[IN] bsExtraXmlData** – the additional data which is presented XML. Max size is 512 bytes.

# SetErrorLang(BYTE bErrLanguage)

This function is used to set language of property (_messages that library returns_, _not POS_) **get_LastStatMsgDescription**.

**[IN] bErrLanguage** _–_ type of language:

1. – ENG
2. – UKR
3. – RUS

# SendFile(BSTR bsFullPath, BYTE bECRDataType, BYTE bECRCommand)

This function is used to send file to POS terminal.

**[IN] bsFullPath** _–_ location path of a file subject to printing:

**[IN] bECRDataType** _–_ type of file:

1. – receipt
2. – bmp
3. – data

    **[IN] bECRCommand** _–_ type of operation determining terminal’s actions towards received file:

4. – save
5. – print
6. – print and delete

# CorrectTransaction(ULONG ulAmount, ULONG ulAddAmount)

This function is used to send updated information when operation is in progress (LastStatMsgCode = 11).

**[IN] ulAmount** _–_ amount of purchase.

**[IN] ulAddAmount** – additional amount (discount). May be used to set purchase discount. In this case ulAmount is original amount and ulAddAmount is discount. Thus, the final amount will be the following: ulAmount – ulAddAmount.

# useLogging(BYTE bLoggingLevel, BSTR bsFilePath)

This function is used to set log level and full path to log file **[IN] bLoggingLevel** _–_ indicate level of log detailing:

1. – no logs
2. – simple log level
3. – detailed log level with get-function **[IN] bsFilePath** _–_ full path to log file.

# UseMac(BYTE macType, BSTR bsKey)

This function is used to set mac algorithm and key for it. If method is not called, BPOS will try to work without MAC verification **[IN] macType** _–_ indicate mac algorithm:

0 – AES

**[IN] bsKey** _–_ key in hex format.

# get_LastResult(BYTE\* pVal), property _LastResult_

Gets the result of fulfilling of the last transaction **[OUT] pVal**\*-\* Returned values:

1. – successfully fulfilled
2. – error (to get the error code, use the property LastErrorCode or the method get_LastErrorCode)
3. – in progress

# get_LastErrorCode (BYTE\* pVal), property _LastErrorCode_

**[OUT] pVal** _-_ returned values:

1. – error opening COM port
2. – need to open COM port
3. – error connecting with terminal
4. – terminal returned an error. For additional analysis Response Code is used (see below).

# get_LastErrorDescription(BSTR\* pVal), property _LastErrorDescription_

**[OUT] pVal** – returns error description – null-terminated string for error description. Max size is 100 bytes.

Gets text description of error either from the library or from the terminal (if _LastErrorCode_ is 4)

# Cancel(void)

Cancels fulfilling the operation in case **LastResult** returns 2.

# Ping(void)

This function is used to check terminal connection status except in case **LastResult** returns 2.

# CheckTerminal(void)

Check terminal connection status in the operation in case **LastResult** returns 2 and **LastStatMsgCode** doesn’t change a long time.

# get_LastStatMsgCode(BYTE\* pVal), property _LastStatMsgCode_

**[OUT] pVal -** returns one following status codes:

1. – status code is not available.
2. – card was read
3. – used a chip card
4. – authorization in progress
5. – waiting for cashier action
6. – printing receipt
7. – pin entry is needed
8. – card was removed
9. – EMV multi aid’s
10. – waiting for card
11. – in progress
12. – correct transaction
13. – Pin input wait key
14. – Pin input backspace pressed
15. – Pin input key pressed
16. – Account Selection
17. – Purchase only
18. – Confirmation Purchase only
19. – Waiting finger match verification

# get_LastStatMsgDescription(BSTR\* pVal), property _LastStatMsgDescription_

**[OUT] pVal** – returns error description – null-terminated string for status description. Max size is 100 bytes.

Gets text description of status from the terminal (if LastStatMsgCode is not 0)

# get_ResponseCode(ULONG\* pVal), property _ResponseCode_

**[OUT] pVal** – returns response code\*.\*

Is used in case of approved / not approved authorization. **Codes below 1000 are received from host.**

1. – General error (should be used in exceptional case)
2. – Transaction canceled by user
3. – EMV Decline
4. – Transaction log is full. Need close batch
5. – No connection with host
6. – No paper in printer
7. – Error Crypto keys
8. – Card reader is not connected
9. – Transaction is already complete

# get_Receipt (BSTR\* pVal), property _Receipt_

**[OUT] pVal** – returns null-terminated string for current receipt slip of transaction. Length is variable, maximum length is **32648** bytes _(reserved for 140-150 financial transactions in batch totals transaction log printing)_

_If the length eaqual to 0, it means no receipt slip. Should be requested in case LastResult_ _= 0 or (LastResult = 1 and LastErrorCode = 4) TEXT Encoding WIN1251._

# get_LibraryVersion(BSTR\* pVal), property _LibraryVersion_

**[OUT] pVal –** current library version.

_Gets the library version which is used on PC currently._

# ReqDataFile (void)

This function is used to send request to POS for the content of data file. In case of successful command data file will be received by get_DataFile

# get_DataFile (VARIANT\* pVal), property _DataFile_

**[OUT] pVal** – returns SAFEARRAY of bytes for data file. Maximum size is 204800 bytes

**Functions (properties) described below are available only for financial transactions (purchase, refund, void) and for GetTxnDataByInv and GetTxnDataByOrder. Otherwise they will return empty results.**

# get_PAN(BSTR\* pVal), property _PAN_

**[OUT] pVal** – returns PAN of card. Length is variable.

Gets the number of client’s account in case transaction successfully completed.

# get_PanHash(BSTR\* pVal), property _PanHash_

**[OUT] pVal** – returns PAN hash of card. Lengh is variable.

Gets the number of client’s account in case transaction successfully completed.

# get_SlipPrinted(BYTE\* pVal), property _SlipPrinted_

**[OUT] pVal** – returns result of print operation on POS-terminal.

1. – slip has not been printed
2. – slip has been printed

# get_DateTime(BSTR\* pVal), property _DateTime_

**[OUT] pVal** – returns null-terminated string for transaction date time

Gets transaction date and time in format YYMMDDhhmmss in case transaction successfully completed.

# get_TerminalID(BSTR\* pVal), property _TerminalID_

**[OUT] pVal** – returns null-terminated string for TerminalID. Length is 8 bytes. Gets Terminal ID.

# get_MerchantID(BSTR\* pVal), property _MerchantID_

**[OUT] pVal** – returns null-terminated string for MerchantID. Length is variable. Gets Merchant ID.

# get_AuthCode(BSTR\* pVal), property _AuthCode_

**[OUT] pVal** – returns null-terminated string for Authorization code of transaction. Length is 6 bytes. Gets Authorization Code.

# get_Amount(ULONG \*pVal), property _Amount_

**[OUT] pVal** – gets final amount. It can differ from initial amount that was sent from the ECR

# get_AddAmount(ULONG \*pVal), property _AddAmount_

**[OUT] pVal** – gets final additional amount. Additional amount can consist discount or bonus amount if it was used by terminal.

## get_TxnType(BYTE\* pVal), property _TxnType_

**[OUT] pVal** – gets transaction type of current transaction. Intended for usage for GetTxnDataByInv and GetTxnDataByOrder methods.

_TxnType_ has one of following values:

1. – undefined
2. – Purchase
3. – Refund
4. – Void
5. – Completion
6. – IdentifyCard
7. – BalanceInquiry
8. – Deposit
9. – CardVoucher
10. – Cashback // deprecated
11. – Installment
12. – CashAdvance
13. – AccountFunding
14. – AccountConfirmation
15. – PurchaseWithCashback

## get_EntryMode(BYTE\* pVal), property _EntryMode_

**[OUT] pVal** – Get type of the card that was used to perform transaction. Length - three bytes. _EntryMode_ has one of following values:

1. – undefined
2. – Magnetic stripe card
3. – EMV chip card
4. – Contactless chip card
5. – Contactless stripe card
6. – Fallback (magnetic stripe was used by card that has EMV chip)
7. – Manual (card number was entered manually)

## get_emvAID(BSTR\* pVal), property _emvAID_

**[OUT] pVal** – returns EMV AID, as string. Max length is 32 bytes. If not EMV card was used _emvAID_ will be empty.

## get_ExpDate(BSTR\* pVal), property _ExpDate_

**[OUT] pVal** – returns null-terminated string for card’s expiration date\*.\* Format of returned value is YYMM.

## get_CardHolder(BSTR\* pVal), property _CardHolder_

**[OUT] pVal** – returns null-terminated string for Authorisation code of transaction. . Length is variable. Gets Cardholder name.

## get_IssuerName(BSTR\* pVal), property _IssuerName_

**[OUT] pVal** – returns null-terminated string for issuer’s name of card. Length is variable.

## get_InvoiceNum(ULONG\* pVal), property _InvoiceNum_

**[OUT] pVal** – returns invoice number of transaction. This number is used to void the original transaction.

## get_CompletionInvoiceNum(ULONG\* pVal), property _CompletionInvoiceNum_

**[OUT] pVal** – returns invoice number of second transaction in two-pass scheme. It will be presented only after original purchase was complete (_Completion_ was executed). This number is used in case when transaction has been completed to void completion receipt.

## get_RRN(BSTR\* pVal), property _RRN_

**[OUT] pVal** – returns null-terminated string for RRN of transaction. Length is 6 bytes\*.\* RRN (Retrieval reference number) is unique for each transaction.

## get_SignVerif (BYTE\* pVal), property _SignVerif_

**[OUT] pVal** – returns:

1 – if signature verification is needed for current transaction. 0 – if it is not needed.

Signature verification – is one of the cardholder verification methods. If verification was failed, transaction should be voided.

## get_Track3(BSTR\* pVal), property _Track3_

**[OUT] pVal** – returns Track3 of card. Length is variable. Gets the specific magnetic stripe information of card in case transaction successfully completed.

## get_AddData(BSTR\* pVal), property _AddData_

**[OUT] pVal** – returns 63.89 field that contains host service data. Can return the specific information from financial packet after service operation performing.

## get_CryptedData(BSTR\* pVal), property _CryptedData_

**[OUT] pVal** – returns encrypted data.

Can return the specific information, which will be encrypted by POS.

## get_ExtraCardData(BSTR\* pVal), property _ExtraCardData_

**[OUT] pVal** – returns extra information about card in TLV format.

Can return the extra information about card (length up to 150 symbols), which will be in TLV format send by POS.

## get_TerminalInfo(BSTR\* pVal), property _TerminalInfo_

**[OUT] pVal** – returns POS information.

Gets the string of terminal software version + terminal profile ID + POS S/N + the list of acquirers after _POSGetInfo_ operation. For example: TE7E118

0000005100CT20221154/AQ1/AQ2/AQ3.

## get_DiscountName(BSTR\* pVal), property _DiscountName_

**[OUT] pVal** – returns information about the card. Gets the information about the card, if it is also presented in Discount cards table.

## get_DiscountAttribute(BSTR\* pVal), property _DiscountAttribute_

**[OUT] pVal** – returns information about the card. Gets the information about the card attribute, if it is also presented in Discount cards table.

## get_ECRDataTM(BSTR\* pVal), property _ECRDataTM_

**[OUT] pVal** – returns specific information from POS. Gets the specific string (length up to 50 symbols) from POS.

## get_TrnStatus(BYTE \* pVal), property _TrnStatus_

**[OUT] pVal** – Get transaction status

_TrnStatus_ has one of following values:

1. – undefined
2. – approved
3. – declined
4. – reversed
5. – canceled

## get_Currency(BSTR \* pVal), property _Currency_

**[OUT] pVal** – return currency of transaction.

## get_TrnBatchNum(ULONG\* pVal), property _TrnBatchNum_

**[OUT] pVal** – returns batch number of transaction.

## get_RNK(BSTR\* pVal), property _RNK_

**[OUT] pVal** – returns customer\`s RNK. Max size is 20 bytes. _Available only for IdentifyCard method._

## get_CurrencyCode(BSTR\* pVal), property _CurrencyCode_

**[OUT] pVal** – returns currency code. Max size is 3 bytes.

**Functions (properties) below are available only for financial transactions and ReadBankCard. Otherwise they will return empty results.**

## get_FlagAcquirer(ULONG\* pVal), property _FlagAcquirer_

**[OUT] pVal** – determine native card.

Gets the statement is this native card or no.

_FlagAcquirer_ has one of following values:

1. – false
2. – true

# Totals properties

**Functions (properties) below are available only for Settlement, PrintBatchTotals, GetBatchTotals. Otherwise they will return empty results.**

## get_TotalsDebitAmt(ULONG\* pVal), property _TotalsDebitAmt_

**[OUT] pVal** – total amount of debit transactions. Includes purchases. Gets the total amount of debit transactions within current batch.

## get_TotalsDebitNum(ULONG\* pVal), property _TotalsDebitNum_

**[OUT] pVal** – number of debit transactions. Includes purchases\*.\* Gets the total number of debit transactions within current batch.

## get_TotalsCreditAmt(ULONG\* pVal), property _TotalsCreditAmt_

**[OUT] pVal** – total amount of credit transactions. Includes refunds\*.\*

Gets the total amount of credit transactions within current batch.

## get_TotalsCreditNum(ULONG\* pVal), property _TotalsCreditNum_

**[OUT] pVal** – number of credit transactions. Includes refunds.

Gets the total number of credit transactions within current batch.

## get_TotalsCancelledAmt(ULONG\* pVal), property _TotalsCancelledAmt_

**[OUT] pVal** – total amount of cancelled transactions. Includes voids\*.\* Gets the total amount of cancelled transactions within current batch.

## get_TotalsCancelledNum(ULONG\* pVal), property _TotalsCancelledNum_

**[OUT] pVal** – number of cancelled transactions. Includes voids\*.\*

Gets the total number of cancelled transactions within current batch.

## get_TxnNum(ULONG\* pVal), property _TxnNum_

**[OUT] pVal** – number of transactions.

Gets the number of transactions stored in transaction journal of terminal _Available only for GetTxnNum method._

**Functions (properties) below are available only for StartScenario. Otherwise they will return empty results.**

## get_ScenarioData(BSTR\* pVal), property _ScenarioData_

**[OUT] pVal** – returns any information in xml about scenario. Gets the xml with information about scenario, transaction, etc.

# UNATTENDED SECTION

## get_Key(BYTE\* pVal), property _Key_

**[OUT] pVal** – returns pressed key

**! USED FOR UN-ATTENDED POS’es ONLY**

## get_TermStatus(BYTE\* pVal), property _TermStatus_

**[OUT] pVal** – returns status of POS Terminal _Terminal status_ has one of following values:

1. – POS. UNCKNOWN STATUS
2. – POS. WORK IN NORMAL MOD
3. – POS. SYSTEM OPERATION 14 – POS. CLOSE BATCH NEED 15 – POS. READERS FAILED.

    **! USED FOR UN-ATTENDED POS’es ONLY**

# CONTROL MODE FUNCTIONS

## EnterControlMode(void)

Enter into display and keyboard control mode from ECR.

Can’t be used during financial transaction

**! USED FOR UN-ATTENDED POS’es ONLY**

## ExitControlMode(void)

**! USED FOR UN-ATTENDED POS’es ONLY**

## SetControlMode(VARIANT_BOOL isCtrlMode)

Set Control Mode:

Enter into display and keyboard control mode from ECR(ON), or Exit from control mode (OFF).

Can’t be used during financial transaction

**[IN] isCtrlMode –** parameter for ON(1) or OFF(0) CONTROL Mode

**! USED FOR UN-ATTENDED POS’es ONLY**

## ReadKey(BYTE bTimeOut)

Returns code of pressed key. _(Used in Control mode only)_

**[IN] bTimeOut –** parameter for key waiting (in)

**! USED FOR UN-ATTENDED POS’es ONLY**

## DisplayText (BYTE bBeep)

Display text of prepared line by SetLine function. _(Used in Control mode only)_

**[IN] bBeep –** parameter for sound behavior on terminal (in ) **Time OUT to “ECR not connected” – 15 sec**

**! USED FOR UN-ATTENDED POS’es ONLY**

## SetLine(BYTE bRow, BYTE bCol, BSTR bsText, BYTE bInvert)

Set line Text (_Used in Control mode only)_

**[IN] bRow –** line display offset Y (number of line)

**[IN] bCol –** line display offset X (character offset)

**[IN] bInvert –** if 0, no inversion for string, else inverted **Time OUT to “ECR not connected” – 15 sec**

**! USED FOR UN-ATTENDED POS’es ONLY**

## SetScreen(ULONG ulScreenNumber)

Set screen number of special external display with reserved screens (_Used in Control mode only)_ **[IN] ulScreenNumber –** number of screen

**! USED FOR UN-ATTENDED POS’es ONLY**

0 – no Screen

## ExchangeStatuses(BYTE bECRStatus)

**[IN] bECRStatus –** status of ECR.

Send status of ECR and receive status of POS terminal. _(Used in Control mode only) ECR status_ has one of following values: 01 – ECR. NOT SUPPORTED

1. – ECR. WORK IN NORMAL MOD.
2. – ECR. CUSTOMER IN PROGRESS 04 – ECR. WORK IN MAINTANANEC MOD.

    05 – ECR. NOT CONNECT.

    **! USED FOR UN-ATTENDED POS’es ONLY**

# Appendix A. Interface in use

Below there is an example (for VBA):

#### PURCHASE (simplified example)

## PURCHASE (simplified example, VBA)

```vba
Dim Terminal As Object
Const BaudRate As Double = 115200
Const timeout As Double = 200
Const Amount As Double = 1000
Const AddAmount As Double = 0
Const MerchantIdx As Double = 1
```

```vba


****************************** Wait Response FUNCTION ******************************

Public Function SetCellValueInt(ByVal sCellName As String, ByVal sCellValue As Integer, ByVal i As Integer) As Boolean
    If (sCellValue = 0) Then
        Worksheets("Sheet1").Range(sCellName).Offset(0, i).Value = "empty"
    Else
        Worksheets("Sheet1").Range(sCellName).Offset(0, i).Value = sCellValue
    End If
End Function
```

```vba
Rem ************************************
Rem * Author, Desc: TestUnit *
Rem ************************************

Public Function SetCellValue(ByVal sCellName As String, ByVal sCellValue As String, ByVal i As Integer) As Boolean
    If (sCellValue = "") Then
        Worksheets("Sheet1").Range(sCellName).Offset(0, i).Value = "empty"
    Else
        Worksheets("Sheet1").Range(sCellName).Offset(0, i).Value = sCellValue
    End If
End Function


```

```vba
Public Function WaitResponse(ByRef obj)
    Dim Res As Boolean
    Dim PAN As String
    Dim EntryMode As String
    Dim LastStMsCode As Integer
    LastStMsCode = 0

    Do While obj.LastResult = 2
        DoEvents
        If Terminal.LastStatMsgCode <> 0 And Terminal.LastStatMsgCode <> LastStMsCode Then
            ' HAS POS status progress message, we can display it on the ECR
            ' POS can send multiple status messages as (CardRead, PIN Required, Host Auth)
            LastStMsCode = Terminal.LastStatMsgCode
        End If

        If LastStMsCode = 11 Then
            PAN = Terminal.PAN
            EntryMode = Terminal.EntryMode
            Terminal.CorrectTransaction(Amount, AddAmount)
        End If
    Loop
End Function

```

```vba
Sub Pause(Wait)
    Dim Current As Long
    Current = Timer
    Do Until Timer - Current >= Wait
        DoEvents
    Loop
End Sub

```

```vba
****************************** Return values FUNCTION ******************************

Sub ReturnValues(ByRef Terminal As Object)
    mErr = WaitResponse(Terminal)

    If Terminal.LastResult = 1 And Val(Terminal.ResponseCode) <> 20 Then
        ' Error occurred
        Status = "ERROR"
        LastResult = Terminal.LastErrorCode
        ErrorDescr = Terminal.LastErrorDescription

        If Terminal.LastErrorCode = 4 Then
            ' Transaction has been declined (host or terminal declined), we need response code
            Res = SetCellValueInt("B13", Terminal.ResponseCode, 0)
            Res = SetCellValue("B14", Terminal.TerminalID, 0)
            Res = SetCellValue("B15", Terminal.MerchantID, 0)
        End If
    Else
        Res = SetCellValue("B10", "Successful", 0)
    End If

    Res = SetCellValueInt("B13", Terminal.ResponseCode, 0)
    Res = SetCellValue("B14", Terminal.TerminalID, 0)
    Res = SetCellValue("B15", Terminal.MerchantID, 0)
    Res = SetCellValue("B16", Terminal.PAN, 0)
    Res = SetCellValue("B17", Terminal.RRN, 0)
    Res = SetCellValue("B18", Terminal.AuthCode, 0)
    Res = SetCellValue("B19", Terminal.DateTime, 0)
    Res = SetCellValueInt("B20", Terminal.InvoiceNum, 0)
    Res = SetCellValue("B21", Terminal.ExpDate, 0)
    Res = SetCellValue("B22", Terminal.CardHolder, 0)
    Res = SetCellValue("B23", Terminal.IssuerName, 0)
    Res = SetCellValueInt("B24", Terminal.Amount, 0)
    Res = SetCellValue("B25", Terminal.SignVerif, 0)
    Res = SetCellValue("B26", Terminal.TxnNum, 0)
    Res = SetCellValue("B27", Terminal.TxnType, 0)

    ' This part is required for Transaction totals only
    Res = SetCellValue("B37", Terminal.TotalsDebitNum, 0)
    Res = SetCellValue("B38", Terminal.TotalsDebitAmt, 0)
    Res = SetCellValue("B39", Terminal.TotalsCreditNum, 0)
    Res = SetCellValue("B40", Terminal.TotalsCreditAmt, 0)
    Res = SetCellValue("B41", Terminal.TotalsCancelledNum, 0)
    Res = SetCellValue("B42", Terminal.TotalsCancelledAmt, 0)
End Sub

```

```vba
************************ MAIN FUNCTION ************************

Sub ECRMAIN()
    Dim Res As Boolean

    If (Terminal Is Nothing) Then
        Rem Initialize library
        Set Terminal = CreateObject("ECRCommX.BPOS1Lib")
    End If

    Terminal.CommClose
    Terminal.SetErrorLang(1) '***** Set library messages to Ukrainian

    Res = Terminal.CommOpenAuto(BaudRate)
    Res = Terminal.Purchase(Amount, AddAmount, MerchantIdx)

    Call ReturnValues(Terminal)

    If Terminal.LastResult = 0 Then
        Terminal.Confirm
        ' Check if confirmation is received
        mErr = WaitResponse(Terminal)

        ' Get formatted receipt into one buffer
        If Terminal.LastResult = 0 Then
            mErr = Terminal.ReqCurrReceipt
            ' Check if ReqCurrReceipt is sent
            mErr = WaitResponse(Terminal)

            If Terminal.LastResult = 0 Then
                ReceiptSlip = Terminal.ReceiptSlip
            End If
        End If
    ElseIf Terminal.LastErrorCode = 4 Then
        mErr = Terminal.ReqCurrReceipt
        ' Check if ReqCurrReceipt is sent
        mErr = WaitResponse(Terminal)

        If Terminal.LastResult = 0 Then
            ReceiptSlip = Terminal.ReceiptSlip
        End If
    End If

    ' CLOSE Communication port
    Terminal.CommClose
    Set Terminal = Nothing
End Sub

```
