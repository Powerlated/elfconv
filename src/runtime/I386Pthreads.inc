#include <array>
#include <chrono>
#include <condition_variable>
#include <sched.h>
#include <signal.h>
#include <thread>
constexpr unsigned pthread_destructor_iterations = 4;
// Included by I386Imports.cpp: uses its guest ABI, heap, and callback helpers.
namespace {
struct PthreadCall : Call {
  using Call::Call;
  ~PthreadCall() { errno = initial_host_errno; }
};
struct GuestPthread {
  State cpu{};
  pthread_t host{};
  RuntimeManager *runtime = nullptr;
  uint8_t *arena = nullptr;
  uint32_t id = 0, stack = 0, stack_allocation = 0;
  uint32_t entry = 0, argument = 0, result = 0;
  bool owns_stack = false, joining = false, detached = false, finished = false;
};
struct GuestPthreadExit { uint32_t result; };
struct GuestPthreadStart {
  std::shared_ptr<GuestPthread> thread;
};
std::mutex pthread_registry_mutex;
std::map<uint32_t, std::shared_ptr<GuestPthread>> guest_pthreads;
uint32_t next_pthread_id = 2;
thread_local uint32_t current_pthread_id = 1;
constexpr uint32_t pthread_stack_size = 8 * 1024 * 1024;
constexpr uint32_t linux_i386_pthread_stack_min = 16 * 1024;

struct GuestPthreadKey {
  uint32_t generation = 0;
  uint32_t destructor = 0;
  bool active = false;
};
struct GuestSpecificValue {
  uint32_t generation, value;
};
constexpr size_t guest_pthread_key_count = 1024;
using GuestPthreadKeyTable = std::array<GuestPthreadKey, guest_pthread_key_count>;
std::mutex pthread_key_mutex;
std::map<RuntimeManager *, GuestPthreadKeyTable> guest_pthread_keys;
thread_local std::map<RuntimeManager *, std::map<uint32_t, GuestSpecificValue>>
    current_pthread_values;
pthread_t main_host_pthread{};
bool main_host_pthread_valid = false;

void drain_pthread_key_destructors(GuestPthread &thread);

void *run_guest_pthread(void *argument) {
  std::unique_ptr<GuestPthreadStart> start(static_cast<GuestPthreadStart *>(argument));
  const auto thread = start->thread;
  current_pthread_id = thread->id;
  CPUState = &thread->cpu;
  try {
    invoke(thread->arena, &thread->cpu, thread->runtime, thread->entry, &thread->argument, 1);
    thread->result = thread->cpu.gpr.rax.dword;
  } catch (const GuestPthreadExit &exit) {
    thread->result = exit.result;
  }
  drain_tls_destructors(thread->arena, &thread->cpu, thread->runtime);
  drain_pthread_key_destructors(*thread);
  __ecv_i386_finalize_thread_tls(thread->arena, &thread->cpu, thread->runtime);
  CPUState = nullptr;
  current_pthread_id = 0;

  uint32_t stack_to_release = 0;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    thread->finished = true;
    if (thread->detached) {
      auto found = guest_pthreads.find(thread->id);
      if (found != guest_pthreads.end() && found->second == thread)
        guest_pthreads.erase(found);
      if (thread->owns_stack) stack_to_release = thread->stack_allocation;
      thread->owns_stack = false;
    }
  }
  release(stack_to_release);
  return nullptr;
}

void remember_main_host_pthread() {
  if (current_pthread_id != 1) return;
  std::lock_guard<std::mutex> lock(pthread_registry_mutex);
  if (!main_host_pthread_valid) {
    main_host_pthread = pthread_self();
    main_host_pthread_valid = true;
  }
}

int find_host_pthread(uint32_t id, pthread_t &host,
                      std::shared_ptr<GuestPthread> &keep_alive) {
  if (id == current_pthread_id) {
    host = pthread_self();
    return 0;
  }
  std::lock_guard<std::mutex> lock(pthread_registry_mutex);
  if (id == 1 && main_host_pthread_valid) {
    host = main_host_pthread;
    return 0;
  }
  auto found = guest_pthreads.find(id);
  if (found == guest_pthreads.end()) return ESRCH;
  keep_alive = found->second;
  host = keep_alive->host;
  return 0;
}

void drain_pthread_key_destructors(GuestPthread &thread) {
  struct Callback {
    uint32_t key, generation, function, value;
  };
  for (unsigned iteration = 0; iteration < pthread_destructor_iterations; ++iteration) {
    std::vector<Callback> callbacks;
    {
      std::lock_guard<std::mutex> lock(pthread_key_mutex);
      auto values = current_pthread_values.find(thread.runtime);
      auto keys = guest_pthread_keys.find(thread.runtime);
      if (values != current_pthread_values.end() && keys != guest_pthread_keys.end()) {
        for (auto &entry : values->second) {
          const uint32_t key = entry.first;
          auto &value = entry.second;
          if (value.value && key < keys->second.size()) {
            const auto &slot = keys->second[key];
            if (slot.active && slot.generation == value.generation && slot.destructor) {
              callbacks.push_back({key, value.generation, slot.destructor, value.value});
              value.value = 0;
            }
          }
        }
      }
    }
    if (callbacks.empty()) break;
    for (const auto &callback : callbacks) {
      bool active = false;
      {
        std::lock_guard<std::mutex> lock(pthread_key_mutex);
        auto keys = guest_pthread_keys.find(thread.runtime);
        if (keys != guest_pthread_keys.end()) {
          const auto &slot = keys->second[callback.key];
          active = slot.active && slot.generation == callback.generation;
        }
      }
      if (active)
        invoke(thread.arena, &thread.cpu, thread.runtime, callback.function, &callback.value, 1);
    }
  }
  current_pthread_values.erase(thread.runtime);
}

// Linux i386 glibc NPTL pthread_attr_t is 36 bytes: schedparam@0,
// schedpolicy@4, flags@8, guardsize@12, stackaddr@16, stacksize@20.
constexpr uint32_t guest_attr_detached = 0x0001;
constexpr uint32_t guest_attr_explicit_schedule = 0x0002;
constexpr uint32_t guest_attr_process_scope = 0x0004;
constexpr uint32_t guest_attr_stack_address = 0x0008;
struct GuestThreadAttributes {
  uint32_t detach_state = PTHREAD_CREATE_JOINABLE;
  int32_t scheduling_policy = SCHED_OTHER;
  int32_t priority = 0;
  uint32_t inherit_scheduling = PTHREAD_INHERIT_SCHED;
  uint32_t scope = PTHREAD_SCOPE_SYSTEM;
  uint32_t stack_address_set = 0;
  uint32_t stack_address = 0;  // NPTL stores the high address for a down-growing stack.
  uint32_t stack_size = 0;
};

int read_thread_attributes(uint8_t *arena, uint32_t address,
                           GuestThreadAttributes &attributes) {
  if (!address) return 0;
  const auto *object = static_cast<const uint8_t *>(guest(arena, address, 36));
  uint32_t flags;
  memcpy(&attributes.priority, object, 4);
  memcpy(&attributes.scheduling_policy, object + 4, 4);
  memcpy(&flags, object + 8, 4);
  memcpy(&attributes.stack_address, object + 16, 4);
  memcpy(&attributes.stack_size, object + 20, 4);
  attributes.detach_state = (flags & guest_attr_detached)
      ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
  attributes.inherit_scheduling = (flags & guest_attr_explicit_schedule)
      ? PTHREAD_EXPLICIT_SCHED : PTHREAD_INHERIT_SCHED;
  attributes.scope = (flags & guest_attr_process_scope)
      ? PTHREAD_SCOPE_PROCESS : PTHREAD_SCOPE_SYSTEM;
  attributes.stack_address_set = (flags & guest_attr_stack_address) != 0;
  return 0;
}

struct GuestMutex {
  pthread_mutex_t host;
  bool initialized = false;
  ~GuestMutex() { if (initialized) pthread_mutex_destroy(&host); }
};
struct GuestCondition {
  pthread_cond_t host;
  bool initialized = false;
  ~GuestCondition() { if (initialized) pthread_cond_destroy(&host); }
};
struct GuestBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  unsigned threshold = 0, arrived = 0, generation = 0;
};
struct GuestOnce { std::once_flag flag; };
std::map<uint32_t, std::shared_ptr<GuestBarrier>> guest_barriers;
std::map<std::pair<RuntimeManager *, uint32_t>, std::shared_ptr<GuestOnce>> guest_once;
std::mutex synchronization_registry_mutex;
std::map<uint32_t, std::shared_ptr<GuestMutex>> guest_mutexes;
std::map<uint32_t, std::shared_ptr<GuestCondition>> guest_conditions;

int initialize_mutex(GuestMutex &mutex, uint32_t kind) {
  // Linux i386 kinds: normal=0, recursive=1, error-check=2, adaptive=3.
  if (kind > 3) return ENOTSUP;  // Robust/process-shared mutexes need a different ABI.
  pthread_mutexattr_t attributes;
  int error = pthread_mutexattr_init(&attributes);
  if (error) return error;
  const int types[] = {PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE,
                       PTHREAD_MUTEX_ERRORCHECK, PTHREAD_MUTEX_NORMAL};
  error = pthread_mutexattr_settype(&attributes, types[kind]);
  if (!error) error = pthread_mutex_init(&mutex.host, &attributes);
  pthread_mutexattr_destroy(&attributes);
  mutex.initialized = !error;
  return error;
}

std::shared_ptr<GuestMutex> find_mutex(uint8_t *arena, uint32_t address, int &error) {
  auto *object = static_cast<uint8_t *>(guest(arena, address, 24));
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  auto found = guest_mutexes.find(address);
  if (found != guest_mutexes.end()) return found->second;
  uint32_t kind;
  memcpy(&kind, object + 12, 4);
  auto mutex = std::make_shared<GuestMutex>();
  error = initialize_mutex(*mutex, kind);
  if (error) return {};
  guest_mutexes.emplace(address, mutex);
  return mutex;
}

std::shared_ptr<GuestCondition> find_condition(uint8_t *arena, uint32_t address, int &error) {
  auto *object = static_cast<const uint8_t *>(guest(arena, address, 48));
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  auto found = guest_conditions.find(address);
  if (found != guest_conditions.end()) return found->second;
  // A zero-filled Linux i386 condition variable is the static initializer.
  for (unsigned i = 0; i < 48; ++i) if (object[i]) { error = EINVAL; return {}; }
  auto condition = std::make_shared<GuestCondition>();
  error = pthread_cond_init(&condition->host, nullptr);
  if (error) return {};
  condition->initialized = true;
  guest_conditions.emplace(address, condition);
  return condition;
}
int prepare_host_thread_attributes(const GuestThreadAttributes &attributes,
                                  uint32_t stack_size, pthread_attr_t &host_attributes) {
  int error = pthread_attr_init(&host_attributes);
  if (error) return error;
  if (attributes.detach_state != PTHREAD_CREATE_JOINABLE &&
      attributes.detach_state != PTHREAD_CREATE_DETACHED) error = EINVAL;
  if (!error && attributes.inherit_scheduling != PTHREAD_INHERIT_SCHED &&
      attributes.inherit_scheduling != PTHREAD_EXPLICIT_SCHED) error = EINVAL;
  if (!error && attributes.scope != PTHREAD_SCOPE_SYSTEM) error = ENOTSUP;
  if (!error) error = pthread_attr_setstacksize(&host_attributes, stack_size);
  if (!error)
    error = pthread_attr_setdetachstate(&host_attributes, int(attributes.detach_state));
  if (!error && attributes.inherit_scheduling == PTHREAD_EXPLICIT_SCHED) {
#ifdef __EMSCRIPTEN__
    if (attributes.scheduling_policy == SCHED_OTHER && attributes.priority == 0) {
      error = 0;
    } else if (attributes.scheduling_policy == SCHED_FIFO ||
               attributes.scheduling_policy == SCHED_RR) {
      error = attributes.priority < 1 || attributes.priority > 99 ? EINVAL : ENOTSUP;
    } else {
      error = EINVAL;
    }
#else
    sched_param parameter{};
    parameter.sched_priority = attributes.priority;
    error = pthread_attr_setinheritsched(&host_attributes, PTHREAD_EXPLICIT_SCHED);
    if (!error) error = pthread_attr_setschedpolicy(&host_attributes, attributes.scheduling_policy);
    if (!error) error = pthread_attr_setschedparam(&host_attributes, &parameter);
#endif
  }
  if (error) pthread_attr_destroy(&host_attributes);
  return error;
}

}  // namespace

IMPORT(pthread_create) {
  PthreadCall call(arena, state, runtime);
  auto *output = guest(arena, call.word(0), 4);
  GuestThreadAttributes attributes;
  int error = read_thread_attributes(arena, call.word(1), attributes);
  auto function = std::lower_bound(runtime->addr_funptr_srt_list.begin(),
      runtime->addr_funptr_srt_list.end(), call.word(2),
      [](const auto &item, uint32_t address) { return item.first < address; });
  if (!error && (function == runtime->addr_funptr_srt_list.end() ||
                 function->first != call.word(2))) error = EINVAL;
  uint32_t stack_size = attributes.stack_size ? attributes.stack_size : pthread_stack_size;
  if (!error && (stack_size < linux_i386_pthread_stack_min ||
                 attributes.stack_address_set > 1)) error = EINVAL;
  pthread_attr_t host_attributes;
  if (!error) {
    error = prepare_host_thread_attributes(attributes, stack_size, host_attributes);
  }
  if (error) { call.result(host_errno_to_i386(error)); return; }

  auto thread = std::make_shared<GuestPthread>();
  thread->owns_stack = !attributes.stack_address_set;
  if (attributes.stack_address_set) {
    const uint64_t end = attributes.stack_address;
    if (end > MEMORY_ARENA_SIZE || end < stack_size ||
        end - stack_size < NULL_GUARD_SIZE) {
      pthread_attr_destroy(&host_attributes);
      call.result(host_errno_to_i386(EINVAL));
      return;
    }
    thread->stack = uint32_t(end - stack_size);
    thread->stack_allocation = 0;
  } else {
    thread->stack_allocation = allocate(stack_size);
    if (!thread->stack_allocation) {
      pthread_attr_destroy(&host_attributes);
      call.result(host_errno_to_i386(EAGAIN));
      return;
    }
    thread->stack = thread->stack_allocation;
  }
  thread->arena = arena;
  thread->runtime = runtime;
  thread->entry = call.word(2);
  thread->argument = call.word(3);
  thread->detached = attributes.detach_state == PTHREAD_CREATE_DETACHED;
  thread->cpu.seg = state->seg;
  thread->cpu.rflag.flat = 0x202;
  thread->cpu.x87.fxsave.cwd = state->x87.fxsave.cwd;
  thread->cpu.x87.fxsave.swd.flat = state->x87.fxsave.swd.flat & ~(7u << 11);
  thread->cpu.x87.fxsave.mxcsr = state->x87.fxsave.mxcsr;
  thread->cpu.sw = state->sw;
  thread->cpu.gpr.rsp.dword =
      uint32_t(uint64_t(thread->stack) + stack_size) & ~15u;

  remember_main_host_pthread();
  auto start = std::make_unique<GuestPthreadStart>();
  start->thread = thread;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    if (!next_pthread_id) error = EAGAIN;
    else {
      const uint32_t id = next_pthread_id++;
      thread->id = id;
      guest_pthreads.emplace(id, thread);
      error = pthread_create(&thread->host, &host_attributes, run_guest_pthread, start.get());
      if (error) guest_pthreads.erase(id);
      else {
        start.release();
        memcpy(output, &id, 4);
      }
    }
  }
  pthread_attr_destroy(&host_attributes);
  if (error) {
    if (thread->owns_stack) release(thread->stack_allocation);
    thread->owns_stack = false;
  }
  call.result(host_errno_to_i386(error));
}

IMPORT(pthread_attr_init) {
  PthreadCall call(arena, state, runtime);
  auto *attribute = static_cast<uint8_t *>(guest(arena, call.word(0), 36));
  memset(attribute, 0, 36);
  const uint32_t guard_size = 4096;
  memcpy(attribute + 12, &guard_size, 4);
  call.result(0);
}
IMPORT(pthread_attr_setstacksize) {
  PthreadCall call(arena, state, runtime);
  const uint32_t size = call.word(1);
  if (size < linux_i386_pthread_stack_min) {
    call.result(host_errno_to_i386(EINVAL));
    return;
  }
  auto *attribute = static_cast<uint8_t *>(guest(arena, call.word(0), 36));
  memcpy(attribute + 20, &size, 4);
  call.result(0);
}

IMPORT(pthread_self) {
  PthreadCall call(arena, state, runtime);
  remember_main_host_pthread();
  call.result(current_pthread_id);
}
IMPORT(pthread_equal) { PthreadCall call(arena, state, runtime); call.result(call.word(0) == call.word(1)); }
IMPORT(pthread_join) {
  PthreadCall call(arena, state, runtime);
  const uint32_t id = call.word(0);
  void *output = call.word(1) ? guest(arena, call.word(1), 4) : nullptr;
  if (id == current_pthread_id) { call.result(host_errno_to_i386(EDEADLK)); return; }
  std::shared_ptr<GuestPthread> thread;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    auto found = guest_pthreads.find(id);
    if (found == guest_pthreads.end()) { call.result(host_errno_to_i386(ESRCH)); return; }
    thread = found->second;
    if (thread->joining || thread->detached) {
      call.result(host_errno_to_i386(EINVAL));
      return;
    }
    thread->joining = true;
  }
  // Never hold the registry lock while waiting for a guest callback to finish.
  const int error = pthread_join(thread->host, nullptr);
  uint32_t stack_to_release = 0;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    if (error) thread->joining = false;
    else {
      if (output) memcpy(output, &thread->result, 4);
      auto found = guest_pthreads.find(id);
      if (found != guest_pthreads.end() && found->second == thread)
        guest_pthreads.erase(found);
      if (thread->owns_stack) stack_to_release = thread->stack_allocation;
      thread->owns_stack = false;
    }
  }
  release(stack_to_release);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_detach) {
  PthreadCall call(arena, state, runtime);
  const uint32_t id = call.word(0);
  remember_main_host_pthread();
  std::shared_ptr<GuestPthread> thread;
  uint32_t stack_to_release = 0;
  int error = 0;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    auto found = guest_pthreads.find(id);
    if (id == 1) {
      error = main_host_pthread_valid ? pthread_detach(main_host_pthread) : ESRCH;
    } else if (found == guest_pthreads.end()) error = ESRCH;
    else {
      thread = found->second;
      if (thread->joining || thread->detached) error = EINVAL;
      else {
        error = pthread_detach(thread->host);
        if (!error) {
          thread->detached = true;
          if (thread->finished) {
            guest_pthreads.erase(found);
            if (thread->owns_stack) stack_to_release = thread->stack_allocation;
            thread->owns_stack = false;
          }
        }
      }
    }
  }
  release(stack_to_release);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_getschedparam) {
  PthreadCall call(arena, state, runtime);
  pthread_t host{};
  std::shared_ptr<GuestPthread> keep_alive;
  int error = find_host_pthread(call.word(0), host, keep_alive);
  int policy = 0;
  sched_param parameter{};
#ifdef __EMSCRIPTEN__
  if (!error) {
    policy = SCHED_OTHER;
    parameter.sched_priority = 0;
  }
#else
  if (!error) error = pthread_getschedparam(host, &policy, &parameter);
#endif
  if (!error) {
    memcpy(guest(arena, call.word(1), 4), &policy, 4);
    memcpy(guest(arena, call.word(2), 4), &parameter.sched_priority, 4);
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_setschedparam) {
  PthreadCall call(arena, state, runtime);
  const int policy = int32_t(call.word(1));
  int priority;
  memcpy(&priority, guest(arena, call.word(2), 4), 4);
  sched_param parameter{};
  parameter.sched_priority = priority;
  pthread_t host{};
  std::shared_ptr<GuestPthread> keep_alive;
  int error = find_host_pthread(call.word(0), host, keep_alive);
#ifdef __EMSCRIPTEN__
  if (!error) {
    if (policy == SCHED_OTHER) error = priority == 0 ? 0 : EINVAL;
    else if (policy == SCHED_FIFO || policy == SCHED_RR)
      error = priority < 1 || priority > 99 ? EINVAL : ENOTSUP;
    else error = EINVAL;
  }
#else
  if (!error) error = pthread_setschedparam(host, policy, &parameter);
#endif
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_kill) {
  PthreadCall call(arena, state, runtime);
  int maximum_signal = 64;
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  maximum_signal = SIGRTMAX;
#endif
  const int signal_number = int32_t(call.word(1));
  int error = signal_number < 0 || signal_number > maximum_signal ? EINVAL : 0;
  pthread_t host{};
  std::shared_ptr<GuestPthread> keep_alive;
  if (!error) error = find_host_pthread(call.word(0), host, keep_alive);
  if (!error) error = pthread_kill(host, signal_number);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_setname_np) {
  PthreadCall call(arena, state, runtime);
  pthread_t host{};
  std::shared_ptr<GuestPthread> keep_alive;
  int error = find_host_pthread(call.word(0), host, keep_alive);
  if (!error) {
    const char *name = text(arena, call.word(1));
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    error = pthread_setname_np(host, name);
#else
    error = ENOTSUP;
#endif
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_yield) {
  PthreadCall call(arena, state, runtime);
  call.result(host_errno_to_i386(sched_yield()));
}
IMPORT(pthread_key_create) {
  PthreadCall call(arena, state, runtime);
  const uint32_t destructor = call.word(1);
  if (destructor) {
    auto function = std::lower_bound(runtime->addr_funptr_srt_list.begin(),
        runtime->addr_funptr_srt_list.end(), destructor,
        [](const auto &item, uint32_t address) { return item.first < address; });
    if (function == runtime->addr_funptr_srt_list.end() || function->first != destructor) {
      call.result(host_errno_to_i386(EINVAL));
      return;
    }
  }
  uint32_t key = 0;
  int error = EAGAIN;
  {
    std::lock_guard<std::mutex> lock(pthread_key_mutex);
    auto &keys = guest_pthread_keys[runtime];
    for (uint32_t i = 0; i < keys.size(); ++i) {
      auto &slot = keys[i];
      if (slot.active || slot.generation == UINT32_MAX) continue;
      ++slot.generation;
      slot.destructor = destructor;
      slot.active = true;
      key = i;
      error = 0;
      break;
    }
  }
  if (!error) memcpy(guest(arena, call.word(0), 4), &key, 4);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_key_delete) {
  PthreadCall call(arena, state, runtime);
  const uint32_t key = call.word(0);
  int error = EINVAL;
  {
    std::lock_guard<std::mutex> lock(pthread_key_mutex);
    auto keys = guest_pthread_keys.find(runtime);
    if (keys != guest_pthread_keys.end() && key < keys->second.size() &&
        keys->second[key].active) {
      keys->second[key].active = false;
      keys->second[key].destructor = 0;
      auto values = current_pthread_values.find(runtime);
      if (values != current_pthread_values.end()) {
        values->second.erase(key);
        if (values->second.empty()) current_pthread_values.erase(values);
      }
      error = 0;
    }
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_getspecific) {
  PthreadCall call(arena, state, runtime);
  const uint32_t key = call.word(0);
  uint32_t result = 0;
  {
    std::lock_guard<std::mutex> lock(pthread_key_mutex);
    auto keys = guest_pthread_keys.find(runtime);
    if (keys != guest_pthread_keys.end() && key < keys->second.size() &&
        keys->second[key].active) {
      auto values = current_pthread_values.find(runtime);
      if (values != current_pthread_values.end()) {
        auto value = values->second.find(key);
        if (value != values->second.end() &&
            value->second.generation == keys->second[key].generation)
          result = value->second.value;
      }
    }
  }
  call.result(result);
}
IMPORT(pthread_setspecific) {
  PthreadCall call(arena, state, runtime);
  const uint32_t key = call.word(0), value = call.word(1);
  int error = EINVAL;
  {
    std::lock_guard<std::mutex> lock(pthread_key_mutex);
    auto keys = guest_pthread_keys.find(runtime);
    if (keys != guest_pthread_keys.end() && key < keys->second.size() &&
        keys->second[key].active) {
      if (value) {
        current_pthread_values[runtime][key] =
            GuestSpecificValue{keys->second[key].generation, value};
      } else {
        auto values = current_pthread_values.find(runtime);
        if (values != current_pthread_values.end()) {
          values->second.erase(key);
          if (values->second.empty()) current_pthread_values.erase(values);
        }
      }
      error = 0;
    }
  }
  call.result(host_errno_to_i386(error));
}


// glibc nptl/pthread_mutexattr_{init,settype,destroy}.c: one i386 word;
// settype retains robust/shared/protocol/priority flags; destroy owns no resources.
IMPORT(pthread_mutexattr_init) {
  PthreadCall call(arena, state, runtime);
  const uint32_t kind = 0;
  memcpy(guest(arena, call.word(0), 4), &kind, 4);
  call.result(0);
}
IMPORT(pthread_mutexattr_settype) {
  PthreadCall call(arena, state, runtime);
  const uint32_t kind = call.word(1);
  if (kind > 3) { call.result(host_errno_to_i386(EINVAL)); return; }
  auto *attribute = guest(arena, call.word(0), 4);
  uint32_t value;
  memcpy(&value, attribute, 4);
  value = (value & 0xf0fff000u) | kind;
  memcpy(attribute, &value, 4);
  call.result(0);
}
IMPORT(pthread_mutexattr_destroy) {
  PthreadCall call(arena, state, runtime);
  call.result(0);
}
IMPORT(pthread_mutex_init) {
  PthreadCall call(arena, state, runtime);
  const uint32_t address = call.word(0);
  auto *object = static_cast<uint8_t *>(guest(arena, address, 24));
  uint32_t kind = 0;
  if (call.word(1)) memcpy(&kind, guest(arena, call.word(1), 4), 4);
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  if (guest_mutexes.count(address)) { call.result(host_errno_to_i386(EBUSY)); return; }
  auto mutex = std::make_shared<GuestMutex>();
  int error = initialize_mutex(*mutex, kind);
  if (!error) {
    memset(object, 0, 24);
    memcpy(object + 12, &kind, 4);
    guest_mutexes.emplace(address, std::move(mutex));
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_mutex_destroy) {
  PthreadCall call(arena, state, runtime);
  int error = 0;
  auto mutex = find_mutex(arena, call.word(0), error);
  if (mutex) {
    std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
    if (mutex.use_count() > 2) error = EBUSY;
    else error = pthread_mutex_destroy(&mutex->host);
    if (!error) {
      mutex->initialized = false;
      guest_mutexes.erase(call.word(0));
      const uint32_t destroyed = UINT32_MAX;
      memcpy(guest(arena, call.word(0) + 12, 4), &destroyed, 4);
    }
  }
  call.result(host_errno_to_i386(error));
}
#define MUTEX_OPERATION(name) \
IMPORT(name) { \
  PthreadCall call(arena, state, runtime); \
  int error = 0; \
  auto mutex = find_mutex(arena, call.word(0), error); \
  if (mutex) error = name(&mutex->host); \
  call.result(host_errno_to_i386(error)); \
}
MUTEX_OPERATION(pthread_mutex_lock)
MUTEX_OPERATION(pthread_mutex_trylock)
MUTEX_OPERATION(pthread_mutex_unlock)
#undef MUTEX_OPERATION
IMPORT(pthread_cond_init) {
  PthreadCall call(arena, state, runtime);
  const uint32_t address = call.word(0);
  auto *object = guest(arena, address, 48);
  uint32_t value = 0;
  if (call.word(1)) memcpy(&value, guest(arena, call.word(1), 4), 4);
  if (value & ~3u) { call.result(host_errno_to_i386(EINVAL)); return; }
  if (value & 1u) { call.result(host_errno_to_i386(ENOTSUP)); return; }
  const clockid_t clock = (value & 2u) ? CLOCK_MONOTONIC : CLOCK_REALTIME;

  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  if (guest_conditions.count(address)) { call.result(host_errno_to_i386(EBUSY)); return; }
  auto condition = std::make_shared<GuestCondition>();
  pthread_condattr_t attributes;
  int error = pthread_condattr_init(&attributes);
  const bool attributes_initialized = !error;
  if (!error && clock == CLOCK_MONOTONIC)
    error = pthread_condattr_setclock(&attributes, clock);
  if (!error) error = pthread_cond_init(&condition->host, &attributes);
  if (!error) {
    condition->initialized = true;
    memset(object, 0, 48);
    guest_conditions.emplace(address, std::move(condition));
  }
  if (attributes_initialized) pthread_condattr_destroy(&attributes);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_cond_destroy) {
  PthreadCall call(arena, state, runtime);
  int error = 0;
  auto condition = find_condition(arena, call.word(0), error);
  if (condition) {
    std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
    if (condition.use_count() > 2) error = EBUSY;
    else error = pthread_cond_destroy(&condition->host);
    if (!error) {
      condition->initialized = false;
      guest_conditions.erase(call.word(0));
      memset(guest(arena, call.word(0), 48), 0xff, 48);
    }
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_cond_wait) {
  PthreadCall call(arena, state, runtime);
  int error = 0;
  auto condition = find_condition(arena, call.word(0), error);
  auto mutex = error ? std::shared_ptr<GuestMutex>() : find_mutex(arena, call.word(1), error);
  if (condition && mutex) error = pthread_cond_wait(&condition->host, &mutex->host);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_cond_timedwait) {
  PthreadCall call(arena, state, runtime);
  int error = 0;
  auto condition = find_condition(arena, call.word(0), error);
  auto mutex = error ? std::shared_ptr<GuestMutex>() : find_mutex(arena, call.word(1), error);
  int32_t guest_time[2];
  if (!error) memcpy(guest_time, guest(arena, call.word(2), sizeof(guest_time)), sizeof(guest_time));
  if (!error && (guest_time[1] < 0 || guest_time[1] >= 1000000000)) error = EINVAL;
  if (!error && condition && mutex) {
    const timespec deadline = {static_cast<time_t>(guest_time[0]), guest_time[1]};
    error = pthread_cond_timedwait(&condition->host, &mutex->host, &deadline);
  }
  call.result(host_errno_to_i386(error));
}
#define CONDITION_OPERATION(name) \
IMPORT(name) { \
  PthreadCall call(arena, state, runtime); \
  int error = 0; \
  auto condition = find_condition(arena, call.word(0), error); \
  if (condition) error = name(&condition->host); \
  call.result(host_errno_to_i386(error)); \
}
CONDITION_OPERATION(pthread_cond_signal)
CONDITION_OPERATION(pthread_cond_broadcast)
#undef CONDITION_OPERATION
IMPORT(pthread_barrier_init) {
  PthreadCall call(arena, state, runtime);
  const uint32_t address = call.word(0), count = call.word(2);
  if (!count) { call.result(host_errno_to_i386(EINVAL)); return; }
  if (call.word(1)) {
    uint32_t flags;
    memcpy(&flags, guest(arena, call.word(1), 4), 4);
    if (flags) { call.result(host_errno_to_i386(ENOTSUP)); return; }
  }
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  if (guest_barriers.count(address)) { call.result(host_errno_to_i386(EBUSY)); return; }
  guest_barriers.emplace(address, std::make_shared<GuestBarrier>());
  guest_barriers[address]->threshold = count;
  memset(guest(arena, address, 20), 0, 20);
  call.result(0);
}
IMPORT(pthread_barrier_wait) {
  PthreadCall call(arena, state, runtime);
  std::shared_ptr<GuestBarrier> barrier;
  {
    std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
    auto found = guest_barriers.find(call.word(0));
    if (found == guest_barriers.end()) { call.result(host_errno_to_i386(EINVAL)); return; }
    barrier = found->second;
  }
  std::unique_lock<std::mutex> lock(barrier->mutex);
  const unsigned generation = barrier->generation;
  if (++barrier->arrived == barrier->threshold) {
    barrier->arrived = 0;
    ++barrier->generation;
    lock.unlock();
    barrier->condition.notify_all();
    call.result(uint32_t(PTHREAD_BARRIER_SERIAL_THREAD));
    return;
  }
  barrier->condition.wait(lock, [&] { return barrier->generation != generation; });
  call.result(0);
}
IMPORT(pthread_barrier_destroy) {
  PthreadCall call(arena, state, runtime);
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  auto found = guest_barriers.find(call.word(0));
  if (found == guest_barriers.end()) { call.result(host_errno_to_i386(EINVAL)); return; }
  {
    std::lock_guard<std::mutex> barrier_lock(found->second->mutex);
    if (found->second->arrived) { call.result(host_errno_to_i386(EBUSY)); return; }
  }
  guest_barriers.erase(found);
  memset(guest(arena, call.word(0), 20), 0xff, 20);
  call.result(0);
}
IMPORT(pthread_once) {
  PthreadCall call(arena, state, runtime);
  const uint32_t address = call.word(0), initializer = call.word(1);
  std::shared_ptr<GuestOnce> once;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    auto &slot = guest_once[{runtime, address}];
    if (!slot) slot = std::make_shared<GuestOnce>();
    once = slot;
  }
  std::call_once(once->flag, [&] {
    invoke(arena, state, runtime, initializer, nullptr, 0);
    const uint32_t complete = 1;
    memcpy(guest(arena, address, 4), &complete, 4);
  });
  call.result(0);
}
IMPORT(pthread_sigmask) {
  PthreadCall call(arena, state, runtime);
  sigset_t set, previous;
  sigset_t *set_pointer = nullptr, *previous_pointer = nullptr;
  if (call.word(1)) {
    memset(&set, 0, sizeof(set));
    memcpy(&set, guest(arena, call.word(1), std::min(sizeof(set), size_t(128))), sizeof(set));
    set_pointer = &set;
  }
  if (call.word(2)) previous_pointer = &previous;
  const int error = pthread_sigmask(int32_t(call.word(0)), set_pointer, previous_pointer);
  if (!error && previous_pointer) {
    uint8_t guest_set[128] = {};
    memcpy(guest_set, &previous, sizeof(previous));
    memcpy(guest(arena, call.word(2), sizeof(guest_set)), guest_set, sizeof(guest_set));
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_getcpuclockid) {
  PthreadCall call(arena, state, runtime);
#ifdef __EMSCRIPTEN__
  call.result(host_errno_to_i386(ENOTSUP));
#else
  pthread_t thread{};
  std::shared_ptr<GuestPthread> keep_alive;
  int error = find_host_pthread(call.word(0), thread, keep_alive);
  clockid_t clock;
  if (!error) error = pthread_getcpuclockid(thread, &clock);
  if (!error && call.word(1)) {
    const int32_t guest_clock = static_cast<int32_t>(clock);
    memcpy(guest(arena, call.word(1), sizeof(guest_clock)), &guest_clock, sizeof(guest_clock));
  }
  call.result(host_errno_to_i386(error));
#endif
}
IMPORT(pthread_exit) {
  PthreadCall call(arena, state, runtime);
  if (current_pthread_id == 1)
    elfconv_runtime_error("pthread_exit from the primary converted thread is unsupported.\n");
  throw GuestPthreadExit{call.word(0)};
}
IMPORT(pthread_mutex_timedlock) {
  PthreadCall call(arena, state, runtime);
  int error = 0;
  auto mutex = find_mutex(arena, call.word(0), error);
  if (!error) {
    int32_t time[2];
    memcpy(time, guest(arena, call.word(1), sizeof(time)), sizeof(time));
    if (time[1] < 0 || time[1] >= 1000000000) error = EINVAL;
    else {
      const auto deadline = std::chrono::system_clock::time_point(
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::seconds(time[0]) + std::chrono::nanoseconds(time[1])));
      do {
        error = pthread_mutex_trylock(&mutex->host);
        if (error != EBUSY) break;
        if (std::chrono::system_clock::now() >= deadline) {
          error = ETIMEDOUT; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (true);
    }
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_getaffinity_np) {
  PthreadCall call(arena, state, runtime);
  errno = ENOSYS; call.result(host_errno_to_i386(ENOSYS));
}
IMPORT(pthread_setaffinity_np) {
  PthreadCall call(arena, state, runtime);
  errno = ENOSYS; call.result(host_errno_to_i386(ENOSYS));
}
