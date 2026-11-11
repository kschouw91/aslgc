// SPDX-License-Identifier: MIT
#pragma once

#include "Account.h"

#include <QJsonDocument>
#include <QObject>
#include <QVector>
#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

namespace aslgc {

struct HostedUser {
    QString username;
    QString name;
};

struct HostedRepo {
    QString fullName;  // owner/repo or group/subgroup/repo
    QString description;
    QString httpsUrl;
    QString sshUrl;
    QString webUrl;
    QString defaultBranch;
    bool isPrivate = false;
};

// Minimal REST client for GitHub (incl. Enterprise) and GitLab (incl. self-hosted).
// All callbacks run on the GUI thread; `error` is empty on success.
class HostingClient : public QObject {
    Q_OBJECT
public:
    explicit HostingClient(QObject* parent = nullptr);

    void fetchUser(const Account& account, const QString& token,
                   std::function<void(const HostedUser&, const QString& error)> done);
    void listRepositories(const Account& account, const QString& token,
                          std::function<void(const QVector<HostedRepo>&, const QString& error)> done);
    void createReviewRequest(const Account& account, const QString& token, const QString& repoPath, const QString& source,
                             const QString& target, const QString& title, const QString& body, bool draft,
                             std::function<void(const QString& url, const QString& error)> done);

    // GitHub OAuth device flow (RFC 8628): request a user code, then poll until the user
    // approves it in the browser. Needs the client ID of an OAuth App with device flow enabled.
    struct DeviceAuthorization {
        QString deviceCode;
        QString userCode;
        QString verificationUri;
        int interval = 5;     // seconds between polls
        int expiresIn = 900;  // seconds
    };
    enum class DevicePoll { Pending, SlowDown, Granted, Failed };
    void requestDeviceCode(const QString& host, const QString& clientId, const QString& scopes,
                           std::function<void(const DeviceAuthorization&, const QString& error)> done);
    // `value` is the access token when Granted, an error message when Failed.
    void pollDeviceToken(const QString& host, const QString& clientId, const QString& deviceCode,
                         std::function<void(DevicePoll result, const QString& value, int interval)> done);

private:
    using Reply = std::function<void(int status, const QJsonDocument& json, const QString& error)>;
    void postForm(const QUrl& url, const QByteArray& form, Reply done);
    void finish(QNetworkReply* reply, Reply done);
    void send(const Account& account, const QString& token, const QByteArray& verb, const QUrl& url, const QByteArray& body,
              Reply done);
    void listPage(const Account& account, const QString& token, int page, std::shared_ptr<QVector<HostedRepo>> collected,
                  std::function<void(const QVector<HostedRepo>&, const QString&)> done);

    QNetworkAccessManager* m_net;
};

}  // namespace aslgc
