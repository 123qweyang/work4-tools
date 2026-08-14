#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// 简单多线程任务池：支持任务内再派发子任务，WaitAll 等待全部完成
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Enqueue(std::function<void()> task);
    void EnqueueBatch(std::vector<std::function<void()>> tasks);
    void WaitAll();
    size_t ThreadCount() const { return workers_.size(); }
    size_t Pending();

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pending_ = 0;
    std::mutex doneMutex_;
    std::condition_variable doneCv_;
    bool stop_ = false;
};
