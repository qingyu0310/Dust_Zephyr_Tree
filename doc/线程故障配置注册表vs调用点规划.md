# 线程故障配置：注册表 vs 调用点传参

> 2026-10-24。当前形态是「链接段集中注册 + `Start()` 按线程名查表」。评估另一种形态：**配置作为 `Start()` 的参数**，与线程名在同一个调用点。两者没有绝对优劣，本文件把取舍写死，供拍板。
>
> **状态：待拍板。**

---

## 1 两种形态

### 1.1 现状（A）：链接段注册 + 名字查表

```cpp
// trd_imu.cpp 文件末尾
REGISTER_THREAD_FAULT(::imu::ImuManager::OnRetry, ::imu::ImuManager::OnExit, Recover, "imu");

// imu.hpp 里
thread_.Start(TaskEntry, kPeriod, prio, this, "imu");
```

- 配置集中在文件末尾一行，与 `REGISTER_INIT` / `REGISTER_THREAD` 并列。
- 线程名在**两个地方**各写一次（`Start` 的 `name` 和宏的 `name_`），靠 `strcmp` 在运行期绑定。

### 1.2 备选（B）：`Start()` 参数

```cpp
// imu.hpp 里，配置就在调用点
thread_.Start(TaskEntry, kPeriod, prio, this, "imu",
              ThreadFaultConfig{ OnRetry, OnExit, Recover });
```

- 名字只出现一次，**不可能不匹配**。
- 配置与线程名同处一个调用点，读代码不用跳文件末尾。
- 不需要链接段、不需要 `FindThreadFault`、不需要 `ThreadFaultEntry`。
- `fault_` 可以直接是 `const` 成员（构造时定死，不再运行期拷贝）。

带的默认值，不传就是默认策略：

```cpp
struct ThreadFaultConfig
{
    RetryHook         retry      = nullptr;                        // Retry 钩子
    ExitHook          exit       = nullptr;                        // 亡语
    ThreadFaultPolicy policy     = ThreadFaultPolicy::Recover;     // 故障策略
    bool              keep_alive = false;                          // Recover 全杀时是否保留
};

void Start(TaskFunc entry, k_timeout_t period, ThreadPrio prio, void* context,
           const char* name, const ThreadFaultConfig& fault = {});
```

`keep_alive` 进配置的理由：之前排除它是因为「注册宏要多写一个参数」，而 `keep_alive` 只有 `shell` 一处用。改成调用点传参后这个理由消失了——传参本来就带默认值，`shell` 写 `true`、其余不写即可，没有多余成本。而它本来就是故障契约的一部分（"出故障时留不留我"），收进同一个结构才完整。

---

## 2 对比

| 维度 | A 注册表 | B 调用点参数 |
|------|---------|-------------|
| 线程名写几次 | **2 次**（`Start` + 宏），运行期 `strcmp` 绑定 | **1 次** |
| 名字写错 | 静默退回默认 + 启动 WRN | 不可能 |
| 配置位置 | 文件末尾一行，集中 | `Start()` 调用点，分散 |
| 与 `REGISTER_INIT`/`REGISTER_THREAD` 一致性 | ✅ 同构 | ❌ 不同构（配置回到代码里） |
| 是否需要链接段 | 需要 `.thread_fault` + 链接脚本 | **不需要** |
| 是否需要查表 | 需要 `FindThreadFault` + 5×O(n) 字符串比较 | **不需要** |
| `fault_` 能否 `const` | 否（查表后赋值） | **是**（构造参数） |
| 运行期改配置 | 不可（表在 ROM） | 不可（`const` 成员） |
| `keep_alive` 归属 | 独立接口 `SetKeepAlive()` | 进 `ThreadFaultConfig`，与策略同处 |
| 删除代码量 | — | 可删 `ThreadFaultEntry`、`FindThreadFault`、`.thread_fault` 段、5 处注册宏、`SetKeepAlive` 虚函数与实现 |
| 改动面 | 已实现 | 5 处 `Start` 调用 + 删一段机制 + 文档回填 |
| 诊断 | 启动 WRN 能发现漏配 | 漏配即默认值，无提示（但也没有"漏"这回事——默认值就是声明） |

### 2.1 关键差异一句话

**A 把「名字”当成主键，B 把「调用点」当成主键。** A 换来的是集中与框架一致性，代价是一个运行期绑定；B 换来的是类型安全和零机制，代价是配置散落。

---

## 3 推荐

**B**，理由三条：

1. **消除唯一的隐患**。A 现在最大的问题就是名字对不上只能靠启动日志发现；B 从根上不存在这个问题。
2. **删代码**。A 需要链接段 + 结构 + 查表函数 + 5 处宏；B 只需要一个带默认值的参数。少了约 60 行机制代码和一个链接段。
3. **`fault_` 变 `const`**，语义更准：配置是编译期定死的，就不该是可写成员。

**A 的唯一优势**是「所有注册集中在一处」的整齐感。但这个整齐感在 `shell` 上已经破了：它一半在 `REGISTER_THREAD_FAULT`、一半在 `shell_.SetKeepAlive(true)`，本来就不完整。

B 把 `keep_alive` 一并收进 `ThreadFaultConfig`，顺手消掉接口不对称——三个 setter 已删、`SetKeepAlive` 还留着的问题也一起解决，`ThreadBase` / `Thread` 里的 `SetKeepAlive` 与 `KeepAlive()` 两个虚函数都可以删掉（`Halt()` 直接读配置）。

---

## 4 执行步表（B）

> **拆成 8 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 排序原则：**先加新参数、再迁调用点、最后删旧机制**，全程不出现编不过的中间态。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|----|------|------|----------|------|
| **1** | `framework/init/thread/thread_fault.hpp` | 新增 `ThreadFaultConfig{retry, exit, policy, keep_alive}` | 无 | 无 |
| **2** | `framework/init/thread/thread.hpp` | `Start()` 加第 6 个参数 `const ThreadFaultConfig& fault = {}`，内部写入 `fault_` 与 `run_.keep_alive`；**暂时保留查表逻辑**（参数优先，未传时回退查表）；`KeepAlive()` 改为读 `run_.keep_alive`（本来就是） | 步 1 | 无 |
| **3** | `framework/modules/imu/drivers/imu.hpp` + `project/thread/imu/trd_imu.cpp` | `imu_.Start()` 内传 `ThreadFaultConfig{OnRetry, OnExit, Recover}`；删 `trd_imu.cpp` 的注册宏 | 步 2 | 无 |
| **4** | `framework/modules/remotes/remote.hpp` + `trd_remote.cpp` | 同步第 3 步 | 步 2 | 无 |
| **5** | `framework/shell/shell.hpp` + `shell.cpp` | `shell_.Start()` 改为传 `ThreadFaultConfig{nullptr, nullptr, Recover, true}`；删 `shell_.SetKeepAlive(true)` 与 `shell.hpp` 的 `SetKeepAlive` 包装 | 步 2 | 无 |
| **6** | `project/thread/gpio/trd_gpio.cpp`、`project/thread/test/trd_test.cpp` | 改为在 `Start()` 里传配置；删注册宏 | 步 2 | 无 |
| **7** | `framework/init/thread/thread.hpp` + `thread_fault.hpp/.cpp` + `framework/cmd/linker/dust_init.ld` | 删查表逻辑、`ThreadFaultEntry`、`FindThreadFault`、`REGISTER_THREAD_FAULT`、`.thread_fault` 段；`fault_` 改 `const`；删 `SetKeepAlive` 的两个虚函数与实现 | 步 3~6 | 无 |
| **8** | 全量检查 + 文档回填 | `grep -rn "REGISTER_THREAD_FAULT\|FindThreadFault\|SetKeepAlive"` 零命中；更新 `framework/init/README.md`、`ARCHITECTURE.md`、`快速上手.md`、`子树架构介绍.md`、`doc/线程编写指南.md`、主 `README.md` | 步 7 | 无 |

**编译状态**：全程无编不过的中间态（步 2 参数优先、查表兜底，新旧并存）。

**编译是用户的动作，AI 不代跑。**

---

## 5 验证清单

| # | 场景 | 预期 |
|---|------|------|
| 1 | 正常启动 | 五个线程全部起来，无任何 WRN |
| 2 | `var set test_trigger 1` | `retry handled`，线程继续 |
| 3 | `var set test_trigger 2` | `exit hook` → 全杀 |
| 4 | `thread exit output` | 只杀 `output` |
| 5 | 全杀后 `thread list` | 只剩 `shell`（`keep_alive` 来自参数） |
| 6 | `grep -rn "REGISTER_THREAD_FAULT\|.thread_fault"` | 零命中 |
| 7 | map 文件 | 无 `.thread_fault` 段 |

---

## 6 待拍板

1. **A 保持 / B 切换**（§3 推荐 B）。
2. 若切 B：`ThreadFaultConfig` 的字段顺序 —— `{retry, exit, policy, keep_alive}`（本规划）还是与 `Start` 其他参数同序？
3. 若切 B：`shell` 的 `SetKeepAlive` 包装函数（`shell.hpp:55`）直接删还是留作对外接口。
