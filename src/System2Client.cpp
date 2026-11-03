#include "System2Client.hpp"
#include "ConversationPrompt.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

System2Client::System2Client(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), network_(network ? network : new QNetworkAccessManager(this)) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail(QString::fromUtf8("Sarashinaの応答がタイムアウトしました。")); });
}
void System2Client::cancel() {
    timeout_.stop();
    if (reply_) { auto* r = reply_.data(); reply_ = nullptr; r->disconnect(this); r->abort(); r->deleteLater(); }
    parser_.clear();
}
void System2Client::fail(const QString& message) {
    const auto turn = turn_;
    cancel(); emit error(turn, message);
}
void System2Client::start(quint64 turn, const QString& summary, const QString& original,
    const QString& immediate, const QString& question, const QVariantMap& settings) {
    cancel(); turn_ = turn; bytes_ = characters_ = 0; completed_ = false;
    const auto key = settings.value("sarashinaApiKey").toString().trimmed();
    if (key.isEmpty()) { emit error(turn, QString::fromUtf8("Sarashina APIキーが設定されていません。")); return; }
    QFile prompt(":/prompts/system2.txt");
    if (!prompt.open(QIODevice::ReadOnly)) { emit error(turn, "Missing System 2 prompt"); return; }
    QJsonArray messages{QJsonObject{{"role", "system"}, {"content",
        ConversationPrompt::system(settings.value("systemPrompt").toString(), QString::fromUtf8(prompt.readAll()))}}};
    // Carry the handoff before the summary and leave the original utterance last.
    const QJsonObject handoff{{"immediate_response", immediate}, {"task", QJsonObject{{"question", question}}}};
    messages.append(QJsonObject{{"role", "user"}, {"content", ConversationPrompt::input(summary, original, handoff)}});
    QJsonObject body{{"model", settings.value("sarashinaModel", "sarashina3.2-mini-thinking").toString()},
        {"messages", messages}, {"stream", true}, {"max_tokens", 4096}};
    if (body.value("model") == "sarashina3.2-mini-thinking") body["reasoning_effort"] = "medium";
    QNetworkRequest request(QUrl("https://api.sarashina.ai/v1/chat/completions"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto* r = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply_ = r; r->setReadBufferSize(128 * 1024);
    timeout_.start(120000);
    connect(r, &QNetworkReply::readyRead, this, &System2Client::consume);
    connect(r, &QNetworkReply::finished, this, [this, r, turn] {
        if (reply_ != r) return;
        consume(); if (reply_ != r) return;
        if (r->error() != QNetworkReply::NoError || r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            fail(QString::fromUtf8("Sarashinaへの接続に失敗しました（HTTP %1）。").arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        } else if (!completed_ || !characters_) fail(QString::fromUtf8("Sarashinaの応答が空、または途中で終了しました。"));
        else { cancel(); emit finished(turn); }
    });
}
void System2Client::consume() {
    if (!reply_ || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
    try {
        const auto data = reply_->readAll(); bytes_ += data.size();
        if (bytes_ > 1024 * 1024) throw std::runtime_error("Sarashina stream exceeds limit");
        for (const auto& event : parser_.push(std::string_view(data.constData(), data.size()))) {
            if (event == "[DONE]") { completed_ = true; continue; }
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(event), &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object().contains("error"))
                throw std::runtime_error("Invalid Sarashina stream event");
            const auto choices = doc.object().value("choices").toArray();
            if (choices.isEmpty()) continue; // Usage event.
            const auto choice = choices.first().toObject();
            const auto reason = choice.value("finish_reason").toString();
            if (!reason.isEmpty() && reason != "stop") throw std::runtime_error("Sarashina response did not complete normally");
            const auto delta = choice.value("delta").toObject();
            if (!delta.value("tool_calls").toArray().isEmpty()) throw std::runtime_error("Unexpected tool call");
            // Ignore encrypted reasoning/reasoning_content; only final content is spoken.
            const auto text = delta.value("content").toString();
            characters_ += text.size();
            if (characters_ > 12000 || text.contains(QChar::ReplacementCharacter)) throw std::runtime_error("Invalid Sarashina answer text");
            if (!text.isEmpty()) {
                auto* current = reply_.data(); emit fragment(turn_, text);
                if (reply_ != current) return;
            }
            if (reason == "stop") completed_ = true;
        }
    } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
}
