#pragma once

#include <boost/json/object.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace circud {

    inline constexpr std::size_t kDefaultEventQueryLimit = 100;
    inline constexpr std::size_t kMaxEventQueryLimit = 200;

    enum class EventLevel {
        info,
        warning,
        error,
    };

    std::string_view to_string(EventLevel level) noexcept;
    std::optional<EventLevel> event_level_from_string(
        std::string_view value) noexcept;

    struct ManagementEvent {
        std::uint64_t id = 0;
        std::chrono::system_clock::time_point timestamp;
        EventLevel level = EventLevel::info;
        std::string category;
        std::optional<std::string> package_id;
        std::string action;
        std::string code;
        std::string message;
        boost::json::object details;
    };

    struct ManagementEventInput {
        EventLevel level = EventLevel::info;
        std::string category;
        std::optional<std::string> package_id;
        std::string action;
        std::string code;
        std::string message;
        boost::json::object details;
    };

    struct EventQuery {
        std::uint64_t after_id = 0;
        std::size_t limit = kDefaultEventQueryLimit;
        std::optional<EventLevel> level;
        std::optional<std::string> category;
        std::optional<std::string> package_id;
    };

    struct EventPage {
        std::vector<ManagementEvent> events;
        std::string journal_id;
        std::uint64_t oldest_id = 0;
        std::uint64_t latest_id = 0;
        std::uint64_t next_after_id = 0;
        std::uint64_t dropped_before_id = 0;
        bool gap = false;
        bool reset = false;
        bool has_more = false;
    };

    class EventJournal {
    public:
        EventJournal(bool enabled, std::size_t capacity);

        std::uint64_t append(ManagementEventInput input);
        EventPage list(const EventQuery& query) const;

        bool enabled() const noexcept { return enabled_; }
        std::size_t capacity() const noexcept { return capacity_; }
        const std::string& journal_id() const noexcept { return journal_id_; }

    private:
        bool enabled_;
        std::size_t capacity_;
        std::string journal_id_;
        mutable std::mutex mutex_;
        std::deque<ManagementEvent> events_;
        std::uint64_t next_id_ = 1;
    };

} // namespace circud
