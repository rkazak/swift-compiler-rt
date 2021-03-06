//===-- tsan_libdispatch_mac.cc -------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Mac-specific libdispatch (GCD) support.
//===----------------------------------------------------------------------===//

#include "sanitizer_common/sanitizer_platform.h"
#if SANITIZER_MAC

#include "sanitizer_common/sanitizer_common.h"
#include "interception/interception.h"
#include "tsan_interceptors.h"
#include "tsan_platform.h"
#include "tsan_rtl.h"

#include <Block.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

typedef long long_t;  // NOLINT

namespace __tsan {

typedef struct {
  dispatch_queue_t queue;
  void *orig_context;
  dispatch_function_t orig_work;
  uptr sync_object;
  dispatch_object_t object_to_release;
  bool free_context_in_callback;
  bool release_sync_object_in_callback;
} tsan_block_context_t;

// The offsets of different fields of the dispatch_queue_t structure, exported
// by libdispatch.dylib.
extern "C" struct dispatch_queue_offsets_s {
  const uint16_t dqo_version;
  const uint16_t dqo_label;
  const uint16_t dqo_label_size;
  const uint16_t dqo_flags;
  const uint16_t dqo_flags_size;
  const uint16_t dqo_serialnum;
  const uint16_t dqo_serialnum_size;
  const uint16_t dqo_width;
  const uint16_t dqo_width_size;
  const uint16_t dqo_running;
  const uint16_t dqo_running_size;
  const uint16_t dqo_suspend_cnt;
  const uint16_t dqo_suspend_cnt_size;
  const uint16_t dqo_target_queue;
  const uint16_t dqo_target_queue_size;
  const uint16_t dqo_priority;
  const uint16_t dqo_priority_size;
} dispatch_queue_offsets;

static bool IsQueueSerial(dispatch_queue_t q) {
  CHECK_EQ(dispatch_queue_offsets.dqo_width_size, 2);
  uptr width = *(uint16_t *)(((uptr)q) + dispatch_queue_offsets.dqo_width);
  CHECK_NE(width, 0);
  return width == 1;
}

static tsan_block_context_t *AllocContext(ThreadState *thr, uptr pc,
                                          dispatch_queue_t queue,
                                          void *orig_context,
                                          dispatch_function_t orig_work) {
  tsan_block_context_t *new_context =
      (tsan_block_context_t *)user_alloc(thr, pc, sizeof(tsan_block_context_t));
  new_context->queue = queue;
  new_context->orig_context = orig_context;
  new_context->orig_work = orig_work;
  new_context->sync_object = (uptr)new_context;
  new_context->object_to_release = nullptr;
  new_context->free_context_in_callback = true;
  new_context->release_sync_object_in_callback = false;
  return new_context;
}

static void dispatch_callback_wrap(void *param) {
  SCOPED_INTERCEPTOR_RAW(dispatch_callback_wrap);
  tsan_block_context_t *context = (tsan_block_context_t *)param;

  Acquire(thr, pc, context->sync_object);

  // Extra retain/release is required for dispatch groups. We use the group
  // itself to synchronize, but in a notification (dispatch_group_notify
  // callback), it may be disposed already. To solve this, we retain the group
  // and release it here.
  if (context->object_to_release) dispatch_release(context->object_to_release);

  // In serial queues, work items can be executed on different threads, we need
  // to explicitly synchronize on the queue itself.
  if (IsQueueSerial(context->queue)) Acquire(thr, pc, (uptr)context->queue);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  context->orig_work(context->orig_context);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
  if (IsQueueSerial(context->queue)) Release(thr, pc, (uptr)context->queue);

  if (context->release_sync_object_in_callback)
    Release(thr, pc, context->sync_object);

  if (context->free_context_in_callback) user_free(thr, pc, context);
}

static void invoke_and_release_block(void *param) {
  dispatch_block_t block = (dispatch_block_t)param;
  block();
  Block_release(block);
}

#define DISPATCH_INTERCEPT_B(name)                                           \
  TSAN_INTERCEPTOR(void, name, dispatch_queue_t q, dispatch_block_t block) { \
    SCOPED_TSAN_INTERCEPTOR(name, q, block);                                 \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                           \
    dispatch_block_t heap_block = Block_copy(block);                         \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                             \
    tsan_block_context_t *new_context =                                      \
        AllocContext(thr, pc, q, heap_block, &invoke_and_release_block);     \
    Release(thr, pc, (uptr)new_context);                                     \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                           \
    REAL(name##_f)(q, new_context, dispatch_callback_wrap);                  \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                             \
  }

#define DISPATCH_INTERCEPT_SYNC_B(name)                                      \
  TSAN_INTERCEPTOR(void, name, dispatch_queue_t q, dispatch_block_t block) { \
    SCOPED_TSAN_INTERCEPTOR(name, q, block);                                 \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                           \
    dispatch_block_t heap_block = Block_copy(block);                         \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                             \
    tsan_block_context_t new_context = {                                     \
        q, heap_block, &invoke_and_release_block, 0, 0, false, true};        \
    new_context.sync_object = (uptr)&new_context;                            \
    Release(thr, pc, (uptr)&new_context);                                    \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                           \
    REAL(name##_f)(q, &new_context, dispatch_callback_wrap);                 \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                             \
    Acquire(thr, pc, (uptr)&new_context);                                    \
  }

#define DISPATCH_INTERCEPT_F(name)                                \
  TSAN_INTERCEPTOR(void, name, dispatch_queue_t q, void *context, \
                   dispatch_function_t work) {                    \
    SCOPED_TSAN_INTERCEPTOR(name, q, context, work);              \
    tsan_block_context_t *new_context =                           \
        AllocContext(thr, pc, q, context, work);                  \
    Release(thr, pc, (uptr)new_context);                          \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                \
    REAL(name)(q, new_context, dispatch_callback_wrap);           \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                  \
  }

#define DISPATCH_INTERCEPT_SYNC_F(name)                                       \
  TSAN_INTERCEPTOR(void, name, dispatch_queue_t q, void *context,             \
                   dispatch_function_t work) {                                \
    SCOPED_TSAN_INTERCEPTOR(name, q, context, work);                          \
    tsan_block_context_t new_context = {q, context, work, 0, 0, false, true}; \
    new_context.sync_object = (uptr)&new_context;                             \
    Release(thr, pc, (uptr)&new_context);                                     \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();                            \
    REAL(name)(q, &new_context, dispatch_callback_wrap);                      \
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();                              \
    Acquire(thr, pc, (uptr)&new_context);                                     \
  }

// We wrap dispatch_async, dispatch_sync and friends where we allocate a new
// context, which is used to synchronize (we release the context before
// submitting, and the callback acquires it before executing the original
// callback).
DISPATCH_INTERCEPT_B(dispatch_async)
DISPATCH_INTERCEPT_B(dispatch_barrier_async)
DISPATCH_INTERCEPT_F(dispatch_async_f)
DISPATCH_INTERCEPT_F(dispatch_barrier_async_f)
DISPATCH_INTERCEPT_SYNC_B(dispatch_sync)
DISPATCH_INTERCEPT_SYNC_B(dispatch_barrier_sync)
DISPATCH_INTERCEPT_SYNC_F(dispatch_sync_f)
DISPATCH_INTERCEPT_SYNC_F(dispatch_barrier_sync_f)

TSAN_INTERCEPTOR(void, dispatch_after, dispatch_time_t when,
                 dispatch_queue_t queue, dispatch_block_t block) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_after, when, queue, block);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  dispatch_block_t heap_block = Block_copy(block);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
  tsan_block_context_t *new_context =
      AllocContext(thr, pc, queue, heap_block, &invoke_and_release_block);
  Release(thr, pc, (uptr)new_context);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  REAL(dispatch_after_f)(when, queue, new_context, dispatch_callback_wrap);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
}

TSAN_INTERCEPTOR(void, dispatch_after_f, dispatch_time_t when,
                 dispatch_queue_t queue, void *context,
                 dispatch_function_t work) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_after_f, when, queue, context, work);
  WRAP(dispatch_after)(when, queue, ^(void) {
    work(context);
  });
}

// GCD's dispatch_once implementation has a fast path that contains a racy read
// and it's inlined into user's code. Furthermore, this fast path doesn't
// establish a proper happens-before relations between the initialization and
// code following the call to dispatch_once. We could deal with this in
// instrumented code, but there's not much we can do about it in system
// libraries. Let's disable the fast path (by never storing the value ~0 to
// predicate), so the interceptor is always called, and let's add proper release
// and acquire semantics. Since TSan does not see its own atomic stores, the
// race on predicate won't be reported - the only accesses to it that TSan sees
// are the loads on the fast path. Loads don't race. Secondly, dispatch_once is
// both a macro and a real function, we want to intercept the function, so we
// need to undefine the macro.
#undef dispatch_once
TSAN_INTERCEPTOR(void, dispatch_once, dispatch_once_t *predicate,
                 dispatch_block_t block) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_once, predicate, block);
  atomic_uint32_t *a = reinterpret_cast<atomic_uint32_t *>(predicate);
  u32 v = atomic_load(a, memory_order_acquire);
  if (v == 0 &&
      atomic_compare_exchange_strong(a, &v, 1, memory_order_relaxed)) {
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
    block();
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
    Release(thr, pc, (uptr)a);
    atomic_store(a, 2, memory_order_release);
  } else {
    while (v != 2) {
      internal_sched_yield();
      v = atomic_load(a, memory_order_acquire);
    }
    Acquire(thr, pc, (uptr)a);
  }
}

#undef dispatch_once_f
TSAN_INTERCEPTOR(void, dispatch_once_f, dispatch_once_t *predicate,
                 void *context, dispatch_function_t function) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_once_f, predicate, context, function);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  WRAP(dispatch_once)(predicate, ^(void) {
    function(context);
  });
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
}

TSAN_INTERCEPTOR(long_t, dispatch_semaphore_signal,
                 dispatch_semaphore_t dsema) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_semaphore_signal, dsema);
  Release(thr, pc, (uptr)dsema);
  return REAL(dispatch_semaphore_signal)(dsema);
}

TSAN_INTERCEPTOR(long_t, dispatch_semaphore_wait, dispatch_semaphore_t dsema,
                 dispatch_time_t timeout) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_semaphore_wait, dsema, timeout);
  long_t result = REAL(dispatch_semaphore_wait)(dsema, timeout);
  if (result == 0) Acquire(thr, pc, (uptr)dsema);
  return result;
}

TSAN_INTERCEPTOR(long_t, dispatch_group_wait, dispatch_group_t group,
                 dispatch_time_t timeout) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_wait, group, timeout);
  long_t result = REAL(dispatch_group_wait)(group, timeout);
  if (result == 0) Acquire(thr, pc, (uptr)group);
  return result;
}

TSAN_INTERCEPTOR(void, dispatch_group_leave, dispatch_group_t group) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_leave, group);
  Release(thr, pc, (uptr)group);
  REAL(dispatch_group_leave)(group);
}

TSAN_INTERCEPTOR(void, dispatch_group_async, dispatch_group_t group,
                 dispatch_queue_t queue, dispatch_block_t block) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_async, group, queue, block);
  dispatch_retain(group);
  dispatch_group_enter(group);
  WRAP(dispatch_async)(queue, ^(void) {
    block();
    WRAP(dispatch_group_leave)(group);
    dispatch_release(group);
  });
}

TSAN_INTERCEPTOR(void, dispatch_group_async_f, dispatch_group_t group,
                 dispatch_queue_t queue, void *context,
                 dispatch_function_t work) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_async_f, group, queue, context, work);
  dispatch_retain(group);
  dispatch_group_enter(group);
  WRAP(dispatch_async)(queue, ^(void) {
    work(context);
    WRAP(dispatch_group_leave)(group);
    dispatch_release(group);
  });
}

TSAN_INTERCEPTOR(void, dispatch_group_notify, dispatch_group_t group,
                 dispatch_queue_t q, dispatch_block_t block) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_notify, group, q, block);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  dispatch_block_t heap_block = Block_copy(block);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
  tsan_block_context_t *new_context =
      AllocContext(thr, pc, q, heap_block, &invoke_and_release_block);
  new_context->sync_object = (uptr)group;

  // Will be released in dispatch_callback_wrap.
  new_context->object_to_release = group;
  dispatch_retain(group);

  Release(thr, pc, (uptr)group);
  REAL(dispatch_group_notify_f)(group, q, new_context,
                                dispatch_callback_wrap);
}

TSAN_INTERCEPTOR(void, dispatch_group_notify_f, dispatch_group_t group,
                 dispatch_queue_t q, void *context, dispatch_function_t work) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_group_notify_f, group, q, context, work);
  tsan_block_context_t *new_context = AllocContext(thr, pc, q, context, work);
  new_context->sync_object = (uptr)group;

  // Will be released in dispatch_callback_wrap.
  new_context->object_to_release = group;
  dispatch_retain(group);

  Release(thr, pc, (uptr)group);
  REAL(dispatch_group_notify_f)(group, q, new_context,
                                dispatch_callback_wrap);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_event_handler,
                 dispatch_source_t source, dispatch_block_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_event_handler, source, handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_event_handler)(source, nullptr);
  dispatch_block_t new_handler = ^(void) {
    {
      SCOPED_INTERCEPTOR_RAW(dispatch_source_set_event_handler_callback);
      Acquire(thr, pc, (uptr)source);
    }
    handler();
  };
  Release(thr, pc, (uptr)source);
  REAL(dispatch_source_set_event_handler)(source, new_handler);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_event_handler_f,
                 dispatch_source_t source, dispatch_function_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_event_handler_f, source, handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_event_handler)(source, nullptr);
  dispatch_block_t block = ^(void) {
    handler(dispatch_get_context(source));
  };
  WRAP(dispatch_source_set_event_handler)(source, block);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_cancel_handler,
                 dispatch_source_t source, dispatch_block_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_cancel_handler, source, handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_cancel_handler)(source, nullptr);
  dispatch_block_t new_handler = ^(void) {
    {
      SCOPED_INTERCEPTOR_RAW(dispatch_source_set_cancel_handler_callback);
      Acquire(thr, pc, (uptr)source);
    }
    handler();
  };
  Release(thr, pc, (uptr)source);
  REAL(dispatch_source_set_cancel_handler)(source, new_handler);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_cancel_handler_f,
                 dispatch_source_t source, dispatch_function_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_cancel_handler_f, source,
                          handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_cancel_handler)(source, nullptr);
  dispatch_block_t block = ^(void) {
    handler(dispatch_get_context(source));
  };
  WRAP(dispatch_source_set_cancel_handler)(source, block);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_registration_handler,
                 dispatch_source_t source, dispatch_block_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_registration_handler, source,
                          handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_registration_handler)(source, nullptr);
  dispatch_block_t new_handler = ^(void) {
    {
      SCOPED_INTERCEPTOR_RAW(dispatch_source_set_registration_handler_callback);
      Acquire(thr, pc, (uptr)source);
    }
    handler();
  };
  Release(thr, pc, (uptr)source);
  REAL(dispatch_source_set_registration_handler)(source, new_handler);
}

TSAN_INTERCEPTOR(void, dispatch_source_set_registration_handler_f,
                 dispatch_source_t source, dispatch_function_t handler) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_source_set_registration_handler_f, source,
                          handler);
  if (handler == nullptr)
    return REAL(dispatch_source_set_registration_handler)(source, nullptr);
  dispatch_block_t block = ^(void) {
    handler(dispatch_get_context(source));
  };
  WRAP(dispatch_source_set_registration_handler)(source, block);
}

TSAN_INTERCEPTOR(void, dispatch_apply, size_t iterations,
                 dispatch_queue_t queue, void (^block)(size_t)) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_apply, iterations, queue, block);

  void *parent_to_child_sync = nullptr;
  uptr parent_to_child_sync_uptr = (uptr)&parent_to_child_sync;
  void *child_to_parent_sync = nullptr;
  uptr child_to_parent_sync_uptr = (uptr)&child_to_parent_sync;

  Release(thr, pc, parent_to_child_sync_uptr);
  void (^new_block)(size_t) = ^(size_t iteration) {
    SCOPED_INTERCEPTOR_RAW(dispatch_apply);
    Acquire(thr, pc, parent_to_child_sync_uptr);
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
    block(iteration);
    SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
    Release(thr, pc, child_to_parent_sync_uptr);
  };
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_START();
  REAL(dispatch_apply)(iterations, queue, new_block);
  SCOPED_TSAN_INTERCEPTOR_USER_CALLBACK_END();
  Acquire(thr, pc, child_to_parent_sync_uptr);
}

TSAN_INTERCEPTOR(void, dispatch_apply_f, size_t iterations,
                 dispatch_queue_t queue, void *context,
                 void (*work)(void *, size_t)) {
  SCOPED_TSAN_INTERCEPTOR(dispatch_apply_f, iterations, queue, context, work);
  void (^new_block)(size_t) = ^(size_t iteration) {
    work(context, iteration);
  };
  WRAP(dispatch_apply)(iterations, queue, new_block);
}

}  // namespace __tsan

#endif  // SANITIZER_MAC
