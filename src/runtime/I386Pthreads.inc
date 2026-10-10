// Included by I386Imports.cpp: uses its guest ABI, heap, and callback helpers.
namespace {
struct PthreadCall : Call {
  using Call::Call;
  ~PthreadCall() { errno = initial_host_errno; }
};
struct GuestPthread {
  State cpu{};
  pthread_t host{};
  RuntimeManager *runtime;
  uint8_t *arena;
  uint32_t id, stack, entry, argument, result = 0;
  bool joining = false;
};
std::mutex pthread_registry_mutex;
std::map<uint32_t, std::unique_ptr<GuestPthread>> guest_pthreads;
uint32_t next_pthread_id = 2;
thread_local uint32_t current_pthread_id = 1;
constexpr uint32_t pthread_stack_size = 8 * 1024 * 1024;

void *run_guest_pthread(void *argument) {
  auto &thread = *static_cast<GuestPthread *>(argument);
  current_pthread_id = thread.id;
  CPUState = &thread.cpu;
  __ecv_i386_initialize_thread_tls(thread.arena, &thread.cpu, thread.runtime);
  invoke(thread.arena, &thread.cpu, thread.runtime, thread.entry, &thread.argument, 1);
  thread.result = thread.cpu.gpr.rax.dword;
  __ecv_i386_finalize_thread_tls(thread.arena, &thread.cpu, thread.runtime);
  CPUState = nullptr;
  return nullptr;
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
}  // namespace

IMPORT(pthread_create) {
  PthreadCall call(arena, state);
  auto *output = guest(arena, call.word(0), 4);
  if (call.word(1)) { call.result(host_errno_to_i386(ENOTSUP)); return; }
  auto function = std::lower_bound(runtime->addr_funptr_srt_list.begin(),
      runtime->addr_funptr_srt_list.end(), call.word(2),
      [](const auto &item, uint32_t address) { return item.first < address; });
  if (function == runtime->addr_funptr_srt_list.end() || function->first != call.word(2)) {
    call.result(host_errno_to_i386(EINVAL)); return;
  }
  auto thread = std::make_unique<GuestPthread>();
  thread->stack = allocate(pthread_stack_size);
  if (!thread->stack) { call.result(host_errno_to_i386(EAGAIN)); return; }
  thread->arena = arena;
  thread->runtime = runtime;
  thread->entry = call.word(2);
  thread->argument = call.word(3);
  thread->cpu.seg = state->seg;
  thread->cpu.rflag.flat = 0x202;
  thread->cpu.x87.fxsave.cwd = state->x87.fxsave.cwd;
  thread->cpu.x87.fxsave.swd.flat = state->x87.fxsave.swd.flat & ~(7u << 11);
  thread->cpu.x87.fxsave.mxcsr = state->x87.fxsave.mxcsr;
  thread->cpu.sw = state->sw;
  thread->cpu.gpr.rsp.dword = thread->stack + pthread_stack_size;
  std::lock_guard<std::mutex> lock(pthread_registry_mutex);
  if (!next_pthread_id) {
    release(thread->stack);
    call.result(host_errno_to_i386(EAGAIN)); return;
  }
  const uint32_t id = next_pthread_id++;
  thread->id = id;
  auto *record = thread.get();
  guest_pthreads.emplace(id, std::move(thread));
  const int error = pthread_create(&record->host, nullptr, run_guest_pthread, record);
  if (error) {
    release(record->stack);
    guest_pthreads.erase(id);
  } else memcpy(output, &id, 4);
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_self) { PthreadCall call(arena, state); call.result(current_pthread_id); }
IMPORT(pthread_equal) { PthreadCall call(arena, state); call.result(call.word(0) == call.word(1)); }
IMPORT(pthread_join) {
  PthreadCall call(arena, state);
  const uint32_t id = call.word(0);
  void *output = call.word(1) ? guest(arena, call.word(1), 4) : nullptr;
  if (id == current_pthread_id) { call.result(host_errno_to_i386(EDEADLK)); return; }
  GuestPthread *thread;
  {
    std::lock_guard<std::mutex> lock(pthread_registry_mutex);
    auto found = guest_pthreads.find(id);
    if (found == guest_pthreads.end()) { call.result(host_errno_to_i386(ESRCH)); return; }
    thread = found->second.get();
    if (thread->joining) { call.result(host_errno_to_i386(EINVAL)); return; }
    thread->joining = true;
  }
  // Never hold the registry lock while waiting for a guest callback to finish.
  const int error = pthread_join(thread->host, nullptr);
  std::lock_guard<std::mutex> lock(pthread_registry_mutex);
  if (error) thread->joining = false;
  else {
    if (output) memcpy(output, &thread->result, 4);
    release(thread->stack);
    guest_pthreads.erase(id);
  }
  call.result(host_errno_to_i386(error));
}
// glibc nptl/pthread_mutexattr_{init,settype,destroy}.c: one i386 word;
// settype retains robust/shared/protocol/priority flags; destroy owns no resources.
IMPORT(pthread_mutexattr_init) {
  PthreadCall call(arena, state);
  const uint32_t kind = 0;
  memcpy(guest(arena, call.word(0), 4), &kind, 4);
  call.result(0);
}
IMPORT(pthread_mutexattr_settype) {
  PthreadCall call(arena, state);
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
  PthreadCall call(arena, state);
  call.result(0);
}
IMPORT(pthread_mutex_init) {
  PthreadCall call(arena, state);
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
  PthreadCall call(arena, state);
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
  PthreadCall call(arena, state); \
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
  PthreadCall call(arena, state);
  const uint32_t address = call.word(0);
  auto *object = guest(arena, address, 48);
  uint32_t attributes = 0;
  if (call.word(1)) memcpy(&attributes, guest(arena, call.word(1), 4), 4);
  if (attributes) { call.result(host_errno_to_i386(ENOTSUP)); return; }
  std::lock_guard<std::mutex> lock(synchronization_registry_mutex);
  if (guest_conditions.count(address)) { call.result(host_errno_to_i386(EBUSY)); return; }
  auto condition = std::make_shared<GuestCondition>();
  const int error = pthread_cond_init(&condition->host, nullptr);
  if (!error) {
    condition->initialized = true;
    memset(object, 0, 48);
    guest_conditions.emplace(address, std::move(condition));
  }
  call.result(host_errno_to_i386(error));
}
IMPORT(pthread_cond_destroy) {
  PthreadCall call(arena, state);
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
  PthreadCall call(arena, state);
  int error = 0;
  auto condition = find_condition(arena, call.word(0), error);
  auto mutex = error ? std::shared_ptr<GuestMutex>() : find_mutex(arena, call.word(1), error);
  if (condition && mutex) error = pthread_cond_wait(&condition->host, &mutex->host);
  call.result(host_errno_to_i386(error));
}
#define CONDITION_OPERATION(name) \
IMPORT(name) { \
  PthreadCall call(arena, state); \
  int error = 0; \
  auto condition = find_condition(arena, call.word(0), error); \
  if (condition) error = name(&condition->host); \
  call.result(host_errno_to_i386(error)); \
}
CONDITION_OPERATION(pthread_cond_signal)
CONDITION_OPERATION(pthread_cond_broadcast)
#undef CONDITION_OPERATION
