#include "device_time.h"

#include <cstdlib>
#include <cstring>

namespace device_time {
namespace {

constexpr int kBeijingOffset = 8 * 60 * 60;

bool LeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int LeapYearsBefore(int year) {
    const int last = year - 1;
    return last / 4 - last / 100 + last / 400;
}

std::optional<time_t> Epoch(const std::tm& calendar) {
    const int year = calendar.tm_year + 1900;
    if (year < 2000 || year > 2099 || calendar.tm_mon < 0 || calendar.tm_mon > 11) return {};

    constexpr int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = LeapYear(year);
    const int month_days = days_in_month[calendar.tm_mon] + (leap && calendar.tm_mon == 1);
    if (calendar.tm_mday < 1 || calendar.tm_mday > month_days || calendar.tm_sec > 59) return {};

    int days = (year - 1970) * 365 + LeapYearsBefore(year) - LeapYearsBefore(1970);
    for (int i = 0; i < calendar.tm_mon; ++i) days += days_in_month[i] + (leap && i == 1);
    days += calendar.tm_mday - 1;
    return static_cast<time_t>(days) * 86400 + calendar.tm_hour * 3600 +
        calendar.tm_min * 60 + calendar.tm_sec;
}

bool Digit(char ch) { return ch >= '0' && ch <= '9'; }

std::optional<int> WireOffset(const char* suffix) {
    if (*suffix == '.') {
        ++suffix;
        if (!Digit(*suffix)) return {};
        while (Digit(*suffix)) ++suffix;
    }
    if (std::strcmp(suffix, "Z") == 0) return 0;
    if (std::strlen(suffix) != 6 || (suffix[0] != '+' && suffix[0] != '-') || suffix[3] != ':' ||
        !Digit(suffix[1]) || !Digit(suffix[2]) || !Digit(suffix[4]) || !Digit(suffix[5])) return {};
    const int hour = (suffix[1] - '0') * 10 + suffix[2] - '0';
    const int minute = (suffix[4] - '0') * 10 + suffix[5] - '0';
    if (hour > 23 || minute > 59) return {};
    return (suffix[0] == '-' ? -1 : 1) * (hour * 3600 + minute * 60);
}

}  // namespace

void InitializeBeijingTimezone() {
    setenv("TZ", "CST-8", 1);
    tzset();
}

std::optional<time_t> ParseHttpDate(const std::string& value) {
    std::tm date{};
    const auto* end = strptime(value.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &date);
    if (!end || *end || date.tm_year < 120) return {};
    return Epoch(date);
}

std::string FormatBeijing(const std::string& timestamp, bool seconds) {
    std::tm date{};
    const auto* suffix = strptime(timestamp.c_str(), "%Y-%m-%dT%H:%M:%S", &date);
    if (!suffix || suffix - timestamp.c_str() != 19) return timestamp;
    const auto offset = WireOffset(suffix);
    const auto epoch = Epoch(date);
    if (!offset || !epoch) return timestamp;

    const time_t display_time = *epoch - *offset + kBeijingOffset;
    std::tm display{};
    if (!gmtime_r(&display_time, &display)) return timestamp;
    char output[24];
    const char* format = seconds ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M";
    return std::strftime(output, sizeof(output), format, &display) ? output : timestamp;
}
}  // namespace device_time
