# 线程层借鉴 Dust_Vision 规划

> 日期：2026-10-19
> 来源：`temp/Dust_Vision_Architecture-main/framework/init/`（用户独立搭建的 Linux 嵌入式视觉架构，从本子树架构重写）
> 目标：把 Dust_Vision 线程层中**适用于 MCU** 的设计吸收到 `framework/init`，重点是「任务体与错误处理分离」。
> 边界：只动 `framework/init` 与各处的注册调用点，不改业务逻辑、不改算法/模块分层。

---

## 1. 先划清：哪些能学，哪些不能

### 1.1 Dust_Vision 与本子树的关系

Dust_Vision 是 Linux 用户态进程架构：`std::thread` + `pthread` + 信号 + 异常 + 进程退出。它的线程层解决的是「进程什么时候启动、出错怎么办、什么时候优雅退出」。

本子树是 Zephyr 裸机/RTOS：线程常驻、无进程边界、无退出语义、异常默认关闭。所以**结构可以学，机制必须换**。

### 1.2 明确不能照搬的四项

| Dust_Vision 做法 | 为什么不能照搬 | 本子树的替代 |
|---|---|---|
| `try / catch / throw` | Zephyr 默认 `-fno-exceptions`，开了要背 unwinder 表 + 栈展开开销 + ISR 禁抛 | **返回状态码**（`TaskResult`） |
| `dynamic_cast` / RTTI | `CONFIG_CPP_RTTI` 未开启，`-fno-rtti` | 错误码 / 枚举分类 |
| `System_WaitForStop` + `System_Shutdown` 进程退出 | MCU 没有进程生命周期，`main` 常驻 | 只保留**线程级** stop/join；系统级 shutdown 仅服务于软重启 |
| `System_SetMainLoop`（Qt 主线程约束） | MCU 无 GUI 线程约束 | 不引入 |

**证据**：

- [project/build/zephyr/.config:887](../../project/build/zephyr/.config#L887) `# CONFIG_CPP_EXCEPTIONS is not set`
- [project/build/zephyr/.config:888](../../project/build/zephyr/.config#L888) `# CONFIG_CPP_RTTI is not set`
- `E:/Zephyr/zephyr/CMakeLists.txt:378-386`：未开这两个符号时，对全部 C++ 编译单元注入 `no_exceptions` / `no_rtti` 编译选项
- `E:/Zephyr/zephyr/lib/cpp/Kconfig:157-166`：两个符号都存在，但默认 n

### 1.3 值得学的六项（评级）

| # | Dust_Vision 的设计 | 对本子树的价值 | 建议 |
|---|---|---|---|
| A | `Thread::Run` 外壳：任务只写正常路径，异常由外壳统一处理 | **高**——当前所有 `Task` 都是裸函数指针，出错只能就地写分支 | 做 |
| B | `WaitForStop(duration)` 取代不可中断 sleep | **高**——是 A 和 C 成立的前提 | 做 |
| C | `RequestStop + Join` 完整线程生命周期 | **中高**——支持线程内重启、软重启、模块卸载 | 做 |
| D | `REGISTER_THREAD` 带 stop/join 回调，启动器统一收尾 | **中**——只在需要软重启时体现价值 | 做（轻量） |
| E | `REGISTER_INIT` 带 `cleanup`，支持部分初始化回滚 | **低中**——只在 `ContinueOnFail` 降级运行时有用 | 可选 |
| F | 链接段内存**指针**而非结构体 | **低**——本质是 ABI/布局防御 | 可选 |

> **G（新增，非 Dust_Vision 原生）**：线程运行统计 + `thread` shell 命令。Dust_Vision 没有这项，但它的 `Run` 外壳正好是挂统计的唯一位置——**统计不放业务 Task，放外壳**，与 A 同源。

### 1.4 统计与外壳的关系（G）

`Run` 外壳是每个任务体的**唯一入口**，因此：

- 运行次数、Retry/Fatal/Exit 次数、单次耗时，全部由外壳自动采集；
- 业务 `Task` 不需要写一行统计代码；
- 统计对所有线程格式统一，shell 命令可以统一遍历。

这与「任务体与错误处理分离」是同一个设计：**跨线程关心的东西放外壳，具体业务放 Task**。

---

## 2. 优点 A：任务体与错误处理分离（最高优先级）

### 2.1 现状：错误处理只能挤在 Task 里

当前 `Thread` 只是 `k_thread_create` 的薄封装（[framework/init/thread.hpp:36-51](../../framework/init/thread.hpp#L36-L51)）：

```cpp
template<uint32_t StackSize = 1024>
class Thread final
{
public:
    void Start(k_thread_entry_t entry, ThreadPrio prio = ThreadPrio::Normal, void* p1 = nullptr, const char* name = nullptr)
    {
        k_thread_create(&thread_, stack_, K_THREAD_STACK_SIZEOF(stack_),
                        entry, p1, nullptr, nullptr, static_cast<uint8_t>(prio), 0, K_NO_WAIT);
        if (name != nullptr) {
            k_thread_name_set(&thread_, name);
        }
    }
private:
    k_thread thread_ {};
    K_KERNEL_STACK_MEMBER(stack_, StackSize);
};
```

`entry` 直接是业务 `Task`，框架层对「任务怎么结束、出错怎么办」一无所知：

- [framework/modules/imu/drivers/imu.cpp:102](../../framework/modules/imu/drivers/imu.cpp#L102)：`source_->Read(sample_)` 失败直接跳过，不记录、不重试、不退出；
- [framework/modules/remotes/remote.cpp:27-51](../../framework/modules/remotes/remote.cpp#L27-L51)：串口超时只在 `else` 分支里处理，没有任何终止路径；
- [project/thread/gpio/trd_gpio.cpp:27-48](../../project/thread/gpio/trd_gpio.cpp#L27-L48)：无限循环，无退出条件。

结果是：**每个业务线程都要自己决定「错误要不要记、要不要重试、要不要退出」**，写法各不相同。

### 2.2 Dust_Vision 的做法

`Thread::Run`（`temp/.../framework/init/thread.hpp:192-237`）作为统一入口：

```cpp
void Run(Entry entry, void* p1)
{
    try
    {
        while (!StopRequested())
        {
            try { entry(p1, nullptr, nullptr); return; }
            catch (const std::exception& error)
            {
                if (StopRequested()) return;
                if (recovery_hook_ == nullptr) throw;
                const auto action = recovery_hook_(error, *this, recovery_context_);
                if (StopRequested()) return;
                if (action != RecoveryAction::Restart) throw;
            }
        }
    }
    catch (const std::exception& error) { System_RequestStop(1); DUST_LOG_ERR("%s", error.what()); }
    catch (...) { System_RequestStop(1); }
}
```

带来的效果：`Task` 里没有一行 try/catch，掉线直接抛，重连逻辑全在 `Recover()` 里（`trd_camera.cpp:272-321`）。

### 2.3 MCU 等价物：`TaskResult` + 恢复钩子

用返回值代替异常：

```cpp
/**
 * @brief 线程任务结果
 */
enum class TaskResult : uint8_t
{
    Exit  = 0,   // 任务正常结束：线程退出，不重进
    Retry = 1,   // 可恢复错误：交给恢复钩子决定是否重进任务体
    Fatal = 2,   // 不可恢复错误：记录后线程退出
};

using TaskFunc     = TaskResult (*)(void* context);
/**
 * @brief 恢复钩子：仅当任务返回 Retry 时调用
 * @return true=重进任务入口；false=线程退出
 */
using RecoveryHook = bool (*)(void* context);
```

外壳：

```cpp
static void Run(void* p1, void*, void*)
{
    auto* self = static_cast<Thread*>(p1);

    for (;;)
    {
        if (self->stop_requested_) break;

        atomic_inc(&self->run_count_);                 // 任务体每进入一次计一次
        const uint64_t begin_cycle = k_cycle_get_64();

        const TaskResult result = self->task_(self->context_);

        const uint64_t elapsed = k_cycle_get_64() - begin_cycle;
        self->run_cycles_ += elapsed;
        if (elapsed > self->max_run_cycles_) self->max_run_cycles_ = elapsed;

        if (result == TaskResult::Exit) break;

        if (result == TaskResult::Fatal)
        {
            atomic_inc(&self->fatal_count_);
            DUST_LOG_ERR("thread %s fatal", self->name_);
            break;
        }

        if (self->stop_requested_) break;

        atomic_inc(&self->retry_count_);               // Retry 次数即恢复次数
        if (self->hook_ == nullptr || !self->hook_(self->context_)) break;
    }
}
```

**统计字段（§2.5）**：`Thread` 内挂 4 个计数器 + 3 个耗时量，`thread` shell 命令读出。计数器用 `atomic_inc`（ISR 上下文安全），耗时量只由被管理线程自己写、shell 侧只读，无需加锁。

**与 Dust_Vision 的语义一一对应**：

| Dust_Vision | 本子树 |
|---|---|
| `entry()` 正常 return | `return TaskResult::Exit` |
| 抛 `HikRobotError(recoverable)` | `return TaskResult::Retry` |
| 抛不可恢复异常 | `return TaskResult::Fatal` |
| `RecoveryAction::Restart` | 钩子返回 `true` |
| `RecoveryAction::Stop` | 钩子返回 `false` |
| `recovery_hook_ == nullptr` → 统一退出 | `hook_ == nullptr` → 线程退出 |

### 2.4 契约（必须写进注释）

1. **跨重启状态放 Task 之外的 static**——`Restart` 是重新进入函数体，Task 内的局部变量会重置。参见 `trd_camera.cpp` 把 `camera_` / `camera_ready_` / `max_frame_gap_ms_` 全部放在命名空间作用域。
2. **恢复钩子只处理"当前这次运行"的现场**——资源重建放在钩子里，状态清理也在钩子里。
3. **`Fatal` 不走钩子**——避免不可恢复错误被钩子误判成可重试。
4. **钩子不得阻塞过久**——它在被管理线程自己的上下文里执行。

### 2.5 线程运行统计（计数器 + shell 命令）

**需求**：每个线程都有运行次数、错误次数、耗时统计，通过 shell 命令查看。

#### 2.5.1 统计字段

| 字段 | 类型 | 更新位置 | 含义 |
|---|---|---|---|
| `run_count_` | `atomic_t` | `Run` 每次进入 Task 前 `atomic_inc` | **任务体运行次数**（重进一次算一次） |
| `retry_count_` | `atomic_t` | Task 返回 `Retry` 时 `atomic_inc` | 可恢复错误次数（= 恢复钩子调用次数） |
| `fatal_count_` | `atomic_t` | Task 返回 `Fatal` 时 `atomic_inc` | 不可恢复错误次数 |
| `exit_count_` | `atomic_t` | Task 返回 `Exit` 时 `atomic_inc` | 正常退出次数 |
| `run_cycles_` | `uint64_t` | 每次 `Run` 累加 | 任务体累计执行周期数 |
| `max_run_cycles_` | `uint64_t` | 每次 `Run` 取最大值 | 单次最长执行周期数 |
| `last_cycles_` | `uint64_t` | 每次 `Run` 覆盖 | 最近一次执行周期数 |
| `drop_count_` | `atomic_t` | 业务代码显式调用 `CountDrop()` | 业务级丢帧/丢包计数（可选） |

**为什么用 `k_cycle_get_64`**：

- `CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER=y`（HPM5361 当前配置已具备）
- 直接读硬件计数器，无 syscall、无锁，任务体测量开销 ≈ 几条指令
- 转 ns 用 `k_cyc_to_ns_floor64()`，与 [imu.cpp:91](../../framework/modules/imu/drivers/imu.cpp#L91) 现有写法一致

> **注意**：49 天溢出（24MHz 下 `2^64/24e6 ≈ 24.3 年`，不构成问题）；`run_cycles_` 用 `uint64_t` 累加也不会溢出。

#### 2.5.2 统计的取值时机

`run_count_` / `retry_count_` 等只在 **`Thread::Run` 外壳**里更新——业务 `Task` 一行都不用写。这正是「任务体与错误处理分离」的延伸：**任务体也实例化统计**。

业务想要额外指标（如丢帧），调用：

```cpp
thread_.CountDrop();   // 对应 drop_count_
```

#### 2.5.3 读取接口

```cpp
/**
 * @brief 线程运行统计快照
 */
struct ThreadStats
{
    const char* name;              // 线程名
    uint32_t    run_count;         // 任务体进入次数
    uint32_t    retry_count;       // 可恢复错误次数
    uint32_t    fatal_count;       // 不可恢复错误次数
    uint32_t    exit_count;        // 正常退出次数
    uint32_t    drop_count;        // 业务计数
    uint64_t    run_ns;            // 任务体累计执行时间
    uint64_t    max_run_ns;        // 单次最长执行时间
    uint64_t    last_run_ns;       // 最近一次执行时间
    uint32_t    stack_unused;      // 栈剩余字节（见下）
    bool        running;           // 线程是否还活着
    bool        stop_requested;    // 是否已请求停止
};

/**
 * @brief 取统计快照（任意上下文可调用，不阻塞）
 * @param out 输出快照
 * @return true=数据有效
 */
bool GetStats(ThreadStats& out) const;
```

**栈剩余量**：`k_thread_stack_space_get(&thread_, &unused)`，前提是 `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO`——当前 [project/prj.conf:17-18](../../project/prj.conf#L17-L18) 已开 `CONFIG_INIT_STACKS=y` / `CONFIG_THREAD_STACK_INFO=y`，`project/build/zephyr/.config:68,528` 确认生效。无这两个符号时返回 0，命令显示 `n/a`。

#### 2.5.4 线程注册表

统计需要「能枚举所有线程」，因此 `Thread` 在 `Start()` 时把自己登记到一张静态表：

```cpp
// framework/init/thread.hpp（或在 Init_entry.cpp 内）
constexpr uint8_t kMaxThreads = 16;

/**
 * @brief 线程注册表：供 shell 命令遍历
 */
class ThreadRegistry
{
public:
    static bool Add(ThreadBase* t);          // 容量满返回 false
    static ThreadBase* At(uint8_t i);
    static uint8_t Count();
};
```

`ThreadBase` 是非模板基类（`Thread<StackSize>` 继承它），因为模板实例化后类型不同，注册表必须存基类指针：

```cpp
/**
 * @brief 线程非模板基类：只暴露统计与生命周期接口
 */
class ThreadBase
{
public:
    virtual const char* Name() const = 0;
    virtual bool GetStats(ThreadStats& out) const = 0;
    virtual void RequestStop() = 0;
    virtual int  Join(k_timeout_t timeout) = 0;
};
```

> **为什么需要基类**：当前 `Thread<2048>`、`Thread<128>`、`Thread<1024>` 是三个不同类型，模板类无法放进同一张表。Dust_Vision 里只有一个非模板 `Thread` 类，所以没这个问题。

**内存影响**：`ThreadBase` 加 vtable 指针（4 B/实例）；注册表本身 `16×4 = 64 B`。

#### 2.5.5 shell 命令

在 `framework/cmd/shell/shell.cpp` 的 `ProcessLine` 增加 `thread` 分支：

```text
thread list                       列出所有线程：名字 / 状态 / 运行次数 / 错误次数 / 栈剩余
thread show <name>                单线程详细统计（含耗时）
thread pause <name>               暂停线程（保留现场，可恢复）
thread resume <name>              恢复被暂停的线程
thread reset <name>               清零该线程计数器
```

输出样例（`SendCommandLine` 走命令响应通道）：

```text
THREAD   STATE   RUN     RETRY   FATAL   EXIT    DROP    STACK
shell    ready   1       0       0       0       0       1384/2048
output   ready   1       0       0       0       0       96/128
imu      waiting 12453   0       0       0       0       512/2048
remote   waiting 9871    3       0       0       0       744/2048
test     ready   4521    0       0       0       0       1900/2048
--- 5 threads ---
```

`thread show <name>`：

```text
imu:
  state        waiting
  run          12453
  retry/fatal  0/0
  drop         0
  last         1.002 ms
  max          2.310 ms
  total        12492.4 ms
  stack        512 / 2048 B (25.0% used)
```

**命令解析**：`thread` 与 `var` / `log` 同级，写在 `Shell::ProcessLine`：

```cpp
else if (std::strcmp(reinterpret_cast<const char*>(cmd), "thread") == 0)
{
    ThreadCmd::Process(line);
}
```

`ThreadCmd` 新建 `framework/cmd/shell/thread_cmd.{hpp,cpp}`——与 `var` 同构。

> **归属**：统计字段在 `framework/init/thread.hpp`，命令在 `framework/cmd/shell/`。`cmd` 已经依赖 `init`（`Init_entry.hpp`），不会形成反向依赖。

#### 2.5.6 线程状态怎么取

`k_thread_state_str(tid, buf, size)`（`kernel/thread.c:226`）返回可读状态字符串（`ready` / `pending` / `suspended` / `dead` 等）。无线程名功能时也能用，不依赖额外 Kconfig。

**注意**：该函数返回的字符串直接指向内部缓冲，**必须拷贝后再用**，且调用时线程可能正在切换（读到的状态是瞬时值，属正常）。

#### 2.5.7 统计本身的代价

| 项 | 代价 |
|---|---|
| `atomic_inc` × 2/次运行 | 2 条原子加（单核 Cortex-M/RISC-V 下是普通加 + 内存屏障） |
| `k_cycle_get_64` × 2/次运行 | 2 次硬件计数器读 |
| 每次运行额外 RAM 写 | 3 次（`run_cycles_` / `max` / `last`） |
| 对 1ms 周期线程的影响 | < 1 µs，可忽略 |

**开关**：如果担心影响高频线程，可加 `CONFIG_DUST_THREAD_STATS`（默认 y），关闭后所有统计代码编译期消失。建议**先不加开关**，实测无影响再说。

---

## 3. 优点 B：可中断等待 `WaitForStop`

### 3.1 问题

当前所有周期线程用 `k_msleep`：

- [framework/modules/imu/drivers/imu.cpp:119](../../framework/modules/imu/drivers/imu.cpp#L119) `k_msleep(1)`
- [project/thread/gpio/trd_gpio.cpp:45](../../project/thread/gpio/trd_gpio.cpp#L45) `k_msleep(remain)`
- [project/thread/test/trd_test.cpp:27](../../project/thread/test/trd_test.cpp#L27) `k_msleep(10)`

`k_msleep` 只能等满时长，外部无法立刻打断。`RequestStop` 之后线程最多还要再转一个周期。

### 3.2 实现

在 `Thread` 内加一个停止信号量：

```cpp
bool WaitForStop(k_timeout_t timeout)
{
    if (atomic_get(&stop_requested_)) return true;
    return k_sem_take(&stop_sem_, timeout) == 0;
}
```

- 语义：**最长等 `timeout`，收到停止请求立即返回 true**。
- 用法：把 `k_msleep(1)` 换成 `if (thread_.WaitForStop(K_MSEC(1))) break;`——周期不变，但可被打断。

### 3.3 其他阻塞点怎么处理

Zephyr 没有 `SetWakeFd` 这种通用手段，按阻塞类型分三类：

| 阻塞点 | 处理 |
|---|---|
| `k_msleep` 周期等待 | 直接换 `WaitForStop` |
| `k_sem_take` 等硬件事件 | 保留原信号量，超时后检查 `StopRequested()`；或改用 `k_poll` 同时等两个信号量 |
| `K_FOREVER` 等外部事件 | 必须改：要么加超时，要么用 `k_poll` 同时等停止信号量 |

**`k_poll` 版本（可选，仅对 `K_FOREVER` 阻塞点使用）**：

```cpp
/**
 * @brief 等待事件集，停止请求可打断
 * @param events 调用者初始化的事件数组
 * @param count  事件个数
 * @param timeout 最长等待
 * @return true=被停止请求打断；false=事件到达或超时
 */
bool PollWithStop(k_poll_event* events, int count, k_timeout_t timeout)
{
    k_poll_event stop_ev = K_POLL_EVENT_INITIALIZER(
        K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &stop_sem_);
    events[count] = stop_ev;   // 调用者数组须预留 max+1

    if (k_poll(events, count + 1, timeout) != 0) return atomic_get(&stop_requested_);
    return atomic_get(&stop_requested_);
}
```

> 现阶段 `shell` 的 `K_FOREVER` 是唯一一处硬阻塞（[shell.cpp:90](../../framework/cmd/shell/shell.cpp#L90)），它由 `thread_stop()` 直接 `k_sem_give(&stream_->sem_)` 唤醒即可，不需要 `k_poll`。`PollWithStop` 作为后续可选项。

---

## 4. 优点 C：`RequestStop + Join` 完整生命周期

### 4.1 Dust_Vision 的设计

- `RequestStop()`：置停止标志 + 唤醒 `condition_variable` + 可选唤醒 `wake_fd_`；
- `Join()`：`std::thread::join()`，保证线程真正退出后才释放资源；
- 析构自动 `RequestStop + Join`。

### 4.2 Zephyr 可行性核对

`k_thread_join` 是**无条件可用**的（`E:/Zephyr/zephyr/kernel/sched.c:1374`），不需要 Kconfig 开关：

- 线程已死 → 立即返回 0；
- `K_NO_WAIT` 且线程未死 → `-EBUSY`；
- 自己 join 自己 → `-EDEADLK`；
- 线程终止后，`k_thread` 结构体可以复用（`doc/kernel/services/threads/index.rst:95-100`）。

### 4.3 实现

```cpp
void RequestStop()
{
    atomic_set(&stop_requested_, 1);
    k_sem_give(&stop_sem_);
}

bool StopRequested() const { return atomic_get(&stop_requested_) != 0; }

/**
 * @brief 等待线程退出
 * @param timeout 最长等待；超时返回 -EAGAIN
 * @return 0=已退出；-EAGAIN=超时；-EDEADLK=自己 join 自己
 */
int Join(k_timeout_t timeout = K_FOREVER)
{
    if (!atomic_get(&started_)) return 0;
    return k_thread_join(&thread_, timeout);
}
```

### 4.4 与 Dust_Vision 的差异（必须写进注释）

| 项 | Dust_Vision | 本子树 |
|---|---|---|
| 超时语义 | join 永久阻塞 | **建议默认带超时**，避免软重启卡死 |
| 析构自动收尾 | 有（RAII） | 无（静态对象不析构），由注册表统一调用 |
| 停止后复用 | 不支持（一个 std::thread 对象一次） | **支持**，`k_thread_create` 可复用同一结构体 |

**软重启时的收尾顺序建议**：`stop` 全部逆序 → `join` 全部逆序且每个带 1000ms 超时 → 超时的记 `DUST_LOG_ERR` 后 `k_thread_abort` → 再释放资源。

---

## 5. 优点 D：`REGISTER_THREAD` 带生命周期

### 5.1 现状

[framework/init/Init_entry.hpp:109-114](../../framework/init/Init_entry.hpp#L109-L114)：

```cpp
#define REGISTER_THREAD(fn, stage_, name_)                                     \
    static const InitEntry kThreadEntry_##fn                                   \
    __attribute__((used, __section__(".user_thread"))) = {                     \
        fn, static_cast<InitStage>(ThreadStage::stage_), InitLevel::High,      \
        InitFailPolicy::ContinueOnFail, name_                                  \
    }
```

线程项借用了 `InitEntry`，复用 `level/fail` 两个无意义字段。

### 5.2 改成独立结构

```cpp
/**
 * @brief 线程注册项：启动、停止通知和等待退出构成完整生命周期
 */
struct ThreadEntry
{
    InitFunc    start;   // 创建线程后立即返回
    CleanupFunc stop;    // 通知停止并解除阻塞（可为 nullptr）
    CleanupFunc join;    // 等待线程退出（可为 nullptr）
    ThreadStage stage;   // 所属线程启动阶段
    const char* name;    // 日志中的组件标识
};

#define REGISTER_THREAD(fn, stage_, stop_, join_, name_)                            \
    static const ThreadEntry kThreadEntry_##fn                                      \
    __attribute__((used, __section__(".user_thread"))) = {                          \
        fn, stop_, join_, ThreadStage::stage_, name_                                \
    }

/**
 * @brief 注册简单线程：不需要停止通知与退出等待
 *
 * stop/join 由宏内部填 nullptr，调用点不写这两个不重要的参数。
 * 这类线程不参与 System_Shutdown 的收尾。
 *
 * @param fn 线程启动函数
 * @param stage_ 线程启动阶段
 * @param name_ 线程名称，用于启动日志
 */
#define REGISTER_THREAD_BASE(fn, stage_, name_)                                     \
    REGISTER_THREAD(fn, stage_, nullptr, nullptr, name_)
```

**两个宏的分工**：

| 宏 | 参数 | 适用 |
|---|---|---|
| `REGISTER_THREAD` | `fn, stage, stop, join, name` | 需要在 `System_Shutdown` 里被停止、等待退出的线程（如 shell 的 `K_FOREVER` 阻塞） |
| `REGISTER_THREAD_BASE` | `fn, stage, name` | 简单线程，stop/join 内部填 `nullptr`，不参与收尾 |

### 5.3 启动器记录已启动线程

`System_Startup` 中的线程项压入一个固定容量表（无动态内存）：

```cpp
constexpr uint8_t kMaxStartedThreads = 16;

namespace {
const ThreadEntry* started_threads[kMaxStartedThreads];
uint8_t            started_count = 0;
}

static bool RunStage(ThreadStage stage)
{
    for (const ThreadEntry* e = __user_thread_start; e < __user_thread_end; ++e)
    {
        if (e->stage != stage) continue;

        if (!e->start()) { ... }

        if (started_count < kMaxStartedThreads) {
            started_threads[started_count++] = e;
        } else {
            DUST_LOG_ERR("thread table full, %s not tracked", e->name);
        }
    }
}
```

> 容量 16：当前 project 4 个线程、qingyu 8 个线程（`projects/qingyu/project/thread/` 下 can/chassis/gimbal/gpio/imu/pc/remote/test）。16 足够；超出只影响收尾，不影响启动。

### 5.4 新增 `System_Shutdown`

`System_startup.h` 增加：

```c
/**
 * @brief 请求系统停止（软重启 / 致命错误收尾）
 * @param exit_code 0=正常；非零状态保留到 System_Shutdown 返回值
 */
void System_RequestStop(int exit_code);

/**
 * @brief 是否已请求停止
 */
bool System_StopRequested(void);

/**
 * @brief 逆序停止并 join 所有已启动线程，再逆序释放已初始化资源
 * @return 退出状态（0 正常）
 */
int System_Shutdown(void);
```

实现：

```cpp
int System_Shutdown(void)
{
    System_RequestStop(0);

    for (int i = static_cast<int>(started_count) - 1; i >= 0; --i)
    {
        if (started_threads[i]->stop) started_threads[i]->stop();
    }

    for (int i = static_cast<int>(started_count) - 1; i >= 0; --i)
    {
        if (started_threads[i]->join &&
            started_threads[i]->join() != 0)              // 见 §5.5
        {
            DUST_LOG_ERR("%s join timeout", started_threads[i]->name);
        }
    }

    for (int i = static_cast<int>(inited_count) - 1; i >= 0; --i)
    {
        if (inited[i]->cleanup) inited[i]->cleanup();
    }

    started_count = 0;
    inited_count  = 0;
    return exit_status;
}
```

### 5.5 join 超时怎么传

两种做法：

- **A（推荐）**：`ThreadEntry.join` 保持 `void(void)`，由模块自己的 `thread_join()` 内部固定 `Thread::Join(K_MSEC(1000))`，超时自行打日志。
- **B**：`ThreadEntry` 增加 `uint16_t join_timeout_ms` 字段，由框架统一传。多一个字段、多一处初始化。

选 A：模块最清楚自己的线程该等多久，框架不替它决定。

### 5.6 触发者

`System_Shutdown` 在 MCU 上不会自己发生，必须有触发路径：

| 场景 | 触发点 | 建议 |
|---|---|---|
| 软重启（后续 `dust reboot` / shell 命令） | shell 命令 → `System_Shutdown()` → `sys_reboot()` | 推荐，作为第一个真实用例 |
| 板卡升级 / flash 写入前 | 业务代码显式调用 | 按需 |
| 启动失败收尾 | `System_Startup` 返回 false 后 | 只在这种情况下有意义 |

**注意**：当前 `HaltOnFail` 是 `while(1){}`（[Init_entry.cpp:29-32](../../framework/init/Init_entry.cpp#L29-L32)），失败后不会走到 shutdown。若要支持失败回滚，需要把 `while(1)` 改成「清理后停机」或「清理后降级运行」。

---

## 6. 优点 E（可选）：`REGISTER_INIT` 的 cleanup 与回滚

### 6.1 现状

`InitEntry` 只有 `func`，初始化到一半失败时，已经申请的资源无法回滚：

```cpp
struct InitEntry
{
    InitFunc          func;
    InitStage         stage;
    InitLevel         level;
    InitFailPolicy    fail;
    const char*       name;
};
```

### 6.2 改法

```cpp
/**
 * @brief 资源释放、停止通知或清理回调
 */
using CleanupFunc = void (*)();

struct InitEntry
{
    InitFunc          function;  // 初始化函数
    CleanupFunc       cleanup;   // 失败回滚与退出清理；可为 nullptr
    InitStage         stage;
    InitLevel         level;
    InitFailPolicy    fail;
    const char*       name;
};

#define REGISTER_INIT(fn, stage_, level_, fail_, cleanup_, name_)                  \
    static const InitEntry kInitEntry_##fn                                        \
    __attribute__((used, __section__(".user_init"))) = {                           \
        fn, cleanup_, InitStage::stage_, InitLevel::level_,                        \
        InitFailPolicy::fail_, name_                                               \
    }
```

### 6.3 失败路径

```cpp
if (!e->func())
{
    if (e->cleanup) e->cleanup();     // 部分初始化也须回滚
    HandleInitFail(*e);
    continue;
}
```

### 6.4 契约

1. **cleanup 必须支持「部分初始化」和「未初始化」两种状态**（Dust_Vision 注释原话）。
2. **cleanup 必须可重复调用**——失败回滚一次、退出时再调一次不能出错。
3. **没资源可释放就传 `nullptr`**，不要写空函数。

### 6.5 现有 7 处调用点

| 文件 | 现状 | cleanup 建议 |
|---|---|---|
| [buzzer.cpp:129](../../framework/cmd/buzzer/buzzer.cpp#L129) | `REGISTER_INIT(buzzer_init, PreInit, Mid, HaltOnFail, "buzzer")` | `buzzer::Instance().Off()`，传 `buzzer_deinit` |
| [w25q128.cpp:457](../../framework/cmd/flash/w25q128.cpp#L457) | `REGISTER_INIT(flash_init, PreInit, Mid, HaltOnFail, "w25q128")` | `PowerDown()`，传 `flash_deinit` |
| [shell.cpp:174](../../framework/cmd/shell/shell.cpp#L174) | `REGISTER_INIT(thread_init, PreInit, High, HaltOnFail, "dbg_init")` | `nullptr`（`rx` 是 `thread_init` 内局部 static，无法从外部访问；不为此改动所有权） |
| [trd_gpio.cpp:68](../../project/thread/gpio/trd_gpio.cpp#L68) | `REGISTER_INIT(thread_init, PreInit, Low, HaltOnFail, "output_init")` | `nullptr` |
| [trd_imu.cpp:31](../../project/thread/imu/trd_imu.cpp#L31) | `REGISTER_INIT(thread_init, EarlyInit, High, HaltOnFail, "imu_init")` | `nullptr` 或 `imu_.Deinit()`（需新增） |
| [trd_remote.cpp:55](../../project/thread/remote/trd_remote.cpp#L55) | `REGISTER_INIT(thread_init, EarlyInit, High, HaltOnFail, "remote_init")` | `nullptr` |
| [trd_test.cpp:49](../../project/thread/test/trd_test.cpp#L49) | `REGISTER_INIT(thread_init, LateInit, High, HaltOnFail, "test_init")` | `nullptr` |

> 只有 buzzer / flash 有真正值得释放的硬件状态。其余传 `nullptr` 即可，不为凑格式写空函数。

---

## 7. 优点 F（可选）：链接段存指针

### 7.1 Dust_Vision 的做法

```cpp
static const init::InitEntry DUST_JOIN(dust_init_, fn){...};
static const init::InitEntry* const DUST_JOIN(dust_init_ptr_, fn)
    __attribute__((used, section(".user_init"), aligned(sizeof(void*)))) =
        &DUST_JOIN(dust_init_, fn);
```

段里是 4 字节指针，结构体本身按正常布局存放。

### 7.2 实际收益

| 项 | 现状（段内存结构体） | 指针表 |
|---|---|---|
| 每个 init 项 flash 占用 | 16 B（`func`4 + 枚举3 + 对齐1 + `name`4） | 4 B |
| 每个 thread 项 flash 占用 | 16 B（借用的 `InitEntry`） | 4 B |
| 结构体字段变更 | 各编译单元共用同一头文件，同步生效 | 同样同步生效 |
| 跨版本预编译库 | 布局变化会静默错位 | 指针大小不变 |

**结论**：收益主要是**防御性**的（跨版本/跨编译单元 ABI），当前项目全源码编译，实际收益很小。按 `Init_entry.hpp` 现有注释，段内遍历依赖 `sizeof(InitEntry)` 一致，指针表能消除这个隐含假设。

### 7.3 改法（若做）

```cpp
extern const InitEntry* const __user_init_start[];
extern const InitEntry* const __user_init_end[];
extern const ThreadEntry* const __user_thread_start[];
extern const ThreadEntry* const __user_thread_end[];

for (const InitEntry* const* item = __user_init_start; item < __user_init_end; ++item)
{
    const InitEntry* e = *item;
    ...
}
```

`cmd/linker/dust_init.ld` 不需要改（仍是 `KEEP(*(.user_init))`）。

**建议**：放在最后一阶段做，或者不做。它与 A~D 无依赖。

---

## 8. 目标 API（改完后）

```cpp
// framework/init/thread.hpp

enum class TaskResult : uint8_t { Exit = 0, Retry = 1, Fatal = 2 };
using TaskFunc     = TaskResult (*)(void* context);
using RecoveryHook = bool (*)(void* context);

/**
 * @brief 线程运行统计快照
 */
struct ThreadStats
{
    const char* name;
    uint32_t    run_count;
    uint32_t    retry_count;
    uint32_t    fatal_count;
    uint32_t    exit_count;
    uint32_t    drop_count;
    uint64_t    run_ns;
    uint64_t    max_run_ns;
    uint64_t    last_run_ns;
    uint32_t    stack_unused;
    bool        running;
    bool        stop_requested;
};

/**
 * @brief 线程非模板基类：供注册表与 shell 命令统一访问
 */
class ThreadBase
{
public:
    virtual const char* Name() const = 0;
    virtual bool        GetStats(ThreadStats& out) const = 0;
    virtual void        RequestStop() = 0;
    virtual int         Join(k_timeout_t timeout) = 0;
};

template<uint32_t StackSize = 1024>
class Thread final : public ThreadBase
{
public:
    Thread();

    // 裸任务：保持现有行为，用于不需要停止/恢复/统计的线程
    void Start(k_thread_entry_t entry, ThreadPrio prio = ThreadPrio::Normal,
               void* p1 = nullptr, const char* name = nullptr);

    // 受管任务：任务体只返回结果，错误处理与统计由外壳负责
    void Start(TaskFunc entry, ThreadPrio prio, void* context, const char* name);

    void SetRecoveryHook(RecoveryHook hook);

    void RequestStop() override;
    bool StopRequested() const;
    bool WaitForStop(k_timeout_t timeout);
    int  Join(k_timeout_t timeout = K_FOREVER) override;

    void CountDrop() { atomic_inc(&drop_count_); }

    const char* Name() const override { return name_; }
    bool GetStats(ThreadStats& out) const override;
};
```

**内存影响**：

| 项 | 字节 | 说明 |
|---|---|---|
| vtable 指针 | 4 | `ThreadBase` 虚函数 |
| `k_sem` | 16 | 停止通知 |
| `atomic_t` × 7 | 28 | stop / started / run / retry / fatal / exit / drop |
| `uint64_t` × 3 | 24 | run / max / last 耗时 |
| **合计** | **≈ 72 B/实例** | 不含栈（含对齐） |

当前 5 个 `Thread` 实例共 +360 B。`ThreadRegistry` 表 `16 × 4 = 64 B`。合计约 **+424 B RAM**——在 STM32F103C8（20KB RAM）上需要留意，HPM5361（大 RAM）无压力。若板卡 RAM 紧张，后续可加 `CONFIG_DUST_THREAD_STATS` 编译期裁剪（本规划不实现）。

---

## 9. 分阶段执行方案

> 每个阶段独立可编译、可验证。阶段 1~3 只改 `thread.hpp` 与新增文件，不动任何业务线程；阶段 4 才改调用点。

### 阶段 0：确认（不改代码）—— ✅ 2026-10-19 已拍板

| 待确认项 | 结论 |
|---|---|
| 是否引入 `System_Shutdown` | **引入，但先只提供 API，不接线**（等软重启需求） |
| join 超时 | **1000 ms**（模块内固定） |
| 线程表容量 | **16** |
| 是否改指针表 | **做**（原建议暂不做，用户决定做） |
| 是否加 init cleanup | **做**（仅 buzzer/flash 有真实清理实体，其余传 `nullptr`） |
| 统计是否加 Kconfig 开关 | **先不加**，实测无影响再说 |
| 统计是否对裸任务生效 | **否**——裸任务无 `Run` 外壳，只统计受管任务 |

**结论**：六个阶段全部执行（原阶段 5、6 由「可选」转为「必做」）。

> **⚠️ 2026-10-19 实施中变更（阶段 4 完成后由用户否决）**：
> - **不做 `System_Shutdown` / `System_RequestStop` / `System_StopRequested`** —— MCU 运行期不存在"停止系统"，无调用者。上表第一行的结论已作废。
> - **`ThreadEntry` 不带 `stop` / `join`** —— 唯一用途是 Shutdown 收尾，随之删除。
> - **`REGISTER_THREAD_BASE` 删除** —— 与 `REGISTER_THREAD` 合并，只剩 3 参数一种。
> - 阶段 4 的实际落地内容与下方原文不同，以「阶段 5 §5.1 现状盘点」为准。
> - 新增 **阶段 5：周期外壳**（用户追加），原阶段 5/6 顺延为 6/7。

### 阶段 1：`Thread` 停止原语 + 可中断等待

**目标**：`Thread` 具备 `RequestStop` / `StopRequested` / `WaitForStop` / `Join`，不改变任何现有调用方行为。

**改动**：`framework/init/thread.hpp`

old（全文替换 class 部分）：

```cpp
template<uint32_t StackSize = 1024>
class Thread final
{
public:
    void Start(k_thread_entry_t entry, ThreadPrio prio = ThreadPrio::Normal, void* p1 = nullptr, const char* name = nullptr)
    {
        k_thread_create(&thread_, stack_, K_THREAD_STACK_SIZEOF(stack_),
                        entry, p1, nullptr, nullptr, static_cast<uint8_t>(prio), 0, K_NO_WAIT);
        if (name != nullptr) {
            k_thread_name_set(&thread_, name);
        }
    }
private:
    k_thread thread_ {};
    K_KERNEL_STACK_MEMBER(stack_, StackSize);
};
```

new：

```cpp
template<uint32_t StackSize = 1024>
class Thread final
{
public:
    Thread() { k_sem_init(&stop_sem_, 0, 1); }

    void Start(k_thread_entry_t entry, ThreadPrio prio = ThreadPrio::Normal, void* p1 = nullptr, const char* name = nullptr)
    {
        atomic_clear(&stop_requested_);
        k_sem_reset(&stop_sem_);

        k_thread_create(&thread_, stack_, K_THREAD_STACK_SIZEOF(stack_),
                        entry, p1, nullptr, nullptr, static_cast<uint8_t>(prio), 0, K_NO_WAIT);
        atomic_set(&started_, 1);

        if (name != nullptr) {
            k_thread_name_set(&thread_, name);
        }
    }

    /**
     * @brief 请求线程停止：置标志并唤醒 WaitForStop 等待者
     */
    void RequestStop()
    {
        atomic_set(&stop_requested_, 1);
        k_sem_give(&stop_sem_);
    }

    bool StopRequested() const { return atomic_get(&stop_requested_) != 0; }

    /**
     * @brief 等待停止请求或超时
     * @param timeout 最长等待时间
     * @return true=收到停止请求；false=超时
     */
    bool WaitForStop(k_timeout_t timeout)
    {
        if (atomic_get(&stop_requested_)) return true;
        return k_sem_take(&stop_sem_, timeout) == 0;
    }

    /**
     * @brief 等待线程真正退出
     * @param timeout 最长等待时间
     * @return 0=已退出；-EAGAIN=超时；-EDEADLK=自己 join 自己
     */
    int Join(k_timeout_t timeout = K_FOREVER)
    {
        if (!atomic_get(&started_)) return 0;
        return k_thread_join(&thread_, timeout);
    }

private:
    k_thread thread_ {};
    K_KERNEL_STACK_MEMBER(stack_, StackSize);

    struct k_sem stop_sem_ {};                       // 停止通知（初值 0）
    atomic_t     stop_requested_ = ATOMIC_INIT(0);   // 停止标志
    atomic_t     started_        = ATOMIC_INIT(0);   // 已创建
};
```

**验证**：编译通过即可。现有 5 处 `Thread` 调用方一行不改。

**产出**：`thread.hpp`（version → 0.2，date → 2026-10-19）

---

### 阶段 2：受管任务外壳 + 恢复钩子

**目标**：`Thread` 支持 `TaskResult` 任务体，错误处理从任务里剥离。

**改动**：`framework/init/thread.hpp`

在 `ThreadPrio` 之后增加：

```cpp
/**
 * @brief 线程任务结果（代替异常）
 */
enum class TaskResult : uint8_t
{
    Exit  = 0,   // 正常结束：线程退出，不重进
    Retry = 1,   // 可恢复：交给恢复钩子决定是否重进
    Fatal = 2,   // 不可恢复：记录后退出
};

using TaskFunc     = TaskResult (*)(void* context);
using RecoveryHook = bool (*)(void* context);
```

class 内增加：

```cpp
public:
    /**
     * @brief 启动受管任务：任务体只返回结果，错误处理由外壳负责
     * @param entry   任务入口
     * @param prio    线程优先级
     * @param context 任务上下文（每次重进原样传回）
     * @param name    线程名
     */
    void Start(TaskFunc entry, ThreadPrio prio, void* context, const char* name)
    {
        task_    = entry;
        context_ = context;
        name_    = name;

        Start(RunEntry, prio, this, name);   // 复用裸任务路径
    }

    /**
     * @brief 注册恢复钩子：任务返回 Retry 时调用
     * @param hook true=重进任务；false=线程退出
     */
    void SetRecoveryHook(RecoveryHook hook) { hook_ = hook; }

private:
    static void RunEntry(void* p1, void* p2, void* p3)
    {
        ARG_UNUSED(p2);
        ARG_UNUSED(p3);
        static_cast<Thread*>(p1)->Run();
    }

    void Run()
    {
        for (;;)
        {
            if (StopRequested()) break;

            const TaskResult result = task_(context_);

            if (result == TaskResult::Exit) break;

            if (result == TaskResult::Fatal)
            {
                DUST_LOG_ERR("thread %s fatal", (name_ != nullptr) ? name_ : "?");
                break;
            }

            if (StopRequested()) break;
            if (hook_ == nullptr || !hook_(context_)) break;
        }
    }

    TaskFunc     task_    = nullptr;
    void*        context_ = nullptr;
    const char*  name_    = nullptr;
    RecoveryHook hook_    = nullptr;
```

**注意**：

- `thread.hpp` 需要 `#include "log.hpp"`（`DUST_LOG_ERR`）。`framework/init/Init_entry.cpp` 已经依赖 `log.hpp`，不算新增依赖类别。
- `Start(TaskFunc, ...)` 与 `Start(k_thread_entry_t, ...)` 靠函数指针类型区分，重载无歧义。
- 裸任务路径**不会**调用 `Run`，行为与现在完全一致。

**验证**：编译通过；现有调用方不改。

**产出**：`thread.hpp`（version → 0.3）

---

### 阶段 3：线程运行统计 + `thread` shell 命令

**目标**：受管任务自动统计运行次数与错误次数，shell 可查看。

**改动 1**：`framework/init/thread.hpp`

在 `TaskResult` 之后增加 `ThreadStats` 结构（见 §8）与 `ThreadBase` 基类：

```cpp
/**
 * @brief 线程非模板基类：供注册表与 shell 命令统一访问
 */
class ThreadBase
{
public:
    virtual const char* Name() const = 0;
    virtual bool        GetStats(ThreadStats& out) const = 0;
    virtual void        RequestStop() = 0;
    virtual int         Join(k_timeout_t timeout) = 0;
};
```

`Thread` 改为 `class Thread final : public ThreadBase`，增加统计成员与接口：

```cpp
    void RequestStop() override
    {
        atomic_set(&stop_requested_, 1);
        k_sem_give(&stop_sem_);
    }

    int Join(k_timeout_t timeout = K_FOREVER) override
    {
        if (!atomic_get(&started_)) return 0;
        return k_thread_join(&thread_, timeout);
    }

    const char* Name() const override { return name_; }

    /**
     * @brief 取运行统计快照
     * @param out 输出
     * @return true=已启动过，数据有效
     */
    bool GetStats(ThreadStats& out) const override
    {
        out.name           = name_;
        out.run_count      = static_cast<uint32_t>(atomic_get(&run_count_));
        out.retry_count    = static_cast<uint32_t>(atomic_get(&retry_count_));
        out.fatal_count    = static_cast<uint32_t>(atomic_get(&fatal_count_));
        out.exit_count     = static_cast<uint32_t>(atomic_get(&exit_count_));
        out.drop_count     = static_cast<uint32_t>(atomic_get(&drop_count_));
        out.run_ns         = k_cyc_to_ns_floor64(run_cycles_);
        out.max_run_ns     = k_cyc_to_ns_floor64(max_run_cycles_);
        out.last_run_ns    = k_cyc_to_ns_floor64(last_run_cycles_);
        out.running        = !atomic_get(&started_) ? false : IsAlive();
        out.stop_requested = StopRequested();
        out.stack_unused   = StackUnused();
        return atomic_get(&started_) != 0;
    }

    void CountDrop() { atomic_inc(&drop_count_); }

private:
    /**
     * @brief 线程是否仍在运行（未被 join 掉）
     */
    bool IsAlive() const
    {
        char buf[32];
        const char* state = k_thread_state_str(&thread_, buf, sizeof(buf));
        return (state != nullptr) && (std::strstr(state, "dead") == nullptr) && (std::strstr(state, "exited") == nullptr);
    }

    /**
     * @brief 栈剩余字节（未开 INIT_STACKS/THREAD_STACK_INFO 时返回 0）
     */
    uint32_t StackUnused() const
    {
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
        size_t unused = 0;
        if (k_thread_stack_space_get(&thread_, &unused) == 0) {
            return static_cast<uint32_t>(unused);
        }
#else
        ARG_UNUSED(this);
#endif
        return 0;
    }

    atomic_t run_count_      = ATOMIC_INIT(0);
    atomic_t retry_count_    = ATOMIC_INIT(0);
    atomic_t fatal_count_    = ATOMIC_INIT(0);
    atomic_t exit_count_     = ATOMIC_INIT(0);
    atomic_t drop_count_     = ATOMIC_INIT(0);
    uint64_t run_cycles_     = 0;   // 累计执行周期（仅工作线程写）
    uint64_t max_run_cycles_ = 0;   // 单次最长（仅工作线程写）
    uint64_t last_run_cycles_= 0;   // 最近一次（仅工作线程写）
```

`Run` 外壳补统计（见 §2.3 代码）。

**改动 2**：新增 `framework/init/ThreadRegistry.hpp`

```cpp
/**
 * @brief 线程注册表：所有 Thread 在 Start 时登记，供 shell 遍历
 */
class ThreadRegistry
{
public:
    static constexpr uint8_t kMaxThreads = 16;

    /**
     * @brief 登记线程
     * @return true=成功；false=表满（不影响线程启动）
     */
    static bool Add(ThreadBase* thread)
    {
        if (count_ >= kMaxThreads) return false;
        threads_[count_++] = thread;
        return true;
    }

    static ThreadBase* At(uint8_t i) { return (i < count_) ? threads_[i] : nullptr; }
    static uint8_t Count() { return count_; }
    static ThreadBase* Find(const char* name)
    {
        for (uint8_t i = 0; i < count_; ++i) {
            if (std::strcmp(threads_[i]->Name(), name) == 0) return threads_[i];
        }
        return nullptr;
    }
    static void Reset() { count_ = 0; }

private:
    static inline ThreadBase* threads_[kMaxThreads] {};
    static inline uint8_t     count_ = 0;
};
```

`Thread::Start` 末尾调用 `ThreadRegistry::Add(this)`。

**改动 3**：新增 `framework/cmd/shell/thread_cmd.hpp` / `thread_cmd.cpp`

```cpp
namespace debug {

/**
 * @brief thread 命令：list / show / stop / reset
 */
class ThreadCmd final
{
public:
    static void Process(uint8_t* line);
};

} // namespace debug
```

实现要点：

- `thread list`：遍历 `ThreadRegistry`，每行 `name state run retry fatal exit drop stack`；
- `thread show <name>`：`Find` 后输出多行详情；
- `thread pause <name>` / `thread resume <name>`：`Pause()` / `Resume()`；
- `thread reset <name>`：清零该线程计数器（需要给 `ThreadBase` 增加 `ResetStats()` 虚函数）。
- 输出全部走 `Log::SendCommandLine()`，与 `var` 一致。

**改动 4**：`framework/cmd/shell/CMakeLists.txt` / `framework/cmd/CMakeLists.txt`

```cmake
if(CONFIG_DUST_CMD_SHELL)
    target_sources(app PRIVATE shell/shell.cpp shell/thread/thread_cmd.cpp)
endif()
```

**改动 5**：`Shell::ProcessLine` 增加分支

```cpp
else if (std::strcmp(reinterpret_cast<const char*>(cmd), "thread") == 0)
{
    ThreadCmd::Process(line);
}
```

`Shell::CmdHelp` 增加：

```cpp
Log::SendCommandLine("thread list             列出所有线程运行统计");
Log::SendCommandLine("thread show <name>      查看单线程详细统计");
Log::SendCommandLine("thread pause <name>     暂停线程（保留现场）");
Log::SendCommandLine("thread resume <name>    恢复被暂停的线程");
Log::SendCommandLine("thread reset <name>     清零线程计数器");
```

**改动 6**：`framework/init/CMakeLists.txt` 不需要改（`thread.hpp` 是 header-only）；`ThreadRegistry.hpp` 也 header-only。

**验证**：

1. 编译通过；
2. 板卡启动后 `thread list` 输出 5 行（shell / output / imu / remote / test），`run` 计数随时间增长；
3. `thread show imu` 的 `last` ≈ 1 ms（对应 `k_msleep(1)` 周期）；
4. `thread reset imu` 后 `run` 归零并重新增长；
5. 栈剩余量与 `CONFIG_INIT_STACKS` 开启前的估算一致（可用于后续调栈）。

**产出**：`thread.hpp`（version → 0.4）、新增 `ThreadRegistry.hpp`、新增 `thread_cmd.hpp/cpp`、`shell.cpp`、`cmd/CMakeLists.txt`

**注意**：

- 未启动的 `Thread` 不登记，`thread list` 只显示真正启动过的线程。
- `k_thread_state_str` 返回内部缓冲，**必须立即拷贝**，且状态是瞬时值。
- 裸任务（`Start(k_thread_entry_t, ...)`）也登记，但 `run_count` 永远是 0——它没有 `Run` 外壳。若要统计裸任务，迁移到受管任务。

---

### 阶段 4：`REGISTER_THREAD` 生命周期（⚠️ stop/join 部分已否决，见 §5.1）

> **实施结果**：只保留 `ThreadEntry` 独立结构 + `REGISTER_INIT` 加 `cleanup`。
> `stop` / `join` 字段、`System_Shutdown`、登记表、`REGISTER_THREAD_BASE` 全部删除。
> 下方原文保留作为决策记录，**不要照它执行**。

**改动 1**：`framework/init/Init_entry.hpp`

在 `InitFunc` 后增加：

```cpp
using CleanupFunc = void (*)();
```

`REGISTER_THREAD` old → new：

```cpp
// old
#define REGISTER_THREAD(fn, stage_, name_)                                     \
    static const InitEntry kThreadEntry_##fn                                   \
    __attribute__((used, __section__(".user_thread"))) = {                     \
        fn, static_cast<InitStage>(ThreadStage::stage_), InitLevel::High,      \
        InitFailPolicy::ContinueOnFail, name_                                  \
    }

// new
/**
 * @brief 线程注册项：启动、停止通知和等待退出构成完整生命周期
 */
struct ThreadEntry
{
    InitFunc     start;   // 创建线程后立即返回
    CleanupFunc  stop;    // 通知停止并解除阻塞（可为 nullptr）
    CleanupFunc  join;    // 等待线程退出（可为 nullptr）
    ThreadStage  stage;   // 所属线程启动阶段
    const char*  name;    // 日志中的组件标识
};

#define REGISTER_THREAD(fn, stage_, stop_, join_, name_)                       \
    static const ThreadEntry kThreadEntry_##fn                                 \
    __attribute__((used, __section__(".user_thread"))) = {                     \
        fn, stop_, join_, ThreadStage::stage_, name_                           \
    }

/**
 * @brief 注册简单线程：不需要停止通知与退出等待
 *
 * stop/join 由宏内部填 nullptr，这类线程不参与 System_Shutdown 收尾。
 *
 * @param fn 线程启动函数
 * @param stage_ 线程启动阶段
 * @param name_ 线程名称，用于启动日志
 */
#define REGISTER_THREAD_BASE(fn, stage_, name_)                                \
    REGISTER_THREAD(fn, stage_, nullptr, nullptr, name_)
```

**改动 2**：`framework/init/Init_entry.cpp`

`RunStage(ThreadStage)` old：

```cpp
static void RunStage(ThreadStage stage)
{
    extern const InitEntry __user_thread_start[], __user_thread_end[];

    for (const InitEntry* e = __user_thread_start; e < __user_thread_end; ++e)
    {
        if (static_cast<ThreadStage>(e->stage) != stage) {
            continue;
        }
        e->func();
        DUST_LOG_OK("[boot] %s start", e->name);
    }
    DUST_LOG_OK("=====     [boot] %s ok     =====\n", StageName(stage));
}
```

new：

```cpp
namespace {
constexpr uint8_t kMaxStartedThreads = 16;
const ThreadEntry* started_threads[kMaxStartedThreads];
uint8_t            started_count = 0;
}

static void RunStage(ThreadStage stage)
{
    extern const ThreadEntry __user_thread_start[], __user_thread_end[];

    for (const ThreadEntry* e = __user_thread_start; e < __user_thread_end; ++e)
    {
        if (e->stage != stage) {
            continue;
        }
        e->start();
        if (started_count < kMaxStartedThreads) {
            started_threads[started_count++] = e;
        } else {
            DUST_LOG_ERR("thread table full, %s not tracked", e->name);
        }
        DUST_LOG_OK("[boot] %s start", e->name);
    }
    DUST_LOG_OK("=====     [boot] %s ok     =====\n", StageName(stage));
}
```

**改动 3**：`framework/init/System_startup.h` 增加 `System_RequestStop` / `System_StopRequested` / `System_Shutdown`（见 §5.4）。

**改动 4**：5 处 `REGISTER_THREAD` 调用点

| 文件 | old | new | 说明 |
|---|---|---|---|
| [shell.cpp:175](../../framework/cmd/shell/shell.cpp#L175) | `REGISTER_THREAD(thread_start, PreThread, "dbg_start")` | `REGISTER_THREAD(thread_start, PreThread, shell_stop, shell_join, "dbg_start")` | 有 `K_FOREVER` 阻塞，**必须**有 stop |
| [trd_remote.cpp:56](../../project/thread/remote/trd_remote.cpp#L56) | `REGISTER_THREAD(thread_start, EarlyThread, "remote_start")` | `REGISTER_THREAD(thread_start, EarlyThread, remote_stop, remote_join, "remote_start")` | 等 50ms 超时，可被打断 |
| [trd_imu.cpp:32](../../project/thread/imu/trd_imu.cpp#L32) | `REGISTER_THREAD(thread_start, EarlyThread, "imu_start")` | `REGISTER_THREAD(thread_start, EarlyThread, imu_stop, imu_join, "imu_start")` | 等 1ms 超时，可被打断 |
| [trd_gpio.cpp:69](../../project/thread/gpio/trd_gpio.cpp#L69) | `REGISTER_THREAD(thread_start, PreThread, "output_start")` | `REGISTER_THREAD_BASE(thread_start, PreThread, "output_start")` | 简单心跳线程，用基础版 |
| [trd_test.cpp:50](../../project/thread/test/trd_test.cpp#L50) | `REGISTER_THREAD(thread_start, LateThread, "test_start")` | `REGISTER_THREAD_BASE(thread_start, LateThread, "test_start")` | 简单测试线程，用基础版 |

> **基础版的判定标准**：线程本身没有需要优雅收尾的资源（纯周期任务 / 测试代码），停止时直接让它跑完当前周期即可。**有阻塞点、有硬件句柄、有缓存的线程必须用完整版**。

**改动 5**：各 stop/join 实现

**shell.cpp**：

```cpp
static void shell_stop()
{
    shell_.RequestStop();
}

static void shell_join()
{
    shell_.Join();
}
```

`Shell` 增加：

```cpp
void RequestStop()
{
    thread_.RequestStop();
    if (stream_ != nullptr) k_sem_give(&stream_->sem_);   // 唤醒 K_FOREVER 阻塞
}
int Join() { return thread_.Join(K_MSEC(1000)); }
```

`Shell::Task` 的循环入口加：

```cpp
for (;;)
{
    if (thread_.StopRequested()) break;
    k_sem_take(&stream_->sem_, K_FOREVER);
    ...
}
```

**trd_gpio.cpp / trd_test.cpp（基础版）**：

不需要 stop/join 函数，`REGISTER_THREAD_BASE` 内部已经填 `nullptr`。Task 里的 `k_msleep` 保持原样——基础版线程不参与收尾，无需可中断等待。

**trd_imu.cpp / trd_remote.cpp**：

`ImuManager` / `Remote` 各增加：

```cpp
void Stop() { thread_.RequestStop(); }
int  Join() { return thread_.Join(K_MSEC(1000)); }
```

Task 里 `k_msleep(1)` → `if (thread_.WaitForStop(K_MSEC(1))) break;`；`k_sem_take(&uart_->sem_, K_MSEC(50))` 保持不变，超时分支之后检查 `thread_.StopRequested()`。

**注意**：`Remote::Task` 里已有 `k_sem_take(..., K_MSEC(50))`，停止延迟最长 50ms，可接受，不强制改 `k_poll`。

**验证**：

1. 编译通过；
2. 板卡启动，日志中 `[boot] xxx start` 顺序不变；
3. 手动调用 `System_Shutdown()`（可在 test 线程里临时加一行），观察：
   - 各线程 stop 后退出；
   - join 不超时；
   - 无卡死。

**产出**：`Init_entry.hpp`、`Init_entry.cpp`、`System_startup.h`、`shell.hpp/cpp`、`imu.hpp/cpp`、`remote.hpp/cpp`、`trd_gpio.cpp`、`trd_test.cpp`（改用基础版）、`trd_imu.cpp`、`trd_remote.cpp`

---

### 阶段 5：周期外壳 —— 把「等下一拍」收进 `Thread`（2026-10-19 追加）

#### 5.1 现状盘点（截至阶段 4 完成）

**已落地**：

| 项 | 位置 | 状态 |
|---|---|---|
| `TaskResult{Exit,Retry,Fatal}` + `RecoveryHook` | `thread/thread.hpp` | 就绪，**无任何线程使用** |
| `Thread::Run` 受管任务外壳 | `thread/thread.hpp` | 就绪，**无任何线程使用** |
| 统计字段 + `GetStats` + `ThreadBase` | `thread/thread.hpp` | 就绪 |
| `ThreadRegistry` + `thread list/show/pause/resume/reset` | `init/thread/` + `cmd/shell/thread/` | 就绪 |
| ~~`RequestStop` / `StopRequested` / `WaitForStop` / `Join`~~ | `thread/thread.hpp` | **已删除**（无调用者，见 §5.1） |
| `InitEntry.cleanup` + 失败回滚 | `startup/Init_entry.*` | 就绪，buzzer/flash 有实体 |
| `REGISTER_THREAD(fn, stage, name)` | `startup/Init_entry.hpp` | 3 参数，`ThreadEntry` 独立结构 |
| 目录分层 | `init/{startup,thread,irq}`、`shell/{log,var,thread}` | 完成 |

**已舍弃**（阶段 0 曾拍板要做，实施后由用户否决）：

| 舍弃项 | 原因 |
|---|---|
| `System_Shutdown` / `System_RequestStop` / `System_StopRequested` | MCU 无进程退出语义，运行期不存在"停止系统" |
| `ThreadEntry.stop` / `ThreadEntry.join` 字段 | 唯一用途是 Shutdown 遍历收尾，Shutdown 删后无调用者 |
| `REGISTER_THREAD_BASE` 宏 | 与 `REGISTER_THREAD` 合并，只剩 3 参数一种 |
| `started_threads[]` / `inited_entries[]` 登记表 | 只服务 Shutdown |

**已废弃**：`RequestStop` / `StopRequested` / `WaitForStop` / `Join` —— 用户确认"MCU 上线程不需要停止"，改用 `Pause` / `Resume`（`k_thread_suspend` / `k_thread_resume`，保留全部现场）。

#### 5.2 问题：循环控制泄进了 Task

当前 `ImuManager::Task`：

```cpp
for (;;)
{
    if (thread_.StopRequested()) break;              // ← 外壳该管的事
    ...
    if (thread_.WaitForStop(K_MSEC(1))) break;       // ← 外壳该管的事
}
```

这两行与业务无关，却写在业务任务里。只要线程迁到受管任务（`TaskResult`），它们就完全多余——外壳本来就有循环。

同理 `Shell::Task` 和 `Remote::Task` 的 `StopRequested()` 检查也是外壳该做的。

#### 5.3 设计：Task 只写「一拍」

`TaskResult` 增加 `Continue`：

```cpp
enum class TaskResult : uint8_t
{
    Continue = 0,   // 本拍正常结束：外壳等下一拍再调一次
    Exit     = 1,   // 任务主动结束：线程退出
    Retry    = 2,   // 可恢复错误：交给恢复钩子
    Fatal    = 3,   // 不可恢复错误：记录后退出
};
```

`Start` 增加周期参数：

```cpp
/**
 * @brief 启动周期任务：外壳负责循环、停止检查、周期等待与统计
 * @param entry    任务入口（每拍调用一次）
 * @param period   两拍之间的等待时长
 * @param prio     线程优先级
 * @param context  任务上下文（每拍原样传回）
 * @param name     线程名
 */
void Start(TaskFunc entry, k_timeout_t period, ThreadPrio prio, void* context, const char* name);
```

外壳：

```cpp
void RunPeriodic()
{
    while (!StopRequested())
    {
        atomic_inc(&run_count_);
        const uint64_t begin_cycle = k_cycle_get_64();

        const TaskResult result = task_(context_);

        const uint64_t elapsed = k_cycle_get_64() - begin_cycle;
        run_cycles_ += elapsed;
        last_run_cycles_ = elapsed;
        if (elapsed > max_run_cycles_) max_run_cycles_ = elapsed;

        switch (result)
        {
            case TaskResult::Continue:
                break;

            case TaskResult::Exit:
                atomic_inc(&exit_count_);
                return;

            case TaskResult::Fatal:
                atomic_inc(&fatal_count_);
                DUST_LOG_ERR("thread %s fatal", Name());
                return;

            case TaskResult::Retry:
                atomic_inc(&retry_count_);
                if (hook_ == nullptr || !hook_(context_)) return;
                break;
        }

        if (WaitForStop(period_)) return;   // 停止请求在这里也能立即生效
    }
}
```

**Task 变成**：

```cpp
static TaskResult Task(void* ctx)
{
    auto* self = static_cast<ImuManager*>(ctx);
    if (!self->ReadOnce()) return TaskResult::Retry;
    return TaskResult::Continue;
}
```

没有 `for (;;)`、没有 `StopRequested()`、没有 `WaitForStop()`、没有统计代码。

#### 5.4 兼容：非周期任务

一次性任务（跑完就结束，如标定）不需要周期：

```cpp
void Start(TaskFunc entry, ThreadPrio prio, void* context, const char* name);   // 保留，周期视为 K_NO_WAIT
void Start(TaskFunc entry, k_timeout_t period, ThreadPrio prio, void* context, const char* name);
```

两参重载内部 `period_ = K_NO_WAIT`，外壳仍走 `RunPeriodic()`，但 `WaitForStop(K_NO_WAIT)` 立即返回，等价于连续调用 Task，直到返回 `Exit`/`Fatal` 或钩子拒绝。

#### 5.5 裸任务不变

`Start(k_thread_entry_t, ...)` 完全不动。裸任务仍然是 `void(void*,void*,void*)` 入口、自己写循环、自己 `k_msleep`。**统计对裸任务依旧为 0**（没有外壳）。

#### 5.6 改动清单

| 文件 | 改动 |
|---|---|
| `framework/init/thread/thread.hpp` | `TaskResult` 加 `Continue`；`Start` 加重载带 `k_timeout_t period`；`Run()` 改 `RunPeriodic()` + `period_` 成员 |
| `framework/modules/imu/drivers/imu.cpp` / `.hpp` | `Task()` 改成单拍函数返回 `TaskResult`；`Start(prio)` → `Start(task, period, prio, this, "imu")`；删 `StopRequested`/`WaitForStop` 两行 |
| `framework/modules/remotes/remote.cpp` / `.hpp` | 同上（解析循环内部那段 `while(true)` 保留，因为它是在处理一次收到的数据，不是周期结构） |
| `framework/cmd/shell/shell.cpp` / `.hpp` | `Task()` 改成单拍函数；接收循环 `k_sem_take` 保持在单拍内；外壳负责重复调用 |
| `project/thread/imu/trd_imu.cpp`、`project/thread/remote/trd_remote.cpp` | 若 `Start` 签名变了，这里同步 |
| `framework/cmd/shell/thread/thread_cmd.cpp` | `thread pause` / `resume`（`stop` 已删除） |

#### 5.7 验证

1. 编译通过；
2. 上板，`thread list` 中 `imu` / `remote` / `shell` 的 `run` 计数持续增长；
3. `thread show imu` 的 `last` ≈ 1ms（IMU 周期），`run_ns` 随运行时间线性增长；
4. `thread pause imu` 后，`state` 变 `paused`，`run` 停止增长；`thread resume imu` 后继续增长且不归零；
5. 启动日志顺序不变。

#### 5.8 待确认

- `shell` 的单拍是「等一次 UART 事件」而不是固定周期。`k_sem_take(K_FOREVER)` 放在单拍内会让外壳的 `WaitForStop(period)` 失去意义。两种处理：
  - **A（推荐）**：shell 继续用裸任务，不迁受管任务——它是事件驱动，不是周期任务；
  - **B**：外壳支持「Task 自己阻塞」模式，即 `period = K_FOREVER` 时外壳不再额外等待；
- `retry_count` 在 `Continue` 模式下语义仍为「可恢复错误次数」，不受影响。

---

### 阶段 6：`REGISTER_INIT` cleanup（已拍板执行）

**目标**：初始化失败可回滚。

**改动 1**：`Init_entry.hpp` 的 `InitEntry` 增加 `cleanup`（见 §6.2），`REGISTER_INIT` 增加参数。

**改动 2**：`Init_entry.cpp` 失败路径调用 `cleanup`；`System_Shutdown` 逆序调用已成功项。

**改动 3**：7 处调用点补 `nullptr` 或真实清理函数（见 §6.5 表格）。

**验证**：编译通过；buzzer / flash 的初始化失败路径不产生资源泄漏（实际上很难构造，主要看代码审查）。

---

### 阶段 7：指针表（已拍板执行）

见 §7。独立阶段，与前面各阶段无依赖；放在最后执行，避免与结构体改签名的改动混在一起。

**改动 1**：`framework/init/Init_entry.hpp` 的两个注册宏

```cpp
#define REGISTER_INIT(fn, stage_, level_, fail_, cleanup_, name_)                   \
    static const InitEntry kInitEntry_##fn {                                        \
        fn, cleanup_, InitStage::stage_, InitLevel::level_,                         \
        InitFailPolicy::fail_, name_                                                \
    };                                                                              \
    static const InitEntry* const kInitEntryPtr_##fn                                \
        __attribute__((used, __section__(".user_init"), aligned(sizeof(void*)))) =  \
            &kInitEntry_##fn

#define REGISTER_THREAD(fn, stage_, name_)                                          \
    static const ThreadEntry kThreadEntry_##fn {                                    \
        fn, ThreadStage::stage_, name_                                              \
    };                                                                              \
    static const ThreadEntry* const kThreadEntryPtr_##fn                            \
        __attribute__((used, __section__(".user_thread"), aligned(sizeof(void*)))) = \
            &kThreadEntry_##fn
```

**改动 2**：`framework/init/Init_entry.cpp` 的遍历

```cpp
extern const InitEntry*   const __user_init_start[];
extern const InitEntry*   const __user_init_end[];
extern const ThreadEntry* const __user_thread_start[];
extern const ThreadEntry* const __user_thread_end[];

// Init
for (const InitEntry* const* item = __user_init_start; item < __user_init_end; ++item)
{
    const InitEntry* e = *item;
    ...
}

// Thread
for (const ThreadEntry* const* item = __user_thread_start; item < __user_thread_end; ++item)
{
    const ThreadEntry* e = *item;
    if (e->stage != stage) continue;
    ...
}
```

**改动 3**：`framework/cmd/linker/dust_init.ld` **不需要改**——`KEEP(*(.user_init))` 对指针表同样有效。

**验证**：

1. 编译通过；
2. 上板启动日志顺序与阶段 4/5 完全一致；
3. 可顺带核对 `sizeof(void*)` 对齐：段内每个条目 4 B（32 位目标）。

**产出**：`Init_entry.hpp`、`Init_entry.cpp`（`dust_init.ld` 不动）

---

## 10. 迁移对照：业务线程怎么改成受管任务

以 `trd_test.cpp` 为例。

**old**：

```cpp
static void Task(void*, void*, void*)
{
    for (;;)
    {
        k_msleep(10);
        ...
    }
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::High, nullptr, "test");
    return true;
}
```

**new**（阶段 5 之后：Task 只写一拍，循环/等待/停止检查都在外壳）：

```cpp
static TaskResult Task(void*)
{
    ...
    return TaskResult::Continue;     // 这一拍正常，等下一拍
}

static bool test_recover(void*)
{
    return false;   // 测试线程没有可恢复错误
}

bool thread_start()
{
    thread_.SetRecoveryHook(test_recover);
    thread_.Start(Task, K_MSEC(10), ThreadPrio::High, nullptr, "test");
    return true;
}
```

**需要重连的线程（模式参照 `trd_camera.cpp`）**：

```cpp
static TaskResult Task(void*)
{
    if (!device_.IsOpen()) return TaskResult::Retry;    // 交给钩子重连
    ...
    return TaskResult::Continue;                       // 这一拍正常
}

static bool recover(void*)
{
    device_.Close();

    for (;;)
    {
        if (thread_.StopRequested()) return false;     // 停止请求：不再重连
        if (device_.Open()) return true;               // 重进 Task
        k_msleep(1000);
    }
}
```

**注意**：钩子里不要用 `WaitForStop` 做"等一会儿"，那是步进等待语义；重连间隔用 `k_msleep` 更直白，停止检查单独一行。

---

## 11. 参数与约定总表

| 项 | 值 | 位置 |
|---|---|---|
| `ThreadRegistry::kMaxThreads` | 16 | `thread_registry.hpp` |
| `Thread` 增量 RAM | ≈72 B/实例（统计口径） | `thread.hpp` |
| 统计时间源 | `k_cycle_get_64` + `k_cyc_to_ns_floor64` | `Thread::RunPeriodic` |
| 统计开关 | 先不加 Kconfig | — |
| `TaskResult` 取值 | Continue=0 / Exit=1 / Retry=2 / Fatal=3 | `thread.hpp` |
| 周期等待 | `WaitForStop(period_)` | `Thread::RunPeriodic` |
| 恢复钩子调用时机 | 仅 `Retry` | `Thread::RunPeriodic` |
| `Fatal` 处理 | 打 ERR 日志后退出线程 | `Thread::RunPeriodic` |
| 裸任务 | 不参与统计，不参与周期外壳 | `Start(k_thread_entry_t, ...)` |
| ~~stop/join 顺序~~ | ~~逆序~~ | ~~`System_Shutdown`（已删除）~~ |
| ~~join 超时~~ | ~~1000 ms~~ | ~~已删除~~ |
| 线程名 | ≤31 字节（`CONFIG_THREAD_MAX_NAME_LEN=32`） | Zephyr Kconfig |
| `REGISTER_THREAD` | 3 参数（`fn, stage, name`） | `Init_entry.hpp` |
| `REGISTER_INIT` | 6 参数（`fn, stage, level, fail, cleanup, name`） | `Init_entry.hpp` |

---

## 12. 风险与回退

| 风险 | 影响 | 应对 |
|---|---|---|
| `WaitForStop` 替换 `k_msleep` 后周期不准 | 业务节奏变化 | `WaitForStop` 的 timeout 与原 sleep 相同，停止时才提前返回；周期误差在 1 个 tick 内 |
| `TaskResult` 加 `Continue` 改枚举值 | 已有代码若按数值比较会错位 | 现有代码全部用 `TaskResult::Xxx` 符号，无数字比较；改后重新编译即可 |
| `Thread` 构造函数在静态初始化阶段调用 `k_sem_init` | 早期初始化顺序问题 | Zephyr 静态构造在驱动初始化之后、`main` 之前；`k_sem_init` 本身不依赖调度器 |
| 新增 `log.hpp` 依赖到 `thread.hpp` | 无壳编译时宏为空，无影响 | `log.hpp` 已有 `#ifdef` 空宏分支 |
| 注册表容量 16 不够 | 超出线程不参与收尾 | 打 ERR 日志；容量可调 |
| `REGISTER_INIT` 改签名 | 7 处调用点必须同步改，漏改编译失败 | 一次性全改；编译期就能发现，不会静默 |
| 统计侵入高频线程 | 1ms 周期线程多 2 次 cycle 读 + 2 次 atomic_inc | 实测 < 1µs；真影响再加 Kconfig 开关 |
| `ThreadBase` 引入 vtable | +4 B/实例，且 `Thread` 不再是纯值类型 | 无动态分配，删除拷贝构造即可 |
| `k_thread_state_str` 读状态时线程正在切换 | 读到的状态是瞬时值 | 属正常；命令输出附「瞬时」说明 |
| 统计字段被多任务并发读写 | `uint64_t` 非原子，32 位平台上撕裂 | 耗时量只由工作线程写、shell 只读，允许瞬时不一致 |

**回退**：阶段 1~3、5 只改 `thread.hpp` + 新增文件，回退就是删除新增文件、恢复 `thread.hpp`；阶段 4 的回退需要同时恢复各注册点与 `Init_entry.*`。

---

## 12.5 实施进度（2026-10-19）

| 阶段 | 状态 | 说明 |
|---|---|---|
| 0 确认 | ✅ | 见 §9 阶段 0 |
| 1 停止原语 | ✅ | `thread.hpp` 0.2 → 已并入后续版本 |
| 2 受管任务 + 钩子 | ✅ | `thread.hpp` 0.3 |
| 3 统计 + `thread` 命令 | ✅ | `thread.hpp` 0.3、`thread_registry.hpp`、`thread_cmd.*` |
| 4 注册项生命周期 | ✅ | `ThreadEntry` 独立结构 + `InitEntry.cleanup`；`stop`/`join`/`System_Shutdown`/登记表/`REGISTER_THREAD_BASE` **已删除**（用户否决）；线程控制改 `Pause`/`Resume` |
| 5 周期外壳 | ✅ | `TaskResult::Continue` + `Start(period)` + `RunPeriodic`；imu/remote/test 已迁移，shell 保持裸任务 |
| 6 `REGISTER_INIT` cleanup | ✅ | buzzer/flash 有实体，其余 `nullptr` |
| 7 指针表 | ✅ | 段内存 `const InitEntry* const` / `const ThreadEntry* const`；遍历改双层指针 |
| 目录分层 | ✅ | `init/{startup,thread,irq}`、`shell/{log,var,thread}` |
| 用户区同步 | ✅ | 7 个工作区 78 处注册点已迁移（`REGISTER_INIT` 6 参 / `REGISTER_THREAD` 3 参） |

---

## 13. 执行清单

```text
[x] 阶段 0：确认 §9 阶段 0 的各项
[x] 阶段 1：改 framework/init/thread/thread.hpp（停止原语）
[x] 阶段 1：编译验证（现有调用方零改动）
[x] 阶段 2：改 framework/init/thread/thread.hpp（受管任务 + 恢复钩子）
[x] 阶段 2：编译验证
[x] 阶段 3：改 thread.hpp（ThreadStats + ThreadBase + 统计字段）
[x] 阶段 3：新增 framework/init/thread/thread_registry.hpp
[x] 阶段 3：新增 framework/cmd/shell/thread/thread_cmd.{hpp,cpp}
[x] 阶段 3：改 framework/cmd/CMakeLists.txt + shell.cpp（命令分发 + 帮助）
[x] 阶段 3：编译验证
[x] 阶段 3：上板验证 thread list / show / pause / resume / reset 五条命令
[x] 阶段 4：改 Init_entry.hpp（ThreadEntry 独立结构 + REGISTER_INIT 加 cleanup）
[x] 阶段 4：改 Init_entry.cpp（失败回滚 + 线程启动失败检查）
[~] 阶段 4：System_Shutdown / ThreadEntry.stop/join / REGISTER_THREAD_BASE —— **已删除**
[x] 阶段 4：改 7 处注册点（REGISTER_INIT 补 cleanup 参数）
[x] 阶段 4：编译验证
[x] 阶段 4：上板验证启动日志顺序不变
[x] 阶段 5：thread.hpp 加 TaskResult::Continue + Start(period) 重载 + RunPeriodic
[x] 阶段 5：imu / remote / test 迁受管周期任务（Task 只写一拍）
[x] 阶段 5：shell 保持裸任务（事件驱动，见 §5.8 选项 A）
[x] 阶段 5：编译验证 + 上板验证 run 计数与周期
[x] 阶段 6：InitEntry + REGISTER_INIT + 7 处调用点（cleanup）
[x] 阶段 7：链接段改指针表
[x] 目录分层：init/{startup,thread,irq}、shell/{log,var,thread}
[x] 用户区同步：7 个工作区 78 处注册点改新签名（REGISTER_INIT 6 参 / REGISTER_THREAD 3 参）
[x] 文档同步：framework/init/README.md、ARCHITECTURE.md
[x] 记忆同步：project-state-snapshots
```

---

## 14. 附：Dust_Vision 其余可借鉴但不在本次范围的点

| 点 | 文件 | 为什么不在本次范围 |
|---|---|---|
| `Semaphore::Guard` 作用域锁 | `framework/init/semaphore.hpp` | MCU 侧已有 `k_mutex` / Zephyr 的 `k_mutex_lock` + 手动解锁；做个 RAII 包装是独立小优化 |
| `SubmitCommand`（shell 提交、工作线程执行硬件写） | `project/thread/camera/trd_camera.cpp:106-197` | 业务层模式，与线程层无关；适合在需要「shell 改外设参数」时单独规划 |
| `CancelCommand`（停止时取消在途命令） | 同上 | 同上 |
| `System_SetMainLoop` 主线程回调 | `framework/init/System_startup.hpp` | MCU 无 GUI 线程约束 |
| 信号处理（SIGINT/SIGTERM） | `framework/init/signal_handler.cpp` | MCU 无进程信号 |
| YAML 线程配置 | `project/thread/*/trd_*.yaml` | 属于配置层，不是线程层 |
