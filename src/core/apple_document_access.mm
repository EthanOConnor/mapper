/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 */

#include "core/apple_document_access.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <memory>
#include <utility>

#include <QEventLoop>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>
#include <QStandardPaths>

using OpenOrienteering::AppleDocumentAccess::ChangeHandler;
using OpenOrienteering::AppleDocumentAccess::PresentedDocumentChange;
using OpenOrienteering::AppleDocumentAccess::PresentedDocumentEvent;

namespace {

QString fromNSString(NSString* string)
{
	return string ? QString::fromUtf8(string.UTF8String) : QString{};
}

NSString* toNSString(const QString& string)
{
	const auto bytes = string.toUtf8();
	return [NSString stringWithUTF8String:bytes.constData()];
}

QString errorDescription(NSError* error, const QString& fallback)
{
	return error ? fromNSString(error.localizedDescription) : fallback;
}

NSObject* bookmarkStoreLock()
{
	static NSObject* lock = [NSObject new];
	return lock;
}

NSURL* bookmarkStoreURL()
{
	auto* app_support = [[NSFileManager.defaultManager URLsForDirectory:
		NSApplicationSupportDirectory inDomains:NSUserDomainMask] firstObject];
	return [app_support
		URLByAppendingPathComponent:@"MapperSecurityScopedBookmarks.plist"];
}

QString bookmarkKey(NSURL* url)
{
	return QDir::cleanPath(
		fromNSString(url.path).normalized(QString::NormalizationForm_D));
}

bool persistSecurityScopedBookmark(NSURL* url, QString* error_message = nullptr)
{
	if (!url || !url.isFileURL)
		return false;

	const bool scope_active = [url startAccessingSecurityScopedResource];
	NSError* bookmark_error = nil;
	auto* bookmark = [url bookmarkDataWithOptions:NSURLBookmarkCreationOptions(0)
		includingResourceValuesForKeys:nil
		relativeToURL:nil
		error:&bookmark_error];
	if (scope_active)
		[url stopAccessingSecurityScopedResource];
	if (!bookmark)
	{
		if (error_message)
		{
			*error_message = errorDescription(
				bookmark_error,
				QStringLiteral("Could not preserve access to the selected document."));
		}
		return false;
	}

	auto* file_manager = NSFileManager.defaultManager;
	auto* app_support = [[file_manager URLsForDirectory:
		NSApplicationSupportDirectory inDomains:NSUserDomainMask] firstObject];
	NSError* directory_error = nil;
	if (![file_manager createDirectoryAtURL:app_support
		withIntermediateDirectories:YES
		attributes:nil
		error:&directory_error])
	{
		if (error_message)
		{
			*error_message = errorDescription(
				directory_error,
				QStringLiteral("Could not create Mapper's document-access store."));
		}
		return false;
	}

	auto* bookmark_file = bookmarkStoreURL();
	@synchronized(bookmarkStoreLock())
	{
		NSError* read_error = nil;
		auto* existing = [NSDictionary dictionaryWithContentsOfURL:
			bookmark_file error:&read_error];
		if (!existing && [file_manager fileExistsAtPath:bookmark_file.path])
		{
			if (error_message)
			{
				*error_message = errorDescription(
					read_error,
					QStringLiteral("Mapper's document-access store could not be read."));
			}
			return false;
		}
		NSMutableDictionary* bookmarks = existing
		                               ? [existing mutableCopy]
		                               : [NSMutableDictionary dictionary];
		bookmarks[toNSString(bookmarkKey(url))] = bookmark;
		NSError* write_error = nil;
		if (![bookmarks writeToURL:bookmark_file error:&write_error])
		{
			if (error_message)
			{
				*error_message = errorDescription(
					write_error,
					QStringLiteral("Could not preserve access to the selected document."));
			}
			return false;
		}
	}
	return true;
}

void removeSecurityScopedBookmark(const QString& key)
{
	auto* bookmark_file = bookmarkStoreURL();
	@synchronized(bookmarkStoreLock())
	{
		NSError* read_error = nil;
		auto* existing = [NSDictionary dictionaryWithContentsOfURL:
			bookmark_file error:&read_error];
		if (!existing || !existing[toNSString(key)])
			return;
		NSMutableDictionary* bookmarks = [existing mutableCopy];
		[bookmarks removeObjectForKey:toNSString(key)];
		NSError* write_error = nil;
		(void)[bookmarks writeToURL:bookmark_file error:&write_error];
	}
}

NSURL* fileURL(const QString& path)
{
	const auto bytes = path.toUtf8();
	auto* raw_url = [NSURL fileURLWithPath:
		[NSString stringWithUTF8String:bytes.constData()]];
	const auto key = QDir::cleanPath(
		path.normalized(QString::NormalizationForm_D));
	NSData* bookmark_data = nil;
	@synchronized(bookmarkStoreLock())
	{
		NSError* read_error = nil;
		auto* bookmarks = [NSDictionary dictionaryWithContentsOfURL:
			bookmarkStoreURL() error:&read_error];
		id stored = bookmarks[toNSString(key)];
		if ([stored isKindOfClass:NSData.class])
			bookmark_data = [stored copy];
	}
	if (!bookmark_data)
		return raw_url;

	BOOL stale = NO;
	NSError* error = nil;
	auto* resolved_url = [NSURL URLByResolvingBookmarkData:bookmark_data
		options:NSURLBookmarkResolutionWithoutImplicitStartAccessing
		relativeToURL:nil
		bookmarkDataIsStale:&stale
		error:&error];
	if (!resolved_url || error)
		return raw_url;
	if (stale || bookmarkKey(resolved_url) != key)
	{
		QString refresh_error;
		// Keep the old key as an alias until the model adopts the resolved path;
		// an in-flight security-scope lease may still be balanced under it.
		(void)persistSecurityScopedBookmark(resolved_url, &refresh_error);
	}
	return resolved_url;
}

NSMutableDictionary<NSString*, NSURL*>* auxiliaryScopeURLs()
{
	static NSMutableDictionary<NSString*, NSURL*>* urls =
		[NSMutableDictionary new];
	return urls;
}

NSMutableDictionary<NSString*, NSNumber*>* auxiliaryScopeCounts()
{
	static NSMutableDictionary<NSString*, NSNumber*>* counts =
		[NSMutableDictionary new];
	return counts;
}

QString normalizedPathKey(const QString& path)
{
	return QDir::cleanPath(path.normalized(QString::NormalizationForm_D));
}

bool awaitDocumentOperation(
	const std::function<void(void (^)(BOOL success))>& start_operation)
{
	struct WaitState
	{
		std::atomic_bool finished{false};
		std::atomic_bool succeeded{false};
	};
	auto state = std::make_shared<WaitState>();
	QEventLoop event_loop;
	auto* loop = &event_loop;
	start_operation(^(BOOL success) {
		state->succeeded.store(success);
		// Enqueue the wakeup before publishing completion. If the operation
		// finishes synchronously, QEventLoop destruction safely removes the
		// queued call; if it races with exec(), that call wakes the loop.
		QMetaObject::invokeMethod(
			loop, &QEventLoop::quit, Qt::QueuedConnection);
		state->finished.store(true);
	});
	if (!state->finished.load())
		event_loop.exec(QEventLoop::ExcludeUserInputEvents);
	return state->succeeded.load();
}

UIViewController* activeViewController()
{
	UIWindow* selected_window = nil;
	for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![scene isKindOfClass:UIWindowScene.class]
		    || scene.activationState != UISceneActivationStateForegroundActive)
		{
			continue;
		}
		for (UIWindow* window in ((UIWindowScene*)scene).windows)
		{
			if (window.isKeyWindow)
			{
				selected_window = window;
				break;
			}
			if (!selected_window && !window.hidden)
				selected_window = window;
		}
		if (selected_window)
			break;
	}
	UIViewController* controller = selected_window.rootViewController;
	while (controller.presentedViewController
	       && !controller.presentedViewController.isBeingDismissed)
	{
		controller = controller.presentedViewController;
	}
	return controller;
}

}  // namespace


@interface MapperChoiceDelegate : NSObject <UIAdaptivePresentationControllerDelegate>
{
	QEventLoop* _eventLoop;
	NSInteger _selectedIndex;
	bool _finished;
}

- (instancetype)initWithEventLoop:(QEventLoop*)eventLoop;
- (void)selectIndex:(NSInteger)index;
- (void)finishSelection;
- (void)finishWithIndex:(NSInteger)index;
- (bool)isFinished;
- (NSInteger)selectedIndex;

@end


@implementation MapperChoiceDelegate

- (instancetype)initWithEventLoop:(QEventLoop*)eventLoop
{
	self = [super init];
	if (self)
	{
		_eventLoop = eventLoop;
		_selectedIndex = -1;
		_finished = false;
	}
	return self;
}

- (void)finishWithIndex:(NSInteger)index
{
	if (_finished)
		return;
	_selectedIndex = index;
	_finished = true;
	if (_eventLoop)
		_eventLoop->quit();
}

- (void)selectIndex:(NSInteger)index
{
	_selectedIndex = index;
}

- (void)finishSelection
{
	[self finishWithIndex:_selectedIndex];
}

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController
{
	Q_UNUSED(presentationController)
	[self finishSelection];
}

- (bool)isFinished
{
	return _finished;
}

- (NSInteger)selectedIndex
{
	return _selectedIndex;
}

@end


@interface MapperExportPickerDelegate : NSObject <UIDocumentPickerDelegate>
{
	QEventLoop* _eventLoop;
	NSURL* _selectedURL;
	bool _finished;
}

- (instancetype)initWithEventLoop:(QEventLoop*)eventLoop;
- (bool)isFinished;
- (NSURL*)selectedURL;

@end


@implementation MapperExportPickerDelegate

- (instancetype)initWithEventLoop:(QEventLoop*)eventLoop
{
	self = [super init];
	if (self)
	{
		_eventLoop = eventLoop;
		_finished = false;
	}
	return self;
}

- (void)finishWithURL:(NSURL*)url
{
	if (_finished)
		return;
	_selectedURL = url;
	_finished = true;
	if (_eventLoop)
		_eventLoop->quit();
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
	didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
	Q_UNUSED(controller)
	[self finishWithURL:urls.firstObject];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
	Q_UNUSED(controller)
	[self finishWithURL:nil];
}

- (bool)isFinished
{
	return _finished;
}

- (NSURL*)selectedURL
{
	return _selectedURL;
}

@end


@interface MapperUIDocument : UIDocument
{
	NSData* _loadedContents;
	NSData* _acknowledgedContents;
	NSData* _pendingContents;
	NSData* _expectedSelfSavedContents;
	NSError* _lastError;
	ChangeHandler _changeHandler;
	quint64 _presentationToken;
	std::atomic_bool _active;
	std::atomic_bool _mapperModified;
	std::atomic_bool _explicitSaveActive;
	std::atomic_bool _presentedChangeDuringSave;
	NSURL* _securityScopeURL;
	bool _securityScopeActive;
	NSArray<NSFileVersion*>* _approvedConflictVersions;
	quint64 _approvedConflictToken;
}

- (instancetype)initWithFileURL:(NSURL*)url
	changeHandler:(ChangeHandler)changeHandler
	presentationToken:(quint64)presentationToken
	errorMessage:(QString*)errorMessage;
- (void)setMapperModified:(bool)modified;
- (void)setExplicitSaveActive:(bool)active;
- (void)finishExplicitSave:(bool)saved;
- (bool)isMapperActive;
- (void)activateMapperPresentation;
- (void)deactivateMapperPresentation;
- (void)clearLastError;
- (QString)lastErrorMessage:(const QString&)fallback;
- (bool)writeLoadedContentsToPath:(const QString&)path errorMessage:(QString*)errorMessage;
- (QByteArray)acknowledgedContentsFingerprint;
- (void)acknowledgeLoadedContents;
- (bool)loadPendingContentsFromPath:(const QString&)path errorMessage:(QString*)errorMessage;
- (bool)replaceProviderContentsMatchingFingerprint:(const QByteArray&)fingerprint
	errorMessage:(QString*)errorMessage;
- (void)commitPendingContents;
- (void)discardPendingContents;
- (quint64)captureConflictVersions;
- (void)discardConflictVersions:(quint64)token;
- (bool)resolveConflictVersions:(quint64)token errorMessage:(QString*)errorMessage;
- (void)emitChange:(PresentedDocumentChange)change
	previousPath:(const QString&)previousPath
	path:(const QString&)path
	error:(const QString&)error;
- (void)processPresentedItemChange;

@end


static MapperUIDocument* active_document = nil;
static NSURL* pending_export_url = nil;
static bool pending_export_scope_active = false;
static std::atomic<quint64> next_presentation_token{1};
static std::atomic<quint64> next_conflict_token{1};


@implementation MapperUIDocument

- (instancetype)initWithFileURL:(NSURL*)url
	changeHandler:(ChangeHandler)changeHandler
	presentationToken:(quint64)presentationToken
	errorMessage:(QString*)errorMessage
{
	self = [super initWithFileURL:url];
	if (!self)
		return nil;
	_changeHandler = std::move(changeHandler);
	_presentationToken = presentationToken;
	_active.store(false);
	_mapperModified.store(false);
	_explicitSaveActive.store(false);
	_presentedChangeDuringSave.store(false);
	_approvedConflictToken = 0;
	const auto path = fromNSString(url.path);
	if (path.isEmpty())
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("The document provider returned an invalid file URL.");
		return nil;
	}
	_securityScopeURL = url;
	_securityScopeActive = [_securityScopeURL startAccessingSecurityScopedResource];
	[[NSNotificationCenter defaultCenter]
		addObserver:self
		selector:@selector(documentStateChanged:)
		name:UIDocumentStateChangedNotification
		object:self];
	return self;
}

- (void)dealloc
{
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	if (_securityScopeActive)
		[_securityScopeURL stopAccessingSecurityScopedResource];
}

- (id)contentsForType:(NSString*)typeName error:(NSError**)outError
{
	Q_UNUSED(typeName)
	Q_UNUSED(outError)
	@synchronized(self)
	{
		NSData* data = _pendingContents ? _pendingContents
		                                : (_loadedContents ? _loadedContents
		                                                   : [NSData data]);
		// UIDocument may autosave these bytes when the app resigns active,
		// outside Mapper's explicit-save bracket. Remember them so the ensuing
		// presenter notification is recognized as our own write.
		_expectedSelfSavedContents = [data copy];
		return data;
	}
}

- (BOOL)loadFromContents:(id)contents
	ofType:(NSString*)typeName
	error:(NSError**)outError
{
	Q_UNUSED(typeName)
	NSData* data = nil;
	if ([contents isKindOfClass:[NSData class]])
		data = contents;
	else if ([contents isKindOfClass:[NSFileWrapper class]])
		data = [contents regularFileContents];
	if (!data)
	{
		if (outError)
		{
			*outError = [NSError errorWithDomain:NSCocoaErrorDomain
			                                code:NSFileReadCorruptFileError
			                            userInfo:@{
				NSLocalizedDescriptionKey: @"The provider returned an unsupported document representation."
			}];
		}
		return NO;
	}
	@synchronized(self)
	{
		_loadedContents = [data copy];
		if (!_acknowledgedContents)
			_acknowledgedContents = [_loadedContents copy];
		_pendingContents = nil;
	}
	return YES;
}

- (void)handleError:(NSError*)error userInteractionPermitted:(BOOL)userInteractionPermitted
{
	@synchronized(self)
	{
		_lastError = [error copy];
	}
	[super handleError:error userInteractionPermitted:userInteractionPermitted];
}

- (void)setMapperModified:(bool)modified
{
	_mapperModified.store(modified);
}

- (void)setExplicitSaveActive:(bool)active
{
	if (active)
		_presentedChangeDuringSave.store(false);
	_explicitSaveActive.store(active);
}

- (void)finishExplicitSave:(bool)saved
{
	@synchronized(self)
	{
		_expectedSelfSavedContents = saved ? [_loadedContents copy] : nil;
	}
	_explicitSaveActive.store(false);
	if (_presentedChangeDuringSave.exchange(false))
		[self processPresentedItemChange];
}

- (bool)isMapperActive
{
	return _active.load();
}

- (void)activateMapperPresentation
{
	_active.store(true);
	if (self.documentState & UIDocumentStateInConflict)
	{
		const auto path = fromNSString(self.fileURL.path);
		[self emitChange:PresentedDocumentChange::Changed
		 previousPath:path
		 path:path
		 error:QStringLiteral("The document provider reported a conflicting version.")];
	}
}

- (void)deactivateMapperPresentation
{
	_active.store(false);
}

- (void)clearLastError
{
	@synchronized(self)
	{
		_lastError = nil;
	}
}

- (QString)lastErrorMessage:(const QString&)fallback
{
	@synchronized(self)
	{
		return errorDescription(_lastError, fallback);
	}
}

- (bool)writeLoadedContentsToPath:(const QString&)path errorMessage:(QString*)errorMessage
{
	NSData* contents = nil;
	@synchronized(self)
	{
		contents = _loadedContents;
	}
	if (!contents)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("The document provider returned no contents.");
		return false;
	}
	NSError* error = nil;
	const auto bytes = path.toUtf8();
	const bool success = [contents writeToFile:
		[NSString stringWithUTF8String:bytes.constData()]
		options:NSDataWritingAtomic
		error:&error];
	if (!success && errorMessage)
		*errorMessage = errorDescription(error, QStringLiteral("Could not create the local document snapshot."));
	return success;
}

- (QByteArray)acknowledgedContentsFingerprint
{
	NSData* contents = nil;
	@synchronized(self)
	{
		contents = [_acknowledgedContents copy];
	}
	if (!contents)
		return {};
	return QCryptographicHash::hash(
		QByteArrayView{
			reinterpret_cast<const char*>(contents.bytes),
			qsizetype(contents.length)},
		QCryptographicHash::Sha256);
}

- (void)acknowledgeLoadedContents
{
	@synchronized(self)
	{
		_acknowledgedContents = [_loadedContents copy];
	}
}

- (bool)loadPendingContentsFromPath:(const QString&)path errorMessage:(QString*)errorMessage
{
	NSError* error = nil;
	NSData* contents = [NSData dataWithContentsOfURL:fileURL(path)
	                                      options:NSDataReadingMappedIfSafe
	                                        error:&error];
	if (!contents)
	{
		if (errorMessage)
			*errorMessage = errorDescription(error, QStringLiteral("Could not read the local save snapshot."));
		return false;
	}
	@synchronized(self)
	{
		_pendingContents = contents;
	}
	return true;
}

- (bool)replaceProviderContentsMatchingFingerprint:(const QByteArray&)fingerprint
	errorMessage:(QString*)errorMessage
{
	NSData* pending_contents = nil;
	@synchronized(self)
	{
		pending_contents = [_pendingContents copy];
	}
	if (!pending_contents || fingerprint.isEmpty())
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral(
				"Mapper has no verified provider generation for this save.");
		}
		return false;
	}

	__block BOOL replaced = NO;
	__block NSError* read_error = nil;
	__block NSError* write_error = nil;
	__block bool provider_changed = false;
	NSError* coordination_error = nil;
	auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:self];
	[coordinator coordinateWritingItemAtURL:self.fileURL
		options:NSFileCoordinatorWritingForReplacing
		error:&coordination_error
		byAccessor:^(NSURL* coordinated_url) {
			auto* current_contents = [NSData dataWithContentsOfURL:coordinated_url
				options:NSDataReadingMappedIfSafe
				error:&read_error];
			if (!current_contents)
				return;
			const auto current_fingerprint = QCryptographicHash::hash(
				QByteArrayView{
					reinterpret_cast<const char*>(current_contents.bytes),
					qsizetype(current_contents.length)},
				QCryptographicHash::Sha256);
			if (current_fingerprint != fingerprint)
			{
				provider_changed = true;
				return;
			}
			replaced = [pending_contents writeToURL:coordinated_url
				options:NSDataWritingAtomic
				error:&write_error];
		}];
	if (!replaced && errorMessage)
	{
		if (provider_changed)
		{
			*errorMessage = QStringLiteral(
				"The document changed again after overwrite was approved. Mapper did not replace that newer version.");
		}
		else
		{
			*errorMessage = errorDescription(
				coordination_error ? coordination_error
				                   : (read_error ? read_error : write_error),
				QStringLiteral("The document provider could not complete the verified save."));
		}
	}
	return replaced;
}

- (void)commitPendingContents
{
	@synchronized(self)
	{
		_loadedContents = _pendingContents;
		_acknowledgedContents = [_loadedContents copy];
		_pendingContents = nil;
	}
}

- (void)discardPendingContents
{
	@synchronized(self)
	{
		_pendingContents = nil;
	}
}

- (quint64)captureConflictVersions
{
	auto* versions = [NSFileVersion
		unresolvedConflictVersionsOfItemAtURL:self.fileURL];
	@synchronized(self)
	{
		_approvedConflictVersions = versions.count ? [versions copy] : nil;
		_approvedConflictToken = versions.count
		                         ? next_conflict_token.fetch_add(1)
		                         : 0;
		return _approvedConflictToken;
	}
}

- (void)discardConflictVersions:(quint64)token
{
	@synchronized(self)
	{
		if (token && token == _approvedConflictToken)
		{
			_approvedConflictVersions = nil;
			_approvedConflictToken = 0;
		}
	}
}

- (bool)resolveConflictVersions:(quint64)token errorMessage:(QString*)errorMessage
{
	NSArray<NSFileVersion*>* approved_versions = nil;
	@synchronized(self)
	{
		if (!token || token != _approvedConflictToken)
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("The approved provider conflict set expired before it could be resolved.");
			return false;
		}
		approved_versions = [_approvedConflictVersions copy];
	}

	bool success = true;
	for (NSFileVersion* approved_version in approved_versions)
	{
		auto* version = [NSFileVersion versionOfItemAtURL:self.fileURL
			forPersistentIdentifier:approved_version.persistentIdentifier];
		if (!version || !version.isConflict || version.isResolved)
			continue;
		version.resolved = YES;
		NSError* error = nil;
		if (![version removeAndReturnError:&error])
		{
			success = false;
			if (errorMessage)
			{
				*errorMessage = errorDescription(
					error,
					QStringLiteral("The provider saved the current document but could not remove an approved conflicting version."));
			}
			break;
		}
	}
	[self discardConflictVersions:token];
	if (success
	    && [NSFileVersion unresolvedConflictVersionsOfItemAtURL:self.fileURL].count)
	{
		success = false;
		if (errorMessage)
		{
			*errorMessage = QStringLiteral(
				"A new provider conflict appeared while the document was saving. Mapper left that version unresolved.");
		}
	}
	return success;
}

- (void)emitChange:(PresentedDocumentChange)change
	previousPath:(const QString&)previousPath
	path:(const QString&)path
	error:(const QString&)error
{
	if (![self isMapperActive])
		return;
	auto handler = _changeHandler;
	if (handler)
		handler({change, _presentationToken, previousPath, path, error});
}

- (void)documentStateChanged:(NSNotification*)notification
{
	Q_UNUSED(notification)
	if (![self isMapperActive])
		return;
	const auto state = self.documentState;
	if (state & UIDocumentStateInConflict)
	{
		const auto path = fromNSString(self.fileURL.path);
		[self emitChange:PresentedDocumentChange::Changed
		 previousPath:path
		 path:path
		 error:QStringLiteral("The document provider reported a conflicting version.")];
	}
	else if (state & UIDocumentStateEditingDisabled)
	{
		const auto path = fromNSString(self.fileURL.path);
		[self emitChange:PresentedDocumentChange::Changed
		 previousPath:path
		 path:path
		 error:QStringLiteral("The document provider temporarily disabled editing.")];
	}
}

- (void)presentedItemDidChange
{
	[super presentedItemDidChange];
	if (![self isMapperActive])
		return;
	if (_explicitSaveActive.load())
	{
		_presentedChangeDuringSave.store(true);
		if (_explicitSaveActive.load())
			return;
		_presentedChangeDuringSave.store(false);
	}
	[self processPresentedItemChange];
}

- (void)processPresentedItemChange
{
	NSData* expected_contents = nil;
	@synchronized(self)
	{
		expected_contents = _expectedSelfSavedContents;
	}
	if (expected_contents)
	{
		NSError* error = nil;
		auto* current_contents = [NSData dataWithContentsOfURL:self.fileURL
			options:NSDataReadingMappedIfSafe
			error:&error];
		if (current_contents && !error
		    && [current_contents isEqualToData:expected_contents])
		{
			// UIDocument providers may coalesce or repeat self-write callbacks.
			// Keep the fingerprint until content actually differs.
			return;
		}
		@synchronized(self)
		{
			_expectedSelfSavedContents = nil;
		}
	}
	const auto path = fromNSString(self.fileURL.path);
	[self emitChange:PresentedDocumentChange::Changed
	 previousPath:path
	 path:path
	 error:QString{}];
}

- (void)presentedItemDidMoveToURL:(NSURL*)newURL
{
	const auto old_path = fromNSString(self.fileURL.path);
	const auto new_path = fromNSString(newURL.path);
	const bool replacement_scope_active = [newURL startAccessingSecurityScopedResource];
	NSURL* previous_scope_url = _securityScopeURL;
	const bool previous_scope_active = _securityScopeActive;
	QString bookmark_error;
	if (persistSecurityScopedBookmark(newURL, &bookmark_error))
	{
		removeSecurityScopedBookmark(QDir::cleanPath(
			old_path.normalized(QString::NormalizationForm_D)));
	}
	[super presentedItemDidMoveToURL:newURL];
	_securityScopeURL = newURL;
	_securityScopeActive = replacement_scope_active;
	if (previous_scope_active)
		[previous_scope_url stopAccessingSecurityScopedResource];
	[self emitChange:PresentedDocumentChange::Moved
	 previousPath:old_path
	 path:new_path
	 error:bookmark_error];
}

- (void)accommodatePresentedItemDeletionWithCompletionHandler:
	(void (^)(NSError* _Nullable errorOrNil))completionHandler
{
	const auto path = fromNSString(self.fileURL.path);
	[super accommodatePresentedItemDeletionWithCompletionHandler:^(NSError* error) {
		if (error)
		{
			[self emitChange:PresentedDocumentChange::Changed
			 previousPath:path
			 path:path
			 error:errorDescription(error, QString{})];
		}
		else
		{
			[self emitChange:PresentedDocumentChange::Deleted
			 previousPath:path
			 path:QString{}
			 error:QString{}];
		}
		completionHandler(error);
	}];
}

- (void)savePresentedItemChangesWithCompletionHandler:
	(void (^)(NSError* _Nullable errorOrNil))completionHandler
{
	if (!_mapperModified.load())
	{
		[super savePresentedItemChangesWithCompletionHandler:completionHandler];
		return;
	}
	// Mapper's model is QWidget-owned and cannot be serialized on the file
	// presenter's queue. Preserve it and require the explicit GUI save flow.
	completionHandler([NSError errorWithDomain:NSCocoaErrorDomain
	                                     code:NSFileWriteUnknownError
	                                 userInfo:@{
		NSLocalizedDescriptionKey: @"The document has unsaved changes in Mapper."
	}]);
}

@end


namespace OpenOrienteering::AppleDocumentAccess {

namespace {

MapperUIDocument* makeDocument(
	NSURL* url,
	ChangeHandler change_handler,
	quint64* presentation_token,
	QString* error_message)
{
	const auto token = next_presentation_token.fetch_add(1);
	auto* document = [[MapperUIDocument alloc]
		initWithFileURL:url
		changeHandler:std::move(change_handler)
		presentationToken:token
		errorMessage:error_message];
	if (document && presentation_token)
		*presentation_token = token;
	return document;
}

MapperUIDocument* makeDocument(
	const QString& path,
	ChangeHandler change_handler,
	quint64* presentation_token,
	QString* error_message)
{
	return makeDocument(
		fileURL(path), std::move(change_handler), presentation_token, error_message);
}

void clearPendingExport()
{
	if (pending_export_scope_active)
		[pending_export_url stopAccessingSecurityScopedResource];
	pending_export_scope_active = false;
	pending_export_url = nil;
}

void closeDocument(MapperUIDocument* document)
{
	if (!document)
		return;
	[document deactivateMapperPresentation];
	if (!(document.documentState & UIDocumentStateClosed))
	{
		awaitDocumentOperation([document](void (^completion)(BOOL)) {
			[document closeWithCompletionHandler:completion];
		});
	}
}

bool matchesActiveDocument(const QString& requested_path)
{
	if (!active_document || ![active_document isMapperActive])
		return false;
	const auto active_path = QFileInfo{fromNSString(active_document.fileURL.path)}.absoluteFilePath();
	const auto requested = QFileInfo{requested_path}.absoluteFilePath();
	return active_path == requested;
}

QString privateRecoveryDirectory(const QString& document_path, bool create)
{
	const auto app_data = QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation);
	if (app_data.isEmpty() || document_path.isEmpty())
		return {};
	const auto identity = QDir::cleanPath(document_path)
	                      .normalized(QString::NormalizationForm_D);
	const auto digest = QCryptographicHash::hash(
		identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
	const auto directory = QDir{app_data}.filePath(
		QLatin1String("Template Recovery/") + QString::fromLatin1(digest));
	return !create || QDir{}.mkpath(directory) ? directory : QString{};
}

bool copyFileAtomically(const QString& source_path,
	                    const QString& destination_path,
	                    QString* error_message)
{
	QFile source{source_path};
	QSaveFile destination{destination_path};
	if (!source.open(QIODevice::ReadOnly)
	    || !destination.open(QIODevice::WriteOnly))
	{
		if (error_message)
		{
			*error_message = source.isOpen()
			                 ? destination.errorString()
			                 : source.errorString();
		}
		return false;
	}
	QByteArray buffer(1024 * 1024, Qt::Uninitialized);
	while (!source.atEnd())
	{
		const auto count = source.read(buffer.data(), buffer.size());
		if (count < 0 || destination.write(buffer.constData(), count) != count)
		{
			if (error_message)
				*error_message = count < 0 ? source.errorString() : destination.errorString();
			return false;
		}
	}
	if (!destination.commit())
	{
		if (error_message)
			*error_message = destination.errorString();
		return false;
	}
	return true;
}

}  // namespace

bool chooseDocumentToOpen(const QString& title,
	                      QString* selected_path,
	                      QString* error_message)
{
	if (selected_path)
		selected_path->clear();
	if (error_message)
		error_message->clear();
	if (![NSThread isMainThread])
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"The native document picker must run on the UI thread.");
		}
		return false;
	}
	auto* presenter = activeViewController();
	if (!presenter)
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"Mapper could not find an active window for the Files picker.");
		}
		return false;
	}

	QEventLoop event_loop;
	auto* delegate = [[MapperExportPickerDelegate alloc]
		initWithEventLoop:&event_loop];
	auto* picker = [[UIDocumentPickerViewController alloc]
		initForOpeningContentTypes:@[UTTypeData]
		asCopy:NO];
	picker.delegate = delegate;
	picker.allowsMultipleSelection = NO;
	picker.shouldShowFileExtensions = YES;
	picker.modalPresentationStyle = UIModalPresentationFormSheet;
	picker.navigationItem.title = toNSString(title);
	[presenter presentViewController:picker animated:YES completion:nil];
	if (![delegate isFinished])
		event_loop.exec(QEventLoop::ExcludeUserInputEvents);
	auto* selected_url = [delegate selectedURL];
	if (!selected_url)
		return false;

	QString bookmark_error;
	if (!persistSecurityScopedBookmark(selected_url, &bookmark_error))
	{
		if (error_message)
			*error_message = bookmark_error;
		return false;
	}
	if (selected_path)
		*selected_path = fromNSString(selected_url.path);
	return selected_path ? !selected_path->isEmpty() : true;
}

bool beginAuxiliaryDocumentAccess(const QString& path)
{
	if (path.isEmpty())
		return false;
	const auto key = toNSString(normalizedPathKey(path));
	@synchronized(bookmarkStoreLock())
	{
		if (auto* count = auxiliaryScopeCounts()[key])
		{
			auxiliaryScopeCounts()[key] = @(count.unsignedIntegerValue + 1);
			return true;
		}
	}

	auto* url = fileURL(path);
	if (![url startAccessingSecurityScopedResource])
		return false;
	@synchronized(bookmarkStoreLock())
	{
		if (auto* count = auxiliaryScopeCounts()[key])
		{
			auxiliaryScopeCounts()[key] = @(count.unsignedIntegerValue + 1);
			[url stopAccessingSecurityScopedResource];
		}
		else
		{
			auxiliaryScopeURLs()[key] = url;
			auxiliaryScopeCounts()[key] = @1;
		}
	}
	return true;
}

void endAuxiliaryDocumentAccess(const QString& path)
{
	if (path.isEmpty())
		return;
	const auto key = toNSString(normalizedPathKey(path));
	@synchronized(bookmarkStoreLock())
	{
		auto* count = auxiliaryScopeCounts()[key];
		if (!count)
			return;
		if (count.unsignedIntegerValue > 1)
		{
			auxiliaryScopeCounts()[key] = @(count.unsignedIntegerValue - 1);
			return;
		}
		auto* url = auxiliaryScopeURLs()[key];
		[url stopAccessingSecurityScopedResource];
		[auxiliaryScopeURLs() removeObjectForKey:key];
		[auxiliaryScopeCounts() removeObjectForKey:key];
	}
}

QString resolvedAuxiliaryDocumentPath(const QString& path)
{
	if (path.isEmpty())
		return {};
	const auto resolved = fromNSString(fileURL(path).path);
	return resolved.isEmpty()
	       ? path
	       : QFileInfo{resolved}.absoluteFilePath();
}

bool writeAuxiliaryDocument(const QString& path,
	                        const QString& local_snapshot_path,
	                        const QByteArray& expected_fingerprint,
	                        QByteArray* committed_fingerprint,
	                        QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (committed_fingerprint)
		committed_fingerprint->clear();
	if (path.isEmpty() || local_snapshot_path.isEmpty()
	    || expected_fingerprint.isEmpty())
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"Mapper cannot verify that this template is unchanged. Save a copy instead.");
		}
		return false;
	}
	NSError* read_error = nil;
	auto* contents = [NSData dataWithContentsOfURL:fileURL(local_snapshot_path)
		options:NSDataReadingMappedIfSafe
		error:&read_error];
	if (!contents)
	{
		if (error_message)
		{
			*error_message = errorDescription(
				read_error,
				QStringLiteral("Could not read the private template snapshot."));
		}
		return false;
	}

	auto* destination_url = fileURL(path);
	const bool scope_active =
		[destination_url startAccessingSecurityScopedResource];
	__block BOOL wrote = NO;
	__block NSError* write_error = nil;
	NSError* coordination_error = nil;
	auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
	const auto expected = expected_fingerprint;
	[coordinator coordinateWritingItemAtURL:destination_url
		options:NSFileCoordinatorWritingForReplacing
		error:&coordination_error
		byAccessor:^(NSURL* coordinated_url) {
			if ([NSFileVersion unresolvedConflictVersionsOfItemAtURL:
					coordinated_url].count > 0)
			{
				write_error = [NSError errorWithDomain:@"org.openorienteering.Mapper"
					code:2
					userInfo:@{NSLocalizedDescriptionKey:
						@"The template has conflicting provider versions. Resolve them in Files or save a copy."}];
				return;
			}
			NSError* current_error = nil;
			auto* current_contents = [NSData dataWithContentsOfURL:coordinated_url
				options:NSDataReadingMappedIfSafe
				error:&current_error];
			if (!current_contents)
			{
				write_error = current_error;
				return;
			}
			const auto current_fingerprint = QCryptographicHash::hash(
				QByteArrayView{
					reinterpret_cast<const char*>(current_contents.bytes),
					qsizetype(current_contents.length)},
				QCryptographicHash::Sha256);
			if (current_fingerprint != expected)
			{
				write_error = [NSError errorWithDomain:@"org.openorienteering.Mapper"
					code:1
					userInfo:@{NSLocalizedDescriptionKey:
						@"The template changed outside Mapper after it was loaded. Reload it or save a copy."}];
				return;
			}
			wrote = [contents writeToURL:coordinated_url
				options:NSDataWritingAtomic
				error:&write_error];
		}];
	if (scope_active)
		[destination_url stopAccessingSecurityScopedResource];
	if (!wrote)
	{
		if (error_message)
		{
			*error_message = errorDescription(
				coordination_error ? coordination_error : write_error,
				QStringLiteral("The provider could not safely replace the template."));
		}
		return false;
	}
	if (committed_fingerprint)
	{
		*committed_fingerprint = QCryptographicHash::hash(
			QByteArrayView{
				reinterpret_cast<const char*>(contents.bytes),
				qsizetype(contents.length)},
			QCryptographicHash::Sha256);
	}
	return true;
}

bool readAuxiliaryDocument(const QString& path,
	                       AuxiliaryReadHandler reader,
	                       QByteArray* fingerprint,
	                       QString* error_message)
{
	if (fingerprint)
		fingerprint->clear();
	if (error_message)
		error_message->clear();
	if (path.isEmpty() || !reader || !fingerprint)
		return false;

	auto* url = fileURL(path);
	const bool scope_active = [url startAccessingSecurityScopedResource];
	__block BOOL reader_succeeded = NO;
	__block NSData* contents = nil;
	__block NSError* read_error = nil;
	__block QByteArray coordinated_fingerprint;
	NSError* coordination_error = nil;
	auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
	[coordinator coordinateReadingItemAtURL:url
		options:NSFileCoordinatorReadingWithoutChanges
		error:&coordination_error
		byAccessor:^(NSURL* coordinated_url) {
			reader_succeeded = reader(fromNSString(coordinated_url.path));
			if (!reader_succeeded)
				return;
			contents = [NSData dataWithContentsOfURL:coordinated_url
				options:NSDataReadingMappedIfSafe
				error:&read_error];
			if (contents)
			{
				coordinated_fingerprint = QCryptographicHash::hash(
					QByteArrayView{
						reinterpret_cast<const char*>(contents.bytes),
						qsizetype(contents.length)},
					QCryptographicHash::Sha256);
			}
		}];
	if (scope_active)
		[url stopAccessingSecurityScopedResource];
	if (!reader_succeeded)
	{
		if (error_message && coordination_error)
		{
			*error_message = errorDescription(
				coordination_error,
				QStringLiteral("Could not coordinate the selected template read."));
		}
		return false;
	}
	if (!contents)
	{
		if (error_message)
		{
			*error_message = errorDescription(
				coordination_error ? coordination_error : read_error,
				QStringLiteral("Could not fingerprint the selected template."));
		}
		return false;
	}
	*fingerprint = coordinated_fingerprint;
	return true;
}

bool snapshotAuxiliaryDocument(const QString& path,
	                           const QString& local_snapshot_path,
	                           QByteArray* fingerprint,
	                           QString* resolved_path,
	                           QString* error_message)
{
	if (fingerprint)
		fingerprint->clear();
	if (resolved_path)
		resolved_path->clear();
	if (error_message)
		error_message->clear();
	if (path.isEmpty() || local_snapshot_path.isEmpty() || !fingerprint)
		return false;

	auto* source_url = fileURL(path);
	const QFileInfo source_info{fromNSString(source_url.path)};
	const auto source_extension = source_info.suffix().toLower();
	const auto candidate_path = QDir::cleanPath(source_info.absoluteFilePath());
	const auto belongs_to = [&candidate_path](const QString& root) {
		const auto normalized_root = QDir::cleanPath(root);
		return !normalized_root.isEmpty()
		       && (candidate_path == normalized_root
		           || candidate_path.startsWith(
			           normalized_root + QLatin1Char('/')));
	};
	const bool application_owned =
		belongs_to(QStandardPaths::writableLocation(
			QStandardPaths::AppDataLocation))
		|| belongs_to(fromNSString(NSTemporaryDirectory()))
		|| belongs_to(fromNSString(NSBundle.mainBundle.bundlePath));
	// A file-scoped Files grant cannot prove that siblings are absent. Fail
	// closed to formats whose useful dataset is carried by one selected document.
	const QStringList self_contained_extensions{
		QStringLiteral("ocd"), QStringLiteral("omap"),
		QStringLiteral("xmap"), QStringLiteral("gpx"),
		QStringLiteral("gpkg"), QStringLiteral("geojson"),
		QStringLiteral("json"), QStringLiteral("geojsonl"),
		QStringLiteral("geojsons")};
	if (!application_owned
	    && !self_contained_extensions.contains(source_extension))
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"Mapper on iOS accepts external templates only when the complete "
				"dataset is one durable Files document: Mapper/OCD maps, GPX, "
				"GeoPackage, or GeoJSON. Convert this template to one of those "
				"formats; compound datasets, referenced files, and optional sidecars "
				"are not imported from a file-scoped grant.");
		}
		return false;
	}
	if (resolved_path)
		*resolved_path = fromNSString(source_url.path);
	const bool scope_active =
		[source_url startAccessingSecurityScopedResource];
	__block BOOL copied = NO;
	__block NSError* read_error = nil;
	__block NSError* write_error = nil;
	__block QByteArray coordinated_fingerprint;
	NSError* coordination_error = nil;
	auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
	[coordinator coordinateReadingItemAtURL:source_url
		options:NSFileCoordinatorReadingForUploading
		error:&coordination_error
		byAccessor:^(NSURL* coordinated_url) {
			auto* contents = [NSData dataWithContentsOfURL:coordinated_url
				options:NSDataReadingMappedIfSafe
				error:&read_error];
			if (!contents)
				return;
			coordinated_fingerprint = QCryptographicHash::hash(
				QByteArrayView{
					reinterpret_cast<const char*>(contents.bytes),
					qsizetype(contents.length)},
				QCryptographicHash::Sha256);
			auto* snapshot_url = [NSURL fileURLWithPath:
				toNSString(local_snapshot_path)];
			copied = [contents writeToURL:snapshot_url
				options:NSDataWritingAtomic
				error:&write_error];
		}];
	if (scope_active)
		[source_url stopAccessingSecurityScopedResource];
	if (!copied)
	{
		if (error_message)
		{
			*error_message = errorDescription(
				coordination_error ? coordination_error
				                   : (read_error ? read_error : write_error),
				QStringLiteral("Could not make a stable copy of the selected template."));
		}
		return false;
	}
	*fingerprint = coordinated_fingerprint;
	return true;
}

bool fingerprintAuxiliaryDocument(const QString& path,
	                              QByteArray* fingerprint,
	                              QString* error_message)
{
	return readAuxiliaryDocument(
		path,
		[](const QString&) { return true; },
		fingerprint,
		error_message);
}

bool hasPersistedDocumentAccess(const QString& path)
{
	if (path.isEmpty())
		return false;
	const auto key = toNSString(normalizedPathKey(path));
	@synchronized(bookmarkStoreLock())
	{
		NSError* read_error = nil;
		auto* bookmarks = [NSDictionary dictionaryWithContentsOfURL:
			bookmarkStoreURL() error:&read_error];
		return [bookmarks[key] isKindOfClass:NSData.class];
	}
}

bool exportAuxiliaryDocument(const QString& local_snapshot_path,
	                         QString* exported_path,
	                         QString* error_message)
{
	const bool exported = exportDocument(
		local_snapshot_path, exported_path, nullptr, error_message);
	abandonExportedDocument();
	return exported;
}

QString privateAuxiliaryDraftDirectory(const QString& document_path)
{
	const auto app_data = QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation);
	if (app_data.isEmpty() || document_path.isEmpty())
		return {};
	const auto identity = QDir::cleanPath(document_path)
	                      .normalized(QString::NormalizationForm_D);
	const auto digest = QCryptographicHash::hash(
		identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
	const auto directory = QDir{app_data}.filePath(
		QLatin1String("Template Drafts/") + QString::fromLatin1(digest));
	return QDir{}.mkpath(directory) ? directory : QString{};
}

bool isPrivateAuxiliaryDraft(const QString& path)
{
	const auto app_data = QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation);
	if (app_data.isEmpty() || path.isEmpty())
		return false;
	const auto draft_root = QDir::cleanPath(
		QDir{app_data}.filePath(QLatin1String("Template Drafts")));
	const auto candidate = QDir::cleanPath(QFileInfo{path}.absoluteFilePath());
	return candidate == draft_root
	       || candidate.startsWith(draft_root + QLatin1Char('/'));
}

QString privateAuxiliaryRecoveryPath(const QString& document_path,
	                                 const QString& resource_identity,
	                                 const QString& template_path)
{
	if (template_path.isEmpty())
		return {};
	const auto template_digest = QCryptographicHash::hash(
		(resource_identity.isEmpty()
		 ? QDir::cleanPath(template_path)
			.normalized(QString::NormalizationForm_D)
		 : resource_identity).toUtf8(),
		QCryptographicHash::Sha256).toHex().left(16);
	const auto directory = privateRecoveryDirectory(document_path, true);
	if (directory.isEmpty())
		return {};
	const auto digest_name = QString::fromLatin1(template_digest);
	if (!resource_identity.isEmpty())
	{
		const QDir recovery_directory{directory};
		const auto exact_path = recovery_directory.filePath(digest_name);
		if (QFileInfo::exists(exact_path))
			return exact_path;
		const auto existing_receipts = recovery_directory.entryList(
			QStringList{digest_name + QLatin1String(".*")},
			QDir::Files | QDir::NoDotAndDotDot,
			QDir::Name);
		if (!existing_receipts.isEmpty())
			return recovery_directory.filePath(existing_receipts.constFirst());
	}
	auto suffix = QFileInfo{template_path}.suffix();
	if (!suffix.isEmpty())
		suffix.prepend(QLatin1Char('.'));
	return QDir{directory}.filePath(
		digest_name + suffix);
}

void discardPrivateAuxiliaryRecovery(const QString& document_path,
	                                 const QString& resource_identity,
	                                 const QString& template_path)
{
	const auto recovery_path = privateAuxiliaryRecoveryPath(
		document_path, resource_identity, template_path);
	if (!recovery_path.isEmpty())
		QFile::remove(recovery_path);
}

bool migratePrivateAuxiliaryRecovery(const QString& old_document_path,
	                                 const QString& new_document_path,
	                                 QString* error_message)
{
	if (error_message)
		error_message->clear();
	const auto source_directory =
		privateRecoveryDirectory(old_document_path, false);
	const auto destination_directory =
		privateRecoveryDirectory(new_document_path, true);
	if (source_directory.isEmpty() || destination_directory.isEmpty())
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"Could not resolve Mapper's private template recovery directory.");
		}
		return false;
	}
	if (source_directory == destination_directory
	    || !QFileInfo::exists(source_directory))
	{
		return true;
	}

	const QDir source{source_directory};
	for (const auto& name : source.entryList(QDir::Files | QDir::NoDotAndDotDot))
	{
		if (!copyFileAtomically(
				source.filePath(name),
				QDir{destination_directory}.filePath(name),
				error_message))
		{
			return false;
		}
	}
	if (!QDir{source_directory}.removeRecursively())
	{
		qWarning("Could not remove migrated iOS template recovery directory: %s",
		         qUtf8Printable(source_directory));
	}
	return true;
}

int chooseDocumentFormat(const QString& title,
	                     const QStringList& options,
	                     int preferred_index,
	                     const QString& cancel_title)
{
	if (options.isEmpty() || ![NSThread isMainThread])
		return -1;
	auto* presenter = activeViewController();
	if (!presenter)
		return -1;

	QEventLoop event_loop;
	auto* delegate = [[MapperChoiceDelegate alloc] initWithEventLoop:&event_loop];
	auto* alert = [UIAlertController alertControllerWithTitle:toNSString(title)
		message:nil
		preferredStyle:UIAlertControllerStyleActionSheet];
	__weak UIAlertController* weak_alert = alert;
	UIAlertAction* preferred_action = nil;
	for (NSInteger index = 0; index < options.size(); ++index)
	{
		const NSInteger action_index = index;
		auto* action = [UIAlertAction actionWithTitle:toNSString(options.at(index))
			style:UIAlertActionStyleDefault
			handler:^(UIAlertAction*) {
				[delegate selectIndex:action_index];
				[weak_alert dismissViewControllerAnimated:YES completion:^{
					[delegate finishSelection];
				}];
			}];
		[alert addAction:action];
		if (index == preferred_index)
			preferred_action = action;
	}
	[alert addAction:[UIAlertAction actionWithTitle:toNSString(cancel_title)
		style:UIAlertActionStyleCancel
		handler:^(UIAlertAction*) {
			[delegate selectIndex:-1];
			[weak_alert dismissViewControllerAnimated:YES completion:^{
				[delegate finishSelection];
			}];
		}]];
	alert.preferredAction = preferred_action;
	alert.modalInPresentation = YES;
	if (auto* popover = alert.popoverPresentationController)
	{
		popover.sourceView = presenter.view;
		popover.sourceRect = CGRectMake(
			CGRectGetMidX(presenter.view.bounds),
			CGRectGetMidY(presenter.view.bounds),
			1,
			1);
		popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
	}
	[presenter presentViewController:alert animated:YES completion:nil];
	alert.presentationController.delegate = delegate;
	if (![delegate isFinished])
		event_loop.exec(QEventLoop::ExcludeUserInputEvents);
	return int([delegate selectedIndex]);
}

bool openDocument(const QString& path,
	              const QString& local_snapshot_path,
	              ChangeHandler change_handler,
	              quint64* presentation_token,
	              QString* coordinated_path,
	              QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (presentation_token)
		*presentation_token = 0;
	if (path.isEmpty() || local_snapshot_path.isEmpty())
		return false;

	auto* candidate = makeDocument(
		path, std::move(change_handler), presentation_token, error_message);
	if (!candidate)
		return false;
	[candidate clearLastError];
	const bool opened = awaitDocumentOperation([candidate](void (^completion)(BOOL)) {
		[candidate openWithCompletionHandler:completion];
	});
	if (!opened
	    || ![candidate writeLoadedContentsToPath:local_snapshot_path
		                               errorMessage:error_message])
	{
		if (!opened && error_message)
		{
			*error_message = [candidate lastErrorMessage:
				QStringLiteral("The document provider could not open this document.")];
		}
		closeDocument(candidate);
		if (presentation_token)
			*presentation_token = 0;
		return false;
	}

	auto* previous = active_document;
	active_document = candidate;
	[candidate activateMapperPresentation];
	closeDocument(previous);
	if (coordinated_path)
		*coordinated_path = fromNSString(candidate.fileURL.path);
	return true;
}

bool readPresentedDocument(const QString& path,
	                       const QString& local_snapshot_path,
	                       QString* coordinated_path,
	                       QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (!matchesActiveDocument(path))
	{
		if (error_message)
			*error_message = QStringLiteral("The document moved or closed before it could be reloaded.");
		return false;
	}

	[active_document clearLastError];
	const bool reverted = awaitDocumentOperation([](void (^completion)(BOOL)) {
		[active_document revertToContentsOfURL:active_document.fileURL
		                     completionHandler:completion];
	});
	if (!reverted
	    || ![active_document writeLoadedContentsToPath:local_snapshot_path
		                                    errorMessage:error_message])
	{
		if (!reverted && error_message)
		{
			*error_message = [active_document lastErrorMessage:
				QStringLiteral("The document provider could not reload this document.")];
		}
		return false;
	}
	[active_document acknowledgeLoadedContents];
	if (coordinated_path)
		*coordinated_path = fromNSString(active_document.fileURL.path);
	return true;
}

bool capturePresentedDocumentWriteReceipt(const QString& path,
	                                      bool accept_current_provider,
	                                      QByteArray* fingerprint,
	                                      QString* error_message)
{
	if (fingerprint)
		fingerprint->clear();
	if (!matchesActiveDocument(path))
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"The document moved or closed before Mapper could verify its current version.");
		}
		return false;
	}
	if (accept_current_provider)
		return fingerprintAuxiliaryDocument(path, fingerprint, error_message);
	*fingerprint = [active_document acknowledgedContentsFingerprint];
	if (!fingerprint->isEmpty())
		return true;
	if (error_message)
	{
		*error_message = QStringLiteral(
			"Mapper has no loaded provider generation to verify before saving.");
	}
	return false;
}

bool writePresentedDocument(const QString& path,
	                        const QString& local_snapshot_path,
	                        const QByteArray& expected_fingerprint,
	                        quint64 conflict_resolution_token,
	                        QString* coordinated_path,
	                        QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (!matchesActiveDocument(path))
	{
		if (error_message)
			*error_message = QStringLiteral("The document moved or closed before it could be saved.");
		return false;
	}
	if ((active_document.documentState & UIDocumentStateInConflict)
	    && !conflict_resolution_token)
	{
		if (error_message)
			*error_message = QStringLiteral("Resolve or reload the provider's conflicting version before saving.");
		return false;
	}
	if (![active_document loadPendingContentsFromPath:local_snapshot_path
		                                  errorMessage:error_message])
	{
		return false;
	}
	[active_document clearLastError];
	[active_document setExplicitSaveActive:true];
	const bool saved =
		[active_document replaceProviderContentsMatchingFingerprint:
			expected_fingerprint
		 errorMessage:error_message];
	if (saved)
		[active_document commitPendingContents];
	else
		[active_document discardPendingContents];
	[active_document finishExplicitSave:saved];
	if (!saved)
	{
		[active_document discardConflictVersions:conflict_resolution_token];
		return false;
	}
	if (conflict_resolution_token
	    && ![active_document resolveConflictVersions:conflict_resolution_token
		                                errorMessage:error_message])
	{
		return false;
	}
	if (coordinated_path)
		*coordinated_path = fromNSString(active_document.fileURL.path);
	return true;
}

bool exportDocument(const QString& local_snapshot_path,
	                QString* exported_path,
	                QByteArray* exported_fingerprint,
	                QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (exported_path)
		exported_path->clear();
	if (exported_fingerprint)
		exported_fingerprint->clear();
	clearPendingExport();
	if (local_snapshot_path.isEmpty()
	    || !QFileInfo::exists(local_snapshot_path))
		return false;
	QByteArray expected_fingerprint;
	if (exported_fingerprint)
	{
		QFile local_snapshot{local_snapshot_path};
		QCryptographicHash local_hash{QCryptographicHash::Sha256};
		if (!local_snapshot.open(QIODevice::ReadOnly)
		    || !local_hash.addData(&local_snapshot))
		{
			if (error_message)
			{
				*error_message = QStringLiteral(
					"Mapper could not fingerprint the immutable Save As snapshot.");
			}
			return false;
		}
		expected_fingerprint = local_hash.result();
		*exported_fingerprint = expected_fingerprint;
	}
	if (![NSThread isMainThread])
	{
		if (error_message)
			*error_message = QStringLiteral("The native document exporter must run on the UI thread.");
		return false;
	}
	auto* presenter = activeViewController();
	if (!presenter)
	{
		if (error_message)
			*error_message = QStringLiteral("Mapper could not find an active window for the Files exporter.");
		return false;
	}

	QEventLoop event_loop;
	auto* delegate = [[MapperExportPickerDelegate alloc] initWithEventLoop:&event_loop];
	auto* picker = [[UIDocumentPickerViewController alloc]
		initForExportingURLs:@[fileURL(local_snapshot_path)]
		asCopy:YES];
	picker.delegate = delegate;
	picker.modalPresentationStyle = UIModalPresentationFormSheet;
	[presenter presentViewController:picker animated:YES completion:nil];
	if (![delegate isFinished])
		event_loop.exec(QEventLoop::ExcludeUserInputEvents);
	NSURL* selected_url = [delegate selectedURL];
	if (!selected_url)
		return false;

	pending_export_url = selected_url;
	pending_export_scope_active =
		[pending_export_url startAccessingSecurityScopedResource];
	const auto selected_path = fromNSString(pending_export_url.path);
	if (selected_path.isEmpty())
	{
		if (error_message)
			*error_message = QStringLiteral("The Files provider returned an invalid document URL.");
		clearPendingExport();
		return false;
	}
	if (exported_path)
		*exported_path = selected_path;

	QString bookmark_error;
	if (!persistSecurityScopedBookmark(selected_url, &bookmark_error))
	{
		if (error_message)
		{
			*error_message = exported_fingerprint
				? QStringLiteral(
					"Files created the provisional copy, but Mapper could not preserve "
					"access to it. Mapper will remove it only if it is still unchanged. ")
					+ bookmark_error
				: QStringLiteral(
					"Files created the copy, but Mapper could not preserve access to it. "
					"The copy was left untouched. ")
					+ bookmark_error;
		}
		return false;
	}
	if (exported_fingerprint)
	{
		QByteArray provider_fingerprint;
		QString fingerprint_error;
		if (!fingerprintAuxiliaryDocument(
			    selected_path, &provider_fingerprint, &fingerprint_error)
		    || provider_fingerprint != expected_fingerprint)
		{
			if (error_message)
			{
				*error_message = fingerprint_error.isEmpty()
					? QStringLiteral(
						"The exported document changed before Mapper could verify it. "
						"Mapper did not overwrite that generation.")
					: fingerprint_error;
			}
			return false;
		}
	}
	return true;
}

bool adoptExportedDocument(const QString& path,
	                       const QString& local_snapshot_path,
	                       const QByteArray& expected_fingerprint,
	                       ChangeHandler change_handler,
	                       quint64* presentation_token,
	                       QString* coordinated_path,
	                       QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (presentation_token)
		*presentation_token = 0;
	if (path.isEmpty() || local_snapshot_path.isEmpty()
	    || expected_fingerprint.isEmpty()
	    || !pending_export_url)
	{
		clearPendingExport();
		return false;
	}
	const auto exported_path = QFileInfo{fromNSString(pending_export_url.path)}.absoluteFilePath();
	if (exported_path != QFileInfo{path}.absoluteFilePath())
	{
		if (error_message)
			*error_message = QStringLiteral("The exported document URL changed before Mapper could adopt it.");
		clearPendingExport();
		return false;
	}

	auto* candidate = makeDocument(
		pending_export_url,
		std::move(change_handler),
		presentation_token,
		error_message);
	if (!candidate)
	{
		clearPendingExport();
		return false;
	}
	[candidate clearLastError];
	const bool opened = awaitDocumentOperation([candidate](void (^completion)(BOOL)) {
		[candidate openWithCompletionHandler:completion];
	});
	if (!opened)
	{
		if (error_message)
		{
			*error_message = [candidate lastErrorMessage:
				QStringLiteral("The file was exported, but its provider would not reopen it for editing.")];
		}
		closeDocument(candidate);
		clearPendingExport();
		if (presentation_token)
			*presentation_token = 0;
		return false;
	}
	if (![candidate loadPendingContentsFromPath:local_snapshot_path
		                              errorMessage:error_message])
	{
		closeDocument(candidate);
		clearPendingExport();
		if (presentation_token)
			*presentation_token = 0;
		return false;
	}
	[candidate setMapperModified:true];
	[candidate clearLastError];
	[candidate setExplicitSaveActive:true];
	const bool saved =
		[candidate replaceProviderContentsMatchingFingerprint:
			expected_fingerprint
		 errorMessage:error_message];
	if (saved)
		[candidate commitPendingContents];
	else
		[candidate discardPendingContents];
	[candidate finishExplicitSave:saved];
	if (!saved)
	{
		closeDocument(candidate);
		clearPendingExport();
		if (presentation_token)
			*presentation_token = 0;
		return false;
	}

	auto* previous = active_document;
	active_document = candidate;
	[candidate activateMapperPresentation];
	closeDocument(previous);
	clearPendingExport();
	if (coordinated_path)
		*coordinated_path = fromNSString(candidate.fileURL.path);
	return true;
}

void abandonExportedDocument()
{
	clearPendingExport();
}

bool removeExportedDocument(const QString& path,
	                        const QByteArray& expected_fingerprint,
	                        QString* error_message)
{
	if (error_message)
		error_message->clear();
	if (path.isEmpty() || expected_fingerprint.isEmpty())
	{
		clearPendingExport();
		return false;
	}
	if (matchesActiveDocument(path))
	{
		if (error_message)
		{
			*error_message = QStringLiteral(
				"Mapper refused to remove the incomplete exported copy because Files "
				"returned the currently open source document. Review it before retrying.");
		}
		clearPendingExport();
		return false;
	}
	auto* url = fileURL(path);
	const bool scope_active = [url startAccessingSecurityScopedResource];
	__block BOOL removed = NO;
	__block bool provider_changed = false;
	__block NSError* read_error = nil;
	__block NSError* removal_error = nil;
	NSError* coordination_error = nil;
	auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
	[coordinator coordinateWritingItemAtURL:url
		options:NSFileCoordinatorWritingForDeleting
		error:&coordination_error
		byAccessor:^(NSURL* coordinated_url) {
			if (![NSFileManager.defaultManager
			      fileExistsAtPath:coordinated_url.path])
			{
				removed = YES;
				return;
			}
			auto* current_contents = [NSData dataWithContentsOfURL:coordinated_url
				options:NSDataReadingMappedIfSafe
				error:&read_error];
			if (!current_contents)
				return;
			const auto current_fingerprint = QCryptographicHash::hash(
				QByteArrayView{
					reinterpret_cast<const char*>(current_contents.bytes),
					qsizetype(current_contents.length)},
				QCryptographicHash::Sha256);
			if (current_fingerprint != expected_fingerprint)
			{
				provider_changed = true;
				return;
			}
			removed = [NSFileManager.defaultManager
				removeItemAtURL:coordinated_url error:&removal_error];
		}];
	if (scope_active)
		[url stopAccessingSecurityScopedResource];
	clearPendingExport();
	if (removed)
	{
		removeSecurityScopedBookmark(normalizedPathKey(path));
		return true;
	}
	if (error_message)
	{
		*error_message = provider_changed
			? QStringLiteral(
				"The incomplete exported copy changed in Files, so Mapper left that "
				"newer generation untouched. Review or delete it manually.")
			: errorDescription(
				coordination_error ? coordination_error
				                   : (read_error ? read_error : removal_error),
				QStringLiteral("Files could not remove the incomplete exported copy."));
	}
	return false;
}

bool hasPresentedDocumentConflict()
{
	return active_document
	       && [active_document isMapperActive]
	       && (active_document.documentState & UIDocumentStateInConflict);
}

quint64 capturePresentedDocumentConflicts()
{
	if (!active_document || ![active_document isMapperActive])
		return 0;
	return [active_document captureConflictVersions];
}

void discardPresentedDocumentConflicts(quint64 token)
{
	if (active_document && [active_document isMapperActive])
		[active_document discardConflictVersions:token];
}

void setPresentedDocumentModified(bool modified)
{
	if (active_document && [active_document isMapperActive])
		[active_document setMapperModified:modified];
}

void stopPresenting()
{
	clearPendingExport();
	auto* document = active_document;
	active_document = nil;
	closeDocument(document);
}

}  // namespace OpenOrienteering::AppleDocumentAccess
