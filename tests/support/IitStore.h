#pragma once
#include <string>

// Забезпечує сховище довіри ІІТ для арбітра iit_verify.
//
// P7B — це ВХІДНИЙ формат ІМПОРТУ (EUSaveCertificates), а не вміст файлового
// сховища: сама бібліотека веде його в каталозі EUSetFileStoreSettings і
// наповнює лише через SaveCertificates. Тому мало покласти CACertificates.p7b
// у каталог — його ще треба ОДИН РАЗ згодувати SaveCertificates.
//
//   outDirUtf8    — каталог для EUSetFileStoreSettings (UTF-8), заповнюється завжди
//   outBundlePath — шлях до CACertificates.p7b (широкий); ПОРОЖНІЙ, якщо
//                   імпорт не потрібен (сховище цієї машини вже наповнене)
//   outNeedImport — true, якщо бандл ще НЕ імпортовано в сховище цієї машини
//                   (немає маркера markIitStoreImported або бандл змінився)
//
// Повертає false, якщо каталог кешу ні знайти, ні наповнити не вдалося —
// виклик -> SKIP, ніколи не PASS.
bool ensureIitStore(std::string& outDirUtf8, std::wstring& outBundlePath, bool& outNeedImport);

// Позначає бандл як успішно імпортований у сховище довіри (маркер поруч із
// бандлом: розмір + FNV-1a хеш). Викликати ЛИШЕ після EUSaveCertificates == 0.
// Робить наступні прогони швидкими: EUSaveCertificates на великому бандлі ЦЗО —
// дорога операція, і повторювати її на кожен запуск марно.
bool markIitStoreImported(const std::wstring& bundlePath);
