#include "test.hpp"

#include <circud/event_journal.hpp>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

namespace {

circud::ManagementEventInput event(std::string category,
                                   circud::EventLevel level,
                                   std::string package_id = {}) {
    circud::ManagementEventInput value;
    value.level = level;
    value.category = std::move(category);
    if (!package_id.empty()) value.package_id = std::move(package_id);
    value.action = "test";
    value.code = "test_event";
    value.message = "Test event";
    return value;
}

} // namespace

CIRCUD_TEST(event_journal_evicts_old_entries_and_reports_gap) {
    circud::EventJournal journal(true, 3);
    for (int index = 0; index < 5; ++index)
        (void)journal.append(event("test", circud::EventLevel::info));

    circud::EventQuery query;
    query.after_id = 1;
    query.limit = 10;
    const auto page = journal.list(query);
    circud::test::require(page.events.size() == 3);
    circud::test::require(!page.journal_id.empty());
    circud::test::require(page.events.front().id == 3);
    circud::test::require(page.events.back().id == 5);
    circud::test::require(page.oldest_id == 3 && page.latest_id == 5);
    circud::test::require(page.dropped_before_id == 2 && page.gap);
    circud::test::require(page.next_after_id == 5 && !page.has_more);
}

CIRCUD_TEST(event_journal_filters_paginates_and_resets_future_cursor) {
    circud::EventJournal journal(true, 10);
    (void)journal.append(event("package", circud::EventLevel::info, "one"));
    (void)journal.append(event("rpc", circud::EventLevel::error, "one"));
    (void)journal.append(event("rpc", circud::EventLevel::error, "two"));
    (void)journal.append(event("rpc", circud::EventLevel::error, "one"));

    circud::EventQuery query;
    query.limit = 1;
    query.level = circud::EventLevel::error;
    query.category = "rpc";
    query.package_id = "one";
    const auto first = journal.list(query);
    circud::test::require(first.events.size() == 1 && first.events[0].id == 2);
    circud::test::require(first.has_more && first.next_after_id == 2);
    query.after_id = first.next_after_id;
    const auto second = journal.list(query);
    circud::test::require(second.events.size() == 1 && second.events[0].id == 4);
    circud::test::require(!second.has_more && second.next_after_id == 4);

    query.after_id = 999;
    query.limit = 10;
    const auto reset = journal.list(query);
    circud::test::require(reset.reset && !reset.gap);
    circud::test::require(reset.events.size() == 2);
}

CIRCUD_TEST(event_journal_supports_concurrent_append_and_query) {
    constexpr int writers = 4;
    constexpr int events_per_writer = 250;
    circud::EventJournal journal(true, writers * events_per_writer);
    std::atomic<bool> start{ false };
    std::atomic<bool> done{ false };
    std::atomic<bool> ordered{ true };
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) {}
        while (!done.load(std::memory_order_acquire)) {
            circud::EventQuery query;
            query.limit = 200;
            const auto page = journal.list(query);
            for (std::size_t index = 1; index < page.events.size(); ++index)
                if (page.events[index - 1].id >= page.events[index].id)
                    ordered.store(false, std::memory_order_release);
        }
    });
    std::vector<std::thread> threads;
    for (int writer = 0; writer < writers; ++writer) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {}
            for (int index = 0; index < events_per_writer; ++index)
                (void)journal.append(event("concurrent",
                    circud::EventLevel::info));
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    done.store(true, std::memory_order_release);
    reader.join();
    circud::test::require(ordered.load(std::memory_order_acquire),
                          "concurrent event query was not ordered");

    std::set<std::uint64_t> ids;
    std::uint64_t cursor = 0;
    do {
        circud::EventQuery query;
        query.after_id = cursor;
        query.limit = 200;
        const auto page = journal.list(query);
        for (const auto& item : page.events) ids.insert(item.id);
        cursor = page.next_after_id;
        if (!page.has_more) break;
    } while (true);
    circud::test::require(ids.size() == writers * events_per_writer);
    circud::test::require(*ids.begin() == 1 &&
                          *ids.rbegin() == writers * events_per_writer);
}

CIRCUD_TEST(disabled_event_journal_retains_nothing) {
    circud::EventJournal journal(false, 32);
    circud::test::require(
        journal.append(event("test", circud::EventLevel::info)) == 0);
    circud::EventQuery query;
    query.limit = 10;
    const auto page = journal.list(query);
    circud::test::require(page.events.empty() && page.latest_id == 0);
    circud::test::require(!page.journal_id.empty());
}

CIRCUD_TEST(event_journal_instances_have_distinct_generation_ids) {
    circud::EventJournal first(true, 32);
    circud::EventJournal second(true, 32);
    circud::EventQuery query;
    circud::test::require(first.list(query).journal_id !=
                          second.list(query).journal_id);
}
