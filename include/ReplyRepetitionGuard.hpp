#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <utility>

// Hold a stream only while it could be a verbatim copy of the previous reply.
// A rejected copy never reaches the sentence chunker or audio playback.
class ReplyRepetitionGuard {
    QString previous_, candidate_, held_;
    bool released_ = false;
    static QString normalized(QString text) {
        static const QRegularExpression whitespace(QStringLiteral("\\s+"));
        return text.remove(whitespace);
    }
public:
    void begin(const QJsonArray& history) {
        previous_.clear(); candidate_.clear(); held_.clear(); released_ = false;
        QString request;
        for (auto i = history.size(); i > 0; --i) {
            const auto message = history.at(i-1).toObject();
            if (request.isEmpty() && message.value("role").toString() == "user")
                request = message.value("content").toString();
            if (message.value("role").toString() == "assistant") {
                previous_ = normalized(message.value("content").toString());
                break;
            }
        }
        static const QRegularExpression repeatRequest(QString::fromUtf8(
            "復唱して|繰り返して(?:ください|ほしい|[。！!]|$)|"
            "同じ(?:文章|文|内容|返答)(?:を)?(?:もう一度)?(?:言って|読んで|返して)|"
            "(?:もう一度|もう一回|再度).{0,12}(?:言って|読んで|聞かせ|話して)|"
            "^(?:もう一度|もう一回)[。！!]?\\s*$"));
        // Short factual answers can legitimately coincide across different questions.
        released_ = previous_.size() < 12 || repeatRequest.match(request).hasMatch();
    }
    QString push(const QString& text) {
        candidate_ += text;
        if (released_) return text;
        held_ += text;
        const auto current = normalized(candidate_);
        if (!previous_.startsWith(current) && !current.startsWith(previous_)) {
            released_ = true;
            return std::exchange(held_, {});
        }
        return {};
    }
    bool duplicate() const {
        return !released_ && !previous_.isEmpty() && normalized(candidate_).startsWith(previous_);
    }
    QString finish() { return duplicate() ? QString() : std::exchange(held_, {}); }
};
