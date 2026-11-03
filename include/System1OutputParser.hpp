#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <optional>

struct System1Output {
    QString action, speech, question;
};

// Keep the HTTP schema, native grammar and defensive validation together.
class System1OutputParser {
public:
    static QJsonObject schema() {
        const QJsonObject speech{{"type", "string"}, {"minLength", 1}, {"maxLength", 400}};
        const QJsonObject question{{"type", "string"}, {"minLength", 1}, {"maxLength", 2000}};
        const auto branch = [&](const QJsonArray& actions, const QJsonObject& task) {
            return QJsonObject{{"type", "object"}, {"properties", QJsonObject{
                {"action", QJsonObject{{"type", "string"}, {"enum", actions}}},
                {"speech", speech}, {"task", task}}},
                {"required", QJsonArray{"action", "speech", "task"}}, {"additionalProperties", false}};
        };
        return QJsonObject{{"oneOf", QJsonArray{
            branch({"LOCAL", "CLARIFY"}, QJsonObject{{"type", "null"}}),
            branch({"THINK"}, QJsonObject{{"type", "object"}, {"properties", QJsonObject{{"question", question}}},
                {"required", QJsonArray{"question"}}, {"additionalProperties", false}})}}};
    }
    // Equivalent schema compiled to GBNF for the pinned llama.cpp C API.
    static const char* grammar() {
        return R"GBNF(root ::= "{" ws "\"action\"" ws ":" ws (local | think) ws "}"
local ::= ("\"LOCAL\"" | "\"CLARIFY\"") ws "," ws "\"speech\"" ws ":" ws speech ws "," ws "\"task\"" ws ":" ws "null"
think ::= "\"THINK\"" ws "," ws "\"speech\"" ws ":" ws speech ws "," ws "\"task\"" ws ":" ws "{" ws "\"question\"" ws ":" ws question ws "}"
speech ::= "\"" char{1,400} "\""
question ::= "\"" char{1,2000} "\""
char ::= [^"\\\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
ws ::= [ \t\n\r]*
)GBNF";
    }
    static bool validText(const QString& text, int maximum) {
        if (text.trimmed().isEmpty() || text.size() > maximum || text.contains(QChar::ReplacementCharacter)) return false;
        for (auto ch : text) if (ch.unicode() < 32 && ch != '\n' && ch != '\t') return false;
        return true;
    }
    static std::optional<System1Output> parse(const QString& text) {
        if (text.size() > 16384) return {};
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
        // QJsonDocument normalizes duplicate keys; reject that ambiguity before
        // accepting the normalized object. Scan string tokens, skipping escapes.
        int keys = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text[i] != '"') continue;
            for (++i; i < text.size(); ++i) {
                if (text[i] == '\\') { ++i; continue; }
                if (text[i] == '"') break;
            }
            auto next = i + 1;
            while (next < text.size() && text[next].isSpace()) ++next;
            if (next < text.size() && text[next] == ':') ++keys;
        }
        const auto obj = doc.object();
        if (keys != obj.size() + (obj.value("task").isObject() ? obj.value("task").toObject().size() : 0)) return {};
        if (obj.size() != 3 || !obj.value("action").isString() || !obj.value("speech").isString() || !obj.contains("task")) return {};
        System1Output result{obj.value("action").toString(), obj.value("speech").toString().trimmed(), {}};
        if (!validText(result.speech, 400)) return {};
        if (result.action == "LOCAL" || result.action == "CLARIFY") {
            if (!obj.value("task").isNull()) return {};
        } else if (result.action == "THINK") {
            if (!obj.value("task").isObject()) return {};
            const auto task = obj.value("task").toObject();
            if (task.size() != 1 || !task.value("question").isString()) return {};
            result.question = task.value("question").toString().trimmed();
            if (!validText(result.question, 2000)) return {};
        } else return {};
        return result;
    }
};
