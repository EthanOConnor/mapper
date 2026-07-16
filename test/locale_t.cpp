/*
 *    Copyright 2016 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "locale_t.h"

#include <QtTest>
#include <QLatin1String>
#include <QLocale>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "util/translation_util.h"

using namespace OpenOrienteering;


namespace OpenOrienteering {

inline
bool operator==(const TranslationUtil::Language& first, const TranslationUtil::Language& second) {
	return first.code == second.code && first.displayName == second.displayName;
}


}  // namespace OpenOrienteering



LocaleTest::LocaleTest(QObject* parent)
: QObject(parent)
{
	// nothing
}


void LocaleTest::testEsperantoQLocale()
{
	QCOMPARE(QLocale::languageToString(QLocale::Esperanto), QString::fromLatin1("Esperanto"));
	
	QCOMPARE(QLocale(QString::fromLatin1("eo")).language(), QLocale::Esperanto);
	
	QCOMPARE(QLocale(QString::fromLatin1("eo_C")).language(), QLocale::Esperanto);
	
	QCOMPARE(QLocale(QLocale::Esperanto, QLocale::AnyScript, QLocale::AnyCountry).language(), QLocale::Esperanto);
}

void LocaleTest::testEsperantoTranslationUtil()
{
	auto eo = QString::fromLatin1("eo");
	QCOMPARE(TranslationUtil::languageFromCode(eo).code, eo);
	QCOMPARE(TranslationUtil::languageFromCode(eo).displayName, QString::fromLatin1("Esperanto"));
	
	TranslationUtil translation { eo };
	QCOMPARE(translation.code(), eo);
	QCOMPARE(translation.displayName(), QString::fromLatin1("Esperanto"));
	
	auto test_basename = QLatin1String("LocaleTest");
	TranslationUtil::setBaseName(test_basename);
	
	auto test_filename = QString { QLatin1String("some_dir/") + test_basename + QLatin1String("_eo.qm") };
	QCOMPARE(TranslationUtil::languageFromFilename(test_filename), TranslationUtil::languageFromCode(eo));
}

void LocaleTest::testPreferredTranslationLanguage()
{
	const TranslationUtil::LanguageList available_languages = {
		TranslationUtil::languageFromCode(QString::fromLatin1("en")),
		TranslationUtil::languageFromCode(QString::fromLatin1("pt")),
		TranslationUtil::languageFromCode(QString::fromLatin1("pt_BR")),
		TranslationUtil::languageFromCode(QString::fromLatin1("zh_CN")),
		TranslationUtil::languageFromCode(QString::fromLatin1("zh_Hant")),
	};

	auto preferred = [&available_languages](const QLocale& locale) {
		return TranslationUtil::languageFromUiLanguages(
			locale.uiLanguages(QLocale::TagSeparator::Underscore),
			available_languages
		).code;
	};

	QCOMPARE(preferred(QLocale(QString::fromLatin1("pt_BR"))), QString::fromLatin1("pt_BR"));
	QCOMPARE(preferred(QLocale(QString::fromLatin1("pt_PT"))), QString::fromLatin1("pt"));
	QCOMPARE(preferred(QLocale(QString::fromLatin1("zh_Hant_TW"))), QString::fromLatin1("zh_Hant"));
	QCOMPARE(
		TranslationUtil::languageFromUiLanguages(
			{ QString::fromLatin1("fr_CA"), QString::fromLatin1("fr") },
			available_languages
		).code,
		QString::fromLatin1("en")
	);
}

void LocaleTest::testExplicitTranslationLanguage()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());

	QSettings settings(directory.filePath(QString::fromLatin1("settings.ini")), QSettings::IniFormat);
	settings.setValue(QString::fromLatin1("language"), QString::fromLatin1("zz_Explicit"));

	QCOMPARE(TranslationUtil::languageFromSettings(settings).code,
	         QString::fromLatin1("zz_Explicit"));
}


QTEST_GUILESS_MAIN(LocaleTest)
