/*
 * Copyright (C) 2012 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define LOG_TAG "memory_map"
#include "memory_map.h"

#include <ctype.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#if defined(__LP64__)
#define PAD_PTR "016" PRIxPTR
#else
#define PAD_PTR "08" PRIxPTR
#endif

// Format of /proc/<PID>/maps:
// 6f000000-6f01e000 rwxp 00000000 00:0c 16389419   /system/lib/libcomposer.so
// 解析 /proc/<PID>/maps 文件的一行数据并构造 MapEntry 对象。
// 示例行数据:
// "7fbdac000000-7fbdac021000 r-xp 00000000 08:02 661909             /lib/x86_64-linux-gnu/libpthread-2.23.so"
static MapEntry *ParseLine(char *line) {
  // 地址变量的声明
  uintptr_t start; // 段的起始地址
  uintptr_t end; // 段的结束地址
  uintptr_t offset; // 文件偏移
  int flags; // 线段的权限标志
  char permissions[5]; // 权限字符串
  int name_pos; // 文件名在行内的位置
  // 使用 sscanf 解析内存映射数据
  if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s %" PRIxPTR " %*x:%*x %*d %n",
             &start, &end, permissions, &offset, &name_pos) < 2) {
    return nullptr; // 如果解析失败，返回 nullptr
  }

  // Example after sscanf:
  // start = 0x7fbdac000000, end = 0x7fbdac021000, permissions = "r-xp", offset = 0

  const char *name = line + name_pos; // 获取文件名的位置
  size_t name_len = strlen(name);
  if (name_len && name[name_len - 1] == '\n') {
    name_len -= 1; // 如果文件名末尾有换行符，去掉它
  }

  // Example after processing the name:
  // name = "/lib/x86_64-linux-gnu/libpthread-2.23.so", name_len = length of name without '\n'

  flags = 0;
  if (permissions[0] == 'r') {
    flags |= PROT_READ; // 如果有读权限，增加读标志
  }
  if (permissions[2] == 'x') {
    flags |= PROT_EXEC; // 如果有执行权限，增加执行标志
  }

  // Example after setting the flags:
  // Considering permissions = "r-xp", flags = PROT_READ | PROT_EXEC

  // 使用解析后的数据构造 MapEntry 对象
  MapEntry *entry = new MapEntry(start, end, offset, name, name_len, flags);
  // 如果没有读权限，将实例设置为无效
  if (!(flags & PROT_READ)) {
    entry->load_bias = 0; // 无法读取的映射就对偏移量做零处理
    entry->init = true; // 标记为初始化
    entry->valid = false; // 标记为无效
  }
  return entry; // 返回构造的 MapEntry 对象
}

template <typename T>
static inline bool GetVal(MapEntry *entry, uintptr_t addr, T *store) {
  if (!(entry->flags & PROT_READ) || addr < entry->start ||
      addr + sizeof(T) > entry->end) {
    return false;
  }
  // Make sure the address is aligned properly.
  if (addr & (sizeof(T) - 1)) {
    return false;
  }
  *store = *reinterpret_cast<T *>(addr);
  return true;
}

static bool ValidElf(MapEntry *entry) {
  uintptr_t addr = entry->start;
  uintptr_t end;
  if (__builtin_add_overflow(addr, SELFMAG, &end) || end >= entry->end) {
    return false;
  }

  return memcmp(reinterpret_cast<void *>(addr), ELFMAG, SELFMAG) == 0;
}

// ReadLoadbias 函数读取并设置给定 MapEntry 的 load_bias 属性。
// ELF 文件的加载偏移量(load_bias)是文件中可执行段的虚拟地址与其在内存中实际加载位置的差值。
static void ReadLoadbias(MapEntry *entry) {
  entry->load_bias = 0; // 初始化 load_bias 为 0，这是偏移量的默认值，适用于未找到可执行段的情况
  uintptr_t addr = entry->start; // MapEntry 结构表示的内存区域的起始地址，假设是 0x100000

  // ELF文件的header结构体，包含了文件的元信息，如程序头部的数量和偏移位置
  ElfW(Ehdr) ehdr;

  // 从内存映射的起点读取程序头部数量和偏移位置，以便于我们知道如何找到这些头部段
  // offsetof 宏计算了结构中某个成员相对结构起始点的字节偏移量
  // GetVal 是一个获取特定地址值的泛型函数，所以我们需要提供正确的类型信息
  // 下面我们用一个假定的数值来说明此过程:
  // 假设 ehdr.e_phnum（程序头部数量）为3
  // 假设 ehdr.e_phoff（第一个程序头部相对于文件开始的偏移量）为64字节(0x40)
  if (!GetVal<ElfW(Half)>(entry, addr + offsetof(ElfW(Ehdr), e_phnum), &ehdr.e_phnum)) {
    return; // 读取失败，退出函数
  }
  if (!GetVal<ElfW(Off)>(entry, addr + offsetof(ElfW(Ehdr), e_phoff), &ehdr.e_phoff)) {
    return; // 读取失败，退出函数
  }

  // 根据 ELF 头部信息，调整 addr 来指向第一个程序头部
  addr += ehdr.e_phoff; // 现在 addr = 0x100040 (表明程序头部从内存地址的这个位置开始)

  // 遍历 ELF 文件的所有程序头部
  for (size_t i = 0; i < ehdr.e_phnum; i++) {
    ElfW(Phdr) phdr; // 程序头部结构

    // 再次使用 GetVal 来读取每个程序头部内的信息，我们关心两个特定的字段：类型 (p_type) 和标志 (p_flags)
    if (!GetVal<ElfW(Word)>(entry, addr + offsetof(ElfW(Phdr), p_type), &phdr.p_type)) {
      return; // 读取失败，退出函数
    }

    // 检查程序头部的类型是否为 PT_LOAD，这表明它对应于一个需要被加载到内存的段
    // 再检查它是否具备 PF_X（执行权限），因为我们只关心可执行的段来计算偏移量
    // 我们假设这个程序头部是第二个（索引为1），并且具备执行权限
    if (phdr.p_type == PT_LOAD && (GetVal<ElfW(Word)>(entry, addr + offsetof(ElfW(Phdr), p_flags), &phdr.p_flags)) && (phdr.p_flags & PF_X)) {
      // 接下来读取这个段的虚拟地址 (p_vaddr) 和在文件中的偏移 (p_offset)
      // 假设对应的数值如下：
      // p_vaddr(段的虚拟地址) = 0x200000
      // p_offset(在文件中的偏移量) = 0x1000
      if (!GetVal<ElfW(Addr)>(entry, addr + offsetof(ElfW(Phdr), p_vaddr), &phdr.p_vaddr)) {
        return; // 读取失败，退出函数
      }
      if (!GetVal<ElfW(Off)>(entry, addr + offsetof(ElfW(Phdr), p_offset), &phdr.p_offset)) {
        return; // 读取失败，退出函数
      }

      // 最终，计算load_bias，它是p_vaddr和p_offset的差值
      // load_bias = p_vaddr - p_offset
      // 例如：load_bias = 0x200000 - 0x1000 = 0x1FF000
      entry->load_bias = phdr.p_vaddr - phdr.p_offset;

      return; // 找到可执行段，设置了load_bias，我们完成了目标，退出循环
    }

    // 不是我们正在寻找的可执行段，移动addr到下一个程序头部的起始位置
    addr += sizeof(phdr); // 假设每个程序头部大小为56字节(0x38)，因此 addr += 0x38
  }
  // 如果没有读取到任何可执行段，load_bias 仍然是0，表明没有偏移量或这个映射不代表可执行的文件
}

static void inline Init(MapEntry *entry) {
  if (entry->init) {
    return;
  }
  entry->init = true;
  if (ValidElf(entry)) {
    entry->valid = true;
    ReadLoadbias(entry);
  }
}

bool MemoryMap::ReadMaps() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) {
    return false;
  }

  std::vector<char> buffer(1024);
  while (fgets(buffer.data(), buffer.size(), fp) != nullptr) {
    MapEntry *entry = ParseLine(buffer.data());
    if (entry == nullptr) {
      fclose(fp);
      return false;
    }

    auto it = entries_.find(entry);
    if (it == entries_.end()) {
      entries_.insert(entry);
    } else {
      delete entry;
    }
  }
  fclose(fp);
  return true;
}

MemoryMap::~MemoryMap() {
  for (auto *entry : entries_) {
    delete entry;
  }
  entries_.clear();
}

MapEntry *MemoryMap::CalculateRelPc(uintptr_t pc, uintptr_t *rel_pc) {
  MapEntry pc_entry(pc);

  auto it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    ReadMaps();
  }
  it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    return nullptr;
  }

  MapEntry *entry = *it;
  Init(entry);

  // 检查当前处理的内存映射条目（entry）是否关联了一个有效的程序计数器（rel_pc）。
  if (rel_pc != nullptr) {
    // 如果当前映射是一个只读执行映射，并且前一个映射是只读的，那么需要特别处理。
    // 这里首先检查entry是否有效，如果不是有效映射，说明可能是一个新映射，需要检查前一个映射。
    if (!entry->valid && it != entries_.begin()) {
      // 获取前一个映射条目（prev_entry），通过迭代器it向前移动一位。
      MapEntry *prev_entry = *--it;
      // 检查前一个映射是否为只读（PROT_READ）类型，并且其偏移量小于当前映射的偏移量，
      // 同时两个映射具有相同的名称，这表明它们可能是同一个文件的不同部分。
      if (prev_entry->flags == PROT_READ &&
          prev_entry->offset < entry->offset &&
          prev_entry->name == entry->name) {
        // 初始化前一个映射条目。
        Init(prev_entry);
        // 如果前一个映射条目有效，设置当前映射条目的起始偏移量。
        if (prev_entry->valid) {
          entry->elf_start_offset = prev_entry->offset;
          // 计算并设置相对PC值，这个值是程序计数器（pc）相对于映射条目起始地址的偏移量。
          *rel_pc = pc - entry->start + entry->offset + prev_entry->load_bias;
          return entry; // 返回当前映射条目，表示处理完成。
        }
      }
    }
    // 如果没有找到合适的只读映射，或者前一个映射不是只读的，那么直接计算当前映射的相对PC值。
    *rel_pc = pc - entry->start + entry->load_bias;
  }
  return entry;
}

// 假设MemoryMap类有一个成员函数FormatSymbol，用于格式化符号信息
std::string MemoryMap::FormatSymbol(MapEntry *entry, uintptr_t pc) {
  // 初始化一个空字符串str，用于存储最终的格式化输出
  std::string str;
  // 初始化offset为0，稍后用于存储偏移量
  uintptr_t offset = 0;
  // 初始化symbol为nullptr，稍后用于存储解析出的符号名称
  const char *symbol = nullptr;

  // 定义一个Dl_info类型的变量info，用于存储dladdr函数的输出
  Dl_info info;
  // 假设dladdr函数调用成功，info被填充如下：
  if (dladdr(reinterpret_cast<void *>(pc), &info) != 0) {
    // 假设info.dli_saddr指向的地址是0x2000，这是一个示例偏移量
    offset = reinterpret_cast<uintptr_t>(info.dli_saddr);
    // 假设info.dli_sname指向的字符串是"example_function"，这是一个示例符号名称
    symbol = info.dli_sname;
  } else {
    // 如果dladdr失败，info.dli_fname将被设置为nullptr
    info.dli_fname = nullptr;
  }

  // 假设entry是一个有效的MapEntry指针，name成员是"libexample.so"的字符串
  const char *soname =
      (entry != nullptr) ? entry->name.c_str() : info.dli_fname;
  // 如果soname为nullptr（在这里不会，因为我们有一个有效的entry），将其设置为"<unknown>"
  if (soname == nullptr) {
    soname = "<unknown>";
  }

  // 定义一个字符数组offset_buf，用于存储格式化后的偏移量信息
  char offset_buf[128];
  // 因为entry是有效的，entry->elf_start_offset假设为0x1000
  if (entry != nullptr && entry->elf_start_offset != 0) {
    // 格式化偏移量信息为" (offset 0x1000)"
    snprintf(offset_buf, sizeof(offset_buf), " (offset 0x%" PRIxPTR ")", entry->elf_start_offset);
  } else {
    // 如果没有偏移量信息，offset_buf将保持为空字符串
    offset_buf[0] = '\0';
  }

  // 定义一个字符数组buf，用于存储格式化后的符号信息
  char buf[1024];
  // 因为我们有一个有效的symbol，我们将使用demangle名称
  if (symbol != nullptr) {
    // 假设demangled_name函数调用返回"int example_function(int, double)"，这是示例demangled名称
    char *demangled_name = abi::__cxa_demangle(symbol, nullptr, nullptr, nullptr);
    const char *name;
    if (demangled_name != nullptr) {
      name = demangled_name;
    } else {
      name = symbol;
    }
    // 格式化符号信息，包括共享对象名称、偏移量信息和符号名称
    snprintf(buf, sizeof(buf), "  %s%s (%s+%" PRIuPTR ")\n", soname, offset_buf,
             name, pc - offset);
    free(demangled_name); // 释放demangled名称占用的内存
  } else {
    // 如果symbol为nullptr，只格式化共享对象名称和偏移量信息
    snprintf(buf, sizeof(buf), "  %s%s\n", soname, offset_buf);
  }
  // 将格式化后的字符串添加到str中
  str += buf;

  // 返回最终的格式化字符串
  return std::move(str);
}
