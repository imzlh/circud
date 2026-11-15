#include <circud/event_journal.hpp>

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <atomic>
#include <sstream>
#include <stdexcept>

namespace circud {
    namespace {

        constexpr std::size_t kMaxEventFieldBytes = 256;
        constexpr std::size_t kMaxEventMessageBytes = 512;
        constexpr std::size_t kMaxEventDetailsBytes = 2048;

        std::string make_journal_id() {
            static std::atomic<std::uint64_t> sequence{ 0 };
            const auto wall = std::chrono::system_clock::now()
                .time_since_epoch().count();
            const auto monotonic = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            std::ostringstream output;
            output << std::hex << wall << '-' << monotonic << '-'
                << sequence.fetch_add(1, std::memory_order_relaxed);
            return output.str();
        }

        void bound_text(std::string& value, std::size_t maximum) {
            if (value.size() > maximum) value.resize(maximum);
        }

        bool matches(const ManagementEvent& event, const EventQuery& query) {
            if (query.level && event.level != *query.level) return false;
            if (query.category && event.category != *query.category) return false;
            if (query.package_id && event.package_id != query.package_id)
                return false;
            return true;
        }

    } // namespace

    std::string_view to_string(EventLevel level) noexcept {
        switch (level) {
        case EventLevel::info: return "info";
        case EventLevel::warning: return "warning";
        case EventLevel::error: return "error";
        }
        return "info";
    }

    std::optional<EventLevel> event_level_from_string(
        std::string_view value) noexcept {
        if (value == "info") return EventLevel::info;
        if (value == "warning") return EventLevel::warning;
        if (value == "error") return EventLevel::error;
        return std::nullopt;
    }

    EventJournal::EventJournal(bool enabled, std::size_t capacity)
        : enabled_(enabled), capacity_(std::max<std::size_t>(capacity, 1)),
        journal_id_(make_journal_id()) {
    }

    std::uint64_t EventJournal::append(ManagementEventInput input) {
        if (!enabled_) return 0;
        bound_text(input.category, kMaxEventFieldBytes);
        if (input.package_id)
            bound_text(*input.package_id, kMaxEventFieldBytes);
        bound_text(input.action, kMaxEventFieldBytes);
        bound_text(input.code, kMaxEventFieldBytes);
        bound_text(input.message, kMaxEventMessageBytes);
        if (boost::json::serialize(input.details).size() > kMaxEventDetailsBytes) {
            input.details.clear();
            input.details["truncated"] = true;
        }

        std::lock_guard lock(mutex_);
        const auto id = next_id_++;
        events_.push_back(ManagementEvent{
            id,
            std::chrono::system_clock::now(),
            input.level,
            std::move(input.category),
            std::move(input.package_id),
            std::move(input.action),
            std::move(input.code),
            std::move(input.message),
            std::move(input.details),
        });
        while (events_.size() > capacity_) events_.pop_front();
        return id;
    }

    EventPage EventJournal::list(const EventQuery& query) const {
        if (query.limit == 0 || query.limit > kMaxEventQueryLimit)
            throw std::invalid_argument("event query limit is out of range");

        EventPage page;
        page.journal_id = journal_id_;
        std::lock_guard lock(mutex_);
        if (events_.empty()) {
            page.reset = query.after_id != 0 && next_id_ == 1;
            return page;
        }

        page.oldest_id = events_.front().id;
        page.latest_id = events_.back().id;
        page.dropped_before_id = page.oldest_id - 1;
        page.reset = query.after_id > page.latest_id;
        const auto after_id = page.reset ? std::uint64_t{0} : query.after_id;
        page.gap = !page.reset && after_id < page.dropped_before_id;

        for (const auto& event : events_) {
            if (event.id <= after_id || !matches(event, query)) continue;
            if (page.events.size() == query.limit) {
                page.has_more = true;
                break;
            }
            page.events.push_back(event);
        }
        page.next_after_id = page.has_more
            ? page.events.back().id
            : page.latest_id;
        return page;
    }

} // namespace circud
