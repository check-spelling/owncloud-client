/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */


#ifndef FOLDERMAN_H
#define FOLDERMAN_H

#include <QObject>
#include <QQueue>
#include <QList>

#include "folder.h"
#include "folderwatcher.h"
#include "navigationpanehelper.h"
#include "syncfileitem.h"

class TestFolderMigration;

namespace OCC {
namespace TestUtils {
    // prototype for test friend
    FolderMan *folderMan();
}

class Application;
class SyncResult;
class SocketApi;
class LockWatcher;

/**
 * @brief Return object for Folder::trayOverallStatus.
 * @ingroup gui
 */
class TrayOverallStatusResult
{
public:
    QDateTime lastSyncDone;

    void addResult(Folder *f);
    const SyncResult &overallStatus() const;

private:
    SyncResult _overallStatus;
};

/**
 * @brief The FolderMan class
 * @ingroup gui
 *
 * The FolderMan knows about all loaded folders and is responsible for
 * scheduling them when necessary.
 *
 * A folder is scheduled if:
 * - The configured force-sync-interval has expired
 *   (_timeScheduler and slotScheduleFolderByTime())
 *
 * - A folder watcher receives a notification about a file change
 *   (_folderWatchers and Folder::slotWatchedPathChanged())
 *
 * - The folder etag on the server has changed
 *   (_etagPollTimer)
 *
 * - The locks of a monitored file are released
 *   (_lockWatcher and slotWatchedFileUnlocked())
 *
 * - There was a sync error or a follow-up sync is requested
 *   (_timeScheduler and slotScheduleFolderByTime()
 *    and Folder::slotSyncFinished())
 */
class FolderMan : public QObject
{
    Q_OBJECT
public:
    ~FolderMan() override;
    static FolderMan *instance();

    int setupFolders();
    int setupFoldersMigration();

    /** Find folder setting keys that need to be ignored or deleted for being too new.
     *
     * The client has a maximum supported version for the folders lists (maxFoldersVersion
     * in folderman.cpp) and a second maximum version for the contained folder configuration
     * (FolderDefinition::maxSettingsVersion()). If a future client creates configurations
     * with higher versions the older client will not be able to process them.
     *
     * Skipping or deleting these keys prevents accidents when switching from a newer
     * client to an older one.
     *
     * This function scans through the settings and finds too-new entries that can be
     * ignored (ignoreKeys) and entries that have to be deleted to keep going (deleteKeys).
     *
     * This data is used in Application::configVersionMigration() to backward-migrate
     * future configurations (possibly with user confirmation for deletions) and in
     * FolderMan::setupFolders() to know which too-new folder configurations to skip.
     */
    static void backwardMigrationSettingsKeys(QStringList *deleteKeys, QStringList *ignoreKeys);

    const QMap<QString, Folder *> &map() const;

    /** Adds a folder for an account, ensures the journal is gone and saves it in the settings.
      */
    Folder *addFolder(AccountStatePtr accountState, const FolderDefinition &folderDefinition);

    /**
     * Adds a folder for an account. Used to be part of the wizard code base. Constructs the folder definition from the parameters.
     */
    Folder *addFolder(AccountStatePtr accountStatePtr, const QString &localFolder, const QString &remotePath, const QUrl &webDavUrl);

    /** Removes a folder */
    void removeFolder(Folder *);

    /**
     * Returns the folder which the file or directory stored in path is in
     *
     * Optionally, the path relative to the found folder is returned in
     * relativePath.
     */
    Folder *folderForPath(const QString &path, QString *relativePath = nullptr);

    /**
      * returns a list of local files that exist on the local harddisk for an
      * incoming relative server path. The method checks with all existing sync
      * folders.
      */
    QStringList findFileInLocalFolders(const QString &relPath, const AccountPtr acc);

    /** Returns the folder by alias or NULL if no folder with the alias exists. */
    Folder *folder(const QString &);

    /**
     * Migrate accounts from owncloud < 2.0
     * Creates a folder for a specific configuration, identified by alias.
     */
    Folder *setupFolderFromOldConfigFile(const QString &, AccountStatePtr account);

    /**
     * Ensures that a given directory does not contain a sync journal file.
     *
     * @returns false if the journal could not be removed, true otherwise.
     */
    static bool ensureJournalGone(const QString &journalDbFile);

    /** Creates a new and empty local directory. */
    bool startFromScratch(const QString &);

    /// Produce text for use in the tray tooltip
    static QString trayTooltipStatusString(const SyncResult &result, bool paused);

    /**
     * Compute status summarizing multiple folders
     * @return tuple containing folders, status, unresolvedConflicts and lastSyncDone
     */
    static TrayOverallStatusResult trayOverallStatus(const QList<Folder *> &folders);

    // Escaping of the alias which is used in QSettings AND the file
    // system, thus need to be escaped.
    static QString escapeAlias(const QString &);
    static QString unescapeAlias(const QString &);

    SocketApi *socketApi();
#ifdef Q_OS_WIN
    NavigationPaneHelper &navigationPaneHelper() { return _navigationPaneHelper; }
#endif

    /**
     * Check if @a path is a valid path for a new folder considering the already sync'ed items.
     * Make sure that this folder, or any subfolder is not sync'ed already.
     *
     * @returns an empty string if it is allowed, or an error if it is not allowed
     */
    QString checkPathValidityForNewFolder(const QString &path) const;

    /**
     * Attempts to find a non-existing, acceptable path for creating a new sync folder.
     *
     * Uses \a basePath as the baseline. It'll return this path if it's acceptable.
     *
     * Note that this can fail. If someone syncs ~ and \a basePath is ~/ownCloud, no
     * subfolder of ~ would be a good candidate. When that happens \a basePath
     * is returned.
     */
    QString findGoodPathForNewSyncFolder(const QString &basePath) const;

    /**
     * While ignoring hidden files can theoretically be switched per folder,
     * it's currently a global setting that users can only change for all folders
     * at once.
     * These helper functions can be removed once it's properly per-folder.
     */
    bool ignoreHiddenFiles() const;
    void setIgnoreHiddenFiles(bool ignore);

    /**
     * Access to the current queue of scheduled folders.
     */
    QQueue<Folder *> scheduleQueue() const;

    /**
     * Access to the currently syncing folder.
     *
     * Note: This is only the folder that's currently syncing *as-scheduled*. There
     * may be externally-managed syncs such as from placeholder hydrations.
     *
     * See also isAnySyncRunning()
     */
    Folder *currentSyncFolder() const;

    /**
     * Returns true if any folder is currently syncing.
     *
     * This might be a FolderMan-scheduled sync, or a externally
     * managed sync like a placeholder hydration.
     */
    bool isAnySyncRunning() const;

    /** Removes all folders */
    void unloadAndDeleteAllFolders();

    /**
     * If enabled is set to false, no new folders will start to sync.
     * The current one will finish.
     */
    void setSyncEnabled(bool);

    /** Queues a folder for syncing. */
    void scheduleFolder(Folder *);

    /** Puts a folder in the very front of the queue. */
    void scheduleFolderNext(Folder *);

    /** Queues all folders for syncing. */
    void scheduleAllFolders();

    void setDirtyProxy();
    void setDirtyNetworkLimits();

    /** Whether or not vfs is supported in the location. */
    bool checkVfsAvailability(const QString &path, Vfs::Mode mode = bestAvailableVfsMode()) const;

    /** If the folder configuration is no longer supported this will return an error string */
    Result<void, QString> unsupportedConfiguration(const QString &path) const;
signals:
    /**
      * signal to indicate a folder has changed its sync state.
      *
      * Attention: The folder may be zero. Do a general update of the state then.
      */
    void folderSyncStateChange(Folder *);

    /**
     * Indicates when the schedule queue changes.
     */
    void scheduleQueueChanged();

    /**
     * Emitted whenever the list of configured folders changes.
     */
    void folderListChanged(const QMap<QString, Folder *> &);
    void folderRemoved(Folder *folder);

public slots:

    /**
     * Schedules folders of newly connected accounts, terminates and
     * de-schedules folders of disconnected accounts.
     */
    void slotAccountStateChanged();

    /**
     * restart the client as soon as it is possible, ie. no folders syncing.
     */
    void slotScheduleAppRestart();

    /**
     * Triggers a sync run once the lock on the given file is removed.
     *
     * Automatically detemines the folder that's responsible for the file.
     * See slotWatchedFileUnlocked().
     */
    void slotSyncOnceFileUnlocks(const QString &path, FileSystem::LockMode mode);

    // slot to schedule an ETag job (from Folder only)
    void slotScheduleETagJob(const QString &alias, RequestEtagJob *job);

private slots:
    void slotFolderSyncPaused(Folder *, bool paused);
    void slotFolderCanSyncChanged();
    void slotFolderSyncStarted();
    void slotFolderSyncFinished(const SyncResult &);

    void slotRunOneEtagJob();
    void slotEtagJobDestroyed(QObject *);

    // slot to take the next folder from queue and start syncing.
    void slotStartScheduledFolderSync();
    void slotEtagPollTimerTimeout();

    void slotRemoveFoldersForAccount(AccountStatePtr accountState);

    // Wraps the Folder::syncStateChange() signal into the
    // FolderMan::folderSyncStateChange(Folder*) signal.
    void slotForwardFolderSyncStateChange();

    void slotServerVersionChanged(Account *account);

    /**
     * A file whose locks were being monitored has become unlocked.
     *
     * This schedules the folder for synchronization that contains
     * the file with the given path.
     */
    void slotWatchedFileUnlocked(const QString &path);

    /**
     * Schedules folders whose time to sync has come.
     *
     * Either because a long time has passed since the last sync or
     * because of previous failures.
     */
    void slotScheduleFolderByTime();

private:
    /** Adds a new folder, does not add it to the account settings and
     *  does not set an account on the new folder.
      */
    Folder *addFolderInternal(FolderDefinition folderDefinition,
        AccountStatePtr accountState, std::unique_ptr<Vfs> vfs);

    /* unloads a folder object, does not delete it */
    void unloadFolder(Folder *);

    /** Will start a sync after a bit of delay. */
    void startScheduledSyncSoon();

    // finds all folder configuration files
    // and create the folders
    QString getBackupName(QString fullPathName) const;

    // makes the folder known to the socket api
    void registerFolderWithSocketApi(Folder *folder);

    // restarts the application (Linux only)
    void restartApplication();

    void setupFoldersHelper(QSettings &settings, AccountStatePtr account, const QStringList &ignoreKeys, bool backwardsCompatible, bool foldersWithPlaceholders);

    QSet<Folder *> _disabledFolders;
    QMap<QString, Folder *> _folderMap;
    QString _folderConfigPath;
    Folder *_currentSyncFolder;
    QPointer<Folder> _lastSyncFolder;
    bool _syncEnabled;

    /// Folder aliases from the settings that weren't read
    QSet<QString> _additionalBlockedFolderAliases;

    /// Starts regular etag query jobs
    QTimer _etagPollTimer;
    /// The currently running etag query
    QPointer<RequestEtagJob> _currentEtagJob;

    /// Watches files that couldn't be synced due to locks
    QScopedPointer<LockWatcher> _lockWatcher;

    /// Occasionally schedules folders
    QTimer _timeScheduler;

    /// Scheduled folders that should be synced as soon as possible
    QQueue<Folder *> _scheduledFolders;

    /// Picks the next scheduled folder and starts the sync
    QTimer _startScheduledSyncTimer;

    QScopedPointer<SocketApi> _socketApi;
#ifdef Q_OS_WIN
    NavigationPaneHelper _navigationPaneHelper;
#endif

    bool _appRestartRequired;

    mutable QMap<QString, Result<void, QString>> _unsupportedConfigurationError;

    static FolderMan *_instance;
    explicit FolderMan(QObject *parent = nullptr);
    friend class OCC::Application;
    friend OCC::FolderMan *OCC::TestUtils::folderMan();
    friend class ::TestFolderMigration;
};

} // namespace OCC
#endif // FOLDERMAN_H
