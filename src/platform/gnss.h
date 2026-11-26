#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace platform {

struct GnssFix {
    double latitude{0};
    double longitude{0};
    std::chrono::system_clock::time_point utc;
    std::string mode;
};

enum class GnssSentenceStatus { Fix, NoFix, Other, Invalid };

GnssSentenceStatus parse_gnss_sentence(std::string_view sentence, GnssFix& fix);
bool gnss_fix_is_fresh(const GnssFix& fix, std::chrono::system_clock::time_point now);

class GnssReceiver {
public:
    static std::unique_ptr<GnssReceiver> open(const std::string& path, std::string& error);
    ~GnssReceiver();
    GnssReceiver(const GnssReceiver&) = delete;
    GnssReceiver& operator=(const GnssReceiver&) = delete;

    bool poll(GnssFix& fix, bool& has_fix, std::string& error);

private:
    explicit GnssReceiver(int fd);
    int fd_{-1};
    std::string buffer_;
    GnssFix fix_;
    bool has_fix_{false};
    unsigned invalid_sentences_{0};
};

} // namespace platform
