// SPDX-License-Identifier: MIT
#include "HostingClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace aslgc {

namespace {

constexpr int kPerPage = 100;
constexpr int kMaxPages = 20;

QString errorText(QNetworkReply* reply, int status, const QByteArray& body) {
    QStringList parts;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        const QJsonValue message = o.value(QStringLiteral("message"));
        if (message.isString())
            parts << message.toString();
        else if (message.isArray())
            parts << QString::fromUtf8(QJsonDocument(message.toArray()).toJson(QJsonDocument::Compact));
        else if (message.isObject())
            parts << QString::fromUtf8(QJsonDocument(message.toObject()).toJson(QJsonDocument::Compact));
        if (parts.isEmpty()) {
            const QString err =
                o.value(QStringLiteral("error_description")).toString(o.value(QStringLiteral("error")).toString());
            if (!err.isEmpty()) parts << err;
        }
        for (const QJsonValue& e : o.value(QStringLiteral("errors")).toArray()) {
            if (e.isString())
                parts << e.toString();
            else if (e.toObject().value(QStringLiteral("message")).isString())
                parts << e.toObject().value(QStringLiteral("message")).toString();
        }
    }
    if (parts.isEmpty()) parts << reply->errorString();
    const QString text = parts.join(QStringLiteral(" — "));
    return status > 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(text) : text;
}

HostedRepo parseRepo(Provider provider, const QJsonObject& o) {
    HostedRepo r;
    if (provider == Provider::GitHub) {
        r.fullName = o.value(QStringLiteral("full_name")).toString();
        r.httpsUrl = o.value(QStringLiteral("clone_url")).toString();
        r.sshUrl = o.value(QStringLiteral("ssh_url")).toString();
        r.webUrl = o.value(QStringLiteral("html_url")).toString();
        r.isPrivate = o.value(QStringLiteral("private")).toBool();
    } else {
        r.fullName = o.value(QStringLiteral("path_with_namespace")).toString();
        r.httpsUrl = o.value(QStringLiteral("http_url_to_repo")).toString();
        r.sshUrl = o.value(QStringLiteral("ssh_url_to_repo")).toString();
        r.webUrl = o.value(QStringLiteral("web_url")).toString();
        r.isPrivate = o.value(QStringLiteral("visibility")).toString() != QLatin1String("public");
    }
    r.description = o.value(QStringLiteral("description")).toString().simplified();
    r.defaultBranch = o.value(QStringLiteral("default_branch")).toString();
    return r;
}

}  // namespace

HostingClient::HostingClient(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void HostingClient::send(const Account& account, const QString& token, const QByteArray& verb, const QUrl& url,
                         const QByteArray& body, Reply done) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "aslgc/" ASLGC_VERSION);
    request.setTransferTimeout(30000);
    if (account.provider == Provider::GitHub) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    } else {
        request.setRawHeader("PRIVATE-TOKEN", token.toUtf8());
    }
    if (!body.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    finish(m_net->sendCustomRequest(request, verb, body), std::move(done));
}

void HostingClient::postForm(const QUrl& url, const QByteArray& form, Reply done) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "aslgc/" ASLGC_VERSION);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(30000);
    finish(m_net->post(request, form), std::move(done));
}

void HostingClient::finish(QNetworkReply* reply, Reply done) {
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        const QByteArray data = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const QString error = ok ? QString() : errorText(reply, status, data);
        if (!ok)
            qWarning().noquote() << "API request" << reply->url().toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery)
                                 << "failed:" << error;
        done(status, QJsonDocument::fromJson(data), error);
    });
}

namespace {
QByteArray formEncode(std::initializer_list<std::pair<const char*, QString>> fields) {
    QByteArray out;
    for (const auto& [key, value] : fields) {
        if (!out.isEmpty()) out += '&';
        out += key;
        out += '=';
        out += QUrl::toPercentEncoding(value);
    }
    return out;
}
}  // namespace

void HostingClient::requestDeviceCode(const QString& host, const QString& clientId, const QString& scopes,
                                      std::function<void(const DeviceAuthorization&, const QString&)> done) {
    postForm(QUrl(QStringLiteral("https://%1/login/device/code").arg(host)),
             formEncode({{"client_id", clientId}, {"scope", scopes}}),
             [done = std::move(done)](int status, const QJsonDocument& json, const QString& error) {
                 const QJsonObject o = json.object();
                 DeviceAuthorization auth;
                 auth.deviceCode = o.value(QStringLiteral("device_code")).toString();
                 auth.userCode = o.value(QStringLiteral("user_code")).toString();
                 auth.verificationUri = o.value(QStringLiteral("verification_uri")).toString();
                 auth.interval = qMax(1, o.value(QStringLiteral("interval")).toInt(5));
                 auth.expiresIn = o.value(QStringLiteral("expires_in")).toInt(900);
                 QString message = status == 404 ? QObject::tr(
                                                       "GitHub does not recognise the OAuth App client ID "
                                                       "(or device flow is not enabled for it).")
                                                 : error;
                 if (message.isEmpty() && o.contains(QStringLiteral("error"))) {
                     message = o.value(QStringLiteral("error_description")).toString(o.value(QStringLiteral("error")).toString());
                 }
                 if (message.isEmpty() && (auth.deviceCode.isEmpty() || auth.userCode.isEmpty())) {
                     message = QObject::tr("GitHub did not return a device code.");
                 }
                 done(auth, message);
             });
}

void HostingClient::pollDeviceToken(const QString& host, const QString& clientId, const QString& deviceCode,
                                    std::function<void(DevicePoll, const QString&, int)> done) {
    postForm(QUrl(QStringLiteral("https://%1/login/oauth/access_token").arg(host)),
             formEncode({{"client_id", clientId},
                         {"device_code", deviceCode},
                         {"grant_type", QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")}}),
             [done = std::move(done)](int, const QJsonDocument& json, const QString& error) {
                 const QJsonObject o = json.object();
                 const int interval = o.value(QStringLiteral("interval")).toInt(0);
                 const QString token = o.value(QStringLiteral("access_token")).toString();
                 if (!token.isEmpty()) {
                     done(DevicePoll::Granted, token, interval);
                     return;
                 }
                 const QString code = o.value(QStringLiteral("error")).toString();
                 if (code == QLatin1String("authorization_pending"))
                     done(DevicePoll::Pending, {}, interval);
                 else if (code == QLatin1String("slow_down"))
                     done(DevicePoll::SlowDown, {}, interval);
                 else if (code == QLatin1String("expired_token"))
                     done(DevicePoll::Failed, QObject::tr("The code expired. Start the sign-in again."), 0);
                 else if (code == QLatin1String("access_denied"))
                     done(DevicePoll::Failed, QObject::tr("Authorization was cancelled in the browser."), 0);
                 else if (!code.isEmpty())
                     done(DevicePoll::Failed, o.value(QStringLiteral("error_description")).toString(code), 0);
                 else
                     done(DevicePoll::Failed, error.isEmpty() ? QObject::tr("Unexpected response from GitHub.") : error, 0);
             });
}

void HostingClient::fetchUser(const Account& account, const QString& token,
                              std::function<void(const HostedUser&, const QString&)> done) {
    send(account, token, "GET", QUrl(account.apiUrl() + QStringLiteral("/user")), {},
         [done = std::move(done), provider = account.provider](int, const QJsonDocument& json, const QString& error) {
             if (!error.isEmpty()) {
                 done({}, error);
                 return;
             }
             const QJsonObject o = json.object();
             HostedUser user;
             user.username =
                 o.value(provider == Provider::GitHub ? QStringLiteral("login") : QStringLiteral("username")).toString();
             user.name = o.value(QStringLiteral("name")).toString();
             done(user, user.username.isEmpty() ? QObject::tr("The server did not return a user name.") : QString());
         });
}

void HostingClient::listRepositories(const Account& account, const QString& token,
                                     std::function<void(const QVector<HostedRepo>&, const QString&)> done) {
    listPage(account, token, 1, std::make_shared<QVector<HostedRepo>>(), std::move(done));
}

void HostingClient::listPage(const Account& account, const QString& token, int page,
                             std::shared_ptr<QVector<HostedRepo>> collected,
                             std::function<void(const QVector<HostedRepo>&, const QString&)> done) {
    QUrl url;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("per_page"), QString::number(kPerPage));
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    if (account.provider == Provider::GitHub) {
        url = QUrl(account.apiUrl() + QStringLiteral("/user/repos"));
        query.addQueryItem(QStringLiteral("sort"), QStringLiteral("updated"));
        query.addQueryItem(QStringLiteral("affiliation"), QStringLiteral("owner,collaborator,organization_member"));
    } else {
        url = QUrl(account.apiUrl() + QStringLiteral("/projects"));
        query.addQueryItem(QStringLiteral("membership"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("simple"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("order_by"), QStringLiteral("last_activity_at"));
    }
    url.setQuery(query);
    send(account, token, "GET", url, {},
         [this, account, token, page, collected, done = std::move(done)](int, const QJsonDocument& json,
                                                                         const QString& error) mutable {
             if (!error.isEmpty()) {
                 done(*collected, error);
                 return;
             }
             const QJsonArray items = json.array();
             for (const QJsonValue& v : items) collected->push_back(parseRepo(account.provider, v.toObject()));
             if (items.size() == kPerPage && page < kMaxPages)
                 listPage(account, token, page + 1, collected, std::move(done));
             else
                 done(*collected, {});
         });
}

void HostingClient::createReviewRequest(const Account& account, const QString& token, const QString& repoPath,
                                        const QString& source, const QString& target, const QString& title, const QString& body,
                                        bool draft, std::function<void(const QString&, const QString&)> done) {
    QJsonObject payload;
    QUrl url;
    if (account.provider == Provider::GitHub) {
        url = QUrl(account.apiUrl() + QStringLiteral("/repos/") + repoPath + QStringLiteral("/pulls"));
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("head"), source);
        payload.insert(QStringLiteral("base"), target);
        payload.insert(QStringLiteral("body"), body);
        payload.insert(QStringLiteral("draft"), draft);
    } else {
        url = QUrl::fromEncoded((account.apiUrl() + QStringLiteral("/projects/")).toUtf8() + QUrl::toPercentEncoding(repoPath) +
                                "/merge_requests");
        payload.insert(QStringLiteral("source_branch"), source);
        payload.insert(QStringLiteral("target_branch"), target);
        payload.insert(QStringLiteral("title"), draft ? QStringLiteral("Draft: ") + title : title);
        payload.insert(QStringLiteral("description"), body);
    }
    send(account, token, "POST", url, QJsonDocument(payload).toJson(QJsonDocument::Compact),
         [done = std::move(done)](int, const QJsonDocument& json, const QString& error) {
             if (!error.isEmpty()) {
                 done({}, error);
                 return;
             }
             const QJsonObject o = json.object();
             done(o.value(QStringLiteral("html_url")).toString(o.value(QStringLiteral("web_url")).toString()), {});
         });
}

}  // namespace aslgc
