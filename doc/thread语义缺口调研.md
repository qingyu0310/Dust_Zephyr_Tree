# thread 语义缺口调研

> 日期：2026-10-23
> 对象：`framework/init/thread/thread.hpp`（`Thread<>` / `ThreadBase` / `TaskResult`）、`thread_registry.hpp`、`framework/shell/thread/thread_cmd.cpp`、以及把它们用起来的 imu / remote / shell / gpio / test 五个线程。
> 本文只做调研：**现状是什么、语义哪里没收清楚、会造成什么后果**。不给实现方案（那是规划文档的事），也不改代码。

---

## 0. 结论摘要

`Thread<>` 现在能正常工作，但它收的是**"正常跑"这一条路径**。异常路径、退出路径、跨线程观察路径，三处的语义都是半截的。

| # | 缺口 | 严重度 | 一句话 |
|---|---|---|---|
| A | **`Retry` 但没有恢复钩子 = 线程静默死亡** | **高** | imu 每次读失败都在走这条路 |
| B | `KeepAlive` 是死标志，没有任何读取者 | **高** | "fatal 时保留 shell"这条语义在代码里不存在 |
| C | 三个 `uint64_t` 统计量跨线程非原子读写 | **高** | 32 位 RISC-V 上会被撕成两半 |
| D | `IsAlive()` 判断里有一个永不匹配的字符串 | 中 | `"exited"` 不是 Zephyr 的状态字符串 |
| E | 没有停止原语，线程退出后注册表项永久残留且无法重启 | 中 | 死了的线程只能看，不能救 |
| F | `Start()` 无幂等保护 | 低 | 二次调用直接在旧 `k_thread` 上重建 |
| G | 模板参数 `StackSize` ≠ 可用栈字节 | 中 | 有 PMP guard 时差距很大 |
| H | 周期是"间隔"不是"节拍"，且没有任何超时观测 | 中 | 文档写了，API 名没体现，统计也不反映 |
| I | `doc/线程编写指南.md` 已与代码不一致 | 中 | §6.1/6.2/6.5/6.8 描述的裸任务和 `Fatal` 都已不存在 |
| J | 注册表满时 `Add` 静默失败 | 低 | 上限 16，当前 5 个，短期无风险 |
| K | `REGISTER_THREAD` 的 fail 策略只管创建那一刻 | 中 | 线程跑起来之后死掉，`HaltOnFail` 什么都不做 |

**最该先处理的三个是 A、B、C。** 它们都不是"设计不够优雅"，是**现在就会出错的**。

**不在缺口之列**：`TaskResult` 只有 `Continue` / `Retry` 两态，**不是缺"正常结束"态**——MCU 上线程常驻，不存在"任务跑完就退"的语义，任务要么继续，要么出错交给钩子。

---

## 1. 现状：链路里到底有什么

### 1.1 唯一的创建入口

```cpp
// framework/init/thread/thread.hpp:126-144
void Start(TaskFunc entry, k_timeout_t period, ThreadPrio prio, void* context, const char* name)
{
    task_    = entry;
    period_  = period;
    context_ = context;

    atomic_clear(&paused_);
    name_ = name;
    k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), RunEntry, this, nullptr, nullptr, static_cast<uint8_t>(prio), 0, K_NO_WAIT);
    atomic_set(&started_, 1);

    if (name != nullptr) k_thread_name_set(&thread_, name);
    RegisterThreadInstance(this);
}
```

2026-10-22 之前还有裸任务重载 `Start(k_thread_entry_t, ...)`，现在删掉了，**全仓库只剩这一个入口**，所有线程都是受管任务（`TaskFunc`）。

### 1.2 外壳 `Run()`

```cpp
// thread.hpp:294-325
for (;;)
{
    atomic_inc(&run_count_);
    const uint64_t begin_cycle = k_cycle_get_64();
    const TaskResult result = task_(context_);
    const uint64_t elapsed = k_cycle_get_64() - begin_cycle;
    run_cycles_ += elapsed;
    last_run_cycles_ = elapsed;
    if (elapsed > max_run_cycles_) max_run_cycles_ = elapsed;

    if (result == TaskResult::Retry)
    {
        atomic_inc(&retry_count_);
        if (hook_ == nullptr || !hook_(context_)) return;   // ← 唯一的退出点
    }

    if (!K_TIMEOUT_EQ(period_, K_NO_WAIT)) k_sleep(period_);
}
```

### 1.3 五个线程的用法

| 线程 | 位置 | 周期 | 用 Retry？ | 设了钩子？ |
|---|---|---|---|---|
| `imu` | `framework/modules/imu/drivers/imu.hpp:45` | `K_MSEC(1)` | **是**（`imu.cpp:106`） | **否** |
| `remote` | `framework/modules/remotes/remote.hpp:77` | `K_NO_WAIT` | 是（`remote.cpp:50`） | 是（`remote.hpp:76`） |
| `shell` | `framework/shell/shell.hpp:38` | `K_NO_WAIT` | 否 | 否 |
| `output`（gpio） | `project/thread/gpio/trd_gpio.cpp:61` | `K_MSEC(1)` | 否 | 否 |
| `test` | `project/thread/test/trd_test.cpp:133` | `K_MSEC(10)` | 否 | 否 |

### 1.4 对外能力

`ThreadBase` 暴露 9 个虚函数（`thread.hpp:69-98`）：`Name` / `Tid` / `GetStats` / `ResetStats` / `SetKeepAlive` / `KeepAlive` / `Pause` / `Resume` / `Paused`。
shell 侧 `thread` 命令只用了 `list` / `show` / `pause` / `resume` / `reset` 五个子命令（`thread_cmd.cpp:198-217`）。

---

## 2. 缺口逐条

### A. `Retry` 但没有恢复钩子 → 线程静默死亡（**高**）

**现状**：`thread.hpp:311-318`，`hook_ == nullptr` 时直接 `return`：

```cpp
if (result == TaskResult::Retry)
{
    atomic_inc(&retry_count_);
    if (hook_ == nullptr || !hook_(context_)) return;
}
```

`retry_count_` 会 +1，然后线程**直接退出**，`Run()` 返回 → `RunEntry` 返回 → `z_thread_entry` 调 `k_thread_abort`（`zephyr/lib/os/thread_entry.c:46`）。**没有任何日志**（`Fatal` 被删时把 `DUST_LOG_ERR` 一起删掉了）。

**实例**：imu 正是这条路。

```cpp
// framework/modules/imu/drivers/imu.cpp:105-107
if (self->source_ == nullptr || !self->source_->Read(self->sample_)) {
    return TaskResult::Retry;
}
```

而 `ImuManager::Start` 从未调用 `SetRecoveryHook`：

```cpp
// framework/modules/imu/drivers/imu.hpp:40-47
bool Start(ThreadPrio prio = ThreadPrio::Normal)
{
    if (!ready_) return false;
    thread_.Start(TaskEntry, kPeriod, prio, this, "imu");   // 没有 SetRecoveryHook
    return true;
}
```

**后果**：IMU 只要**读失败一次**，`imu` 线程就永久死掉，姿态解算链路静默中断。界面上唯一痕迹是 `thread list` 里 `imu` 那行的 STATE 变成 `dead`、`RUN` 不再增长——而那要人去主动敲命令才会发现。启动期的 `[boot] imu_start start` 早就打过了，看起来一切正常。

**这是当前代码里唯一一处"一次瞬时错误 = 永久停摆"的地方。**

### B. `KeepAlive` 是死标志（**高**）

**现状**：
- 写入方：`framework/shell/shell.cpp:179` `shell_.SetKeepAlive(true);`，注释写"fatal 冻结业务线程时保留 shell（日志消费者 + 观察窗口）"。
- 读取方：**全仓库为零**。

```
grep -rn "KeepAlive()" --include=*.cpp --include=*.hpp .   →  无输出（thread.hpp 自身的定义除外）
```

`fatal.cpp` 里也没有任何冻结/暂停逻辑：

```cpp
// framework/cmd/fatal/fatal.cpp:48-59
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());
    char line[128];
    snprintf(line, sizeof(line), "[fatal] %s (reason=%u) thread=%s", ...);
    FatalOut(line);
    DumpEsf(esf);
    while (1) {}
}
```

**而且即使写了冻结也没用**——`z_fatal_error` 在调用 handler **之前**就关了中断，**handler 返回之后**才开：

```c
// zephyr/kernel/fatal.c
unsigned int key = arch_irq_lock();          // :91
...
k_sys_fatal_error_handler(reason, esf);      // :119
...
arch_irq_unlock(key);                        // :174
```

我们的 handler 是 `while (1) {}`（`fatal.cpp:58`），**永远不返回** → 中断永远开不了 → 除当前上下文外**没有任何线程会再被调度**。所以：

- "保留 shell 让它继续发日志"在 fatal 路径上**物理上不可能**——这也是 `FatalOut` 必须逐字节轮询直发的原因（`fatal.cpp:62-77` 的注释已经写明）。
- `KeepAlive` 这条语义链：**写入方存在 → 语义注释存在 → 读取方不存在 → 实际执行路径不可能实现**。四段里断了两段。

**结论**：要么 `KeepAlive` 的语义描述是错的（它描述的机制根本不存在），要么它属于"为将来预留"。当前状态下它是纯粹的死代码 + 误导性注释。

### C. 三个 `uint64_t` 统计量跨线程非原子读写（**高**）

**现状**：

```cpp
// thread.hpp:278-280
uint64_t     run_cycles_      = 0;                // 累计执行周期（仅工作线程写）
uint64_t     max_run_cycles_  = 0;                // 单次最长执行周期（仅工作线程写）
uint64_t     last_run_cycles_ = 0;                // 最近一次执行周期（仅工作线程写）
```

注释说"仅工作线程写"——**写**确实只有 `Run()` 一个写入者，**但读**发生在 shell 线程：

```cpp
// thread.hpp:251-253（由 shell 线程经 ThreadCmd::CmdList/CmdShow 调用）
out.run_ns      = k_cyc_to_ns_floor64(run_cycles_);
out.max_run_ns  = k_cyc_to_ns_floor64(max_run_cycles_);
out.last_run_ns = k_cyc_to_ns_floor64(last_run_cycles_);
```

**目标平台是 RV32（HPM5361，32 位）**，`uint64_t` 的读写被拆成两条 32 位指令。shell 线程在两次指令之间被抢占，就可能读到"高 32 位是旧的、低 32 位是新的"的撕裂值。

**后果**：`thread show` 的 `last` / `max` / `total` 毫秒数偶尔会跳出一个荒谬的大值（或回绕）。这不是崩溃级问题，但**会让性能排查得出错误结论**——而这几个数存在的唯一目的就是性能排查。

对比：`run_count_` / `retry_count_` / `drop_count_` 都是 `atomic_t`，读写在 32 位上天然原子，所以没有问题。**偏偏最需要准确性的耗时统计用了最容易撕裂的类型。**

### D. `IsAlive()` 里有一个永不匹配的判断

```cpp
// thread.hpp:331-336
bool IsAlive() const
{
    char buf[32];
    const char* state = k_thread_state_str(const_cast<k_thread*>(&thread_), buf, sizeof(buf));
    return (state != nullptr) && (std::strstr(state, "dead") == nullptr) && (std::strstr(state, "exited") == nullptr);
}
```

Zephyr 的状态字符串只有八个：`dummy` / `pending` / `sleeping` / `dead` / `suspended` / `aborting` / `suspending` / `queued`（`zephyr/kernel/include/kthread.h:19-26`）。**没有 `"exited"`**。那个 `strstr(state, "exited")` 永远返回 `nullptr`，是一条死条件。

**另一个隐患**：`k_thread thread_ {}`（`thread.hpp:262`）在 `Start()` 之前是全零，`thread_state == 0` → `k_thread_state_str` 返回空串 `""` → `IsAlive()` 返回 `true`。也就是说**一个从未启动过的 `Thread` 对象，`IsAlive()` 会报"活着"。**

这个坑目前被 `GetStats` 的返回值挡住了：

```cpp
// thread.hpp:258
return atomic_get(&started_) != 0;      // 未启动 → 返回 false
```

`thread_cmd.cpp:50` 和 `:83-85` 都用这个返回值决定是否输出（未启动时打 `thread show: not started`）。所以**对外行为正确，但 `IsAlive()` 本身是错的**——任何将来直接调 `IsAlive()` 的地方都会踩。

### E. 没有停止原语；线程退出后注册表项永久残留

**现状**：`ThreadBase` 没有 `RequestStop` / `StopRequested` / `WaitForStop` / `Join`。全仓库搜 `k_thread_abort` / `k_thread_join` / `StopRequested`，除 Zephyr 自身外**零命中**。

`thread_registry` 只有 `Add` / `At` / `Count` / `Find` / `Reset`（`thread_registry.hpp:34-87`），**没有 `Remove`**。`Reset` 注释写着"仅调试/重启流程使用"，全仓库也没有调用者。

**后果**：
1. 线程一旦退出（A 那条路），注册表里那一项永远在，`GetStats` 返回 `running=false` → `thread list` 显示 `dead`。**能看到，但没有任何手段让它重新起来**（`Start()` 理论上可以再调，但见 F）。
2. 想做"某个线程出问题自动重启"这种自愈，当前**没有任何基础设施**：注册表不跟踪生命周期，`Thread` 也没有"重启"这个操作。
3. `remote` 的注释还留着旧假设：

   ```cpp
   // framework/modules/remotes/remote.cpp:22
   * 循环、停止检查由 Thread 外壳负责；节拍由这里的 k_sem_take 决定：
   ```

   **外壳没有"停止检查"**。这句注释描述的是一个不存在的机制。

### F. `Start()` 无幂等保护

`Start()` 里没有任何"已启动"判断，直接 `k_thread_create` 到同一个 `thread_` 成员上。二次调用会：
- 在**已终止或正在运行的** `k_thread` 结构上重新初始化（Zephyr 允许对 dead 线程这么做，但对 running 线程是 UB）；
- `RegisterThreadInstance` 会因为注册表按指针去重（`thread_registry.hpp:38-43`）而不重复添加，所以注册表不会膨胀；
- 但 `started_` 已经是 1，`atomic_clear(&paused_)` 会把暂停状态清掉——**如果线程是暂停中被二次 Start，会静默恢复**。

当前没有任何地方二次调用 `Start()`，所以这只是"没定义的语义"，不是现存 bug。但它和 E 里的"重启"需求直接相关：想做重启就必须先把这条语义定下来。

### G. 模板参数 `StackSize` 不等于可用栈字节

```cpp
// thread.hpp:263
K_KERNEL_STACK_MEMBER(stack_, StackSize);
```

`K_KERNEL_STACK_MEMBER` 展开成 `K_KERNEL_STACK_LEN(size)` 大小的数组（`zephyr/include/zephyr/kernel/thread_stack.h:442`），而

```c
// thread_stack.h:97-108
#define Z_KERNEL_STACK_SIZE_ADJUST(size) (ROUND_UP(size, ARCH_STACK_PTR_ALIGN) + K_KERNEL_STACK_RESERVED)
#define K_KERNEL_STACK_LEN(size) ROUND_UP(Z_KERNEL_STACK_SIZE_ADJUST(size), Z_KERNEL_STACK_OBJ_ALIGN)
```

当前板卡 `.config`：`CONFIG_PMP_STACK_GUARD=y`、`CONFIG_PMP_POWER_OF_TWO_ALIGNMENT=y`、`CONFIG_PMP_STACK_GUARD_MIN_SIZE=256`、`CONFIG_PMP_GRANULARITY=4`。

RISC-V 侧：

```c
// zephyr/include/zephyr/arch/riscv/arch.h:52
#define Z_RISCV_STACK_GUARD_SIZE Z_POW2_CEIL(MAX(sizeof(struct arch_esf) + CONFIG_PMP_STACK_GUARD_MIN_SIZE, Z_RISCV_STACK_PMP_ALIGN))
#define ARCH_KERNEL_STACK_RESERVED Z_RISCV_STACK_GUARD_SIZE    // :74
```

按当前配置推算（`arch_esf` 在 RV32 上约 80B，+256 → 336 → 向上取 2 的幂 = **512**）：`K_KERNEL_STACK_RESERVED` ≈ **512B**。

也就是说 `Thread<128>`（`trd_gpio.cpp:23`）**名义写 128，实际对象 1024B，扣掉 guard 后可用 512B**。传进 `k_thread_create` 的是 `K_KERNEL_STACK_SIZEOF(stack_)`（= 对象大小 − reserved = 512），不是 128。

**这不是 bug，但语义没写清楚**：参数名叫 `StackSize`，读代码的人会以为是"给线程 128 字节栈"。实际是"请求 128，最终按 guard 和 2 的幂对齐向上膨胀"。当前 `Thread<128>` 能跑（GPIO 心跳只有极少局部变量），但**照这个直觉写业务线程，会在某个局部大数组上突然踩到 guard**——而且踩到的是 PMP fault → fatal，不是好排查的错。

### H. 周期是"间隔"不是"节拍"，且没有超时观测

```cpp
if (!K_TIMEOUT_EQ(period_, K_NO_WAIT)) k_sleep(period_);
```

- 这是"上一拍结束到下一拍开始之间睡 `period_`"，**任务自身耗时会把实际周期拉长**。
- `doc/线程编写指南.md:598-620` 已经写清了这个语义，但 **API 名字 `period` 没有任何提示**，`ThreadPrio`/`period` 并列传参更容易让人误读成固定频率。
- **没有任何观测手段**：`ThreadStats` 里有 `last_run_ns` / `max_run_ns`（任务体耗时），但**没有"实际周期"**这个量。想知道 imu 是不是真在 1kHz 跑，只能靠外部逻辑。而 `run_count` 是绝对值，得自己掐表算差分。

### I. 文档与代码已经不一致

`doc/线程编写指南.md` 里有三处当前代码已不存在的内容：

| 位置 | 文档写的 | 实际 |
|---|---|---|
| `:405-441` §6.1 | 三个 `Start` 重载（裸任务 + 受管无周期 + 受管带周期） | **只剩一个**受管带周期版本 |
| `:443-462` §6.2 | "路径 A：裸任务"，`k_thread_entry_t` 入口，自写 `for(;;)` | 已删除；`Task` 必须是 `TaskResult(*)(void*)` |
| `:542-559` §6.5 | `TaskResult` **四**态，含 `Fatal = 2` | 只有**两**态 |
| `:621-650` §6.8 | `ThreadStats` 含 `fatal_count` | 字段已删除 |

另外文档 §6.8 说 `stack_unused` 需要 `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO`——这两个当前都是 `y`（`.config:68` / `:529`），所以这一条是准的。

这份指南是"读完能上手写线程"的入口文档，现在会**教出三种已不存在或不被允许的写法**。

### J. 注册表满时静默失败

```cpp
// thread_registry.hpp:45
if (count_ >= kMaxThreads) return false;    // 上限 16
```

调用方：

```cpp
// framework/init/startup/Init_entry.cpp:35-38
void RegisterThreadInstance(ThreadBase* thread)
{
    (void)thread_registry::Add(thread);     // 返回值被丢弃
}
```

当前 5 个线程，离 16 很远，**短期无风险**。但它意味着：将来线程数超过 16 时，超出的线程**照常运行、但不进注册表**——`thread list` 少一行，`thread pause <name>` 报 `not found`，而没有任何警告。

### K. 两套错误语义没有关系说明

线程的"失败"有两条完全不同的路：

1. **启动期**：`REGISTER_THREAD(fn, stage, fail, name)` 的 `fail`（`HaltOnFail` / `ContinueOnFail`），由 `Init_entry.cpp:196-233` 处理，**只覆盖 `thread_start()` 返回 false 这一瞬间**。
2. **运行期**：`TaskResult::Retry` + 恢复钩子，由 `Thread::Run` 处理。

两者名字上都叫"失败"，但：
- `HaltOnFail` 在运行期**完全不起作用**——线程跑起来之后死掉，不会触发任何停机。
- `ContinueOnFail` 在线程死掉之后也**不会**让它继续。

也就是说 `REGISTER_THREAD` 的 `fail` 策略**只保护了创建那一刻**，它管不到创建之后的任何事。这个边界目前在代码里没有注释说明，`doc/线程编写指南.md` §5.3 也只讲了启动期。

---

## 3. 什么不算缺口（避免过度设计）

以下是我检查过、**确认当前状态是合理的**，不该为了"完整"去补：

1. **`TaskResult` 只有两态**。MCU 上线程常驻，不存在"任务跑完就退"的语义——任务要么返回 `Continue` 继续，要么出错返回 `Retry` 交给钩子。不需要第三态。
2. **没有抢占式时间片控制**。`CONFIG_TIMESLICING=y`、`CONFIG_TIMESLICE_PRIORITY=0`（`.config`），即优先级 0 的同级线程会轮转。`ThreadPrio` 从 1 开始，全在可抢占区，行为符合预期。
3. **`Pause()` 用 `k_thread_suspend` 而不是软件标志**。内核级挂起保留全部现场，比自建标志位干净；文档 §6.9 已经把"持锁被挂起会死锁"的风险写清楚了。
4. **恢复钩子在**被管理线程自己的上下文**里跑**（`Run()` 里调用），不是中断上下文，语义正确。
5. **`RunEntry` 静态函数 + `void* p1 = this`** 的画法是 Zephyr 标准做法，没有额外开销问题。
6. **`RegisterThreadInstance` 做成自由函数放在 `Init_entry.cpp`**，是为了让 `thread.hpp` 不 include `thread_registry.hpp`，避免头文件环——这个设计是对的。
7. **没有线程优先级动态调整**。当前 5 个线程的优先级是写死的，没有运行时改优先级的需求。

---

## 4. 建议处理顺序（只排序，不做决定）

| 顺序 | 缺口 | 理由 |
|---|---|---|
| 1 | **A**（Retry 无钩子静默死） | 现存的静默失效，imu 随时会中 |
| 2 | **C**（64 位统计撕裂） | 现存的错误数据，会让后续所有性能判断失效 |
| 3 | **B**（KeepAlive 死标志） | 要么补实现、要么删标志和注释，不能留"看起来有保护"的假象 |
| 4 | **I**（文档失效） | 会教错人，改动成本最低 |
| 5 | **G**（StackSize 语义） | 需要先定"要不要在 API 上体现 guard"，再动 |
| 6 | **E / F**（停止原语 / Start 幂等） | 同一条设计线：先定"线程能不能被主动停止"，再定 `Start` 幂等。这一组需要用户拍板方向 |
| 7 | **D / H / J / K** | 局部修正，随事随改 |

---

## 5. 附：本次调研的核对方法

每条结论都对应到具体代码位置，未使用推测：

- `thread.hpp` 全文逐行读（357 行）
- `thread_registry.hpp` 全文（92 行）
- `thread_cmd.cpp` 全文（227 行）
- `imu.cpp` / `imu.hpp` / `remote.cpp` / `remote.hpp` / `shell.cpp` / `trd_gpio.cpp` / `trd_test.cpp` 中与线程交互的部分
- `Init_entry.cpp` 启动器全文（290 行）
- `fatal.cpp` 全文（87 行）
- Zephyr 侧：`kernel/fatal.c`（`z_fatal_error` 的中断开关顺序）、`kernel/thread.c`（`k_thread_state_str`、`setup_thread_stack`）、`kernel/sched.c`（`k_thread_suspend`/`resume`）、`lib/os/thread_entry.c`（`z_thread_entry` 的 abort）、`include/zephyr/kernel/thread_stack.h`（栈大小公式）、`include/zephyr/arch/riscv/arch.h`（PMP guard 大小）、`include/zephyr/arch/riscv/exception.h`（`arch_esf` 字段）
- 当前构建配置：`project/build/zephyr/.config`（`TIMESLICE_*`、`PMP_*`、`INIT_STACKS`、`THREAD_STACK_INFO`、`ASSERT`、`HW_STACK_PROTECTION`、`THREAD_MONITOR`）

**未核实项**：`arch_esf` 在 RV32 上的精确 `sizeof` 是按字段推算的（约 80B），未从编译产物读取；由此推出的 512B guard 也是推算值，量级可信、精确值需以 map 文件或编译期 `static_assert` 为准。
