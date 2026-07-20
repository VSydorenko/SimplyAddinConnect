#pragma once

#ifdef _WINDOWS
#include <wtypes.h>
#endif //_WINDOWS

#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <string_view>
#include <functional>
#include <type_traits>

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

class AddInNative;

class DefaultHelper {
private:
	class EmptyValue {};
public:
	std::variant<
		EmptyValue,
		std::u16string,
		int64_t,
		double,
		bool
	> variant;
public:
	DefaultHelper() : variant(EmptyValue()) {}
	DefaultHelper(const std::u16string& s) : variant(s) {}
	DefaultHelper(int64_t value) : variant(value) {}
	DefaultHelper(double value) : variant(value) {}
	DefaultHelper(bool value) : variant(value) {}
	DefaultHelper(const char16_t* value) {
		if (value) variant = std::u16string(value);
		else variant = EmptyValue();
	}
};

using CompFunction = std::function<AddInNative* ()>;

class AddInNative : public IComponentBase
{
private:
	struct Prop;
	struct Meth;
protected:
	class VariantHelper {
	private:
		tVariant* pvar = nullptr;
		AddInNative* addin = nullptr;
		Prop* prop = nullptr;
		Meth* meth = nullptr;
		long number = -1;
	private:
		std::exception error(TYPEVAR vt) const;
	public:
		void AllocMemory(unsigned long size);
		VariantHelper(const VariantHelper& va) :pvar(va.pvar), addin(va.addin), prop(va.prop), meth(va.meth), number(va.number) {}
		VariantHelper(tVariant* pvar, AddInNative* addin) :pvar(pvar), addin(addin) {}
		VariantHelper(tVariant* pvar, AddInNative* addin, Prop* prop) :pvar(pvar), addin(addin), prop(prop) {}
		VariantHelper(tVariant* pvar, AddInNative* addin, Meth* meth, long number) :pvar(pvar), addin(addin), meth(meth), number(number) {}
		VariantHelper& operator<<(const VariantHelper& va) { pvar = va.pvar; addin = va.addin; prop = va.prop; meth = va.meth; number = va.number; return *this; }
		VariantHelper& operator=(const VariantHelper& va) = delete;
		VariantHelper& operator=(const std::string& str);
		VariantHelper& operator=(const std::wstring& str);
		VariantHelper& operator=(const std::u16string& str);
		VariantHelper& operator=(int64_t value);
		VariantHelper& operator=(double value);
		VariantHelper& operator=(bool value);
		operator std::string() const;
		operator std::wstring() const;
		operator std::u16string() const;
		operator int64_t() const;
		operator double() const;
		operator bool() const;
		operator int() const;
		uint32_t size();
		TYPEVAR type();
		char* data();
		void clear();
	};

	using VH = VariantHelper;
	using MethDefaults = std::map<long, DefaultHelper>;

	// Декларативний опис параметра методу: імена (en/ru), ознака обов'язковості
	// й опційне значення за замовчуванням. За наявності byDefault воно потрапляє
	// в MethDefaults методу; required без byDefault перевіряється в ValidateParams
	// перед викликом хендлера (порожній аргумент → AddError + return false).
	struct ParamSpec {
		std::u16string nameEn;
		std::u16string nameRu;
		bool required = false;
		std::optional<DefaultHelper> byDefault{};
	};

	using PropFunction = std::function<void(VH)>;
	using MethFunction0 = std::function<void()>;
	using MethFunction1 = std::function<void(VH)>;
	using MethFunction2 = std::function<void(VH, VH)>;
	using MethFunction3 = std::function<void(VH, VH, VH)>;
	using MethFunction4 = std::function<void(VH, VH, VH, VH)>;
	using MethFunction5 = std::function<void(VH, VH, VH, VH, VH)>;
	using MethFunction6 = std::function<void(VH, VH, VH, VH, VH, VH)>;
	using MethFunction7 = std::function<void(VH, VH, VH, VH, VH, VH, VH)>;

	using MethFunction = std::variant<
		MethFunction0,
		MethFunction1,
		MethFunction2,
		MethFunction3,
		MethFunction4,
		MethFunction5,
		MethFunction6,
		MethFunction7
	>;

	void AddProperty(const std::u16string& nameEn, const std::u16string& nameRu, const PropFunction &getter, const PropFunction &setter = nullptr);
	void AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu, const MethFunction &handler, const MethDefaults &defs = {});
	void AddFunction(const std::u16string& nameEn, const std::u16string& nameRu, const MethFunction &handler, const MethDefaults &defs = {});

	// Перевантаження з декларативним описом параметрів: дефолти беруться зі spec-ів,
	// а required-параметри валідуються перед викликом хендлера (див. ValidateParams).
	void AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu,
	                  const MethFunction& handler, const std::vector<ParamSpec>& params);
	void AddFunction(const std::u16string& nameEn, const std::u16string& nameRu,
	                 const MethFunction& handler, const std::vector<ParamSpec>& params);

	// Обгортає value-повертаючу лямбду у void-хендлер, який присвоює this->result.
	// Потрібно, бо MethFunction — це std::function<void(...)>: значення, повернуте
	// лямбдою напряму, мовчки відкидається і НЕ потрапляє в 1С.
	template <typename F>
	MethFunction Ret(F f) { return WrapRet(std::function(std::move(f))); }

private:
	template <typename R, typename... A>
	MethFunction WrapRet(std::function<R(A...)> f)
	{
		static_assert(!std::is_void_v<R>,
			"Ret(): лямбда мусить повертати значення; для void використовуйте AddProcedure");
		return MethFunction(std::function<void(A...)>(
			[this, f = std::move(f)](A... a) {
				if constexpr (std::is_same_v<R, bool>)
					this->result = f(a...);
				else if constexpr (std::is_integral_v<R>)
					this->result = static_cast<int64_t>(f(a...));
				else if constexpr (std::is_floating_point_v<R>)
					this->result = static_cast<double>(f(a...));
				else
					this->result = f(a...);
			}));
	}
protected:
public:
	static std::u16string AddComponent(const std::u16string& name, CompFunction creator);
	// Фабрика компонент за ім'ям. Публічна — потрібна L1-харнесу core_selftest
	// (створює пробні компоненти без платформи 1С); у DLL її кличе GetClassObject.
	static AddInNative* CreateObject(const std::u16string& name);
	VariantHelper result;
	static std::u16string getComponentNames();
	static std::u16string upper(std::u16string& str);
	static std::wstring upper(std::wstring& str);
	static std::string WCHAR2MB(std::basic_string_view<WCHAR_T> src);
	static std::wstring WCHAR2WC(std::basic_string_view<WCHAR_T> src);
	static std::u16string MB2WCHAR(std::string_view src);
	WCHAR_T* W(const char16_t* str) const;
	static std::string version();

private:
	struct Prop {
		std::vector<std::u16string> names;
		PropFunction getter;
		PropFunction setter;
	};

	struct Meth {
		std::vector<std::u16string> names;
		MethFunction handler;
		MethDefaults defs;
		bool hasRetVal;
		std::vector<ParamSpec> params;
	};

	bool CallMethod(MethFunction* function, tVariant* paParams, Meth* meth, const long lSizeArray);
	// Перевіряє required-параметри без дефолту перед викликом хендлера: за порожнім
	// чи відсутнім аргументом реєструє AddError з ім'ям параметра й повертає false.
	bool ValidateParams(Meth& m, tVariant* paParams, const long lSizeArray);
	// Будує MethDefaults зі spec-ів: параметри з byDefault стають дефолтами 1С.
	static MethDefaults DefaultsFromSpecs(const std::vector<ParamSpec>& params);
	VariantHelper VA(tVariant* pvar) { return VariantHelper(pvar, this); }
	VariantHelper VA(tVariant* pvar, Prop* prop) { return VariantHelper(pvar, this, prop); }
	VariantHelper VA(tVariant* pvar, Meth* meth, long number) { return VariantHelper(pvar + number, this, meth, number); }
	bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte) const noexcept;
	void ADDIN_API FreeMemory(void** pMemory) const noexcept;

	friend const WCHAR_T* GetClassNames();
	friend long GetClassObject(const WCHAR_T*, IComponentBase**);

	// Реєстр компонент — функціо-локальний статик (Meyers singleton): будується
	// при першому виклику, тому файло-рівнева реєстрація (REGISTER_COMPONENT) не
	// залежить від порядку статичної ініціалізації між одиницями трансляції.
	static std::map<std::u16string, CompFunction>& components();
	std::vector<Prop> properties;
	std::vector<Meth> methods;
	std::u16string name;
	bool alias = false;

public:
	AddInNative(void) ;
	virtual ~AddInNative() {}
	
	// Метод для добавления ошибок компонента
	// Перенесено из private в public
	bool AddError(const std::u16string& descr, long scode = 0);

	// Потокобезпечний міст подій у 1С: викликається з БУДЬ-ЯКОГО потоку
	// (фонові reader-потоки транспортів). source = ім'я компоненти (this->name).
	// Повертає false, якщо зв'язку з 1С немає (до Init або після Done).
	bool PostExternalEvent(const std::u16string& message, const std::u16string& data);

	// IInitDoneBase
	virtual bool ADDIN_API Init(void*) override final;
	virtual bool ADDIN_API setMemManager(void* mem) override final;
	virtual long ADDIN_API GetInfo() override final;
	virtual void ADDIN_API Done() override final;

	// ILanguageExtenderBase
	virtual bool ADDIN_API RegisterExtensionAs(WCHAR_T** wsLanguageExt) override final;
	virtual long ADDIN_API GetNProps() override final;
	virtual long ADDIN_API FindProp(const WCHAR_T* wsPropName) override final;
	virtual const WCHAR_T* ADDIN_API GetPropName(long lPropNum, long lPropAlias) override final;
	virtual bool ADDIN_API GetPropVal(const long lPropNum, tVariant* pvarPropVal) override final;
	virtual bool ADDIN_API SetPropVal(const long lPropNum, tVariant* pvarPropVal) override final;
	virtual bool ADDIN_API IsPropReadable(const long lPropNum) override final;
	virtual bool ADDIN_API IsPropWritable(const long lPropNum) override final;
	virtual long ADDIN_API GetNMethods() override final;
	virtual long ADDIN_API FindMethod(const WCHAR_T* wsMethodName) override final;
	virtual const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum, const long lMethodAlias) override final;
	virtual long ADDIN_API GetNParams(const long lMethodNum) override final;
	virtual bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pvarParamDefValue) override final;
	virtual bool ADDIN_API HasRetVal(const long lMethodNum) override final;
	virtual bool ADDIN_API CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray) override final;
	virtual bool ADDIN_API CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray) override final;
	operator IComponentBase* () { return (IComponentBase*)this; };
	
	// LocaleBase
	virtual void ADDIN_API SetLocale(const WCHAR_T* loc) override final;

private:
	IMemoryManager* m_iMemory = nullptr;
	IAddInDefBase* m_iConnect = nullptr;
	// Захищає m_iConnect від гонки між фоновими PostExternalEvent/AddError і Done().
	std::mutex connectMutex_;
};

// Реєстрація компоненти в реєстрі DLL + захист від відкидання лінкером.
// Клас мусить оголосити: static std::vector<std::u16string> names;
// Використання (у .cpp компоненти, на файловому рівні):
//   REGISTER_COMPONENT(u"МояКомпонента", МійКлас)
#define REGISTER_COMPONENT(NAME_U16, CLASS) \
	std::vector<std::u16string> CLASS::names = { \
		AddInNative::AddComponent(NAME_U16, []() -> AddInNative* { return new CLASS; }) \
	}; \
	namespace { [[maybe_unused]] auto& _force_##CLASS##_names = CLASS::names; }
