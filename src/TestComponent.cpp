#include "TestComponent.h"

std::vector<std::u16string> TestComponent::names = {
	AddComponent(u"AddInNative", []() { return new TestComponent; }),
    AddComponent(u"SimplyAddinConnect", []() { return new TestComponent; }),
    AddComponent(u"SimplyConnect", []() { return new TestComponent; })
};

TestComponent::TestComponent()
{
	REPORT_INFO("Инициализация компонента TestComponent");
	
	AddProperty(
		u"Version", u"Версия",
		[&](VH var) { var = this->version(); });

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
	
	// Добавление метода для включения логирования
	AddFunction(
		u"EnableLogging", u"ИспользоватьЛогирование", 
		[&](VH logLevel, VH logFilePath) {
			try {
				std::string level = logLevel.toString();
				std::string path = logFilePath.toString();
				
				return this->EnableLogging(level, path);
			}
			catch (const std::exception& e) {
				REPORT_ERROR("Ошибка при включении логирования: " + std::string(e.what()));
				return false;
			}
		});
}

TestComponent::~TestComponent()
{
	REPORT_INFO("Завершение работы компонента");
	ServiceTools::DisableComponentLogging(this);
}

bool TestComponent::EnableLogging(const std::string& logLevel, const std::string& logFilePath)
{
	// Делегирование вызова к ServiceTools
	return ServiceTools::EnableComponentLogging(this, logLevel, logFilePath);
}

#include <iostream>
#include <ctime>

std::u16string TestComponent::getTestString()
{
	REPORT_DEBUG("Получение текстового значения");
	
	time_t rawtime;
	struct tm *timeinfo;
	char buffer[255];
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
	
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
