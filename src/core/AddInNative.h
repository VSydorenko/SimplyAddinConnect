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
#include <unordered_map>
#include <functional>
#include <type_traits>
#include <utility>

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
	struct PropDesc;
	struct MethDesc;
protected:
	class VariantHelper {
	private:
		tVariant* pvar = nullptr;
		AddInNative* addin = nullptr;
		// Контекст ЛИШЕ для тексту помилки: чиє це значення. Не володіє нічим.
		const PropDesc* prop = nullptr;
		const MethDesc* meth = nullptr;
		long number = -1;
	public:
		VariantHelper(const VariantHelper& va) :pvar(va.pvar), addin(va.addin), prop(va.prop), meth(va.meth), number(va.number) {}
		VariantHelper(tVariant* pvar, AddInNative* addin) :pvar(pvar), addin(addin) {}
		VariantHelper(tVariant* pvar, AddInNative* addin, const PropDesc* prop) :pvar(pvar), addin(addin), prop(prop) {}
		VariantHelper(tVariant* pvar, AddInNative* addin, const MethDesc* meth, long number) :pvar(pvar), addin(addin), meth(meth), number(number) {}
		VariantHelper& operator<<(const VariantHelper& va) { pvar = va.pvar; addin = va.addin; prop = va.prop; meth = va.meth; number = va.number; return *this; }
		// Рибіндинг result: копіювальне присвоєння лишається ЗАБОРОНЕНИМ — навмисний
		// guard Етапу 0 (без нього this->result = f(...) у WrapRet міг би мовчки
		// рибіндити result замість присвоїти значення). Рибіндинг — лише через operator<<.
		VariantHelper& operator=(const VariantHelper& va) = delete;

		// Ядро адаптера: уся робота з tVariant іде через ці два явні методи;
		// оператори нижче — тонкі inline-обгортки над ними. Спеціалізації визначені
		// у .cpp; прототипи — одразу після класу AddInNative, у просторі імен
		// (щоб їх бачила кожна TU, яка підключає цей заголовок, — компоненти теж).
		template <typename T> T    Get() const;
		template <typename T> void Set(const T& value);

		void     AllocMemory(unsigned long size);
		uint32_t size();
		TYPEVAR  type();
		char*    data();
		void     clear();

		// Тіла — тонкі обгортки над Set<T>()/Get<T>(), визначені в .cpp (НЕ inline
		// у тілі класу): виклик Get<T>()/Set<T>() з функції, визначеної ВСЕРЕДИНІ
		// класу, компілюється в "complete-class context" одразу після закриття
		// VariantHelper — тобто ДО того, як компілятор побачить explicit-спеціалізації,
		// оголошені за межами класу AddInNative (вони й фізично не можуть стояти
		// раніше — explicit-спеціалізація вкладеного шаблону методу мусить бути в
		// просторі імен, а AddInNative ще не закрився). Наслідок — компілятор мовчки
		// створює екземпляр primary-шаблону РАНІШЕ оголошення спеціалізації, і MSVC
		// падає з C2908 "явная специализация; уже создан экземпляр". Тому тут —
		// лише декларації.
		VariantHelper& operator=(const std::string& str);
		VariantHelper& operator=(const std::wstring& str);
		VariantHelper& operator=(const std::u16string& str);
		VariantHelper& operator=(int64_t value);
		VariantHelper& operator=(double value);
		VariantHelper& operator=(bool value);

		operator std::string()    const;
		operator std::wstring()   const;
		operator std::u16string() const;
		operator int64_t()        const;
		operator double()         const;
		operator bool()           const;
		operator int()            const;

	private:
		std::exception TypeError(TYPEVAR expected) const;
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
	// Арності 8..16 потрібні драйверам БПО: контракт «Подключаемое оборудование» має
	// методи на 9-10 параметрів (напр. ОплатитьПлатежнойКартой — 9, ОтменитьПлатеж… — 10),
	// які до цього не реєструвалися взагалі (GetNParams віддавав 0). Запас до 16 узятий
	// свідомо: розширення цього списку тягне перезбірку й гейт усіх компонент DLL.
	using MethFunction8  = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction9  = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction10 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction11 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction12 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction13 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction14 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction15 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;
	using MethFunction16 = std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>;

	using MethFunction = std::variant<
		MethFunction0,
		MethFunction1,
		MethFunction2,
		MethFunction3,
		MethFunction4,
		MethFunction5,
		MethFunction6,
		MethFunction7,
		MethFunction8,
		MethFunction9,
		MethFunction10,
		MethFunction11,
		MethFunction12,
		MethFunction13,
		MethFunction14,
		MethFunction15,
		MethFunction16
	>;

	// Порядок альтернатив — контракт GetNParams: він віддає в 1С кількість параметрів
	// методу як index() variant-а. Вставка альтернативи не в кінець (або не за арністю)
	// зламала б це МОВЧКИ — тому інваріант закріплено тут.
	static_assert(std::variant_size_v<MethFunction> == 17,
		"MethFunction: очікується 17 альтернатив (арності 0..16)");
	static_assert(std::is_same_v<std::variant_alternative_t<0, MethFunction>, MethFunction0>,
		"MethFunction: альтернатива 0 мусить бути MethFunction0");
	static_assert(std::is_same_v<std::variant_alternative_t<7, MethFunction>, MethFunction7>,
		"MethFunction: альтернатива 7 мусить бути MethFunction7");
	static_assert(std::is_same_v<std::variant_alternative_t<9, MethFunction>, MethFunction9>,
		"MethFunction: альтернатива 9 мусить бути MethFunction9");
	static_assert(std::is_same_v<std::variant_alternative_t<16, MethFunction>, MethFunction16>,
		"MethFunction: альтернатива 16 мусить бути MethFunction16");

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
	// Дескриптор властивості: дані + обидва імені. Індекс тримає не самі імена,
	// а нормалізовані ключі -> позицію в цьому векторі.
	struct PropDesc {
		std::u16string nameEn;
		std::u16string nameRu;
		PropFunction   getter;
		PropFunction   setter;   // порожній -> властивість лише для читання
	};

	// Дескриптор методу. hasRetVal розрізняє функцію і процедуру для 1С.
	struct MethDesc {
		std::u16string nameEn;
		std::u16string nameRu;
		MethFunction   handler;
		MethDefaults   defaults;
		std::vector<ParamSpec> params;
		bool           hasRetVal = false;
	};

	std::vector<PropDesc> props_;
	std::vector<MethDesc> meths_;

	// Індекс імен -> позиція в векторі. Ключ нормалізований (верхній регістр),
	// обидві мови кладуться окремими ключами. Дає O(1) пошук замість обходу
	// вкладених векторів.
	std::unordered_map<std::u16string, long> propIndex_;
	std::unordered_map<std::u16string, long> methIndex_;

	// Нормалізація імені для індексу: верхній регістр для латиниці й кирилиці.
	static std::u16string NormalizeName(std::u16string_view name);

	// Спільна точка реєстрації методів (AddProcedure/AddFunction обох перевантажень) —
	// щоб індекс наповнювався в одному місці.
	void RegisterMethod(const std::u16string& nameEn, const std::u16string& nameRu,
	                    const MethFunction& handler, const MethDefaults& defs,
	                    const std::vector<ParamSpec>& params, bool hasRetVal);

	bool CallMethod(MethFunction* function, tVariant* paParams, MethDesc* meth, const long lSizeArray);

	// Розгортає виклик хендлера довільної арності: індекси параметрів беруться з
	// index_sequence, тож списки VA(...) для арностей 0..16 не виписуються руками
	// (17 рукописних списків — надто ласий грунт для описки в індексі).
	template <typename Fn, size_t... I>
	void InvokeHandler(const Fn& handler, tVariant* paParams, MethDesc* meth, std::index_sequence<I...>)
	{
		handler(VA(paParams, meth, static_cast<long>(I))...);
	}

	// Одна гілка диспетчера CallMethod: якщо у variant лежить саме Fn — перевірити
	// кількість фактичних параметрів і викликати. false = «це не та альтернатива».
	template <size_t N, typename Fn>
	bool TryCallArity(MethFunction* function, tVariant* paParams, MethDesc* meth, const long lSizeArray)
	{
		auto handler = std::get_if<Fn>(function);
		if (!handler) return false;
		if (lSizeArray < static_cast<long>(N)) throw std::bad_function_call();
		InvokeHandler(*handler, paParams, meth, std::make_index_sequence<N>{});
		return true;
	}
	// Перевіряє required-параметри без дефолту перед викликом хендлера: за порожнім
	// чи відсутнім аргументом реєструє AddError з ім'ям параметра й повертає false.
	bool ValidateParams(MethDesc& m, tVariant* paParams, const long lSizeArray);
	// Будує MethDefaults зі spec-ів: параметри з byDefault стають дефолтами 1С.
	static MethDefaults DefaultsFromSpecs(const std::vector<ParamSpec>& params);
	VariantHelper VA(tVariant* pvar) { return VariantHelper(pvar, this); }
	VariantHelper VA(tVariant* pvar, PropDesc* prop) { return VariantHelper(pvar, this, prop); }
	VariantHelper VA(tVariant* pvar, MethDesc* meth, long number) { return VariantHelper(pvar + number, this, meth, number); }
	bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte) const noexcept;
	void ADDIN_API FreeMemory(void** pMemory) const noexcept;

	// Копія рядка в пам'яті менеджера 1С (примітив алокації). nullptr, якщо
	// менеджера ще немає або алокація провалилась. W() — єдиний інший споживач
	// цієї логіки — побудований поверх цього ж примітиву (кидає bad_alloc сам).
	WCHAR_T* AllocString(const std::u16string& src) const;

	friend const WCHAR_T* GetClassNames();
	friend long GetClassObject(const WCHAR_T*, IComponentBase**);

	// Реєстр компонент — функціо-локальний статик (Meyers singleton): будується
	// при першому виклику, тому файло-рівнева реєстрація (REGISTER_COMPONENT) не
	// залежить від порядку статичної ініціалізації між одиницями трансляції.
	static std::map<std::u16string, CompFunction>& components();
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

// Явні спеціалізації VariantHelper::Get<T>()/Set<T>() — визначення в AddInNative.cpp.
// Прототип тут ОБОВ'ЯЗКОВИЙ: inline-оператори у тілі класу (вище) викликають Get<T>()/
// Set<T>() у КОЖНІЙ одиниці трансляції, що підключає цей заголовок (компоненти в
// src/components теж). Без прототипу компілятор шукав би визначення primary-шаблону
// (якого немає — лише декларація в класі) і впав би лінк-помилкою, що для explicit-
// спеціалізацій вкладеного шаблону методу класична пастка: "не визначено" лише для
// типів, першими використаних без видимого прототипу.
template <> std::string    AddInNative::VariantHelper::Get<std::string>() const;
template <> std::wstring   AddInNative::VariantHelper::Get<std::wstring>() const;
template <> std::u16string AddInNative::VariantHelper::Get<std::u16string>() const;
template <> int64_t        AddInNative::VariantHelper::Get<int64_t>() const;
template <> double         AddInNative::VariantHelper::Get<double>() const;
template <> bool           AddInNative::VariantHelper::Get<bool>() const;

template <> void AddInNative::VariantHelper::Set<std::string>(const std::string& value);
template <> void AddInNative::VariantHelper::Set<std::wstring>(const std::wstring& value);
template <> void AddInNative::VariantHelper::Set<std::u16string>(const std::u16string& value);
template <> void AddInNative::VariantHelper::Set<int64_t>(const int64_t& value);
template <> void AddInNative::VariantHelper::Set<double>(const double& value);
template <> void AddInNative::VariantHelper::Set<bool>(const bool& value);

// Реєстрація компоненти в реєстрі DLL + захист від відкидання лінкером.
// Клас мусить оголосити: static std::vector<std::u16string> names;
// Використання (у .cpp компоненти, на файловому рівні):
//   REGISTER_COMPONENT(u"МояКомпонента", МійКлас)
#define REGISTER_COMPONENT(NAME_U16, CLASS) \
	std::vector<std::u16string> CLASS::names = { \
		AddInNative::AddComponent(NAME_U16, []() -> AddInNative* { return new CLASS; }) \
	}; \
	namespace { [[maybe_unused]] auto& _force_##CLASS##_names = CLASS::names; }
