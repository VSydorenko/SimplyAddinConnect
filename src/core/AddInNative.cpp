#include "pch.h"

#include "../version.h"
#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)

#ifdef _WINDOWS
#pragma warning (disable : 4267)
#else
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#endif

#include <locale>
#include <wchar.h>
#include <iterator>
#include <codecvt>
#include <cwctype>
#include <sstream>

#include "AddInNative.h"
#include "../helpers/ServiceTools.h"

#ifdef _WINDOWS

HMODULE hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE module, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		::DisableThreadLibraryCalls(module);
		::hModule = module;
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
#endif

const WCHAR_T* GetClassNames()
{
	static const std::u16string names(AddInNative::getComponentNames());
	return (const WCHAR_T*)names.c_str();
}

long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface)
{
	if (*pInterface) return 0;
	auto cls_name = std::u16string(reinterpret_cast<const char16_t*>(wsName));
	*pInterface = AddInNative::CreateObject(cls_name);
	// Контракт 1С: ненульове значення = успіх. Повертаємо 1 замість адреси,
	// бо приведення 64-бітного вказівника до long усікає його (UB на x64).
	return *pInterface ? 1 : 0;
}

long DestroyObject(IComponentBase** pInterface)
{
	if (!*pInterface) return -1;
	delete* pInterface;
	*pInterface = nullptr;
	return 0;
}

std::string WC2MB(const std::wstring& wstr)
{
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	return converter.to_bytes(wstr);
}

std::wstring MB2WC(const std::string& str)
{
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	return converter.from_bytes(str);
}

std::map<std::u16string, CompFunction>& AddInNative::components() {
	static std::map<std::u16string, CompFunction> registry;
	return registry;
}

AddInNative::AddInNative(void) : result(nullptr, this) {
	AddProperty(u"Version", u"Версия", [&](VH var) { var = this->version(); });
	// Общий для всех компонент включатель логирования (делегат в ServiceTools).
	AddFunction(u"EnableLogging", u"ИспользоватьЛогирование",
		Ret([this](VH logLevel, VH logFilePath) {
			try {
				std::string level = logLevel;
				std::string path = logFilePath;
				return ServiceTools::EnableComponentLogging(this, level, path);
			}
			catch (...) { return false; }
		}),
		// Явный тип: после появления перегрузки с vector<ParamSpec> (Task 6)
		// braced-list без типа может стать неоднозначным
		MethDefaults{ {0, DefaultHelper(u"info")}, {1, DefaultHelper(u"")} });
}

std::string AddInNative::version()
{
	return STRINGIZE(VERSION_FULL);
}

bool AddInNative::Init(void* pConnection)
{
	std::lock_guard<std::mutex> lock(connectMutex_);
	m_iConnect = static_cast<IAddInDefBase*>(pConnection);
	if (m_iConnect) m_iConnect->SetEventBufferDepth(100);
	return m_iConnect != nullptr;
}

bool AddInNative::setMemManager(void* memory)
{
	return m_iMemory = static_cast<IMemoryManager*>(memory);
}

long AddInNative::GetInfo()
{
	return 2000;
}

void AddInNative::Done()
{
	// Зв'язок з 1С далі недійсний: відсікаємо фонові PostExternalEvent/AddError
	std::lock_guard<std::mutex> lock(connectMutex_);
	m_iConnect = nullptr;
}

bool AddInNative::PostExternalEvent(const std::u16string& message, const std::u16string& data)
{
	// ExternalEvent приймає WCHAR_T* без const — віддаємо mutable-буфери
	// локальних копій (u16string::data() не-const з C++17); платформа копіює
	// їх синхронно всередині виклику
	std::u16string src = name, msg = message, dat = data;
	std::lock_guard<std::mutex> lock(connectMutex_);
	if (!m_iConnect) return false;
	return m_iConnect->ExternalEvent(
		reinterpret_cast<WCHAR_T*>(src.data()),
		reinterpret_cast<WCHAR_T*>(msg.data()),
		reinterpret_cast<WCHAR_T*>(dat.data()));
}

bool AddInNative::RegisterExtensionAs(WCHAR_T** wsLanguageExt)
{
	*wsLanguageExt = W(this->name.c_str());
	return *wsLanguageExt != nullptr;
}

long AddInNative::GetNProps()
{
	return properties.size();
}

long AddInNative::FindProp(const WCHAR_T* wsPropName)
{
	std::u16string name((char16_t*)wsPropName);
	for (auto it = properties.begin(); it != properties.end(); ++it) {
		for (auto n = it->names.begin(); n != it->names.end(); ++n) {
			if (n->compare(name) == 0) return long(it - properties.begin());
		}
	}
	name = upper(name);
	for (auto it = properties.begin(); it != properties.end(); ++it) {
		for (auto n = it->names.begin(); n != it->names.end(); ++n) {
			if (upper(*n).compare(name) == 0) return long(it - properties.begin());
		}
	}
	return -1;
}

const WCHAR_T* AddInNative::GetPropName(long lPropNum, long lPropAlias)
{
	if (lPropNum < 0 || static_cast<size_t>(lPropNum) >= properties.size()) return nullptr;
	try {
		auto it = std::next(properties.begin(), lPropNum);
		if (it == properties.end()) return nullptr;
		auto nm = std::next(it->names.begin(), lPropAlias);
		if (nm == it->names.end()) return nullptr;
		return W(nm->c_str());
	}
	catch (...) {
		return nullptr;
	}
}

bool AddInNative::GetPropVal(const long lPropNum, tVariant* pvarPropVal)
{
	if (lPropNum < 0 || static_cast<size_t>(lPropNum) >= properties.size()) return false;
	auto it = std::next(properties.begin(), lPropNum);
	if (it == properties.end()) return false;
	if (!it->getter) return false;
	try {
		it->getter(VA(pvarPropVal, &(*it)));
		return true;
	}
	catch (const std::u16string& msg) {
		AddError(msg);
		return false;
	}
	catch (...) {
		return false;
	}
}

bool AddInNative::SetPropVal(const long lPropNum, tVariant* pvarPropVal)
{
	if (lPropNum < 0 || static_cast<size_t>(lPropNum) >= properties.size()) return false;
	auto it = std::next(properties.begin(), lPropNum);
	if (it == properties.end()) return false;
	if (!it->setter) return false;
	try {
		it->setter(VA(pvarPropVal, &(*it)));
		return true;
	}
	catch (const std::u16string& msg) {
		AddError(msg);
		return false;
	}
	catch (...) {
		return false;
	}
}

bool AddInNative::IsPropReadable(const long lPropNum)
{
	if (lPropNum < 0 || static_cast<size_t>(lPropNum) >= properties.size()) return false;
	auto it = std::next(properties.begin(), lPropNum);
	if (it == properties.end()) return false;
	return (bool)it->getter;
}

bool AddInNative::IsPropWritable(const long lPropNum)
{
	if (lPropNum < 0 || static_cast<size_t>(lPropNum) >= properties.size()) return false;
	auto it = std::next(properties.begin(), lPropNum);
	if (it == properties.end()) return false;
	return (bool)it->setter;
}

long AddInNative::GetNMethods()
{
	return methods.size();
}

long AddInNative::FindMethod(const WCHAR_T* wsMethodName)
{
	std::u16string name((char16_t*)wsMethodName);
	for (auto it = methods.begin(); it != methods.end(); ++it) {
		for (auto n = it->names.begin(); n != it->names.end(); ++n) {
			if (n->compare(name) == 0) return long(it - methods.begin());
		}
	}
	name = upper(name);
	for (auto it = methods.begin(); it != methods.end(); ++it) {
		for (auto n = it->names.begin(); n != it->names.end(); ++n) {
			if (upper(*n).compare(name) == 0) return long(it - methods.begin());
		}
	}
	return -1;
}

const WCHAR_T* AddInNative::GetMethodName(const long lMethodNum, const long lMethodAlias)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return nullptr;
	try {
		auto it = std::next(methods.begin(), lMethodNum);
		if (it == methods.end()) return nullptr;
		auto nm = std::next(it->names.begin(), lMethodAlias);
		if (nm == it->names.end()) return nullptr;
		return W(nm->c_str());
	}
	catch (...) {
		return nullptr;
	}
}

long AddInNative::GetNParams(const long lMethodNum)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return 0;
	auto it = std::next(methods.begin(), lMethodNum);
	if (it == methods.end()) return 0;
	// Альтернативи MethFunction упорядковані за арністю (MethFunctionN на позиції N),
	// тож індекс variant-а і Є кількістю параметрів. Інваріант закріплено static_assert-ами.
	if (it->handler.valueless_by_exception()) return 0;
	return static_cast<long>(it->handler.index());
}

bool AddInNative::GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pvarParamDefValue)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return true;
	try {
		VA(pvarParamDefValue).clear();
		auto it = std::next(methods.begin(), lMethodNum);
		if (it == methods.end()) return true;
		auto p = it->defs.find(lParamNum);
		if (p == it->defs.end()) return true;
		auto var = &p->second.variant;
		if (auto value = std::get_if<std::u16string>(var)) {
			VA(pvarParamDefValue) = *value;
			return true;
		}
		if (auto value = std::get_if<int64_t>(var)) {
			VA(pvarParamDefValue) = *value;
			return true;
		}
		if (auto value = std::get_if<double>(var)) {
			VA(pvarParamDefValue) = *value;
			return true;
		}
		if (auto value = std::get_if<bool>(var)) {
			VA(pvarParamDefValue) = *value;
			return true;
		}
		return true;
	}
	catch (const std::u16string& msg) {
		AddError(msg);
		return false;
	}
	catch (...) {
		return false;
	}
}

bool AddInNative::HasRetVal(const long lMethodNum)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return false;
	try {
		auto it = std::next(methods.begin(), lMethodNum);
		if (it == methods.end()) return false;
		return it->hasRetVal;
	}
	catch (...) {
		return false;
	}
}

bool AddInNative::CallMethod(MethFunction* func, tVariant* p, Meth* m, const long lSizeArray)
{
	// Кожна гілка: «якщо у variant лежить саме ця арність — перевірити кількість
	// фактичних параметрів і викликати». Короткозамкнене || дає ту саму семантику,
	// що й колишній ланцюжок if-ів, але без рукописних списків VA(p, m, 0..N).
	return TryCallArity<0,  MethFunction0 >(func, p, m, lSizeArray)
	    || TryCallArity<1,  MethFunction1 >(func, p, m, lSizeArray)
	    || TryCallArity<2,  MethFunction2 >(func, p, m, lSizeArray)
	    || TryCallArity<3,  MethFunction3 >(func, p, m, lSizeArray)
	    || TryCallArity<4,  MethFunction4 >(func, p, m, lSizeArray)
	    || TryCallArity<5,  MethFunction5 >(func, p, m, lSizeArray)
	    || TryCallArity<6,  MethFunction6 >(func, p, m, lSizeArray)
	    || TryCallArity<7,  MethFunction7 >(func, p, m, lSizeArray)
	    || TryCallArity<8,  MethFunction8 >(func, p, m, lSizeArray)
	    || TryCallArity<9,  MethFunction9 >(func, p, m, lSizeArray)
	    || TryCallArity<10, MethFunction10>(func, p, m, lSizeArray)
	    || TryCallArity<11, MethFunction11>(func, p, m, lSizeArray)
	    || TryCallArity<12, MethFunction12>(func, p, m, lSizeArray)
	    || TryCallArity<13, MethFunction13>(func, p, m, lSizeArray)
	    || TryCallArity<14, MethFunction14>(func, p, m, lSizeArray)
	    || TryCallArity<15, MethFunction15>(func, p, m, lSizeArray)
	    || TryCallArity<16, MethFunction16>(func, p, m, lSizeArray);
}

bool AddInNative::CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return false;
	auto it = std::next(methods.begin(), lMethodNum);
	if (it == methods.end()) return false;
	if (!ValidateParams(*it, paParams, lSizeArray)) return false;
	try {
		result << VA(nullptr);
		return CallMethod(&it->handler, paParams, &(*it), lSizeArray);
	}
	catch (const std::u16string& msg) {
		AddError(msg);
		return false;
	}
	catch (...) {
		return false;
	}
}

bool AddInNative::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray)
{
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return false;
	auto it = std::next(methods.begin(), lMethodNum);
	if (it == methods.end()) return false;
	if (!ValidateParams(*it, paParams, lSizeArray)) return false;
	try {
		result << VA(pvarRetValue);
		bool ok = CallMethod(&it->handler, paParams, &(*it), lSizeArray);
		result << VA(nullptr);
		return ok;
	}
	catch (const std::u16string& msg) {
		AddError(msg);
		return false;
	}
	catch (...) {
		result << VA(nullptr);
		return false;
	}
}

void AddInNative::SetLocale(const WCHAR_T* locale)
{
	std::string loc = WCHAR2MB(locale);
	this->alias = loc.substr(0, 3) == "rus";
}

std::u16string AddInNative::getComponentNames() {
	const char16_t* const delim = u"|";
	std::vector<std::u16string> names;
	for (auto it = components().begin(); it != components().end(); ++it) names.push_back(it->first);
	std::basic_ostringstream<char16_t, std::char_traits<char16_t>, std::allocator<char16_t>> imploded;
	std::copy(names.begin(), names.end(), std::ostream_iterator<std::u16string, char16_t, std::char_traits<char16_t>>(imploded, delim));
	std::u16string result = imploded.str();
	result.pop_back();
	return result;
}

std::u16string AddInNative::AddComponent(const std::u16string& name, CompFunction creator)
{
	components().insert({ name, creator });
	return name;
}

AddInNative* AddInNative::CreateObject(const std::u16string& name) {
	auto it = components().find(name);
	if (it == components().end()) return nullptr;
	AddInNative* object = it->second();
	object->name = name;
	return object;
}

void AddInNative::AddProperty(const std::u16string& nameEn, const std::u16string& nameRu, const PropFunction& getter, const PropFunction& setter)
{
	properties.push_back({ { nameEn, nameRu }, getter, setter });
}

void AddInNative::AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu, const MethFunction& handler, const MethDefaults& defs)
{
	methods.push_back({ { nameEn, nameRu }, handler, defs, false });
}

void AddInNative::AddFunction(const std::u16string& nameEn, const std::u16string& nameRu, const MethFunction& handler, const MethDefaults& defs)
{
	methods.push_back({ { nameEn, nameRu }, handler, defs, true });
}

// Будує MethDefaults зі spec-ів: параметри, що мають byDefault, стають дефолтами 1С.
AddInNative::MethDefaults AddInNative::DefaultsFromSpecs(const std::vector<ParamSpec>& params)
{
	MethDefaults defs;
	for (long i = 0; i < (long)params.size(); ++i)
		if (params[i].byDefault) defs.emplace(i, *params[i].byDefault);
	return defs;
}

void AddInNative::AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu,
                               const MethFunction& handler, const std::vector<ParamSpec>& params)
{
	methods.push_back({ { nameEn, nameRu }, handler, DefaultsFromSpecs(params), false, params });
}

void AddInNative::AddFunction(const std::u16string& nameEn, const std::u16string& nameRu,
                              const MethFunction& handler, const std::vector<ParamSpec>& params)
{
	methods.push_back({ { nameEn, nameRu }, handler, DefaultsFromSpecs(params), true, params });
}

bool AddInNative::ValidateParams(Meth& m, tVariant* paParams, const long lSizeArray)
{
	for (size_t i = 0; i < m.params.size(); ++i) {
		const ParamSpec& spec = m.params[i];
		if (!spec.required || spec.byDefault) continue;
		const bool missing = (long)i >= lSizeArray
			|| paParams == nullptr
			|| paParams[i].vt == VTYPE_EMPTY;
		if (missing) {
			const std::u16string& pname = alias ? spec.nameRu : spec.nameEn;
			const std::u16string& mname = alias ? m.names[1] : m.names[0];
			AddError(u"Параметр '" + pname + u"' методу '" + mname +
			         u"' обов'язковий, отримано порожнє значення");
			return false;
		}
	}
	return true;
}

bool ADDIN_API AddInNative::AllocMemory(void** pMemory, unsigned long ulCountByte) const noexcept
{
	return m_iMemory ? m_iMemory->AllocMemory(pMemory, ulCountByte) : false;
}

void ADDIN_API AddInNative::FreeMemory(void** pMemory) const noexcept
{
	if (m_iMemory) m_iMemory->FreeMemory(pMemory);
}

std::string AddInNative::WCHAR2MB(std::basic_string_view<WCHAR_T> src)
{
#ifdef _WINDOWS
	static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt_utf8_utf16;
	return cvt_utf8_utf16.to_bytes(src.data(), src.data() + src.size());
#else
	static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> cvt_utf8_utf16;
	return cvt_utf8_utf16.to_bytes(reinterpret_cast<const char16_t*>(src.data()),
		reinterpret_cast<const char16_t*>(src.data() + src.size()));
#endif//_WINDOWS
}

std::wstring AddInNative::WCHAR2WC(std::basic_string_view<WCHAR_T> src) {
#ifdef _WINDOWS
	return std::wstring(src);
#else
	std::wstring_convert<std::codecvt_utf16<wchar_t, 0x10ffff, std::little_endian>> conv;
	return conv.from_bytes(reinterpret_cast<const char*>(src.data()),
		reinterpret_cast<const char*>(src.data() + src.size()));
#endif//_WINDOWS
}

std::u16string AddInNative::MB2WCHAR(std::string_view src) {
#ifdef _WINDOWS
	static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt_utf8_utf16;
	std::wstring tmp = cvt_utf8_utf16.from_bytes(src.data(), src.data() + src.size());
	return std::u16string(reinterpret_cast<const char16_t*>(tmp.data()), tmp.size());
#else
	static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> cvt_utf8_utf16;
	return cvt_utf8_utf16.from_bytes(src.data(), src.data() + src.size());
#endif//_WINDOWS
}

// Локаль для регістронезалежного пошуку імен. НЕ глобальний об'єкт:
// std::locale("ru_RU.UTF-8") може кинути виняток, а на етапі статичної
// ініціалізації DLL це означає відмову завантаження компоненти в 1С.
static const std::locale& RuLocale()
{
	static const std::locale loc = []() -> std::locale {
		try { return std::locale("ru_RU.UTF-8"); }
		catch (...) {
			try { return std::locale("Russian_Russia.1251"); }
			catch (...) { return std::locale::classic(); }
		}
	}();
	return loc;
}

std::u16string AddInNative::upper(std::u16string& str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](wchar_t ch) { return std::toupper(ch, RuLocale()); });
	return str;
}

std::wstring AddInNative::upper(std::wstring& str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](wchar_t ch) { return std::toupper(ch, RuLocale()); });
	return str;
}

TYPEVAR AddInNative::VariantHelper::type()
{
	if (pvar == nullptr) throw std::bad_variant_access();
	return pvar->vt;
}

uint32_t AddInNative::VariantHelper::size()
{
	if (pvar == nullptr) throw std::bad_variant_access();
	if (pvar->vt != VTYPE_BLOB) throw this->error(VTYPE_BLOB);
	return pvar->strLen;
}

char* AddInNative::VariantHelper::data()
{
	if (pvar == nullptr) throw std::bad_variant_access();
	if (pvar->vt != VTYPE_BLOB) throw this->error(VTYPE_BLOB);
	return pvar->pstrVal;
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(const std::string& str)
{
	return operator=(AddInNative::MB2WCHAR(str));
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(const std::wstring& str)
{
	if (sizeof(wchar_t) == 2) {
		return operator=(std::u16string(reinterpret_cast<const char16_t*>(str.data()), str.size()));
	}
	else {
		return operator=(WC2MB(str));
	}
}

void AddInNative::VariantHelper::clear()
{
	if (pvar == nullptr) throw std::bad_variant_access();
	switch (TV_VT(pvar)) {
	case VTYPE_BLOB:
	case VTYPE_PWSTR:
		addin->FreeMemory(reinterpret_cast<void**>(&TV_WSTR(pvar)));
		break;
	}
	tVarInit(pvar);
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(int64_t value)
{
	// Присвоєння у відʼєднаний result (CallAsProc навмисно обнуляє pvar, коли
	// функцію викликано як процедуру — результат не потрібен) — тихе відкидання,
	// а не bad_variant_access через clear() на nullptr
	if (pvar == nullptr) return *this;
	clear();
	if (INT32_MIN <= value && value <= INT32_MAX) {
		TV_VT(pvar) = VTYPE_I4;
		TV_I4(pvar) = (int32_t)value;
	}
	else {
		TV_VT(pvar) = VTYPE_R8;
		TV_R8(pvar) = (double)value;
	}
	return *this;
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(double value)
{
	// Відʼєднаний result (CallAsProc обнуляє pvar) — тихо відкидаємо присвоєння
	if (pvar == nullptr) return *this;
	clear();
	TV_VT(pvar) = VTYPE_R8;
	TV_R8(pvar) = value;
	return *this;
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(bool value)
{
	// Відʼєднаний result (CallAsProc обнуляє pvar) — тихо відкидаємо присвоєння
	if (pvar == nullptr) return *this;
	clear();
	TV_VT(pvar) = VTYPE_BOOL;
	TV_BOOL(pvar) = value;
	return *this;
}

AddInNative::VariantHelper& AddInNative::VariantHelper::operator=(const std::u16string& str)
{
	// Відʼєднаний result (CallAsProc обнуляє pvar) — тихо відкидаємо присвоєння
	if (pvar == nullptr) return *this;
	clear();
	TV_VT(pvar) = VTYPE_PWSTR;
	pvar->pwstrVal = nullptr;
	size_t size = (str.size() + 1) * sizeof(char16_t);
	if (!addin->AllocMemory(reinterpret_cast<void**>(&pvar->pwstrVal), size)) throw std::bad_alloc();
	memcpy(pvar->pwstrVal, str.c_str(), size);
	pvar->wstrLen = str.size();
	while (pvar->wstrLen && pvar->pwstrVal[pvar->wstrLen - 1] == 0) pvar->wstrLen--;
	return *this;
}

bool AddInNative::AddError(const std::u16string& descr, long scode)
{
	std::u16string info = u"AddIn." + name;
	// Синхронізація з Done()/фоновими потоками: читання m_iConnect під тим самим
	// м'ютексом (жоден шлях не викликає AddError, тримаючи connectMutex_)
	std::lock_guard<std::mutex> lock(connectMutex_);
	return m_iConnect && m_iConnect->AddError(ADDIN_E_IMPORTANT, (WCHAR_T*)info.c_str(), (WCHAR_T*)descr.c_str(), scode);
}

static std::u16string typeinfo(TYPEVAR vt, bool alias)
{
	switch (vt) {
	case VTYPE_EMPTY:
		return alias ? u"Неопределено" : u"Undefined";
	case VTYPE_I2:
	case VTYPE_I4:
	case VTYPE_ERROR:
	case VTYPE_UI1:
		return alias ? u"Целое число" : u"Integer";
	case VTYPE_BOOL:
		return alias ? u"Булево" : u"Boolean";
	case VTYPE_R4:
	case VTYPE_R8:
		return alias ? u"Число" : u"Float";
	case VTYPE_DATE:
	case VTYPE_TM:
		return alias ? u"Дата" : u"Date";
	case VTYPE_PSTR:
	case VTYPE_PWSTR:
		return alias ? u"Строка" : u"String";
	case VTYPE_BLOB:
		return alias ? u"Двоичные данные" : u"Binary";
	default:
		return alias ? u"Неопределено" : u"Undefined";
	}
}

std::exception AddInNative::VariantHelper::error(TYPEVAR vt) const
{
	std::basic_stringstream<char16_t, std::char_traits<char16_t>, std::allocator<char16_t>> ss;
	if (addin && addin->alias) {
		ss << u"Ошибка получения значения";
		if (prop) ss << u" при обращении к свойству <" << prop->names[1] << ">";
		if (meth) ss << u" при вызове метода <" << meth->names[1] << ">";
		if (number >= 0) ss << u" параметр <" << number + 1 << ">";
		ss << u" ожидается <" + typeinfo(vt, true) << u">";
		if (pvar) ss << u" фактически <" + typeinfo(pvar->vt, true) << u">";
	}
	else {
		ss << u"Error getting value";
		if (prop) ss << u" of property <" << prop->names[0] << ">";
		if (meth) ss << u" when calling method <" << meth->names[0] << ">";
		if (number >= 0) ss << u" parameter <" << number + 1 << ">";
		ss << u" expected <" + typeinfo(vt, false) << u">";
		if (pvar) ss << u" actual value <" + typeinfo(pvar->vt, false) << u">";
	}
	if (addin) addin->AddError(ss.str());
	return std::bad_typeid();
}

AddInNative::VariantHelper::operator std::string() const
{
	std::u16string str(*this);
	return WCHAR2MB((WCHAR_T*)str.c_str());
}

AddInNative::VariantHelper::operator std::wstring() const
{
	std::u16string str(*this);
	return WCHAR2WC((WCHAR_T*)str.c_str());
}

AddInNative::VariantHelper::operator std::u16string() const
{
	if (pvar == nullptr) throw std::bad_variant_access();
	if (pvar->vt != VTYPE_PWSTR) throw error(VTYPE_PWSTR);
	return reinterpret_cast<char16_t*>(pvar->pwstrVal);
}

AddInNative::VariantHelper::operator int64_t() const
{
	if (pvar == nullptr) throw std::bad_variant_access();
	switch (TV_VT(pvar)) {
	case VTYPE_I2:
	case VTYPE_I4:
	case VTYPE_UI1:
	case VTYPE_ERROR:
		return (int64_t)pvar->lVal;
	case VTYPE_R4:
	case VTYPE_R8:
		return (int64_t)pvar->dblVal;
	default:
		throw error(VTYPE_I4);
	}
}

AddInNative::VariantHelper::operator int() const
{
	if (pvar == nullptr) throw std::bad_variant_access();
	switch (TV_VT(pvar)) {
	case VTYPE_I2:
	case VTYPE_I4:
	case VTYPE_UI1:
	case VTYPE_ERROR:
		return (int)pvar->lVal;
	case VTYPE_R4:
	case VTYPE_R8:
		return (int)pvar->dblVal;
	default:
		throw error(VTYPE_I4);
	}
}

AddInNative::VariantHelper::operator double() const
{
	if (pvar == nullptr) throw std::bad_variant_access();
	switch (TV_VT(pvar)) {
	case VTYPE_I2:
	case VTYPE_I4:
	case VTYPE_UI1:
	case VTYPE_ERROR:
		return (double)pvar->lVal;
	case VTYPE_R4:
	case VTYPE_R8:
		return (double)pvar->dblVal;
	default:
		throw error(VTYPE_R4);
	}
}

AddInNative::VariantHelper::operator bool() const
{
	if (pvar == nullptr) throw std::bad_variant_access();
	switch (TV_VT(pvar)) {
	case VTYPE_BOOL:
		return TV_BOOL(pvar);
	case VTYPE_I2:
	case VTYPE_I4:
	case VTYPE_UI1:
	case VTYPE_ERROR:
		return (bool)pvar->lVal;
	default:
		throw error(VTYPE_BOOL);
	}
}

void AddInNative::VariantHelper::AllocMemory(unsigned long size)
{
	clear();
	if (!addin->AllocMemory((void**)&pvar->pstrVal, size)) throw std::bad_alloc();
	TV_VT(pvar) = VTYPE_BLOB;
	pvar->strLen = size;
}

WCHAR_T* AddInNative::W(const char16_t* str) const
{
	WCHAR_T* res = NULL;
	size_t length = std::char_traits<char16_t>::length(str) + 1;
	unsigned long size = length * sizeof(WCHAR_T);
	if (!AllocMemory((void**)&res, size)) throw std::bad_alloc();
	memcpy(res, str, size);
	return res;
}

