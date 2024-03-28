/*
 * Copyright (c) 2021. Kwai, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Created by lbtrace on 2021.
 *
 */

#define LOG_TAG "leak_monitor"
#include "leak_monitor.h"

#include <asm/mman.h>
#include <assert.h>
#include <dlfcn.h>
#include <kwai_util/kwai_macros.h>
#include <log/kcheck.h>
#include <log/log.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <unwind.h>
#include <utils/hook_helper.h>
#include <utils/stack_trace.h>

#include <functional>
#include <regex>
#include <thread>

#include "kwai_linker/kwai_dlfcn.h"
#include "utils/auto_time.h"

namespace kwai {
namespace leak_monitor {

#define CLEAR_MEMORY(ptr, size) \
  do {                          \
    if (ptr) {                  \
      memset(ptr, 0, size);     \
    }                           \
  } while (0)

#define WRAP(x) x##Monitor
#define HOOK(ret_type, function, ...) \
  static ALWAYS_INLINE ret_type WRAP(function)(__VA_ARGS__)

// Define allocator proxies; aligned_alloc included in API 28 and valloc/pvalloc
// can ignore in LP64 So we can't proxy aligned_alloc/valloc/pvalloc.
HOOK(void, free, void *ptr) {
  free(ptr);
  if (ptr) {
    LeakMonitor::GetInstance().UnregisterAlloc(
        reinterpret_cast<uintptr_t>(ptr));
  }
}

HOOK(void *, malloc, size_t size) {
  auto result = malloc(size);
  LeakMonitor::GetInstance().OnMonitor(reinterpret_cast<intptr_t>(result),
                                       size);
  CLEAR_MEMORY(result, size);
  return result;
}

HOOK(void *, realloc, void *ptr, size_t size) {
  auto result = realloc(ptr, size);
  if (ptr != nullptr) {
    LeakMonitor::GetInstance().UnregisterAlloc(
        reinterpret_cast<uintptr_t>(ptr));
  }
  LeakMonitor::GetInstance().OnMonitor(reinterpret_cast<intptr_t>(result),
                                       size);
  return result;
}

HOOK(void *, calloc, size_t item_count, size_t item_size) {
  auto result = calloc(item_count, item_size);
  LeakMonitor::GetInstance().OnMonitor(reinterpret_cast<intptr_t>(result),
                                       item_count * item_size);
  return result;
}

HOOK(void *, memalign, size_t alignment, size_t byte_count) {
  auto result = memalign(alignment, byte_count);
  LeakMonitor::GetInstance().OnMonitor(reinterpret_cast<intptr_t>(result),
                                       byte_count);
  CLEAR_MEMORY(result, byte_count);
  return result;
}

HOOK(int, posix_memalign, void **memptr, size_t alignment, size_t size) {
  auto result = posix_memalign(memptr, alignment, size);
  LeakMonitor::GetInstance().OnMonitor(reinterpret_cast<intptr_t>(*memptr),
                                       size);
  CLEAR_MEMORY(*memptr, size);
  return result;
}

LeakMonitor &LeakMonitor::GetInstance() {
  static LeakMonitor leak_monitor;
  return leak_monitor;
}

bool LeakMonitor::Install(std::vector<std::string> *selected_list,
                          std::vector<std::string> *ignore_list) {
  KCHECK(!has_install_monitor_);

  // Reinstall can't hook again
  if (has_install_monitor_) {
    return true;
  }

  memory_analyzer_ = std::make_unique<MemoryAnalyzer>();
  if (!memory_analyzer_->IsValid()) {
    ALOGE("memory_analyzer_ NOT Valid");
    return false;
  }

  std::vector<const std::string> register_pattern = {"^/data/.*\\.so$"};
  std::vector<const std::string> ignore_pattern = {".*/libkoom-native.so$",
                                                   ".*/libxhook_lib.so$"};

  if (ignore_list != nullptr) {
    for (std::string &item : *ignore_list) {
      ignore_pattern.push_back(".*/" + item + ".so$");
    }
  }
  if (selected_list != nullptr && !selected_list->empty()) {
    // only hook the so in selected list
    register_pattern.clear();
    for (std::string &item : *selected_list) {
      register_pattern.push_back("^/data/.*/" + item + ".so$");
    }
  }
  std::vector<std::pair<const std::string, void *const>> hook_entries = {
      std::make_pair("malloc", reinterpret_cast<void *>(WRAP(malloc))),
      std::make_pair("realloc", reinterpret_cast<void *>(WRAP(realloc))),
      std::make_pair("calloc", reinterpret_cast<void *>(WRAP(calloc))),
      std::make_pair("memalign", reinterpret_cast<void *>(WRAP(memalign))),
      std::make_pair("posix_memalign",
                     reinterpret_cast<void *>(WRAP(posix_memalign))),
      std::make_pair("free", reinterpret_cast<void *>(WRAP(free)))};

  if (HookHelper::HookMethods(register_pattern, ignore_pattern, hook_entries)) {
    has_install_monitor_ = true;
    return true;
  }

  HookHelper::UnHookMethods();
  live_alloc_records_.Clear();
  memory_analyzer_.reset(nullptr);
  ALOGE("%s Fail", __FUNCTION__);
  return false;
}

void LeakMonitor::Uninstall() {
  KCHECKV(has_install_monitor_)
  has_install_monitor_ = false;
  HookHelper::UnHookMethods();
  live_alloc_records_.Clear();
  memory_analyzer_.reset(nullptr);
}

void LeakMonitor::SetMonitorThreshold(size_t threshold) {
  KCHECK(has_install_monitor_);
  alloc_threshold_ = threshold;
}

// GetLeakAllocs方法用于获取当前存在的内存泄漏分配记录
std::vector<std::shared_ptr<AllocRecord>> LeakMonitor::GetLeakAllocs() {
  // KCHECK宏检查监控器是否已安装，如果未安装，则抛出异常
  KCHECK(has_install_monitor_);

  // 调用MemoryAnalyzer类的CollectUnreachableMem方法来收集不可达的内存块信息
  auto unreachable_allocs = memory_analyzer_->CollectUnreachableMem();

  // 创建两个vector来存储活动分配和泄漏分配的记录
  std::vector<std::shared_ptr<AllocRecord>> live_allocs;
  std::vector<std::shared_ptr<AllocRecord>> leak_allocs;

  // 定义一个lambda表达式来收集活动内存块
  // collect_func是一个函数对象，它接受一个AllocRecord的shared_ptr并将其添加到live_allocs中
  auto collect_func = [&](std::shared_ptr<AllocRecord> &alloc_info) -> void {
    live_allocs.push_back(alloc_info);
  };

  // 使用collect_func函数对象来遍历live_alloc_records_（一个内部映射，存储所有活动的内存分配）
  live_alloc_records_.Dump(collect_func);

  // 定义一个lambda表达式来判断一个活动分配是否是一个泄漏
  // is_leak接受一个不可达内存块和一个活动分配的引用，返回一个bool值表示是否泄漏
  auto is_leak = [&](decltype(unreachable_allocs)::value_type &unreachable,
                     std::shared_ptr<AllocRecord> &live) -> bool {
    // 使用CONFUSE宏来混淆地址，防止地址被优化掉
    auto live_start = CONFUSE(live->address);
    auto live_end = live_start + live->size;
    auto unreachable_start = unreachable.first;
    auto unreachable_end = unreachable_start + unreachable.second;
    // 判断活动分配是否与不可达内存块有重叠，如果是，则认为是泄漏
    return live_start == unreachable_start ||
           live_start >= unreachable_start && live_end <= unreachable_end;
  };

  // 遍历所有活动分配，检查它们是否与不可达内存块重叠
  for (auto &live : live_allocs) {
    for (auto &unreachable : unreachable_allocs) {
      // 如果发现活动分配是泄漏的，将其添加到leak_allocs中，并从活动分配中移除
      if (is_leak(unreachable, live)) {
        leak_allocs.push_back(live);
        // 移除泄漏的分配记录
        UnregisterAlloc(CONFUSE(live->address));
      }
    }
  }

  // 返回包含所有泄漏分配记录的vector
  return leak_allocs;
}

uint64_t LeakMonitor::CurrentAllocIndex() {
  KCHECK(has_install_monitor_);
  return alloc_index_.load(std::memory_order_relaxed);
}

ALWAYS_INLINE void LeakMonitor::RegisterAlloc(uintptr_t address, size_t size) {
  if (!address || !size) {
    return;
  }

  auto unwind_backtrace = [](uintptr_t *frames, uint32_t *frame_count) {
    *frame_count = StackTrace::FastUnwind(frames, kMaxBacktraceSize);
  };

  thread_local ThreadInfo thread_info;
  auto alloc_record = std::make_shared<AllocRecord>();
  alloc_record->address = CONFUSE(address);
  alloc_record->size = size;
  alloc_record->index = alloc_index_++;
  memcpy(alloc_record->thread_name, thread_info.name, kMaxThreadNameLen);
  unwind_backtrace(alloc_record->backtrace, &(alloc_record->num_backtraces));
  live_alloc_records_.Put(CONFUSE(address), std::move(alloc_record));
}

ALWAYS_INLINE void LeakMonitor::UnregisterAlloc(uintptr_t address) {
  live_alloc_records_.Erase(CONFUSE(address));
}

ALWAYS_INLINE void LeakMonitor::OnMonitor(uintptr_t address, size_t size) {
  if (!has_install_monitor_ || !address ||
      size < alloc_threshold_.load(std::memory_order_relaxed)) {
    return;
  }

  RegisterAlloc(address, size);
}
}  // namespace leak_monitor
}  // namespace kwai