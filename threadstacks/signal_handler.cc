

// Edited: Pixie Labs Inc. 2019 (zasgar@pixielabs.ai)

#include "threadstacks/signal_handler.h"
#include "absl/debugging/symbolize.h"
#include <fcntl.h>
// The following #define makes libunwind use a faster unwinding mechanism.
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/defer.h"
#include "common/sysutil.h"
#include "threadstacks/stack_tracer.h"

namespace threadstacks {
namespace {

// A form sent by StackTraceCollector to threads to fill in their stack trace
// and submit the results. Note that methods of this class invoked by signal
// handler of recipient threads should *NOT* call any async-signal-unsafe
// methods.
class StackTraceForm {
 public:
  StackTraceForm(pid_t tid, int ack_fd) : ack_fd_(ack_fd) { stack_.tid = tid; }
  ~StackTraceForm() = default;

  // Adds an address to the stack trace.
  bool AddInfo(int64_t size, int64_t address) {
    if (stack_.depth >= ThreadStack::kMaxDepth) {
      return false;
    }
    stack_.sizes[stack_.depth] = size;
    stack_.address[stack_.depth] = address;
    stack_.depth++;
    return true;
  }

  // Submits the strack trace form.
  bool Submit() {
    // Write a one-byte ack.
    const char ack_ch = 'y';  // Value doesn't matter.
    static_assert(1 == sizeof(ack_ch), "char size is not 1 byte");
    auto num_written = write(ack_fd_, &ack_ch, sizeof(ack_ch));
    return sizeof(ack_ch) == num_written;
  }

  // Returns a const reference to the stack trace submitted in the form.
  const ThreadStack& stack() const { return stack_; }

 private:
  // File descriptor where the ack should be written.
  const int ack_fd_;
  // Stack trace of the thread.
  ThreadStack stack_;
};

// State associated with the external stacktrace signal handler.
struct ExternalHandlerState {
  ExternalHandlerState();
  const pid_t server_tgid;
  int server_fd;
};

// Note that this function uses a function local static to guarantee a single
// initialization of ExternalHandlerState object that it returns. This means
// that any subsequent calls to this function don't update the external handler
// state or result in any side-effects of construction of ExternalHandlerState
// object (e.g. launch of RequestProcessor thread).
const ExternalHandlerState& GetExternalHandlerState() {
  static ExternalHandlerState state;
  return state;
};

// Returns in @difference the set of elements that are present in the first
// set, but not in the second one.
template <typename T>
void STLSetDifference(const std::set<T>& first,
                      const std::set<T>& second,
                      std::set<T>* difference) {
  difference->clear();
  std::set_difference(first.begin(), first.end(),
                      second.begin(), second.end(),
                      std::inserter(*difference, difference->end()));
}

void ErrLog(const char* msg) { write(STDERR_FILENO, msg, strlen(msg)); }

void InternalHandler(int signum, siginfo_t* siginfo, void* ucontext) {
  // Typically the stacktrace collection signal is sent by a StackTraceCollector
  // object. However, it can also be sent by an external entity, e.g. using
  // 'kill' command. We choose to ignore the signal in that case.
  if (siginfo->si_pid != getpid()) {
    ErrLog("Ignoring signal sent from an outsider pid...\n");
    return;
  }
  auto form = reinterpret_cast<StackTraceForm*>(siginfo->si_value.sival_ptr);
  if (nullptr == form) {
    ErrLog("Couldn't retrieve StackTraceForm pointer, ignoring signal...\n");
    return;
  }

  BackwardsTrace trace;
  trace.Capture(ucontext);
  trace.stack().Visit([&](int, int size, int64_t addr) {
                form->AddInfo(size, reinterpret_cast<int64_t>(addr));
                      });

  if (not form->Submit()) {
    ErrLog("Failed to submit stacktrace form...\n");
  }
}

void ExternalHandler(int signum,
                     siginfo_t* siginfo,
                     void* ucontext,
                     int stack_trace_fd) {
  int pipe_fd[2];
  if (-1 == pipe(pipe_fd)) {
    ErrLog(
        "Failed to create pipe for communicating with stack trace "
        "service thread\n");
    return;
  }
  // We don't wait for the stack trace service thread to finish servicing this
  // request, so the read end can be closed right away.
  close(pipe_fd[0]);
  auto ret = write(stack_trace_fd, &pipe_fd[1], sizeof(pipe_fd[1]));
  if (-1 == ret) {
    // TODO(nipun): Print errno.
    ErrLog("Failed to send a request to stack trace service thread\n");
    close(pipe_fd[1]);
    return;
  }
  if (sizeof(pipe_fd[1]) != ret) {
    ErrLog("Failed to request stack trace service thread.\n");
    close(pipe_fd[1]);
    return;
  }
}

// The signal handler for external stack trace collection signal. It basically
// delegates the collection work to the dedicated signal collection request
// processor thread. Such an indirection help us work around the restrictions
// imposed by asyn-signal-safety, such as not being able to allocate memory in
// signal handler.
void ExternalStackTraceSignalHandler(int signum,
                                     siginfo_t* siginfo,
                                     void* ucontext) {
  // This happens when a process forks a child process, which then received a
  // signal. The child process will not have the stack trace server thread, as
  // fork doesn't carry forward all the threads of the forked process. So, we
  // simply check whether the thread group-id of this process is the same as the
  // thread group-id of the process that started the stack trace server.
  if (GetExternalHandlerState().server_tgid != getpid()) {
    ErrLog(
        "Not contacting stack trace server started in a different thread "
        "group\n");
  } else {
    ExternalHandler(
        signum, siginfo, ucontext, GetExternalHandlerState().server_fd);
  }
}

// TODO(nipun): Allow for customized behaviors - accept a callback that
// processes the collected stack traces. The default behavior is to write
// stacktraces to stderr.

// The function run by the stack trace service thread. Returns a file
// descriptor, by populating @p, which can be written to request a dump of
// stack trace on stderr. Each request should contain another file
// descriptor, which is closed at the end of servicing the request - this can
// be used by requesters to wait for their request to be serviced.
void RequestProcessor(std::promise<int>* p) {
  std::cout << "Started external stacktrace collection signal processor thread"
            << std::endl;
  int pipe_fd[2];
  // Open the pipe with O_CLOEXEC so that it is not visible to an exec'ed
  // child process.
  if (0 != pipe2(pipe_fd, O_CLOEXEC)) {
    std::cerr << "Failed to create pipe" << std::endl;  // errno, crash.
  }
  // Acknowledge the start of stack trace service thread.
  p->set_value(pipe_fd[1]);
  int64_t request_count = 0;
  while (true) {
    ++request_count;
    int ack_fd;
    auto ret = read(pipe_fd[0], &ack_fd, sizeof(ack_fd));
    if (-1 == ret) {
      std::cerr << "Failed to read stack trace service request"
                << std::endl;  // errno
      continue;
    }
    if (0 == ret) {
      std::cerr << "Received request to terminate stack trace service thread"
                << std::endl;  // errno.
      close(pipe_fd[1]);
      break;
    }
    if (sizeof(ack_fd) != ret) {
      std::cerr
          << "Read partial data of stack trace collection request. Expected "
          << sizeof(ack_fd) << " bytes, got " << ret << " bytes" << std::endl;
      continue;
    }
    DEFER(if (0 != close(ack_fd)) {
      std::cerr << "Failed to ack stack trace requester" << std::endl;  // errno
    });
    // Flush stderr before acking the requester. This is required because some
    // requesters assert the presence of stack traces in stderr, after they
    // receive the ack.
    DEFER(fflush(stderr));
    fprintf(stderr,
            "=============================================\n"
            "%ld) Stack traces - Start \n"
            "=============================================\n",
            request_count);
    StackTraceCollector collector;
    std::string error;
    auto results = collector.Collect(&error);
    if (results.empty()) {
      std::cerr << "StackTrace collection failed: " << error << std::endl;
    } else {
      const auto& trace = StackTraceCollector::ToPrettyString(results);
      fprintf(stderr, "\n%s\n", trace.c_str());
      fprintf(stderr,
              "============================================\n"
              "%ld) Stack traces - End \n"
              "============================================\n",
              request_count);
    }
  }
}

ExternalHandlerState::ExternalHandlerState() : server_tgid(getpid()) {
  std::promise<int> p;
  auto f = p.get_future();
  auto t = std::thread(RequestProcessor, &p);
  // Stack trace service thread runs for the entire lifetime of the process.
  t.detach();
  // Wait for stack trace service thread to start.
  server_fd = f.get();
}

// Sends signal @signum to thread @tid of process group @pid with payload
// @payload. Returns -1 on a failure and sets errno appropriately
// (see man rt_tgsigqueueinfo). Retuns 0 on success.
int SignalThread(pid_t pid, pid_t tid, uid_t uid, int signum, sigval payload) {
  // The following code is inspired by the implementation of pthread_sigqueue().
  // Note that we can't use pthread_sigqueue() directly, as it requires
  // pthread_t handles.
  siginfo_t info;
  memset(&info, '\0', sizeof(info));
  info.si_signo = signum;
  info.si_code = SI_QUEUE;
  info.si_pid = pid;
  info.si_uid = uid;
  info.si_value = payload;
  // Note that sigqueue() syscall can't be used to direct signal at a precise
  // thread - the kernel is free to deliver such a signal to any thread of that
  // process group. Hence, we use tgsigqueueinfo() instead, which delivers the
  // signal to the exact thread it was directed at.
  return syscall(SYS_rt_tgsigqueueinfo, pid, tid, signum, &info);
}

}  // namespace

auto StackTraceCollector::Collect(std::string* error) -> std::vector<Result> {
  auto tids_v = common::Sysutil::ListThreads();
  std::set<pid_t> init_tids(tids_v.begin(), tids_v.end());
  std::vector<std::unique_ptr<StackTraceForm>> slot;
  // Step 1: Create a pipe on which threads can send acks after they finish
  // writing their stacktrace.
  int pipe_fd[2];
  if (-1 == pipe(pipe_fd)) {
    std::cerr << "Failed to create pipe" << std::endl;  // errno
    error->assign("Internal server error");
    return {};
  }
  DEFER(close(pipe_fd[0]));
  DEFER(close(pipe_fd[1]));
  const auto pid = getpid();
  const auto uid = getuid();
  // Step 2: Signal all threads to write their stack trace in a pre-allocated
  // area. Note that some threads might have died by now, so signalling them
  // will fail. Such failures are noted in @failed_tids.
  std::set<pid_t> failed_tids;
  for (auto tid : init_tids) {
    std::unique_ptr<StackTraceForm> stack(new StackTraceForm(tid, pipe_fd[1]));
    union sigval payload;
    payload.sival_ptr = stack.get();
    // Signaling might fail if the thread is no longer alive.
    auto ret = SignalThread(
        pid, tid, uid, StackTraceSignal::InternalSignum(), payload);
    if (0 != ret) {
      std::cerr << "Unable to signal thread " << tid << std::endl;  // errno
      failed_tids.insert(tid);
    } else {
      slot.push_back(std::move(stack));
    }
  }
  std::set<pid_t> tids;
  STLSetDifference(init_tids, failed_tids, &tids);

  // Step 3: Create a 5 second timer, to perform a bounded wait on acks from
  // threads.
  auto timer_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
  if (timer_fd == -1) {
    std::cerr << "Failed to create timer" << std::endl;  // errno
    error->assign("Failed to create an internal timer");
    return {};
  }
  struct itimerspec time_spec;
  bzero(&time_spec, sizeof(time_spec));
  time_spec.it_value.tv_sec = 5;  // TODO(nipun): Make configurable.
  time_spec.it_value.tv_nsec = 0;
  time_spec.it_interval.tv_sec = 0;
  time_spec.it_interval.tv_nsec = 0;
  if (-1 == timerfd_settime(timer_fd, 0, &time_spec, nullptr)) {
    std::cerr << "Failed to set timer" << std::endl;  // errno
    error->assign("Failed to set an internal timer");
    return {};
  }
  DEFER(close(timer_fd));

  // Step 4: Wait for all the acks, timing out after 5 seconds.
  int acks = 0;
  while (acks < static_cast<int>(tids.size())) {
    // Set operations on pipe_fd[0] to be non-blocking. This is important if the
    // select() on this fd returns, but the subsequent read block. This behaviour
    // is possible in exceptional cases, and when occurs would cause the entire
    // process to become non-responsive.
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(pipe_fd[0], &read_fds);
    FD_SET(timer_fd, &read_fds);
    auto max_fd = std::max(pipe_fd[0], timer_fd) + 1;
    auto ret = select(max_fd, &read_fds, nullptr, nullptr, nullptr);
    if (ret == -1) {
      std::cerr << "select(...) failed, will try again" << std::endl;  // errno
    } else if (ret == 0) {
      // We should never encounter this case as we use an infinite timeout in
      // the select syscall.
      std::cerr << "No file descriptors ready, will try again"
                << std::endl;  // errno
    } else if (FD_ISSET(timer_fd, &read_fds)) {
      std::cerr << "Failed to get all (" << tids.size()
                << ") the stacktrace acks within timeout. Got only " << acks
                << std::endl;  // errno
      error->assign("Failed to get all (" + std::to_string(tids.size()) +
                    ") stacktraces within timeout. Got only " +
                    std::to_string(acks));
      return {};
    } else if (FD_ISSET(pipe_fd[0], &read_fds)) {
      char ch;
      auto num_read = read(pipe_fd[0], &ch, sizeof(ch));
      if (-1 == num_read) {
        std::cerr << "Failed to read from pipe" << std::endl;
      } else if (sizeof(ch) != num_read) {
        std::cerr << "Read unexpected number of bytes. Expected: " << sizeof(ch)
                  << ", got: " << num_read << std::endl;
      } else {
        ++acks;
      }
    }
  }

  // Step 6: All acks have been received, post-process the data communicated by
  // threads and produce the final result.
  struct StackComparator {
    bool operator()(StackTraceForm* a, StackTraceForm* b) const {
      const auto& astack = a->stack();
      const auto& bstack = b->stack();
      if (astack.depth != bstack.depth) {
        return astack.depth < bstack.depth;
      }
      for (int i = 0; i < astack.depth; ++i) {
        if (astack.address[i] != bstack.address[i]) {
          return astack.address[i] < bstack.address[i];
        }
      }
      return false;
    };
  };

  // Map from a stacktrace to the vector of tids that have the exact same
  // stacktrace.
  std::map<StackTraceForm*, std::vector<pid_t>, StackComparator> unique_traces;
  for (const auto& e : slot) {
    auto it = unique_traces.find(e.get());
    if (it == unique_traces.end()) {
      unique_traces[e.get()].push_back(e->stack().tid);
    } else {
      it->second.push_back(e->stack().tid);
    }
  }

  std::vector<Result> results;
  results.reserve(unique_traces.size());
  for (const auto& e : unique_traces) {
    const auto& stack = e.first->stack();
    Result r;
    r.tids = e.second;
    r.trace = stack;
    results.push_back(r);
  }
  return results;
}

// static
std::string StackTraceCollector::ToPrettyString(const std::vector<Result>& r) {
  std::ostringstream ss;
  for (const auto& e : r) {
    if (e.tids.empty()) {
      ss << "No Threads" << std::endl;
      continue;
    }
    ss << "Threads: ";
    for (int i = 0; i < static_cast<int>(e.tids.size()) - 1; ++i) {
      ss << e.tids[i] << ", ";
    }
    ss << *e.tids.rbegin() << std::endl;
    ss << "Stack trace:" << std::endl;
    e.trace.PrettyPrint(
        [&](const char *str) {
          ss << str;
        });
    ss << std::endl;
  }
  return ss.str();
}

int StackTraceSignal::InternalSignum() { return SIGRTMIN; }

int StackTraceSignal::ExternalSignum() { return SIGRTMIN + 1; }

// static
bool StackTraceSignal::InstallInternalHandler() {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = InternalHandler;
  // Set SA_RESTART so that supported syscalls are automatically restarted if
  // interrupted by the stacktrace collection signal.
  action.sa_flags = SA_RESTART | SA_SIGINFO;
  return 0 == sigaction(StackTraceSignal::InternalSignum(), &action, nullptr);
}

bool StackTraceSignal::InstallExternalHandler() {
  auto state = GetExternalHandlerState();
  if (state.server_fd < 0) {
    std::cerr << "Failed to setup external signal handler" << std::endl;
    return false;
  }

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = ExternalStackTraceSignalHandler;
  // Set SA_RESTART so that supported syscalls are automatically restarted if
  // interrupted by the stacktrace collection signal.
  action.sa_flags = SA_RESTART | SA_SIGINFO;
  return 0 == sigaction(StackTraceSignal::ExternalSignum(), &action, nullptr);
}

}  // namespace threadstacks
