#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

class SseParser {
    std::string buffer_, event_;
public:
    void clear() { buffer_.clear(); event_.clear(); }
    std::vector<std::string> push(std::string_view bytes) {
        buffer_.append(bytes);
        if (buffer_.size() + event_.size() > 1024 * 1024)
            throw std::runtime_error("SSE event exceeds 1 MiB");
        std::vector<std::string> events;
        std::size_t end;
        while ((end = buffer_.find('\n')) != std::string::npos) {
            auto line = buffer_.substr(0, end); buffer_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                if (!event_.empty()) { event_.pop_back(); events.push_back(std::move(event_)); event_.clear(); }
            } else if (line.starts_with("data:")) {
                auto data = line.substr(5);
                if (data.starts_with(' ')) data.erase(0, 1);
                event_ += data; event_ += '\n';
            }
        }
        return events;
    }
};
