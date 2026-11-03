#pragma once
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <functional>

// Search-only tool: the model supplies a query, never a URL or executable code.
namespace BrowserSearch {
inline QUrl page(const QString& query) {
    QUrl url("https://www.bing.com/search");
    QUrlQuery parameters; parameters.addQueryItem("q", query);
    url.setQuery(parameters); return url;
}
inline QJsonObject search(const QString& query, const std::function<bool()>& cancelled) {
    const auto clean = query.trimmed();
    if (clean.isEmpty() || clean.size() > 300 || clean.contains(QChar::Null))
        return {{"error", "検索語は1〜300文字で指定してください。"}};
    if (cancelled()) return {{"error", "検索は中断されました。"}};
    auto url = page(clean); QUrlQuery parameters(url); parameters.addQueryItem("format", "rss"); url.setQuery(parameters);
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "CharaShell/0.1 WebSearch");
    request.setTransferTimeout(12000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto reply = network.get(request);
    QEventLoop loop; QTimer poll, deadline;
    poll.setInterval(50); deadline.setSingleShot(true);
    QByteArray bytes; bool tooLarge = false, timedOut = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        bytes += reply->readAll();
        if (bytes.size() > 512 * 1024) { tooLarge = true; reply->abort(); }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (cancelled()) reply->abort(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { timedOut = true; reply->abort(); });
    poll.start(); deadline.start(15000);
    if (!reply->isFinished()) loop.exec();
    bytes += reply->readAll();
    if (cancelled()) return {{"error", "検索は中断されました。"}};
    if (tooLarge || timedOut || reply->error() != QNetworkReply::NoError ||
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return {{"error", "検索結果を取得できませんでした。"}, {"query", clean}};
    QXmlStreamReader xml(bytes); QJsonArray results;
    while (!xml.atEnd() && results.size() < 3) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("item")) continue;
        QString title, link, snippet;
        while (xml.readNextStartElement()) {
            const auto name = xml.name().toString();
            if (name == "title") title = xml.readElementText().left(100);
            else if (name == "link") link = xml.readElementText();
            else if (name == "description") snippet = xml.readElementText().left(200);
            else xml.skipCurrentElement();
        }
        const QUrl target(link);
        if (target.isValid() && (target.scheme() == "https" || target.scheme() == "http") &&
            !target.host().isEmpty() && target.userInfo().isEmpty() && link.size() <= 400)
            results.append(QJsonObject{{"title", title}, {"url", link}, {"snippet", snippet}});
    }
    if (xml.hasError() || results.isEmpty())
        return {{"error", "検索結果が空、または検索サービスの応答を読み取れませんでした。"}, {"query", clean}};
    return {{"query", clean}, {"results", results}};
}
}
