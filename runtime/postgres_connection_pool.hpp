#pragma once

#ifdef AGENTOS_ENABLE_POSTGRES

#include <pqxx/pqxx>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class PostgresConnectionPool {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                pool_ = std::exchange(other.pool_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        ~Lease() {
            release();
        }

        pqxx::connection& connection() const {
            if (pool_ == nullptr) {
                throw std::logic_error("connection lease is empty");
            }
            return *pool_->connections_[index_];
        }

    private:
        friend class PostgresConnectionPool;

        Lease(PostgresConnectionPool& pool, std::size_t index)
            : pool_(&pool), index_(index) {}

        void release() noexcept {
            if (pool_ != nullptr) {
                pool_->release(index_);
                pool_ = nullptr;
            }
        }

        PostgresConnectionPool* pool_ = nullptr;
        std::size_t index_ = 0;
    };

    explicit PostgresConnectionPool(const std::string& connection_string,
                                    std::size_t size = 8) {
        if (size == 0) {
            throw std::invalid_argument("PostgreSQL connection pool size must be positive");
        }
        connections_.reserve(size);
        available_.assign(size, true);
        for (std::size_t index = 0; index < size; ++index) {
            connections_.push_back(std::make_unique<pqxx::connection>(connection_string));
        }
    }

    PostgresConnectionPool(const PostgresConnectionPool&) = delete;
    PostgresConnectionPool& operator=(const PostgresConnectionPool&) = delete;

    Lease acquire() {
        std::unique_lock lock(mutex_);
        available_condition_.wait(lock, [this] {
            for (const auto available : available_) {
                if (available) {
                    return true;
                }
            }
            return false;
        });
        for (std::size_t index = 0; index < available_.size(); ++index) {
            if (available_[index]) {
                available_[index] = false;
                return Lease(*this, index);
            }
        }
        throw std::logic_error("PostgreSQL connection pool wakeup without a connection");
    }

private:
    void release(std::size_t index) noexcept {
        {
            std::lock_guard lock(mutex_);
            available_[index] = true;
        }
        available_condition_.notify_one();
    }

    std::vector<std::unique_ptr<pqxx::connection>> connections_;
    std::vector<bool> available_;
    std::mutex mutex_;
    std::condition_variable available_condition_;
};

#endif
