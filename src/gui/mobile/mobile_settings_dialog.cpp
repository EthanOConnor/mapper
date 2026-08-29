/*
 *    Copyright 2026 Ethan O'Connor
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

#include "mobile_settings_dialog.h"

#include <QColor>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWidget>
#include <QQuickStyle>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include "gnss/gnss_controller.h"
#include "gnss/correction/ntrip_profile.h"
#include "gnss/correction/ntrip_profile_store.h"
#include "settings.h"
#include "util/translation_util.h"

/// The resource lives in a static library: without an explicit init the
/// linker drops it. Q_INIT_RESOURCE must be used at global scope, outside
/// any namespace (an anonymous namespace breaks its extern declaration).
static void initMobileResources()
{
	Q_INIT_RESOURCE(mobile);
}

namespace OpenOrienteering {

namespace {

/// One-time Qt Quick Controls style selection. Must run before the first
/// QML load; a no-op if some other component got there first.
void ensureQuickStyle()
{
	static const bool initialized = [] {
		if (QQuickStyle::name().isEmpty())
			QQuickStyle::setStyle(QStringLiteral("Material"));
		return true;
	}();
	Q_UNUSED(initialized)
}

}  // namespace


MobileSettingsBridge::MobileSettingsBridge(QObject* parent)
: QObject(parent)
{
	connect(&Settings::getInstance(), &Settings::settingsChanged, this, [this]() {
		emit receiverChanged();
		emit optionsChanged();
	});
}

QString MobileSettingsBridge::receiverSummary() const
{
	const auto& settings = Settings::getInstance();
	auto name = settings.gnssDeviceName();
	if (!name.isEmpty())
		return name;
	auto address = settings.gnssDeviceAddress();
	if (!address.isEmpty())
		return address;
	return tr("Phone location");
}

bool MobileSettingsBridge::hasSavedReceiver() const
{
	return !Settings::getInstance().gnssDeviceAddress().isEmpty();
}

bool MobileSettingsBridge::gnssAutoConnect() const
{
	return Settings::getInstance().gnssAutoConnect();
}

void MobileSettingsBridge::setGnssAutoConnect(bool value)
{
	Settings::getInstance().setGnssAutoConnect(value);
	emit optionsChanged();
}

bool MobileSettingsBridge::ntripAutoStart() const
{
	return Settings::getInstance().gnssAutoStartNtrip();
}

void MobileSettingsBridge::setNtripAutoStart(bool value)
{
	Settings::getInstance().setGnssAutoStartNtrip(value);
	emit optionsChanged();
}

bool MobileSettingsBridge::rawLogging() const
{
	return Settings::getInstance().gnssRawLogging();
}

void MobileSettingsBridge::setRawLogging(bool value)
{
	Settings::getInstance().setGnssRawLogging(value);
	emit optionsChanged();
}

QStringList MobileSettingsBridge::ntripProfiles() const
{
	return NtripProfileStore::profileNames();
}

QString MobileSettingsBridge::activeNtripProfile() const
{
	return Settings::getInstance().gnssNtripActiveProfile();
}

void MobileSettingsBridge::setActiveNtripProfile(const QString& name)
{
	Settings::getInstance().setGnssNtripActiveProfile(name);
	emit profilesChanged();
}

bool MobileSettingsBridge::openMruFile() const
{
	return Settings::getInstance().getSetting(Settings::General_OpenMRUFile).toBool();
}

void MobileSettingsBridge::setOpenMruFile(bool value)
{
	Settings::getInstance().setSetting(Settings::General_OpenMRUFile, value);
	emit optionsChanged();
}

QVariantList MobileSettingsBridge::languages() const
{
	QVariantList result;
	const auto languages = TranslationUtil::availableLanguages();
	for (const auto& language : languages)
	{
		QVariantMap entry;
		entry[QStringLiteral("code")] = language.code;
		entry[QStringLiteral("name")] = language.displayName;
		result.append(entry);
	}
	return result;
}

QString MobileSettingsBridge::languageCode() const
{
	return Settings::getInstance().getSetting(Settings::General_Language).toString();
}

void MobileSettingsBridge::setLanguageCode(const QString& code)
{
	Settings::getInstance().setSetting(Settings::General_Language, code);
	emit optionsChanged();
}

QStringList MobileSettingsBridge::sketchColors() const
{
	QStringList result;
	for (const auto& color : Settings::getInstance().paintOnTemplateColors())
		result.append(color.name(QColor::HexArgb));
	return result;
}

void MobileSettingsBridge::setSketchColors(const QStringList& colors)
{
	std::vector<QColor> values;
	values.reserve(std::size_t(colors.size()));
	for (const auto& name : colors)
	{
		QColor color(name);
		if (color.isValid())
			values.push_back(color);
	}
	Settings::getInstance().setPaintOnTemplateColors(values);
	emit sketchColorsChanged();
}

void MobileSettingsBridge::chooseReceiver()
{
	auto* parent_widget = qobject_cast<QWidget*>(this->parent());
	GnssController::instance().chooseExternalReceiver(parent_widget);
	emit receiverChanged();
}

void MobileSettingsBridge::useSystemLocation()
{
	auto& settings = Settings::getInstance();
	settings.setGnssDeviceAddress({});
	settings.setGnssDeviceName({});
	emit receiverChanged();
}

QVariantMap MobileSettingsBridge::loadNtripProfile(const QString& name) const
{
	QVariantMap map;
	QString error;
	auto profile = NtripProfileStore::load(name, &error);
	if (!profile)
		return map;
	map[QStringLiteral("name")] = profile->name;
	map[QStringLiteral("host")] = profile->casterHost;
	map[QStringLiteral("port")] = int(profile->casterPort);
	map[QStringLiteral("mountpoint")] = profile->mountpoint;
	map[QStringLiteral("username")] = profile->username;
	map[QStringLiteral("hasPassword")] = !profile->password.isEmpty();
	map[QStringLiteral("useTls")] = profile->useTls;
	map[QStringLiteral("sendGga")] = profile->sendGga;
	return map;
}

QString MobileSettingsBridge::saveNtripProfile(const QString& original_name,
                                               const QVariantMap& fields)
{
	NtripProfile profile;
	QString error;
	if (!original_name.isEmpty())
	{
		if (auto saved = NtripProfileStore::load(original_name, &error))
			profile = *saved;
	}
	profile.name = fields.value(QStringLiteral("name")).toString().trimmed();
	profile.casterHost = fields.value(QStringLiteral("host")).toString().trimmed();
	profile.casterPort = quint16(fields.value(QStringLiteral("port")).toInt());
	profile.mountpoint = fields.value(QStringLiteral("mountpoint")).toString().trimmed();
	profile.username = fields.value(QStringLiteral("username")).toString();
	auto password = fields.value(QStringLiteral("password")).toString();
	if (!password.isEmpty())
		profile.password = password;
	profile.useTls = fields.value(QStringLiteral("useTls")).toBool();
	profile.sendGga = fields.value(QStringLiteral("sendGga")).toBool();

	if (profile.name.isEmpty())
		return tr("A profile name is required.");
	if (profile.casterHost.isEmpty())
		return tr("A caster host is required.");

	error.clear();
	auto ok = (!original_name.isEmpty() && original_name != profile.name)
	          ? NtripProfileStore::rename(original_name, profile, &error)
	          : NtripProfileStore::save(profile, &error);
	if (!ok)
		return error.isEmpty() ? tr("The profile could not be saved.") : error;

	if (activeNtripProfile() == original_name && original_name != profile.name)
		Settings::getInstance().setGnssNtripActiveProfile(profile.name);

	emit profilesChanged();
	return {};
}

void MobileSettingsBridge::removeNtripProfile(const QString& name)
{
	NtripProfileStore::remove(name);
	if (activeNtripProfile() == name)
		Settings::getInstance().setGnssNtripActiveProfile({});
	emit profilesChanged();
}

QVariantList MobileSettingsBridge::sketchPresets() const
{
	// Matches the classic sketch palette presets.
	struct Preset { const char* name; const char* colors; };
	static const Preset presets[] = {
		{ QT_TR_NOOP("Default"), "FF0000,FFFF00,00FF00,DB00D9,0000FF,D15C00,000000" },
		{ QT_TR_NOOP("High contrast"), "FF0000,00FF00,0000FF,FFFF00,FF00FF,00FFFF,000000,FFFFFF" },
	};
	QVariantList result;
	for (const auto& preset : presets)
	{
		QVariantMap entry;
		entry[QStringLiteral("name")] = tr(preset.name);
		QStringList colors;
		for (const auto& color : Settings::colorsStringToVector(QLatin1String(preset.colors)))
			colors.append(color.name(QColor::HexArgb));
		entry[QStringLiteral("colors")] = colors;
		result.append(entry);
	}
	return result;
}


MobileSettingsDialog::MobileSettingsDialog(QWidget* parent)
: QDialog(parent)
, quick_widget(nullptr)
, bridge(new MobileSettingsBridge(this))
{
	setWindowTitle(tr("Settings"));
	initMobileResources();
	ensureQuickStyle();

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	quick_widget = new QQuickWidget(this);
	quick_widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
	quick_widget->engine()->rootContext()->setContextProperty(
	    QStringLiteral("settingsBridge"), bridge);
	quick_widget->setSource(QUrl(QStringLiteral("qrc:/mobile/MobileSettings.qml")));
	layout->addWidget(quick_widget);

	connect(bridge, &MobileSettingsBridge::closeRequested,
	        this, &MobileSettingsDialog::accept);

	if (parent)
		setGeometry(parent->window()->geometry());
	else if (auto* screen = QGuiApplication::primaryScreen())
		setGeometry(screen->availableGeometry());

	// Temporary UI-iteration hook: save a rendering of this dialog.
	if (qEnvironmentVariableIsSet("MAPPER_DEBUG_SHOOT"))
	{
		auto* timer = new QTimer(this);
		timer->setInterval(1500);
		connect(timer, &QTimer::timeout, this, [this] {
			grab().save(qEnvironmentVariable("MAPPER_DEBUG_SHOOT"));
		});
		timer->start();
	}
}

MobileSettingsDialog::~MobileSettingsDialog() = default;

}  // namespace OpenOrienteering
