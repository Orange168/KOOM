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

#define LOG_TAG "memory_analyzer"
#include "memory_analyzer.h"

#include <dlfcn.h>
#include <log/log.h>
#include <sys/prctl.h>

#include <regex>

#include "kwai_linker/kwai_dlfcn.h"

namespace kwai {
namespace leak_monitor {
static const char *kLibMemUnreachableName = "libmemunreachable.so";
// Just need the symbol in arm64-v8a so
// API level > Android O
static const char *kGetUnreachableMemoryStringSymbolAboveO =
    "_ZN7android26GetUnreachableMemoryStringEbm";
// API level <= Android O
static const char *kGetUnreachableMemoryStringSymbolBelowO =
    "_Z26GetUnreachableMemoryStringbm";

MemoryAnalyzer::MemoryAnalyzer()
    : get_unreachable_fn_(nullptr), handle_(nullptr) {
  auto handle = kwai::linker::DlFcn::dlopen(kLibMemUnreachableName, RTLD_NOW);
  if (!handle) {
    ALOGE("dlopen %s error: %s", kLibMemUnreachableName, dlerror());
    return;
  }

  if (android_get_device_api_level() > __ANDROID_API_O__) {
    get_unreachable_fn_ =
        reinterpret_cast<GetUnreachableFn>(kwai::linker::DlFcn::dlsym(
            handle, kGetUnreachableMemoryStringSymbolAboveO));
  } else {
    get_unreachable_fn_ =
        reinterpret_cast<GetUnreachableFn>(kwai::linker::DlFcn::dlsym(
            handle, kGetUnreachableMemoryStringSymbolBelowO));
  }
}

MemoryAnalyzer::~MemoryAnalyzer() {
  if (handle_) {
    kwai::linker::DlFcn::dlclose(handle_);
  }
}

bool MemoryAnalyzer::IsValid() { return get_unreachable_fn_ != nullptr; }

//std::vector<std::pair<uintptr_t, size_t>>
    // MemoryAnalyzer的成员函数，用于收集不可达的内存块
std::vector<std::pair<uintptr_t, size_t>> MemoryAnalyzer::CollectUnreachableMem() {
  // 用于存储不可达内存的地址和大小
  std::vector<std::pair<uintptr_t, size_t>> unreachable_mem;
  // 检查MemoryAnalyzer对象是否有效
  if (!IsValid()) {
    ALOGE("MemoryAnalyzer NOT valid"); // 打印错误日志
    return std::move(unreachable_mem); // 返回空的不可达内存列表
  }
  // 获取当前进程的dumpable状态
  int origin_dumpable = prctl(PR_GET_DUMPABLE);
  // 尝试设置进程为dumpable，以允许libmemunreachable使用ptrace
  if (prctl(PR_SET_DUMPABLE, 1) == -1) {
    ALOGE("Set process dumpable Fail"); // 如果设置失败，打印错误日志
    return std::move(unreachable_mem); // 返回空的不可达内存列表
  }
  // 调用libmemunreachable获取不可达内存；这是一个耗时操作
  std::string unreachable_memory = get_unreachable_fn_(false, 1024);
  // 恢复进程的dumpable状态，这是出于安全考虑
  prctl(PR_SET_DUMPABLE, origin_dumpable);
  // 使用正则表达式匹配不可达内存块的信息
  std::regex filter_regex("[0-9]+ bytes unreachable at [A-Za-z0-9]+");
  // 创建一个正则表达式，用于匹配形如“123 bytes unreachable at ABC123”这样的字符串。
  std::sregex_iterator unreachable_begin(
      unreachable_memory.begin(), unreachable_memory.end(), filter_regex);
  // 创建一个正则表达式迭代器`unreachable_begin`，用于在字符串`unreachable_memory`中迭代查找所有匹配`filter_regex`的子串。
  std::sregex_iterator unreachable_end;
  // 创建一个正则表达式迭代器`unreachable_end`，作为迭代结束标志。
  // 遍历所有匹配的结果
  for (; unreachable_begin != unreachable_end; ++unreachable_begin) {
    const auto& line = unreachable_begin->str();
    // 获取当前匹配到的字符串，例如“123 bytes unreachable at ABC123”。
    // 解析内存块地址
    auto address =
        std::stoul(line.substr(line.find_last_of(' ') + 1,
                               line.length() - line.find_last_of(' ') - 1),
                   0, 16);
    // 从匹配到的字符串中提取内存地址部分，并转换为无符号长整型数。
    // 例如，如果line是"123 bytes unreachable at ABC123"，address将是0xABC123。
    // 解析内存块大小
    auto size = std::stoul(line.substr(0, line.find_first_of(' ')));
    // 从匹配到的字符串中提取内存大小部分，并转换为无符号长整型数。
    // 例如，如果line是"123 bytes unreachable at ABC123"，size将是123。
    // 把地址和大小添加到结果中
    unreachable_mem.emplace_back(address, size);
    // 将解析出的地址和大小作为一个pair添加到`unreachable_mem`容器中。
  }
  // 返回所有找到的不可达内存块
  return std::move(unreachable_mem);
}
}  // namespace leak_monitor
}  // namespace kwai
