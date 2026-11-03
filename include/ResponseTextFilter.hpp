#pragma once
#include <QString>
#include <QRegularExpression>

// Buffer parentheses across token boundaries. Remove process narration and
// empty parentheses, while retaining factual notes such as dates and numbers.
class ResponseTextFilter {
    QString bracketed_;
    int depth_ = 0;
    static bool narration(const QString& text) {
        auto content = text;
        content.remove('('); content.remove(')');
        content.remove(QChar(0xff08)); content.remove(QChar(0xff09));
        if (content.trimmed().isEmpty()) return true;
        static const QRegularExpression stage(
            QString::fromUtf8("検索中|調査中|確認中|思考中|検索を(?:開始|実行|行|し)|調べ(?:ます|ています|てきます)|"
                "ツール(?:コール|呼び出し|を実行)|ブラウザ(?:を開|で検索)|検索結果を(?:取得|確認|まとめ)|"
                "(?:回答|要約)(?:を生成|を作成|中)|^(?:searching|thinking|checking|thought|reasoning|"
                "search in progress|calling (?:a )?tool|executing (?:a )?search)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        return stage.match(content.trimmed()).hasMatch();
    }
public:
    QString push(const QString& text) {
        QString visible;
        for (const auto character : text) {
            const bool open = character == '(' || character == QChar(0xff08);
            const bool close = character == ')' || character == QChar(0xff09);
            if (depth_ > 0) {
                bracketed_ += character;
                if (open) ++depth_;
                if (close && --depth_ == 0) {
                    if (!narration(bracketed_)) visible += bracketed_;
                    bracketed_.clear();
                }
            } else if (open) {
                bracketed_ = character; depth_ = 1;
            } else visible += character;
        }
        return visible;
    }
    QString flush() {
        const auto visible = narration(bracketed_) ? QString() : bracketed_;
        bracketed_.clear(); depth_ = 0; return visible;
    }
};
