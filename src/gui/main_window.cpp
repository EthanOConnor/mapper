/*
 *    Copyright 2012, 2013, 2014 Thomas Schöps
 *    Copyright 2012-2018 Kai Pastor
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


#include "main_window.h"
#include "gui/action_icon.h"

#include <chrono>
#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMenuBar>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWhatsThis>

#if defined(Q_OS_ANDROID)
#  include <QCoreApplication>
#  include <QJniObject>
#  include <QScreen>
#endif

#include "mapper_config.h"
#include "settings.h"
#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_imagery_catalog.h"
#include "collaboration/map_hub_read_only_document.h"
#include "collaboration/map_hub_sync_controller.h"
#include "collaboration/map_hub_workspace.h"
#include "core/document_path.h"
#include "core/georeferencing.h"
#if defined(Q_OS_IOS)
#  include "core/apple_document_access.h"
#endif
#include "core/map.h"
#include "core/map_view.h"
#include "core/symbols/symbol.h"
#include "fileformats/file_format.h"
#include "fileformats/file_format_registry.h"
#include "fileformats/file_import_export.h"
#include "gui/about_dialog.h"
#include "gui/autosave_dialog.h"
#include "gui/file_dialog.h"
#include "gui/home_screen_controller.h"
#include "gui/map_hub_dialog.h"
#include "gui/settings_dialog.h"
#include "gui/util_gui.h"
#include "gui/map/map_editor.h"
#include "gui/map/new_map_dialog.h"
#include "gui/widgets/toast.h"
#include "imagery/tile_network_manager.h"
#include "templates/template.h"
#include "undo/undo_manager.h"
#include "util/util.h"
#include "util/mapper_service_proxy.h"


#ifdef __clang_analyzer__
#define singleShot(A, B, C) singleShot(A, B, #C) // NOLINT 
#endif


namespace OpenOrienteering {

constexpr int MainWindow::max_recent_files;

int MainWindow::num_open_files = 0;

MainWindow::MainWindow(QWidget* parent, Qt::WindowFlags flags)
: MainWindow { true, parent, flags }
{
	// nothing else
}

MainWindow::MainWindow(bool as_main_window, QWidget* parent, Qt::WindowFlags flags)
: QMainWindow           { parent, flags }
, controller            { nullptr }
, create_menu           { as_main_window }
, show_menu             { create_menu && !Settings::mobileModeEnforced() }
, shortcuts_blocked     { false }
, general_toolbar       { nullptr }
, file_menu             { nullptr }
, has_opened_file       { false }
, has_unsaved_changes   { false }
, has_autosave_conflict { false }
, maximized_before_fullscreen { false }
, homescreen_disabled   { false }
{
	setWindowIcon(QIcon(QString::fromLatin1(":/images/mapper.png")));
	setAttribute(Qt::WA_DeleteOnClose);
	
	status_label = new QLabel();
	statusBar()->addWidget(status_label, 1);
	map_hub_sync_label = new QLabel();
	map_hub_sync_label->setVisible(false);
	map_hub_sync_label->setContentsMargins(8, 0, 4, 0);
	map_hub_sync_label->setTextFormat(Qt::RichText);
	map_hub_sync_label->setOpenExternalLinks(false);
	connect(map_hub_sync_label, &QLabel::linkActivated, this,
	        [this](const QString& link) {
		        if (link == QLatin1String("local-copy"))
			        openMapHubLocalCopy();
		        else
			        showMapHub();
	        });
	statusBar()->addPermanentWidget(map_hub_sync_label);
	statusBar()->setSizeGripEnabled(as_main_window);
	updateToastEnabled();
	
	central_widget = new QStackedWidget(this);
	QMainWindow::setCentralWidget(central_widget);
	
	if (as_main_window)
		loadWindowSettings();
	
#if defined(MAPPER_MOBILE)
	// Needed to catch Qt::Key_Back, cf. MainWindow::eventFilter()
	qApp->installEventFilter(this);
#else
	installEventFilter(this);
#endif
	
	connect(&Settings::getInstance(), &Settings::settingsChanged, this, &MainWindow::settingsChanged);
	connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::applicationStateChanged);
	map_hub_lease_timer = new QTimer(this);
	map_hub_lease_timer->setInterval(std::chrono::minutes(15));
	connect(map_hub_lease_timer, &QTimer::timeout, this,
	        &MainWindow::renewMapHubLeaseIfNeeded);
	map_hub_lease_timer->start();
	map_hub_access_timer = new QTimer(this);
	map_hub_access_timer->setSingleShot(true);
	connect(map_hub_access_timer, &QTimer::timeout, this,
	        &MainWindow::pollMapHubReadOnlyAccess);
	map_hub_sync = new MapHubSyncController(this);
	connect(
		map_hub_sync, &MapHubSyncController::stateChanged, this,
		[this](MapHubSyncController::State state, const QString& text) {
			const auto visible =
				state != MapHubSyncController::State::Disconnected;
			map_hub_sync_label->setVisible(visible);
			const auto needs_review =
				state == MapHubSyncController::State::UpstreamChanged
				|| state == MapHubSyncController::State::ActionRequired;
			map_hub_sync_label->setText(
				needs_review
				? tr("%1 · <a href=\"map-hub\">Review</a>")
				    .arg(text.toHtmlEscaped())
				: text.toHtmlEscaped());
			map_hub_sync_label->setToolTip(text);
		});
	connect(
		map_hub_sync, &MapHubSyncController::upstreamChangeDetected, this,
		[this](const QString& message) {
			showStatusBarMessage(message, 15000);
		});
	static bool map_hub_credential_registered = false;
	if (!map_hub_credential_registered)
	{
		map_hub_credential_registered = true;
		auto server = Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
		auto credential = MapHubCredentials::readToken(server);
		if (!credential.token.isEmpty())
			imagery::TileNetworkManager::instance().setBearerCredential(
			  QUrl(server), credential.token.toUtf8(), MapHubCredentials::accountName(server).toUtf8());
	}
}



MainWindow::~MainWindow()
{
#if defined(Q_OS_IOS)
	if (presents_document)
	{
		presented_document_token = 0;
		AppleDocumentAccess::stopPresenting();
	}
#endif
	if (controller)
	{
		controller->detach();
		delete controller;
		delete general_toolbar;
	}
}

void MainWindow::settingsChanged()
{
	updateRecentFileActions();
	updateToastEnabled();
}

void MainWindow::updateToastEnabled()
{
	if (!Settings::getInstance().touchModeEnabled())
	{
		delete toast;
		toast = nullptr;
	}
	else if (!toast)
	{
		toast = new Toast(this);
	}
}



void MainWindow::applicationStateChanged()
{
	if (QGuiApplication::applicationState() == Qt::ApplicationActive)
	{
		QTimer::singleShot(0, this, &MainWindow::renewMapHubLeaseIfNeeded);
		if (map_hub_read_only && map_hub_access_timer)
			map_hub_access_timer->start(0);
		if (map_hub_sync)
			map_hub_sync->applicationBecameActive();
		QTimer::singleShot(0, this, &MainWindow::refreshMapHubImageryCatalog);
	}
	else if (map_hub_sync)
	{
		map_hub_sync->applicationWillResignActive();
	}
#ifdef Q_OS_ANDROID
	// The Android app may be started or resumed when the user triggers a suitable "intent".
	if (QGuiApplication::applicationState() == Qt::ApplicationActive)
	{
		auto activity = QNativeInterface::QAndroidApplication::context();
		auto intent_uri = activity.callMethod<QString>("takeIntentUri");
		if (!intent_uri.isEmpty())
		{
			const auto selected_file = DocumentPath::fromUrl(QUrl{intent_uri});
			openExternalPath(selected_file);
			return;
		}
	}
#endif

#if defined(Q_OS_IOS)
	if (QGuiApplication::applicationState() == Qt::ApplicationActive)
	{
		if (external_change_pending)
			QTimer::singleShot(0, this, &MainWindow::processPresentedDocumentChange);
	}
	else
	{
		// iOS can suspend the process immediately after this transition. Flush
		// both preferences and the existing crash-recovery document while the
		// application still has execution time; never request background location.
		QSettings().sync();
		if (has_opened_file && has_unsaved_changes
		    && canAdvancePrivateRecovery())
			autosave();
	}
#endif
	
	// Only on startup, we may need to load the most recently used file.
	static bool starting_up = true;
	if (starting_up)
	{
		starting_up = false;
		QSettings settings;
		if (path_backlog.isEmpty()
		    && settings.value(QLatin1String("openMRUFile")).toBool())
		{
			const auto files = settings.value(QLatin1String("recentFileList")).toStringList();
			if (!files.isEmpty())
				openPathLater(files[0]);
		}
	}
}



QString MainWindow::appName() const
{
	return APP_NAME;
}


void MainWindow::setCentralWidget(QWidget* widget)
{
	if (widget)
	{
		// Main window shall not resize to central widget size hint.
		widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
		
		int index = central_widget->addWidget(widget);
		central_widget->setCurrentIndex(index);
	}
	
	if (central_widget->count() > 1)
	{
		QWidget* w = central_widget->widget(0);
		central_widget->removeWidget(w);
		w->deleteLater();
	}
}

void MainWindow::setHomeScreenDisabled(bool disabled)
{
	homescreen_disabled = disabled;
}

void MainWindow::setIgnoreTouch(bool on)
{
	ignore_touch_input = on;
}

void MainWindow::warnAndSetIgnoreTouch(bool on)
{
	if (!on || ignoreTouch())
	{
		setIgnoreTouch(on);
		return;
	}
	
	auto* layout = new QVBoxLayout();
	layout->addWidget(Util::Headline::create(tr("Information")));
	auto* text = new QLabel(tr("When you select the \"OK\" button, the editor will ignore touch input."));
	text->setWordWrap(true);
	layout->addWidget(text);
	auto* buttons = new QDialogButtonBox();
	auto* ok_button = buttons->addButton(QDialogButtonBox::Ok);
	buttons->addButton(tr("Continue with touch input"), QDialogButtonBox::RejectRole);
	layout->addWidget(buttons);
	
	QDialog dialog(this);
	dialog.setLayout(layout);
	dialog.setWindowModality(Qt::WindowModal);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	
	// The OK button must "accept" touch events in order to make
	// the event filter receive and consume these events.
	ok_button->setAttribute(Qt::WA_AcceptTouchEvents, true);
	ok_button->setDefault(true);
	
	ignore_touch_test_button = ok_button;
	auto confirmed = dialog.exec() == QDialog::Accepted;
	ignore_touch_test_button = nullptr;
	
	setIgnoreTouch(confirmed);
}

bool MainWindow::ignoreTouch()
{
	return ignore_touch_input;
}

void MainWindow::setController(MainWindowController* new_controller)
{
	setController(new_controller, false);
	setCurrentFile({}, nullptr);
}

void MainWindow::setController(MainWindowController* new_controller, const QString& path, const FileFormat* format)
{
	setController(new_controller, true);
	setCurrentFile(path, format);
}

void MainWindow::setController(MainWindowController* new_controller, bool has_file)
{
	if (controller)
	{
		controller->detach();
		delete controller;
		controller = nullptr;
		
		if (show_menu)
			menuBar()->clear();
		delete general_toolbar;
		general_toolbar = nullptr;
	}
	
	has_opened_file = has_file;
	shortcuts_blocked = false;
	
	if (create_menu)
		createFileMenu();
	
	controller = new_controller;
	menuBar()->setVisible(new_controller->menuBarVisible());
	statusBar()->setVisible(new_controller->statusBarVisible());
	controller->attach(this);
	
	if (create_menu)
		createHelpMenu();
	
#if defined(Q_OS_MACOS)
	if (isVisible() && qApp->activeWindow() == this)
	{
		// Force a menu synchronisation,
		// QCocoaMenuBar::updateMenuBarImmediately(),
		// via QCocoaNativeInterface::onAppFocusWindowChanged().
		/// \todo Review in Qt > 5.6
		qApp->focusWindowChanged(qApp->focusWindow());
	}
	
# if defined(MAPPER_DEVELOPMENT_BUILD)
	{
		// Qt's menu text heuristic can assign unexpected platform specific roles,
		// which resulted in Mapper issue #1067. The only supported solution is
		// assigning QAction::NoRole) before adding items to the menubar.
		// Cf. QTBUG-30812.
		// However, the heuristic is required for some platform-specific items.
		// Cf. detectMenuRole() in qtbase/src/plugins/platforms/cocoa/messages.cpp
		const auto platform_keywords = {
			QCoreApplication::translate("QCocoaMenuItem", "Cut"),
			QCoreApplication::translate("QCocoaMenuItem", "Copy"),
			QCoreApplication::translate("QCocoaMenuItem", "Paste"),
			QCoreApplication::translate("QCocoaMenuItem", "Select All")
		};
		const auto menubar_actions = menuBar()->actions();
		for (const auto* menubar_action : menubar_actions)
		{
			if (const auto* menu = menubar_action->menu())
			{
				const auto menu_actions = menu->actions();
				for (const auto* action : menu_actions)
				{
					if (action->menuRole() != QAction::TextHeuristicRole
						|| action->isSeparator())
						continue;
					const auto text = action->text().remove(QLatin1Char('&'));
					if (std::none_of(begin(platform_keywords), end(platform_keywords), [&text](const auto& keyword) {
									return keyword.compare(text, Qt::CaseInsensitive) == 0;
						}))
					{
						// Such warnings may indicate missing setting of QAction::NoRole
						// on a (new) item, or incomplete translations for Mapper or Qt.
						qDebug("Unexpected TextHeuristicRole for \"%s > %s\"",
						       qUtf8Printable(menubar_action->text()),
						       qUtf8Printable(action->text()));
					}
				}
			}
		}
	}
# endif  // MAPPER_DEVELOPMENT_BUILD
#endif  // Q_OS_MACOS
	
	setHasAutosaveConflict(false);
	setHasUnsavedChanges(false);
}

void MainWindow::createFileMenu()
{
	QAction* new_act = new QAction(ActionIcon::fromName(u"new"), tr("&New"), this);
	new_act->setMenuRole(QAction::NoRole);
	new_act->setShortcuts(QKeySequence::New);
	new_act->setStatusTip(tr("Create a new map"));
	new_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(new_act, &QAction::triggered, this, &MainWindow::showNewMapWizard);
	
	QAction* open_act = new QAction(ActionIcon::fromName(u"open"), tr("&Open..."), this);
	open_act->setMenuRole(QAction::NoRole);
	open_act->setShortcuts(QKeySequence::Open);
	open_act->setStatusTip(tr("Open an existing file"));
	open_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(open_act, &QAction::triggered, this, &MainWindow::showOpenDialog);

	auto* map_hub_act = new QAction(tr("Map &Hub…"), this);
	map_hub_act->setMenuRole(QAction::NoRole);
	map_hub_act->setStatusTip(tr("Open the connected map library and your assignments"));
	connect(map_hub_act, &QAction::triggered, this, &MainWindow::showMapHub);

	map_hub_checkpoint_act = new QAction(tr("Checkpoint to Map Hub"), this);
	map_hub_checkpoint_act->setMenuRole(QAction::NoRole);
	map_hub_checkpoint_act->setStatusTip(tr("Upload an immutable checkpoint of this managed .omap workspace"));
	connect(map_hub_checkpoint_act, &QAction::triggered, this, [this] { checkpointMapHub(); });

	map_hub_submit_act = new QAction(tr("Submit to Map Hub for review…"), this);
	map_hub_submit_act->setMenuRole(QAction::NoRole);
	map_hub_submit_act->setStatusTip(tr("Checkpoint this managed map and submit it for review"));
	connect(map_hub_submit_act, &QAction::triggered, this, [this] { submitMapHub(); });
	
	open_recent_menu = new QMenu(tr("Open &recent"), this);
	open_recent_menu->menuAction()->setMenuRole(QAction::NoRole);
	open_recent_menu->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	for (auto& action : recent_file_act)
	{
		action = new QAction(this);
		connect(action, &QAction::triggered, this, &MainWindow::openRecentFile);
	}
	open_recent_menu_inserted = false;
	
	// NOTE: if you insert something between open_recent_menu and save_act, adjust updateRecentFileActions()!
	
	save_act = new QAction(ActionIcon::fromName(u"save"), tr("&Save"), this);
	save_act->setMenuRole(QAction::NoRole);
	save_act->setShortcuts(QKeySequence::Save);
	save_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(save_act, &QAction::triggered, this, &MainWindow::save);
	
	save_as_act = new QAction(tr("Save &as..."), this);
	save_as_act->setMenuRole(QAction::NoRole);
	if (QKeySequence::keyBindings(QKeySequence::SaveAs).empty())
		save_as_act->setShortcut(tr("Ctrl+Shift+S"));
	else
		save_as_act->setShortcuts(QKeySequence::SaveAs);
	save_as_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(save_as_act, &QAction::triggered, this, &MainWindow::showSaveAsDialog);
	
	settings_act = new QAction(tr("Settings..."), this);
	settings_act->setMenuRole(QAction::PreferencesRole);
	settings_act->setShortcut(QKeySequence::Preferences);
	connect(settings_act, &QAction::triggered, this, &MainWindow::showSettings);
	
	close_act = new QAction(ActionIcon::fromName(u"close"), tr("Close"), this);
	close_act->setMenuRole(QAction::NoRole);
	close_act->setShortcut(QKeySequence::Close);
	close_act->setStatusTip(tr("Close this file"));
	close_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(close_act, &QAction::triggered, this, &MainWindow::closeFile);
	
	QAction* exit_act = new QAction(tr("E&xit"), this);
	exit_act->setMenuRole(QAction::QuitRole);
	exit_act->setShortcuts(QKeySequence::Quit);
	exit_act->setStatusTip(tr("Exit the application"));
	exit_act->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	connect(exit_act, &QAction::triggered, qApp, &QApplication::closeAllWindows);
	
	if (show_menu)
	{
		file_menu = menuBar()->addMenu(tr("&File"));
	}
	else
	{
		delete file_menu;
		file_menu = new QMenu(this);
	}

	file_menu->setWhatsThis(Util::makeWhatThis("file_menu.html"));
	file_menu->addAction(new_act);
	file_menu->addAction(open_act);
	file_menu->addAction(map_hub_act);
	file_menu->addAction(map_hub_checkpoint_act);
	file_menu->addAction(map_hub_submit_act);
	file_menu->addSeparator();
	file_menu->addAction(save_act);
	file_menu->addAction(save_as_act);
	file_menu->addSeparator();
	file_menu->addAction(settings_act);
	file_menu->addSeparator();
	file_menu->addAction(close_act);
#if !defined(Q_OS_IOS)
	file_menu->addAction(exit_act);
#endif
	
	general_toolbar = new QToolBar(tr("General"));
	general_toolbar->setObjectName(QString::fromLatin1("General toolbar"));
	general_toolbar->addAction(new_act);
	general_toolbar->addAction(open_act);
	general_toolbar->addAction(map_hub_act);
	general_toolbar->addAction(save_act);
	
	save_act->setEnabled(has_opened_file);
	save_as_act->setEnabled(has_opened_file);
	close_act->setEnabled(has_opened_file);
	updateMapHubActions();
	updateRecentFileActions();
}

void MainWindow::createHelpMenu()
{
	// Help menu
	QAction* manualAct = new QAction(ActionIcon::fromName(u"help"), tr("Open &Manual"), this);
	manualAct->setMenuRole(QAction::NoRole);
	manualAct->setStatusTip(tr("Show the help file for this application"));
	manualAct->setShortcut(QKeySequence::HelpContents);
	connect(manualAct, &QAction::triggered, this, &MainWindow::showHelp);
	
	QAction* aboutAct = new QAction(tr("&About %1").arg(appName()), this);
	aboutAct->setMenuRole(QAction::AboutRole);
	aboutAct->setStatusTip(tr("Show information about this application"));
	connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
	
	QAction* aboutQtAct = new QAction(tr("About &Qt"), this);
	aboutQtAct->setMenuRole(QAction::AboutQtRole);
	aboutQtAct->setStatusTip(tr("Show information about Qt"));
	connect(aboutQtAct, &QAction::triggered, qApp, QApplication::aboutQt);
	
	if (show_menu)
	{
		QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
		helpMenu->addAction(manualAct);
		helpMenu->addAction([this] {
			auto action = QWhatsThis::createAction(this);
			action->setMenuRole(QAction::NoRole);
			return action;
		}());
		helpMenu->addSeparator();
		helpMenu->addAction(aboutAct);
		helpMenu->addAction(aboutQtAct);
	}
}

void MainWindow::setCurrentFile(const QString& path, const FileFormat* format)
{
	Q_ASSERT(has_opened_file || path.isEmpty());
	const auto identity = DocumentPath::canonical(path);
	const auto previous_path = current_path;
	QString workspace_error;
	auto managed_workspace = previous_path.isEmpty()
	                       ? ManagedMapWorkspace{}
	                       : ManagedMapWorkspace::loadForMap(previous_path, &workspace_error);
	
	if (identity != current_path)
	{
		QString window_file_path;
		current_path.clear();
		current_format = nullptr;
		if (has_opened_file)
		{
			if (identity.isEmpty())
			{
				window_file_path = tr("Unsaved file");
			}
			else
			{
				current_path = identity;
				current_format = format;
				window_file_path = DocumentPath::isContentUri(identity)
				                 ? DocumentPath::displayName(identity)
				                 : identity;
			}
		}
		setWindowFilePath(window_file_path);
		if (managed_workspace.isValid() && !current_path.isEmpty() && previous_path != current_path)
		{
			managed_workspace.local_map_path = current_path;
			if (ManagedMapWorkspace::save(managed_workspace, &workspace_error))
				ManagedMapWorkspace::removeForMap(previous_path);
			else
				showStatusBarMessage(tr("Map Hub workspace metadata could not follow Save As: %1").arg(workspace_error), 10000);
		}
	}
	else if (!windowFilePath().isEmpty() && !has_opened_file)
	{
		setWindowFilePath({});
	}
	updateMapHubActions();
	configureMapHubSync();
	QTimer::singleShot(0, this, &MainWindow::renewMapHubLeaseIfNeeded);
}

void MainWindow::setMostRecentlyUsedFile(const QString& path)
{
	const auto identity = DocumentPath::canonical(path);
	if (!identity.isEmpty())
	{
		Settings& settings = Settings::getInstance();
		
		// Update least recently used directory
		if (!DocumentPath::isContentUri(identity))
		{
			const QString open_directory = QFileInfo(identity).canonicalPath();
			if (!open_directory.isEmpty())
				QSettings().setValue(QString::fromLatin1("openFileDirectory"), open_directory);
		}
		
		// Update recent file lists
		QStringList files = settings.getSettingCached(Settings::General_RecentFilesList).toStringList();
		files.removeAll(identity);
		files.prepend(identity);
		if (files.size() > max_recent_files)
			files.erase(files.begin() + max_recent_files, files.end());
		settings.setSetting(Settings::General_RecentFilesList, files);
	}
}

void MainWindow::setHasUnsavedChanges(bool value)
{
#if defined(Q_OS_IOS)
	presented_document_content_dirty = value;
	external_resources_dirty =
		controller && controller->hasDirtyExternalResources();
	has_unsaved_changes =
		presented_document_content_dirty || external_resources_dirty;
	if (presents_document)
		AppleDocumentAccess::setPresentedDocumentModified(
			presented_document_content_dirty);
#endif
#if !defined(Q_OS_IOS)
	has_unsaved_changes = value;
#endif
#if defined(Q_OS_IOS)
	setAutosaveNeeded(has_unsaved_changes
	                  && hasOpenedFile() && canAdvancePrivateRecovery());
#else
	setAutosaveNeeded(has_unsaved_changes && hasOpenedFile() && !has_autosave_conflict);
#endif
	setWindowModified(has_unsaved_changes);
	
#ifdef Q_OS_ANDROID
	if (!service_proxy)
		service_proxy = std::make_unique<MapperServiceProxy>();
	service_proxy->setActiveWindow(has_unsaved_changes ? this : nullptr);
#endif
}

#if defined(Q_OS_IOS)
void MainWindow::setUnsavedStateAfterDocumentCommit(bool external_resources_dirty)
{
	presented_document_content_dirty = false;
	this->external_resources_dirty = external_resources_dirty;
	has_unsaved_changes = this->external_resources_dirty;
	if (presents_document)
		AppleDocumentAccess::setPresentedDocumentModified(false);
	setAutosaveNeeded(has_unsaved_changes
	                  && hasOpenedFile() && canAdvancePrivateRecovery());
	setWindowModified(has_unsaved_changes);
}
#endif

void MainWindow::setStatusBarText(const QString& text)
{
	status_label->setText(text);
	status_label->setToolTip(text);
}

void MainWindow::showStatusBarMessage(const QString& text, int timeout)
{
	if (toast)
		toast->showText(text, timeout);
	else
		statusBar()->showMessage(text, timeout);
}

void MainWindow::showStatusBarMessageImmediately(const QString& text, int timeout)
{
	showStatusBarMessage(text, timeout);
	// Make sure that paint events reach the user screen, by processing events
	// until the queue is empty, including events appended during processing.
	// In the worst case, this will stop after 100 ms.
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100 /* ms */);
}

void MainWindow::clearStatusBarMessage()
{
	if (toast)
		toast->hide();
	else
		statusBar()->clearMessage();
}

void MainWindow::setShortcutsBlocked(bool blocked)
{
	shortcuts_blocked = blocked;
}

bool MainWindow::closeFile()
{
#if defined(Q_OS_IOS)
	if (provider_document_transaction_active || close_in_progress)
		return false;
	close_in_progress = true;
	auto close_guard = qScopeGuard([this] { close_in_progress = false; });
#endif
	bool closed = !has_opened_file || showSaveOnCloseDialog();
	if (closed)
	{
		if (has_opened_file)
		{
#if defined(Q_OS_IOS)
			if (!stopPresentingForClose())
				return false;
#endif
			num_open_files--;
			has_opened_file = false;
		}
		if (homescreen_disabled || num_open_files > 0)
			close();
		else
			setController(new HomeScreenController());
	}
	
	setIgnoreTouch(false);
	
	return closed;
}

bool MainWindow::event(QEvent* event)
{
	switch (event->type())
	{
	case QEvent::ShortcutOverride:
		if (shortcutsBlocked())
			event->accept();
		break;
		
	case QEvent::Resize:
		if (toast)
			toast->adjustPosition(frameGeometry());
		break;
		
	default:
		; // nothing
	}
	
	return QMainWindow::event(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
#if defined(Q_OS_IOS)
	if (provider_document_transaction_active)
	{
		event->ignore();
	}
	else
#endif
	if (!has_opened_file)
	{
		saveWindowSettings();
		event->accept();
	}
#if defined(Q_OS_IOS)
	else if (close_in_progress)
	{
		event->ignore();
	}
#endif
	else
	{
#if defined(Q_OS_IOS)
		close_in_progress = true;
		auto close_guard = qScopeGuard([this] { close_in_progress = false; });
#endif
		if (!showSaveOnCloseDialog())
		{
			event->ignore();
			return;
		}
		if (has_opened_file)
		{
#if defined(Q_OS_IOS)
			if (!stopPresentingForClose())
			{
				event->ignore();
				return;
			}
#endif
			num_open_files--;
			has_opened_file = false;
		}
		saveWindowSettings();
		event->accept();
	}
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
	if (controller && controller->keyPressEventFilter(event))
	{
		// Event filtered, stop handling
		return;
	}
	
	QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
	if (controller && controller->keyReleaseEventFilter(event))
	{
		// Event filtered, stop handling
		return;
	}
	
	QMainWindow::keyReleaseEvent(event);
}

bool MainWindow::showSaveOnCloseDialog()
{
#if defined(Q_OS_IOS)
	external_resources_dirty =
		controller && controller->hasDirtyExternalResources();
	has_unsaved_changes =
		presented_document_content_dirty || external_resources_dirty;
#endif
	if (has_opened_file && (has_unsaved_changes || has_autosave_conflict))
	{
		// Show the window in case it is minimized
		setWindowState( (windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
		raise();
		activateWindow();
		
		QMessageBox::StandardButton ret;
		if (!has_unsaved_changes && actual_path != autosavePath(currentPath()))
		{
			ret = QMessageBox::warning(this, appName(),
			                           tr("Do you want to remove the autosaved version?"),
			                           QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
		}
		else
		{
			ret = QMessageBox::warning(this, appName(),
			                           tr("The file has been modified.\n"
			                              "Do you want to save your changes?"),
			                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
		}
		
		switch (ret)
		{
		case QMessageBox::Cancel:
			return false;
			
		case QMessageBox::Discard:
#if defined(Q_OS_IOS)
			discardAuxiliaryDraftRecovery();
			// Deferred deletion is only valid after this in-memory map revision
			// commits. Discarding restores the provider's older references.
			clearDeferredPrivateDraftCleanup();
#endif
			if (has_autosave_conflict)
				setHasAutosaveConflict(false);
			removeAutosaveFile();
			break;
			
		case QMessageBox::Save:
			if (!save())
				return false;
			Q_FALLTHROUGH(); 
			
		 case QMessageBox::Yes:
			setHasAutosaveConflict(false);
			removeAutosaveFile();
			break;
			
		case QMessageBox::No:
			setHasAutosaveConflict(false);
			break;
			
		default:
			qWarning("Unsupported return value from message box");
			break;
		}
		
	}
	
	return true;
}

void MainWindow::saveWindowSettings()
{
#if !defined(MAPPER_MOBILE)
	QSettings settings;
	
	settings.beginGroup(QString::fromLatin1("MainWindow"));
	settings.setValue(QString::fromLatin1("pos"), pos());
	settings.setValue(QString::fromLatin1("size"), size());
	settings.setValue(QString::fromLatin1("maximized"), isMaximized());
	settings.endGroup();
#endif
}

void MainWindow::loadWindowSettings()
{
#if defined(Q_OS_ANDROID)
	// Always show the window on the whole available area on Android.
	if (auto* screen = qApp->screenAt(geometry().center()))
		resize(screen->availableGeometry().size());
#elif defined(MAPPER_MOBILE)
	// UIKit owns scene geometry, safe areas, Split View, and Stage Manager.
	// Avoid restoring desktop coordinates or forcing a full-screen size.
#else
	QSettings settings;
	
	settings.beginGroup(QString::fromLatin1("MainWindow"));
	QPoint pos = settings.value(QString::fromLatin1("pos"), QPoint(100, 100)).toPoint();
	QSize size = settings.value(QString::fromLatin1("size"), QSize(800, 600)).toSize();
	bool maximized = settings.value(QString::fromLatin1("maximized"), false).toBool();
	settings.endGroup();
	
	move(pos);
	resize(size);
	if (maximized)
		setWindowState((windowState() & ~(Qt::WindowMinimized | Qt::WindowFullScreen))
		               | Qt::WindowMaximized); // Cf. QWidget::showMaximized()
#endif
}

MainWindow* MainWindow::findMainWindow(const QString& file_name)
{
	const auto canonical_file_path = DocumentPath::canonical(file_name);
	if (canonical_file_path.isEmpty())
		return nullptr;
	
	const auto top_level_widgets = qApp->topLevelWidgets();
	for (auto widget : top_level_widgets)
	{
		MainWindow* other = qobject_cast<MainWindow*>(widget);
		if (other && other->currentPath() == canonical_file_path)
			return other;
	}
	
	return nullptr;
}

void MainWindow::showNewMapWizard()
{
	createNewMapWithWizard();
}

MainWindow* MainWindow::createNewMapWithWizard(
  unsigned int required_scale,
  const QString& required_crs,
  const QString& required_symbol_standard)
{
	NewMapDialog newMapDialog(this);
	if (required_scale > 0)
		newMapDialog.setInitialScale(required_scale, true);
	if (!required_symbol_standard.isEmpty())
		newMapDialog.setRequiredSymbolStandard(required_symbol_standard);
	newMapDialog.setWindowModality(Qt::WindowModal);
	newMapDialog.exec();
	
	if (newMapDialog.result() == QDialog::Rejected)
		return nullptr;
	
	Map* new_map = new Map();
	MapView tmp_view { nullptr, new_map };
	QString symbol_set_path = newMapDialog.getSelectedSymbolSetPath();
	if (symbol_set_path.isEmpty())
	{
		new_map->setScaleDenominator(newMapDialog.getSelectedScale());
	}
	else if (auto importer = FileFormats.makeImporter(symbol_set_path, *new_map, nullptr))
	{
		importer->setLoadSymbolsOnly(true);
		if (!importer->doImport())
		{
			QMessageBox::warning(this, tr("Error"),
			                     tr("Cannot open file:\n%1\n\n%2").
			                     arg(symbol_set_path, importer->warnings().back()));
			delete new_map;
			return nullptr;
		}
		if (!importer->warnings().empty())
			showMessageBox(this, tr("Warning"), tr("The symbol set import generated warnings."), importer->warnings());
		
		if (new_map->getScaleDenominator() != newMapDialog.getSelectedScale())
		{
			if (QMessageBox::question(this, tr("Warning"), tr("The selected map scale is 1:%1, but the chosen symbol set has a nominal scale of 1:%2.\n\nDo you want to scale the symbols to the selected scale?").arg(newMapDialog.getSelectedScale()).arg(new_map->getScaleDenominator()),  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
			{
				double factor = double(new_map->getScaleDenominator()) / newMapDialog.getSelectedScale();
				new_map->scaleAllSymbols(factor);
			}
			
			new_map->setScaleDenominator(newMapDialog.getSelectedScale());
		}
		
		for (int i = new_map->getNumSymbols(); i > 0; i = qMin(i, new_map->getNumSymbols()))
		{
			--i;
			auto symbol = new_map->getSymbol(i);
			if (symbol->isHidden()
			    && !new_map->existsObjectWithSymbol(symbol))
			{
				new_map->deleteSymbol(i);
			}
		}
	}
	else
	{
		;  /// \todo error message, cleanup
	}

	if (!required_crs.isEmpty())
	{
		auto georeferencing = new_map->getGeoreferencing();
		if (!georeferencing.setProjectedCRS(required_crs, required_crs))
		{
			QMessageBox::warning(
			  this, tr("Map Hub target CRS is unavailable"),
			  tr("Mapper could not configure the required coordinate reference "
			     "system %1. The Map Hub project remains available, but this local "
			     "map was not created or bound.")
			    .arg(required_crs));
			delete new_map;
			return nullptr;
		}
		new_map->setGeoreferencing(georeferencing);
	}
	if (!required_symbol_standard.isEmpty())
		new_map->setSymbolSetId(required_symbol_standard);

#ifdef MAPPER_MOBILE
	// Stage the complete replacement first. Only after symbol import and map
	// construction succeed do we ask to save/close the current document.
	if (hasOpenedFile() && !closeFile())
	{
		delete new_map;
		return nullptr;
	}
#endif
	
	auto map_view = new MapView { new_map };
	map_view->setGridVisible(tmp_view.isGridVisible());
	
	new_map->setHasUnsavedChanges(false);
	new_map->undoManager().clear();
	
	MainWindow* new_window = hasOpenedFile() ? new MainWindow() : this;
	auto const ignore_touch = Settings::getInstance().getSetting(Settings::MapEditor_IgnoreTouchInput).toBool();
	new_window->warnAndSetIgnoreTouch(ignore_touch);
	new_window->setWindowFilePath(tr("Unsaved file"));
	new_window->setController(new MapEditorController(MapEditorController::MapEditor, new_map, map_view), QString(), nullptr);
	
	new_window->show();
	new_window->raise();
	new_window->activateWindow();
	num_open_files++;

#if defined(Q_OS_IOS)
	// An untitled map has no durable provider identity, so it cannot satisfy
	// iOS's suspend-at-any-time recovery contract. Place the complete document
	// in Files before editing begins; cancelling abandons the new draft.
	if (!new_window->showSaveAsDialog()
	    && new_window->currentPath().isEmpty())
	{
		--num_open_files;
		new_window->setController(new HomeScreenController());
		new_window->showStatusBarMessage(
			tr("The new map was not created because no Files location was chosen."),
			5000);
	}
#endif

	return new_window;
}

void MainWindow::showMapHub()
{
	MapHubDialog dialog(this);
	dialog.setWindowModality(Qt::WindowModal);
	dialog.exec();
}

void MainWindow::createConnectedMap(const ManagedMapWorkspace& workspace)
{
	if (workspace.server_url.isEmpty() || workspace.project_id.isEmpty()
	    || workspace.work_package_id.isEmpty() || workspace.workspace_id.isEmpty())
	{
		QMessageBox::warning(this, tr("Map Hub"),
		                     tr("The server created the project but did not return a complete workspace. The project remains in Map Hub; refresh it before creating a local map."));
		return;
	}
	auto root = normalizedMapHubWorkspaceRoot(
	  Settings::getInstance().getSetting(Settings::MapHub_WorkspaceRoot).toString());
	if (!root.isEmpty())
	{
		auto directory_name = workspace.project_title;
		directory_name.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}._ -]+")), QStringLiteral("-"));
		directory_name = directory_name.simplified();
		if (directory_name.isEmpty() || directory_name == QLatin1String(".") || directory_name == QLatin1String(".."))
			directory_name = QStringLiteral("connected-map");
		if (directory_name.size() > 48)
			directory_name.truncate(48);
		auto project_directory = QDir(root).filePath(directory_name);
		QDir().mkpath(project_directory);
		QSettings().setValue(QString::fromLatin1("openFileDirectory"), project_directory);
	}
	auto* new_window = createNewMapWithWizard(
	  workspace.target_scale > 0 ? unsigned(workspace.target_scale) : 0,
	  workspace.target_crs, workspace.symbol_standard);
	if (!new_window)
	{
		QMessageBox::information(this, tr("Connected map not created locally"),
		                         tr("The Map Hub project was created and is still available in the library. No local map file was created."));
		return;
	}
	if (!new_window->showSaveAsDialog())
	{
		QMessageBox::information(new_window, tr("Connected map needs a local workspace"),
		                         tr("Save this new map as an .omap file to bind it to the Map Hub project."));
		return;
	}
	if (DocumentPath::suffix(new_window->currentPath()).compare(
	      QLatin1String("omap"), Qt::CaseInsensitive) != 0)
	{
		QMessageBox::warning(new_window, tr("Native .omap workspace required"),
		                     tr("The server project was created, but connected checkpoints require a native .omap workspace. Use Save As to create an .omap file, then start the work from Map Hub again."));
		return;
	}
	auto managed = workspace;
	managed.local_map_path = new_window->currentPath();
	managed.last_synced_at = QDateTime::currentDateTimeUtc();
	QString error;
	if (!ManagedMapWorkspace::save(managed, &error))
	{
		QMessageBox::warning(new_window, tr("Map saved but not connected"), error);
		return;
	}
	new_window->updateMapHubActions();
	new_window->configureMapHubSync();
	new_window->showStatusBarMessage(tr("Connected to Map Hub project “%1”.").arg(managed.project_title), 8000);
}

bool MainWindow::openConnectedWorkspace(const QString& source_path,
                                        const QString& normalized_omap_path,
                                        ManagedMapWorkspace workspace)
{
	if (!openPath(source_path))
		return false;
	auto* open_window = findMainWindow(source_path);
	if (!open_window)
	{
		QMessageBox::warning(this, tr("Map Hub"),
		                     tr("The verified map opened, but Mapper could not identify its editor window to bind the workspace."));
		return false;
	}
	auto local_path = source_path;
	if (DocumentPath::suffix(source_path).compare(
	      QLatin1String("omap"), Qt::CaseInsensitive) != 0)
	{
		auto* native_format = FileFormats.findFormat(FileFormats.defaultFormat());
		if (!native_format || normalized_omap_path.isEmpty()
		    || DocumentPath::suffix(normalized_omap_path).compare(
		         QLatin1String("omap"), Qt::CaseInsensitive) != 0
		    || !open_window->saveTo(normalized_omap_path, *native_format))
		{
			QMessageBox::warning(open_window, tr("Could not normalize connected map"),
			                     tr("The original server artifact remains preserved at:\n%1\n\nMapper could not create the required native .omap workspace.").arg(source_path));
			return false;
		}
		local_path = normalized_omap_path;
	}
	workspace.source_artifact_path = source_path;
	workspace.local_map_path = local_path;
	workspace.last_synced_at = QDateTime::currentDateTimeUtc();
	QString sidecar_error;
	if (!ManagedMapWorkspace::save(workspace, &sidecar_error))
	{
		QMessageBox::warning(open_window, tr("Map opened but not connected"), sidecar_error);
		return false;
	}
	open_window->updateMapHubActions();
	open_window->configureMapHubSync();
	open_window->showStatusBarMessage(
	  source_path == local_path
	    ? tr("Verified Map Hub revision r%1 opened.")
	        .arg(workspace.active_revision_number > 0
	               ? workspace.active_revision_number
	               : workspace.base_revision_number)
	    : tr("Preserved the original %1 baseline and created a normalized .omap workspace.")
	        .arg(workspace.base_artifact_kind.toUpper()),
	  10000);
	return true;
}

bool MainWindow::openMapHubReadOnly(
	const QString& source_path, const MapHubReadOnlyDocument& document)
{
	const QFileInfo source_info(source_path);
	QString hash_error;
	const auto actual_sha = MapHubApiClient::sha256ForFile(
	  source_path, &hash_error);
	if (!source_info.isFile()
	    || source_info.size() != document.revision_size_bytes
	    || actual_sha.compare(document.revision_sha256,
	                          Qt::CaseInsensitive) != 0)
	{
		QMessageBox::warning(
		  this,
		  tr("Could not verify read-only map"),
		  hash_error.isEmpty()
		    ? tr("The cached Map Hub revision did not match its advertised "
		         "size and checksum. It was not opened.")
		    : hash_error);
		return false;
	}
	if (!QFile::setPermissions(source_path, QFileDevice::ReadOwner))
	{
		QMessageBox::warning(
		  this, tr("Could not protect read-only map"),
		  tr("Mapper could not make its private Map Hub cache immutable. "
		     "The map was not opened."));
		return false;
	}
	if (!openPath(source_path))
		return false;

	auto* open_window = findMainWindow(source_path);
	if (!open_window)
	{
		QMessageBox::warning(
		  this, tr("Map Hub"),
		  tr("The verified map opened, but Mapper could not identify its "
		     "viewer window."));
		return false;
	}

	auto bound_document = document;
	bound_document.local_map_path = source_path;
	QString metadata_error;
	if (!MapHubReadOnlyDocument::save(bound_document, &metadata_error))
	{
		QMessageBox::warning(
		  open_window, tr("Map opened without read-only protection"),
		  metadata_error);
		open_window->closeFile();
		return false;
	}
	open_window->updateMapHubActions();
	open_window->configureMapHubSync();
	open_window->showStatusBarMessage(
	  tr("Opened verified Map Hub revision r%1 read-only.")
	    .arg(bound_document.revision_number),
	  10000);
	return true;
}

void MainWindow::updateMapHubReadOnlyAccess(
	const QString& project_id, const QJsonObject& request)
{
	if (currentPath().isEmpty())
		return;
	QString error;
	auto document = MapHubReadOnlyDocument::loadForMap(currentPath(), &error);
	if (!document.isValid() || document.project_id != project_id)
		return;

	const auto request_id = request.value(QStringLiteral("id")).toString();
	const auto status = request.value(QStringLiteral("status")).toString();
	static const QSet<QString> valid_statuses = {
		QStringLiteral("pending"),
		QStringLiteral("approved"),
		QStringLiteral("declined"),
		QStringLiteral("cancelled"),
		QStringLiteral("expired"),
	};
	if (QUuid(request_id).isNull() || !valid_statuses.contains(status))
		return;
	const auto previous_request_id = document.access_request_id;
	const auto previous_status = document.access_request_status;
	document.access_request_id = request_id;
	document.access_request_status = status;
	const auto assignment =
	  request.value(QStringLiteral("assignment")).toObject();
	document.approved_assignment_id =
	  assignment.value(QStringLiteral("id")).toString();
	document.last_checked_at = QDateTime::currentDateTimeUtc();
	if (previous_request_id != request_id)
		map_hub_access_etag.clear();
	if (MapHubReadOnlyDocument::save(document, &error))
	{
		updateMapHubActions();
		configureMapHubSync();
		if (status == QLatin1String("approved") && previous_status != status)
			showStatusBarMessage(
			  tr("Editing access for “%1” was approved.")
			    .arg(document.project_title),
			  15000);
	}
	else
	{
		showStatusBarMessage(
		  tr("Mapper could not retain the Map Hub request status: %1")
		    .arg(error),
		  10000);
	}
}

void MainWindow::updateMapHubActions()
{
	if (!map_hub_checkpoint_act || !map_hub_submit_act)
		return;
	QString error;
	auto workspace = current_path.isEmpty() ? ManagedMapWorkspace{} : ManagedMapWorkspace::loadForMap(current_path, &error);
	auto read_only = current_path.isEmpty()
	               ? MapHubReadOnlyDocument{}
	               : MapHubReadOnlyDocument::loadForMap(current_path, &error);
	auto native_workspace = workspace.isValid()
	                     && DocumentPath::suffix(current_path).compare(
	                          QLatin1String("omap"), Qt::CaseInsensitive) == 0;
	map_hub_checkpoint_act->setEnabled(
	  !read_only.isValid() && native_workspace
	  && workspace.status != QLatin1String("submitted"));
	map_hub_submit_act->setEnabled(
	  !read_only.isValid() && native_workspace
	  && workspace.status != QLatin1String("submitted"));
	if (native_workspace)
	{
		map_hub_checkpoint_act->setText(tr("Checkpoint “%1” to Map Hub").arg(workspace.project_title));
		map_hub_submit_act->setText(tr("Submit “%1” for review…").arg(workspace.project_title));
	}
	else
	{
		map_hub_checkpoint_act->setText(tr("Checkpoint to Map Hub"));
		map_hub_submit_act->setText(tr("Submit to Map Hub for review…"));
	}
}

void MainWindow::configureMapHubSync()
{
	if (!map_hub_sync)
		return;
	map_hub_sync->clear();
	auto* map_editor = qobject_cast<MapEditorController*>(controller);
	if (!controller || currentPath().isEmpty() || !currentFormat()
	    || !map_editor)
	{
		map_hub_read_only = false;
		if (map_hub_access_timer)
			map_hub_access_timer->stop();
		map_hub_access_etag.clear();
		return;
	}
	QString error;
	auto read_only = MapHubReadOnlyDocument::loadForMap(currentPath(), &error);
	if (read_only.isValid())
	{
		map_hub_read_only = true;
		map_editor->setReadOnly(true);
		if (save_act)
			save_act->setEnabled(false);
		if (save_as_act)
			save_as_act->setEnabled(false);
		map_hub_sync_label->setVisible(true);
		QString action = tr("Request edit access");
		QString state = tr("Read-only r%1").arg(read_only.revision_number);
		if (read_only.access_request_status == QLatin1String("pending"))
		{
			state = tr("Edit access pending");
			action = tr("Check request");
		}
		else if (read_only.access_request_status == QLatin1String("approved")
		         && !read_only.approved_assignment_id.isEmpty())
		{
			state = tr("Edit access approved");
			action = tr("Start editing");
		}
		map_hub_sync_label->setText(
		  tr("%1 · <a href=\"map-hub\">%2</a> · "
		     "<a href=\"local-copy\">Open local copy</a>")
		    .arg(state.toHtmlEscaped(), action.toHtmlEscaped()));
		map_hub_sync_label->setToolTip(
		  tr("This verified Map Hub revision cannot be changed locally."));
		if (read_only.access_request_status == QLatin1String("pending")
		    && map_hub_access_timer && !map_hub_access_poll_pending
		    && !map_hub_access_timer->isActive())
		{
			map_hub_access_timer->start(2000);
		}
		else if (read_only.access_request_status != QLatin1String("pending")
		         && map_hub_access_timer)
		{
			map_hub_access_timer->stop();
			map_hub_access_etag.clear();
		}
		QTimer::singleShot(0, this, &MainWindow::refreshMapHubImageryCatalog);
		return;
	}

	map_hub_read_only = false;
	if (map_hub_access_timer)
		map_hub_access_timer->stop();
	map_hub_access_etag.clear();
	map_editor->setReadOnly(false);
	if (save_act)
		save_act->setEnabled(has_opened_file);
	if (save_as_act)
		save_as_act->setEnabled(has_opened_file);
	if (DocumentPath::suffix(currentPath()).compare(
	      QLatin1String("omap"), Qt::CaseInsensitive) != 0)
		return;
	auto managed = ManagedMapWorkspace::loadForMap(currentPath(), &error);
	if (!managed.isValid())
		return;
	map_hub_sync->configure(
		managed,
		map_editor->getMap(),
		[this] {
			return controller ? controller->saveRevision() : quint64{0};
		},
		[this](const QString& destination, quint64* staged_revision,
		       QString* error) {
			if (!controller || !currentFormat())
			{
				if (error)
					*error = tr("The map editor is not ready to save.");
				return false;
			}
			QSaveFile file(destination);
			if (!file.open(QIODevice::WriteOnly))
			{
				if (error)
					*error = file.errorString();
				return false;
			}
			if (!controller->stageSaveTo(
			      currentPath(), *currentFormat(), &file, staged_revision)
			    || !file.commit())
			{
				if (error && error->isEmpty())
					*error = file.errorString();
				return false;
			}
			return true;
		},
#if defined(Q_OS_IOS)
		[this](const QString& snapshot_path, qint64 expected_size,
		       QString* error) {
			if (error)
				error->clear();
			if (snapshot_path.isEmpty()
			    || QFileInfo(snapshot_path).size() != expected_size)
			{
				if (error)
					*error = tr("The connected-editing recovery snapshot did not verify.");
				return false;
			}
			if (!presents_document || presented_document_deleted
			    || provider_document_transaction_active
			    || external_change_pending
			    || AppleDocumentAccess::hasPresentedDocumentConflict())
			{
				if (error)
					*error = tr("The document provider has a pending change.");
				return false;
			}

			const auto transaction_token = presented_document_token;
			const auto transaction_path = currentPath();
			const auto transaction_generation =
				presented_document_change_generation;
			QByteArray receipt;
			QString provider_error;
			if (!AppleDocumentAccess::capturePresentedDocumentWriteReceipt(
			      transaction_path, false, &receipt, &provider_error))
			{
				if (error)
					*error = provider_error;
				return false;
			}

			QString coordinated_path;
			bool saved = false;
			{
				QScopedValueRollback<bool> transaction_guard{
					provider_document_transaction_active, true};
				saved = AppleDocumentAccess::writePresentedDocument(
					transaction_path, snapshot_path, receipt, 0,
					&coordinated_path, &provider_error);
			}
			replayPendingPresentedDocumentEvents();
			const auto committed_path = coordinated_path.isEmpty()
			                          ? transaction_path
			                          : DocumentPath::canonical(coordinated_path);
			const auto stable = saved
			                    && presented_document_token == transaction_token
			                    && presents_document
			                    && !presented_document_deleted
			                    && currentPath() == transaction_path
			                    && committed_path == transaction_path
			                    && presented_document_change_generation
			                       == transaction_generation;
			if (!stable && error)
			{
				*error = provider_error.isEmpty()
				       ? tr("The document provider changed during autosave.")
				       : provider_error;
			}
			return stable;
		}
#else
		MapHubSyncController::WorkingCopyCommitter{}
#endif
	);
	QTimer::singleShot(0, this, &MainWindow::refreshMapHubImageryCatalog);
}


void MainWindow::refreshMapHubImageryCatalog()
{
	if (map_hub_imagery_refresh_pending || currentPath().isEmpty())
		return;
	QString error;
	auto const managed = ManagedMapWorkspace::loadForMap(currentPath(), &error);
	auto const read_only = MapHubReadOnlyDocument::loadForMap(currentPath(), &error);
	QString server_url;
	QString project_id;
	QString manifest_url;
	if (managed.isValid())
	{
		server_url = managed.server_url;
		project_id = managed.project_id;
		manifest_url = managed.manifest_url;
	}
	else if (read_only.isValid())
	{
		server_url = read_only.server_url;
		project_id = read_only.project_id;
		manifest_url = read_only.manifest_url;
	}
	if (server_url.isEmpty() || project_id.isEmpty() || manifest_url.isEmpty())
		return;
	auto const credential = MapHubCredentials::readToken(server_url);
	if (!credential.error.isEmpty() || credential.token.isEmpty())
		return;

	map_hub_imagery_refresh_pending = true;
	auto* client = new MapHubApiClient(server_url, credential.token, this);
	client->projectManifest(
		project_id,
		[this, client, manifest_url](
			const QJsonObject& manifest,
			const MapHubApiClient::Error& api_error) {
			map_hub_imagery_refresh_pending = false;
			client->deleteLater();
			if (api_error)
				return;
			auto const result = MapHubImageryCatalog::install(
				manifest, manifest_url);
			if (!result.error.isEmpty())
				showStatusBarMessage(result.error, 12000);
		});
}

void MainWindow::pollMapHubReadOnlyAccess()
{
	if (map_hub_access_poll_pending || currentPath().isEmpty()
	    || QGuiApplication::applicationState() != Qt::ApplicationActive)
		return;

	QString error;
	const auto document =
	  MapHubReadOnlyDocument::loadForMap(currentPath(), &error);
	if (!document.isValid()
	    || document.access_request_status != QLatin1String("pending")
	    || QUuid(document.access_request_id).isNull())
		return;
	const auto credential = MapHubCredentials::readToken(document.server_url);
	if (!credential || credential.token.isEmpty())
	{
		map_hub_sync_label->setToolTip(
		  tr("Reconnect this Map Hub account to refresh the access request."));
		return;
	}

	map_hub_access_poll_pending = true;
	auto* client =
	  new MapHubApiClient(document.server_url, credential.token, this);
	const auto expected_path = DocumentPath::canonical(currentPath());
	const auto expected_request = document.access_request_id;
	client->editAccessRequest(
	  document.access_request_id, map_hub_access_etag,
	  [this, client, expected_path, expected_request](
	    const QJsonObject& response, const QString& etag, bool not_modified,
	    const MapHubApiClient::Error& poll_error) {
		  map_hub_access_poll_pending = false;
		  client->deleteLater();
		  if (DocumentPath::canonical(currentPath()) != expected_path)
		  {
			  if (map_hub_access_timer)
				  map_hub_access_timer->start(0);
			  return;
		  }
		  if (!etag.isEmpty())
			  map_hub_access_etag = etag;
		  if (poll_error)
		  {
			  if (map_hub_access_timer)
				  map_hub_access_timer->start(30000);
			  return;
		  }
		  if (!not_modified)
		  {
			  auto request =
			    response.value(QStringLiteral("request")).toObject();
			  if (request.isEmpty())
				  request = response;
			  if (request.value(QStringLiteral("id")).toString()
			      != expected_request)
				  return;
			  updateMapHubReadOnlyAccess(
			    request.value(QStringLiteral("project_id")).toString(),
			    request);
		  }
		  else if (map_hub_access_timer)
		  {
			  map_hub_access_timer->start(10000);
		  }
	  });
}

void MainWindow::renewMapHubLeaseIfNeeded()
{
	if (map_hub_lease_renewal_pending || currentPath().isEmpty())
		return;
	QString metadata_error;
	auto managed = ManagedMapWorkspace::loadForMap(currentPath(), &metadata_error);
	if (!managed.isValid() || managed.status == QLatin1String("submitted")
	    || managed.file_protocol == QLatin1String("omap-snapshot/1"))
		return;
	auto now = QDateTime::currentDateTimeUtc();
	if (managed.lease_expires_at.isValid()
	    && now.secsTo(managed.lease_expires_at) > 2 * 60 * 60)
		return;
	auto account = MapHubCredentials::readToken(managed.server_url);
	auto lease_key = MapHubCredentials::workspaceLeaseKey(
	  managed.server_url, managed.workspace_id);
	auto lease = MapHubCredentials::readToken(lease_key);
	if (!account || !lease || account.token.isEmpty() || lease.token.isEmpty())
	{
		showStatusBarMessage(
		  tr("Map Hub could not renew this map's editing lease. Reopen the assignment before checkpointing."),
		  15000);
		return;
	}
	map_hub_lease_renewal_pending = true;
	auto* client = new MapHubApiClient(
	  managed.server_url, account.token, this);
	client->renewLease(
	  managed.workspace_id, lease.token,
	  [this, client, managed](const QJsonObject& response,
	                          const MapHubApiClient::Error& error) mutable {
		map_hub_lease_renewal_pending = false;
		client->deleteLater();
		if (error)
		{
			showStatusBarMessage(
			  tr("Map Hub editing lease was not renewed: %1").arg(error.message),
			  15000);
			return;
		}
		auto expires = QDateTime::fromString(
		  response.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
		if (!expires.isValid() || expires <= QDateTime::currentDateTimeUtc())
		{
			showStatusBarMessage(
			  tr("Map Hub returned an invalid editing-lease renewal."), 15000);
			return;
		}
		managed.lease_expires_at = expires;
		QString sidecar_error;
		if (!ManagedMapWorkspace::save(managed, &sidecar_error))
			showStatusBarMessage(
			  tr("The editing lease renewed, but its local expiry could not be recorded: %1")
			    .arg(sidecar_error),
			  15000);
	  });
}

void MainWindow::checkpointMapHub()
{
	checkpointMapHub(false);
}

void MainWindow::submitMapHub()
{
	checkpointMapHub(true);
}

void MainWindow::checkpointMapHub(bool submit_after)
{
	QString metadata_error;
	auto managed = ManagedMapWorkspace::loadForMap(currentPath(), &metadata_error);
	if (!managed.isValid())
	{
		QMessageBox::warning(this, tr("Map Hub"), metadata_error.isEmpty()
		                     ? tr("This is a standalone map, not a managed Map Hub workspace.") : metadata_error);
		return;
	}
	if (DocumentPath::suffix(currentPath()).compare(
	      QLatin1String("omap"), Qt::CaseInsensitive) != 0)
	{
		QMessageBox::warning(this, tr("Native .omap workspace required"),
		                     tr("Map Hub preserves the complete native Mapper workspace. Save this document as .omap before sending it for review."));
		return;
	}
	if (hasUnsavedChanges() && !save())
		return;
	QString hash_error;
	auto local_sha = MapHubApiClient::sha256ForFile(currentPath(), &hash_error);
	if (local_sha.isEmpty())
	{
		QMessageBox::warning(this, tr("Could not send map"), hash_error);
		return;
	}
	auto api_credential = MapHubCredentials::readToken(managed.server_url);
	if (!api_credential || api_credential.token.isEmpty())
	{
		QMessageBox::warning(this, tr("Map Hub account required"),
		                     api_credential.error.isEmpty() ? tr("Reconnect this server in Settings → Map Hub.") : api_credential.error);
		return;
	}

	auto* client = new MapHubApiClient(managed.server_url, api_credential.token, this);
	auto submit_revision = [this, client](QString revision_id, ManagedMapWorkspace updated) mutable {
		showStatusBarMessageImmediately(tr("Sending Map Hub revision for review…"));
		client->submitRevision(revision_id, {}, [this, client, updated, revision_id](const QJsonObject& response, const MapHubApiClient::Error& error) mutable {
			if (error)
			{
				clearStatusBarMessage();
				QMessageBox::warning(this, tr("Could not submit map"), error.message);
				client->deleteLater();
				return;
			}
			if (response.value(QStringLiteral("revision_id")).toString() != revision_id
			    || response.value(QStringLiteral("state")).toString() != QLatin1String("submitted"))
			{
				clearStatusBarMessage();
				QMessageBox::warning(this, tr("Invalid submission response"),
				                     tr("Map Hub did not confirm submission of the exact saved map."));
				client->deleteLater();
				return;
			}
			updated.status = QStringLiteral("submitted");
			updated.last_synced_at = QDateTime::currentDateTimeUtc();
			QString sidecar_error;
			if (!ManagedMapWorkspace::save(updated, &sidecar_error))
			{
				clearStatusBarMessage();
				QMessageBox::warning(this, tr("Map submitted, but local status was not updated"),
				                     tr("The server accepted the submission, but Mapper could not update its private workspace record: %1").arg(sidecar_error));
				client->deleteLater();
				return;
			}
			clearStatusBarMessage();
			updateMapHubActions();
			configureMapHubSync();
			QMessageBox::information(this, tr("Submitted to Map Hub"),
			                         tr("Revision r%1 is ready for review. Your local .omap file remains unchanged.")
			                           .arg(updated.active_revision_number));
			client->deleteLater();
		});
	};

	const bool needs_checkpoint = managed.active_revision_id.isEmpty()
	                             || managed.active_sha256.compare(local_sha, Qt::CaseInsensitive) != 0;
	if (!needs_checkpoint)
	{
		if (submit_after)
			submit_revision(managed.active_revision_id, managed);
		else
		{
			QMessageBox::information(this, tr("Map Hub checkpoint"),
			                         tr("This exact .omap file is already checkpointed as r%1.").arg(managed.active_revision_number));
			client->deleteLater();
		}
		return;
	}

	bool accepted = false;
	auto summary = QInputDialog::getMultiLineText(this,
	                                             submit_after ? tr("Submit map for review") : tr("Checkpoint map"),
	                                             tr("What changed?"), {}, &accepted).trimmed();
	if (!accepted)
	{
		client->deleteLater();
		return;
	}

	QString file_version_id;
	if (!map_hub_sync || !map_hub_sync->checkpointFileVersion(&file_version_id))
	{
		QMessageBox::information(
		  this, tr("Map Hub is still saving"),
		  tr("Your work is safe on this device. Wait for “Available on your other devices,” then try again."));
		client->deleteLater();
		return;
	}
	const auto revision_base = managed.active_revision_id.isEmpty()
	                         ? managed.base_revision_id : managed.active_revision_id;
	const auto key_material = managed.workspace_id + QLatin1Char('|')
	                        + revision_base + QLatin1Char('|')
	                        + file_version_id + QLatin1Char('|') + local_sha;
	const auto idempotency_key = QStringLiteral("mapper-%1").arg(QString::fromLatin1(
	  QCryptographicHash::hash(key_material.toUtf8(), QCryptographicHash::Sha256).toHex().left(48)));
	showStatusBarMessageImmediately(tr("Creating a reviewable Map Hub checkpoint…"));
	client->checkpointFile(
	  managed.workspace_id, currentPath(), revision_base, file_version_id,
	  managed.client_instance_id,
	  submit_after ? tr("Submission checkpoint") : tr("Mapper checkpoint"),
	  summary, idempotency_key,
	  [this, client, managed, local_sha, submit_after, submit_revision]
	  (const QJsonObject& response, const MapHubApiClient::Error& error) mutable {
		if (error)
		{
			clearStatusBarMessage();
			auto message = error.message;
			if (error.code == QLatin1String("stale_base"))
				message += tr("\n\nA newer review checkpoint exists. Your local file is unchanged; reopen Map Hub to compare.");
			QMessageBox::warning(this, tr("Could not checkpoint map"), message);
			client->deleteLater();
			return;
		}
		const auto returned_revision_id = response.value(QStringLiteral("revision_id")).toString();
		const auto returned_number = response.value(QStringLiteral("number")).toInt();
		const auto returned_sha = response.value(QStringLiteral("sha256")).toString();
		const auto returned_state = response.value(QStringLiteral("state")).toString();
		const auto valid_state = returned_state == QLatin1String("checkpoint")
		                    || returned_state == QLatin1String("draft")
		                    || returned_state == QLatin1String("rejected")
		                    || returned_state == QLatin1String("submitted");
		static const QRegularExpression sha256_pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
		if (QUuid(returned_revision_id).isNull() || returned_number <= 0
		    || !sha256_pattern.match(returned_sha).hasMatch() || !valid_state
		    || returned_sha.compare(local_sha, Qt::CaseInsensitive) != 0)
		{
			clearStatusBarMessage();
			QMessageBox::warning(this, tr("Invalid checkpoint response"),
			                     tr("Map Hub did not confirm the exact saved map. The local workspace was not advanced."));
			client->deleteLater();
			return;
		}
		auto updated = managed;
		updated.active_revision_id = returned_revision_id;
		updated.active_revision_number = returned_number;
		updated.active_sha256 = returned_sha;
		updated.status = returned_state;
		updated.last_synced_at = QDateTime::currentDateTimeUtc();
		QString sidecar_error;
		if (!ManagedMapWorkspace::save(updated, &sidecar_error))
		{
			clearStatusBarMessage();
			QMessageBox::warning(this, tr("Checkpoint uploaded, but local status was not updated"), sidecar_error);
			client->deleteLater();
			return;
		}
		configureMapHubSync();
		if (submit_after)
			submit_revision(updated.active_revision_id, updated);
		else
		{
			clearStatusBarMessage();
			showStatusBarMessage(tr("Map Hub checkpoint r%1 is ready to review.").arg(updated.active_revision_number), 10000);
			client->deleteLater();
		}
	  });
}
void MainWindow::showOpenDialog()
{
	const auto selected = getOpenFileName(
		this, tr("Open file"), FileFormat::AllFiles);
	if (selected)
	{
#ifdef MAPPER_MOBILE
		if (hasOpenedFile() && !closeFile())
			return;
#endif
		openPath(selected.filePath(), selected.fileFormat());
	}
}

bool MainWindow::openPath(const QString &path)
{
	auto format = FileFormats.findFormatForFilename(path, &FileFormat::supportsFileOpen);
#if !defined(Q_OS_IOS)
	if (!format)
		format = FileFormats.findFormatForData(path, FileFormat::AllFiles);
#endif
	return openPath(path, format);
}

bool MainWindow::openPath(const QString& path, const FileFormat* format)
{
	// Empty path does nothing. This also helps with the single instance application code.
	if (path.isEmpty())
		return true;

#if defined(Q_OS_IOS)
	if (!beginNativeDocumentInteraction())
	{
		openPathLater(path);
		return false;
	}
	auto provider_guard = qScopeGuard(
		[this] { endNativeDocumentInteraction(); });
#endif
	
#ifdef MAPPER_MOBILE
	if (hasOpenedFile())
	{
		const auto requested_path = DocumentPath::canonical(path);
		if (!requested_path.isEmpty() && requested_path == currentPath())
		{
			show();
			raise();
			activateWindow();
			return true;
		}

		showStatusBarMessage(
			tr("Close the current file before opening another document."), 5000);
		return false;
	}

	showStatusBarMessageImmediately(tr("Opening %1").arg(DocumentPath::displayName(path)));
#else
	MainWindow* const existing = findMainWindow(path);
	if (existing)
	{
		existing->show();
		existing->raise();
		existing->activateWindow();
		return true;
	}
#endif

	QString document_path = path;
	const FileFormat* document_format = format;
#if defined(Q_OS_IOS)
	const auto requested_document_path = DocumentPath::canonical(path);
	document_path = requested_document_path;
	auto provider_snapshot = std::make_unique<QTemporaryFile>(
		QDir::tempPath() + QLatin1String("/Mapper-open-XXXXXX"));
	if (!provider_snapshot->open())
	{
		QMessageBox::warning(this, tr("Error"),
		                     tr("Could not create a private document snapshot: %1")
		                     .arg(provider_snapshot->errorString()));
		return false;
	}
	provider_snapshot->close();

	QString coordination_error;
	QString coordinated_path;
	quint64 opening_token = 0;
	if (!AppleDocumentAccess::openDocument(
			document_path,
			provider_snapshot->fileName(),
			makePresentedDocumentChangeHandler(),
			&opening_token,
			&coordinated_path,
			&coordination_error))
	{
		if (!coordination_error.isEmpty())
			QMessageBox::warning(this, tr("Error"), coordination_error);
		return false;
	}
	presented_document_token = opening_token;
	presents_document = true;
	presented_document_deleted = false;
	presented_document_change_generation = 0;
	presented_document_content_dirty = false;
	external_resources_dirty = false;
	if (!coordinated_path.isEmpty())
	{
		const auto coordinated_document_path =
			DocumentPath::canonical(coordinated_path);
		if (!requested_document_path.isEmpty()
		    && coordinated_document_path != requested_document_path)
		{
			if (!migrateRecoveryForDocumentMove(
				    requested_document_path, coordinated_document_path))
			{
				QMessageBox::warning(
					this,
					tr("Document not opened"),
					tr("Files moved this document while Mapper was closed, but Mapper "
					   "could not move every private recovery receipt to its new identity. "
					   "Mapper left the old recovery untouched and did not open the provider "
					   "version. Free storage if necessary, then retry."));
				presented_document_token = 0;
				presents_document = false;
				AppleDocumentAccess::stopPresenting();
				pending_presented_document_events.clear();
				return false;
			}
			auto recent_files = Settings::getInstance()
			                    .getSettingCached(Settings::General_RecentFilesList)
			                    .toStringList();
			recent_files.removeAll(requested_document_path);
			Settings::getInstance().setSetting(
				Settings::General_RecentFilesList, recent_files);
		}
		document_path = coordinated_document_path;
	}

	if (!document_format)
		document_format = FileFormats.findFormatForFilename(
			document_path, &FileFormat::supportsFileOpen);
	if (!document_format)
		document_format = FileFormats.findFormatForData(
			provider_snapshot->fileName(), FileFormat::AllFiles);
#endif

	if (!document_format || !document_format->supportsReading())
	{
		QMessageBox::warning(this, tr("Error"),
		                     tr("Cannot open file:\n%1\n\n%2").
		                     arg(document_path, tr("Invalid file type.")));
#if defined(Q_OS_IOS)
		presented_document_token = 0;
		presents_document = false;
		AppleDocumentAccess::stopPresenting();
		pending_presented_document_events.clear();
#endif
		return false;
	}
	
	// Check a blocker that prevents immediate re-opening of crashing files.
	// Needed for stopping auto-loading a crashing file on startup.
	static const QString reopen_blocker = QString::fromLatin1("open_in_progress");
	QSettings settings;
	const QString open_in_progress(settings.value(reopen_blocker).toString());
	if (open_in_progress == document_path)
	{
		int result = QMessageBox::warning(this, tr("Crash warning"), 
		  tr("It seems that %1 crashed the last time this file was opened:<br />"
		     "<tt>%2</tt><br /><br />"
		     "Really retry to open it?")
		  .arg(appName(), document_path),
		  QMessageBox::Yes | QMessageBox::No);
		settings.remove(reopen_blocker);
		if (result == QMessageBox::No)
		{
#if defined(Q_OS_IOS)
			presented_document_token = 0;
			presents_document = false;
			AppleDocumentAccess::stopPresenting();
			pending_presented_document_events.clear();
#endif
			return false;
		}
	}
	
	settings.setValue(reopen_blocker, document_path);
	settings.sync();
	
	MainWindowController* const new_controller = MainWindowController::controllerForFile(
		document_path, document_format);
	if (!new_controller)
	{
		QMessageBox::warning(this, tr("Error"), tr("Cannot open file:\n%1\n\nFile format not recognized.").arg(document_path));
		settings.remove(reopen_blocker);
#if defined(Q_OS_IOS)
		presented_document_token = 0;
		presents_document = false;
		AppleDocumentAccess::stopPresenting();
		pending_presented_document_events.clear();
#endif
		return false;
	}
	
	QString new_actual_path = document_path;
	const FileFormat* new_actual_format = document_format;
	QString autosave_path = Autosave::autosavePath(document_path);
	bool new_autosave_conflict = QFileInfo::exists(autosave_path);
	if (new_autosave_conflict)
	{
#if defined(MAPPER_MOBILE)
		// Assuming small screen, showing dialog before opening the file
		AutosaveDialog* autosave_dialog = new AutosaveDialog(document_path, autosave_path, autosave_path, this);
		int result = autosave_dialog->exec();
		new_actual_path = (result == QDialog::Accepted) ? autosave_dialog->selectedPath() : QString();
		new_actual_format = (new_actual_path == document_path)
		                    ? document_format
		                    : FileFormats.findFormat(FileFormats.defaultFormat());
		delete autosave_dialog;
#else
		// Assuming large screen, dialog will be shown while the autosaved file is open
		new_actual_path = autosave_path;
		new_actual_format = FileFormats.findFormat(FileFormats.defaultFormat());
#endif
	}
	
#if defined(Q_OS_IOS)
	bool loaded = false;
	const bool loading_provider_snapshot =
		DocumentPath::canonical(new_actual_path) == document_path;
	const bool loading_private_recovery =
		!new_actual_path.isEmpty() && !loading_provider_snapshot;
	if (loading_provider_snapshot)
	{
		QFile snapshot_source{provider_snapshot->fileName()};
		loaded = snapshot_source.open(QIODevice::ReadOnly)
		         && new_controller->loadFrom(
			         document_path, *new_actual_format, this, &snapshot_source);
	}
	else if (loading_private_recovery)
	{
		// The autosave is a private byte source, not the document identity.
		// Keep the provider path as the importer's logical base so relative
		// template references resolve exactly as they did in the original map.
		QFile recovery_source{new_actual_path};
		loaded = recovery_source.open(QIODevice::ReadOnly)
		         && new_controller->loadFrom(
			         document_path, *new_actual_format, this, &recovery_source);
	}
#else
	const bool loaded = !new_actual_path.isEmpty()
	                    && new_controller->loadFrom(
		                    new_actual_path, *new_actual_format, this);
#endif
	if (!loaded)
	{
		delete new_controller;
		settings.remove(reopen_blocker);
#if defined(Q_OS_IOS)
		presented_document_token = 0;
		presents_document = false;
		AppleDocumentAccess::stopPresenting();
		pending_presented_document_events.clear();
#endif
		return false;
	}

#if defined(Q_OS_IOS)
	if (loading_provider_snapshot && new_autosave_conflict)
	{
		// The mobile recovery chooser is the user's resolution decision. Once
		// the provider version has loaded successfully, retire the rejected
		// recovery generation so new edits can immediately establish a fresh one.
		if (QFileInfo::exists(autosave_path) && !QFile::remove(autosave_path))
		{
			qWarning("Could not remove the rejected iOS recovery document: %s",
			         qUtf8Printable(autosave_path));
		}
		new_autosave_conflict = false;
	}
#endif
	
	MainWindow* open_window = this;
#if !defined(MAPPER_MOBILE)
	if (has_opened_file)
		open_window = new MainWindow();
#endif
	
	auto const ignore_touch = Settings::getInstance().getSetting(Settings::MapEditor_IgnoreTouchInput).toBool();
	open_window->warnAndSetIgnoreTouch(ignore_touch);
	
	open_window->setController(new_controller, document_path, document_format);
	open_window->actual_path = new_actual_path;
	open_window->setHasAutosaveConflict(new_autosave_conflict);
#if defined(Q_OS_IOS)
	open_window->restoreAuxiliaryDraftRecovery();
#endif
	open_window->setHasUnsavedChanges(
#if defined(Q_OS_IOS)
		loading_private_recovery
#else
		false
#endif
	);
#if defined(Q_OS_IOS)
	open_window->external_change_pending = false;
	open_window->presented_document_deleted = false;
	open_window->presented_document_snapshot = std::move(provider_snapshot);
#endif
	
	open_window->setVisible(true); // Respect the window flags set by new_controller.
	open_window->raise();
	num_open_files++;
	settings.remove(reopen_blocker);
	setMostRecentlyUsedFile(document_path);
	
#if !defined(MAPPER_MOBILE)
	// Assuming large screen. Android handled above.
	if (new_autosave_conflict)
	{
		auto autosave_dialog = new AutosaveDialog(document_path, autosave_path, new_actual_path, open_window, Qt::WindowTitleHint | Qt::CustomizeWindowHint);
		autosave_dialog->move(open_window->rect().right() - autosave_dialog->width(), open_window->rect().top());
		autosave_dialog->show();
		autosave_dialog->raise();
		
		connect(autosave_dialog, &AutosaveDialog::pathSelected, open_window, &MainWindow::switchActualPath);
		connect(open_window, &MainWindow::actualPathChanged, autosave_dialog, &AutosaveDialog::setSelectedPath);
		connect(open_window, &MainWindow::autosaveConflictResolved, autosave_dialog, &AutosaveDialog::autosaveConflictResolved);
	}
#endif
	
	open_window->activateWindow();

	return true;
}

void MainWindow::switchActualPath(const QString& path)
{
	if (path == actual_path)
	{
		return;
	}
	
	int ret = QMessageBox::Ok;
	if (has_unsaved_changes)
	{
		ret = QMessageBox::warning(this, appName(),
		                           tr("The file has been modified.\n"
		                              "Do you want to discard your changes?"),
		                           QMessageBox::Discard | QMessageBox::Cancel);
	}
	
	if (ret != QMessageBox::Cancel)
	{
		const QString& current_path = currentPath();
		auto format = (path == current_path) ? current_format : FileFormats.findFormat(FileFormats.defaultFormat());
		MainWindowController* const new_controller = MainWindowController::controllerForFile(current_path, format);
		if (new_controller && new_controller->loadFrom(path, *format, this))
		{
			setController(new_controller, current_path, format);
			actual_path = path;
			setHasUnsavedChanges(false);
		}
	}
	
	emit actualPathChanged(actual_path);
	activateWindow();
}

void MainWindow::openPathLater(const QString& path)
{
	path_backlog.push_back(path);
	QTimer::singleShot(10, this, &MainWindow::openPathBacklog);
}

void MainWindow::openExternalPath(const QString& path)
{
	if (path.isEmpty())
		return;

#if defined(MAPPER_MOBILE)
	if (!hasOpenedFile())
	{
		openPathLater(path);
		return;
	}

#if defined(Q_OS_IOS)
	if (DocumentPath::canonical(currentPath()) != DocumentPath::canonical(path))
	{
		QPointer<MainWindow> window{this};
		QTimer::singleShot(0, this, [window, path] {
			if (!window)
				return;
			if (window->provider_document_transaction_active
			    || window->close_in_progress
			    || QApplication::activeModalWidget())
			{
				QTimer::singleShot(50, window, [window, path] {
					if (window)
						window->openExternalPath(path);
				});
				return;
			}
			if (window->hasOpenedFile() && !window->closeFile())
				return;
			window->openPathLater(path);
		});
	}
	else
	{
		show();
		raise();
		activateWindow();
	}
#else
	if (DocumentPath::canonical(currentPath()) != DocumentPath::canonical(path))
	{
		showStatusBarMessage(
			tr("Close the current file before opening another document."), 5000);
	}
	else
	{
		show();
		raise();
		activateWindow();
	}
#endif
#else
	openPathLater(path);
#endif
}

#if defined(Q_OS_IOS)
MainWindow* MainWindow::nativeDocumentInteractionOwner(QWidget* widget)
{
	for (auto* candidate = widget; candidate; candidate = candidate->parentWidget())
	{
		auto* window = qobject_cast<MainWindow*>(candidate);
		if (window && window->create_menu)
			return window;
	}
	return nullptr;
}

void MainWindow::deferPrivateDraftCleanup(
	const QString& path, const QString& resource_identity)
{
	if (!AppleDocumentAccess::isPrivateAuxiliaryDraft(path))
		return;
	const auto identity = QFileInfo{path}.absoluteFilePath();
	const auto already_deferred = std::any_of(
		deferred_private_draft_cleanup.cbegin(),
		deferred_private_draft_cleanup.cend(),
		[&](const DeferredPrivateDraftCleanup& item) {
			return item.path == identity
			       && item.resource_identity == resource_identity;
		});
	if (!already_deferred)
	{
		deferred_private_draft_cleanup.push_back(
			{identity, resource_identity});
	}
}

bool MainWindow::beginNativeDocumentInteraction(
	NativeDocumentCheckpoint checkpoint)
{
	if (provider_document_transaction_active)
		return false;
	provider_document_transaction_active = true;
	native_interaction_gps_controller.clear();
	resume_gps_after_native_interaction = false;
	if (checkpoint != NativeDocumentCheckpoint::Skip)
	{
		native_interaction_gps_controller =
			qobject_cast<MapEditorController*>(controller);
		resume_gps_after_native_interaction =
			native_interaction_gps_controller
			&& native_interaction_gps_controller->isGPSDisplayEnabled();
		if (native_interaction_gps_controller)
			native_interaction_gps_controller->enableGPSDisplay(false);
	}
	if (checkpoint != NativeDocumentCheckpoint::Skip
	    && has_opened_file
	    && has_unsaved_changes)
	{
		const bool checkpoint_ready = canAdvancePrivateRecovery()
		                              && persistRecoverySnapshot();
		if (!checkpoint_ready)
		{
			showStatusBarMessage(
				tr("Mapper could not make a current recovery checkpoint before "
				   "opening Files. Save before leaving the app."),
				6000);
			if (checkpoint == NativeDocumentCheckpoint::Required)
			{
				endNativeDocumentInteraction();
				return false;
			}
		}
	}
	return true;
}

void MainWindow::endNativeDocumentInteraction()
{
	Q_ASSERT(provider_document_transaction_active);
	replayPendingPresentedDocumentEvents();
	provider_document_transaction_active = false;
	if (native_interaction_gps_controller
	    && native_interaction_gps_controller == controller
	    && resume_gps_after_native_interaction)
	{
		native_interaction_gps_controller->enableGPSDisplay(true);
	}
	native_interaction_gps_controller.clear();
	resume_gps_after_native_interaction = false;
	if (!path_backlog.empty())
		QTimer::singleShot(0, this, &MainWindow::openPathBacklog);
}

bool MainWindow::stopPresentingForClose()
{
	if (provider_document_transaction_active)
		return false;
	QPointer<MapEditorController> editor{
		qobject_cast<MapEditorController*>(controller)};
	const bool gps_was_enabled = editor && editor->isGPSDisplayEnabled();
	if (editor)
		editor->enableGPSDisplay(false);
	auto gps_restore_guard = qScopeGuard([editor, gps_was_enabled] {
		if (editor && gps_was_enabled)
			editor->enableGPSDisplay(true);
	});
	const auto closing_revision = controller ? controller->saveRevision() : 0;
	const bool closing_external_dirty =
		controller && controller->hasDirtyExternalResources();

	provider_document_transaction_active = true;
	if (presents_document)
	{
		presented_document_token = 0;
		AppleDocumentAccess::stopPresenting();
	}
	provider_document_transaction_active = false;
	presents_document = false;
	external_change_pending = false;
	presented_document_deleted = false;
	presented_document_change_generation = 0;
	pending_presented_document_events.clear();

	const bool changed_during_close =
		controller
		&& (controller->saveRevision() != closing_revision
		    || controller->hasDirtyExternalResources()
		       != closing_external_dirty);
	if (changed_during_close
	    && !persistRecoverySnapshot())
	{
		presented_document_content_dirty = true;
		setHasUnsavedChanges(true);
		QMessageBox::warning(
			this,
			tr("Close postponed"),
			tr("The map changed while its provider document was closing, and "
			   "Mapper could not finish a private recovery copy. Use Save As "
			   "before closing."));
		return false;
	}

	gps_restore_guard.dismiss();
	presented_document_content_dirty = false;
	external_resources_dirty = false;
	presented_document_snapshot.reset();
	return true;
}

AppleDocumentAccess::ChangeHandler MainWindow::makePresentedDocumentChangeHandler()
{
	QPointer<MainWindow> window{this};
	return [window](const AppleDocumentAccess::PresentedDocumentEvent& event) {
		// NSFilePresenter callbacks run on a private operation queue. The event
		// value is copied before crossing to Qt's GUI thread; QPointer is only
		// dereferenced by the context-bound queued invocation.
		QMetaObject::invokeMethod(
			qApp,
			[window, event] {
				if (window)
					window->handlePresentedDocumentChange(event);
			},
			Qt::QueuedConnection);
	};
}

void MainWindow::handlePresentedDocumentChange(
	const AppleDocumentAccess::PresentedDocumentEvent& event)
{
	// A replacement UIDocument may become active inside a native transaction
	// before the GUI installs its new presentation token. Preserve every event
	// until the transaction finishes, then apply the normal token/path filters.
	if (provider_document_transaction_active
	    && !replaying_presented_document_events)
	{
		pending_presented_document_events.push_back(event);
		return;
	}
	if (!event.presentation_token
	    || event.presentation_token != presented_document_token)
		return;
	if (!hasOpenedFile())
	{
		pending_presented_document_events.push_back(event);
		return;
	}

	const auto previous_path = DocumentPath::canonical(event.previous_path);
	if (!previous_path.isEmpty() && previous_path != currentPath())
		return;

	switch (event.change)
	{
	case AppleDocumentAccess::PresentedDocumentChange::Changed:
	{
		if (DocumentPath::canonical(event.path) != currentPath())
			return;
		++presented_document_change_generation;
		if (!event.error.isEmpty())
			showStatusBarMessage(event.error, 0);
		const bool already_pending = external_change_pending;
		external_change_pending = true;
		if (!already_pending
		    && QGuiApplication::applicationState() == Qt::ApplicationActive)
			QTimer::singleShot(0, this, &MainWindow::processPresentedDocumentChange);
		break;
	}

	case AppleDocumentAccess::PresentedDocumentChange::Moved:
		{
			++presented_document_change_generation;
			const auto old_path = currentPath();
			const auto new_path = DocumentPath::canonical(event.path);
			if (new_path.isEmpty())
				return;
			const auto* format = currentFormat();
			const bool using_private_recovery =
				DocumentPath::canonical(actual_path)
				== DocumentPath::canonical(autosavePath(old_path));
			if (actual_path == old_path)
				actual_path = new_path;
			bool recovery_migrated =
				migrateRecoveryForDocumentMove(old_path, new_path);
			setCurrentFile(new_path, format);
			if (using_private_recovery)
			{
				if (!recovery_migrated)
					recovery_migrated = persistRecoverySnapshot();
				if (recovery_migrated)
					actual_path = autosavePath(new_path);
			}
			auto recent_files = Settings::getInstance()
				.getSettingCached(Settings::General_RecentFilesList)
				.toStringList();
			recent_files.removeAll(old_path);
			Settings::getInstance().setSetting(
				Settings::General_RecentFilesList, recent_files);
			setMostRecentlyUsedFile(new_path);
			if (!event.error.isEmpty())
			{
				const auto detail = event.error;
				QTimer::singleShot(0, this, [this, detail] {
					QMessageBox::warning(
						this, tr("Document access changed"), detail);
				});
			}
			showStatusBarMessage(tr("The document moved to %1.")
			                     .arg(DocumentPath::displayName(new_path)), 5000);
			if (!recovery_migrated)
			{
				showStatusBarMessage(
					tr("The document moved, but Mapper could not move every private "
					   "template recovery copy. Save the template or map before closing."),
					0);
			}
		}
		break;

	case AppleDocumentAccess::PresentedDocumentChange::Deleted:
	{
		QScopedValueRollback<bool> transaction_guard{
			provider_document_transaction_active, true};
		++presented_document_change_generation;
		presented_document_token = 0;
		AppleDocumentAccess::stopPresenting();
		presents_document = false;
		external_change_pending = false;
		pending_presented_document_events.clear();
		presented_document_deleted = true;
		setHasUnsavedChanges(true);
		// Preserve the logical identity so private autosave remains available.
		// save() forces Save As while this tombstone state is active.
		const auto recovery_result = persistRecoverySnapshot()
		                             ? Autosave::Success
		                             : Autosave::PermanentFailure;
		QString recovery_detail;
		switch (recovery_result)
		{
		case Autosave::Success:
			recovery_detail = tr("Mapper kept a current private recovery copy; use Save As to keep it.");
			break;
		case Autosave::TemporaryFailure:
			recovery_detail = tr("Mapper could not update its recovery copy while an edit is in progress. "
			                     "Finish the edit and use Save As immediately.");
			break;
		case Autosave::PermanentFailure:
		default:
			recovery_detail = QFileInfo::exists(autosavePath(currentPath()))
			                  ? tr("Mapper could not update the existing recovery copy; it may be stale. "
			                       "Use Save As immediately.")
			                  : tr("Mapper could not create a recovery copy. Keep this screen open and "
			                       "use Save As immediately.");
			break;
		}
		QTimer::singleShot(0, this, [this, recovery_detail] {
			QMessageBox::warning(
				this,
				tr("Document removed"),
				tr("The open document was removed by its file provider. ")
				+ recovery_detail);
		});
		break;
	}
	}
}

void MainWindow::replayPendingPresentedDocumentEvents()
{
	if (replaying_presented_document_events)
		return;
	QScopedValueRollback<bool> replay_guard{
		replaying_presented_document_events, true};
	while (!pending_presented_document_events.empty())
	{
		const auto pending_events =
			std::exchange(pending_presented_document_events, {});
		for (const auto& event : pending_events)
			handlePresentedDocumentChange(event);
	}
}

bool MainWindow::migrateRecoveryForDocumentMove(
	const QString& old_path, const QString& new_path)
{
	if (old_path.isEmpty() || new_path.isEmpty() || old_path == new_path)
		return true;
	QString auxiliary_error;
	const bool auxiliary_migrated =
		AppleDocumentAccess::migratePrivateAuxiliaryRecovery(
			old_path, new_path, &auxiliary_error);
	if (!auxiliary_migrated && !auxiliary_error.isEmpty())
	{
		qWarning("Could not migrate private iOS template recovery: %s",
		         qUtf8Printable(auxiliary_error));
	}
	const auto old_autosave = Autosave::autosavePath(old_path);
	const auto new_autosave = Autosave::autosavePath(new_path);
	if (old_autosave == new_autosave || !QFileInfo::exists(old_autosave))
		return auxiliary_migrated;

	QDir{}.mkpath(QFileInfo(new_autosave).absolutePath());
	QFile source{old_autosave};
	QSaveFile destination{new_autosave};
	bool autosave_migrated = source.open(QIODevice::ReadOnly)
	                         && destination.open(QIODevice::WriteOnly);
	QByteArray buffer(64 * 1024, Qt::Uninitialized);
	while (autosave_migrated && !source.atEnd())
	{
		const auto bytes_read = source.read(buffer.data(), buffer.size());
		autosave_migrated = bytes_read >= 0
		                    && destination.write(buffer.constData(), bytes_read)
		                       == bytes_read;
	}
	if (autosave_migrated)
		autosave_migrated = destination.commit();
	else
		destination.cancelWriting();
	if (autosave_migrated)
		QFile::remove(old_autosave);
	return auxiliary_migrated && autosave_migrated;
}

void MainWindow::processPresentedDocumentChange()
{
	if (provider_document_transaction_active
	    || close_in_progress
	    || QApplication::activeModalWidget())
	{
		QTimer::singleShot(
			50, this, &MainWindow::processPresentedDocumentChange);
		return;
	}
	if (!external_change_pending || !hasOpenedFile()
	    || QGuiApplication::applicationState() != Qt::ApplicationActive)
	{
		return;
	}

	const bool protected_local_state = hasUnsavedChanges()
	                                   || has_autosave_conflict
	                                   || DocumentPath::canonical(actual_path)
	                                      != currentPath();
	auto* const decision_controller = controller;
	const auto decision_token = presented_document_token;
	const auto decision_path = currentPath();
	const auto decision_generation = presented_document_change_generation;
	if (protected_local_state)
	{
		const auto choice = QMessageBox::warning(
			this,
			tr("Document changed"),
			tr("This document changed in another app. Reloading it will discard "
			   "your unsaved Mapper changes or selected recovery copy."),
			QMessageBox::Discard | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (choice != QMessageBox::Discard)
		{
			showStatusBarMessage(
				tr("The open document has newer external changes."), 0);
			return;
		}
		if (controller != decision_controller
		    || presented_document_token != decision_token
		    || currentPath() != decision_path
		    || presented_document_change_generation != decision_generation)
		{
			return;
		}
	}

	reloadPresentedDocument(protected_local_state);
}

bool MainWindow::reloadPresentedDocument(bool discard_local_recovery)
{
	if (!beginNativeDocumentInteraction(NativeDocumentCheckpoint::Required))
		return false;
	bool interaction_active = true;
	auto interaction_guard = qScopeGuard(
		[this, &interaction_active] {
			if (interaction_active)
				endNativeDocumentInteraction();
		});
	const auto path = currentPath();
	const auto* format = currentFormat();
	if (path.isEmpty() || !format)
		return false;
	auto* const source_controller = controller;
	const auto source_revision = source_controller->saveRevision();
	const bool source_external_dirty =
		source_controller->hasDirtyExternalResources();
	const auto transaction_token = presented_document_token;
	const auto transaction_generation = presented_document_change_generation;

	std::unique_ptr<MainWindowController> new_controller{
		MainWindowController::controllerForFile(path, format)};
	if (!new_controller)
		return false;

	auto snapshot = std::make_unique<QTemporaryFile>(
		QDir::tempPath() + QLatin1String("/Mapper-reload-XXXXXX"));
	if (!snapshot->open())
	{
		QMessageBox::warning(this, tr("Error"), snapshot->errorString());
		return false;
	}
	snapshot->close();

	QString coordination_error;
	QString coordinated_path;
	bool loaded = false;
	const bool copied = AppleDocumentAccess::readPresentedDocument(
		path, snapshot->fileName(), &coordinated_path, &coordination_error);
	QFile snapshot_source{snapshot->fileName()};
	loaded = copied
	         && snapshot_source.open(QIODevice::ReadOnly)
	         && new_controller->loadFrom(
		         path, *format, this, &snapshot_source);
	replayPendingPresentedDocumentEvents();
	if (controller != source_controller
	    || source_controller->saveRevision() != source_revision
	    || source_controller->hasDirtyExternalResources()
	       != source_external_dirty)
	{
		const bool recovery_current =
			controller == source_controller && persistRecoverySnapshot();
		QMessageBox::warning(
			this,
			tr("Reload postponed"),
			recovery_current
				? tr("The map changed while Mapper was reading the provider's version. "
				     "Mapper kept the current map and updated its private recovery copy.")
				: tr("The map changed while Mapper was reading the provider's version. "
				     "Mapper kept the current map, but its recovery copy could not be "
				     "updated; save it before leaving the app."));
		return false;
	}
	if (!loaded)
	{
		if (!coordination_error.isEmpty())
			QMessageBox::warning(this, tr("Error"), coordination_error);
		return false;
	}

	const auto effective_path = coordinated_path.isEmpty()
	                            ? path
	                            : DocumentPath::canonical(coordinated_path);
	if (presented_document_token != transaction_token
	    || !presents_document
	    || presented_document_deleted
	    || presented_document_change_generation != transaction_generation
	    || currentPath() != path)
	{
		showStatusBarMessage(
			tr("The provider changed the document again while it was reloading. "
			   "Review the new change before retrying."), 0);
		return false;
	}
	if (discard_local_recovery)
	{
		discardAuxiliaryDraftRecovery();
		clearDeferredPrivateDraftCleanup();
	}
	if (effective_path != path)
		migrateRecoveryForDocumentMove(path, effective_path);
	setController(new_controller.release(), effective_path, format);
	actual_path = effective_path;
	presented_document_snapshot = std::move(snapshot);
	setHasAutosaveConflict(false);
	setHasUnsavedChanges(false);
	removeAutosaveFile();
	external_change_pending =
		AppleDocumentAccess::hasPresentedDocumentConflict();
	showStatusBarMessage(
		external_change_pending
		? tr("Reloaded the provider's current version. Conflicting alternatives remain; save to keep this version.")
		: tr("Reloaded external document changes."),
		external_change_pending ? 0 : 3000);
	return true;
}

bool MainWindow::persistAuxiliaryDraftRecovery()
{
	auto* editor = qobject_cast<MapEditorController*>(controller);
	if (!editor || currentPath().isEmpty())
		return true;
	auto* map = editor->getMap();
	bool complete = true;
	for (int index = 0; index < map->getNumTemplates(); ++index)
	{
		auto* temp = map->getTemplate(index);
		const auto template_path = temp->getTemplatePath();
		if (template_path.isEmpty())
			continue;
		if (!temp->hasUnsavedChanges())
			continue;
		const auto recovery_path =
			AppleDocumentAccess::privateAuxiliaryRecoveryPath(
				currentPath(), temp->resourceIdentity(), template_path);
		if (recovery_path.isEmpty() || !temp->writeTemplateFile(recovery_path))
		{
			complete = false;
			qWarning("Could not persist private iOS template recovery for %s",
			         qUtf8Printable(temp->getTemplateFilename()));
		}
	}
	return complete;
}

bool MainWindow::persistRecoverySnapshot()
{
	if (!controller || currentPath().isEmpty()
	    || controller->isEditingInProgress())
	{
		return false;
	}
	const bool auxiliary_saved = persistAuxiliaryDraftRecovery();
	const bool map_saved = persistMapRecoverySnapshot();
	return auxiliary_saved && map_saved;
}

void MainWindow::discardAuxiliaryDraftRecovery()
{
	auto* editor = qobject_cast<MapEditorController*>(controller);
	if (!editor || currentPath().isEmpty())
		return;
	auto* map = editor->getMap();
	for (int index = 0; index < map->getNumTemplates(); ++index)
	{
		auto* temp = map->getTemplate(index);
		AppleDocumentAccess::discardPrivateAuxiliaryRecovery(
			currentPath(),
			temp->resourceIdentity(),
			temp->getTemplatePath());
	}
}

void MainWindow::commitDeferredPrivateDraftCleanup()
{
	auto* editor = qobject_cast<MapEditorController*>(controller);
	auto* map = editor ? editor->getMap() : nullptr;
	const auto is_still_referenced = [map](const QString& path) {
		if (!map)
			return true;
		const auto identity = QFileInfo{path}.absoluteFilePath();
		for (int index = 0; index < map->getNumTemplates(); ++index)
		{
			if (QFileInfo{map->getTemplate(index)->getTemplatePath()}
			        .absoluteFilePath() == identity)
			{
				return true;
			}
		}
		for (int index = 0; index < map->getNumClosedTemplates(); ++index)
		{
			if (QFileInfo{map->getClosedTemplate(index)->getTemplatePath()}
			        .absoluteFilePath() == identity)
			{
				return true;
			}
		}
		return false;
	};
	const auto is_resource_still_referenced =
		[map](const QString& resource_identity) {
			if (!map || resource_identity.isEmpty())
				return false;
			for (int index = 0; index < map->getNumTemplates(); ++index)
			{
				if (map->getTemplate(index)->resourceIdentity()
				    == resource_identity)
				{
					return true;
				}
			}
			for (int index = 0; index < map->getNumClosedTemplates(); ++index)
			{
				if (map->getClosedTemplate(index)->resourceIdentity()
				    == resource_identity)
				{
					return true;
				}
			}
			return false;
		};
	for (const auto& item : std::as_const(deferred_private_draft_cleanup))
	{
		if (!is_still_referenced(item.path))
		{
			if (!is_resource_still_referenced(item.resource_identity))
			{
				AppleDocumentAccess::discardPrivateAuxiliaryRecovery(
					currentPath(), item.resource_identity, item.path);
			}
			QFile::remove(item.path);
		}
	}
	deferred_private_draft_cleanup.clear();
}

void MainWindow::clearDeferredPrivateDraftCleanup()
{
	deferred_private_draft_cleanup.clear();
}

bool MainWindow::persistMapRecoverySnapshot()
{
	const auto* format = FileFormats.findFormat(FileFormats.defaultFormat());
	if (!controller || !format || currentPath().isEmpty())
		return false;
	const auto recovery_path = autosavePath(currentPath());
	if (!QDir{}.mkpath(QFileInfo{recovery_path}.absolutePath()))
		return false;
	QSaveFile recovery_file{recovery_path};
	quint64 staged_revision = 0;
	const auto logical_path = format->fixupExtension(currentPath());
	return recovery_file.open(QIODevice::WriteOnly)
	       && controller->stageSaveTo(
		       logical_path, *format, &recovery_file, &staged_revision)
	       && recovery_file.commit()
	       && controller->saveRevision() == staged_revision;
}

void MainWindow::restoreAuxiliaryDraftRecovery()
{
	auto* editor = qobject_cast<MapEditorController*>(controller);
	if (!editor || currentPath().isEmpty())
		return;
	auto* map = editor->getMap();
	for (int index = 0; index < map->getNumTemplates(); ++index)
	{
		auto* temp = map->getTemplate(index);
		const auto template_path = temp->getTemplatePath();
		const auto recovery_path =
			AppleDocumentAccess::privateAuxiliaryRecoveryPath(
				currentPath(), temp->resourceIdentity(), template_path);
		if (recovery_path.isEmpty() || !QFileInfo::exists(recovery_path))
			continue;

		QMessageBox prompt{
			QMessageBox::Warning,
			tr("Recover template changes?"),
			tr("Mapper found private unsaved changes for template “%1”.")
				.arg(temp->getTemplateFilename()),
			QMessageBox::NoButton,
			this};
		auto* restore_button = prompt.addButton(
			tr("Restore Draft"), QMessageBox::AcceptRole);
		auto* discard_button = prompt.addButton(
			tr("Discard Draft"), QMessageBox::DestructiveRole);
		prompt.addButton(QMessageBox::Cancel);
		prompt.exec();
		if (prompt.clickedButton() == restore_button)
		{
			if (!temp->recoverFromPrivateSnapshot(recovery_path))
			{
				QMessageBox::warning(
					this,
					tr("Template recovery failed"),
					tr("Mapper could not load the private template recovery copy."));
			}
		}
		else if (prompt.clickedButton() == discard_button)
		{
			AppleDocumentAccess::discardPrivateAuxiliaryRecovery(
				currentPath(), temp->resourceIdentity(), template_path);
		}
	}
}
#endif

void MainWindow::openPathBacklog()
{
	if (path_backlog.empty() || path_backlog_busy)
		return;
#if defined(Q_OS_IOS)
	if (provider_document_transaction_active
	    || close_in_progress
	    || QApplication::activeModalWidget())
	{
		QTimer::singleShot(10, this, &MainWindow::openPathBacklog);
		return;
	}
#endif

	QScopedValueRollback<bool> rollback{path_backlog_busy, true};
	openPath(path_backlog.takeFirst());
	QTimer::singleShot(10, this, &MainWindow::openPathBacklog);
}

void MainWindow::openRecentFile()
{
	if (auto action = qobject_cast<QAction*>(sender()))
	{
#ifdef MAPPER_MOBILE
		if (hasOpenedFile() && !closeFile())
			return;
#endif
		openPath(action->data().toString());
	}
}

void MainWindow::updateRecentFileActions()
{
	if (! create_menu)
		return;

	QStringList files = Settings::getInstance().getSettingCached(Settings::General_RecentFilesList).toStringList();

	int num_recent_files = qMin(files.size(), max_recent_files);

	open_recent_menu->clear();
	for (int i = 0; i < num_recent_files; ++i) {
		QString text = tr("&%1 %2").arg(i + 1).arg(DocumentPath::displayName(files[i]));
		recent_file_act[i]->setText(text);
		recent_file_act[i]->setData(files[i]);
		open_recent_menu->addAction(recent_file_act[i]);
	}

	if (num_recent_files > 0 && !open_recent_menu_inserted)
		file_menu->insertMenu(save_act, open_recent_menu);
	else if (!(num_recent_files > 0) && open_recent_menu_inserted)
		file_menu->removeAction(open_recent_menu->menuAction());
	open_recent_menu_inserted = num_recent_files > 0;
}

void MainWindow::setHasAutosaveConflict(bool value)
{
	if (has_autosave_conflict != value)
	{
		has_autosave_conflict = value;
#if defined(Q_OS_IOS)
		setAutosaveNeeded(has_unsaved_changes
		                  && canAdvancePrivateRecovery());
#else
		setAutosaveNeeded(has_unsaved_changes && !has_autosave_conflict);
#endif
		if (!has_autosave_conflict)
			emit autosaveConflictResolved();
	}
}

#if defined(Q_OS_IOS)
bool MainWindow::canAdvancePrivateRecovery() const
{
	return !has_autosave_conflict
	       || (!currentPath().isEmpty()
	           && DocumentPath::canonical(actual_path)
	              == DocumentPath::canonical(autosavePath(currentPath())));
}
#endif

bool MainWindow::removeAutosaveFile() const
{
	if (!currentPath().isEmpty() && !has_autosave_conflict)
	{
		QFile autosave_file(autosavePath(currentPath()));
		return !autosave_file.exists() || autosave_file.remove();
	}
	return false;
}

Autosave::AutosaveResult MainWindow::autosave()
{
	if (map_hub_read_only)
		return Autosave::Success;

	QString path = currentPath();
	auto autosave_format = FileFormats.findFormat(FileFormats.defaultFormat());
	if (path.isEmpty() || !controller || !autosave_format)
	{
		return Autosave::PermanentFailure;
	}
#if defined(Q_OS_IOS)
	else if (provider_document_transaction_active)
	{
		// Native document pickers and coordinated provider operations keep the
		// Qt event dispatcher alive. Do not enter a second serialization while
		// one of those transactions owns an immutable save receipt.
		return Autosave::TemporaryFailure;
	}
#endif
	else if (controller->isEditingInProgress())
	{
		return Autosave::TemporaryFailure;
	}
	else
	{
#if defined(Q_OS_IOS)
		if (!beginNativeDocumentInteraction(NativeDocumentCheckpoint::Skip))
			return Autosave::TemporaryFailure;
		auto interaction_guard = qScopeGuard(
			[this] { endNativeDocumentInteraction(); });
#endif
		showStatusBarMessageImmediately(tr("Autosaving..."), 0);
#if defined(Q_OS_IOS)
		const bool saved = persistRecoverySnapshot();
#else
		const bool saved = controller->exportTo(
			autosavePath(currentPath()), *autosave_format);
#endif
		if (saved)
		{
			// Success
			clearStatusBarMessage();
			return Autosave::Success;
		}
		else
		{
			// Failure
			showStatusBarMessage(tr("Autosaving failed!"), 6000);
			return Autosave::PermanentFailure;
		}
	}
}

bool MainWindow::save()
{
	if (map_hub_read_only)
	{
		QMessageBox::information(
		  this, tr("Read-only Map Hub map"),
		  tr("Request editing access from Map Hub before changing or saving "
		     "this map."));
		return false;
	}

	auto path = currentPath();
	auto format = currentFormat();
	if (path.isEmpty()
	    || !format
	    || !format->supportsFileSave()
#if defined(Q_OS_IOS)
	    || presented_document_deleted
#endif
	   )
	{
		return showSaveAsDialog();
	}

	return saveTo(path, *currentFormat());
}

bool MainWindow::saveTo(const QString &path, const FileFormat& format)
{
	if (map_hub_read_only && !map_hub_local_copy_in_progress)
	{
		QMessageBox::information(
		  this, tr("Read-only Map Hub map"),
		  tr("Request editing access from Map Hub before changing or saving "
		     "this map."));
		return false;
	}

	if (!controller || path.isEmpty())
	{
		qWarning("Unexpected call to MainWindow::saveTo(PATH, FORMAT)");
		return false;
	}

	if (format.isWritingLossy())
	{
		auto message =
		        tr("This map is being saved as a \"%1\" file. "
		           "Information may be lost.\n\n"
		           "Press Yes to save in this format.\n"
		           "Press No to choose a different format.")
		        .arg(format.description());
		int result = QMessageBox::warning(this, tr("Warning"), message, QMessageBox::Yes, QMessageBox::No);
		if (result != QMessageBox::Yes)
			return showSaveAsDialog();
	}

#if defined(Q_OS_IOS)
	const auto destination_path = DocumentPath::canonical(path);
	if (!presents_document || presented_document_deleted
	    || destination_path != currentPath())
	{
		return showSaveAsDialog();
	}
	if (!beginNativeDocumentInteraction())
		return false;
	bool interaction_active = true;
	auto interaction_guard = qScopeGuard(
		[this, &interaction_active] {
			if (interaction_active)
				endNativeDocumentInteraction();
		});
	const auto conflict_resolution_token =
		AppleDocumentAccess::capturePresentedDocumentConflicts();
	const bool provider_conflict = conflict_resolution_token != 0;
	auto conflict_snapshot_guard = qScopeGuard([conflict_resolution_token] {
		AppleDocumentAccess::discardPresentedDocumentConflicts(
			conflict_resolution_token);
	});
	const auto transaction_token = presented_document_token;
	const auto transaction_path = currentPath();
	const auto transaction_generation = presented_document_change_generation;
	QByteArray provider_write_receipt;
	QString receipt_error;
	if (!AppleDocumentAccess::capturePresentedDocumentWriteReceipt(
		    destination_path,
		    external_change_pending || provider_conflict,
		    &provider_write_receipt,
		    &receipt_error))
	{
		QMessageBox::warning(
			this,
			tr("Document changed"),
			receipt_error.isEmpty()
			? tr("Mapper could not verify the provider version before saving.")
			: receipt_error);
		return false;
	}
	if (external_change_pending || provider_conflict)
	{
		const auto choice = QMessageBox::warning(
			this,
			tr("Overwrite external changes?"),
			provider_conflict
			? tr("This document has conflicting provider versions. Saving keeps "
			     "Mapper's version and resolves the alternatives.")
			: tr("This document changed in another app after Mapper loaded it. "
			     "Saving now will overwrite those external changes."),
			QMessageBox::Save | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (choice != QMessageBox::Save)
			return false;
	}
#endif

	bool saved = false;
#if defined(Q_OS_IOS)
	auto staged_document = std::make_unique<QTemporaryFile>(
		QDir::tempPath() + QLatin1String("/Mapper-save-XXXXXX.")
		+ format.primaryExtension());
	if (!staged_document->open())
	{
		QMessageBox::warning(
			this, tr("Error"),
			tr("Could not create a private save snapshot: %1")
			.arg(staged_document->errorString()));
		return false;
	}

	// Serializing a map may touch QWidget-owned controller state. Keep that on
	// Qt's GUI thread and leave the map dirty until the provider commit succeeds.
	quint64 staged_revision = 0;
	if (!controller->stageSaveTo(
			destination_path, format, staged_document.get(), &staged_revision)
	    || !staged_document->flush())
	{
		return false;
	}
	staged_document->close();
	replayPendingPresentedDocumentEvents();
	if (presented_document_token != transaction_token
	    || currentPath() != transaction_path
	    || presented_document_change_generation != transaction_generation)
	{
		QMessageBox::warning(
			this,
			tr("Document changed"),
			tr("The document changed while Mapper was preparing the save. "
			   "Review the provider change and save again."));
		return false;
	}

	QString coordination_error;
	QString coordinated_path;
	{
		QScopedValueRollback<bool> transaction_guard{
			provider_document_transaction_active, true};
		saved = AppleDocumentAccess::writePresentedDocument(
			destination_path,
			staged_document->fileName(),
			provider_write_receipt,
			conflict_resolution_token,
			&coordinated_path,
			&coordination_error);
	}
	replayPendingPresentedDocumentEvents();
	if (!coordination_error.isEmpty())
		QMessageBox::warning(this, tr("Error"), coordination_error);
#else
	saved = controller->saveTo(path, format);
#endif
	if (!saved)
		return false;

#if defined(Q_OS_IOS)
	const auto committed_path = coordinated_path.isEmpty()
	                            ? DocumentPath::canonical(path)
	                            : DocumentPath::canonical(coordinated_path);
	const bool provider_stable =
		presented_document_token == transaction_token
		&& presents_document
		&& !presented_document_deleted
		&& presented_document_change_generation == transaction_generation
		&& currentPath() == transaction_path
		&& committed_path == transaction_path;
	if (!provider_stable)
	{
		if (!presented_document_deleted && !committed_path.isEmpty()
		    && committed_path != currentPath())
		{
			const auto old_path = currentPath();
			migrateRecoveryForDocumentMove(old_path, committed_path);
			setCurrentFile(committed_path, &format);
			actual_path = committed_path;
		}
		const bool recovery_current = persistRecoverySnapshot();
		QMessageBox::warning(
			this,
			tr("Document changed during save"),
			recovery_current
				? tr("The provider changed, moved, or removed the document while the "
				     "save was completing. Mapper kept the map marked as modified and "
				     "updated its recovery copy.")
				: tr("The provider changed, moved, or removed the document while the "
				     "save was completing. Mapper kept the map marked as modified, but "
				     "could not update its recovery copy."));
		return false;
	}
	if (controller->saveRevision() != staged_revision)
	{
		const bool recovery_current = persistRecoverySnapshot();
		QMessageBox::warning(
			this,
			tr("Map changed during save"),
			recovery_current
				? tr("The provider safely received the staged version, but the map was "
				     "edited while that save was completing. Mapper kept the newer map "
				     "marked as modified and updated its recovery copy.")
				: tr("The provider safely received the staged version, but the map was "
				     "edited while that save was completing. Mapper kept the newer map "
				     "marked as modified, but could not update its recovery copy."));
		return false;
	}
#else
	const auto committed_path = path;
#endif
	setMostRecentlyUsedFile(committed_path);

	if (committed_path != currentPath())
	{
		setCurrentFile(committed_path, &format);
	}

#if defined(Q_OS_IOS)
	const bool fully_clean = controller->markSaveCommitted(staged_revision, false);
#else
	const bool fully_clean = true;
#endif
#if defined(Q_OS_IOS)
	commitDeferredPrivateDraftCleanup();
	setUnsavedStateAfterDocumentCommit(!fully_clean);
	setHasAutosaveConflict(false);
	const bool external_recovery_current =
		fully_clean ? removeAutosaveFile() : persistRecoverySnapshot();
	external_change_pending = false;
	if (!fully_clean)
	{
		QMessageBox::warning(
			this,
			tr("External template not saved"),
			external_recovery_current
			? tr("The map was saved safely, but an edited external template still "
			     "needs its own authorized save or relink. Mapper kept the document "
			     "marked as modified and preserved a private recovery map.")
			: tr("The map was saved safely, but an edited external template still "
			     "needs its own authorized save or relink. Mapper kept the document "
			     "marked as modified, but could not update its private recovery map; "
			     "keep Mapper open and retry."));
	}
#else
	setHasAutosaveConflict(false);
	removeAutosaveFile();
	setHasUnsavedChanges(!fully_clean);
#endif
	if (fully_clean && map_hub_sync)
		map_hub_sync->savedExplicitly();

#if defined(Q_OS_IOS)
	return fully_clean;
#else
	return true;
#endif
}

// static
MainWindow::FileInfo MainWindow::getOpenFileName(QWidget* parent, const QString& title, FileFormat::FileTypes types)
{
	// Get the saved directory to start in, defaulting to the user's home directory.
	QSettings settings;
	QString open_directory = settings.value(QString::fromLatin1("openFileDirectory"), QDir::homePath()).toString();

	// Build the list of supported file filters based on the file format registry
	QString filters, extensions;

	if (types.testFlag(FileFormat::MapFile) || types.testFlag(FileFormat::OgrFile))
	{
		for (auto format : FileFormats.formats())
		{
			if (format->supportsFileOpen())
			{
				if (filters.isEmpty())
				{
					filters    = format->filter();
					extensions = QLatin1String("*.") + format->fileExtensions().join(QString::fromLatin1(" *."));
				}
				else
				{
					filters    = filters    + QLatin1String(";;")  + format->filter();
					extensions = extensions + QLatin1String(" *.") + format->fileExtensions().join(QString::fromLatin1(" *."));
				}
			}
		}
		filters = 
			tr("All maps")  + QLatin1String(" (") + extensions + QLatin1String(");;") +
			filters         + QLatin1String(";;");
	}
	
	filters += tr("All files") + QLatin1String(" (*.*)");
	
	QString filter; // will be set to the selected filter by QFileDialog
	QString path;
#if defined(Q_OS_IOS)
	auto* interaction_owner = nativeDocumentInteractionOwner(parent);
	if (!interaction_owner
	    || !interaction_owner->beginNativeDocumentInteraction(
		    NativeDocumentCheckpoint::Required))
	{
		return {QString{}, nullptr};
	}
	auto interaction_guard = qScopeGuard([interaction_owner] {
		interaction_owner->endNativeDocumentInteraction();
	});
	QString picker_error;
	const bool selected = AppleDocumentAccess::chooseDocumentToOpen(
		title, &path, &picker_error);
	if (!picker_error.isEmpty())
		QMessageBox::warning(parent, tr("Document access"), picker_error);
	if (!selected)
		path.clear();
#else
	path = FileDialog::getOpenFileName(
		parent, title, open_directory, filters, &filter);
#endif
	
	const FileFormat* format = nullptr;
	if (!path.isEmpty())
	{
		path = DocumentPath::canonical(path);
		format = FileFormats.findFormatByFilter(filter, &FileFormat::supportsFileOpen);
		if (!format)
		format = FileFormats.findFormatForFilename(path, &FileFormat::supportsFileOpen);
#if !defined(Q_OS_IOS)
		if (!format)
			format = FileFormats.findFormatForData(path, types);
#endif
	}
	return { path, format };
}



// static
void MainWindow::showMessageBox(QWidget* parent, const QString& title, const QString& headline, const std::vector<QString>& messages)
{
	QString document;
	if (!headline.isEmpty())
		document += QLatin1String("<p><b>") + headline + QLatin1String("</b></p>");
	for (const auto& message : messages)
		document += Qt::convertFromPlainText(message, Qt::WhiteSpaceNormal);
	
	TextBrowserDialog dialog(document, parent);
	dialog.setWindowTitle(title);
	dialog.setWindowModality(Qt::WindowModal);
	dialog.exec();
	// Let Android update the screen.
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100 /* ms */);
}



#if defined(Q_OS_IOS)
bool MainWindow::exportPresentedDocument(
	const FileFormat& format, const QString& suggested_name)
{
	if (!controller || !format.supportsFileSaveAs())
		return false;
	if (!beginNativeDocumentInteraction())
		return false;
	bool interaction_active = true;
	auto interaction_guard = qScopeGuard(
		[this, &interaction_active] {
			if (interaction_active)
				endNativeDocumentInteraction();
		});
	if (format.isWritingLossy())
	{
		const auto choice = QMessageBox::warning(
			this,
			tr("Warning"),
			tr("This map is being exported as a “%1” file. Information may be lost.")
				.arg(format.description()),
			QMessageBox::Save | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (choice != QMessageBox::Save)
			return false;
	}

	QTemporaryDir staging_directory{
		QDir::tempPath() + QLatin1String("/Mapper-export-XXXXXX")};
	if (!staging_directory.isValid())
	{
		QMessageBox::warning(
			this, tr("Error"), tr("Could not create a private export directory."));
		return false;
	}
	QString export_name = QFileInfo{suggested_name}.fileName();
	if (export_name.isEmpty())
		export_name = tr("Untitled map");
	export_name = format.fixupExtension(export_name);
	const auto provisional_path =
		QDir{staging_directory.path()}.filePath(export_name);
	const auto provisional_logical_path = currentPath().isEmpty()
	                                      ? provisional_path
	                                      : currentPath();
	QSaveFile provisional_file{provisional_path};
	quint64 provisional_revision = 0;
	if (!provisional_file.open(QIODevice::WriteOnly)
	    || !controller->stageSaveTo(
		    provisional_logical_path,
		    format,
		    &provisional_file,
		    &provisional_revision)
	    || !provisional_file.commit())
	{
		QMessageBox::warning(
			this,
			tr("Error"),
			tr("Mapper could not prepare a complete document for the Files exporter."));
		return false;
	}

	const auto transaction_generation = presented_document_change_generation;
	QString exported_path;
	QByteArray exported_fingerprint;
	QString coordinated_path;
	QString provider_error;
	quint64 replacement_token = 0;
	bool exported = false;
	bool corrected_snapshot_ready = false;
	bool adopted = false;
	quint64 staged_revision = 0;
	QString committed_path;
	bool recovery_identity_ready = true;
	bool failed_export_removed = false;
	struct PrivateDraftRebinding
	{
		Template* temp = nullptr;
		QString old_path;
		QString old_relative_path;
		QByteArray old_fingerprint;
		QString new_path;
	};
	std::vector<PrivateDraftRebinding> private_draft_rebindings;
	std::unique_ptr<QTemporaryDir> private_draft_fork;
	bool keep_private_draft_fork = false;
	const auto rollback_private_draft_fork = [&] {
		for (auto it = private_draft_rebindings.rbegin();
		     it != private_draft_rebindings.rend(); ++it)
		{
			it->temp->setTemplatePath(it->old_path);
			it->temp->setTemplateRelativePath(it->old_relative_path);
			it->temp->setAuxiliaryDocumentFingerprint(it->old_fingerprint);
		}
		private_draft_rebindings.clear();
		private_draft_fork.reset();
	};
	auto private_draft_rollback = qScopeGuard([&] {
		if (!keep_private_draft_fork)
			rollback_private_draft_fork();
	});
	showStatusBarMessageImmediately(tr("Waiting for the Files provider…"), 0);
	{
		QScopedValueRollback<bool> transaction_guard{
			provider_document_transaction_active, true};
		exported = AppleDocumentAccess::exportDocument(
			provisional_path,
			&exported_path,
			&exported_fingerprint,
			&provider_error);
		if (exported)
		{
			exported_path = DocumentPath::canonical(exported_path);
			const auto exported_extension = QFileInfo{exported_path}.suffix();
			if (!format.fileExtensions().contains(
				    exported_extension, Qt::CaseInsensitive))
			{
					AppleDocumentAccess::abandonExportedDocument();
					provider_error = tr(
						"Files created a copy whose .%1 extension does not match the selected %2 format. Mapper will remove that incomplete copy; retry with one of these extensions: .%3")
					.arg(
						exported_extension,
						format.description(),
						format.fileExtensions().join(QLatin1String(", .")));
				exported = false;
			}
			else
			{
				auto* editor = qobject_cast<MapEditorController*>(controller);
				auto* map = editor ? editor->getMap() : nullptr;
				int clone_index = 0;
				auto clone_private_draft = [&](Template* temp) {
					const auto source_path = QFileInfo{temp->getTemplatePath()}
					                         .absoluteFilePath();
					if (!AppleDocumentAccess::isPrivateAuxiliaryDraft(source_path))
						return true;
					if (!private_draft_fork)
					{
						const auto destination_root =
							AppleDocumentAccess::privateAuxiliaryDraftDirectory(
								exported_path);
						private_draft_fork = std::make_unique<QTemporaryDir>(
							QDir{destination_root}.filePath(
								QLatin1String("SaveAs-XXXXXX")));
						if (destination_root.isEmpty()
						    || !private_draft_fork->isValid())
						{
							provider_error = tr(
								"Mapper could not create independent template storage for the exported document.");
							return false;
						}
					}
					const auto instance_directory =
						QDir{private_draft_fork->path()}.filePath(
							QString::number(++clone_index));
					if (!QDir{}.mkpath(instance_directory))
					{
						provider_error = tr(
							"Mapper could not create independent storage for template “%1”.")
							.arg(temp->getTemplateFilename());
						return false;
					}
					auto leaf_name = QFileInfo{source_path}.fileName();
					if (leaf_name.isEmpty())
						leaf_name = QLatin1String("template-data");
					const auto destination_path =
						QDir{instance_directory}.filePath(leaf_name);
					const bool copied =
						temp->getTemplateState() == Template::Loaded
						&& temp->hasUnsavedChanges()
						? temp->writeTemplateFile(destination_path)
						: QFile::copy(source_path, destination_path);
					if (!copied)
					{
						provider_error = tr(
							"Mapper could not clone the private template “%1” for the exported document.")
							.arg(temp->getTemplateFilename());
						return false;
					}
					private_draft_rebindings.push_back({
						temp,
						temp->getTemplatePath(),
						temp->getTemplateRelativePath(),
						temp->auxiliaryDocumentFingerprint(),
						destination_path});
					temp->setTemplatePath(destination_path);
					temp->setTemplateRelativePath({});
					return true;
				};
				bool private_drafts_ready = map != nullptr;
				for (int index = 0;
				     private_drafts_ready && index < map->getNumTemplates();
				     ++index)
				{
					private_drafts_ready = clone_private_draft(
						map->getTemplate(index));
				}
				for (int index = 0;
				     private_drafts_ready && index < map->getNumClosedTemplates();
				     ++index)
				{
					private_drafts_ready = clone_private_draft(
						map->getClosedTemplate(index));
				}
				const auto corrected_path = QDir{staging_directory.path()}.filePath(
					QLatin1String("destination-") + export_name);
				QSaveFile corrected_file{corrected_path};
				corrected_snapshot_ready = private_drafts_ready
				                           && corrected_file.open(QIODevice::WriteOnly)
				                           && controller->stageSaveTo(
					                           exported_path,
					                           format,
					                           &corrected_file,
					                           &staged_revision)
				                           && corrected_file.commit();
				if (corrected_snapshot_ready)
				{
					adopted = AppleDocumentAccess::adoptExportedDocument(
						exported_path,
						corrected_path,
						exported_fingerprint,
						makePresentedDocumentChangeHandler(),
						&replacement_token,
						&coordinated_path,
						&provider_error);
				}
				else
				{
					AppleDocumentAccess::abandonExportedDocument();
				}
			}
		}
		if (adopted)
		{
			if (private_draft_fork)
				private_draft_fork->setAutoRemove(false);
			keep_private_draft_fork = true;
			// Save As forks the source document. Any pending source-side cleanup
			// must remain available to the unchanged source provider document.
			clearDeferredPrivateDraftCleanup();
			presented_document_token = replacement_token;
			presents_document = true;
			presented_document_deleted = false;
			committed_path = coordinated_path.isEmpty()
			                 ? exported_path
			                 : DocumentPath::canonical(coordinated_path);
			const auto old_path = currentPath();
			if (old_path != committed_path)
			{
				recovery_identity_ready = migrateRecoveryForDocumentMove(
					old_path, committed_path);
				setCurrentFile(committed_path, &format);
			}
			// Re-keying a receipt is not enough: the recovery map must serialize
			// the freshly forked private-resource paths under the new identity.
			if (has_unsaved_changes || has_autosave_conflict)
			{
				const bool recovery_snapshot_saved = persistRecoverySnapshot();
				recovery_identity_ready =
					recovery_identity_ready && recovery_snapshot_saved;
			}
			actual_path = committed_path;
			presented_document_snapshot.reset();
		}
	}
	clearStatusBarMessage();
	if (!adopted)
	{
		// Provider callbacks must observe source identities after a failed
		// adoption, not temporary paths that are about to disappear.
		rollback_private_draft_fork();
		private_draft_rollback.dismiss();
		if (!exported_path.isEmpty())
		{
			QString cleanup_error;
			failed_export_removed = AppleDocumentAccess::removeExportedDocument(
				exported_path, exported_fingerprint, &cleanup_error);
			if (!failed_export_removed && !cleanup_error.isEmpty())
			{
				if (!provider_error.isEmpty())
					provider_error += QLatin1String("\n\n");
				provider_error += cleanup_error;
			}
		}
	}
	replayPendingPresentedDocumentEvents();

	if (!exported)
	{
		if (!provider_error.isEmpty())
			QMessageBox::warning(this, tr("Export failed"), provider_error);
		return false;
	}
	if (!corrected_snapshot_ready || !adopted)
	{
		QString detail = provider_error;
		if (detail.isEmpty())
		{
			detail = tr("Mapper could not prepare or reopen the provider-relative version.");
		}
		QMessageBox::warning(
			this,
			tr("Exported copy needs attention"),
			failed_export_removed
			? tr("Mapper could not safely rewrite and adopt the exported document, "
			     "so it removed the incomplete copy. Retry Save As.\n\n%1")
				.arg(detail)
			: tr("Files exported a provisional copy to %1, but Mapper could not "
			     "safely rewrite and adopt it or remove it afterward. It may still "
			     "reference private template drafts owned by the source document. "
			     "Delete that copy and retry Save As.\n\n%2")
				.arg(DocumentPath::displayName(exported_path), detail));
		return false;
	}

	const bool provider_stable = replacement_token != 0
	                             && presented_document_token == replacement_token
	                             && presents_document
	                             && !presented_document_deleted
	                             && currentPath() == committed_path
	                             && presented_document_change_generation
	                                == transaction_generation;
	if (!provider_stable)
	{
		const bool recovery_current = persistRecoverySnapshot();
		QMessageBox::warning(
			this,
			tr("Export changed during completion"),
			recovery_current
				? tr("The provider changed the exported document while Mapper was "
				     "adopting it. The map remains marked as modified and its recovery "
				     "copy was updated.")
				: tr("The provider changed the exported document while Mapper was "
				     "adopting it. The map remains marked as modified, but Mapper could "
				     "not update its recovery copy."));
		return false;
	}
	if (!recovery_identity_ready)
	{
		QMessageBox::warning(
			this,
			tr("Recovery copy needs attention"),
			tr("The exported document is open, but Mapper could not establish a "
			   "current private recovery copy under its new identity. Keep Mapper "
			   "open and save again before leaving the app."));
		return false;
	}
	if (controller->saveRevision() != staged_revision)
	{
		const bool recovery_current = persistRecoverySnapshot();
		QMessageBox::warning(
			this,
			tr("Map changed during export"),
			recovery_current
				? tr("Files received the staged version, but the map was edited while "
				     "the export was completing. Mapper kept the newer map marked as "
				     "modified and updated its recovery copy.")
				: tr("Files received the staged version, but the map was edited while "
				     "the export was completing. Mapper kept the newer map marked as "
				     "modified, but could not update its recovery copy."));
		return false;
	}

	setMostRecentlyUsedFile(committed_path);
	const bool fully_clean = controller->markSaveCommitted(staged_revision, false);
	setUnsavedStateAfterDocumentCommit(!fully_clean);
	setHasAutosaveConflict(false);
	const bool external_recovery_current =
		fully_clean ? removeAutosaveFile() : persistRecoverySnapshot();
	external_change_pending = false;
	if (!fully_clean)
	{
		QMessageBox::warning(
			this,
			tr("External template not saved"),
			external_recovery_current
			? tr("The map was exported safely, but an edited external template still "
			     "needs its own authorized save or relink. Mapper preserved a private "
			     "recovery map with stable resource identities.")
			: tr("The map was exported safely, but an edited external template still "
			     "needs its own authorized save or relink. Mapper could not update its "
			     "private recovery map; keep Mapper open and retry."));
	}
	return fully_clean;
}
#endif

bool MainWindow::showSaveAsDialog()
{
	if (map_hub_read_only && !map_hub_local_copy_in_progress)
	{
		QMessageBox::information(
		  this, tr("Read-only Map Hub map"),
		  tr("Request editing access from Map Hub before creating an "
		     "editable copy."));
		return false;
	}

	if (!controller)
		return false;
	
	// Try current directory first
	QString save_directory;
	if (!DocumentPath::isContentUri(currentPath()))
		save_directory = QFileInfo(currentPath()).canonicalPath();
	if (save_directory.isEmpty())
	{
		// revert to least recently used directory or home directory.
		QSettings settings;
		save_directory = settings.value(QString::fromLatin1("openFileDirectory"), QDir::homePath()).toString();
	}
	
	// Build the list of supported file filters based on the file format registry
	QString filters;
	std::vector<const FileFormat*> writable_formats;
	for (auto format : FileFormats.formats())
	{
		if (format->supportsFileSaveAs())
		{
			writable_formats.push_back(format);
			if (filters.isEmpty()) 
				filters = format->filter();
			else
				filters = filters + QLatin1String(";;") + format->filter();
		}
	}

	const FileFormat* suggested_format = current_format;
	if (!suggested_format || !suggested_format->supportsFileSaveAs())
		suggested_format = FileFormats.findFormat(FileFormats.defaultFormat());
	if (!suggested_format)
		return false;

	QString dialog_location = save_directory;
	QString filter; // will be set to the selected filter by QFileDialog
#if defined(Q_OS_IOS)
	QStringList format_options;
	int preferred_index = 0;
	for (int index = 0; index < int(writable_formats.size()); ++index)
	{
		const auto* format = writable_formats.at(index);
		format_options.push_back(
			tr("%1 (.%2)").arg(
				format->description(),
				format->fileExtensions().join(QLatin1String(", ."))));
		if (format == suggested_format)
			preferred_index = index;
	}
	if (format_options.isEmpty())
		return false;
	int selected_index = 0;
	if (format_options.size() > 1)
	{
		if (!beginNativeDocumentInteraction())
			return false;
		auto interaction_guard = qScopeGuard(
			[this] { endNativeDocumentInteraction(); });
		selected_index = AppleDocumentAccess::chooseDocumentFormat(
			tr("Choose file format"),
			format_options,
			preferred_index,
			tr("Cancel"));
	}
	if (selected_index < 0 || selected_index >= int(writable_formats.size()))
		return false;
	suggested_format = writable_formats.at(selected_index);

	// UIDocumentPicker derives the exported document's type and suggested name
	// from the input URL. Establish both before presenting it; never mutate the
	// security-scoped URL returned by the provider afterwards.
	QString suggested_name = DocumentPath::displayName(currentPath());
	if (suggested_name.isEmpty())
		suggested_name = tr("Untitled map");
	if (suggested_format)
	{
		suggested_name = suggested_format->fixupExtension(suggested_name);
		filter = suggested_format->filter();
	}
	dialog_location = QDir(save_directory).filePath(suggested_name);
	return exportPresentedDocument(*suggested_format, suggested_name);
#endif
	QString path = FileDialog::getSaveFileName(
		this, tr("Save file"), dialog_location, filters, &filter);
	
	// On Windows, when the user enters "sample", we get "sample.omap *.xmap".
	// (Fixed in upstream qtbase/src/plugins/platforms/windows/qwindowsdialoghelpers.cpp
	// Wednesday March 20 2013 in commit 426f2cc.)
	// This results in an error later, because "*" is not a valid character.
	// But it is reasonable to apply the workaround to all platforms, 
	// due to the special meaning of "*" in shell patterns.
	const int extensions_quirk = path.indexOf(QLatin1String(" *."));
	if (extensions_quirk >= 0)
	{
		path.truncate(extensions_quirk);
	}
	
	if (path.isEmpty())
		return false;
	
	const FileFormat *format = FileFormats.findFormatByFilter(filter, &FileFormat::supportsFileSaveAs);
	if (!format)
		format = FileFormats.findFormatForFilename(path, &FileFormat::supportsFileSaveAs);
	if (!format && suggested_format && suggested_format->supportsFileSaveAs())
		format = suggested_format;
	if (!format && current_format && current_format->supportsFileSaveAs())
		format = current_format;
	if (!format)
	{
		auto* const default_format = FileFormats.findFormat(FileFormats.defaultFormat());
		if (default_format && default_format->supportsFileSaveAs())
			format = default_format;
	}
	if (!format)
	{
		QMessageBox::information(this, tr("Error"), 
		  tr("File could not be saved:") + QLatin1Char('\n') +
		  tr("There was a problem in determining the file format.") + QLatin1Char('\n') + QLatin1Char('\n') +
		  tr("Please report this as a bug.") );
		return false;
	}
	
#if !defined(Q_OS_IOS)
	if (!DocumentPath::isContentUri(path))
		path = format->fixupExtension(path);
#endif
	return saveTo(path, *format);
}

void MainWindow::openMapHubLocalCopy()
{
	if (!map_hub_read_only || map_hub_local_copy_in_progress)
		return;

	map_hub_local_copy_in_progress = true;
	auto reset = qScopeGuard(
	  [this] { map_hub_local_copy_in_progress = false; });
	if (showSaveAsDialog())
	{
		showStatusBarMessage(
		  tr("Opened an independent local copy. Map Hub will not receive "
		     "changes from this file."),
		  8000);
	}
}

void MainWindow::toggleFullscreenMode()
{
	if (isFullScreen())
	{
		showNormal();
		if (maximized_before_fullscreen)
			showMaximized();
	}
	else
	{
		maximized_before_fullscreen = isMaximized();
		showFullScreen();
	}
}

void MainWindow::showSettings()
{
	SettingsDialog dialog(this);
	dialog.exec();
}

void MainWindow::showAbout()
{
	AboutDialog about_dialog(this);
	about_dialog.exec();
}

void MainWindow::showHelp()
{
	Util::showHelp(this);
}

void MainWindow::linkClicked(const QString &link)
{
	if (link.compare(QLatin1String("settings:"), Qt::CaseInsensitive) == 0)
		showSettings();
	else if (link.compare(QLatin1String("help:"), Qt::CaseInsensitive) == 0)
		showHelp();
	else if (link.compare(QLatin1String("about:"), Qt::CaseInsensitive) == 0)
		showAbout();
	else if (link.startsWith(QLatin1String("examples:"), Qt::CaseInsensitive))
		openPathLater(QLatin1String("data:/examples/") + QStringView{link}.mid(9));
	else
		QDesktopServices::openUrl(QUrl{link});
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
	Q_UNUSED(object)
	
	switch (event->type())
	{
	case QEvent::WhatsThisClicked:
		{
			QWhatsThisClickedEvent* e = static_cast<QWhatsThisClickedEvent*>(event);
			Util::showHelp(this, e->href());
		};
		break;
		
	case QEvent::TouchBegin:
		if (ignore_touch_test_button && object == ignore_touch_test_button)
		{
			showStatusBarMessage(tr("When you want to have touch input disabled,"
			                        " you must use another pointing device"
			                        " to select the \"OK\" button."), 3000);
		}
		Q_FALLTHROUGH();
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
		if (ignoreTouch()
		    || (ignore_touch_test_button && object == ignore_touch_test_button))
		{
			event->accept();
			return true;
		}
		break;

#if defined(Q_OS_ANDROID)
	case QEvent::KeyRelease:
		if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Back && hasOpenedFile())
		{
			/* Don't let Qt close the application in
			 * QGuiApplicationPrivate::processKeyEvent() while a file is opened.
			 * 
			 * This must be the application-wide event filter in order to
			 * catch Qt::Key_Back from popup menus (such as template list,
			 * overflow actions) and modal dialogs.
			 * 
			 * Popup are closed when this event is received. Any other widget
			 * which wants to handle Qt::Key_Back needs to watch for
			 * QEvent::KeyPress.
			 */
			if (auto* popup = QApplication::activePopupWidget())
				popup->close();
			
			event->accept();
			return true;
		}
		break;
#endif
	default:
		; // nothing
	}
	
	return false;
}


}  // namespace OpenOrienteering
