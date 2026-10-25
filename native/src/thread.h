// Threads made straight from the Windows API instead of std::thread. The toolchain's std::thread goes through
// winpthreads, which recycles its thread records: when one thread is created while another is ending, the new one
// can be started with a null argument and die on its first instruction (an access violation inside
// execute_native_thread_routine, seen in about half the full self-test runs of the release exe on two Windows
// Server versions, and never on the development PC). Nothing here creates a pthread.
#pragma once
#include "common.h"

class Thread {
 public:
  Thread() = default;
  explicit Thread(std::function<void()> fn);
  Thread(Thread&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
  Thread& operator=(Thread&& other) noexcept;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread();  // like std::thread: a thread that is still joinable here ends the program, so join() or detach() it

  bool joinable() const { return h_ != nullptr; }
  void join();
  void detach();

 private:
  HANDLE h_ = nullptr;
};

// Runs `fn` on a thread of its own and lets it go.
void runDetached(std::function<void()> fn);
