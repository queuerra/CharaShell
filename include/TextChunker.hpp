#pragma once
#include <string>
#include <string_view>
#include <vector>

// UTF-8 byte fragments may split a character. Only emit complete codepoints.
class TextChunker {
    std::string pending_;
    std::size_t minimum_, maximum_;
public:
    enum class Boundary { Clause, Sentence };
private:
    Boundary boundary_;
    std::vector<std::string> sentences() {
        std::vector<std::string> result;
        std::size_t pos = 0, end = 0;
        while (pos < pending_.size()) {
            const auto c = static_cast<unsigned char>(pending_[pos]);
            const auto width = c < 0x80 ? 1u : c < 0xe0 ? 2u : c < 0xf0 ? 3u : 4u;
            if (pos + width > pending_.size()) break;
            const auto cp = pending_.substr(pos, width);
            const bool space = cp == " " || cp == "\t" || cp == "\r" || cp == "\n";
            const bool closer = cp == "」" || cp == "』" || cp == "）" || cp == "】" ||
                cp == "〕" || cp == "〉" || cp == "》" || cp == "”" || cp == "’" ||
                cp == "\"" || cp == "'" || cp == ")" || cp == "]" || cp == "}";
            // Look ahead at an ASCII period so a fragmented decimal is not split.
            if (cp == "." && pos + width == pending_.size()) break;
            const bool period = cp == "." && (pending_[pos + width] == ' ' ||
                pending_[pos + width] == '\t' || pending_[pos + width] == '\r' ||
                pending_[pos + width] == '\n' || pending_[pos + width] == '"' ||
                pending_[pos + width] == '\'' || pending_[pos + width] == ')');
            const bool terminal = cp == "。" || cp == "！" || cp == "？" ||
                cp == "!" || cp == "?" || cp == "\n" || period;
            if (end && !terminal && !closer && !space) {
                result.push_back(pending_.substr(0, end));
                pending_.erase(0, end); pos -= end; end = 0;
                continue;
            }
            pos += width;
            if (terminal || (end && (closer || space || cp == "."))) end = pos;
        }
        // Keep a trailing boundary until the next fragment to include closing quotes.
        return result;
    }
public:
    explicit TextChunker(std::size_t minimum = 12, std::size_t maximum = 60,
            Boundary boundary = Boundary::Clause)
        : minimum_(minimum), maximum_(maximum), boundary_(boundary) {}
    void clear() { pending_.clear(); }
    std::vector<std::string> push(std::string_view fragment) {
        pending_.append(fragment);
        if (boundary_ == Boundary::Sentence) return sentences();
        std::vector<std::string> result;
        std::size_t pos = 0, count = 0;
        while (pos < pending_.size()) {
            auto c = static_cast<unsigned char>(pending_[pos]);
            auto width = c < 0x80 ? 1u : c < 0xe0 ? 2u : c < 0xf0 ? 3u : 4u;
            if (pos + width > pending_.size()) break;
            auto cp = pending_.substr(pos, width);
            pos += width; ++count;
            bool sentence = cp == "。" || cp == "！" || cp == "？" || cp == "!" || cp == "?" || cp == "\n";
            bool clause = cp == "、" || cp == "," || cp == ";" || cp == ":";
            if (sentence || (clause && count >= minimum_) || count >= maximum_) {
                result.push_back(pending_.substr(0, pos));
                pending_.erase(0, pos); pos = 0; count = 0;
            }
        }
        return result;
    }
    std::string flush() { auto result = std::move(pending_); pending_.clear(); return result; }
};
