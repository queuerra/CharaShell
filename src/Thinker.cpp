#include "Thinker.hpp"
#include "System1OutputParser.hpp"
#include "ConversationSummary.hpp"
#include "LocalRequest.hpp"
#include "BrowserSearch.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QDebug>
#include <algorithm>
#ifdef CHARASHELL_NATIVE
#include "NativeInference.hpp"
#endif

Thinker::Thinker(QString cloudExecutable) : cloudExecutable_(std::move(cloudExecutable)) {}
Thinker::~Thinker() = default;

void Thinker::preload(quint64 generation, const QVariantMap& settings) {
    if (activeTurn_.load() != generation) return;
    try {
        if (settings.value("backend", "native").toString() == "native") {
#ifdef CHARASHELL_NATIVE
            if (!native_) native_ = std::make_unique<NativeThinker>();
            native_->load(generation, settings, activeTurn_, [this, generation](double value, const QString& status) {
                if (activeTurn_.load() == generation) emit loadProgress(generation, value, status);
            });
#else
            throw std::runtime_error("Native inference is not included in this build");
#endif
        }
        if (activeTurn_.load() == generation) emit loaded(generation);
    } catch (const std::exception& e) {
        if (activeTurn_.load() == generation) emit loadFailed(generation, QString::fromUtf8(e.what()));
    }
}

void Thinker::beginReply(const QJsonArray& history) {
    repetition_.begin(history);
}

void Thinker::emitReply(quint64 turn, const QString& text) {
    if (structured_) { emit fragment(turn, text); return; }
    const auto visible = repetition_.push(text);
    if (!visible.isEmpty()) emit fragment(turn, visible);
}

void Thinker::finishReply(quint64 turn, const QJsonArray& history, const QVariantMap& settings) {
    if (activeTurn_.load() != 0 && activeTurn_.load() != turn) return;
    if (structured_) { emit finished(turn); return; }
    if (!repetition_.duplicate()) {
        const auto tail = repetition_.finish();
        if (!tail.isEmpty()) emit fragment(turn, tail);
        emit finished(turn);
        return;
    }
    if (settings.value("replyRegenerated", false).toBool()) {
        emit error(turn, QString::fromUtf8("直前と同じ回答が生成されたため、読み上げを止めました。"));
        return;
    }
    qWarning() << "Repeated reply suppressed; regenerating turn" << turn;
    auto retry = settings;
    retry["replyRegenerated"] = true;
    retry["systemPrompt"] = settings.value("systemPrompt").toString() +
        (settings.value("replyPurpose").toString() == "planning" ? QString::fromUtf8(
        "\n直前の生成は以前の返答のコピーになったため採用されなかった。今の依頼に固有の対象と確認する観点を踏まえて、"
        "短い返答と具体的な実行計画を新しく生成する。結果や結論はまだ作らず、口語の短い散文で話す。") : QString::fromUtf8(
        "\n直前の生成は前の回答のコピーになったため採用されなかった。履歴の最後のユーザー発話に改めて答える。"
        "以前の自分の発言は根拠ではない。特に疑い・否定・訂正を受けているなら、その指摘を受け止め、"
        "根拠を確認できない部分を認める。前の回答を復唱したり、言い回しだけ変えたりせず、"
        "今の発話への新しい返答を口語の短い散文で生成する。"));
    QMetaObject::invokeMethod(this, [this, turn, history, retry] {
        if (activeTurn_.load() == 0 || activeTurn_.load() == turn) generate(turn, history, retry);
    }, Qt::QueuedConnection);
}

void Thinker::cancel() {
    if (reply_) { auto r = reply_.data(); reply_ = nullptr; r->disconnect(this); r->abort(); r->deleteLater(); }
    if (cloudProcess_) {
        auto p = cloudProcess_.data(); cloudProcess_ = nullptr; p->disconnect(this);
        p->kill(); p->waitForFinished(1000); p->deleteLater();
    }
    cloudDirectory_.reset();
    cloudDiagnostic_.clear();
    parser_.clear();
}
void Thinker::generate(quint64 turn, const QJsonArray& history, const QVariantMap& settings) {
    if (activeTurn_.load() && activeTurn_.load() != turn) return;
    // Network search runs an event loop on the worker. A newly queued turn must
    // wait for the previous native call to unwind before touching its context.
    if (nativeBusy_) { pendingTurn_ = turn; pendingHistory_ = history; pendingSettings_ = settings; return; }
    structured_ = settings.value("system1Structured", false).toBool() || settings.value("conversationSummary", false).toBool();
    if (settings.value("useChatGPT").toBool()) {
        cancel();
        const auto executable = cloudExecutable_.isEmpty() ? QStandardPaths::findExecutable("codex") : cloudExecutable_;
        if (executable.isEmpty()) { emit error(turn, QString::fromUtf8("ChatGPT連携にはCodex CLIのインストールとサインインが必要です")); return; }
        cloudDirectory_ = std::make_unique<QTemporaryDir>();
        if (!cloudDirectory_->isValid()) { emit error(turn, QString::fromUtf8("ChatGPT用の一時フォルダを作成できません")); return; }
        const auto outputPath = cloudDirectory_->filePath("answer.txt");
        const auto schemaPath = cloudDirectory_->filePath("answer-schema.json");
        const auto question = history.isEmpty() ? QString() : history.last().toObject().value("content").toString();
        const bool searchingWeb = settings.value("chatgptWebSearch").toBool();
        const QJsonObject sourceSchema{{"type", "object"}, {"properties", QJsonObject{
            {"title", QJsonObject{{"type", "string"}}}, {"url", QJsonObject{{"type", "string"}}}}},
            {"required", QJsonArray{"title", "url"}}, {"additionalProperties", false}};
        const QJsonObject answerSchema{{"type", "object"}, {"properties", QJsonObject{
            {"reply", QJsonObject{{"type", "string"}}},
            {"sources", QJsonObject{{"type", "array"}, {"items", sourceSchema}}}}},
            {"required", QJsonArray{"reply", "sources"}}, {"additionalProperties", false}};
        QFile schemaFile(schemaPath);
        if (!schemaFile.open(QIODevice::WriteOnly) || schemaFile.write(QJsonDocument(answerSchema).toJson(QJsonDocument::Compact)) < 0) {
            emit error(turn, QString::fromUtf8("ChatGPTの応答形式を準備できません")); return;
        }
        schemaFile.close();
        QString prompt = QString::fromUtf8("あなたはCharaShellの高度な依頼を担当します。日本語で質問へ直接回答してください。会話相手の人格設定: ")
            + settings.value("systemPrompt").toString()
            + QString::fromUtf8("\nreply は質問への答えを口語の自然な散文で書いてください。見出しや箇条書きを使わず、要点をつなげて説明してください。定型の相槌や作業実況は不要です。URLや出典一覧は reply に入れず、参照したページの名前とURLを sources に分けてください。未確認のことは断定しないでください。ローカルファイルの読み書き、シェル操作、コード実行は行わないでください。\n")
            + (searchingWeb ? QString::fromUtf8("この依頼は最新情報を必要とするため、Web検索を使って確認してください。\n") : QString())
            + QString::fromUtf8("会話履歴:\n");
        for (const auto& item : history) {
            const auto object = item.toObject();
            prompt += object.value("role").toString() + ": " + object.value("content").toString() + "\n";
        }
        prompt += QString::fromUtf8("\n直近の依頼に答えてください。");
        auto* process = new QProcess(this);
        cloudProcess_ = process;
        cloudDiagnostic_.clear();
        process->setWorkingDirectory(cloudDirectory_->path());
        connect(process, &QProcess::readyReadStandardOutput, this, [process] { process->readAllStandardOutput(); });
        connect(process, &QProcess::readyReadStandardError, this, [this, process] {
            if (process != cloudProcess_) return;
            cloudDiagnostic_ += process->readAllStandardError();
            if (cloudDiagnostic_.size() > 8192) cloudDiagnostic_ = cloudDiagnostic_.right(8192);
        });
        QStringList arguments;
        arguments << "--ask-for-approval" << "never";
        if (searchingWeb) arguments << "--search";
        arguments << "exec" << "--ephemeral" << "--ignore-user-config" << "--skip-git-repo-check"
                  << "-s" << "read-only" << "-C" << cloudDirectory_->path()
                  << "--output-schema" << schemaPath << "-o" << outputPath << "-";
        connect(process, &QProcess::errorOccurred, this, [this, process, turn](QProcess::ProcessError reason) {
            if (process != cloudProcess_ || reason != QProcess::FailedToStart) return;
            cloudProcess_ = nullptr; cloudDirectory_.reset(); process->deleteLater();
            emit error(turn, QString::fromUtf8("Codex CLIを起動できません: ") + process->errorString());
        });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, turn, outputPath, searchingWeb](int exitCode, QProcess::ExitStatus status) {
                if (process != cloudProcess_) return;
                cloudProcess_ = nullptr;
                QFile answerFile(outputPath);
                const auto answer = answerFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(answerFile.readAll()).trimmed() : QString();
                answerFile.close();
                auto diagnostic = QString::fromUtf8(cloudDiagnostic_ + process->readAllStandardError()).trimmed();
                cloudDirectory_.reset(); process->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0 || answer.isEmpty()) {
                    if (diagnostic.size() > 500) diagnostic = diagnostic.right(500);
                    emit error(turn, QString::fromUtf8("Codex CLIの処理に失敗しました。 ") + diagnostic);
                    return;
                }
                QString replyText = answer;
                QJsonArray rawSources;
                const auto structured = QJsonDocument::fromJson(answer.toUtf8());
                if (structured.isObject() && structured.object().value("reply").isString()) {
                    replyText = structured.object().value("reply").toString().trimmed();
                    rawSources = structured.object().value("sources").toArray();
                } else if (searchingWeb) {
                    QRegularExpression links(R"(\[([^\]]+)\]\((https?://[^\s)]+)\))");
                    auto match = links.globalMatch(replyText);
                    while (match.hasNext()) {
                        const auto item = match.next();
                        rawSources.append(QJsonObject{{"title", item.captured(1)}, {"url", item.captured(2)}});
                    }
                    const QRegularExpression sourceHeading(QString::fromUtf8(R"((?im)^\s*(?:#{1,3}\s*)?(?:出典|参考資料|sources|references)\s*:?\s*$)"));
                    const auto heading = sourceHeading.match(replyText);
                    if (heading.hasMatch() && !rawSources.isEmpty()) replyText = replyText.left(heading.capturedStart()).trimmed();
                    replyText.replace(links, "\\1");
                }
                QJsonArray sources;
                QSet<QString> seen;
                for (const auto& item : rawSources) {
                    const auto source = item.toObject();
                    const auto title = source.value("title").toString().trimmed();
                    const auto url = source.value("url").toString().trimmed();
                    const QUrl parsed(url);
                    if (!parsed.isValid() || (parsed.scheme() != "https" && parsed.scheme() != "http") || seen.contains(url)) continue;
                    seen.insert(url);
                    sources.append(QJsonObject{{"title", title.isEmpty() ? url : title}, {"url", url}});
                    if (sources.size() == 8) break;
                }
                if (!sources.isEmpty()) emit searchResults(turn, sources);
                if (replyText.isEmpty()) { emit error(turn, QString::fromUtf8("ChatGPTの回答が空でした")); return; }
                emit fragment(turn, replyText); emit finished(turn);
            });
        if (searchingWeb) emit searching(turn, question, QString(), false);
        process->start(executable, arguments);
        if (process->state() != QProcess::NotRunning) { process->write(prompt.toUtf8()); process->closeWriteChannel(); }
        return;
    }
#ifdef CHARASHELL_NATIVE
    if (settings.value("backend", "native").toString() == "native") {
        if (activeTurn_.load() != turn) return;
        beginReply(history);
        nativeBusy_ = true;
        try {
            if (!native_) native_ = std::make_unique<NativeThinker>();
            native_->generate(turn, history, settings, activeTurn_, [this, turn](const QString& text) {
                if (activeTurn_.load() == turn) emitReply(turn, text);
            }, [this, turn, settings](const QString& query) {
                emit searching(turn, query, BrowserSearch::page(query).toString(), settings.value("searchOpenBrowser", true).toBool());
                const auto result = BrowserSearch::search(query, [this, turn] { return activeTurn_.load() != turn; });
                if (activeTurn_.load() == turn) emit searchResults(turn, result.value("results").toArray());
                return result;
            });
            if (activeTurn_.load() == turn) finishReply(turn, history, settings);
        } catch (const std::exception& e) { if (activeTurn_.load() == turn) emit error(turn, QString::fromUtf8(e.what())); }
        nativeBusy_ = false;
        if (pendingTurn_) {
            const auto next = pendingTurn_; const auto messages = pendingHistory_; const auto configuration = pendingSettings_;
            pendingTurn_ = 0; pendingHistory_ = {}; pendingSettings_.clear();
            if (activeTurn_.load() == next) QMetaObject::invokeMethod(this, [this, next, messages, configuration] {
                if (activeTurn_.load() == next) generate(next, messages, configuration);
            }, Qt::QueuedConnection);
        }
        return;
    }
#else
    if (settings.value("backend").toString() == "native") { emit error(turn, "Native inference is not included in this build"); return; }
#endif
    cancel(); completed_ = false;
    beginReply(history);
    if (!network_) { network_ = new QNetworkAccessManager(this); network_->setProxy(QNetworkProxy::NoProxy); }
    try {
        auto request = localRequest(settings.value("llmUrl").toString(), "/v1/chat/completions");
        QJsonArray messages{QJsonObject{{"role", "system"}, {"content", settings.value("systemPrompt").toString()}}};
        for (const auto& message : history) messages.append(message);
        QJsonObject body{{"model", settings.value("llmModel").toString()}, {"messages", messages},
            {"stream", true}, {"max_tokens", std::clamp(settings.value("maxOutputTokens", 256).toInt(), 32, 768)}, {"temperature", 0.7},
            {"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}}};
        if (structured_) body["response_format"] = QJsonObject{{"type", "json_schema"},
            {"json_schema", QJsonObject{{"name", settings.value("conversationSummary").toBool() ? "history_summary" : "system1"},
                {"strict", true}, {"schema", settings.value("conversationSummary").toBool() ? ConversationSummary::schema() : System1OutputParser::schema()}}}};
        auto r = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)); reply_ = r;
        connect(r, &QNetworkReply::readyRead, this, [this, r, turn] {
            if (r != reply_) return;
            if (activeTurn_.load() && activeTurn_.load() != turn) { cancel(); return; }
            if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
            try {
                auto bytes = r->readAll();
                for (const auto& data : parser_.push(std::string_view(bytes.constData(), bytes.size()))) {
                    if (data == "[DONE]") { completed_ = true; continue; }
                    QJsonParseError parseError{};
                    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(data), &parseError);
                    if (parseError.error != QJsonParseError::NoError) throw std::runtime_error("Invalid JSON in LLM stream");
                    if (doc.object().contains("error")) throw std::runtime_error("LLM returned an error event");
                    auto choices = doc.object().value("choices").toArray();
                    if (choices.isEmpty()) continue;
                    auto choice = choices.first().toObject();
                    auto text = choice.value("delta").toObject().value("content").toString();
                    if (!text.isEmpty()) emitReply(turn, text);
                    if (!choice.value("finish_reason").isNull() && !choice.value("finish_reason").isUndefined()) completed_ = true;
                }
            } catch (const std::exception& e) { cancel(); emit error(turn, QString::fromUtf8(e.what())); }
        });
        connect(r, &QNetworkReply::finished, this, [this, r, turn, history, settings] {
            if (r != reply_) return;
            if (activeTurn_.load() && activeTurn_.load() != turn) { cancel(); return; }
            reply_ = nullptr;
            if (r->error() != QNetworkReply::NoError || r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                emit error(turn, "LLM: " + r->errorString());
            else if (!completed_) emit error(turn, "LLMのストリームが途中で終了しました");
            else finishReply(turn, history, settings);
            r->deleteLater();
        });
    } catch (const std::exception& e) { emit error(turn, QString::fromUtf8(e.what())); }
}
