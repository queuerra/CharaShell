#pragma once
#include "System1OutputParser.hpp"

// The summarizer has its own output contract; its JSON never reaches speech.
namespace ConversationSummary {
inline QJsonObject schema() {
    return {{"type", "object"}, {"properties", QJsonObject{{"summary", QJsonObject{
        {"type", "string"}, {"minLength", 1}, {"maxLength", 800}}}}},
        {"required", QJsonArray{"summary"}}, {"additionalProperties", false}};
}
inline const char* grammar() {
    return R"GBNF(root ::= "{" ws "\"summary\"" ws ":" ws "\"" char{1,800} "\"" ws "}" ws
char ::= [^"\\\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
ws ::= [ \t\n\r]*
)GBNF";
}
inline std::optional<QString> parse(const QString& text) {
    if (text.size() > 8192) return {};
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
    const auto object = doc.object();
    if (object.size() != 1 || !object.value("summary").isString()) return {};
    // Reject duplicate keys before QJsonDocument's normalization hides them.
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
    const auto summary = object.value("summary").toString().trimmed();
    if (keys != 1 || !System1OutputParser::validText(summary, 800)) return {};
    return summary;
}
}
