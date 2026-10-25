#include "thread.h"

#include <process.h>

namespace {

unsigned __stdcall entry(void* arg) {
  std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
  (*fn)();
  return 0;
}

}  // namespace

Thread::Thread(std::function<void()> fn) {
  auto* job = new std::function<void()>(std::move(fn));
  const uintptr_t h = _beginthreadex(nullptr, 0, entry, job, 0, nullptr);
  if (!h) {
    delete job;
    std::terminate();  // no thread, no way to carry on: the same as std::thread running out of them
  }
  h_ = (HANDLE)h;
}

Thread& Thread::operator=(Thread&& other) noexcept {
  if (this != &other) {
    if (h_) std::terminate();
    h_ = other.h_;
    other.h_ = nullptr;
  }
  return *this;
}

Thread::~Thread() {
  if (h_) std::terminate();
}

void Thread::join() {
  if (!h_) return;
  WaitForSingleObject(h_, INFINITE);
  CloseHandle(h_);
  h_ = nullptr;
}

void Thread::detach() {
  if (!h_) return;
  CloseHandle(h_);
  h_ = nullptr;
}

void runDetached(std::function<void()> fn) { Thread(std::move(fn)).detach(); }
