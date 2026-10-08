#pragma once
// A few long-lived worker threads for background work (cell loading, path finding, character
// imports). The threads start with the first task and live until the pool is destroyed: no thread
// starts or ends while the game runs, and dropping a future never waits for a thread (std::async
// does both - a thread per task whose future joins it - and froze the loading screen on some
// Windows PCs). Tasks run in the order they were submitted; an exception thrown by a task goes to
// its future.

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace rjc {

class JobPool {
 public:
  explicit JobPool(int threads) : n_(threads < 1 ? 1 : threads) {}
  JobPool(const JobPool&) = delete;
  JobPool& operator=(const JobPool&) = delete;
  ~JobPool() {
    {
      std::lock_guard<std::mutex> lk(mx_);
      stop_ = true;
      q_.clear();  // (tasks not started: their futures report broken_promise)
    }
    cv_.notify_all();
    for (auto& t : th_) t.join();
  }

  template <class F>
  auto submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    std::future<R> fut = task->get_future();
    {
      std::lock_guard<std::mutex> lk(mx_);
      if (th_.empty())
        for (int i = 0; i < n_; ++i) th_.emplace_back([this] { run(); });
      q_.push_back([task] { (*task)(); });
    }
    cv_.notify_one();
    return fut;
  }

 private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(mx_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        job = std::move(q_.front());
        q_.pop_front();
      }
      job();
    }
  }

  int n_;
  std::mutex mx_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> q_;
  std::vector<std::thread> th_;
  bool stop_ = false;
};

}  // namespace rjc
