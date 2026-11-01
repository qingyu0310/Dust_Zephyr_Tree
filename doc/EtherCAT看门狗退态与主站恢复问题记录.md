# EtherCAT 看门狗退态与主站恢复问题记录

日期：2026-10-31。

## 当前结果

用户报告，主站增加 AL 状态监测及 SAFE-OP 错误恢复后，已经运行二十多分钟，最新从站日志仍显示正常 OP 和接收回调增长。

```text
ecat diag: pdi=2502491 sm2=2502847
ecat diag: last_sm2_age_ms=1 max_gap_ms=58688 sm3=2502847
ecat app: rx_callbacks=2368950

ecat diag: state=0x08 code=0x0000 output=1 event=0x0000
ecat diag: pdi=2503374 sm2=2503730
ecat diag: last_sm2_age_ms=1 max_gap_ms=58688 sm3=2503730
ecat app: rx_callbacks=2369833
```

相邻记录中 SM2、SM3 和实际接收回调均增加 883 次；状态为 OP、错误码为零，最近 SM2 事件距今 1 ms。历史最大间隔保持 58688 ms，没有在这两条记录之间继续增加。

这是阶段性的运行证据，不表示最初断流根因已经消除，也不是连续二十多分钟完全无中断的证明。当前版本可能已具备自动恢复能力，使故障不再表现为永久停止接收。

## 最初现象

业务接收回调 `project/thread/test/trd_test.cpp::on_master_data()` 原本打印每方向八个 32 位计数器。运行一段时间后，EtherCAT RX 日志消失，但 GPIO 心跳 INF 和 shell 命令响应仍能输出。

最初怀疑过日志队列或串口吞吐量。静态检查确认存在日志超载条件：

- 两条带前缀、颜色与换行的 INF 各约 47 字节；若每秒回调 1000 次，仅这两条就产生约 94000 B/s。
- UART 配置为 921600 波特率，按 8N1 计算理论容量约 92160 B/s。
- 原始记录池与发送帧池均为 8 条，存在满队列静默丢弃路径。

这些因素能解释逐帧日志丢行，但不足以解释实际接收回调计数停止。后续诊断确认，此次 RX 停止具有明确的 EtherCAT 状态原因。

## 从站诊断及其含义

新增低频诊断，每秒报告：

```text
ecat diag: state=... code=... output=... event=...
ecat diag: pdi=... sm2=...
ecat diag: last_sm2_age_ms=... max_gap_ms=... sm3=...
ecat app: rx_callbacks=...
```

| 字段 | 含义与限制 |
|---|---|
| state | 软件 AL 基础状态，不包含错误位 |
| code | 软件记录的 AL 状态码 |
| output | 是否允许运行业务输出接收处理 |
| event | 采样时读到的 AL Event；零不代表此前没有事件 |
| pdi | PDI 入口累计次数 |
| sm2 / sm3 | SM2/SM3 事件处理累计次数，含线程轮询处理；不是独立硬件报文计数 |
| rx_callbacks | 实际业务接收回调累计次数，长度检查之前递增 |
| last_sm2_age_ms | 距最近一次软件处理 SM2 事件的毫秒数；没有收到过时显示零 |
| max_gap_ms | 相邻两次软件处理 SM2 事件的历史最大间隔；跨停机、重连的间隔也会计入 |

错误检查关闭输出时还抓取：

```text
ecat local state: 0x08 -> 0x04 code=...
ecat stop: last_sm2_age_ms=... max_gap_ms=...
ecat stop SM2: addr=... len=... settings=...
ecat watchdog: divider=... time=... status=...
```

对应实现主要位于 `core/ecat_device.cpp/.hpp`、`core/al/ecat_state_machine.cpp` 和 `project/thread/test/trd_test.cpp`。诊断不修改收发逻辑或放宽看门狗。

## 已确认的故障过程

一次故障后持续出现：

```text
state=0x04 code=0x001b output=0
pdi=1132 sm2=1132
rx_callbacks=1132
```

含义：SM 过程数据看门狗超时，代码将从站从 OP 退到 SAFE-OP，关闭业务输出处理。

后续另一组日志更明确地显示“事件恢复，但业务没有恢复”：

```text
state=0x04 code=0x001b output=0
sm2=71880 sm3=71880 rx_callbacks=71880
last_sm2_age_ms=25613 ... 28614

state=0x04 code=0x001b output=0
sm2=72013 sm3=72013 rx_callbacks=71880
last_sm2_age_ms=1 max_gap_ms=29465

state=0x04 code=0x001b output=0
sm2=72886 sm3=72886 rx_callbacks=71880
```

软件处理 SM2 事件出现约 29.465 秒空档，之后 SM2/SM3 再次增长，但从站仍处于 SAFE-OP 错误态，接收回调没有恢复。

当前业务门控为：

```cpp
if (state_machine_.IsOutputRunning()) {
    process_data_.ConsumeOutputs(...); // 进入业务接收回调
} else {
    AcknowledgeSyncManagerArea(...);   // 只确认事件
}
```

因此 `output=0` 时，恢复收到过程数据事件并不意味着业务可以重新执行；必须按 AL 状态机完成恢复。

用户随后重启上位机，接收恢复。这支持“重新建链能恢复业务、原先运行态恢复不完整”的判断，不能单独证明最初断流由主站造成。

## 主站如何检测和恢复

从站 `Publish()` 在该故障下向 ESC 发布：

| 寄存器 | 预期值 | 含义 |
|---|---|---|
| AL Status `0x0130` | `0x0014` | SAFE-OP `0x04` 加错误位 `0x10` |
| AL Status Code `0x0134` | `0x001b` | SM 过程数据看门狗超时 |

主站不能只用 WKC 或初始化时的 configured 标志判定业务正常；WKC 与从站业务是否处于 OP 是不同条件。

此前本地主站参考代码的周期 `Read()` 主要检查 WKC，WKC 不足时交由 Retry 拆链重连。它没有完整覆盖“过程数据仍有响应，但从站已退出 OP”的状态恢复。

用户反馈，主站已实施以下修改（属于用户报告，本文没有重新逐行审查该版主站源码）：

1. `Read()` 每 100 ms 检查完整 AL Status；WKC 异常时立即检查。
2. `ReadAlStatus()` 直接读取 ESC 状态和错误码，日志记录完整 16 位状态、WKC、期望 WKC 与 since_send。
3. 从站退出 OP 时明确报告输出已停止。
4. `Recover()` 在 SAFE-OP 错误态先写 `0x0014` 确认错误，保持周期收发，然后请求 `0x0008`；以真实 AL Status 已进入 OP 且错误位清零判定恢复成功。
5. `IsReady()` 不再仅依赖初始化标志。
6. Retry 优先原地恢复，失败后才 Close + Init。

实施时仍须保证：

- IsReady 同时检查 OP 和错误位，读取失败不能继续当作就绪。
- 恢复期间的底层 PDO 收发不被普通业务的 OP 就绪检查挡住。
- AL Control 写入成功或读回请求值，不等于从站已经完成状态转换。
- 若 since_send 在每次 Send 成功后刷新，紧随其后的 Read 日志可能捕捉不到上一段长停顿；应另外在刷新前记录相邻发送间隔。

## 关于“跑久了溢出”的检查结果

现有证据不支持将本次故障认定为计数器溢出：

- 接收、PDO 输入及线程运行计数采用 32 位，不会在十几分钟内达到上限。
- 后续计数正常越过 65535，并已超过二百万次。
- 诊断毫秒差采用 32 位无符号差值，普通短间隔可以跨时间回绕计算。
- 日志队列是固定池循环使用，并非按运行时间无限追加。

但这不是对所有长期运行问题的排除证明；内存破坏、资源泄漏、主站调度停顿和链路异常仍应依据各自证据检查。

## 尚未定位的部分

**最初为什么出现长时间过程数据事件空档，尚未定责。**

从站的事件处理计数不能区分主站停止发送、传输链路中断，或从站一度没有及时处理事件。29.465 秒、58.688 秒的历史空档也可能包含故障之后的停机或重启等待，不能直接当作首次故障本身的持续时间。

后续若复现，应保留：

- 从站首次 `ecat local state` 和三条 `ecat stop/watchdog` 现场。
- 主站同一时段的首次 WKC 异常、AL Status/Code、相邻发送间隔。
- Recover 开始、错误确认、OP 请求、实际状态确认、失败转重连的时间记录。

当前可确认的是：曾发生硬件看门狗超时；事件恢复后未进入 OP 会持续阻止业务 RX；主站补充运行态监测与恢复后，用户观察到二十多分钟仍持续接收。最初断流根因保留为未解决调查项。
