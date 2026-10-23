#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <entityfy/executor.hpp>
#include <entityfy/task.hpp>

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using entityfy::executor;
using entityfy::inline_executor;
using entityfy::make_task;
using entityfy::task;
using entityfy::task_cancelled;
using entityfy::task_completer;
using entityfy::thread_pool_executor;

static_assert(std::is_base_of_v<std::runtime_error, task_cancelled>);
static_assert(std::is_move_constructible_v<task<int>>);
static_assert(!std::is_copy_constructible_v<task<int>>);
static_assert(!std::is_copy_assignable_v<task<int>>);
static_assert(std::is_nothrow_move_constructible_v<task<int>>);
static_assert(std::is_nothrow_move_assignable_v<task<int>>);
static_assert(std::is_move_constructible_v<task<void>>);
static_assert(!std::is_copy_constructible_v<task_completer<int>>);
static_assert(std::is_nothrow_move_constructible_v<task_completer<int>>);
static_assert(std::is_move_constructible_v<task_completer<void>>);
static_assert(std::is_base_of_v<executor, inline_executor>);
static_assert(std::is_base_of_v<executor, thread_pool_executor>);
static_assert(!std::is_copy_constructible_v<thread_pool_executor>);

static_assert(std::same_as<decltype(std::declval<task<int>&>().then([](int value) { return value; })), task<int>>);
static_assert(std::same_as<decltype(std::declval<task<int>&>().then([](int value) { return std::to_string(value); })), task<std::string>>);
static_assert(std::same_as<decltype(std::declval<task<int>&>().then([](int value) { (void)value; })), task<void>>);
static_assert(std::same_as<decltype(std::declval<task<void>&>().then([] { return 1; })), task<int>>);
static_assert(std::same_as<decltype(std::declval<task<int>&>().then([](int value) { (void)value; return make_task<double>().value; })), task<double>>);
static_assert(std::same_as<decltype(std::declval<task<void>&>().then([] { return make_task<void>().value; })), task<void>>);

// One-shot gate: tests synchronise with condition variables, never with sleeps.
class latch
{
public:
    void set()
    {
        {
            std::lock_guard lock{mutex_};
            open_ = true;
        }

        cv_.notify_all();
    }

    void wait()
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return open_; });
    }

    [[nodiscard]] bool is_open() const
    {
        std::lock_guard lock{mutex_};
        return open_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Counting gate: waits until at least `target` increments happened.
class counter
{
public:
    void increment()
    {
        {
            std::lock_guard lock{mutex_};
            ++value_;
        }

        cv_.notify_all();
    }

    void wait_for(std::size_t target)
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this, target] { return value_ >= target; });
    }

    [[nodiscard]] std::size_t value() const
    {
        std::lock_guard lock{mutex_};
        return value_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t value_ = 0;
};

// Serialised record of the order in which pool jobs ran.
class ordered_log
{
public:
    void append(int id)
    {
        std::lock_guard lock{mutex_};
        entries_.push_back(id);
    }

    [[nodiscard]] std::vector<int> entries() const
    {
        std::lock_guard lock{mutex_};
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<int> entries_;
};

} // namespace

TEST_CASE("task delivers one value and reports readiness", "[task]")
{
    auto [pending, completer] = make_task<int>();

    REQUIRE(pending.valid());
    REQUIRE_FALSE(pending.is_ready());
    REQUIRE_FALSE(pending.is_cancel_requested());
    REQUIRE_FALSE(completer.is_completed());

    completer.set_value(41);

    REQUIRE(completer.is_completed());
    REQUIRE(pending.is_ready());
    REQUIRE(pending.get() == 41);

    // get() consumes the handle, so the value cannot be delivered twice.
    REQUIRE_FALSE(pending.valid());
    REQUIRE_FALSE(pending.is_ready());
    REQUIRE_THROWS_AS(pending.get(), std::logic_error);
    REQUIRE_THROWS_AS(pending.on_complete([] {}), std::logic_error);

    // The completer completes exactly once.
    REQUIRE_THROWS_AS(completer.set_value(42), std::logic_error);
    REQUIRE_THROWS_AS(completer.set_cancelled(), std::logic_error);
    REQUIRE_THROWS_AS(completer.set_exception(std::make_exception_ptr(std::runtime_error{"late"})), std::logic_error);
}

TEST_CASE("a default constructed task is invalid", "[task]")
{
    task<int> empty;

    REQUIRE_FALSE(empty.valid());
    REQUIRE_FALSE(empty.is_ready());
    REQUIRE_FALSE(empty.is_cancel_requested());
    REQUIRE_THROWS_AS(empty.get(), std::logic_error);
    REQUIRE_THROWS_AS(empty.request_cancel(), std::logic_error);
    REQUIRE_THROWS_AS(empty.then([](int value) { return value; }), std::logic_error);
}

TEST_CASE("task_completer hands out its task once", "[task]")
{
    auto pair = make_task<int>();
    task_completer<int> completer = std::move(pair.completer);

    task<int> paired = completer.task();
    REQUIRE(paired.valid());
    REQUIRE_THROWS_AS(completer.task(), std::logic_error);

    completer.set_value(5);
    REQUIRE(paired.get() == 5);
}

TEST_CASE("task carries a move-only value", "[task]")
{
    auto [pending, completer] = make_task<std::unique_ptr<int>>();
    completer.set_value(std::make_unique<int>(7));

    std::unique_ptr<int> value = pending.get();
    REQUIRE(value != nullptr);
    REQUIRE(*value == 7);

    auto [other, other_completer] = make_task<std::unique_ptr<int>>();
    std::thread producer{[&other_completer] { other_completer.set_value(std::make_unique<int>(9)); }};

    std::unique_ptr<int> moved = other.get();
    producer.join();

    REQUIRE(moved != nullptr);
    REQUIRE(*moved == 9);
}

TEST_CASE("get blocks until another thread completes the task", "[task]")
{
    auto [pending, completer] = make_task<std::string>();
    latch started;

    std::thread producer{[&] {
        started.set();
        completer.set_value("from another thread");
    }};

    started.wait();
    REQUIRE(pending.get() == "from another thread");
    producer.join();
}

TEST_CASE("set_exception makes get rethrow the stored exception", "[task]")
{
    SECTION("inline completion")
    {
        auto [pending, completer] = make_task<int>();
        completer.set_exception(std::make_exception_ptr(std::runtime_error{"producer failed"}));

        REQUIRE(pending.is_ready());
        REQUIRE_THROWS_WITH(pending.get(), "producer failed");
    }

    SECTION("completion from another thread")
    {
        auto [pending, completer] = make_task<void>();
        std::thread producer{[&completer] {
            completer.set_exception(std::make_exception_ptr(std::logic_error{"off-thread failure"}));
        }};

        REQUIRE_THROWS_WITH(pending.get(), "off-thread failure");
        producer.join();
    }
}

TEST_CASE("cancellation surfaces through get and the cancel hook", "[task][cancel]")
{
    SECTION("set_cancelled")
    {
        auto [pending, completer] = make_task<int>();
        completer.set_cancelled();

        REQUIRE(pending.is_ready());
        REQUIRE(completer.is_completed());
        REQUIRE_THROWS_AS(pending.get(), task_cancelled);
    }

    SECTION("request_cancel runs the hook once and the hook cancels")
    {
        auto [pending, completer] = make_task<int>();
        std::atomic<int> hook_calls{0};

        // The hook cancels through the completer, so it only finishes if it runs
        // outside the state lock.
        completer.set_cancel_hook([&] {
            hook_calls.fetch_add(1);
            completer.set_cancelled();
        });

        REQUIRE_FALSE(pending.is_cancel_requested());

        pending.request_cancel();
        pending.request_cancel();

        REQUIRE(hook_calls.load() == 1);
        REQUIRE(pending.is_cancel_requested());
        REQUIRE(completer.is_cancel_requested());
        REQUIRE(pending.is_ready());
        REQUIRE_THROWS_AS(pending.get(), task_cancelled);
    }

    SECTION("a task completed before the request never runs the hook")
    {
        auto [pending, completer] = make_task<int>();
        std::atomic<int> hook_calls{0};
        completer.set_cancel_hook([&] { hook_calls.fetch_add(1); });

        completer.set_value(4);
        pending.request_cancel();

        REQUIRE(hook_calls.load() == 0);
        REQUIRE(pending.is_cancel_requested());
        REQUIRE(pending.get() == 4);
    }

    SECTION("a hook installed after the request runs immediately")
    {
        auto [pending, completer] = make_task<void>();
        pending.request_cancel();
        REQUIRE(pending.is_cancel_requested());

        std::atomic<int> hook_calls{0};
        completer.set_cancel_hook([&] { hook_calls.fetch_add(1); });

        REQUIRE(hook_calls.load() == 1);

        pending.request_cancel();
        REQUIRE(hook_calls.load() == 1);
    }

    SECTION("concurrent requests still call the hook once")
    {
        auto [pending, completer] = make_task<void>();
        std::atomic<int> hook_calls{0};
        completer.set_cancel_hook([&] { hook_calls.fetch_add(1); });

        latch start;
        auto request = [&] {
            start.wait();
            pending.request_cancel();
        };

        std::thread first{request};
        std::thread second{request};
        start.set();
        first.join();
        second.join();

        REQUIRE(hook_calls.load() == 1);
        REQUIRE(pending.is_cancel_requested());
    }

    SECTION("a second cancel hook is rejected")
    {
        auto [pending, completer] = make_task<int>();
        completer.set_cancel_hook([] {});
        REQUIRE_THROWS_AS(completer.set_cancel_hook([] {}), std::logic_error);
    }
}

TEST_CASE("on_complete runs the single completion callback exactly once", "[task]")
{
    SECTION("deferred until the task completes")
    {
        auto [pending, completer] = make_task<int>();
        std::atomic<int> calls{0};
        std::thread::id callback_thread;
        latch ran;

        pending.on_complete([&] {
            callback_thread = std::this_thread::get_id();
            calls.fetch_add(1);
            ran.set();
        });

        REQUIRE_FALSE(ran.is_open());

        std::thread producer{[&completer] { completer.set_value(1); }};
        const std::thread::id producer_id = producer.get_id();

        ran.wait();
        producer.join();

        REQUIRE(calls.load() == 1);
        REQUIRE(callback_thread == producer_id);
        REQUIRE(pending.is_ready());
        REQUIRE_THROWS_AS(pending.on_complete([] {}), std::logic_error);
        REQUIRE(pending.get() == 1);
    }

    SECTION("immediately when the task is already ready")
    {
        auto [pending, completer] = make_task<int>();
        completer.set_value(2);

        const std::thread::id caller_id = std::this_thread::get_id();
        std::atomic<int> calls{0};
        std::thread::id callback_thread;

        pending.on_complete([&] {
            callback_thread = std::this_thread::get_id();
            calls.fetch_add(1);
        });

        REQUIRE(calls.load() == 1);
        REQUIRE(callback_thread == caller_id);
        REQUIRE_THROWS_AS(pending.on_complete([] {}), std::logic_error);
        REQUIRE(pending.get() == 2);
    }

    SECTION("outside the state lock")
    {
        auto [pending, completer] = make_task<int>();
        std::atomic<int> seen{0};

        // Consuming the task from inside the callback would deadlock if the
        // callback ran while the state mutex was held.
        pending.on_complete([&] { seen.store(pending.get()); });

        completer.set_value(9);
        REQUIRE(seen.load() == 9);
    }
}

TEST_CASE("then chains values and flattens nested tasks", "[task][then]")
{
    SECTION("two levels of values")
    {
        auto [root, root_completer] = make_task<int>();

        task<int> chained = root.then([](int value) { return value * 2; }).then([](int value) { return value + 1; });
        REQUIRE_FALSE(root.valid());

        root_completer.set_value(5);
        REQUIRE(chained.get() == 11);
    }

    SECTION("a ready task runs the continuation immediately")
    {
        auto [root, root_completer] = make_task<int>();
        root_completer.set_value(3);

        bool ran = false;
        task<int> chained = root.then([&](int value) {
            ran = true;
            return value;
        });

        REQUIRE(ran);
        REQUIRE(chained.get() == 3);
    }

    SECTION("an exception thrown by the callback propagates")
    {
        auto [root, root_completer] = make_task<int>();
        task<int> chained = root.then([](int) -> int { throw std::runtime_error{"callback failed"}; });

        root_completer.set_value(1);
        REQUIRE_THROWS_WITH(chained.get(), "callback failed");
    }

    SECTION("the source exception is forwarded")
    {
        auto [root, root_completer] = make_task<int>();
        task<int> chained = root.then([](int value) { return value; });

        root_completer.set_exception(std::make_exception_ptr(std::runtime_error{"source failed"}));
        REQUIRE_THROWS_WITH(chained.get(), "source failed");
    }

    SECTION("the source cancellation is forwarded")
    {
        auto [root, root_completer] = make_task<int>();
        task<int> chained = root.then([](int value) { return value; });

        root_completer.set_cancelled();
        REQUIRE_THROWS_AS(chained.get(), task_cancelled);
    }

    SECTION("a callback returning a task is flattened")
    {
        auto [root, root_completer] = make_task<int>();
        auto [inner, inner_completer] = make_task<int>();

        task<int> flattened = root.then([&inner](int value) {
            REQUIRE(value == 2);
            return std::move(inner);
        });

        std::thread producer{[&] {
            root_completer.set_value(2);
            inner_completer.set_value(99);
        }};

        REQUIRE(flattened.get() == 99);
        producer.join();
    }

    SECTION("a flattened task resolves when the inner task completes later")
    {
        auto [root, root_completer] = make_task<int>();
        auto [inner, inner_completer] = make_task<int>();
        latch continuation_ran;

        task<int> flattened = root.then([&](int value) {
            continuation_ran.set();
            return std::move(inner);
        });

        root_completer.set_value(4);
        continuation_ran.wait();
        REQUIRE_FALSE(flattened.is_ready());

        std::thread producer{[&inner_completer] { inner_completer.set_value(64); }};
        REQUIRE(flattened.get() == 64);
        producer.join();
    }

    SECTION("a callback returning void produces task<void>")
    {
        auto [root, root_completer] = make_task<int>();
        std::atomic<int> seen{0};

        task<void> chained = root.then([&](int value) { seen.store(value); });

        root_completer.set_value(4);
        REQUIRE(seen.load() == 4);
        REQUIRE_NOTHROW(chained.get());
    }
}

TEST_CASE("task<void> completes, chains and forwards failures", "[task]")
{
    SECTION("set_value and get")
    {
        auto [pending, completer] = make_task<void>();
        REQUIRE_FALSE(pending.is_ready());

        completer.set_value();

        REQUIRE(pending.is_ready());
        REQUIRE_NOTHROW(pending.get());
        REQUIRE_FALSE(pending.valid());
    }

    SECTION("then receives no argument and produces a value")
    {
        auto [pending, completer] = make_task<void>();
        task<int> chained = pending.then([] { return 7; });

        completer.set_value();
        REQUIRE(chained.get() == 7);
    }

    SECTION("a value-less continuation chains into a value")
    {
        auto [pending, completer] = make_task<int>();
        task<int> chained = pending.then([](int value) { REQUIRE(value == 2); }).then([] { return 5; });

        completer.set_value(2);
        REQUIRE(chained.get() == 5);
    }

    SECTION("a nested task<void> is flattened")
    {
        auto [outer, outer_completer] = make_task<void>();
        auto [inner, inner_completer] = make_task<void>();

        task<void> chained = outer.then([&inner] { return std::move(inner); });

        outer_completer.set_value();

        std::thread producer{[&inner_completer] { inner_completer.set_value(); }};
        REQUIRE_NOTHROW(chained.get());
        producer.join();
    }

    SECTION("exceptions propagate through then")
    {
        auto [pending, completer] = make_task<void>();
        task<int> chained = pending.then([]() -> int { throw std::runtime_error{"void callback failed"}; });

        completer.set_value();
        REQUIRE_THROWS_WITH(chained.get(), "void callback failed");
    }

    SECTION("cancellation propagates")
    {
        auto [pending, completer] = make_task<void>();
        completer.set_cancelled();

        REQUIRE_THROWS_AS(pending.get(), task_cancelled);
    }
}

TEST_CASE("destroying a pending task or completer is safe", "[task]")
{
    SECTION("both ends destroyed while pending")
    {
        std::atomic<int> calls{0};
        {
            auto [pending, completer] = make_task<int>();
            pending.on_complete([&] { calls.fetch_add(1); });
        }

        REQUIRE(calls.load() == 0);
    }

    SECTION("a pending task survives its completer")
    {
        std::atomic<int> calls{0};

        auto pair = make_task<int>();
        task<int> pending = std::move(pair.value);
        pending.on_complete([&] { calls.fetch_add(1); });

        {
            task_completer<int> completer = std::move(pair.completer);
            REQUIRE_FALSE(completer.is_completed());
        }

        REQUIRE_FALSE(pending.is_ready());
        REQUIRE(calls.load() == 0);
    }

    SECTION("a completer outlives its task")
    {
        task_completer<int> completer = [] {
            auto pair = make_task<int>();
            return std::move(pair.completer);
        }();

        REQUIRE_FALSE(completer.is_completed());
        completer.set_value(3);
        REQUIRE(completer.is_completed());
        REQUIRE_THROWS_AS(completer.set_value(4), std::logic_error);
    }
}

TEST_CASE("inline_executor runs jobs synchronously", "[executor]")
{
    inline_executor direct;
    REQUIRE(direct.is_inline());

    bool ran = false;
    std::thread::id ran_on;
    direct.submit([&] {
        ran = true;
        ran_on = std::this_thread::get_id();
    });

    REQUIRE(ran);
    REQUIRE(ran_on == std::this_thread::get_id());

    executor& base = direct;
    REQUIRE(base.is_inline());
    int value = 0;
    base.submit([&] { value = 5; });
    REQUIRE(value == 5);
}

TEST_CASE("thread_pool_executor runs every queued job on its workers", "[executor][pool]")
{
    thread_pool_executor pool{2, 4};
    REQUIRE(pool.pending() == std::size_t{0});

    constexpr std::size_t job_count = 8;
    counter done;
    std::mutex ids_mutex;
    std::set<std::thread::id> ids;

    for(std::size_t index = 0; index < job_count; ++index)
    {
        pool.submit([&] {
            {
                std::lock_guard lock{ids_mutex};
                ids.insert(std::this_thread::get_id());
            }

            done.increment();
        });
    }

    done.wait_for(job_count);

    REQUIRE(done.value() == job_count);
    REQUIRE(pool.pending() == std::size_t{0});

    std::lock_guard lock{ids_mutex};
    REQUIRE(ids.size() <= 2);
    REQUIRE_FALSE(ids.empty());
}

TEST_CASE("thread_pool_executor survives a throwing job", "[executor][pool]")
{
    thread_pool_executor pool{2, 4};

    latch failed;
    latch afterwards;
    counter done;

    pool.submit([&] {
        failed.set();
        throw std::runtime_error{"job failed"};
    });

    failed.wait();
    pool.submit([&] {
        done.increment();
        afterwards.set();
    });

    afterwards.wait();
    REQUIRE(done.value() == 1);

    pool.shutdown();
    REQUIRE(pool.pending() == std::size_t{0});
}

TEST_CASE("thread_pool_executor runs two jobs concurrently", "[executor][pool]")
{
    thread_pool_executor pool{2, 4};
    counter started;

    // Each job waits until both have started, which only completes with two
    // live workers.
    auto rendezvous = [&] {
        started.increment();
        started.wait_for(2);
    };

    pool.submit(rendezvous);
    pool.submit(rendezvous);

    started.wait_for(2);
    REQUIRE(started.value() == 2);

    pool.shutdown();
}

TEST_CASE("thread_pool_executor blocks submit while the bounded queue is full", "[executor][pool]")
{
    thread_pool_executor pool{1, 1};

    latch active;
    latch release;
    ordered_log order;

    pool.submit([&] {
        active.set();
        release.wait();
        order.append(0);
    });
    active.wait();

    // One busy worker, one queue slot: the queue is full now.
    pool.submit([&] { order.append(1); });
    REQUIRE(pool.pending() == std::size_t{1});

    latch submit_entered;
    latch submit_returned;
    std::thread extra{[&] {
        submit_entered.set();
        pool.submit([&] { order.append(2); });
        submit_returned.set();
    }};

    submit_entered.wait();

    // The third job cannot be accepted while the single slot is taken, so the
    // submitting thread must still be blocked inside submit.
    REQUIRE_FALSE(submit_returned.is_open());

    release.set();
    submit_returned.wait();
    extra.join();

    pool.shutdown();

    REQUIRE(order.entries() == std::vector<int>{0, 1, 2});
}

TEST_CASE("thread_pool_executor shutdown drains queued jobs and rejects later work", "[executor][pool]")
{
    counter done;

    {
        thread_pool_executor pool{2, 8};
        constexpr int job_count = 24;

        for(int index = 0; index < job_count; ++index)
        {
            pool.submit([&done] { done.increment(); });
        }

        // shutdown returns only after every queued and running job finished.
        pool.shutdown();

        REQUIRE(done.value() == static_cast<std::size_t>(job_count));
        REQUIRE(pool.pending() == std::size_t{0});
        REQUIRE_THROWS_AS(pool.submit([&done] { done.increment(); }), std::logic_error);

        pool.shutdown();
        REQUIRE(done.value() == static_cast<std::size_t>(job_count));
    }

    REQUIRE(done.value() == static_cast<std::size_t>(24));
}

TEST_CASE("nested awaits complete on a two-thread pool", "[executor][pool][task]")
{
    thread_pool_executor pool{2, 8};

    SECTION("the waiting job submits the completing job")
    {
        auto [inner, inner_completer] = make_task<int>();
        auto [outer, outer_completer] = make_task<int>();

        pool.submit([&] {
            pool.submit([&] { inner_completer.set_value(7); });
            outer_completer.set_value(inner.get() + 1);
        });

        REQUIRE(outer.get() == 8);
        pool.shutdown();
    }

    SECTION("the waiting job blocks while another job completes the task")
    {
        auto [late, late_completer] = make_task<int>();
        auto [result, result_completer] = make_task<int>();
        latch waiting;

        pool.submit([&] {
            waiting.set();
            result_completer.set_value(late.get() * 2);
        });

        waiting.wait();
        pool.submit([&] { late_completer.set_value(21); });

        REQUIRE(result.get() == 42);
        pool.shutdown();
    }
}

TEST_CASE("then runs the continuation on the completing thread", "[task][then]")
{
    auto [pending, completer] = make_task<int>();

    std::mutex mutex;
    std::condition_variable cv;
    bool ran = false;
    std::thread::id callback_thread;

    task<int> chained = pending.then([&](int value) {
        {
            std::lock_guard lock{mutex};
            ran = true;
            callback_thread = std::this_thread::get_id();
        }

        cv.notify_all();
        return value + 1;
    });

    std::thread producer{[&completer] { completer.set_value(1); }};
    const std::thread::id producer_id = producer.get_id();

    {
        std::unique_lock lock{mutex};
        cv.wait(lock, [&] { return ran; });
    }

    producer.join();

    REQUIRE(callback_thread == producer_id);
    REQUIRE(callback_thread != std::this_thread::get_id());
    REQUIRE(chained.get() == 2);
}
