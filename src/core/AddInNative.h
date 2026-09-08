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

// Значення параметра за замовчуванням, яке 1С запитує через GetParamDefValue.
// Тримає рівно один із чотирьох типів, що їх уміє віддати платформа, або нічого.
class DefaultHelper {
public:
	// Конструктор під кожен підтримуваний тип — компонента пише DefaultHelper(u"info"),
	// DefaultHelper(true) тощо, і потрібна альтернатива обирається за перевантаженням.
	DefaultHelper()                        : slot_(Unset{})  {}
	DefaultHelper(const std::u16string& v) : slot_(v)        {}
	DefaultHelper(int64_t v)               : slot_(v)        {}
	DefaultHelper(double v)                : slot_(v)        {}
	DefaultHelper(bool v)                  : slot_(v)        {}
	// Літерал u"..." — найчастіший запис у компонентах; nullptr означає «дефолту немає».
	DefaultHelper(const char16_t* v)       : slot_(Unset{})
	{
		if (v) slot_ = std::u16string(v);
	}

	// Записує збережений дефолт у комірку 1С через адаптер VariantHelper.
	// «Немає дефолту» не пише нічого: викликач (GetParamDefValue) уже привів
	// комірку до VTYPE_EMPTY, і саме так платформа читає «параметр не задано».
	//
	// Параметри навмисно сирі (tVariant*, AddInNative*), а не VariantHelper: цей клас
	// оголошений ПЕРЕД AddInNative (той тримає DefaultHelper усередині MethDefaults і
	// ParamSpec), тож у цій точці AddInNative лише forward-declared і назвати його
	// вкладений тип неможливо. Означення — у .cpp, куди дружба відкриває доступ.
	void Apply(tVariant* pvar, AddInNative* addin) const;

private:
	// Тег «дефолту немає». Окремий тип, а не std::monostate, щоб альтернатива
	// читалась за іменем у повідомленнях компілятора.
	struct Unset {};

	using Slot = std::variant<Unset, std::u16string, int64_t, double, bool>;

	// Порядок альтернатив — контракт Apply(): switch там іде по slot_.index(), і
	// кожен індекс жорстко прив'язаний до типу. Перестановка зламала б Apply МОВЧКИ,
	// тому інваріант закріплено тут — тим самим прийомом, що й для арностей нижче.
	static_assert(std::variant_size_v<Slot> == 5,
		"DefaultHelper::Slot: очікується 5 альтернатив (Unset/u16string/int64_t/double/bool)");
	static_assert(std::is_same_v<std::variant_alternative_t<1, Slot>, std::u16string>,
		"DefaultHelper::Slot: альтернатива 1 мусить бути std::u16string");
	static_assert(std::is_same_v<std::variant_alternative_t<2, Slot>, int64_t>,
		"DefaultHelper::Slot: альтернатива 2 мусить бути int64_t");
	static_assert(std::is_same_v<std::variant_alternative_t<3, Slot>, double>,
		"DefaultHelper::Slot: альтернатива 3 мусить бути double");
	static_assert(std::is_same_v<std::variant_alternative_t<4, Slot>, bool>,
		"DefaultHelper::Slot: альтернатива 4 мусить бути bool");

	Slot slot_;
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
		// оператори нижче — тонкі обгортки над ними, і тіла їхні лежать у .cpp,
		// НЕ в тілі класу (причина — у коментарі перед їхніми деклараціями).
		// Спеціалізації Get/Set теж визначені в .cpp.
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
		// Єдина точка перевірки прив'язки: усі читання й clear() ходять через неї,
		// щоб перевірка «комірку не прив'язано» існувала в одному екземплярі, а не
		// повторювалась рядком у кожній спеціалізації Get<T>.
		tVariant* Bound() const;

		// Числове читання. Цілі різновиди 1С лежать у lVal, VTYPE_R4 — у fltVal,
		// VTYPE_R8 — у dblVal: це РІЗНІ члени об'єднання, і плутанина між ними вже
		// коштувала вади (TestFloatR4Conversion). expectedForError визначає, який тип
		// назве повідомлення про невідповідність: ціле чи дійсне.
		template <typename N>
		N ReadNumeric(TYPEVAR expectedForError) const;

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

	// Хендлер методу арності N — це std::function<void(VH, ..., VH)> з N аргументами.
	// Сімнадцять рядків «using MethFunctionN = ...» тут НЕ виписуються руками: список
	// породжується з index_sequence, тож описка в одному з них неможлива за побудовою,
	// а зміна межі арності — це правка одного числа, а не сімнадцяти рядків.
	template <typename Seq> struct HandlerOf;
	template <size_t... I> struct HandlerOf<std::index_sequence<I...>> {
		template <size_t> using ArgVH = VH;      // кожен індекс пакета дає рівно один VH
		using type = std::function<void(ArgVH<I>...)>;
	};
	template <size_t N> using MethFunctionN = typename HandlerOf<std::make_index_sequence<N>>::type;

	// Верхня межа арності. 9-10 параметрів потрібні драйверам БПО (контракт
	// «Подключаемое оборудование»: ОплатитьПлатежнойКартой — 9,
	// ОтменитьПлатежПоПлатежнойКарте — 10); такі методи до розширення не
	// реєструвалися взагалі — GetNParams віддавав 0, і 1С вважала метод
	// безпараметровим. Запас до 16 узятий свідомо: підняття межі тягне перезбірку
	// й гейт усіх компонент DLL.
	static constexpr size_t kMaxArity = 16;

	// Хендлер будь-якої підтримуваної арності. ПОЗИЦІЯ альтернативи в цьому варіанті
	// і Є арністю — на цьому стоїть GetNParams, який віддає в 1С index() варіанта.
	// Побудова з того самого index_sequence гарантує цю відповідність структурно.
	template <typename Seq> struct HandlerVariantOf;
	template <size_t... I> struct HandlerVariantOf<std::index_sequence<I...>> {
		using type = std::variant<MethFunctionN<I>...>;
	};
	using MethFunction = typename HandlerVariantOf<std::make_index_sequence<kMaxArity + 1>>::type;

	// Асерти перевіряють не рукописний список (його вже немає), а те, що ПОБУДОВА
	// дає обіцяне: альтернатива N приймає рівно N аргументів VH. Саме на цьому
	// тримається контракт GetNParams.
	static_assert(std::variant_size_v<MethFunction> == kMaxArity + 1,
		"MethFunction: очікується kMaxArity + 1 альтернатив (арності 0..kMaxArity)");
	static_assert(std::is_same_v<std::variant_alternative_t<0, MethFunction>,
		std::function<void()>>,
		"MethFunction: альтернатива 0 мусить бути хендлером без параметрів");
	static_assert(std::is_same_v<std::variant_alternative_t<1, MethFunction>,
		std::function<void(VH)>>,
		"MethFunction: альтернатива 1 мусить приймати рівно один VH");
	static_assert(std::is_same_v<std::variant_alternative_t<9, MethFunction>,
		std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH)>>,
		"MethFunction: альтернатива 9 мусить приймати рівно дев'ять VH (ОплатитьПлатежнойКартой)");
	static_assert(std::is_same_v<std::variant_alternative_t<kMaxArity, MethFunction>,
		std::function<void(VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH, VH)>>,
		"MethFunction: остання альтернатива мусить приймати рівно kMaxArity аргументів VH");

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

	// Згортання регістру. ОБИДВА перевантаження мутують аргумент і повертають його ж —
	// на це спирається код поза ядром, тож сигнатури незмінні. Реалізація — поверх
	// NormalizeName, щоб таблиця регістру жила в одному місці.
	static std::u16string upper(std::u16string& str);
	static std::wstring   upper(std::wstring& str);

	// Конвертації між UTF-8 і UTF-16. Довжина скрізь передається ЯВНО — саме це
	// зберігає вбудований  ; обгортки ServiceTools::Safe* стоять поверх них.
	static std::string    WCHAR2MB(std::basic_string_view<WCHAR_T> src);
	static std::wstring   WCHAR2WC(std::basic_string_view<WCHAR_T> src);
	static std::u16string MB2WCHAR(std::string_view src);

	// Копія рядка в пам'яті менеджера 1С; кидає bad_alloc, якщо менеджера немає.
	WCHAR_T* W(const char16_t* str) const;

	// Версія компоненти з version.h — віддається у властивість Version/Версия.
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

	// Спільне тіло CallAsProc/CallAsFunc: межі індексу методу, ValidateParams (наш
	// механізм, ДО try — як і раніше) і виклик CallMethod у try/catch. CallAsFunc
	// НЕ будується поверх CallAsProc (той відв'язує result на самому вході — така
	// композиція стерла б прив'язку до комірки повернення); замість цього обидва
	// entry point у .cpp самі керують result навколо виклику Dispatch.
	bool Dispatch(const long n, tVariant* paParams, const long lSizeArray);

	// Спільна обгортка винятків для точок входу IComponentBase: std::u16string ->
	// AddError + false, будь-що інше -> тихий false. Один екземпляр ланцюга —
	// GetPropVal/SetPropVal/GetParamDefValue/Dispatch раніше тримали по своїй
	// дослівній копії того самого семирядкового try/catch; тепер лише викликають
	// Guarded з тілом-лямбдою. Шаблон — member, щоб AddError у catch-гілках
	// резолвився на this без явної передачі.
	template <typename Body>
	bool Guarded(Body&& body)
	{
		try { return body(); }
		catch (const std::u16string& msg) { AddError(msg); return false; }
		catch (...) { return false; }
	}

	// Розгортає виклик хендлера довільної арності: індекси параметрів беруться з
	// index_sequence, тож списки VA(...) для арностей 0..16 не виписуються руками
	// (17 рукописних списків — надто ласий грунт для описки в індексі).
	template <typename Fn, size_t... I>
	void InvokeHandler(const Fn& handler, tVariant* paParams, MethDesc* meth, std::index_sequence<I...>)
	{
		handler(VA(paParams, meth, static_cast<long>(I))...);
	}

	// Одна гілка диспетчера CallMethod: якщо у variant лежить саме Fn — перевірити
	// кількість фактичних параметрів і викликати. false = «це не та альтернатива,
	// або ця альтернатива не змогла виконатися» (в обох випадках CallMethod має
	// йти далі/повернути false — коротке замикання || коректне і без throw).
	template <size_t N>
	bool TryCallArity(MethFunction* function, tVariant* paParams, MethDesc* meth, const long lSizeArray)
	{
		auto handler = std::get_if<MethFunctionN<N>>(function);
		if (!handler) return false;
		if (lSizeArray < static_cast<long>(N)) {
			// 1С передала менше параметрів, ніж арність хендлера. Раніше тут летів
			// голий std::bad_function_call(), який зовнішній catch(...) мовчки гасив
			// у false — без жодного AddError 1С-розробнику. Явна перевірка з іменем
			// методу лишає той самий false, але з діагностикою — локаль-залежним
			// іменем, як і сусідній ValidateParams (той самий alias ? ru : en), з тим
			// самим запасним варіантом на порожнє nameRu, що й у GetMethodName.
			const std::u16string& mname = alias
				? (meth->nameRu.empty() ? meth->nameEn : meth->nameRu)
				: meth->nameEn;
			AddError(u"Невідповідність кількості параметрів методу " + mname);
			return false;
		}
		InvokeHandler(*handler, paParams, meth, std::make_index_sequence<N>{});
		return true;
	}

	// Перебір усіх арностей 0..kMaxArity. Згортка по || має ту саму семантику
	// короткого замикання, що й ланцюжок із сімнадцяти рядків, але список арностей
	// знову ж таки породжується, а не виписується — і не може розійтися з
	// MethFunction, бо будується з тієї самої межі.
	template <size_t... I>
	bool TryEachArity(MethFunction* function, tVariant* paParams, MethDesc* meth,
	                  const long lSizeArray, std::index_sequence<I...>)
	{
		return (TryCallArity<I>(function, paParams, meth, lSizeArray) || ...);
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

	// Дає DefaultHelper::Apply доступ до protected VariantHelper. DefaultHelper
	// живе ПЕРЕД AddInNative (потрібен йому для MethDefaults/ParamSpec), тож не
	// може ні назвати AddInNative::VariantHelper у власній сигнатурі, ні
	// сконструювати її без цієї дружби.
	friend class DefaultHelper;

	// Реєстр компонент — функціо-локальний статик (Meyers singleton): будується
	// при першому виклику, тому файло-рівнева реєстрація (REGISTER_COMPONENT) не
	// залежить від порядку статичної ініціалізації між одиницями трансляції.
	static std::map<std::u16string, CompFunction>& components();

	// Ім'я, під яким компоненту створила 1С: іде у префікс AddError і в
	// RegisterExtensionAs.
	std::u16string name;
	// Мова платформи: true == російська локаль. Вибирає, яке з двох імен і яку
	// мову повідомлення побачить прикладний розробник. Ставиться в SetLocale.
	bool alias = false;

public:
	AddInNative();
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
// Ці прототипи документують ПОВНИЙ перелік типів, які адаптер уміє читати й писати:
// primary-шаблон визначення не має, тож будь-який інший T дав би лінк-помилку.
// Для збірки вони не обов'язкові — Get<T>/Set<T> кличуть лише тіла операторів, а ті
// лежать в AddInNative.cpp нижче за самі спеціалізації; жодна інша одиниця трансляції
// (компоненти в src/components у тому числі) цих методів не називає — вона лінкується
// з символами операторів.
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
