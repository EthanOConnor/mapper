/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2013-2016  Kai Pastor
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


#ifndef OPENORIENTEERING_MAIN_WINDOW_H
#define OPENORIENTEERING_MAIN_WINDOW_H

#include <memory>

#include <Qt>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "collaboration/managed_map_workspace.h"
#include "core/autosave.h"
#if defined(Q_OS_IOS)
#  include "core/apple_document_access.h"
#endif
#include "fileformats/file_format.h"

class QAction;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QLabel;
class QMenu;
class QJsonObject;
class QStackedWidget;
class QTemporaryFile;
class QToolBar;
class QTimer;
class QWidget;

namespace OpenOrienteering {

class MainWindowController;
class MapEditorController;
class MapHubApiClient;
class MapHubSyncController;
class MapperServiceProxy;
class Toast;
struct MapHubReadOnlyDocument;
/**
 * The MainWindow class provides the generic application window.
 * 
 * It always has an active controller (class MainWindowController)
 * which provides the specific window content and behaviours.
 * The controller can be exchanged while the window is visible.
 */
class MainWindow : public QMainWindow, private Autosave
{
Q_OBJECT
public:
	struct FileInfo
	{
		// To be used with aggregate initialization
		QString file_path;
		const FileFormat* file_format;
		
		// cf. QFileInfo::filePath
		QString filePath() const { return file_path; }
		
		const FileFormat* fileFormat() const noexcept { return file_format; }
		
		operator bool() const noexcept { return !file_path.isEmpty(); }
		
	};
	
		
	/**
	 * Creates a new main window.
	 */
	explicit MainWindow(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	
private:
	/**
	 * Creates a new main window.
	 * 
	 * The flag as_main_window is a contradiction to the general intent of this
	 * class. The value false is used only once, in SymbolSettingDialog.
	 * For this case, it disables some features such as the main menu.
	 * 
	 * \todo Refactor to remove the flag as_main_window.
	 */
	explicit MainWindow(bool as_main_window, QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	
	friend class SymbolSettingDialog;
	
public:
	/** Destroys a main window. */
	~MainWindow() override;
	
	/** Returns the application's localized name. */
	QString appName() const;
	
	
	/**
	 * Changes the controller.
	 *
	 * The new controller does not edit a file.
	 */
	void setController(MainWindowController* new_controller);
	
	/**
	 * Changes the controller.
	 *
	 * The new controller edits the file with the given path.
	 * The path may be empty for a new (unnamed) file.
	 */
	void setController(MainWindowController* new_controller, const QString& path, const FileFormat* format);
	
private:
	void setController(MainWindowController* new_controller, bool has_file);

public:	
	/** Returns the current controller. */
	MainWindowController* getController() const;
	
	
	/**
	 * Returns the canonical path of the currently open file.
	 * 
	 * It no file is open, returns an empty string.
	 */
	const QString& currentPath() const { return current_path; }
	
	/**
	 * Returns the file format of the currently open file.
	 * 
	 * It no file is open or the format is unknown, returns nullptr.
	 */
	const FileFormat* currentFormat() const { return current_format; }
	
	
	
	/** Registers the given path as most recently used file.
	 * 
	 *  The path is added at (or moved to) the top of the list of most recently
	 *  used files, and the directory is saved as most recently used directory.
	 */
	static void setMostRecentlyUsedFile(const QString& path);
	
	/** Returns true if a file is opened in this main window. */
	bool hasOpenedFile() const;
	
	
	/** Returns true if the opened file is marked as having unsaved changes. */
	bool hasUnsavedChanges() const;
	
	
	/** Sets the text in the status bar. */ 
	void setStatusBarText(const QString& text);
	
	/**
	 * Shows a temporary message in the status bar.
	 * 
	 * Normally, the message won't become visible before control returns
	 * to the event loop where the GUI update events are processed.
	 * For messages indicating a potentially long-running operation, use
	 * showStatusBarMessageImmediately() instead.
	 */
	void showStatusBarMessage(const QString& text, int timeout = 0);
	
	/**
	 * Shows a temporary message in the status bar immediately.
	 * 
	 * Use this function instead of showStatusBarMessage() when indicating the
	 * start of potentially long-running operations to ensure that GUI update
	 * events are processed immediately.
	 */
	void showStatusBarMessageImmediately(const QString& text, int timeout = 0);
	
	/** Clears temporary messages set in the status bar with showStatusBarMessage(). */
	void clearStatusBarMessage();
	
	
	/**
	 * Blocks shortcuts.
	 * 
	 * During text input, it may be necessary to disable shortcuts.
	 * 
	 * @param blocked true for blocking shortcuts, false for normal behaviour.
	 */
	void setShortcutsBlocked(bool blocked);
	
	/** Returns true if shortcuts are currently disabled. */
	bool shortcutsBlocked() const;
	
	
	/** Returns the main window's file menu so that it can be extended. */
	QMenu* getFileMenu() const;
	
	/** Returns an QAction which serves as extension point in the file menu. */
	QAction* getFileMenuExtensionAct() const;
	
	/** Returns the save action. */
	QAction* getSaveAct() const;
	
	/** Returns the close action. */
	QAction* getCloseAct() const;

	/** Returns the always-available connected-workspace status action. */
	QAction* getMapHubWorkspaceAct() const;
	
	
	/**
	 * Returns a general toolbar with standard file actions (new, open, save).
	 * 
	 * The MainWindowController is responsible to add it to the main window.
	 * It will be destroyed (and recreated) when the controller changes.
	 */
	QToolBar* getGeneralToolBar() const;
	
	
	/** Open the file with the given path after all events have been processed.
	 *  May open a new main window.
	 *  If loading is successful, the selected path will become
	 *  the [new] window's current path.
	 */
	void openPathLater(const QString &path);

	/**
	 * Handles a document delivered by the operating system. Mobile platforms
	 * deliberately keep one editing document in the single application scene.
	 */
	void openExternalPath(const QString& path);

#if defined(Q_OS_IOS)
	enum class NativeDocumentCheckpoint
	{
		BestEffort,
		Required,
		Skip,
	};

	/** Serializes native pickers and coordinated provider transactions. */
	bool beginNativeDocumentInteraction(
		NativeDocumentCheckpoint checkpoint = NativeDocumentCheckpoint::BestEffort);
	void endNativeDocumentInteraction();

	/** Finds the real document window above a dialog or embedded preview. */
	static MainWindow* nativeDocumentInteractionOwner(QWidget* widget);

	/** Deletes an unlinked private draft only after its map revision commits. */
	void deferPrivateDraftCleanup(
		const QString& path, const QString& resource_identity);
#endif
	
	/**
	 * Save the content of the main window.
	 */ 
	bool saveTo(const QString &path, const FileFormat& format);
	
	/** Shows the open file dialog for the given file type(s) and returns the chosen file
	 *  or an empty string if the dialog is aborted.
	 */
	static MainWindow::FileInfo getOpenFileName(QWidget* parent, const QString& title, FileFormat::FileTypes types);
	
	
	/**
	 * Shows a message box for a list of unformatted messages.
	 */
	static void showMessageBox(QWidget* parent, const QString& title, const QString& headline, const std::vector<QString>& messages);
	
	
	/**
	 * Sets the MainWindow's effective central widget.
	 * 
	 * Any previously set widget will be hidden and scheduled for deletion.
	 * 
	 * Hides an implementation in QMainWindow which causes problems with
	 * dock widgets when switching from home screen widget to map widget.
	 * NEVER call QMainWindow::setCentralWidget(...) on a MainWindow.
	 */
	void setCentralWidget(QWidget* widget);
	
	
	/**
	 * Indicates whether the home screen is disabled.
	 * 
	 * Normally the last main window will return to the home screen when a file
	 * is closed. When the home screen is disabled, the last window will be
	 * closed instead.
	 */
	bool homeScreenDisabled() const;
	
	/**
	 * Sets whether to show the home screen after closing the last file.
	 * 
	 * @see homeScreenDisabled()
	 */
	void setHomeScreenDisabled(bool disabled);

	/**
	 * Control whether touch input events should be ignored by the main window.
	 */	
	void setIgnoreTouch(bool on);
	bool ignoreTouch();
	
	/**
	 * Wrapper for setIgnoreTouch() that warns the user about touch 
	 * input disable. If the user does not accept the new setting, 
	 * the method keeps the status quo.
	 */	
	void warnAndSetIgnoreTouch(bool on);
	
public slots:
	/**
	 * Reacts to application state changes.
	 * 
	 * On Android, when the application state becomes Qt::ApplicationActive,
	 * this method looks for the Android activity's current intent and triggers
	 * the loading of a given file (if there is not already another file loaded).
	 * 
	 * In general, when called for the first time after application start, it
	 * opens the most recently used file, unless this feature is disabled in the
	 * settings, and unless other files are registered for opening (i.e. files
	 * given as command line parameters.)
	 */
	void applicationStateChanged();
	
	/**
	 * Show a wizard for creating new maps.
	 * 
	 * May open a new main window.
	 */
	void showNewMapWizard();

	/** Open the connected map library and this user's assignments. */
	void showMapHub();

	/** Show the local and shared truth for the current connected workspace. */
	void showMapHubWorkspaceStatus();

	/** Create an ordinary .omap map bound to a server-created workspace. */
	void createConnectedMap(const ManagedMapWorkspace& workspace);

	/** Open a verified server artifact and normalize imports to a managed .omap. */
	bool openConnectedWorkspace(const QString& source_path,
	                            const QString& normalized_omap_path,
	                            ManagedMapWorkspace workspace);

	/** Open a verified immutable Map Hub revision in non-mutating viewer mode. */
	bool openMapHubReadOnly(const QString& source_path,
	                       const MapHubReadOnlyDocument& document);

	/** Fork the current immutable Map Hub revision into an ordinary local map. */
	void openMapHubLocalCopy();

	/** Persist native edit-access state for the current read-only map. */
	void updateMapHubReadOnlyAccess(const QString& project_id,
	                                const QJsonObject& request);

	/** Save and upload an immutable checkpoint for the current managed map. */
	void checkpointMapHub();

	/** Submit the current managed-map checkpoint for review. */
	void submitMapHub();
	
	/**
	 * Show a file-open dialog and load the select file.
	 * 
	 * May open a new main window.
	 * If loading is successful, the selected path will become
	 * the [new] window's current path.
	 */
	void showOpenDialog();
	
	/**
	 * Show a file-save dialog.
	 * 
	 * If saving is successful, the selected path will become
	 * this window's current path.
	 * 
	 * @return true if saving was successful, false otherwise
	 */
	bool showSaveAsDialog();
	
	/**
	 * Open the file with the given path.
	 * 
	 * May open a new main window.
	 * If loading is successful, the selected path will become
	 * the [new] window's current path.
	 * 
	 * @return true if loading was successful, false otherwise
	 */
	bool openPath(const QString &path);
	
	bool openPath(const QString &path, const OpenOrienteering::FileFormat* format);
	
	/**
	 * Open the file specified in the sending action's data.
	 * 
	 * This is intended for opening recent files.
	 */
	void openRecentFile();
	
	/**
	 * Notify the main window of a change to the list of recent files.
	 */
	void updateRecentFileActions();
	
	/**
	 * Save the current content to the current path.
	 * 
	 * This will trigger a file-save dialog if the current path is not set (i.e. empty).
	 */
	bool save();
	
	/** Save the current content to the current path.
	 */
	Autosave::AutosaveResult autosave() override;
	
	/**
	 * Close the file currently opened.
	 * 
	 * If there are changes to the current file, the user will be asked if he
	 * wants to save it - the user may even cancel the closing of the file.
	 * 
	 * This will close the window unless this is the last window.
	 * 
	 * @return True if the file was actually closed, false otherwise.
	 */
	bool closeFile();
	
	/** Toggle between normal window and fullscreen mode.
	 */
	void toggleFullscreenMode();
	
	/** Show the settings dialog.
	 */
	void showSettings();
	
	/** Show the about dialog.
	 */
	void showAbout();
	
	/** Show the index page of the manual in the help browser.
	 */
	void showHelp();
	
	/** Open a link.
	 *  This is called when the user clicks on a link in the UI,
	 *  e.g. in the tip of the day.
	 * 
	 * 	@param link the target URI
	 */
	void linkClicked(const QString &link);
	
	/**
	 * Notifies this window of unsaved changes.
	 * 
	 * If the controller was set as having an opened file, setting this value to
	 * true will start the autosave countdown if the previous value was false.
	 * 
	 * This will update the window title via QWidget::setWindowModified().
	 */
	void setHasUnsavedChanges(bool value);
	
signals:
	/**
	 * This signal is emitted when the actual path changes.
	 * 
	 * @see switchActualPath()
	 */
	void actualPathChanged(const QString &path);
	
	/**
	 * This signal is emitted when an autosave conflict gets resolved.
	 * 
	 * @see setHasAutosaveConflict()
	 */
	void autosaveConflictResolved();
	
protected slots:
	/**
	 * Switches to a different controller and loads the given path.
	 * 
	 * This method is meant for switching between an original file and
	 * autosaved versions. It does not touch current_path. The class of the new
	 * controller is determined from the current_path (i.e. original file).
	 * 
	 * If the given path is the current actual_path, no change is made.
	 * 
	 * If the currently loaded file was modified, the user is asked whether he
	 * really wants to switch to another file which means losing the changes
	 * he had made.
	 */
	void switchActualPath(const QString &path);
	
	/**
	 * Open the files which have been registered by openPathLater().
	 */
	void openPathBacklog();
	
	/**
	 * Listens to configuration changes.
	 */
	void settingsChanged();
	
private:
	/**
	 * Enables or disables the "toast" which replaces the status bar in touch mode.
	 */
	void updateToastEnabled();
	
protected:
	/** 
	 * Sets the path of the file edited by this windows' controller.
	 * 
	 * This will update the window title via QWidget::setWindowFilePath().
	 * 
	 * If the controller was not set as having an opened file,
	 * the path must be empty.
	 */
	void setCurrentFile(const QString& path, const FileFormat* format);
	
	/**
	 * Notifies the windows of autosave conflicts.
	 * 
	 * An autosave conflict is the situation where a autosaved file exists
	 * when the original file is opened. This autosaved file indicates that
	 * the original file was not properly closed, i.e. the software crashed
	 * before closing.
	 */
	void setHasAutosaveConflict(bool value);
	
	/**
	 * Removes the autosave file if it exists.
	 * 
	 * Returns true if the file was removed or didn't exist, false otherwise.
	 */
	bool removeAutosaveFile() const;
	
	bool event(QEvent* event) override;
	void closeEvent(QCloseEvent *event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void keyReleaseEvent(QKeyEvent* event) override;
	
	bool eventFilter(QObject* object, QEvent* event) override;
	
private:
	static constexpr int max_recent_files = 10;
	
	/**
	 * Conditionally shows a dialog for saving pending changes.
	 * 
	 * If this main window has an opened file with unsaved changes, shows
	 * a dialog which lets the user save the file, discard the changes or
	 * cancel. 
	 * 
	 * Returns true if the window can be closed, false otherwise.
	 */
	bool showSaveOnCloseDialog();
	
	
	/** Saves the window position and state. */
	void saveWindowSettings();
	
	/** Loads the window position and state. */
	void loadWindowSettings();
	
	
	void createFileMenu();
	void createHelpMenu();
	MainWindow* createNewMapWithWizard(unsigned int required_scale = 0,
	                                   const QString& required_crs = {},
	                                   const QString& required_symbol_standard = {});
	void checkpointMapHub(bool submit_after);
	void leaveMapHubConnectedEditing();
	bool isCurrentMapHubCheckpoint(MapHubApiClient* client,
	                               quint64 generation,
	                               const QString& path,
	                               const QString& workspace_id) const;
	void finishMapHubCheckpoint(MapHubApiClient* client);
	void updateMapHubActions();
	void updateMapHubStatusSurface();
	void renewMapHubLeaseIfNeeded();
	void configureMapHubSync();
	void refreshMapHubImageryCatalog();
	void pollMapHubReadOnlyAccess();

	static MainWindow* findMainWindow(const QString& file_name);

#if defined(Q_OS_IOS)
	AppleDocumentAccess::ChangeHandler makePresentedDocumentChangeHandler();
	void handlePresentedDocumentChange(const AppleDocumentAccess::PresentedDocumentEvent& event);
	void processPresentedDocumentChange();
	bool reloadPresentedDocument(bool discard_local_recovery = false);
	bool exportPresentedDocument(const FileFormat& format, const QString& suggested_name);
	void setUnsavedStateAfterDocumentCommit(bool external_resources_dirty);
	bool migrateRecoveryForDocumentMove(const QString& old_path, const QString& new_path);
	void replayPendingPresentedDocumentEvents();
	bool persistRecoverySnapshot();
	bool persistAuxiliaryDraftRecovery();
	void discardAuxiliaryDraftRecovery();
	bool persistMapRecoverySnapshot();
	void restoreAuxiliaryDraftRecovery();
	bool canAdvancePrivateRecovery() const;
	void commitDeferredPrivateDraftCleanup();
	void clearDeferredPrivateDraftCleanup();
	bool stopPresentingForClose();
#endif
	
	
	/// The active controller
	MainWindowController* controller;
	const bool create_menu;
	bool show_menu;
	bool shortcuts_blocked;
	bool ignore_touch_input = false;
	QObject* ignore_touch_test_button = nullptr;
	
	QToolBar* general_toolbar;
	QMenu* file_menu;
	QAction* save_act;
	QAction* save_as_act = nullptr;
	QMenu* open_recent_menu;
	bool open_recent_menu_inserted;
	QAction* recent_file_act[max_recent_files];
	QAction* settings_act;
	QAction* close_act;
	QAction* map_hub_act = nullptr;
	QAction* map_hub_checkpoint_act = nullptr;
	QAction* map_hub_submit_act = nullptr;
	MapHubApiClient* map_hub_checkpoint_client = nullptr;
	bool map_hub_checkpoint_pending = false;
	bool map_hub_submission_pending = false;
	quint64 map_hub_document_generation = 0;
	QTimer* map_hub_lease_timer = nullptr;
	bool map_hub_lease_renewal_pending = false;
	QTimer* map_hub_access_timer = nullptr;
	bool map_hub_access_poll_pending = false;
	QString map_hub_access_etag;
	MapHubSyncController* map_hub_sync = nullptr;
	QLabel* map_hub_sync_label = nullptr;
	bool map_hub_read_only = false;
	bool map_hub_local_copy_in_progress = false;
	bool map_hub_imagery_refresh_pending = false;
	QLabel* status_label;
	Toast* toast = nullptr;
	
	std::unique_ptr<MapperServiceProxy> service_proxy;
	
	/// Canonical path to the currently open file or an empty string if the file was not saved yet ("untitled")
	QString current_path;
	/// The current file's format, as determined during opening the file.
	const FileFormat* current_format = nullptr;
	/// The actual path loaded by the editor. @see switchActualPath()
	QString actual_path;
	/// Does the main window display a file? If yes, new controllers will be opened in new main windows instead of replacing the active controller of this one
	bool has_opened_file;
	/// If this window has an opened file: does this file have unsaved changes?
	bool has_unsaved_changes;
	/// Indicates the presence of an autosave conflict. @see setHasAutosaveConflict()
	bool has_autosave_conflict;
#if defined(Q_OS_IOS)
	/// The in-memory editor is older than a coordinated provider write.
	bool external_change_pending = false;
	/// This window owns the process-wide iOS file presenter.
	bool presents_document = false;
	/// Rejects callbacks from a presenter replaced by Save As or Close.
	quint64 presented_document_token = 0;
	/// Provider callbacks seen by a nested dialog while an open is committing.
	QList<AppleDocumentAccess::PresentedDocumentEvent> pending_presented_document_events;
	/// Defers provider callbacks until a staged open/save/reload transaction commits.
	bool provider_document_transaction_active = false;
	/// Lets an owning transaction apply its queued callbacks without opening reentrancy.
	bool replaying_presented_document_events = false;
	/// Native waits pause foreground GPS recording after making a recovery receipt.
	QPointer<MapEditorController> native_interaction_gps_controller;
	bool resume_gps_after_native_interaction = false;
	/// Rejects a second Close while the first close prompt or save is running.
	bool close_in_progress = false;
	/// Monotonic external lifecycle revision used to reject stale GUI staging.
	quint64 presented_document_change_generation = 0;
	/// Dirty bytes belonging to the UIDocument, excluding independent templates.
	bool presented_document_content_dirty = false;
	bool external_resources_dirty = false;
	/// A deleted provider item remains a recoverable in-memory document until Save As.
	bool presented_document_deleted = false;
	/// Keeps the coordinated provider snapshot alive for deferred imports.
	std::unique_ptr<QTemporaryFile> presented_document_snapshot;
	struct DeferredPrivateDraftCleanup
	{
		QString path;
		QString resource_identity;
	};
	/// Old private links that remain valid until their removal is provider-safe.
	QList<DeferredPrivateDraftCleanup> deferred_private_draft_cleanup;
#endif
	
	/// Was the window maximized before going into fullscreen mode? In this case, we have to show it maximized again when leaving fullscreen mode.
	bool maximized_before_fullscreen;
	
	bool homescreen_disabled;

	/// Number of active main windows. The last window shall not close on File > Close.
	static int num_open_files;
	
	/// The central widget which never changes during a MainWindow's lifecycle
	QStackedWidget* central_widget;
	
	/// A list of paths to be opened later.
	QStringList path_backlog;
	/// A flag indicating that backlog procession is currently active.
	bool path_backlog_busy = false;
};


// ### MainWindow inline code ###

inline
MainWindowController* MainWindow::getController() const
{
	return controller;
}



inline
bool MainWindow::hasOpenedFile() const
{
	return has_opened_file;
}

inline
bool MainWindow::hasUnsavedChanges() const
{
	return has_unsaved_changes;
}

inline
bool MainWindow::shortcutsBlocked() const
{
	return shortcuts_blocked;
}

inline
QMenu* MainWindow::getFileMenu() const
{
	return file_menu;
}

inline
QAction* MainWindow::getFileMenuExtensionAct() const
{
	return settings_act;
}

inline
QAction* MainWindow::getSaveAct() const
{
	return save_act;
}

inline
QAction* MainWindow::getCloseAct() const
{
	return close_act;
}

inline
QAction* MainWindow::getMapHubWorkspaceAct() const
{
	return map_hub_act;
}

inline
QToolBar* MainWindow::getGeneralToolBar() const
{
	return general_toolbar;
}

inline
bool MainWindow::homeScreenDisabled() const
{
	return homescreen_disabled;
}


}  // namespace OpenOrienteering

#endif
