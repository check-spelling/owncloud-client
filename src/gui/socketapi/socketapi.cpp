/*
 * Copyright (C) by Dominik Schmidt <dev@dominik-schmidt.de>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Roeland Jago Douma <roeland@famdouma.nl>
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

#include "socketapi.h"
#include "socketapi_p.h"

#include "socketapi/socketuploadjob.h"

#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "capabilities.h"
#include "common/asserts.h"
#include "common/syncjournalfilerecord.h"
#include "common/version.h"
#include "config.h"
#include "configfile.h"
#include "filesystem.h"
#include "folder.h"
#include "folderman.h"
#include "guiutility.h"
#include "sharemanager.h"
#include "syncengine.h"
#include "syncfileitem.h"
#include "theme.h"

#include <array>
#include <QBitArray>
#include <QUrl>
#include <QMetaMethod>
#include <QMetaObject>
#include <QStringList>
#include <QScopedPointer>
#include <QFile>
#include <QDir>
#include <QApplication>
#include <QLocalSocket>
#include <QStringBuilder>
#include <QMessageBox>
#include <QFileDialog>


#include <QAction>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWidget>
#include <QBuffer>

#include <QClipboard>

#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif


// This is the version that is returned when the client asks for the VERSION.
// The first number should be changed if there is an incompatible change that breaks old clients.
// The second number should be changed when there are new features.
#define MIRALL_SOCKET_API_VERSION "1.1"

namespace {

const QLatin1Char RecordSeparator()
{
    return QLatin1Char('\x1e');
}

QStringList split(const QString &data)
{
    // TODO: string ref?
    return data.split(RecordSeparator());
}

#if GUI_TESTING

using namespace OCC;

QList<QObject *> allObjects(const QList<QWidget *> &widgets)
{
    QList<QObject *> objects;
    std::copy(widgets.constBegin(), widgets.constEnd(), std::back_inserter(objects));

    objects << qApp;

    return objects;
}

QObject *findWidget(const QString &queryString, const QList<QWidget *> &widgets = QApplication::allWidgets())
{
    auto objects = allObjects(widgets);

    QList<QObject *>::const_iterator foundWidget;

    if (queryString.contains('>')) {
        qCDebug(lcSocketApi) << "queryString contains >";

        auto subQueries = queryString.split('>', QString::SkipEmptyParts);
        Q_ASSERT(subQueries.count() == 2);

        auto parentQueryString = subQueries[0].trimmed();
        qCDebug(lcSocketApi) << "Find parent: " << parentQueryString;
        auto parent = findWidget(parentQueryString);

        if (!parent) {
            return nullptr;
        }

        auto childQueryString = subQueries[1].trimmed();
        auto child = findWidget(childQueryString, parent->findChildren<QWidget *>());
        qCDebug(lcSocketApi) << "found child: " << !!child;
        return child;

    } else if (queryString.startsWith('#')) {
        auto objectName = queryString.mid(1);
        qCDebug(lcSocketApi) << "find objectName: " << objectName;
        foundWidget = std::find_if(objects.constBegin(), objects.constEnd(), [&](QObject *widget) {
            return widget->objectName() == objectName;
        });
    } else {
        QList<QObject *> matches;
        std::copy_if(objects.constBegin(), objects.constEnd(), std::back_inserter(matches), [&](QObject *widget) {
            return widget->inherits(queryString.toLatin1());
        });

        std::for_each(matches.constBegin(), matches.constEnd(), [](QObject *w) {
            if (!w)
                return;
            qCDebug(lcSocketApi) << "WIDGET: " << w->objectName() << w->metaObject()->className();
        });

        if (matches.empty()) {
            return nullptr;
        }
        return matches[0];
    }

    if (foundWidget == objects.constEnd()) {
        return nullptr;
    }

    return *foundWidget;
}
#endif

static inline QString removeTrailingSlash(QString path)
{
    Q_ASSERT(path.endsWith(QLatin1Char('/')));
    path.truncate(path.length() - 1);
    return path;
}

static QString buildMessage(const QString &verb, const QString &path, const QString &status = QString())
{
    QString msg(verb);

    if (!status.isEmpty()) {
        msg.append(QLatin1Char(':'));
        msg.append(status);
    }
    if (!path.isEmpty()) {
        msg.append(QLatin1Char(':'));
        QFileInfo fi(path);
        msg.append(QDir::toNativeSeparators(fi.absoluteFilePath()));
    }
    return msg;
}
}

namespace OCC {

Q_LOGGING_CATEGORY(lcSocketApi, "gui.socketapi", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPublicLink, "gui.socketapi.publiclink", QtInfoMsg)

void SocketListener::sendMessage(const QString &message, bool doWait) const
{
    if (!socket) {
        qCInfo(lcSocketApi) << "Not sending message to dead socket:" << message;
        return;
    }

    qCInfo(lcSocketApi) << "Sending SocketAPI message -->" << message << "to" << socket;
    QString localMessage = message;
    if (!localMessage.endsWith(QLatin1Char('\n'))) {
        localMessage.append(QLatin1Char('\n'));
    }

    QByteArray bytesToSend = localMessage.toUtf8();
    qint64 sent = socket->write(bytesToSend);
    if (doWait) {
        socket->waitForBytesWritten(1000);
    }
    if (sent != bytesToSend.length()) {
        qCWarning(lcSocketApi) << "Could not send all data on socket for " << localMessage;
    }
}

SocketApi::SocketApi(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<SocketListener *>("SocketListener*");
    qRegisterMetaType<QSharedPointer<SocketApiJob>>("QSharedPointer<SocketApiJob>");
    qRegisterMetaType<QSharedPointer<SocketApiJobV2>>("QSharedPointer<SocketApiJobV2>");

    const QString socketPath = Utility::socketApiSocketPath();

    // Remove any old socket that might be lying around:
    SocketApiServer::removeServer(socketPath);

    // Create the socket path:
    if (!Utility::isMac()) {
        // Not on macOS: there the directory is there, and created for us by the sandboxing
        // environment, because we belong to an App Group.
        QFileInfo info(socketPath);
        if (!info.dir().exists()) {
            bool result = info.dir().mkpath(".");
            qCDebug(lcSocketApi) << "creating" << info.dir().path() << result;
            if (result) {
                QFile::setPermissions(socketPath,
                    QFile::Permissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            }
        }
    }

    // Wire up the server instance to us, so we can accept new connections:
    connect(&_localServer, &SocketApiServer::newConnection, this, &SocketApi::slotNewConnection);

    // Start listeneing:
    if (_localServer.listen(socketPath)) {
        qCInfo(lcSocketApi) << "server started, listening at " << socketPath;
    } else {
        qCWarning(lcSocketApi) << "can't start server" << socketPath;
    }

    // folder watcher
    connect(FolderMan::instance(), &FolderMan::folderSyncStateChange, this, &SocketApi::slotUpdateFolderView);

    // Now we're ready to start the native shell integration:
    Utility::startShellIntegration();
}

SocketApi::~SocketApi()
{
    qCDebug(lcSocketApi) << "dtor";
    _localServer.close();
    // All remaining sockets will be destroyed with _localServer, their parent
    OC_ASSERT(_listeners.isEmpty() || _listeners.first()->socket->parent() == &_localServer);
    _listeners.clear();
}

void SocketApi::slotNewConnection()
{
    // Note that on macOS this is not actually a line-based QIODevice, it's a SocketApiSocket which is our
    // custom message based macOS IPC.
    SocketApiSocket *socket = _localServer.nextPendingConnection();

    if (!socket) {
        return;
    }
    qCInfo(lcSocketApi) << "New connection" << socket;
    connect(socket, &SocketApiSocket::readyRead, this, &SocketApi::slotReadSocket);
    connect(socket, &SocketApiSocket::disconnected, this, [socket] {
        qCInfo(lcSocketApi) << "Lost connection " << socket;
        // will trigger destroyed in the next execution of the main loop
        // a direct removal can cause issues when iterating on _listeners
        socket->deleteLater();
    });
    connect(socket, &SocketApiSocket::destroyed, this, [socket, this] {
        _listeners.remove(socket);
    });
    OC_ASSERT(socket->readAll().isEmpty());

    auto listener = QSharedPointer<SocketListener>::create(socket);
    _listeners.insert(socket, listener);
    for (Folder *f : FolderMan::instance()->map()) {
        if (f->canSync()) {
            QString message = buildRegisterPathMessage(removeTrailingSlash(f->path()));
            listener->sendMessage(message);
        }
    }
}

void SocketApi::slotReadSocket()
{
    SocketApiSocket *socket = qobject_cast<SocketApiSocket *>(sender());
    OC_ENFORCE(socket);

    // Find the SocketListener
    //
    // It's possible for the disconnected() signal to be triggered before
    // the readyRead() signals are received - in that case there won't be a
    // valid listener. We execute the handler anyway, but it will work with
    // a SocketListener that doesn't send any messages.
    static auto invalidListener = QSharedPointer<SocketListener>::create(nullptr);
    const auto listener = _listeners.value(socket, invalidListener);
    while (socket->canReadLine()) {
        // Make sure to normalize the input from the socket to
        // make sure that the path will match, especially on OS X.
        QString line = QString::fromUtf8(socket->readLine()).normalized(QString::NormalizationForm_C);
        // Note: do NOT use QString::trimmed() here! That will also remove any trailing spaces (which _are_ part of the filename)!
        line.chop(1); // remove the '\n'

        qCInfo(lcSocketApi) << "Received SocketAPI message <--" << line << "from" << socket;
        const int argPos = line.indexOf(QLatin1Char(':'));
        const QByteArray command = line.midRef(0, argPos).toUtf8().toUpper();
        const int indexOfMethod = [&] {
            QByteArray functionWithArguments = QByteArrayLiteral("command_");
            if (command.startsWith("ASYNC_")) {
                functionWithArguments += command + QByteArrayLiteral("(QSharedPointer<SocketApiJob>)");
            } else if (command.startsWith("V2/")) {
                functionWithArguments += QByteArrayLiteral("V2_") + command.mid(3) + QByteArrayLiteral("(QSharedPointer<SocketApiJobV2>)");
            } else {
                functionWithArguments += command + QByteArrayLiteral("(QString,SocketListener*)");
            }
            Q_ASSERT(staticQtMetaObject.normalizedSignature(functionWithArguments) == functionWithArguments);
            const auto out = staticMetaObject.indexOfMethod(functionWithArguments);
            if (out == -1) {
                listener->sendError(QStringLiteral("Function %1 not found").arg(QString::fromUtf8(functionWithArguments)));
            }
            OC_ASSERT(out != -1);
            return out;
        }();

        const auto argument = argPos != -1 ? line.midRef(argPos + 1) : QStringRef();
        if (command.startsWith("ASYNC_")) {
            auto arguments = argument.split('|');
            if (arguments.size() != 2) {
                listener->sendError(QStringLiteral("argument count is wrong"));
                return;
            }

            auto json = QJsonDocument::fromJson(arguments[1].toUtf8()).object();

            auto jobId = arguments[0];

            auto socketApiJob = QSharedPointer<SocketApiJob>(
                new SocketApiJob(jobId.toString(), listener, json), &QObject::deleteLater);
            if (indexOfMethod != -1) {
                staticMetaObject.method(indexOfMethod)
                    .invoke(this, Qt::QueuedConnection,
                        Q_ARG(QSharedPointer<SocketApiJob>, socketApiJob));
            } else {
                qCWarning(lcSocketApi) << "The command is not supported by this version of the client:" << command
                                       << "with argument:" << argument;
                socketApiJob->reject(QStringLiteral("command not found"));
            }
        } else if (command.startsWith("V2/")) {
            QJsonParseError error;
            const auto json = QJsonDocument::fromJson(argument.toUtf8(), &error).object();
            if (error.error != QJsonParseError::NoError) {
                qCWarning(lcSocketApi()) << "Invalid json" << argument.toString() << error.errorString();
                listener->sendError(error.errorString());
                return;
            }
            auto socketApiJob = QSharedPointer<SocketApiJobV2>::create(listener, command, json);
            if (indexOfMethod != -1) {
                staticMetaObject.method(indexOfMethod)
                    .invoke(this, Qt::QueuedConnection,
                        Q_ARG(QSharedPointer<SocketApiJobV2>, socketApiJob));
            } else {
                qCWarning(lcSocketApi) << "The command is not supported by this version of the client:" << command
                                       << "with argument:" << argument;
                socketApiJob->failure(QStringLiteral("command not found"));
            }
        } else {
            if (indexOfMethod != -1) {
                // to ensure that listener is still valid we need to call it with Qt::DirectConnection
                OC_ASSERT(thread() == QThread::currentThread())
                staticMetaObject.method(indexOfMethod)
                    .invoke(this, Qt::DirectConnection, Q_ARG(QString, argument.toString()),
                        Q_ARG(SocketListener*, listener.data()));
            }
        }
    }
}

void SocketApi::slotRegisterPath(const QString &alias)
{
    // Make sure not to register twice to each connected client
    if (_registeredAliases.contains(alias))
        return;

    Folder *f = FolderMan::instance()->folder(alias);
    if (f) {
        broadcastMessage(buildRegisterPathMessage(removeTrailingSlash(f->path())));
    }

    _registeredAliases.insert(alias);
}

void SocketApi::slotUnregisterPath(const QString &alias)
{
    if (!_registeredAliases.contains(alias))
        return;

    Folder *f = FolderMan::instance()->folder(alias);
    if (f)
        broadcastMessage(buildMessage(QLatin1String("UNREGISTER_PATH"), removeTrailingSlash(f->path()), QString()), true);

    _registeredAliases.remove(alias);
}

void SocketApi::slotUpdateFolderView(Folder *f)
{
    if (_listeners.isEmpty()) {
        return;
    }

    if (f) {
        // do only send UPDATE_VIEW for a couple of status
        switch (f->syncResult().status()) {
        case SyncResult::SyncPrepare:
            Q_FALLTHROUGH();
        case SyncResult::Success:
            Q_FALLTHROUGH();
        case SyncResult::Paused:
            Q_FALLTHROUGH();
        case SyncResult::Problem:
            Q_FALLTHROUGH();
        case SyncResult::Error:
            Q_FALLTHROUGH();
        case SyncResult::SetupError: {
            const QString rootPath = removeTrailingSlash(f->path());
            broadcastStatusPushMessage(rootPath, f->syncEngine().syncFileStatusTracker().fileStatus(""));

            broadcastMessage(buildMessage(QStringLiteral("UPDATE_VIEW"), rootPath));
        } break;
        case OCC::SyncResult::Undefined:
            Q_FALLTHROUGH();
        case OCC::SyncResult::NotYetStarted:
            Q_FALLTHROUGH();
        case OCC::SyncResult::SyncRunning:
            Q_FALLTHROUGH();
        case OCC::SyncResult::SyncAbortRequested:
            qCDebug(lcSocketApi) << "Not sending UPDATE_VIEW for" << f->alias() << "because status() is" << f->syncResult().status();
        }
    }
}

void SocketApi::broadcastMessage(const QString &msg, bool doWait)
{
    for (const auto &listener : qAsConst(_listeners)) {
        listener->sendMessage(msg, doWait);
    }
}

void SocketApi::processShareRequest(const QString &localFile, SocketListener *listener, ShareDialogStartPage startPage)
{
    auto theme = Theme::instance();

    auto fileData = FileData::get(localFile);
    auto shareFolder = fileData.folder;
    if (!shareFolder) {
        const QString message = QLatin1String("SHARE:NOP:") + QDir::toNativeSeparators(localFile);
        // files that are not within a sync folder are not synced.
        listener->sendMessage(message);
    } else if (!shareFolder->accountState()->isConnected()) {
        const QString message = QLatin1String("SHARE:NOTCONNECTED:") + QDir::toNativeSeparators(localFile);
        // if the folder isn't connected, don't open the share dialog
        listener->sendMessage(message);
    } else if (!theme->linkSharing() && (!theme->userGroupSharing() || shareFolder->accountState()->account()->serverVersionInt() < Account::makeServerVersion(8, 2, 0))) {
        const QString message = QLatin1String("SHARE:NOP:") + QDir::toNativeSeparators(localFile);
        listener->sendMessage(message);
    } else {
        // If the file doesn't have a journal record, it might not be uploaded yet
        if (!fileData.journalRecord().isValid()) {
            const QString message = QLatin1String("SHARE:NOTSYNCED:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
            return;
        }

        auto &remotePath = fileData.serverRelativePath;

        // Can't share root folder
        if (remotePath == "/") {
            const QString message = QLatin1String("SHARE:CANNOTSHAREROOT:") + QDir::toNativeSeparators(localFile);
            listener->sendMessage(message);
            return;
        }

        const QString message = QLatin1String("SHARE:OK:") + QDir::toNativeSeparators(localFile);
        listener->sendMessage(message);

        emit shareCommandReceived(remotePath, fileData.localPath, startPage);
    }
}

void SocketApi::broadcastStatusPushMessage(const QString &systemPath, SyncFileStatus fileStatus)
{
    QString msg = buildMessage(QLatin1String("STATUS"), systemPath, fileStatus.toSocketAPIString());
    Q_ASSERT(!systemPath.endsWith('/'));
    uint directoryHash = qHash(systemPath.left(systemPath.lastIndexOf('/')));
    for (const auto &listener : qAsConst(_listeners)) {
        listener->sendMessageIfDirectoryMonitored(msg, directoryHash);
    }
}

void SocketApi::command_RETRIEVE_FOLDER_STATUS(const QString &argument, SocketListener *listener)
{
    // This command is the same as RETRIEVE_FILE_STATUS
    command_RETRIEVE_FILE_STATUS(argument, listener);
}

void SocketApi::command_RETRIEVE_FILE_STATUS(const QString &argument, SocketListener *listener)
{
    QString statusString;

    auto fileData = FileData::get(argument);
    if (!fileData.folder) {
        // this can happen in offline mode e.g.: nothing to worry about
        statusString = SyncFileStatus(SyncFileStatus::StatusNone).toSocketAPIString();
    } else {
        // The user probably visited this directory in the file shell.
        // Let the listener know that it should now send status pushes for sibblings of this file.
        QString directory = fileData.localPath.left(fileData.localPath.lastIndexOf('/'));
        listener->registerMonitoredDirectory(qHash(directory));

        statusString = fileData.syncFileStatus().toSocketAPIString();
    }

    const QString message = QStringLiteral("STATUS:") % statusString % QLatin1Char(':') % QDir::toNativeSeparators(argument);
    listener->sendMessage(message);
}

void SocketApi::command_SHARE(const QString &localFile, SocketListener *listener)
{
    processShareRequest(localFile, listener, ShareDialogStartPage::UsersAndGroups);
}

void SocketApi::command_MANAGE_PUBLIC_LINKS(const QString &localFile, SocketListener *listener)
{
    processShareRequest(localFile, listener, ShareDialogStartPage::PublicLinks);
}

void SocketApi::command_VERSION(const QString &, SocketListener *listener)
{
    listener->sendMessage(QStringLiteral("VERSION:%1:%2").arg(OCC::Version::versionWithBuildNumber().toString(), QStringLiteral(MIRALL_SOCKET_API_VERSION)));
}

void SocketApi::command_SHARE_MENU_TITLE(const QString &, SocketListener *listener)
{
    listener->sendMessage(QLatin1String("SHARE_MENU_TITLE:") + tr("Share with %1", "parameter is ownCloud").arg(Theme::instance()->appNameGUI()));
}

class GetOrCreatePublicLinkShare : public QObject
{
    Q_OBJECT
public:
    GetOrCreatePublicLinkShare(const AccountPtr &account,
        const QString &serverPath, QObject *parent)
        : QObject(parent)
        , _account(account)
        , _shareManager(account)
        , _serverPath(serverPath)
    {
        connect(&_shareManager, &ShareManager::sharesFetched,
            this, &GetOrCreatePublicLinkShare::sharesFetched);
        connect(&_shareManager, &ShareManager::linkShareCreated,
            this, &GetOrCreatePublicLinkShare::linkShareCreated);
        connect(&_shareManager, &ShareManager::linkShareCreationForbidden,
            this, &GetOrCreatePublicLinkShare::linkShareCreationForbidden);
        connect(&_shareManager, &ShareManager::serverError,
            this, &GetOrCreatePublicLinkShare::serverError);
    }

    void run()
    {
        qCDebug(lcPublicLink) << "Fetching shares";
        _shareManager.fetchShares(_serverPath);
    }

private slots:
    void sharesFetched(const QList<QSharedPointer<Share>> &shares)
    {
        auto shareName = SocketApi::tr("Context menu share");

        // If shares will expire, create a new one every day.
        QDate expireDate;
        if (_account->capabilities().sharePublicLinkDefaultExpire()) {
            shareName = SocketApi::tr("Context menu share %1").arg(QDate::currentDate().toString(Qt::ISODate));
            expireDate = QDate::currentDate().addDays(
                _account->capabilities().sharePublicLinkDefaultExpireDateDays());
        }

        // If there already is a context menu share, reuse it
        for (const auto &share : shares) {
            const auto linkShare = qSharedPointerDynamicCast<LinkShare>(share);
            if (!linkShare)
                continue;

            if (linkShare->getName() == shareName) {
                qCDebug(lcPublicLink) << "Found existing share, reusing";
                return success(linkShare->getLink().toString());
            }
        }

        // otherwise create a new one
        qCDebug(lcPublicLink) << "Creating new share";
        QString noPassword;
        _shareManager.createLinkShare(_serverPath, shareName, noPassword, expireDate);
    }

    void linkShareCreated(const QSharedPointer<LinkShare> &share)
    {
        qCDebug(lcPublicLink) << "New share created";
        success(share->getLink().toString());
    }

    void linkShareCreationForbidden(const QString &message)
    {
        qCInfo(lcPublicLink) << "Could not create link share:" << message;
        emit error(message);
        deleteLater();
    }

    void serverError(int code, const QString &message)
    {
        qCWarning(lcPublicLink) << "Share fetch/create error" << code << message;
        emit error(message);
        deleteLater();
    }

signals:
    void done(const QString &link);
    void error(const QString &message);

private:
    void success(const QString &link)
    {
        emit done(link);
        deleteLater();
    }

    AccountPtr _account;
    ShareManager _shareManager;
    QString _serverPath;
};

void SocketApi::command_COPY_PUBLIC_LINK(const QString &localFile, SocketListener *)
{
    auto fileData = FileData::get(localFile);
    if (!fileData.folder)
        return;

    AccountPtr account = fileData.folder->accountState()->account();
    auto job = new GetOrCreatePublicLinkShare(account, fileData.serverRelativePath, this);
    connect(job, &GetOrCreatePublicLinkShare::done, this,
        [](const QString &url) { copyUrlToClipboard(url); });
    connect(job, &GetOrCreatePublicLinkShare::error, this,
        [=]() { emit shareCommandReceived(fileData.serverRelativePath, fileData.localPath, ShareDialogStartPage::PublicLinks); });
    job->run();
}

// Fetches the private link url asynchronously and then calls the target slot
void SocketApi::fetchPrivateLinkUrlHelper(const QString &localFile, const std::function<void(const QString &url)> &targetFun)
{
    auto fileData = FileData::get(localFile);
    if (!fileData.folder) {
        qCWarning(lcSocketApi) << "Unknown path" << localFile;
        return;
    }

    auto record = fileData.journalRecord();
    if (!record.isValid())
        return;

    fetchPrivateLinkUrl(
        fileData.folder->accountState()->account(),
        fileData.folder->webDavUrl(),
        fileData.serverRelativePath,
        this,
        targetFun);
}

void SocketApi::command_COPY_PRIVATE_LINK(const QString &localFile, SocketListener *)
{
    fetchPrivateLinkUrlHelper(localFile, &SocketApi::copyUrlToClipboard);
}

void SocketApi::command_EMAIL_PRIVATE_LINK(const QString &localFile, SocketListener *)
{
    fetchPrivateLinkUrlHelper(localFile, &SocketApi::emailPrivateLink);
}

void SocketApi::command_OPEN_PRIVATE_LINK(const QString &localFile, SocketListener *)
{
    fetchPrivateLinkUrlHelper(localFile, &SocketApi::openPrivateLink);
}

void SocketApi::command_OPEN_PRIVATE_LINK_VERSIONS(const QString &localFile, SocketListener *)
{
    auto openVersionsLink = [](const QString &link) {
        QUrl url(link);
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("details"), QStringLiteral("versionsTabView"));
        url.setQuery(query);
        Utility::openBrowser(url, nullptr);
    };
    fetchPrivateLinkUrlHelper(localFile, openVersionsLink);
}

void SocketApi::copyUrlToClipboard(const QString &link)
{
    QApplication::clipboard()->setText(link);
}

void SocketApi::command_MAKE_AVAILABLE_LOCALLY(const QString &filesArg, SocketListener *)
{
    const QStringList files = split(filesArg);

    for (const auto &file : files) {
        auto data = FileData::get(file);
        if (!data.folder || !data.folder->isReady())
            continue;

        // Update the pin state on all items
        data.folder->vfs().setPinState(data.folderRelativePath, PinState::AlwaysLocal);

        // Trigger sync
        data.folder->schedulePathForLocalDiscovery(data.folderRelativePath);
        data.folder->scheduleThisFolderSoon();
    }
}

/* Go over all the files and replace them by a virtual file */
void SocketApi::command_MAKE_ONLINE_ONLY(const QString &filesArg, SocketListener *)
{
    const QStringList files = split(filesArg);

    for (const auto &file : files) {
        auto data = FileData::get(file);
        if (!data.folder || !data.folder->isReady())
            continue;

        // Update the pin state on all items
        data.folder->vfs().setPinState(data.folderRelativePath, PinState::OnlineOnly);

        // Trigger sync
        data.folder->schedulePathForLocalDiscovery(data.folderRelativePath);
        data.folder->scheduleThisFolderSoon();
    }
}

void SocketApi::command_DELETE_ITEM(const QString &localFile, SocketListener *)
{
    QFileInfo info(localFile);

    auto result = QMessageBox::question(
        nullptr, tr("Confirm deletion"),
        info.isDir()
            ? tr("Do you want to delete the directory <i>%1</i> and all its contents permanently?").arg(info.dir().dirName())
            : tr("Do you want to delete the file <i>%1</i> permanently?").arg(info.fileName()),
        QMessageBox::Yes, QMessageBox::No);
    if (result != QMessageBox::Yes)
        return;

    if (info.isDir()) {
        FileSystem::RemoveEntryList removed;
        FileSystem::RemoveEntryList locked;
        FileSystem::RemoveErrorList errors;
        FileSystem::removeRecursively(localFile, &removed, &locked, &errors);
    } else {
        QFile(localFile).remove();
    }
}

void SocketApi::command_MOVE_ITEM(const QString &localFile, SocketListener *)
{
    auto fileData = FileData::get(localFile);
    auto parentDir = fileData.parentFolder();
    if (!fileData.folder)
        return; // should not have shown menu item

    QString defaultDirAndName = fileData.folderRelativePath;

    // If it's a conflict, we want to save it under the base name by default
    if (Utility::isConflictFile(defaultDirAndName)) {
        defaultDirAndName = fileData.folder->journalDb()->conflictFileBaseName(fileData.folderRelativePath.toUtf8());
    }

    // If the parent doesn't accept new files, go to the root of the sync folder
    QFileInfo fileInfo(localFile);
    auto parentRecord = parentDir.journalRecord();
    if ((fileInfo.isFile() && !parentRecord._remotePerm.hasPermission(RemotePermissions::CanAddFile))
        || (fileInfo.isDir() && !parentRecord._remotePerm.hasPermission(RemotePermissions::CanAddSubDirectories))) {
        defaultDirAndName = QFileInfo(defaultDirAndName).fileName();
    }

    // Add back the folder path
    defaultDirAndName = QDir(fileData.folder->path()).filePath(defaultDirAndName);

    auto target = QFileDialog::getSaveFileName(
        nullptr,
        tr("Select new location..."),
        defaultDirAndName,
        QString(), nullptr, QFileDialog::HideNameFilterDetails);
    if (target.isEmpty())
        return;

    QString error;
    if (!FileSystem::uncheckedRenameReplace(localFile, target, &error)) {
        qCWarning(lcSocketApi) << "Rename error:" << error;
        QMessageBox::warning(
            nullptr, tr("Error"),
            tr("Moving file failed:\n\n%1").arg(error));
    }
}

void SocketApi::command_V2_LIST_ACCOUNTS(const QSharedPointer<SocketApiJobV2> &job) const
{
    QJsonArray out;
    for (auto acc : AccountManager::instance()->accounts()) {
        out << QJsonObject({ { QStringLiteral("name"), acc->account()->displayName() },
            { QStringLiteral("id"), acc->account()->id() },
            { QStringLiteral("uuid"), acc->account()->uuid().toString(QUuid::WithoutBraces) } });
    }
    job->success({ { QStringLiteral("accounts"), out } });
}

void SocketApi::command_V2_UPLOAD_FILES_FROM(const QSharedPointer<SocketApiJobV2> &job) const
{
    auto uploadJob = new SocketUploadJob(job);
    uploadJob->start();
}

void SocketApi::command_V2_GET_CLIENT_ICON(const QSharedPointer<SocketApiJobV2> &job) const
{
    OC_ASSERT(job);
    const auto &arguments = job->arguments();

    const auto size = arguments.value("size");
    if (size.isUndefined()) {
        qCWarning(lcSocketApi) << "Icon size not given in " << Q_FUNC_INFO;
        job->failure(QStringLiteral("cannot get client icon"));
        return;
    }

    QByteArray data;
    const Theme *theme = Theme::instance();
    // return an empty answer if the end point was disabled
    if (theme->enableSocketApiIconSupport()) {
        const QIcon appIcon = theme->applicationIcon();
        qCDebug(lcSocketApi) << Q_FUNC_INFO << " got icon from theme: " << appIcon;

        // convert to pixmap (might be smaller if size is not available)
        const QPixmap pixmap = appIcon.pixmap(QSize(size.toInt(), size.toInt()));

        // Convert pixmap to in-memory PNG
        QByteArray png;
        QBuffer pngBuffer(&png);
        auto success = pngBuffer.open(QIODevice::WriteOnly);
        if (!success) {
            qCWarning(lcSocketApi) << "Error opening buffer for png in " << Q_FUNC_INFO;
            job->failure(QStringLiteral("cannot get client icon"));
            return;
        }

        success = pixmap.save(&pngBuffer, "PNG");
        if (!success) {
            qCWarning(lcSocketApi) << "Error saving client icon as png in " << Q_FUNC_INFO;
            job->failure(QStringLiteral("cannot get client icon"));
            return;
        }

        data = pngBuffer.data().toBase64();
    }
    job->success({ { QStringLiteral("png"), QString::fromUtf8(data) } });
}

void SocketApi::emailPrivateLink(const QString &link)
{
    Utility::openEmailComposer(
        tr("I shared something with you"),
        link,
        nullptr);
}

void OCC::SocketApi::openPrivateLink(const QString &link)
{
    Utility::openBrowser(link, nullptr);
}

void SocketApi::command_GET_STRINGS(const QString &argument, SocketListener *listener)
{
    static std::array<std::pair<const char *, QString>, 5> strings { {
        { "SHARE_MENU_TITLE", tr("Share...") },
        { "CONTEXT_MENU_TITLE", Theme::instance()->appNameGUI() },
        { "COPY_PRIVATE_LINK_MENU_TITLE", tr("Copy private link to clipboard") },
        { "EMAIL_PRIVATE_LINK_MENU_TITLE", tr("Send private link by email...") },
    } };
    listener->sendMessage(QString("GET_STRINGS:BEGIN"));
    for (auto key_value : strings) {
        if (argument.isEmpty() || argument == QLatin1String(key_value.first)) {
            listener->sendMessage(QString("STRING:%1:%2").arg(key_value.first, key_value.second));
        }
    }
    listener->sendMessage(QString("GET_STRINGS:END"));
}

void SocketApi::sendSharingContextMenuOptions(const FileData &fileData, SocketListener *listener)
{
    auto record = fileData.journalRecord();
    bool isOnTheServer = record.isValid();
    auto flagString = isOnTheServer ? QStringLiteral("::") : QStringLiteral(":d:");

    auto capabilities = fileData.folder->accountState()->account()->capabilities();
    auto theme = Theme::instance();
    if (!capabilities.shareAPI() || !(theme->userGroupSharing() || (theme->linkSharing() && capabilities.sharePublicLink())))
        return;

    // If sharing is globally disabled, do not show any sharing entries.
    // If there is no permission to share for this file, add a disabled entry saying so
    if (isOnTheServer && !record._remotePerm.isNull() && !record._remotePerm.hasPermission(RemotePermissions::CanReshare)) {
        listener->sendMessage(QStringLiteral("MENU_ITEM:DISABLED:d:") + (!record.isDirectory() ? tr("Resharing this file is not allowed") : tr("Resharing this folder is not allowed")));
    } else {
        listener->sendMessage(QStringLiteral("MENU_ITEM:SHARE") + flagString + tr("Share..."));

        // Do we have public links?
        bool publicLinksEnabled = theme->linkSharing() && capabilities.sharePublicLink();

        // Is is possible to create a public link without user choices?
        bool canCreateDefaultPublicLink = publicLinksEnabled
            && !capabilities.sharePublicLinkEnforcePasswordForReadOnly();

        if (canCreateDefaultPublicLink) {
            listener->sendMessage(QStringLiteral("MENU_ITEM:COPY_PUBLIC_LINK") + flagString + tr("Create and copy public link to clipboard"));
        } else if (publicLinksEnabled) {
            listener->sendMessage(QStringLiteral("MENU_ITEM:MANAGE_PUBLIC_LINKS") + flagString + tr("Copy public link to clipboard"));
        }
    }

    listener->sendMessage(QStringLiteral("MENU_ITEM:COPY_PRIVATE_LINK") + flagString + tr("Copy private link to clipboard"));

    // Disabled: only providing email option for private links would look odd,
    // and the copy option is more general.
    //listener->sendMessage(QLatin1String("MENU_ITEM:EMAIL_PRIVATE_LINK") + flagString + tr("Send private link by email..."));
}

SocketApi::FileData SocketApi::FileData::get(const QString &localFile)
{
    FileData data;

    data.localPath = QDir::cleanPath(localFile);
    if (data.localPath.endsWith(QLatin1Char('/')))
        data.localPath.chop(1);

    data.folder = FolderMan::instance()->folderForPath(data.localPath, &data.folderRelativePath);
    if (!data.folder)
        return data;

    data.serverRelativePath = QDir(data.folder->remotePath()).filePath(data.folderRelativePath);
    if (data.folder->isReady()) {
        data.serverRelativePath = data.folder->vfs().underlyingFileName(data.serverRelativePath);
    }
    return data;
}

QString SocketApi::FileData::folderRelativePathNoVfsSuffix() const
{
    if (folder->isReady()) {
        return folder->vfs().underlyingFileName(folderRelativePath);
    }
    return folderRelativePath;
}

SyncFileStatus SocketApi::FileData::syncFileStatus() const
{
    if (!folder)
        return SyncFileStatus::StatusNone;
    return folder->syncEngine().syncFileStatusTracker().fileStatus(folderRelativePath);
}

SyncJournalFileRecord SocketApi::FileData::journalRecord() const
{
    SyncJournalFileRecord record;
    if (!folder)
        return record;
    folder->journalDb()->getFileRecord(folderRelativePath, &record);
    return record;
}

SocketApi::FileData SocketApi::FileData::parentFolder() const
{
    return FileData::get(QFileInfo(localPath).dir().path().toUtf8());
}

void SocketApi::command_GET_MENU_ITEMS(const QString &argument, OCC::SocketListener *listener)
{
    listener->sendMessage(QString("GET_MENU_ITEMS:BEGIN"));
    const QStringList files = split(argument);

    // Find the common sync folder.
    // syncFolder will be null if files are in different folders.
    Folder *folder = nullptr;
    for (const auto &file : files) {
        auto f = FolderMan::instance()->folderForPath(file);
        if (f != folder) {
            if (!folder) {
                folder = f;
            } else {
                folder = nullptr;
                break;
            }
        }
    }

    // Some options only show for single files
    if (files.size() == 1) {
        FileData fileData = FileData::get(files.first());
        auto record = fileData.journalRecord();
        bool isOnTheServer = record.isValid();
        auto flagString = isOnTheServer ? QLatin1String("::") : QLatin1String(":d:");

        if (fileData.folder && fileData.folder->accountState()->isConnected()) {
            sendSharingContextMenuOptions(fileData, listener);
            listener->sendMessage(QLatin1String("MENU_ITEM:OPEN_PRIVATE_LINK") + flagString + tr("Open in browser"));

            // Add link to versions pane if possible
            auto &capabilities = folder->accountState()->account()->capabilities();
            if (capabilities.versioningEnabled()
                && capabilities.privateLinkDetailsParamAvailable()
                && isOnTheServer
                && !record.isDirectory()) {
                listener->sendMessage(QLatin1String("MENU_ITEM:OPEN_PRIVATE_LINK_VERSIONS") + flagString + tr("Show file versions in browser"));
            }

            // Conflict files get conflict resolution actions
            bool isConflict = Utility::isConflictFile(fileData.folderRelativePath);
            if (isConflict || !isOnTheServer) {
                // Check whether this new file is in a read-only directory
                QFileInfo fileInfo(fileData.localPath);
                auto parentDir = fileData.parentFolder();
                auto parentRecord = parentDir.journalRecord();
                bool canAddToDir = parentRecord._remotePerm.isNull()
                    || (fileInfo.isFile() && !parentRecord._remotePerm.hasPermission(RemotePermissions::CanAddFile))
                    || (fileInfo.isDir() && !parentRecord._remotePerm.hasPermission(RemotePermissions::CanAddSubDirectories));
                bool canChangeFile =
                    !isOnTheServer
                    || (record._remotePerm.hasPermission(RemotePermissions::CanDelete)
                        && record._remotePerm.hasPermission(RemotePermissions::CanMove)
                        && record._remotePerm.hasPermission(RemotePermissions::CanRename));

                if (isConflict && canChangeFile) {
                    if (canAddToDir) {
                        if (isOnTheServer) {
                            // Conflict file that is already uploaded
                            listener->sendMessage(QLatin1String("MENU_ITEM:MOVE_ITEM::") + tr("Rename..."));
                        } else {
                            // Local-only conflict file
                            listener->sendMessage(QLatin1String("MENU_ITEM:MOVE_ITEM::") + tr("Rename and upload..."));
                        }
                    } else {
                        if (isOnTheServer) {
                            // Uploaded conflict file in read-only directory
                            listener->sendMessage(QLatin1String("MENU_ITEM:MOVE_ITEM::") + tr("Move and rename..."));
                        } else {
                            // Local-only conflict file in a read-only dir
                            listener->sendMessage(QLatin1String("MENU_ITEM:MOVE_ITEM::") + tr("Move, rename and upload..."));
                        }
                    }
                    listener->sendMessage(QLatin1String("MENU_ITEM:DELETE_ITEM::") + tr("Delete local changes"));
                }

                // File in a read-only directory?
                if (!isConflict && !isOnTheServer && !canAddToDir) {
                    listener->sendMessage(QLatin1String("MENU_ITEM:MOVE_ITEM::") + tr("Move and upload..."));
                    listener->sendMessage(QLatin1String("MENU_ITEM:DELETE_ITEM::") + tr("Delete"));
                }
            }
        }
    }

    // File availability actions
    if (folder
        && folder->isReady()
        && folder->virtualFilesEnabled()
        && folder->vfs().socketApiPinStateActionsShown()) {
        OC_ENFORCE(!files.isEmpty());

        // Determine the combined availability status of the files
        auto combined = Optional<VfsItemAvailability>();
        auto merge = [](VfsItemAvailability lhs, VfsItemAvailability rhs) {
            if (lhs == rhs)
                return lhs;
            if (int(lhs) > int(rhs))
                std::swap(lhs, rhs); // reduce cases ensuring lhs < rhs
            if (lhs == VfsItemAvailability::AlwaysLocal && rhs == VfsItemAvailability::AllHydrated)
                return VfsItemAvailability::AllHydrated;
            if (lhs == VfsItemAvailability::AllDehydrated && rhs == VfsItemAvailability::OnlineOnly)
                return VfsItemAvailability::AllDehydrated;
            return VfsItemAvailability::Mixed;
        };
        for (const auto &file : files) {
            if (!folder->isReady()) {
                continue;
            }
            auto fileData = FileData::get(file);
            auto availability = folder->vfs().availability(fileData.folderRelativePath);
            if (!availability) {
                if (availability.error() == Vfs::AvailabilityError::DbError)
                    availability = VfsItemAvailability::Mixed;
                if (availability.error() == Vfs::AvailabilityError::NoSuchItem)
                    continue;
            }
            if (!combined) {
                combined = *availability;
            } else {
                combined = merge(*combined, *availability);
            }
        }

        // TODO: Should be a submenu, should use icons
        auto makePinContextMenu = [&](bool makeAvailableLocally, bool freeSpace) {
            listener->sendMessage(QStringLiteral("MENU_SEPARATOR:d::"));
            listener->sendMessage(QStringLiteral("MENU_ITEM:CURRENT_PIN:d:")
                + Utility::vfsCurrentAvailabilityText(*combined));
            listener->sendMessage(QStringLiteral("MENU_ITEM:MAKE_AVAILABLE_LOCALLY:")
                + (makeAvailableLocally ? QStringLiteral(":") : QStringLiteral("d:"))
                + Utility::vfsPinActionText());
            listener->sendMessage(QStringLiteral("MENU_ITEM:MAKE_ONLINE_ONLY:")
                + (freeSpace ? QStringLiteral(":") : QStringLiteral("d:"))
                + Utility::vfsFreeSpaceActionText());
        };

        if (combined) {
            switch (*combined) {
            case VfsItemAvailability::AlwaysLocal:
                makePinContextMenu(false, true);
                break;
            case VfsItemAvailability::AllHydrated:
            case VfsItemAvailability::Mixed:
                makePinContextMenu(true, true);
                break;
            case VfsItemAvailability::AllDehydrated:
            case VfsItemAvailability::OnlineOnly:
                makePinContextMenu(true, false);
                break;
            }
        }
    }

    listener->sendMessage(QStringLiteral("GET_MENU_ITEMS:END"));
}

#if GUI_TESTING
void SocketApi::command_ASYNC_LIST_WIDGETS(const QSharedPointer<SocketApiJob> &job)
{
    QString response;
    for (auto &widget : allObjects(QApplication::allWidgets())) {
        auto objectName = widget->objectName();
        if (!objectName.isEmpty()) {
            response += objectName + ":" + widget->property("text").toString() + ", ";
        }
    }
    job->resolve(response);
}

void SocketApi::command_ASYNC_INVOKE_WIDGET_METHOD(const QSharedPointer<SocketApiJob> &job)
{
    auto &arguments = job->arguments();

    auto widget = findWidget(arguments["objectName"].toString());
    if (!widget) {
        job->reject(QLatin1String("widget not found"));
        return;
    }

    QMetaObject::invokeMethod(widget, arguments["method"].toString().toUtf8().constData());
    job->resolve();
}

void SocketApi::command_ASYNC_GET_WIDGET_PROPERTY(const QSharedPointer<SocketApiJob> &job)
{
    QString widgetName = job->arguments()[QLatin1String("objectName")].toString();
    auto widget = findWidget(widgetName);
    if (!widget) {
        QString message = QString(QLatin1String("Widget not found: 2: %1")).arg(widgetName);
        job->reject(message);
        return;
    }

    auto propertyName = job->arguments()[QLatin1String("property")].toString();

    auto segments = propertyName.split('.');

    QObject *currentObject = widget;
    QString value;
    for (int i = 0; i < segments.count(); i++) {
        auto segment = segments.at(i);
        auto var = currentObject->property(segment.toUtf8().constData());

        if (var.canConvert<QString>()) {
            var.convert(QMetaType::QString);
            value = var.value<QString>();
            break;
        }

        auto tmpObject = var.value<QObject *>();
        if (tmpObject) {
            currentObject = tmpObject;
        } else {
            QString message = QString(QLatin1String("Widget not found: 3: %1")).arg(widgetName);
            job->reject(message);
            return;
        }
    }

    job->resolve(value);
}

void SocketApi::command_ASYNC_SET_WIDGET_PROPERTY(const QSharedPointer<SocketApiJob> &job)
{
    auto &arguments = job->arguments();
    QString widgetName = arguments["objectName"].toString();
    auto widget = findWidget(widgetName);
    if (!widget) {
        QString message = QString(QLatin1String("Widget not found: 4: %1")).arg(widgetName);
        job->reject(message);
        return;
    }
    widget->setProperty(arguments["property"].toString().toUtf8().constData(),
        arguments["value"]);

    job->resolve();
}

void SocketApi::command_ASYNC_WAIT_FOR_WIDGET_SIGNAL(const QSharedPointer<SocketApiJob> &job)
{
    auto &arguments = job->arguments();
    QString widgetName = arguments["objectName"].toString();
    auto widget = findWidget(arguments["objectName"].toString());
    if (!widget) {
        QString message = QString(QLatin1String("Widget not found: 5: %1")).arg(widgetName);
        job->reject(message);
        return;
    }

    ListenerClosure *closure = new ListenerClosure([job]() { job->resolve("signal emitted"); });

    auto signalSignature = arguments["signalSignature"].toString();
    signalSignature.prepend("2");
    auto utf8 = signalSignature.toUtf8();
    auto signalSignatureFinal = utf8.constData();
    connect(widget, signalSignatureFinal, closure, SLOT(closureSlot()), Qt::QueuedConnection);
}

void SocketApi::command_ASYNC_TRIGGER_MENU_ACTION(const QSharedPointer<SocketApiJob> &job)
{
    auto &arguments = job->arguments();

    auto objectName = arguments["objectName"].toString();
    auto widget = findWidget(objectName);
    if (!widget) {
        QString message = QString(QLatin1String("Object not found: 1: %1")).arg(objectName);
        job->reject(message);
        return;
    }

    auto children = widget->findChildren<QWidget *>();
    for (auto childWidget : children) {
        // foo is the popupwidget!
        auto actions = childWidget->actions();
        for (auto action : actions) {
            if (action->objectName() == arguments["actionName"].toString()) {
                action->trigger();

                job->resolve("action found");
                return;
            }
        }
    }

    QString message = QString(QLatin1String("Action not found: 1: %1")).arg(arguments["actionName"].toString());
    job->reject(message);
}

void SocketApi::command_ASYNC_ASSERT_ICON_IS_EQUAL(const QSharedPointer<SocketApiJob> &job)
{
    auto widget = findWidget(job->arguments()[QLatin1String("queryString")].toString());
    if (!widget) {
        QString message = QString(QLatin1String("Object not found: 6: %1")).arg(job->arguments()["queryString"].toString());
        job->reject(message);
        return;
    }

    auto propertyName = job->arguments()[QLatin1String("propertyPath")].toString();

    auto segments = propertyName.split('.');

    QObject *currentObject = widget;
    QIcon value;
    for (int i = 0; i < segments.count(); i++) {
        auto segment = segments.at(i);
        auto var = currentObject->property(segment.toUtf8().constData());

        if (var.canConvert<QIcon>()) {
            var.convert(QMetaType::QIcon);
            value = var.value<QIcon>();
            break;
        }

        auto tmpObject = var.value<QObject *>();
        if (tmpObject) {
            currentObject = tmpObject;
        } else {
            job->reject(QString(QLatin1String("Icon not found: %1")).arg(propertyName));
        }
    }

    auto iconName = job->arguments()[QLatin1String("iconName")].toString();
    if (value.name() == iconName) {
        job->resolve();
    } else {
        job->reject("iconName " + iconName + " does not match: " + value.name());
    }
}
#endif

QString SocketApi::buildRegisterPathMessage(const QString &path)
{
    QFileInfo fi(path);
    QString message = QLatin1String("REGISTER_PATH:");
    message.append(QDir::toNativeSeparators(fi.absoluteFilePath()));
    return message;
}

void SocketApiJob::resolve(const QString &response)
{
    _socketListener->sendMessage(QStringLiteral("RESOLVE|") + _jobId + QLatin1Char('|') + response);
}

void SocketApiJob::resolve(const QJsonObject &response)
{
    resolve(QJsonDocument { response }.toJson());
}

void SocketApiJob::reject(const QString &response)
{
    _socketListener->sendMessage(QStringLiteral("REJECT|") + _jobId + QLatin1Char('|') + response);
}

SocketApiJobV2::SocketApiJobV2(const QSharedPointer<SocketListener> &socketListener, const QByteArray &command, const QJsonObject &arguments)
    : _socketListener(socketListener)
    , _command(command)
    , _jobId(arguments[QStringLiteral("id")].toString())
    , _arguments(arguments[QStringLiteral("arguments")].toObject())
{
    OC_ASSERT(!_jobId.isEmpty());
}

void SocketApiJobV2::success(const QJsonObject &response) const
{
    doFinish(response);
}

void SocketApiJobV2::failure(const QString &error) const
{
    doFinish({ { QStringLiteral("error"), error } });
}

void SocketApiJobV2::doFinish(const QJsonObject &obj) const
{
    QJsonObject data { { QStringLiteral("id"), _jobId }, { QStringLiteral("arguments"), obj } };
    if (!_warning.isEmpty()) {
        data[QStringLiteral("warning")] = _warning;
    }
    _socketListener->sendMessage(_command + QStringLiteral("_RESULT:") + QJsonDocument(data).toJson(QJsonDocument::Compact));
    Q_EMIT finished();
}

QString SocketApiJobV2::warning() const
{
    return _warning;
}

void SocketApiJobV2::setWarning(const QString &warning)
{
    _warning = warning;
}

} // namespace OCC

#include "socketapi.moc"
