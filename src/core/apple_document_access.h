/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_APPLE_DOCUMENT_ACCESS_H
#define OPENORIENTEERING_APPLE_DOCUMENT_ACCESS_H

#include <functional>

#include <QtGlobal>
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace OpenOrienteering::AppleDocumentAccess {

enum class PresentedDocumentChange
{
	Changed,
	Moved,
	Deleted,
};

/**
 * A provider callback is tied to one presenter generation. The previous path
 * lets the UI reject a late move/deletion callback from a replaced presenter.
 */
struct PresentedDocumentEvent
{
	PresentedDocumentChange change;
	quint64 presentation_token = 0;
	QString previous_path;
	QString path;
	QString error;
};

using ChangeHandler = std::function<void(const PresentedDocumentEvent& event)>;
using AuxiliaryReadHandler = std::function<bool(const QString& coordinated_path)>;

/** Presents the native Files open-in-place picker and returns its selected path. */
bool chooseDocumentToOpen(const QString& title,
                          QString* selected_path,
                          QString* error_message);

/** Keeps a persisted security-scoped file available for ordinary Qt readers. */
bool beginAuxiliaryDocumentAccess(const QString& path);

/** Balances a successful beginAuxiliaryDocumentAccess() call. */
void endAuxiliaryDocumentAccess(const QString& path);

/** Resolves a persisted bookmark to its current provider path. */
QString resolvedAuxiliaryDocumentPath(const QString& path);

/** Captures the exact coordinated bytes used for stale-write detection. */
bool fingerprintAuxiliaryDocument(const QString& path,
                                  QByteArray* fingerprint,
                                  QString* error_message);

/**
 * Runs @p reader and hashes the same provider bytes under one coordinated read.
 * The returned fingerprint therefore proves the exact baseline the reader saw.
 */
bool readAuxiliaryDocument(const QString& path,
                           AuxiliaryReadHandler reader,
                           QByteArray* fingerprint,
                           QString* error_message);

/**
 * Copies one coordinated provider generation into app-private storage.
 * The local snapshot remains stable for lazy readers after coordination ends.
 */
bool snapshotAuxiliaryDocument(const QString& path,
                               const QString& local_snapshot_path,
                               QByteArray* fingerprint,
                               QString* resolved_path,
                               QString* error_message);

/**
 * Coordinately replaces a security-scoped auxiliary document only when its
 * bytes still match @p expected_fingerprint and it has no unresolved versions.
 */
bool writeAuxiliaryDocument(const QString& path,
                            const QString& local_snapshot_path,
                            const QByteArray& expected_fingerprint,
                            QByteArray* committed_fingerprint,
                            QString* error_message);

/** Returns whether Mapper has a durable bookmark for this exact file path. */
bool hasPersistedDocumentAccess(const QString& path);

/** Exports an auxiliary snapshot as a copy and releases the temporary lease. */
bool exportAuxiliaryDocument(const QString& local_snapshot_path,
                             QString* exported_path,
                             QString* error_message);

/**
 * Returns a stable app-private directory for recoverable external-resource
 * drafts belonging to @p document_path, creating it when necessary.
 */
QString privateAuxiliaryDraftDirectory(const QString& document_path);

/** Returns whether @p path belongs to Mapper's private draft hierarchy. */
bool isPrivateAuxiliaryDraft(const QString& path);

/** Returns the private recovery snapshot path for one external template. */
QString privateAuxiliaryRecoveryPath(const QString& document_path,
                                     const QString& resource_identity,
                                     const QString& template_path);

/** Removes a template recovery snapshot after save or explicit discard. */
void discardPrivateAuxiliaryRecovery(const QString& document_path,
                                     const QString& resource_identity,
                                     const QString& template_path);

/** Moves every external-template recovery receipt to a document's new identity. */
bool migratePrivateAuxiliaryRecovery(const QString& old_document_path,
                                     const QString& new_document_path,
                                     QString* error_message);

/** Presents a native action sheet and returns the selected index, or -1. */
int chooseDocumentFormat(const QString& title,
                         const QStringList& options,
                         int preferred_index,
                         const QString& cancel_title);

/**
 * Opens and presents a provider document with UIDocument, copying its immutable
 * contents to a caller-owned local file for parsing by the Qt model.
 */
bool openDocument(const QString& path,
                  const QString& local_snapshot_path,
                  ChangeHandler change_handler,
                  quint64* presentation_token,
                  QString* coordinated_path,
                  QString* error_message);

/** Copies a fresh coordinated snapshot of the active presented document. */
bool readPresentedDocument(const QString& path,
                           const QString& local_snapshot_path,
                           QString* coordinated_path,
                           QString* error_message);

/** Captures the exact provider generation the user has approved overwriting. */
bool capturePresentedDocumentWriteReceipt(const QString& path,
                                          bool accept_current_provider,
                                          QByteArray* fingerprint,
                                          QString* error_message);

/** Commits a local, fully serialized snapshot to the active document. */
bool writePresentedDocument(const QString& path,
                            const QString& local_snapshot_path,
                            const QByteArray& expected_fingerprint,
                            quint64 conflict_resolution_token,
                            QString* coordinated_path,
                            QString* error_message);

/**
 * Presents the native Files export UI for a complete, immutable snapshot.
 * The provider never sees an empty placeholder. A temporary security-scope
 * lease is retained until adoptExportedDocument() or abandonExportedDocument().
 */
bool exportDocument(const QString& local_snapshot_path,
                    QString* exported_path,
                    QByteArray* exported_fingerprint,
                    QString* error_message);

/**
 * Opens the just-exported document, safely commits a destination-relative
 * snapshot, and only then replaces the active UIDocument.
 */
bool adoptExportedDocument(const QString& path,
                           const QString& local_snapshot_path,
                           const QByteArray& expected_fingerprint,
                           ChangeHandler change_handler,
                           quint64* presentation_token,
                           QString* coordinated_path,
                           QString* error_message);

/** Releases a pending export lease after destination-relative staging fails. */
void abandonExportedDocument();

/** Removes an unsafe copy only if it is still the generation Mapper exported. */
bool removeExportedDocument(const QString& path,
                            const QByteArray& expected_fingerprint,
                            QString* error_message);

/** Returns whether the active provider document has unresolved versions. */
bool hasPresentedDocumentConflict();

/** Captures exactly the unresolved versions shown to the user for consent. */
quint64 capturePresentedDocumentConflicts();

/** Drops an unused conflict-consent snapshot. */
void discardPresentedDocumentConflicts(quint64 token);

/** Updates the active presenter's cheap, thread-safe dirty-state contract. */
void setPresentedDocumentModified(bool modified);

/** Stops presenting the active document and releases its security scope. */
void stopPresenting();

}  // namespace OpenOrienteering::AppleDocumentAccess

#endif
