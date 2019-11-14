#pragma once
// Copyright: Pixie Labs Inc 2019
// Author: Zain Asgar(zasgar@pixielabs.ai)

// Inspired by Envoy BackwardsTrace, but uses libunwind instead.


#include <string>
#include <functional>


namespace threadstacks {


// Stack trace of a thread.
struct ThreadStack {
  // Maximum depth allowed for a stack trace.
  static constexpr int kMaxDepth = 100;
  // Thread id of the thread.
  int tid = -1;
  // The stack trace, in term of memory addresses.
  int64_t address[kMaxDepth];
  // The size of stack trace, bytes;
  int64_t sizes[kMaxDepth];
  // Actual depth of the stack trace.
  int depth = 0;

  void AddFrame(int64_t size, int64_t addr);
  void Visit(const std::function<void(int /*depth*/, int64_t /*frame_size*/, int64_t /*addr*/)>& visitor)  const;
  void VisitWithSymbol(const std::function<void(int /*depth*/, int64_t /*frame_size*/, int64_t /*addr*/, const char* /*sym*/)>& visitor) const;
  void PrettyPrint(const std::function<void(const char*)> writer) const;
};


class BackwardsTrace {
public:
  BackwardsTrace() = default;

  /**
   * Capture the stack trace starting at the current location
   */
  void Capture();

  /*
   * Capture the stack trace starting at the ucontext passed in.
   */
  void Capture(void *ucontext, int skip_count=0);
  const ThreadStack& stack() { return stack_; }

private:
  static constexpr int kMaxStackDepth = ThreadStack::kMaxDepth;;
  ThreadStack stack_;
};



#define BACKTRACE_LOG()                                           \
  do {                                                            \
    ::threadstacks::BackwardsTrace trace;                         \
    trace.Capture();                                              \
    std::string res;                                              \
    res += "-------------------------------------------------\n"; \
    trace.stack().PrettyPrint(                                    \
        [&res](const char *s) {                                   \
          res += s;                                               \
        });                                                       \
    res += "-------------------------------------------------\n"; \
    LOG(INFO) << "BACKTRACE: \n" << res;                          \
  } while(0)

}  // namespace threadstacks
