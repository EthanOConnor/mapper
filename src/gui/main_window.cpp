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

#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
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
#  include <QTimer>
#endif

#include "mapper_config.h"
#include "settings.h"
#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "core/document_path.h"
#include "core/georeferencing.h"
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
	statusBar()->setSizeGripEnabled(as_main_window);
	updateToastEnabled();
	
	central_widget = new QStackedWidget(this);
	QMainWindow::setCentralWidget(central_widget);
	
	if (as_main_window)
		loadWindowSettings();
	
#if defined(Q_OS_ANDROID)
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
		QTimer::singleShot(0, this, &MainWindow::renewMapHubLeaseIfNeeded);
#ifdef Q_OS_ANDROID
	// The Android app may be started or resumed when the user triggers a suitable "intent".
	if (QGuiApplication::applicationState() == Qt::ApplicationActive)
	{
		auto activity = QNativeInterface::QAndroidApplication::context();
		auto intent_uri = activity.callMethod<QString>("takeIntentUri");
		if (!intent_uri.isEmpty())
		{
			const auto selected_file = DocumentPath::fromUrl(QUrl{intent_uri});
			if (!hasOpenedFile())
			{
				openPathLater(selected_file);
			}
			else if (currentPath() != selected_file)
			{
				showStatusBarMessage(tr("You must close the current file before you can open another one."));
			}
			return;
		}
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
	
	auto save_as_act = new QAction(tr("Save &as..."), this);
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
	file_menu->addAction(exit_act);
	
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
	has_unsaved_changes = value;
	setAutosaveNeeded(has_unsaved_changes && hasOpenedFile() && !has_autosave_conflict);
	setWindowModified(has_unsaved_changes);
	
#ifdef Q_OS_ANDROID
	if (!service_proxy)
		service_proxy = std::make_unique<MapperServiceProxy>();
	service_proxy->setActiveWindow(has_unsaved_changes ? this : nullptr);
#endif
}

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
	bool closed = !has_opened_file || showSaveOnCloseDialog();
	if (closed)
	{
		if (has_opened_file)
		{
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
	if (!has_opened_file)
	{
		saveWindowSettings();
		event->accept();
	}
	else if (showSaveOnCloseDialog())
	{
		if (has_opened_file)
		{
			num_open_files--;
			has_opened_file = false;
		}
		saveWindowSettings();
		event->accept();
	}
	else
	{
		event->ignore();
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
			if (has_autosave_conflict)
				setHasAutosaveConflict(false);
			else
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
#if !defined(Q_OS_ANDROID)
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
	auto root = Settings::getInstance().getSetting(Settings::MapHub_WorkspaceRoot).toString();
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

void MainWindow::updateMapHubActions()
{
	if (!map_hub_checkpoint_act || !map_hub_submit_act)
		return;
	QString error;
	auto workspace = current_path.isEmpty() ? ManagedMapWorkspace{} : ManagedMapWorkspace::loadForMap(current_path, &error);
	auto native_workspace = workspace.isValid()
	                     && DocumentPath::suffix(current_path).compare(
	                          QLatin1String("omap"), Qt::CaseInsensitive) == 0;
	map_hub_checkpoint_act->setEnabled(
	  native_workspace && workspace.status != QLatin1String("submitted"));
	map_hub_submit_act->setEnabled(native_workspace && workspace.status != QLatin1String("submitted"));
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

void MainWindow::renewMapHubLeaseIfNeeded()
{
	if (map_hub_lease_renewal_pending || currentPath().isEmpty())
		return;
	QString metadata_error;
	auto managed = ManagedMapWorkspace::loadForMap(currentPath(), &metadata_error);
	if (!managed.isValid() || !managed.exclusive_editing
	    || managed.status == QLatin1String("submitted"))
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
		                     tr("Map Hub checkpoints preserve the native Mapper workspace. Save this document as .omap before checkpointing; OCAD remains available as an explicit export."));
		return;
	}
	if (hasUnsavedChanges() && !save())
		return;
	QString hash_error;
	auto local_sha = MapHubApiClient::sha256ForFile(currentPath(), &hash_error);
	if (local_sha.isEmpty())
	{
		QMessageBox::warning(this, tr("Could not checkpoint map"), hash_error);
		return;
	}
	auto api_credential = MapHubCredentials::readToken(managed.server_url);
	if (!api_credential || api_credential.token.isEmpty())
	{
		QMessageBox::warning(this, tr("Map Hub account required"),
		                     api_credential.error.isEmpty() ? tr("Reconnect this server in Settings → Map Hub.") : api_credential.error);
		return;
	}
	auto lease_key = MapHubCredentials::workspaceLeaseKey(managed.server_url, managed.workspace_id);
	auto lease = MapHubCredentials::readToken(lease_key);
	if (managed.exclusive_editing && lease.token.isEmpty())
	{
		QMessageBox::warning(this, tr("Editing lease required"),
		                     tr("Your local work is safe. Reopen this assignment from Map Hub to obtain a new lease before checkpointing."));
		return;
	}
	bool needs_checkpoint = managed.active_revision_id.isEmpty()
	                        || managed.active_sha256.compare(local_sha, Qt::CaseInsensitive) != 0;
	auto* client = new MapHubApiClient(managed.server_url, api_credential.token, this);
	auto submit_revision = [this, client, managed, lease_key, lease_token = lease.token](QString revision_id, ManagedMapWorkspace updated) mutable {
		showStatusBarMessageImmediately(tr("Submitting Map Hub revision for review…"));
		client->submitRevision(revision_id, lease_token, [this, client, updated, lease_key, revision_id](const QJsonObject& response, const MapHubApiClient::Error& error) mutable {
			if (error)
			{
				clearStatusBarMessage();
				auto message = error.message;
				if (error.code == QLatin1String("lease_required"))
					message += tr("\n\nYour checkpoint is safe on the server. Reopen the assignment from Map Hub to obtain a fresh editing lease, then submit again from this file.");
				QMessageBox::warning(this, tr("Could not submit map"), message);
				client->deleteLater();
				return;
			}
			if (response.value(QStringLiteral("revision_id")).toString() != revision_id
			    || response.value(QStringLiteral("state")).toString() != QLatin1String("submitted"))
			{
				clearStatusBarMessage();
				QMessageBox::warning(this, tr("Invalid submission response"),
				                     tr("Map Hub did not confirm submission of the exact checkpoint. The local workspace and lease were left intact."));
				client->deleteLater();
				return;
			}
			updated.status = QStringLiteral("submitted");
			updated.last_synced_at = QDateTime::currentDateTimeUtc();
			QString sidecar_error;
			if (!ManagedMapWorkspace::save(updated, &sidecar_error))
			{
				clearStatusBarMessage();
				MapHubCredentials::removeToken(lease_key);
				QMessageBox::warning(this, tr("Map submitted, but local status was not updated"),
				                     tr("The server accepted the submission, but Mapper could not update its private workspace record: %1").arg(sidecar_error));
				client->deleteLater();
				return;
			}
			MapHubCredentials::removeToken(lease_key);
			clearStatusBarMessage();
			updateMapHubActions();
			QMessageBox::information(this, tr("Submitted to Map Hub"),
			                         tr("Revision r%1 is ready for librarian or director review. Your local .omap file remains unchanged.")
			                           .arg(updated.active_revision_number));
			client->deleteLater();
		});
	};
	if (!needs_checkpoint)
	{
		if (submit_after)
			submit_revision(managed.active_revision_id, managed);
		else
		{
			QMessageBox::information(this, tr("Map Hub checkpoint"), tr("This exact .omap file is already checkpointed as r%1.").arg(managed.active_revision_number));
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
	auto revision_base = managed.active_revision_id.isEmpty() ? managed.base_revision_id : managed.active_revision_id;
	auto key_material = managed.workspace_id + QLatin1Char('|') + revision_base + QLatin1Char('|') + local_sha;
	auto idempotency_key = QStringLiteral("mapper-%1").arg(QString::fromLatin1(
	  QCryptographicHash::hash(key_material.toUtf8(), QCryptographicHash::Sha256).toHex().left(48)));
	showStatusBarMessageImmediately(tr("Uploading verified .omap checkpoint to Map Hub…"));
	client->checkpoint(managed.workspace_id, currentPath(), revision_base, lease.token,
	                   submit_after ? tr("Submission checkpoint") : tr("Mapper checkpoint"), summary, idempotency_key,
	                   [this, client, managed, local_sha, submit_after, submit_revision]
	                   (const QJsonObject& response, const MapHubApiClient::Error& error) mutable {
		if (error)
		{
			clearStatusBarMessage();
			auto message = error.message;
			if (error.code == QLatin1String("stale_base"))
				message += tr("\n\nThe server has a newer base. Your local file was not changed; open the assignment from Map Hub to compare before retrying.");
			else if (error.code == QLatin1String("lease_required"))
				message += tr("\n\nYour local file is safe. Reopen the assignment from Map Hub to obtain a fresh editing lease, then retry this checkpoint from the original file.");
			QMessageBox::warning(this, tr("Could not checkpoint map"), message);
			client->deleteLater();
			return;
		}
		auto returned_revision_id = response.value(QStringLiteral("revision_id")).toString();
		auto returned_number = response.value(QStringLiteral("number")).toInt();
		auto returned_sha = response.value(QStringLiteral("sha256")).toString();
		auto returned_state = response.value(QStringLiteral("state")).toString();
		auto valid_state = returned_state == QLatin1String("checkpoint")
		                || returned_state == QLatin1String("draft")
		                || returned_state == QLatin1String("rejected")
		                || returned_state == QLatin1String("submitted");
		static const QRegularExpression sha256_pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
		if (QUuid(returned_revision_id).isNull() || returned_number <= 0
		    || !sha256_pattern.match(returned_sha).hasMatch() || !valid_state)
		{
			clearStatusBarMessage();
			QMessageBox::warning(this, tr("Invalid checkpoint response"),
			                     tr("Map Hub did not return a complete verified revision record. Mapper did not advance or submit the local workspace."));
			client->deleteLater();
			return;
		}
		if (returned_sha.compare(local_sha, Qt::CaseInsensitive) != 0)
		{
			clearStatusBarMessage();
			QMessageBox::warning(this, tr("Checkpoint checksum mismatch"),
			                     tr("Map Hub stored bytes with a different checksum. Mapper did not advance the local workspace record; contact the server administrator before retrying."));
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
		if (submit_after)
			submit_revision(updated.active_revision_id, updated);
		else
		{
			clearStatusBarMessage();
			showStatusBarMessage(tr("Map Hub checkpoint r%1 uploaded and verified.").arg(updated.active_revision_number), 10000);
			client->deleteLater();
		}
	});
}

void MainWindow::showOpenDialog()
{
	if (auto selected = getOpenFileName(this, tr("Open file"), FileFormat::AllFiles))
	{
		openPath(selected.filePath(), selected.fileFormat());
	}
}

bool MainWindow::openPath(const QString &path)
{
	auto format = FileFormats.findFormatForFilename(path, &FileFormat::supportsFileOpen);
	if (!format)
		format = FileFormats.findFormatForData(path, FileFormat::AllFiles);
	return openPath(path, format);
}

bool MainWindow::openPath(const QString& path, const FileFormat* format)
{
	// Empty path does nothing. This also helps with the single instance application code.
	if (path.isEmpty())
		return true;
	
#ifdef Q_OS_ANDROID
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
	
	if (!format || !format->supportsReading())
	{
		QMessageBox::warning(this, tr("Error"),
		                     tr("Cannot open file:\n%1\n\n%2").
		                     arg(path, tr("Invalid file type.")));
		return false;
	}
	
	// Check a blocker that prevents immediate re-opening of crashing files.
	// Needed for stopping auto-loading a crashing file on startup.
	static const QString reopen_blocker = QString::fromLatin1("open_in_progress");
	QSettings settings;
	const QString open_in_progress(settings.value(reopen_blocker).toString());
	if (open_in_progress == path)
	{
		int result = QMessageBox::warning(this, tr("Crash warning"), 
		  tr("It seems that %1 crashed the last time this file was opened:<br />"
		     "<tt>%2</tt><br /><br />"
		     "Really retry to open it?")
		  .arg(appName(), path),
		  QMessageBox::Yes | QMessageBox::No);
		settings.remove(reopen_blocker);
		if (result == QMessageBox::No)
			return false;
	}
	
	settings.setValue(reopen_blocker, path);
	settings.sync();
	
	MainWindowController* const new_controller = MainWindowController::controllerForFile(path, format);
	if (!new_controller)
	{
		QMessageBox::warning(this, tr("Error"), tr("Cannot open file:\n%1\n\nFile format not recognized.").arg(path));
		settings.remove(reopen_blocker);
		return false;
	}
	
	QString new_actual_path = path;
	const FileFormat* new_actual_format = format;
	QString autosave_path = Autosave::autosavePath(path);
	bool new_autosave_conflict = QFileInfo::exists(autosave_path);
	if (new_autosave_conflict)
	{
#if defined(Q_OS_ANDROID)
		// Assuming small screen, showing dialog before opening the file
		AutosaveDialog* autosave_dialog = new AutosaveDialog(path, autosave_path, autosave_path, this);
		int result = autosave_dialog->exec();
		new_actual_path = (result == QDialog::Accepted) ? autosave_dialog->selectedPath() : QString();
		new_actual_format = (new_actual_path == path) ? format : FileFormats.findFormat(FileFormats.defaultFormat());
		delete autosave_dialog;
#else
		// Assuming large screen, dialog will be shown while the autosaved file is open
		new_actual_path = autosave_path;
		new_actual_format = FileFormats.findFormat(FileFormats.defaultFormat());
#endif
	}
	
	if (new_actual_path.isEmpty() || !new_controller->loadFrom(new_actual_path, *new_actual_format, this))
	{
		delete new_controller;
		settings.remove(reopen_blocker);
		return false;
	}
	
	MainWindow* open_window = this;
#if !defined(Q_OS_ANDROID)
	if (has_opened_file)
		open_window = new MainWindow();
#endif
	
	auto const ignore_touch = Settings::getInstance().getSetting(Settings::MapEditor_IgnoreTouchInput).toBool();
	open_window->warnAndSetIgnoreTouch(ignore_touch);
	
	open_window->setController(new_controller, path, format);
	open_window->actual_path = new_actual_path;
	open_window->setHasAutosaveConflict(new_autosave_conflict);
	open_window->setHasUnsavedChanges(false);
	
	open_window->setVisible(true); // Respect the window flags set by new_controller.
	open_window->raise();
	num_open_files++;
	settings.remove(reopen_blocker);
	setMostRecentlyUsedFile(path);
	
#if !defined(Q_OS_ANDROID)
	// Assuming large screen. Android handled above.
	if (new_autosave_conflict)
	{
		auto autosave_dialog = new AutosaveDialog(path, autosave_path, new_actual_path, open_window, Qt::WindowTitleHint | Qt::CustomizeWindowHint);
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

void MainWindow::openPathBacklog()
{
	if (path_backlog.empty() || path_backlog_busy)
		return;
	
	QScopedValueRollback<bool> rollback{path_backlog_busy, true};
	openPath(path_backlog.takeFirst());
	QTimer::singleShot(10, this, &MainWindow::openPathBacklog);
}

void MainWindow::openRecentFile()
{
	if (auto action = qobject_cast<QAction*>(sender()))
		openPath(action->data().toString());
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
		setAutosaveNeeded(has_unsaved_changes && !has_autosave_conflict);
		if (!has_autosave_conflict)
			emit autosaveConflictResolved();
	}
}

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
	QString path = currentPath();
	auto autosave_format = FileFormats.findFormat(FileFormats.defaultFormat());
	if (path.isEmpty() || !controller || !autosave_format)
	{
		return Autosave::PermanentFailure;
	}
	else if (controller->isEditingInProgress())
	{
		return Autosave::TemporaryFailure;
	}
	else
	{
		showStatusBarMessageImmediately(tr("Autosaving..."), 0);
		if (controller->exportTo(autosavePath(currentPath()), *autosave_format))
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
	auto path = currentPath();
	auto format = currentFormat();
	if (path.isEmpty()
	    || !format
	    || !format->supportsFileSave())
	{
		return showSaveAsDialog();
	}
	
	return saveTo(path, *currentFormat());
}

bool MainWindow::saveTo(const QString &path, const FileFormat& format)
{
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
	
	if (!controller->saveTo(path, format))
		return false;
	
	setMostRecentlyUsedFile(path);
	
	setHasAutosaveConflict(false);
	removeAutosaveFile();
	
	if (path != currentPath())
	{
		setCurrentFile(path, &format);
		removeAutosaveFile();
	}
	
	setHasUnsavedChanges(false);
	
	return true;
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
	QString path = FileDialog::getOpenFileName(parent, title, open_directory, filters, &filter);
	
	const FileFormat* format = nullptr;
	if (!path.isEmpty())
	{
		path = DocumentPath::canonical(path);
		format = FileFormats.findFormatByFilter(filter, &FileFormat::supportsFileOpen);
		if (!format)
			format = FileFormats.findFormatForFilename(path, &FileFormat::supportsFileOpen);
		if (!format)
			format = FileFormats.findFormatForData(path, types);
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



bool MainWindow::showSaveAsDialog()
{
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
	for (auto format : FileFormats.formats())
	{
		if (format->supportsFileSaveAs())
		{
			if (filters.isEmpty()) 
				filters = format->filter();
			else
				filters = filters + QLatin1String(";;") + format->filter();
		}
	}
	
	QString filter; // will be set to the selected filter by QFileDialog
	QString path = FileDialog::getSaveFileName(this, tr("Save file"), save_directory, filters, &filter);
	
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
	
	if (!DocumentPath::isContentUri(path))
		path = format->fixupExtension(path);
	return saveTo(path, *format);
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
