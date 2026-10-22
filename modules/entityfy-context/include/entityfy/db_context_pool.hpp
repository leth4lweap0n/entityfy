#pragma once

#include <entityfy/db_context.hpp>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace entityfy
{

// Similar to DbContextPool: reuses ready contexts and returns them to the pool
// with the change tracker cleared when a lease ends. The pool outlives its leases.
class db_context_pool
{
public:
    class lease
    {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

        lease(lease&& other) noexcept
            : pool_{other.pool_}, context_{std::move(other.context_)}
        {
            other.pool_ = nullptr;
        }

        lease& operator=(lease&& other) noexcept
        {
            if(this != &other)
            {
                release();
                pool_ = other.pool_;
                context_ = std::move(other.context_);
                other.pool_ = nullptr;
            }

            return *this;
        }

        ~lease()
        {
            release();
        }

        [[nodiscard]] db_context& operator*() const noexcept
        {
            return *context_;
        }

        [[nodiscard]] db_context* operator->() const noexcept
        {
            return context_.get();
        }

        [[nodiscard]] db_context* get() const noexcept
        {
            return context_.get();
        }

    private:
        friend class db_context_pool;

        lease(db_context_pool* pool, std::unique_ptr<db_context> context)
            : pool_{pool}, context_{std::move(context)}
        {
        }

        void release() noexcept
        {
            if(pool_ != nullptr && context_ != nullptr)
            {
                pool_->release(std::move(context_));
                pool_ = nullptr;
            }
        }

        db_context_pool*            pool_;
        std::unique_ptr<db_context> context_;
    };

    explicit db_context_pool(std::string uri, std::size_t max_size = 16)
        : uri_{std::move(uri)}, max_size_{max_size == 0 ? 1 : max_size}
    {
    }

    db_context_pool(const db_context_pool&) = delete;
    db_context_pool& operator=(const db_context_pool&) = delete;

    [[nodiscard]] lease Acquire()
    {
        std::unique_lock lock{mutex_};

        if(idle_.empty() && created_ >= max_size_)
        {
            available_.wait(lock, [&] { return !idle_.empty(); });
        }

        if(!idle_.empty())
        {
            auto context = std::move(idle_.back());
            idle_.pop_back();
            return lease{this, std::move(context)};
        }

        auto created = std::make_unique<db_context>(uri_);
        ++created_;
        return lease{this, std::move(created)};
    }

    [[nodiscard]] std::size_t IdleCount() const
    {
        std::unique_lock lock{mutex_};
        return idle_.size();
    }

    [[nodiscard]] std::size_t CreatedCount() const
    {
        std::unique_lock lock{mutex_};
        return created_;
    }

private:
    void release(std::unique_ptr<db_context> context) noexcept
    {
        if(!context)
        {
            return;
        }

        context->Clear();

        std::unique_lock lock{mutex_};
        idle_.push_back(std::move(context));
        available_.notify_one();
    }

    std::string                              uri_;
    std::size_t                              max_size_;
    mutable std::mutex                       mutex_;
    std::condition_variable                  available_;
    std::vector<std::unique_ptr<db_context>> idle_;
    std::size_t                              created_ = 0;
};

} // namespace entityfy
