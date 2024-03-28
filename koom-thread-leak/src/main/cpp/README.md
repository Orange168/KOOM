# CMakeLists.txt 语法
## CMakeLists.txt转换成C++编译语法
```shell
g++ -Oz -I<include_directories> -L<link_directories> -lname <source_files> -o <output_name>
```

## 常见的C++编译格式对应CMake配置
| C++编译器（如 `g++`）选项 | CMake 配置                                             |
| ------------------------- | ------------------------------------------------------ |
| `g++ -c`                  | `add_library(target STATIC src.cpp)`（仅编译，不链接） |
| `g++ -o output`           | `add_executable(name src.cpp)`（生成可执行文件）       |
| `g++ -I/path/include`     | `include_directories(/path/include)`（头文件目录）     |
| `g++ -L/path/lib`         | `link_directories(/path/lib)`（库文件目录）            |
| `g++ -llibname`           | `target_link_libraries(target libname)`（链接库）      |
| `g++ -DDEFINITION`        | `add_definitions(-DDEFINITION)`（预处理器定义）        |
| `g++ -Olevel`             | `add_compile_options(-Olevel)`（优化级别）             |

