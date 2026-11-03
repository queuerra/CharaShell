#pragma once
#include <QNetworkRequest>
#include <QUrl>
#include <stdexcept>

inline QNetworkRequest localRequest(const QString& base, const QString& path) {
    QUrl url(base);
    // Literal loopback only: no DNS, proxies, redirects or remote services.
    if (url.scheme() != "http" || (url.host() != "127.0.0.1" && url.host() != "::1")
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        throw std::runtime_error("URL must use http://127.0.0.1 or http://[::1]");
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(120000);
    return request;
}
