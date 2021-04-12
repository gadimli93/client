/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
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

#include "socketuploadjob.h"
#include "socketapi_p.h"

#include "application.h"
#include "accountmanager.h"
#include "common/syncjournaldb.h"
#include "csync_exclude.h"
#include "progressdispatcher.h"
#include "syncengine.h"
#include "theme.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryFile>

using namespace OCC;

SocketUploadJob::SocketUploadJob(const QSharedPointer<SocketApiJobV2> &job)
    : _apiJob(job)
{
    connect(job.data(), &SocketApiJobV2::finished, this, &SocketUploadJob::deleteLater);
}

void SocketUploadJob::start()
{
    _localPath = _apiJob->arguments()[QLatin1String("localPath")].toString();
    auto remotePath = _apiJob->arguments()[QLatin1String("remotePath")].toString();
    if (!remotePath.startsWith(QLatin1Char('/'))) {
        remotePath = QLatin1Char('/') + remotePath;
    }

    const auto pattern = _apiJob->arguments()[QLatin1String("pattern")].toString();
    const auto excludes = _apiJob->arguments()[QLatin1String("excludes")].toArray();
    const auto accname = _apiJob->arguments()[QLatin1String("account")][QLatin1String("name")].toString();
    const auto accUUID = QUuid::fromString(_apiJob->arguments()[QLatin1String("account")][QLatin1String("uuid")].toString());
    AccountStatePtr account;
    if (accUUID.isNull()) {
        _apiJob->setWarning("Using the name as identifier is deprecated, please use the uuid");
        account = AccountManager::instance()->account(accname);
    } else {
        account = AccountManager::instance()->account(accUUID);
    }

    logMessage(_localPath, tr("Backup of %1 started").arg(QDir::toNativeSeparators(_localPath)));

    if (!account) {
        fail(tr("Failed to find %1").arg(QString::fromUtf8(QJsonDocument(_apiJob->arguments()[QLatin1String("account")].toObject()).toJson())));
        return;
    }

    if (!QFileInfo(_localPath).isAbsolute()) {
        fail(tr("Local path must be a an absolute path"));
        return;
    }
    auto tmp = new QTemporaryFile(this);
    if (!tmp->open()) {
        fail(tr("Failed to create temporary database"));
        return;
    }

    auto db = new SyncJournalDb(tmp->fileName(), this);
    auto engine = new SyncEngine(account->account(), _localPath.endsWith(QLatin1Char('/')) ? _localPath : _localPath + QLatin1Char('/'), remotePath, db);
    engine->setParent(db);

    for (const auto &i : excludes) {
        engine->excludedFiles().addManualExclude(i.toString());
    }

    connect(engine, &OCC::SyncEngine::transmissionProgress, this, [this](const ProgressInfo &info) {
        Q_EMIT ProgressDispatcher::instance()->progressInfo(_localPath, info);
    });
    connect(engine, &OCC::SyncEngine::itemCompleted, this, [this](const OCC::SyncFileItemPtr item) {
        _syncedFiles.append(item->_file);
    });

    connect(engine, &OCC::SyncEngine::finished, this, [this](bool ok) {
        if (ok) {
            logMessage(_localPath, tr("Backup of %1 succeeded").arg(QDir::toNativeSeparators(_localPath)));
            _apiJob->success({ { QStringLiteral("localPath"), _localPath }, { QStringLiteral("syncedFiles"), QJsonArray::fromStringList(_syncedFiles) } });
        }
    });
    connect(engine, &OCC::SyncEngine::syncError, this, [this](const QString &error, ErrorCategory) {
        fail(error);
    });

    auto opt = engine->syncOptions();
    opt.setFilePattern(pattern);
    if (!opt.fileRegex().isValid()) {
        fail(opt.fileRegex().errorString());
        return;
    }
    engine->setSyncOptions(opt);

    // create the dir, fail if it already exists
    auto mkdir = new OCC::MkColJob(engine->account(), remotePath);
    connect(mkdir, &OCC::MkColJob::finishedWithoutError, engine, &OCC::SyncEngine::startSync);
    connect(mkdir, &OCC::MkColJob::finishedWithError, this, [remotePath, this](QNetworkReply *reply) {
        if (reply->error() == 202) {
            fail(QStringLiteral("Destination %1 already exists").arg(remotePath));
        } else {
            fail(reply->errorString());
        }
    });
    mkdir->start();
}

void SocketUploadJob::fail(const QString &error)
{
    logMessage(_localPath, tr("Backup of %1 failed with: %2").arg(QDir::toNativeSeparators(_localPath), error), false);
    _apiJob->failure(error);
}

void SocketUploadJob::logMessage(const QString &localPath, const QString &message, bool ok)
{
    auto item = SyncFileItemPtr::create();
    QIcon icon; // null icon will cause the default icon
    if (ok) {
        item->_status = SyncFileItem::Success;
        item->_messageString = message;
    } else {
        icon = Theme::instance()->syncStateIcon(SyncResult::Error);
        item->_status = SyncFileItem::FatalError;
        item->_errorString = message;
    }
    ocApp()->gui()->slotShowTrayMessage(tr("%1 backup").arg(ocApp()->applicationName()), message, icon);
    item->_responseTimeStamp = QDateTime::currentDateTime().toString(Qt::RFC2822Date).toUtf8();
    Q_EMIT ProgressDispatcher::instance()->itemCompleted(QDir::toNativeSeparators(localPath), item);
}
