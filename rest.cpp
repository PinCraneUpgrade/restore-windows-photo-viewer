#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

struct Task {
    std::uint64_t id{};
    std::string payload;
    int priority{};
    Clock::time_point created{Clock::now()};
};

struct Result {
    std::uint64_t id{};
    std::string digest;
    double score{};
    Millis elapsed{};
    bool cached{};
};

class Logger {
    std::mutex mutex_;

    std::string stamp() const {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};

#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif

        std::ostringstream out;
        out << std::put_time(&tm, "%H:%M:%S");
        return out.str();
    }

public:
    template <typename... Args>
    void info(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        (out << ... << std::forward<Args>(args));
        std::cout << "[" << stamp() << "] " << out.str() << '\n';
    }
};

template <typename Key, typename Value>
class LruCache {
    using Pair = std::pair<Key, Value>;

    std::size_t capacity_;
    std::list<Pair> items_;
    std::unordered_map<
        Key,
        typename std::list<Pair>::iterator
    > index_;

    mutable std::mutex mutex_;

public:
    explicit LruCache(std::size_t capacity)
        : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = index_.find(key);

        if (it == index_.end())
            return std::nullopt;

        items_.splice(
            items_.begin(),
            items_,
            it->second
        );

        return it->second->second;
    }

    void put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = index_.find(key);

        if (it != index_.end()) {
            it->second->second = value;
            items_.splice(
                items_.begin(),
                items_,
                it->second
            );
            return;
        }

        items_.emplace_front(key, value);
        index_[key] = items_.begin();

        if (index_.size() > capacity_) {
            auto last = std::prev(items_.end());
            index_.erase(last->first);
            items_.pop_back();
        }
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index_.size();
    }
};

class HashEngine {
    static std::uint64_t rotate(
        std::uint64_t x,
        unsigned n
    ) {
        return (x << n) | (x >> (64U - n));
    }

public:
    static std::string digest(const std::string& input) {
        std::uint64_t a =
            0x243F6A8885A308D3ULL;

        std::uint64_t b =
            0x13198A2E03707344ULL;

        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto c =
                static_cast<unsigned char>(input[i]);

            a ^= static_cast<std::uint64_t>(c)
                 + 0x9E3779B97F4A7C15ULL
                 + (a << 6)
                 + (a >> 2);

            b += rotate(
                a ^ (static_cast<std::uint64_t>(c) << 17),
                static_cast<unsigned>((i % 31) + 1)
            );

            a = rotate(a, 13);
            b ^= rotate(b, 29);
        }

        std::ostringstream out;
        out << std::hex
            << std::setw(16)
            << std::setfill('0')
            << a
            << std::setw(16)
            << b;

        return out.str();
    }
};

class Statistics {
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> cached_{0};
    std::atomic<std::uint64_t> failed_{0};

    std::atomic<std::uint64_t> totalTime_{0};

public:
    void processed(std::uint64_t ms) {
        ++processed_;
        totalTime_ += ms;
    }

    void cacheHit() {
        ++cached_;
    }

    void failed() {
        ++failed_;
    }

    std::uint64_t count() const {
        return processed_.load();
    }

    std::uint64_t cacheHits() const {
        return cached_.load();
    }

    std::uint64_t failures() const {
        return failed_.load();
    }

    double averageTime() const {
        const auto count = processed_.load();

        if (count == 0)
            return 0.0;

        return static_cast<double>(
            totalTime_.load()
        ) / static_cast<double>(count);
    }
};

class TaskQueue {
    std::priority_queue<
        Task,
        std::vector<Task>,
        std::function<bool(const Task&, const Task&)>
    > queue_;

    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_{false};

public:
    TaskQueue()
        : queue_(
            [](const Task& a, const Task& b) {
                return a.priority < b.priority;
            }
        ) {}

    void push(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_)
                return;

            queue_.push(std::move(task));
        }

        condition_.notify_one();
    }

    std::optional<Task> pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(
            lock,
            [&] {
                return stopped_ || !queue_.empty();
            }
        );

        if (queue_.empty())
            return std::nullopt;

        Task task = std::move(
            const_cast<Task&>(queue_.top())
        );

        queue_.pop();

        return task;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }

        condition_.notify_all();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
};

class Processor {
    Logger& logger_;
    Statistics& statistics_;
    LruCache<std::string, Result>& cache_;

    std::mt19937_64 generator_;
    std::uniform_int_distribution<int> delay_;

public:
    Processor(
        Logger& logger,
        Statistics& statistics,
        LruCache<std::string, Result>& cache
    )
        : logger_(logger),
          statistics_(statistics),
          cache_(cache),
          generator_(std::random_device{}()),
          delay_(4, 20) {}

    Result execute(const Task& task) {
        const auto start = Clock::now();

        if (auto cached = cache_.get(task.payload)) {
            Result result = *cached;
            result.id = task.id;
            result.cached = true;

            statistics_.cacheHit();

            logger_.info(
                "cache hit for task #",
                task.id
            );

            return result;
        }

        std::this_thread::sleep_for(
            Millis(delay_(generator_))
        );

        const std::string hash =
            HashEngine::digest(task.payload);

        double score = 0.0;

        for (std::size_t i = 0;
             i < task.payload.size();
             ++i) {

            const unsigned char c =
                static_cast<unsigned char>(
                    task.payload[i]
                );

            score +=
                static_cast<double>(
                    (c * (i + 3)) % 101
                ) / 100.0;
        }

        score *=
            1.0 +
            static_cast<double>(task.priority)
            * 0.07;

        const auto elapsed =
            std::chrono::duration_cast<Millis>(
                Clock::now() - start
            );

        Result result{
            task.id,
            hash,
            score,
            elapsed,
            false
        };

        cache_.put(task.payload, result);
        statistics_.processed(
            static_cast<std::uint64_t>(
                elapsed.count()
            )
        );

        logger_.info(
            "processed task #",
            task.id,
            " in ",
            elapsed.count(),
            " ms"
        );

        return result;
    }
};

class WorkerPool {
    TaskQueue queue_;
    Processor processor_;

    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    std::mutex resultsMutex_;
    std::vector<Result> results_;

public:
    WorkerPool(
        Logger& logger,
        Statistics& statistics,
        LruCache<std::string, Result>& cache,
        std::size_t workers
    )
        : processor_(
            logger,
            statistics,
            cache
        ) {
        workers_.reserve(workers);

        for (std::size_t i = 0; i < workers; ++i) {
            workers_.emplace_back(
                [this, i] {
                    workerLoop(i);
                }
            );
        }

        running_ = true;
    }

    ~WorkerPool() {
        shutdown();
    }

    void submit(Task task) {
        queue_.push(std::move(task));
    }

    void workerLoop(std::size_t index) {
        while (true) {
            auto task = queue_.pop();

            if (!task)
                break;

            try {
                Result result =
                    processor_.execute(*task);

                {
                    std::lock_guard<std::mutex> lock(
                        resultsMutex_
                    );

                    results_.push_back(
                        std::move(result)
                    );
                }
            }
            catch (...) {
                std::cerr
                    << "worker "
                    << index
                    << " encountered an error\n";
            }
        }
    }

    void shutdown() {
        if (!running_.exchange(false))
            return;

        queue_.stop();

        for (auto& worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
    }

    std::vector<Result> results() const {
        std::lock_guard<std::mutex> lock(
            const_cast<std::mutex&>(resultsMutex_)
        );

        return results_;
    }
};

class DataGenerator {
    std::mt19937_64 generator_;
    std::uniform_int_distribution<int> priority_;

public:
    DataGenerator()
        : generator_(std::random_device{}()),
          priority_(1, 10) {}

    Task generate(std::uint64_t id) {
        static const std::vector<std::string> words{
            "quantum",
            "vector",
            "matrix",
            "protocol",
            "compiler",
            "network",
            "algorithm",
            "database",
            "pipeline",
            "runtime"
        };

        std::ostringstream payload;

        payload
            << words[id % words.size()]
            << '-'
            << (id * 7919 % 100000)
            << '-'
            << (id * 104729 % 999983);

        return Task{
            id,
            payload.str(),
            priority_(generator_),
            Clock::now()
        };
    }
};

void printResults(
    const std::vector<Result>& results
) {
    std::vector<Result> sorted = results;

    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const Result& a, const Result& b) {
            return a.id < b.id;
        }
    );

    std::cout << "\n========== RESULTS ==========\n";

    for (const auto& result : sorted) {
        std::cout
            << "#"
            << std::setw(3)
            << result.id
            << " | score="
            << std::fixed
            << std::setprecision(3)
            << std::setw(8)
            << result.score
            << " | time="
            << std::setw(3)
            << result.elapsed.count()
            << "ms"
            << " | "
            << (result.cached
                ? "CACHE"
                : "FRESH")
            << " | "
            << result.digest
            << '\n';
    }
}

} // namespace core

int main() {
    using namespace core;

    Logger logger;
    Statistics statistics;

    LruCache<std::string, Result> cache(32);

    const std::size_t threadCount =
        std::max<std::size_t>(
            2,
            std::thread::hardware_concurrency()
        );

    logger.info(
        "initializing worker pool with ",
        threadCount,
        " workers"
    );

    WorkerPool pool(
        logger,
        statistics,
        cache,
        threadCount
    );

    DataGenerator generator;

    constexpr std::size_t taskCount = 80;

    logger.info(
        "generating ",
        taskCount,
        " tasks"
    );

    for (std::size_t i = 1;
         i <= taskCount;
         ++i) {

        pool.submit(
            generator.generate(
                static_cast<std::uint64_t>(i)
            )
        );
    }

    logger.info("waiting for workers");

    std::this_thread::sleep_for(
        std::chrono::milliseconds(500)
    );

    pool.shutdown();

    const auto results = pool.results();

    printResults(results);

    std::cout
        << "\n========== STATISTICS ==========\n"
        << "processed: "
        << statistics.count()
        << '\n'
        << "cache hits: "
        << statistics.cacheHits()
        << '\n'
        << "failures: "
        << statistics.failures()
        << '\n'
        << "average processing time: "
        << std::fixed
        << std::setprecision(2)
        << statistics.averageTime()
        << " ms\n"
        << "cache size: "
        << cache.size()
        << '\n';

    std::cout
        << "\nExecution completed successfully.\n";

    return 0;
}
