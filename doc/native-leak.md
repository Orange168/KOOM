## 调用逻辑
1. **初始化和配置（`leak_monitor.txt` 和 `jni_leak_monitor.txt`）**:
    - Java层通过JNI接口调用`nativeInstallMonitor`方法，传递选定的和忽略的库列表以及是否启用本地符号的布尔值。
    - `InstallMonitor`函数在C++层被调用，它创建`LeakMonitor`的实例，并配置钩子模式。
    - `LeakMonitor`实例调用`HookMethods`方法，通过`HookHelper`类注册需要监控的内存分配和释放函数。
2. **内存分配和释放的监控（`leak_monitor.txt`）**:
    - 当应用程序进行内存分配（如`malloc`、`calloc`、`realloc`、`memalign`、`posix_memalign`）或释放（`free`）时，通过`HookMethods`注册的钩子函数被调用。
    - 钩子函数记录分配的内存地址和大小，并将其注册到`LeakMonitor`的内部数据结构中。
3. **内存泄漏的检测（`leak_monitor.txt` 和 `memory_analyzer.txt`）**:
    - `LeakMonitor`定期调用`MemoryAnalyzer`的`CollectUnreachableMem`方法来收集当前不可达的内存块信息。
    - `MemoryAnalyzer`通过调用`libmemunreachable.so`库的函数来获取不可达内存的地址和大小。
4. **内存映射信息的获取（`memory_map.txt`）**:
    - `MemoryMap`类解析`/proc/self/maps`文件，构建内存映射的内部表示。
    - 当`LeakMonitor`需要获取泄漏内存的符号信息时，它会使用`MemoryMap`来获取映射信息和格式化符号名称。
5. **泄漏信息的报告（`leak_monitor.txt` 和 `jni_leak_monitor.txt`）**:
    - Java层通过JNI接口调用`nativeGetLeakAllocs`方法来获取当前的内存泄漏信息。
    - `GetLeakAllocs`函数在C++层被调用，它从`LeakMonitor`获取泄漏的内存分配记录，并将其转换为Java对象，然后返回给Java层。
6. **清理和卸载（`leak_monitor.txt` 和 `jni_leak_monitor.txt`）**:
    - 当不再需要内存泄漏监控时，Java层通过JNI接口调用`nativeUninstallMonitor`方法来卸载监控器。
    - `UninstallMonitor`函数在C++层被调用，它清除`LeakMonitor`的内部数据结构，并卸载所有之前注册的钩子。

这个系统的调用逻辑涉及多个组件和层次，从Java层到本地C/C++层，再到操作系统层面的内存映射和泄漏分析。通过这种方式，系统能够在整个应用程序的生命周期中监测和报告内存泄漏问题。

## 内存泄漏的分析结果
- 泄漏内存块的起始地址
- 泄漏内存块的大小
- 泄漏内存块所属的线程名称
- 内存分配时的调用栈信息
- 泄漏内存块的符号化地址信息（如果有的话）

### 模拟数据示例：

```agsl
泄漏记录 #1:
地址: 0x7f2a1a00c010
大小: 1024 bytes (1 KB)
线程:MainThread
调用栈:
  libexample.so!example_alloc+0x10
  libexample.so!example_use+0x20
  libexample.so!main+0x30
   libc.so!__libc_start_main+0x20

泄漏记录 #2:
地址: 0x7f2a1a00e00
大小: 2048 bytes (2 KB)
线程:WorkerThread
调用栈:
  libexample.so!worker_alloc+0x10
  libexample.so!worker_process+0x20
  libexample.so!worker_thread_func+0x30
```
总结：这个库的作用的知道内存泄漏的大小，然后通过调用栈和对应的地址能定位到泄漏的位置对吗

### 怎么通过偏移量定位源码
获取符号信息：确保你的libexample.so库包含了调试符号信息。如果是在编译时添加了调试信息（通常是用-g参数），你的.so文件应该包含了足够的信息来回溯函数调用。
使用addr2line工具：在Linux环境下，你可以使用addr2line工具来定位具体的源码位置。需要利用带有调试信息的二进制文件（例如，libexample.so）和泄漏记录中的地址。
执行命令：在终端里，使用下面的格式执行命令：
addr2line -e libexample.so -f -i -C 0x地址

具体到你的例子中，如果处理libexample.so!worker_alloc+0x10，你需要用泄漏地址0x7f2a1a00e00减去偏移量0x10，然后再使用addr2line工具。
分析调用栈：通过分析每个调用栈上的函数，你可以逐步定位到导致泄漏的代码段。
另外，你也可以使用调试器（如gdb）来分析，基本步骤如下：
运行gdb并加载你的.so文件：
gdb -q libexample.so
使用info symbol命令来查找地址对应的函数和偏移量，如：
(gdb) info symbol 0x你的地址-偏移量
需要注意的是，泄漏定位的准确性取决于二进制文件是否包含调试符号以及它的具体编译选项。如果在Release模式下编译，没有包含足够的符号信息，那么将很难进行源码定位。如果是这种情况，可能需要重新编译库文件，加上适当的调试标志再进行调试。 