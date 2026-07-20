#include "core/pch.h"

#include "TestComponent.h"

std::vector<std::u16string> TestComponent::names = {
	AddComponent(u"AddInNative", []() { return new TestComponent; }),
    AddComponent(u"SimplyAddinConnect", []() { return new TestComponent; }),
    AddComponent(u"SimplyConnect", []() { return new TestComponent; })
};
// Анти-стрип: не даємо лінкеру відкинути список імен разом з реєстрацією
namespace { [[maybe_unused]] auto& _forceTestComponentNames = TestComponent::names; }

TestComponent::TestComponent()
{
	REPORT_INFO("Инициализация компонента TestComponent");

	AddProperty(
		u"Text", u"Текст",
		[&](VH var) { var = this->getTestString(); },
		[&](VH var) { this->setTestString(var); });

	AddProperty(
		u"Number", u"Число",
		[&](VH var) { var = this->value; },
		[&](VH var) { this->value = var; });

	AddFunction(
		u"GetText", u"ПолучитьТекст", 
		[&]() { this->result = this->getTestString(); });

	AddProcedure(
		u"SetText", u"УстановитьТекст", 
		[&](VH par) { this->setTestString(par); }, 
		{{0, u"default: "}});

	// Добавление метода для генерации тестовой ошибки
	AddProcedure(
		u"GenerateTestError", u"СоздатьТестовуюОшибку",
		[&]() { this->GenerateTestError(); });

	// Методы для работы с COM-портами
	AddFunction(
		u"GetAvailablePorts", u"ПолучитьДоступныеПорты",
		[&]() { 
			// Преобразуем вектор портов в строку, разделенную запятыми
			auto ports = this->GetAvailablePorts();
			std::u16string portsStr;
			
			for (size_t i = 0; i < ports.size(); ++i) {
				portsStr += ports[i];
				if (i < ports.size() - 1) {
					portsStr += u",";
				}
			}
			
			this->result = portsStr;
			return true;
		});
		
	AddFunction(
		u"CheckPortExists", u"ПроверитьСуществованиеПорта",
		Ret([&](VH portName) {
			std::u16string port = portName;
			return this->CheckPortExists(port);
		}));

	AddFunction(
		u"IsPortAvailable", u"ДоступенПорт",
		Ret([&](VH portName) {
			std::u16string port = portName;
			return this->IsPortAvailable(port);
		}));

	AddFunction(
		u"OpenPort", u"ОткрытьПорт",
		Ret([&](VH portName, VH baudRate) {
			std::u16string port = portName;
			std::u16string baud = baudRate;
			return this->OpenPort(port, baud);
		}));

	AddFunction(
		u"ClosePort", u"ЗакрытьПорт",
		Ret([&](VH portName) {
			std::u16string port = portName;
			return this->ClosePort(port);
		}));
		
	// Свойство состояния порта
	AddProperty(
		u"IsOpen", u"Открыт",
		[&](VH var) { var = this->isPortOpen; });
}

TestComponent::~TestComponent()
{
	REPORT_INFO("Завершение работы компонента");
	
	// Закрываем COM-порт при завершении работы
	if (comTransport && comTransport->IsOpen()) {
		REPORT_INFO("Закрытие COM-порта при завершении работы компонента");
		comTransport->Close();
	}
	
	ServiceTools::DisableComponentLogging(this);
}

std::u16string TestComponent::getTestString()
{
	REPORT_DEBUG("Получение текстового значения");
	
	time_t rawtime;
	struct tm timeinfo;
	char buffer[255];
	time(&rawtime);
	localtime_s(&timeinfo, &rawtime);
	strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
    REPORT_DEBUG("getTestString() викликано: поточний час " + std::string(buffer));
	
	std::string timeStr = buffer;
	REPORT_TRACE("Сформировано время: " + timeStr);
	
	return text + MB2WCHAR(buffer);
}

void TestComponent::setTestString(const std::u16string &text)
{
	try {
		std::string textLog = ServiceTools::SafeWCHAR2MB(text);
		REPORT_INFO("Установка текстового значения: " + textLog);
		this->text = text;
	}
	catch (const std::exception& e) {
		REPORT_ERROR("Ошибка при установке текста: " + std::string(e.what()));
	}
}

void TestComponent::GenerateTestError()
{
	try {
		// Логируем попытку генерации тестовой ошибки
		REPORT_INFO("Запуск метода генерации тестовой ошибки");
		
		// Генерируем ошибку для проверки системы логирования
		REPORT_ERROR("Спеціальна тестова помилка для перевірки");
		
		// // Также добавляем ошибку напрямую через метод AddError
		// std::u16string errorMessage = MB2WCHAR("Спеціальна тестова помилка для перевірки напряму");
		// this->AddError(errorMessage, 1001);
		
		// Логируем, что ошибка была сгенерирована
		REPORT_INFO("Тестовая ошибка успешно создана и передана в 1С");
		
		// Можно также сгенерировать исключение для проверки перехвата
		throw std::runtime_error("Тестовая ошибка для отладки механизма логирования");
	}
	catch (const std::exception& e) {
		// Перехватываем и логируем исключение
		REPORT_ERROR("Перехвачено исключение: " + std::string(e.what()));
	}
}