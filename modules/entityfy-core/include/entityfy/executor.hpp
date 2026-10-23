#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Where a continuation or an offloaded job runs. An executor owns only the
// scheduling policy: it never creates a thread per job and never blocks the
// caller beyond the bounded-queue backpressure of the pool.

namespace entityfy
{

class executor
{
public:
    virtual ~executor() = default;

    virtual void submit(std::function<void()> job) = 0;

    [[nodiscard]] virtual bool is_inline() const noexcept
    {
        return false;
    }
};

// Runs the job on the calling thread before submit returns.
class inline_executor final : public executor
{
public:
    void submit(std::function<void()> job) override
    {
        job();
    }

    [[nodiscard]] bool is_inline() const noexcept override
    {
        return true;
    }
};

// Fixed set of worker threads over a bounded queue: submit blocks while the
// queue is full instead of growing without limit. A max_queue of 0 means the
// queue is unbounded.
class thread_pool_executor final : public executor
{
public:
    explicit thread_pool_executor(std::size_t threads = std::thread::hardware_concurrency(), std::size_t max_queue = 1024)
        : max_queue_{max_queue}
    {
        const std::size_t worker_count = threads == 0 ? 1 : threads;
        workers_.reserve(worker_count);

        for(std::size_t index = 0; index < worker_count; ++index)
        {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Drains queued and active jobs, then joins every worker.
    ~thread_pool_executor() override
    {
        shutdown();
    }

    thread_pool_executor(const thread_pool_executor&) = delete;
    thread_pool_executor& operator=(const thread_pool_executor&) = delete;

    // Blocks while the queue is full and throws logic_error once the pool is
    // shutting down. A job that throws never takes a worker down.
    void submit(std::function<void()> job) override
    {
        {
            std::unique_lock lock{mutex_};

            if(stopping_)
            {
                throw std::logic_error{"entityfy::thread_pool_executor cannot accept a job after shutdown."};
            }

            if(max_queue_ != 0)
            {
                space_cv_.wait(lock, [this] { return stopping_ || queue_.size() < max_queue_; });

                if(stopping_)
                {
                    throw std::logic_error{"entityfy::thread_pool_executor cannot accept a job after shutdown."};
                }
            }

            queue_.push_back(std::move(job));
        }

        work_cv_.notify_one();
    }

    // Idempotent: lets every queued and running job finish, then joins. Must not
    // be called from one of the pool's own jobs.
    void shutdown()
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
            workers = std::move(workers_);
        }

        work_cv_.notify_all();
        space_cv_.notify_all();

        for(auto& worker : workers)
        {
            if(worker.joinable())
            {
                worker.join();
            }
        }
    }

    // Queued jobs, excluding the ones a worker is running right now.
    [[nodiscard]] std::size_t pending() const
    {
        std::lock_guard lock{mutex_};
        return queue_.size();
    }

private:
    void worker_loop()
    {
        for(;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock{mutex_};
                work_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

                if(queue_.empty())
                {
                    if(stopping_)
                    {
                        return;
                    }

                    continue;
                }

                job = std::move(queue_.front());
                queue_.pop_front();
            }

            space_cv_.notify_one();

            if(!job)
            {
                continue;
            }

            try
            {
                job();
            }
            catch(...)
            {
                // The caller owns error reporting; a failing job must not stop
                // the pool or terminate the process.
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable space_cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::size_t max_queue_;
    bool stopping_ = false;
};

} // namespace entityfy
