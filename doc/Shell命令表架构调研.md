# Shell 命令表架构调研：词法解析 + 查表匹配

> 日期：2026-10-20
> 对象：`framework/cmd/shell/` 当前命令分发机制（`shell.cpp` / `var` / `log` / `thread` / `boot`）
> 目标：评估「词法解析 + 命令表查表」相对当前「逐层 `strcmp` 链」的效果，给出是否值得重构的结论

---

## 1. 结论

**推荐方案 A+（链接段注册）+ C（抽 tokenizer）的组合。**

三条理由：

1. **A+ 让扩展不再改中心文件**：新增命令族只在自己的 `.cpp` 里加一行 `REGISTER_SHELL_CMD`，`shell.cpp` 一行不动，且帮助文本自动出现。这与项目已有的 `REGISTER_INIT` / `REGISTER_THREAD` / `REGISTER_SHELL_VAR` 完全同构，**不引入新概念**
2. **C 消除 17 处重复的空白跳过代码**：同一段手写 tokenizer 抄了 5 遍，语义容易漂移
3. **两者正交**：A+ 管「命令族怎么找到」，C 管「参数怎么切」，可以独立实施

**不推荐「静态命令表」（方案 A）**：只把 if-else 换成表，新增命令族仍要改 `shell.cpp`，改动量与现状相同，收益为零。

**不推荐「完整命令树」（方案 B）**：约 200 行基础设施、需重新设计所有命令的回调签名、且 Zephyr shell 本身未启用。

**收益本质**：不是代码变少（A+ 反而多约 40 行），而是**扩展点从「中心文件」移到「各自模块」**，让新增命令变成局部改动。

---

## 2. 现状走查

### 2.1 分派结构

两级 `strcmp` 链：

```text
Shell::ProcessLine(line)                     shell.cpp:62
  ├── token1 = 第一个空格前的内容
  └── strcmp(token1, "var")    → Var::Process(line)
      strcmp(token1, "log")    → Log::ProcessLogCommand(line)
      strcmp(token1, "thread") → ThreadCmd::Process(line)
      strcmp(token1, "boot")   → BootCmd::Process(line)
      strcmp(token1, "h"/"?")  → CmdHelp()
```

每个子模块再重复一遍同样的事：

```cpp
// var.cpp:227 / log.cpp:281 / thread_cmd.cpp:172 —— 完全相同的 5 行
while (*line == ' ') line++;
uint8_t *sub = line;
while (*line && *line != ' ') line++;
if (*line == ' ') { *line = '\0'; line++; }
while (*line == ' ') line++;

if (std::strcmp(sub, "list") == 0) { ... }
else if (std::strcmp(sub, "set") == 0) { ... }
```

### 2.2 代码量统计

| 文件 | 行数 | 分派相关 |
|---|---|---|
| `shell.cpp` | 193 | 分派 ~30 行、帮助 14 行 |
| `var/var.cpp` | 254 | 分派 ~25 行 |
| `log/log.cpp` | 340 | 分派 ~45 行（含 `mode` 二级） |
| `thread/thread_cmd.cpp` | 206 | 分派 ~35 行 |
| `boot/boot_cmd.cpp` | 100 | 分派 ~12 行 |
| **合计** | **1093** | **分派 ~147 行 / 13%** |

重复模式：

- `while (*line == ' ')` 等空白跳过：**17 处**
- `std::strcmp(reinterpret_cast<const char*>`：**22 处**

### 2.3 命令全貌

| 命令族 | 子命令 | 参数形态 |
|---|---|---|
| `var` | `list` / `get <name>` / `set <name> <val>` | 0 / 1 / 2 个位置参数 |
| `log` | `list` / `on <name>` / `off` / `mode <name> on\|off` | 0 / 1 / 2 + 枚举 |
| `thread` | `list` / `show <name>` / `pause <name>` / `resume <name>` / `reset <name>` | 0 / 1 个名字 |
| `boot` | `timing` | 0 |
| 顶层 | `h` / `?` | 0 |

共 **14 条完整命令**。

---

## 3. 候选方案

### 3.1 方案 A：静态命令表

```cpp
struct ShellCmd
{
    const char* name;
    void (*handler)(uint8_t* args);
    const char* help;
};

// 仍然写在 shell.cpp 里
static const ShellCmd kCommands[] = {
    { "var",    Var::Process,       "var list|get|set" },
    { "log",    Log::ProcessLogCommand, "log list|on|off|mode" },
    { "thread", ThreadCmd::Process, "thread list|show|pause|resume|reset" },
    { "boot",   BootCmd::Process,   "boot timing" },
    { "h",      CmdHelp,            "help" },
};
```

**省掉的**：`Shell::ProcessLine` 里的 5 路 `strcmp` 链（约 15 行）。

**没省掉的**：每个子模块内部的二级分派——它们参数形态不同，无法塞进同一张表。

**扩展性**：新增命令族仍需**同时**改两处——`shell.cpp` 的表项 + `#include`。与现状（加一个 `else if` + `#include`）**改动量相同**。

**净收益**：约 15 行 → 一张 5 行表 + 8 行遍历。**几乎为零**。

**定位**：作为 A+ 的退化形态保留在文档里，说明「只把 if-else 换成表」并不解决问题。

### 3.2 方案 A+：链接段注册（扩展性最优）

核心思路：**把「名字 → 处理函数」的绑定从 `shell.cpp` 移出去**。每个命令族在自己的文件里声明，`shell.cpp` 只负责遍历一张链接段表。

#### 3.2.1 数据结构

```cpp
/**
 * @brief shell 命令族注册项
 */
struct ShellCmdEntry
{
    const char* name;       // 命令族名（"var" / "log" / "thread" / "boot"）
    void (*handler)(uint8_t* args);   // 族入口，收到参数原始串
    const char* help;       // 帮助摘要（一行）
};

/**
 * @brief 注册命令族
 *
 * 与 REGISTER_INIT / REGISTER_SHELL_VAR 同构：编译期放入 .shell_cmd 段，
 * shell 线程遍历该段匹配命令名。
 *
 * @param name_    命令族名（字符串字面量）
 * @param help_    帮助摘要（字符串字面量）
 * @param handler_ 族入口函数，签名 void(uint8_t* args)
 */
#define REGISTER_SHELL_CMD(name_, help_, handler_)                              \
    static const ShellCmdEntry kShellCmd_##handler_                             \
        __attribute__((used, __section__(".shell_cmd"))) = {                    \
        name_, handler_, help_                                                  \
    }
```

#### 3.2.2 分派实现

```cpp
void Shell::ProcessLine(uint8_t* line)
{
    const char* cmd = nullptr;
    uint8_t* rest   = NextToken(line, cmd);      // 见 §8.1 的 token.hpp

    if (*cmd == '\0') return;

    for (const ShellCmdEntry* e = __shell_cmd_start; e < __shell_cmd_end; ++e)
    {
        if (std::strcmp(e->name, cmd) == 0)
        {
            e->handler(rest);
            return;
        }
    }

    Log::SendCommandLine("?: unknown command");
}
```

#### 3.2.3 各命令族的改动

```cpp
// thread_cmd.cpp 末尾
REGISTER_SHELL_CMD("thread", "thread list|show|pause|resume|reset", ThreadCmd::Process);

// boot_cmd.cpp 末尾
REGISTER_SHELL_CMD("boot", "boot timing", BootCmd::Process);

// log.cpp 末尾
REGISTER_SHELL_CMD("log", "log list|on|off|mode", Log::ProcessLogCommand);

// var.cpp 末尾
REGISTER_SHELL_CMD("var", "var list|get|set", Var::Process);

// shell.cpp 末尾（帮助本身也是一个命令）
REGISTER_SHELL_CMD("h", "help", Shell::CmdHelpStatic);
```

`Shell::ProcessLine` 不再需要 `#include` 任何子模块头文件。

#### 3.2.4 帮助自动生成

`CmdHelp()` 不再手写 14 行，改为遍历段：

```cpp
void Shell::CmdHelp()
{
    for (const ShellCmdEntry* e = __shell_cmd_start; e < __shell_cmd_end; ++e)
    {
        char line[80];
        snprintf(line, sizeof(line), "%-8s %s", e->name, e->help);
        Log::SendCommandLine(line);
    }
}
```

新增命令族时**帮助自动出现**，不会忘。

#### 3.2.5 链接段

在 `framework/cmd/linker/dust_init.ld` 增加：

```ld
SECTION_PROLOGUE(.shell_cmd,,)
{
    __shell_cmd_start = .;
    KEEP(*(.shell_cmd))
    __shell_cmd_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)
```

只读段（命令族名与函数指针在启动后不变），用 `ROM_SECTIONS` 已有的 `dust_init.ld`，**不需要新增链接脚本**。

#### 3.2.6 与项目现有机制的一致性

| 注册对象 | 段 | 消费者 | 宏 |
|---|---|---|---|
| 初始化项 | `.user_init` | `RunStage(InitStage)` | `REGISTER_INIT` |
| 线程项 | `.user_thread` | `RunStage(ThreadStage)` | `REGISTER_THREAD` |
| 调试变量 | `.shell_var` | `Var::CmdList` | `REGISTER_SHELL_VAR` |
| 遥控协议 | `.remote` | `Remote` 模块 | `REGISTER_REMOTE` |
| IMU 设备 | `.imu` | `ImuManager` | `REGISTER_IMU` |
| **命令族** | **`.shell_cmd`** | **`Shell::ProcessLine`** | **`REGISTER_SHELL_CMD`** |

**这不是引入新概念，是把已有的注册模式再复制一份。**

#### 3.2.7 收益

| 项 | 现状 | A+ |
|---|---|---|
| 新增命令族的改动 | `shell.cpp` 加 `else if` + `#include` | **只在自己的文件里加一行宏**，`shell.cpp` 不动 |
| 帮助文本 | 手写 14 行，容易漏 | **自动生成**，不会漏 |
| 未知命令提示 | `?: var/log/thread/boot/h`（硬编码） | `?: unknown command`（无需维护列表） |
| 与项目架构一致性 | 独立于其它注册机制 | 与 `.shell_var` 等同构 |
| 分派代码 | ~15 行 if-else 链 | ~10 行遍历 |

#### 3.2.8 代价

| 项 | 值 |
|---|---|
| 新增基础设施 | `ShellCmdEntry` + 宏 + `CmdHelp` 遍历，约 40 行 |
| RAM | `12 B × 命令族数`，当前 5 个 = **60 B** |
| Flash | 名字串 + 帮助串指针 |
| 新增段 | `.shell_cmd`（加进 `dust_init.ld`，约 6 行） |
| 改动文件 | 5 个（`shell.cpp` / `var.cpp` / `log.cpp` / `thread_cmd.cpp` / `boot_cmd.cpp`）+ 链接脚本 |

#### 3.2.9 两个必须处理的问题

**① 顺序不确定**

`.shell_cmd` 段内顺序由链接器决定，`CmdHelp()` 的输出行序会不稳定（今天 `var` 在前，明天可能 `boot` 在前）。

三种处理：

| 方案 | 做法 | 代价 |
|---|---|---|
| **不处理** | 接受乱序 | 0，但帮助每次顺序可能变 |
| **加 order 字段** | `ShellCmdEntry` 增加 `uint8_t order`，遍历时按序数排序输出 | 每条 1 B + 帮助函数里做插入排序 |
| **按名字排序输出** | 帮助输出前对指针数组排序 | 需要 `n` 个指针的栈数组 |

**建议**：先不做，观察实际排序是否稳定（GCC 通常按目标文件顺序拼接，而目标文件顺序跟随 `target_sources`）。如果不稳定再加 `order`。

**② 帮助文本粒度变粗**

现状：14 条命令逐条说明（`var list  列出所有调试变量` ...）
A+：每族一行摘要（`var      var list|get|set`）

如果仍需逐条说明，可以让 `help` 字段存多行串（`"list 列出所有变量
get <name> 查看变量"`），`CmdHelp` 按 `
` 拆分输出。**代价是宏参数里要写 `
`**，可读性下降。

**建议**：接受一行摘要，详细用法由各族的 `?: ...` 提示承担（现状已有）。

### 3.3 方案 B：完整命令树（Zephyr 风格）

模仿 Zephyr `SHELL_CMD_REGISTER` 的分层 `shell_static_entry`：

```cpp
SHELL_CMD_REGISTER(var, &var_cmds, "debug variables", NULL);
SHELL_STATIC_SUBCMD_SET_CREATE(var_cmds,
    SHELL_CMD(list, NULL, "list all", Var::CmdList),
    SHELL_CMD_ARG(set, NULL, "set <name> <val>", Var::CmdSet, 3, 0),
    SHELL_SUBCMD_SET_END
);
```

**代价**：

- 需要重新设计 `var` / `log` / `thread` 的回调签名，每个命令一个函数
- `var set` 的类型解析、`log mode` 的二值枚举都还要自己写
- 引入约 200 行的命令树基础设施（如果不用 Zephyr shell，得自己实现）
- **Zephyr shell 本身未启用**（`CONFIG_SHELL` 未 set），引入它意味着放弃自研日志系统与 shell 的深度耦合

**收益**：

- 自动帮助生成
- 前缀补全（需要 Tab 键支持，当前串口终端没做）
- 参数个数校验

**对当前场景**：收益全是「将来可能有」，代价是立刻背上 200 行。

### 3.4 方案 C：只抽 tokenizer

```cpp
/**
 * @brief 取出下一个空格分隔的 token
 * @param line 输入指针（原地修改，token 后写入 '\0'）
 * @param out  输出：token 起始地址
 * @return 下一个 token 的起始位置；nullptr=已到结尾
 */
static uint8_t* NextToken(uint8_t* line, const char*& out);

/**
 * @brief 剩余内容整体作为参数（用于 var set 的 "<val>" 这类尾部参数）
 */
static uint8_t* RestOfLine(uint8_t* line, const char*& out);
```

调用方变成：

```cpp
const char* cmd = nullptr;
uint8_t* rest = NextToken(line, cmd);
if (std::strcmp(cmd, "var") == 0) Var::Process(rest);
```

**收益**：

- 17 处重复的空白跳过代码 → 1 处
- token 边界语义统一（连续空格、行尾空格行为一致）
- 各子模块的二级分派仍然保留（因为参数形态不同）

**代价**：约 20 行工具 + 5 个文件各改 3 行。

### 3.5 方案 D：词法分析器（真正意义上的 lexer）

把整行切成 token 数组：

```cpp
struct Token { const char* start; uint8_t len; };

uint8_t Tokenize(uint8_t* line, Token* out, uint8_t max);
```

**优点**：错误提示可以更准（"多余的参数"、"缺少参数"）

**缺点**：需要 `Token[8]` 栈数组（约 64 B）+ 长度判断逻辑；当前命令最多 3 个 token，`NextToken` 的流式取法已经够用

**结论**：过度设计。

---

## 4. 对比矩阵

| 维度 | 现状 | A 静态表 | **A+ 段注册** | B 命令树 | C tokenizer | D lexer |
|---|---|---|---|---|---|---|
| 新增命令族改动 | 改 shell.cpp + include | 改 shell.cpp + include | **只改自己的文件** | 改自己的文件 | 不涉及 | 不涉及 |
| 分派代码行数 | ~147 | ~132 | ~130 | ~100 | ~130 | ~125 |
| 新增基础设施 | 0 | ~10 | ~40 | ~200 | ~20 | ~60 |
| 帮助文本 | 手写 14 行 | 表内 | **自动生成** | **自动** | 手写 | 手写 |
| 参数校验 | 无 | 无 | 无 | **自动** | 无 | 可做 |
| 前缀补全 | 无 | 无 | 无 | 有 | 无 | 无 |
| 与项目注册模式一致 | 否 | 否 | **是**（同 `.shell_var`） | 否 | — | — |
| 二级分派重复 | 保留 | 保留 | 保留 | 消除 | 保留 | 保留 |
| RAM 增量 | 0 | 24 B | 60 B | ~100 B | 0 | 0 |
| 改动文件 | 0 | 1 | 6 | 5 | 6 | 6 |
| 风险 | 无 | 低 | 低 | **高** | 低 | 中 |
| **结论** | — | ❌ 收益为零 | ✅ **推荐** | ❌ 过重 | ✅ **配套做** | ❌ 过度设计 |

---

## 5. 现状的具体问题（按严重度）

### 5.1 高：重复的空白跳过逻辑

同一段 5 行代码在 5 个文件里复制（`shell.cpp:64-71`、`var.cpp:227-231`、`log.cpp:281-285`、`thread_cmd.cpp:172-176`、`boot_cmd.cpp:88`）。

**风险**：某处改了语义（比如允许多个空格）其它处不同步，行为不一致。

**修法**：方案 C。

### 5.2 中：错误提示不一致

| 位置 | 提示 |
|---|---|
| `shell.cpp:93` | `?: var/log/thread/boot/h` |
| `var.cpp:252` | `?: var list\|get <name>\|set <name> <val>` |
| `log.cpp:323` | `?: log list\|on <name>\|off\|mode <name> on\|off` |
| `thread_cmd.cpp:203` | `?: thread list\|show\|pause\|resume\|reset <name>` |
| `boot_cmd.cpp:97` | `?: boot timing` |

格式大体一致（`?: ` 前缀 + `|` 分隔），但没有统一来源。

**修法**：如果做方案 A，可以顺带把 help 字符串放进表里，一处维护。

### 5.3 低：缺少参数个数校验

```text
thread show          → arg = ""（空串），Find("") 返回 nullptr → "thread show: not found"
thread pause         → 同上
var set              → "usage: var set <name> <value>"（这个有校验）
```

`thread` / `log` / `boot` 都靠下游函数自己判断空串，提示语不统一（`not found` 而不是 `missing argument`）。

**修法**：各命令入口加 `if (*arg == '\0') { SendCommandLine("usage: ..."); return; }`——约 8 行，不需要架构改动。

### 5.4 低：不支持引号/转义

`var set name "hello world"` 会得到 `"hello` 和 `world"` 两个 token。

**实际影响**：当前没有任何命令需要带空格的参数（名字都是标识符、值是数字），**不需要处理**。

### 5.5 信息：`log mode` 的二级分派最深

```text
ProcessLine → log → mode → <name> on|off
```

三层。但这是命令语义本身要求的，不是架构问题。

---

## 6. 性能评估

### 6.1 当前开销

一次 `thread show test` 的解析成本：

| 步骤 | 估计周期数（RISC-V @ 480 MHz） |
|---|---|
| 空白跳过 + token 切分 | ~50 |
| 顶层 `strcmp` × 4（最坏） | ~120 |
| 二级 `strcmp` × 5（最坏） | ~100 |
| **合计** | **~270 周期 ≈ 0.6 µs** |

对比：

- 一次 `SendCommandLine` 的日志入队 + DMA 提交：**数百周期**
- UART 传一行 20 字符 @ 921600：**~217 µs**

**结论**：分派开销比一次串口传输低 3 个数量级，**性能不是考虑因素**。

### 6.2 命令表能不能更快

查找从 `O(n)` 的 `strcmp` 链变成 `O(n)` 的数组遍历 + `strcmp`——**没有本质变化**。

真要快可以：

- 首字符 `switch`（`case 'v': case 'l': case 't': case 'b':`）——能省一半 `strcmp`
- 完美哈希——过度设计

**都不值**。

### 6.3 内存对比

| 方案 | RAM | Flash |
|---|---|---|
| 现状 | 0 | ~0（代码即数据） |
| A 顶层表 | 24 B（6 × 4 B 指针） | ~60 B 字符串 |
| B 完整命令树 | ~100 B | ~400 B |
| C tokenizer | 0 | ~20 行代码 |

当前固件：`text 91 KB / data 6.9 KB / bss 36.8 KB`（`zephyr.elf`）。**命令表无论哪种方案都不会显著改变这个量级**。

---

## 7. 市面方案参考

| 方案 | 特点 | 本项目适配度 |
|---|---|---|
| **Zephyr shell** | 分层命令树 + 自动补全 + 运行时遍历 | ❌ 未启用 `CONFIG_SHELL`，且与自研 log 系统深度耦合 |
| **FreeRTOS CLI** | 一维命令表 + 回调，无树形 | ⚠️ 接近方案 A，收益同样有限 |
| **letter-shell** （国内 MCU 常用） | 命令表 + 参数自动切分 + 变量绑定 | ⚠️ 参数自动切分值得抄（即方案 C/D） |
| **libcli** | 命令树 + 自动帮助 | ❌ 带 `malloc`，不适合 |
| **自研（现状）** | strcmp 链 | ✅ 规模匹配 |

**letter-shell 的设计值得注意**：它的命令表是

```c
typedef struct {
    const char *name;
    const char *help;
    void (*func)(void);       // 无参数
    void (*func_arg)(char*);  // 带原始参数串
} shellCommand;
```

**只区分「有参/无参」两态，不做参数个数校验**——和当前手写的 `Process(uint8_t*)` 接口本质上一样。这说明**在 MCU 规模下，命令表的实际形态就是「一层 `strcmp` + 原始参数串转发」**。

这正好印证了 A+ 的设计：`ShellCmdEntry` 只存 `name` / `handler` / `help` 三个字段，handler 收原始参数串，**不做参数个数校验**——与 letter-shell 的选择一致。

**A+ 相对 letter-shell 的改进**：用链接段注册代替静态数组，新增命令族不再改中心文件，这与本项目已有的 `.shell_var` / `.user_init` 等机制同构。

---

## 8. 推荐方案

### 8.0 总览

| 方案 | 做不做 | 理由 |
|---|---|---|
| **A+ 链接段注册** | ✅ **做** | 扩展点从中心文件移到各自模块，与项目注册模式同构 |
| **C tokenizer** | ✅ **做** | 17 处重复归并，语义统一 |
| 参数缺失提示 | ✅ 做 | 低成本，提示更准确 |
| A 静态表 | ❌ 不做 | 与现状改动量相同，收益为零 |
| B 命令树 | ❌ 不做 | 200 行基础设施，收益在将来 |
| D lexer | ❌ 不做 | 过度设计 |

**A+ 与 C 正交，可独立实施。** 建议先做 A+（结构改动），再做 C（工具抽取），最后补参数提示。

### 8.1 做①：A+ 链接段注册

见 §3.2 完整设计。核心改动三处：

**新建 `framework/cmd/shell/shell_cmd.hpp`**（约 40 行）

```cpp
/**
 * @brief shell 命令族注册项
 */
struct ShellCmdEntry
{
    const char* name;       // 命令族名（"var" / "log" / "thread" / "boot"）
    void (*handler)(uint8_t* args);   // 族入口，收到参数原始串
    const char* help;       // 帮助摘要（一行）
};

/**
 * @brief 注册命令族
 *
 * 编译期放入 .shell_cmd 段，shell 线程遍历该段匹配命令名。
 *
 * @param name_    命令族名（字符串字面量）
 * @param help_    帮助摘要（字符串字面量）
 * @param handler_ 族入口函数，签名 void(uint8_t* args)
 */
#define REGISTER_SHELL_CMD(name_, help_, handler_)                              \
    static const ShellCmdEntry kShellCmd_##handler_                             \
        __attribute__((used, __section__(".shell_cmd"))) = {                    \
        name_, handler_, help_                                                  \
    }

/**
 * @brief 段边界（由 dust_init.ld 导出）
 */
inline const ShellCmdEntry* ShellCmdBegin() { return __shell_cmd_start; }
inline const ShellCmdEntry* ShellCmdEnd()   { return __shell_cmd_end; }

extern const ShellCmdEntry __shell_cmd_start[];
extern const ShellCmdEntry __shell_cmd_end[];
```

**改 `shell.cpp` 的 `ProcessLine`**

```cpp
void Shell::ProcessLine(uint8_t* line)
{
    const char* cmd = nullptr;
    uint8_t* rest   = NextToken(line, cmd);      // 见 §8.2
    if (*cmd == '\0') return;

    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        if (std::strcmp(e->name, cmd) == 0) { e->handler(rest); return; }
    }
    Log::SendCommandLine("?: unknown command");
}
```

**改 `shell.cpp` 的 `CmdHelp`**

```cpp
void Shell::CmdHelp() const
{
    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        char line[80];
        snprintf(line, sizeof(line), "%-8s %s", e->name, e->help);
        Log::SendCommandLine(line);
    }
}
```

**各模块末尾加一行**

```cpp
// var.cpp
REGISTER_SHELL_CMD("var", "var list|get|set", Var::Process);
// log.cpp
REGISTER_SHELL_CMD("log", "log list|on|off|mode", Log::ProcessLogCommand);
// thread_cmd.cpp
REGISTER_SHELL_CMD("thread", "thread list|show|pause|resume|reset", ThreadCmd::Process);
// boot_cmd.cpp
REGISTER_SHELL_CMD("boot", "boot timing", BootCmd::Process);
```

**链接脚本加段**（`framework/cmd/linker/dust_init.ld`）

```ld
SECTION_PROLOGUE(.shell_cmd,,)
{
    __shell_cmd_start = .;
    KEEP(*(.shell_cmd))
    __shell_cmd_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)
```

**注意**：`Shell::CmdHelp` 是 const 成员函数，但 `REGISTER_SHELL_CMD` 需要 `void(uint8_t*)` 签名。把 `CmdHelp` 改成静态函数 `Shell::HelpStatic(uint8_t*)`，或者注册一个薄包装：

```cpp
static void HelpCmd(uint8_t*) { ... }     // 文件内自由函数
REGISTER_SHELL_CMD("h", "help", HelpCmd);
```

### 8.2 做②：抽出 `NextToken()`，替换 17 处重复

新建 `framework/cmd/shell/token.hpp`：

```cpp
/**
 * @brief 跳过多余空格
 * @param p 输入指针
 * @return 第一个非空格字符的位置
 */
inline uint8_t* SkipSpace(uint8_t* p)
{
    while (*p == ' ') ++p;
    return p;
}

/**
 * @brief 取出下一个空格分隔的 token（原地写 '\0'）
 * @param line 输入指针
 * @param out  输出：token 起始
 * @return 剩余内容起点；*out 为空串表示无 token
 */
inline uint8_t* NextToken(uint8_t* line, const char*& out)
{
    line = SkipSpace(line);
    out  = reinterpret_cast<const char*>(line);

    while (*line && *line != ' ') ++line;
    if (*line == ' ') { *line++ = '\0'; }

    return SkipSpace(line);
}
```

调用方（A+ 之后，`shell.cpp` 只需取 token，不再写 strcmp 链）：

```cpp
const char* cmd = nullptr;
uint8_t* rest = NextToken(line, cmd);
if (*cmd == '\0') return;

for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e) { ... }
```

各子模块内部：

```cpp
const char* sub = nullptr;
uint8_t* args = NextToken(line, sub);
if (std::strcmp(sub, "set") == 0) { ... }
```

**收益**：语义统一、17 处重复变 1 处。
**代价**：1 个新头文件（~30 行）+ 5 个文件各改 3-6 行。

### 8.2.1 统一参数缺失提示

每个 `CmdXxx(const char* name)` 开头加：

```cpp
if (name == nullptr || *name == '\0') {
    Log::SendCommandLine("usage: thread show <name>");
    return;
}
```

约 8 处、共 16 行。提示从 `not found`（误导）变成 `usage`（准确）。

### 8.3 不做

**① 静态命令表（方案 A）**——把 if-else 换成表，新增命令族仍改 `shell.cpp`，改动量与现状相同。

**② Zephyr shell**——未启用、耦合深、体量大。

**③ 完整 lexer / 引号转义**——当前无需求。

### 8.4 本节的实施顺序

```text
① shell_cmd.hpp + .shell_cmd 段 + ProcessLine/CmdHelp 改造   （结构）
② token.hpp + 5 处调用点替换                                  （工具）
③ 参数缺失提示                                                （细节）
```

每步独立可编译、可验证。

### 8.5 将来若要再进一步

触发条件（满足任一条再考虑）：

- 命令数 > 30
- 需要「帮助自动生成」或「Tab 补全」
- 需要「命令权限分级」（比如危险命令需要额外确认）

届时的推荐形态：**A+ 的基础上扩展表项**，增加参数个数校验与补全回调：

```cpp
struct ShellCmd
{
    const char* name;
    void (*handler)(uint8_t* args);
    const char* help;
};
```

顶层表负责族名匹配与帮助输出，族内仍按参数语义自由解析。

---

## 9. 具体改动清单（推荐方案）

### 9.1 A+：新增 `shell_cmd.hpp` 和链接段

| 文件 | 改动 |
|---|---|
| `framework/cmd/shell/shell_cmd.hpp` | **新增**：`ShellCmdEntry` + `REGISTER_SHELL_CMD` + 段边界访问（~40 行） |
| `framework/cmd/linker/dust_init.ld` | **追加** `.shell_cmd` 段（6 行） |
| `framework/cmd/shell/shell.cpp` | `ProcessLine` 改为遍历段；`CmdHelp` 改为遍历段输出；删 5 路 strcmp 链与 5 个子模块 `#include` |
| `framework/cmd/shell/var/var.cpp` | 末尾加 `REGISTER_SHELL_CMD("var", ...)` |
| `framework/cmd/shell/log/log.cpp` | 末尾加 `REGISTER_SHELL_CMD("log", ...)` |
| `framework/cmd/shell/thread/thread_cmd.cpp` | 末尾加 `REGISTER_SHELL_CMD("thread", ...)` |
| `framework/cmd/shell/boot/boot_cmd.cpp` | 末尾加 `REGISTER_SHELL_CMD("boot", ...)` |
| `framework/cmd/CMakeLists.txt` | 无需改（include 目录已含 `shell/`） |

**帮助命令**：`Shell::CmdHelp` 是 const 成员函数，签名不匹配 `void(uint8_t*)`。在 `shell.cpp` 里写一个自由函数：

```cpp
static void HelpCmd(uint8_t*) { Shell::PrintHelp(); }
REGISTER_SHELL_CMD("h", "help", HelpCmd);
```

`<?>` 作为别名可以再注册一条同名 handler。

**验证**：

```text
h                    → 5 行命令族摘要（顺序可能不稳定，见 §3.2.9）
var list             → 正常
log mode vofa on     → 正常
thread show test     → 正常
bogus                → ?: unknown command
```

### 9.2 C：新增 `token.hpp` 并替换 5 处

| 文件 | 改动 |
|---|---|
| `framework/cmd/shell/token.hpp` | **新增**：`SkipSpace` + `NextToken`（~30 行） |
| `shell.cpp` | `ProcessLine` 用 `NextToken` |
| `var/var.cpp` | `Process` 用 `NextToken`（含 `set` 的二级切分） |
| `log/log.cpp` | `ProcessLogCommand` 用 `NextToken`（含 `mode` 的二级切分） |
| `thread/thread_cmd.cpp` | `Process` 用 `NextToken` |
| `boot/boot_cmd.cpp` | `Process` 用 `NextToken` |

**验证**：连续空格、行尾空格、空参数三种输入行为与改动前一致。

### 9.3 参数缺失提示

`ThreadCmd::CmdShow/CmdPause/CmdResume/CmdReset`、`Log::ProcessLogCommand` 的 `on` 分支、`BootCmd::Process` 各加 2 行空参判断。

**验证**：

```text
thread show          → usage: thread show <name>
thread pause         → usage: thread pause <name>
log on               → usage: log on <name>
var set              → usage: var set <name> <value>（已有）
thread list          → 正常输出
log mode vofa on     → 正常输出
```

---

## 10. 附录：当前命令分发完整清单

```text
ProcessLine
├── var  → Var::Process
│          ├── list              → CmdList()
│          ├── get <name>        → CmdGet()
│          └── set <name> <val>  → CmdSet()
├── log  → Log::ProcessLogCommand
│          ├── list              → PrintLogList()
│          ├── on <name>         → SelectDebugEntry()
│          ├── off               → DeselectDebugEntry()
│          └── mode <name> on|off → SetDebugVofa()
├── thread → ThreadCmd::Process
│          ├── list              → CmdList()
│          ├── show <name>       → CmdShow()
│          ├── pause <name>      → CmdPause()
│          ├── resume <name>     → CmdResume()
│          └── reset <name>      → CmdReset()
├── boot → BootCmd::Process
│          └── timing            → CmdTiming()
└── h / ? → CmdHelp()
```

**共 14 条完整命令，最深 3 层（`log mode <name> on`）。**
