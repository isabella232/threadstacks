#include "threadstacks/stack_tracer.h"
// Copyright: Pixie Labs Inc 2019
// Author: Zain Asgar(zasgar@pixielabs.ai)

// Inspired by Envoy BackwardsTrace, but uses libunwind instead.

// The following #define makes libunwind use a faster unwinding mechanism.
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <signal.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <memory>

#include "absl/debugging/symbolize.h"


namespace threadstacks {

void ErrLog(const char* msg) { write(STDERR_FILENO, msg, strlen(msg)); }

// The %p field width for printf() functions is two characters per byte,
// and two extra for the leading "0x".
static constexpr int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

/**
 * From ABSL.
 */
// `void*` might not be big enough to store `void(*)(const char*)`.
struct WriterFnStruct {
  void (*writerfn)(const char*);
};

// Many of the absl::debugging_internal::Dump* functions in
// examine_stack.h take a writer function pointer that has a void* arg
// for historical reasons. failure_signal_handler_writer only takes a
// data pointer. This function converts between these types.
void WriterFnWrapper(const char* data, void* arg) {
  static_cast<WriterFnStruct*>(arg)->writerfn(data);
}

/**
 * END From ABSL.
 */

void ThreadStack::AddFrame(int64_t size, int64_t addr) {
  address[depth] = addr;
  sizes[depth] = size;
  ++depth;
}

void ThreadStack::Visit(const std::function<void(int /*depth*/, int64_t /*frame_size*/, int64_t /*addr*/)>& visitor)  const {
  for (int i = 0; i < depth; ++i) {
    visitor(i, sizes[i], address[i]);
  }
}

void ThreadStack::VisitWithSymbol(const std::function<void(int /*depth*/, int64_t /*frame_size*/, int64_t /*addr*/, const char* /*sym*/)>& visitor) const {
  const char *kUnknown = "(unknown)";
  char buffer[1024];
  for (int i = 0; i < depth; ++i) {
    // Note(zasgar): This is a bit hacky, but if symbolization fails we try to symbolize
    // PC - 1. This is because the address might actually be the return value. Strictly,
    // this only applies to the last PC so we can probably make this more robust.
    if (absl::Symbolize(reinterpret_cast<char*>(address[i]),
                        buffer,
                        sizeof buffer) ||
        absl::Symbolize(reinterpret_cast<char*>(address[i]) - 1,
                        buffer,
                        sizeof buffer)) {
      visitor(i, sizes[i], address[i], buffer);
    } else {
      visitor(i, sizes[i], address[i], kUnknown);
    }
  }
}

void ThreadStack::PrettyPrint(const std::function<void(const char*)> writer) const {
  VisitWithSymbol(
      [&](int depth, int64_t framesize, int64_t addr, const char* symbol) {
        char buf[256];
        void* pc = reinterpret_cast<void*>(addr);
        const char * prefix = depth == 0 ? "PC: " : "    ";

        if (framesize <= 0) {
          snprintf(buf, sizeof(buf), "%s@ %*p  (unknown)  %s\n", prefix,
                   kPrintfPointerFieldWidth, pc, symbol);
        } else {
          snprintf(buf, sizeof(buf), "%s@ %*p  %9ld  %s\n", prefix,
                   kPrintfPointerFieldWidth, pc, framesize, symbol);
        }

        writer(buf);
      });
}

/**
 * Capture the stack trace starting at the current location
 */
void BackwardsTrace::Capture() {
  unw_context_t context;
  if (0 != unw_getcontext(&context)) {
    ErrLog("StacktraceCollector: Failed to get current context\n");
    return;
  }
  Capture(&context, /* skip_count */ 0);
}

/*
 * Capture the stack trace starting at the ucontext passed in.
 */
void BackwardsTrace::Capture(void *ucontext, int skip_count) {
  // NOTE(zasgar): Using the ucontext at unwind context is not strictly correct,
  // but works on IA-64 ABI.
  unw_context_t *context = reinterpret_cast<unw_context_t*>(ucontext);
  unw_cursor_t cursor;
  if (0 != unw_init_local(&cursor, context)) {
    ErrLog("StacktraceCollector: Failed to initialize unwinding cursor\n");
    return;
  }

  while(unw_step(&cursor) > 0 && (skip_count-- > 0)) {
    // Skip frames.
  }
  while (unw_step(&cursor) > 0 && stack_.depth < kMaxStackDepth) {
    unw_word_t ip;
    if (0 == unw_get_reg(&cursor, UNW_REG_IP, &ip)) {
      stack_.AddFrame(0, ip);
    } else {
      ErrLog("Failed to get instruction pointer...\n");
    }
  }
}


}  // namespace threadstacks
