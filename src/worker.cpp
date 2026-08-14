#include "worker.h"

#include "utils.h"

#include <exception>
#include <utility>

ThreadPool::ThreadPool(size_t threads) {
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::Enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++pending_;
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::EnqueueBatch(std::vector<std::function<void()>> tasks) {
    if (tasks.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_ += tasks.size();
        for (auto& t : tasks) tasks_.push_back(std::move(t));
    }
    cv_.notify_all();
}

void ThreadPool::WaitAll() {
    std::unique_lock<std::mutex> lk(doneMutex_);
    doneCv_.wait(lk, [this] { return pending_ == 0; });
}

size_t ThreadPool::Pending() {
    std::lock_guard<std::mutex> lk(mutex_);
    return pending_;
}

void ThreadPool::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            util::WriteLogLine(L"[worker] task exception: " + util::ToWide(e.what()));
        } catch (...) {
            util::WriteLogLine(L"[worker] task unknown exception");
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            --pending_;
        }
        doneCv_.notify_all();
    }
}
