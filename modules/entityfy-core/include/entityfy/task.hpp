#pragma once

#include <atomic>
#include <condition_variable>
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// Single-value asynchronous result: a `task<T>` observes one value produced by
// exactly one `task_completer<T>`. Both ends share one state; whichever end dies
// first leaves the other intact. Continuations and completion callbacks run on
// the thread that completes the task, never on a hidden executor.
//
// Lifetime rule: that completing thread is the backend reactor or a pool worker.
// Releasing the last owner of the client, context or executor from a
// continuation or an on_complete callback would destroy it on a thread it has to
// join; the affected destructors diagnose that mistake and abort. Move the value
// out of the callback and release the owner on the owning thread instead.

namespace entityfy
{

// Thrown by task::get() when the producer cancelled instead of producing a value.
class task_cancelled : public std::runtime_error
{
public:
    explicit task_cancelled(std::string message = "entityfy::task was cancelled before it produced a value.")
        : std::runtime_error{std::move(message)}
    {
    }
};

template <typename T>
class task;

template <typename T>
class task_completer;

// Both ends of one asynchronously computed value: `value` observes it and
// `completer` fulfils it exactly once.
template <typename T>
struct task_pair
{
    task<T> value;
    task_completer<T> completer;
};

// Creates a pending task together with its completer.
template <typename T>
[[nodiscard]] task_pair<T> make_task();

namespace detail
{

// Storage for the produced value. The void specialisation only tracks that a
// value was produced.
template <typename T>
struct task_value
{
    std::optional<T> storage;

    void set(T item)
    {
        storage.emplace(std::move(item));
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return storage.has_value();
    }

    T take()
    {
        T item = std::move(*storage);
        storage.reset();
        return item;
    }

    void drop() noexcept
    {
        storage.reset();
    }
};

template <>
struct task_value<void>
{
    bool present = false;

    void set() noexcept
    {
        present = true;
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return present;
    }

    void take() noexcept
    {
        present = false;
    }

    void drop() noexcept
    {
        present = false;
    }
};

// State shared by a task and its completer. Everything except the atomic flags
// is guarded by `mutex`.
template <typename T>
struct task_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<bool> cancel_requested{false};

    bool cancelled = false;
    bool task_released = false;
    std::exception_ptr error;
    task_value<T> value;
    std::function<void()> completion;
    bool completion_registered = false;
    std::function<void()> cancel_hook;
    bool cancel_hook_installed = false;
    bool cancel_hook_invoked = false;
};

// Single doorway into the private state of both ends.
struct task_access
{
    template <typename T>
    [[nodiscard]] static task<T> make(std::shared_ptr<task_state<T>> state);

    template <typename T>
    [[nodiscard]] static task_completer<T> make_completer(std::shared_ptr<task_state<T>> state);

    template <typename T>
    [[nodiscard]] static std::shared_ptr<task_state<T>> take_state(task<T>& source);
};

template <typename T>
struct task_probe : std::false_type
{
};

template <typename T>
struct task_probe<task<T>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_task_v = task_probe<T>::value;

template <typename T>
struct unbox_task
{
    using type = T;
};

template <typename T>
struct unbox_task<task<T>>
{
    using type = T;
};

// Result of `then`: the callback returns either the value itself or a task of it.
template <typename T, typename F>
struct then_result
{
    using type = std::decay_t<std::invoke_result_t<F&, T>>;
};

template <typename F>
struct then_result<void, F>
{
    using type = std::decay_t<std::invoke_result_t<F&>>;
};

template <typename T, typename F>
using then_result_t = typename unbox_task<typename then_result<T, std::decay_t<F>>::type>::type;

// Hooks used by the classes below; defined after the classes so the shared
// state stays an implementation detail.
template <typename T>
void install_completion(std::shared_ptr<task_state<T>> const& state, std::function<void()> callback);

template <typename T>
void complete_value(std::shared_ptr<task_state<T>> const& state, T value) requires (!std::is_void_v<T>);

template <typename T>
void complete_value(std::shared_ptr<task_state<T>> const& state) requires (std::is_void_v<T>);

template <typename T>
void complete_exception(std::shared_ptr<task_state<T>> const& state, std::exception_ptr error);

template <typename T>
void complete_cancelled(std::shared_ptr<task_state<T>> const& state);

template <typename T, typename F>
[[nodiscard]] task<then_result_t<T, F>> then_impl(std::shared_ptr<task_state<T>> source, F&& continuation);

} // namespace detail

// Handle to one asynchronously computed value. Move-only; the value is
// delivered once, so get() and then() consume the handle. A producer may
// complete the task from another thread while this handle waits in get(); the
// handle object itself must not be consumed from two threads at once.
template <typename T>
class task
{
public:
    task() noexcept = default;

    task(task&&) noexcept = default;
    task& operator=(task&&) noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    ~task() = default;

    // True while this handle still refers to a pending or completed state.
    [[nodiscard]] bool valid() const noexcept
    {
        return state_ != nullptr;
    }

    // True once a value, an exception or a cancellation was published.
    [[nodiscard]] bool is_ready() const noexcept
    {
        return state_ != nullptr && state_->ready.load(std::memory_order_acquire);
    }

    // True once a cancellation was requested on either end.
    [[nodiscard]] bool is_cancel_requested() const noexcept
    {
        return state_ != nullptr && state_->cancel_requested.load(std::memory_order_acquire);
    }

    // Marks the request and invokes the completer's cancel hook at most once,
    // outside the state lock. Idempotent; a task that completed before the
    // request never calls the hook.
    void request_cancel()
    {
        auto state = require_state();

        std::function<void()> hook;
        {
            std::lock_guard lock{state->mutex};

            if(state->cancel_requested.load(std::memory_order_relaxed))
            {
                return;
            }

            state->cancel_requested.store(true, std::memory_order_release);

            if(state->ready.load(std::memory_order_acquire) || state->cancel_hook_invoked)
            {
                return;
            }

            if(state->cancel_hook)
            {
                hook = std::move(state->cancel_hook);
                state->cancel_hook = nullptr;
                state->cancel_hook_invoked = true;
            }
        }

        if(hook)
        {
            hook();
        }
    }

    // Blocks until the value arrives, then returns it by move. Rethrows a stored
    // exception and throws task_cancelled after set_cancelled(). Consumes the
    // handle: a second call on the same object is a logic_error.
    T get()
    {
        auto state = consume();

        std::unique_lock lock{state->mutex};
        state->cv.wait(lock, [&state] { return state->ready.load(std::memory_order_acquire); });

        if(state->error)
        {
            std::rethrow_exception(state->error);
        }

        if(state->cancelled)
        {
            throw task_cancelled{};
        }

        return state->value.take();
    }

    // Registers the single completion callback. It runs exactly once on the
    // thread that completes the task, outside the state lock; when the task is
    // already ready it runs immediately on this thread. A second registration
    // throws logic_error. The callback must not release the last owner of the
    // producing client, context or executor; see the lifetime rule above.
    void on_complete(std::function<void()> callback)
    {
        detail::install_completion(require_state(), std::move(callback));
    }

    // Consumes the handle and runs `continuation` with the value once it
    // arrives. The continuation returns `U` or `task<U>`; both produce a
    // `task<U>` (a nested task is flattened). Events that are not a value
    // (exceptions, cancellation) are forwarded to the returned task. The
    // continuation runs on the completing thread, so the lifetime rule above
    // applies to it as well.
    template <typename F>
    auto then(F&& continuation)
    {
        return detail::then_impl<T>(consume(), std::forward<F>(continuation));
    }

private:
    explicit task(std::shared_ptr<detail::task_state<T>> state) noexcept
        : state_{std::move(state)}
    {
    }

    [[nodiscard]] std::shared_ptr<detail::task_state<T>> require_state() const
    {
        if(state_ == nullptr)
        {
            throw std::logic_error{"entityfy::task is not valid: it was consumed or never bound."};
        }

        return state_;
    }

    [[nodiscard]] std::shared_ptr<detail::task_state<T>> consume()
    {
        if(state_ == nullptr)
        {
            throw std::logic_error{"entityfy::task is not valid: it was consumed or never bound."};
        }

        return std::move(state_);
    }

    std::shared_ptr<detail::task_state<T>> state_;

    friend struct detail::task_access;
};

// Producing end of one task. Move-only; the shared state is completed exactly
// once, and a second completion throws logic_error even when it arrives from
// another thread.
template <typename T>
class task_completer
{
public:
    task_completer(task_completer&&) noexcept = default;
    task_completer& operator=(task_completer&&) noexcept = default;
    task_completer(const task_completer&) = delete;
    task_completer& operator=(const task_completer&) = delete;

    ~task_completer() = default;

    // The paired task. Handed out once; a second call throws logic_error.
    [[nodiscard]] entityfy::task<T> task()
    {
        auto state = require_state();
        {
            std::lock_guard lock{state->mutex};

            if(state->task_released)
            {
                throw std::logic_error{"entityfy::task_completer already handed out its task."};
            }

            state->task_released = true;
        }

        return detail::task_access::make<T>(std::move(state));
    }

    // Publishes the value of a non-void task. Throws logic_error when the
    // completer already completed its task.
    template <typename U>
        requires (!std::is_void_v<T>) && std::convertible_to<U, T>
    void set_value(U value)
    {
        detail::complete_value(require_state(), static_cast<T>(std::move(value)));
    }

    // Publishes completion of a void task.
    void set_value()
        requires (std::is_void_v<T>)
    {
        detail::complete_value(require_state());
    }

    // Publishes an exception; task::get() rethrows it.
    void set_exception(std::exception_ptr error)
    {
        if(!error)
        {
            throw std::invalid_argument{"entityfy::task_completer::set_exception requires a live exception."};
        }

        detail::complete_exception(require_state(), std::move(error));
    }

    // Publishes a cancellation; task::get() throws task_cancelled.
    void set_cancelled()
    {
        detail::complete_cancelled(require_state());
    }

    // Installs the single cancellation hook, called at most once and outside the
    // state lock. When a cancellation was already requested for a still pending
    // task, the hook runs immediately on this thread.
    void set_cancel_hook(std::function<void()> hook)
    {
        auto state = require_state();

        std::function<void()> run_now;
        {
            std::lock_guard lock{state->mutex};

            if(state->cancel_hook_installed)
            {
                throw std::logic_error{"entityfy::task_completer already installed a cancel hook."};
            }

            state->cancel_hook_installed = true;
            state->cancel_hook = std::move(hook);

            const bool requested = state->cancel_requested.load(std::memory_order_acquire);
            const bool completed = state->ready.load(std::memory_order_acquire);
            if(requested && !completed && !state->cancel_hook_invoked)
            {
                run_now = std::move(state->cancel_hook);
                state->cancel_hook = nullptr;
                state->cancel_hook_invoked = true;
            }
        }

        if(run_now)
        {
            run_now();
        }
    }

    [[nodiscard]] bool is_cancel_requested() const noexcept
    {
        return state_ != nullptr && state_->cancel_requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_completed() const noexcept
    {
        return state_ != nullptr && state_->ready.load(std::memory_order_acquire);
    }

private:
    explicit task_completer(std::shared_ptr<detail::task_state<T>> state) noexcept
        : state_{std::move(state)}
    {
    }

    [[nodiscard]] std::shared_ptr<detail::task_state<T>> require_state() const
    {
        if(state_ == nullptr)
        {
            throw std::logic_error{"entityfy::task_completer is not valid: it was moved from."};
        }

        return state_;
    }

    std::shared_ptr<detail::task_state<T>> state_;

    friend struct detail::task_access;
};

namespace detail
{

template <typename T>
task<T> task_access::make(std::shared_ptr<task_state<T>> state)
{
    return task<T>{std::move(state)};
}

template <typename T>
task_completer<T> task_access::make_completer(std::shared_ptr<task_state<T>> state)
{
    return task_completer<T>{std::move(state)};
}

template <typename T>
std::shared_ptr<task_state<T>> task_access::take_state(task<T>& source)
{
    return source.consume();
}

template <typename T>
void install_completion(std::shared_ptr<task_state<T>> const& state, std::function<void()> callback)
{
    bool run_now = false;
    {
        std::lock_guard lock{state->mutex};

        if(state->completion_registered)
        {
            throw std::logic_error{"entityfy::task already has a completion callback or continuation."};
        }

        state->completion_registered = true;

        if(state->ready.load(std::memory_order_acquire))
        {
            run_now = true;
        }
        else
        {
            state->completion = std::move(callback);
        }
    }

    if(run_now)
    {
        callback();
    }
}

// Publishes readiness and takes the completion callback so it can run unlocked.
// The caller holds `state.mutex`.
template <typename T>
std::function<void()> publish_ready(task_state<T>& state)
{
    state.ready.store(true, std::memory_order_release);

    auto callback = std::move(state.completion);
    state.completion = nullptr;
    return callback;
}

template <typename T>
void complete_value(std::shared_ptr<task_state<T>> const& state, T value)
    requires (!std::is_void_v<T>)
{
    std::function<void()> callback;
    {
        std::lock_guard lock{state->mutex};

        if(state->ready.load(std::memory_order_relaxed))
        {
            throw std::logic_error{"entityfy::task was already completed."};
        }

        state->value.set(std::move(value));
        callback = publish_ready(*state);
    }

    state->cv.notify_all();

    if(callback)
    {
        callback();
    }
}

template <typename T>
void complete_value(std::shared_ptr<task_state<T>> const& state)
    requires (std::is_void_v<T>)
{
    std::function<void()> callback;
    {
        std::lock_guard lock{state->mutex};

        if(state->ready.load(std::memory_order_relaxed))
        {
            throw std::logic_error{"entityfy::task was already completed."};
        }

        state->value.set();
        callback = publish_ready(*state);
    }

    state->cv.notify_all();

    if(callback)
    {
        callback();
    }
}

template <typename T>
void complete_exception(std::shared_ptr<task_state<T>> const& state, std::exception_ptr error)
{
    std::function<void()> callback;
    {
        std::lock_guard lock{state->mutex};

        if(state->ready.load(std::memory_order_relaxed))
        {
            throw std::logic_error{"entityfy::task was already completed."};
        }

        state->error = std::move(error);
        callback = publish_ready(*state);
    }

    state->cv.notify_all();

    if(callback)
    {
        callback();
    }
}

template <typename T>
void complete_cancelled(std::shared_ptr<task_state<T>> const& state)
{
    std::function<void()> callback;
    {
        std::lock_guard lock{state->mutex};

        if(state->ready.load(std::memory_order_relaxed))
        {
            throw std::logic_error{"entityfy::task was already completed."};
        }

        state->cancelled = true;
        callback = publish_ready(*state);
    }

    state->cv.notify_all();

    if(callback)
    {
        callback();
    }
}

// Moves the terminal outcome of a ready source state into a destination state.
template <typename T>
void forward_outcome(std::shared_ptr<task_state<T>> const& source, std::shared_ptr<task_state<T>> const& destination)
{
    std::exception_ptr error;
    bool cancelled = false;
    std::optional<task_value<T>> value;
    {
        std::lock_guard lock{source->mutex};

        error = source->error;
        cancelled = source->cancelled;

        if(!error && !cancelled)
        {
            value.emplace(std::move(source->value));
            source->value.drop();
        }
    }

    if(error)
    {
        complete_exception(destination, std::move(error));
        return;
    }

    if(cancelled)
    {
        complete_cancelled(destination);
        return;
    }

    if constexpr (std::is_void_v<T>)
    {
        complete_value(destination);
    }
    else
    {
        complete_value(destination, value->take());
    }
}

template <typename U, typename F, typename... Args>
void invoke_and_forward(F& continuation, std::shared_ptr<task_state<U>> const& destination, Args&&... args)
{
    using result_type = std::decay_t<std::invoke_result_t<F&, Args...>>;

    if constexpr (is_task_v<result_type>)
    {
        static_assert(std::is_same_v<typename unbox_task<result_type>::type, U>);

        result_type inner = std::invoke(continuation, std::forward<Args>(args)...);
        auto inner_state = task_access::take_state(inner);
        std::weak_ptr<task_state<U>> weak_inner = inner_state;

        install_completion(inner_state, [weak_inner, destination]() mutable {
            if(auto locked = weak_inner.lock())
            {
                forward_outcome(locked, destination);
            }
        });
    }
    else if constexpr (std::is_void_v<result_type>)
    {
        std::invoke(continuation, std::forward<Args>(args)...);
        complete_value(destination);
    }
    else
    {
        complete_value(destination, std::invoke(continuation, std::forward<Args>(args)...));
    }
}

// Runs the continuation of `then` on the thread that completed the source.
template <typename T, typename U, typename F>
void run_continuation(std::shared_ptr<task_state<T>> const& source, std::shared_ptr<task_state<U>> const& destination, F& continuation)
{
    std::exception_ptr error;
    bool cancelled = false;
    std::optional<task_value<T>> value;
    {
        std::lock_guard lock{source->mutex};

        error = source->error;
        cancelled = source->cancelled;

        if(!error && !cancelled)
        {
            value.emplace(std::move(source->value));
            source->value.drop();
        }
    }

    if(error)
    {
        complete_exception(destination, std::move(error));
        return;
    }

    if(cancelled)
    {
        complete_cancelled(destination);
        return;
    }

    try
    {
        if constexpr (std::is_void_v<T>)
        {
            invoke_and_forward<U>(continuation, destination);
        }
        else
        {
            invoke_and_forward<U>(continuation, destination, value->take());
        }
    }
    catch(...)
    {
        complete_exception(destination, std::current_exception());
    }
}

template <typename T, typename F>
[[nodiscard]] task<then_result_t<T, F>> then_impl(std::shared_ptr<task_state<T>> source, F&& continuation)
{
    using destination_type = then_result_t<T, F>;

    auto destination = std::make_shared<task_state<destination_type>>();
    auto callable = std::make_shared<std::decay_t<F>>(std::forward<F>(continuation));

    std::weak_ptr<task_state<T>> weak_source = source;
    install_completion(source, [weak_source, destination, callable]() mutable {
        if(auto locked = weak_source.lock())
        {
            run_continuation<T, destination_type>(locked, destination, *callable);
        }
    });

    return task_access::make<destination_type>(std::move(destination));
}

} // namespace detail

template <typename T>
[[nodiscard]] task_pair<T> make_task()
{
    auto state = std::make_shared<detail::task_state<T>>();

    return task_pair<T>{
        detail::task_access::make<T>(state),
        detail::task_access::make_completer<T>(std::move(state)),
    };
}

} // namespace entityfy
