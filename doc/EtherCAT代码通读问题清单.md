# EtherCAT 代码通读问题清单

> **通读日期**：2026-10-31
> **通读范围**：`framework/drivers/communication/ethercat/`（31 文件）+ `drivers/ethercat/`（14 文件），共 45 个文件
> **代码状态**：整体 C++ 重构 30 步已执行完，**从未编译、从未上板**（见 `project-state-snapshots.md`）
> **本文性质**：静态阅读记录，不含任何代码改动。所有结论都基于工作区当前内容，未经编译器与硬件验证。

---

## 一、总览

| 编号 | 严重度 | 一句话 | 位置 |
|---|---|---|---|
| A | **阻塞** | SDO/SDO Info 应答入队的是栈上 `MailboxFrame`，返回即悬垂 | `core/ecat_device.cpp:317` |
| B | **阻塞** | Complete Access 遇到占位条目 `continue` 时跳过 `offset += length`，后续字段全部错位 | `core/model/ecat_dictionary.cpp:469`、`:593` |
| C | **阻塞** | 0x10F8 时间戳对象的 `access_hook` 从未绑定，读出来是 `device_type` 的字节 | `core/model/ecat_dictionary.cpp:53` |
| D | 中 | `GetDefaultEcatPort()` 只有声明没有定义（实际定义的是另一个名字） | `drivers/.../ecat_esc_port_hpm.hpp:122` |
| E | 中 | AL Event（0x0220）读即确认，却在 ISR 与线程两处都读、都跑 `HandleAlEvents`，无锁 | `core/ecat_device.cpp:119`、`:140` |
| F | 中 | ESC 非对齐读把地址向下对齐到 4 字节，但读数没有按差量偏移缓冲 | `drivers/.../ecat_esc_port_hpm.cpp:242` |
| G | 中 | 板载 EEPROM 模式下 `ReadWord` 恒返回 true 且不填数据，靠 ESC 配置约定兜底 | `drivers/.../ecat_sii_hpm.cpp:90` |
| H | 中低 | `EcatCoe::Continue`（SDO Info 分片续发）无任何调用者 | `core/mailbox/ecat_coe.cpp:99` |
| I | 低 | 排版不符合本项目规范（缩进、数组列对齐、行尾空格） | 多处 |
| J | 低 | `ecat_esc_port_hpm.cpp` 引入了 `jl1111/phy_jl1111.hpp` 但未使用 | `drivers/.../ecat_esc_port_hpm.cpp:19` |

**A/B/C 三条会让协议栈上板后直接不工作**；D~H 是功能缺口或潜伏问题；I/J 是整洁度。

---

## 二、逐条详述

### A. 邮箱应答入队的是栈指针（阻塞）

#### 现象

`EcatSdo::Reply` / `EcatSdoInfo` 的所有应答都通过 `ReplySink` 走到 `EcatDevice::SubmitReply`：

```cpp
// core/ecat_device.cpp:317
uint8_t EcatDevice::SubmitReply(void* context, uint8_t* data, uint16_t length)
{
	if (context == nullptr || data == nullptr) {
		return mailbox::kErrorInvalidSize;
	}

	auto& self = *static_cast<EcatDevice*>(context);

	MailboxFrame frame {};          // ← 栈上的局部变量

	frame.data = data;
	frame.length = length;
	frame.capacity = kMaxMailboxSize;

	const EcatMailbox::SubmitResult result = self.mailbox_.Submit(&frame);

	return (result == EcatMailbox::SubmitResult::Queued) ? 0 : mailbox::kErrorNoMoreMemory;
}
```

`EcatMailbox::Submit` 只做入队，存的正是这个指针：

```cpp
// core/mailbox/ecat_mailbox.cpp:260
EcatMailbox::SubmitResult EcatMailbox::Submit(MailboxFrame* frame)
{
	...
	if (!Enqueue(frame, true)) {      // ← 把 &frame（栈地址）写进队列
		return SubmitResult::NoMemory;
	}
	...
}

// core/mailbox/ecat_mailbox.cpp:533
bool EcatMailbox::Enqueue(MailboxFrame* frame, bool to_send)
{
	...
	queue[tail] = frame;              // ← 队列里存的是栈地址
	tail = next;
	return true;
}
```

`SubmitReply` 一返回，栈帧销毁，队列里 10 个槽位可能全是悬垂指针。下一拍 `Service()` 里：

```cpp
MailboxFrame* next = Dequeue(true);
if (next != nullptr) {
    if (Transmit(esc, next)) {        // ← next->length / next->data 已是垃圾
```

`Transmit` 读 `frame->length` 做长度校验、`frame->data` 做 `esc.Write`，全是野值。

#### 为什么错

LIFO 的栈地址被存进生命周期更长的队列。对比同一个类的 `Receive()`，那里对这个坑做了专门处理：

```cpp
// core/mailbox/ecat_mailbox.cpp:402
// 队列存的是 slots_ 里的稳定地址，不是栈上的 frame 副本
MailboxFrame* stable = nullptr;

for (uint8_t slot = 0; slot < kMailboxBufferCount; ++slot)
{
	if (slots_[slot].data == frame.data) {
		stable = &slots_[slot];
		break;
	}
}

if (stable == nullptr || !Enqueue(stable, false))
```

`Receive` 有、`Submit` 没有。`SubmitReply` 的注释里写的是"缓冲本来就在池里，所以这里只需把长度改好并交给邮箱队列"——缓冲确实在池里，但**描述符不在**。

#### 修复方向

在 `EcatMailbox::Submit` 内部做一次数据指针到稳定槽位的转换，让所有调用方都安全：

```cpp
EcatMailbox::SubmitResult EcatMailbox::Submit(MailboxFrame* frame)
{
	if (frame == nullptr || frame->data == nullptr) {
		return SubmitResult::NoMemory;
	}

	// 把调用方传进来的描述符落到池内的稳定槽位上
	MailboxFrame* stable = nullptr;

	for (uint8_t slot = 0; slot < kMailboxBufferCount; ++slot)
	{
		if (slots_[slot].data == frame->data) {
			stable = &slots_[slot];
			break;
		}
	}

	if (stable == nullptr) {
		return SubmitResult::NoMemory;
	}
	stable->length = frame->length;

	// 以下原逻辑改用 stable
	...
}
```

这样 `Receive` 里那段手写转换也可以删掉。**取舍**：`Submit` 需要额外做一次 18 次指针比较，属可接受开销；换来的是一次性消灭整类悬垂。

---

### B. Complete Access 读写跳过占位条目时不累加偏移（阻塞）

#### 现象

`EcatDictionary::Read` 的 CA 路径：

```cpp
// core/model/ecat_dictionary.cpp:465
// Complete Access：从子索引 0 一直读到最大子索引
uint32_t offset = 0;
const uint16_t max_sub = SubCountOf(*slot);

for (uint8_t i = 0; i <= max_sub; ++i) {
	const EntryDescription* entry = EntryDescriptionOf(*slot, i);

	if (entry == nullptr || entry->access_flags == 0) {
		continue;                     // ← 直接跳过，offset 不变
	}

	const uint32_t length = ByteWidthOf(entry->bit_length);

	if (offset + length > capacity) {
		return sdo::kAbortParamLengthTooLong;
	}

	const void* source = DataOf(*slot, i);
	...
	offset += length;                 // ← 只有非占位条目才累加
}
```

`Write` 的 CA 路径（`:593`）结构相同，`continue` 与 `offset += length` 的相对位置也一样。

而同一个类的 `TotalLengthOf` 不判权限，只要 `entry != nullptr` 就累加：

```cpp
// core/model/ecat_dictionary.cpp:266
uint32_t EcatDictionary::TotalLengthOf(const ObjectSlot& slot) const
{
	if (slot.code == ObjectCode::Variable) {
		return LengthOf(slot, 0);
	}

	// 子索引 0 占 16 位，其余按各条目位长度累加（不含填充）
	uint32_t bits = 16;

	for (uint8_t i = 1; i <= slot.sub_count; ++i) {
		const EntryDescription* entry = EntryDescriptionOf(slot, i);

		if (entry != nullptr) {
			bits += entry->bit_length;   // ← 占位条目照样加
		}
	}
	return (bits + 7U) / 8U;
}
```

#### 为什么错

0x1C32 / 0x1C33 的描述表里有 19 个占位项（子索引 3、7、14~18、19~31），每个 `bit_length = 0x20`、`access_flags = 0`：

```cpp
// core/spec/ecat_application_objects.hpp:191
{ DataType::Unsigned8,  0x20, 0 },   // 3  偏移时间（不支持）
...
{ DataType::Unsigned8,  0x08, 0 },   // 19 不存在
```

`ObjectData::SyncParameters` 里这些字段是真实存在的 `uint32_t` 成员：

```cpp
// core/spec/ecat_application_objects.hpp:443
uint32_t sub003_reserved = 0;       // 子索引 3（占位）
```

也就是说**内存布局里有这些字段，描述表也标了它们的宽度**，只有权限是 0。CA 读时跳过它们却不跳过内存，从子索引 4 起每个字段都往前错位 4 字节。

`TotalLengthOf` 算出来 78 字节，`Read` 的 CA 路径只写出 18 字节左右 —— 两边不一致，主站拿到的 CA 数据整体错位。

#### 修复方向

用描述表里的 `bit_length` 维护游标，与 `TotalLengthOf` 保持同一套算法：

```cpp
for (uint8_t i = 0; i <= max_sub; ++i) {
	const EntryDescription* entry = EntryDescriptionOf(*slot, i);

	if (entry == nullptr) {
		continue;
	}

	const uint32_t length = ByteWidthOf(entry->bit_length);

	// 占位条目：没有数据可写，但它在内存里占位，游标必须前进
	if (entry->access_flags == 0) {
		offset += length;
		continue;
	}

	if (offset + length > capacity) {
		return sdo::kAbortParamLengthTooLong;
	}
	...
	offset += length;
}
```

`Write` 的 CA 路径同样处理。改完之后建议用 `TotalLengthOf` 做一次断言：CA 读完的 `written` 必须等于 `TotalLengthOf`。

---

### C. 0x10F8 时间戳对象的访问前门从未绑定（阻塞）

#### 现象

对象表里 0x10F8 的行：

```cpp
// core/model/ecat_dictionary.cpp:52
// 0x10F8 由前门算值，前门在绑定时填
{index::kTimestamp,       ObjectCode::Variable,  0, schema::kEntry0x10F8, schema::kName0x10F8,
  0, DataKind::Member, nullptr, nullptr, nullptr },
```

`data_offset = 0`、`kind = DataKind::Member`、`access_hook = nullptr`。

整个仓库里 `EcatDevice::Init` 只做了这些绑定：

```cpp
// core/ecat_device.cpp:56
sync_.BindParameters(dictionary_.Data().sync_manager_out, dictionary_.Data().sync_manager_in);
sync_.ResetParameters();
state_machine_.Bind(mailbox_, process_data_, sync_, sii_service_);
state_machine_.BindDictionary(dictionary_);
dictionary_.BindStateQuery(QueryState, this);
sdo_.Bind(dictionary_, SubmitReply, this);
sdo_info_.Bind(dictionary_, SubmitReply, this);
coe_.Bind(sdo_, sdo_info_);
mailbox_.SetDispatcher(DispatchMailbox, this);
sii_service_.Bind(esc, sii);
```

没有任何一处给 0x10F8 设 `access_hook`。

另外 `EcatSync::UpdateTimestamp`（`core/al/ecat_sync.cpp:263`）定义了 DC 时间戳的读取与 64/32 位处理，但**全仓库没有调用点**。

#### 为什么错

`DataOf` 对 `DataKind::Member` 直接算 `&data_ + data_offset + ByteOffsetOfEntry(...)`，`data_offset = 0` 且对象代码是 `Variable`（`ByteOffsetOfEntry` 对 Variable 恒返回 0），所以返回的是 `ObjectData` 的首地址，也就是 `device_type`。

0x10F8 的描述是 `Unsigned64`，`LengthOf` 返回 8 字节，`TotalLengthOf` 也是 8。主站做 SDO 上传 0x10F8 会拿到 `device_type(0x1389)` + `error_register` + 对齐填充（8 字节），不是时间戳。

#### 为什么不好修

`kSlots` 是 `constexpr`，落在 `.rodata`，运行期不能写 `access_hook`。要让前门可绑定，得二选一：

1. **把 `kSlots` 改成非 const 的静态数组**：19 项 × `sizeof(ObjectSlot)`（约 36 B）≈ 700 B 从 Flash 挪到 RAM。与"对象表 0 B RAM"的内存红线冲突。
2. **给 0x10F8 单开一条旁路**：在 `EcatDictionary` 里加一个 `TimestampProvider` 回调成员 + 访问前门在 `Read` 里按索引特判。多一个特例分支，但保住 0 B RAM。

选 2 更符合本项目的内存红线。取舍需要拍板。

---

### D. `GetDefaultEcatPort()` 是死声明（中）

`drivers/ethercat/hal/hpm/ecat_esc_port_hpm.hpp:122`：

```cpp
namespace ecat {
...
/**
 * @brief 取默认 ESC 端口实例
 */
EcatEsc& GetDefaultEcatPort();
}
```

实际定义在 `ecat_esc_port_hpm.cpp:349`：

```cpp
/**
 * @brief 取默认 ESC 端口实例
 *
 * 声明在 `interface/ecat_esc.hpp` 的全局作用域，这里必须保持同样的作用域。
 */
EcatEsc& GetDefaultEcatEsc()
{
	static ecat::EcatEscPortHpm instance;

	return instance;
}
```

名字不同（`Port` vs 无），作用域也不同（`ecat::` vs 全局）。

#### 影响

- `EthercatSlave::Init` 调的是 `::GetDefaultEcatEsc()`（`ethercat.cpp:30`），与定义匹配，**功能上能链接**。
- 头里那条 `ecat::GetDefaultEcatPort()` 永远没人调，也没有定义；如果将来有人按头文件声明去调，链接期才会炸。

#### 修复方向

删掉 `ecat_esc_port_hpm.hpp` 里那条声明。注释已经明确"声明在 `interface/ecat_esc.hpp` 的全局作用域"，头文件不该再留一条。

---

### E. AL Event 在 ISR 与线程两处都读、都处理，无锁（中）

#### 现象

```cpp
// core/ecat_device.cpp:112
bool EcatDevice::Poll()
{
	if (!ready_) {
		return false;
	}

	mailbox_.Service(*esc_);
	HandleAlEvents(esc_->ReadAlEvent());     // ← 线程侧读 0x0220
	state_machine_.CheckErrors(*esc_);
	...
}

// core/ecat_device.cpp:134
void EcatDevice::OnPdi()
{
	if (!ready_) {
		return;
	}

	HandleAlEvents(esc_->ReadAlEvent());     // ← ISR 侧也读同一个寄存器
}
```

`EcatEsc::ReadAlEvent` 的契约写得很明确：

```cpp
// interface/ecat_esc.hpp:96
/**
 * @brief 读 AL Event Request（0x0220）
 *
 * **读取即确认**：读一次就把事件位清掉。调用方必须一次读完，
 * 不许"先探一下有没有、过一会儿再处理"。
 */
```

#### 为什么错

三层问题叠在一起：

1. **事件被抢**：ISR 读完清了位，线程再读就是 0；反之线程先读，ISR 就收不到。两边都会漏事件。
2. **同一套代码在两条上下文并发跑**：`HandleAlEvents` 会调 `state_machine_.HandleControl`（改状态机成员）、`process_data_.ConsumeOutputs`（调业务回调）、`mailbox_` 的标志位。这些成员没有任何 `irq_lock` / `atomic` 保护。
3. **`Poll()` 里先 `mailbox_.Service()` 再读事件**，而 `Service` 里会读 SM 寄存器状态、改队列 —— 与 ISR 路径对同一批成员交叉写。

#### 修复方向

不是删掉其中一处，而是明确所有权。两条路：

- **ISR 独占读取**：`OnPdi` 读一次，把事件位存进一个 `volatile uint16_t pending_events_`；`Poll()` 只消费 `pending_events_`（用 `irq_lock` 交换清零），不再读寄存器。
- **线程独占读取**：`OnPdi` 只置一个"有事"标志，实际 `ReadAlEvent` 与分发全在 `Poll()` 里做。代价是 PDO 延迟从 ISR 变成一拍线程，违背"1 kHz 控制环帧到即算"的设计（`ethercat.hpp` 的注释明确否决过）。

**推荐第一条**：PDO 保持在 ISR，AL Control / EEPROM 命令这些线程级事件通过待处理位转交。三个事件类（PDO 输出、PDO 输入、SYNC）留在 ISR 处理，其余转交。

---

### F. 非对齐地址读没有补偿（中，潜伏）

```cpp
// drivers/ethercat/hal/hpm/ecat_esc_port_hpm.cpp:232
void EcatEscPortHpm::Read(uint16_t address, void* data, uint16_t length)
{
#if defined(HPM_IP_FEATURE_ESC_BYTE_READ) && HPM_IP_FEATURE_ESC_BYTE_READ
	hpm_esc_read(dev_, address, data, length);
#else
	hpm_esc_read_block(dev_, static_cast<uint32_t>(address) & ~3U, data, length);
#endif
}
```

地址被向下对齐到 4 字节，但读到的字节原样放进 `data`，没有按 `address & 3` 做偏移。

#### 现状

通读所有调用点，传进来的地址都是 4 对齐的（`0x0120` / `0x0130` / `0x0800+8n` / `0x0500` / `0x0910` / DPRAM 起始等）。所以**当前不会触发**。

#### 风险

`EcatEsc::Read` 的契约是"读 address 起的 length 字节"，头文件里没有写"地址必须 4 对齐"。任何后续在 `ecat_sm.cpp` / `ecat_state_machine.cpp` 这类文件里加一个非对齐读取（例如读 `0x0134` 之后再读一个字节偏移），就会静默拿到错位数据。

#### 修复方向

两个选择：

1. **契约写明**：在 `EcatEsc::Read` 的 Doxygen 里加"address 必须 4 字节对齐"，并在 `EcatEscPortHpm::Read` 里加断言或 `__ASSERT`。
2. **静默补偿**：读进一个临时 4 对齐缓冲，再 `memcpy` 到 `data` 的偏移处。多一次拷贝，但契约变宽。

头文件注释里已经承认"地址被悄悄向下取整这件事在迁移时最容易让人以为读到了错的数据"——注释知道坑，代码没防。

---

### G. 板载 EEPROM 模式下 `ReadWord` 恒返回 true 且不填数据（中）

```cpp
// drivers/ethercat/hal/hpm/ecat_sii_hpm.cpp:68
bool EcatSiiHpm::ReadWord(uint32_t word_address, uint8_t* data, uint16_t length)
{
	if (!ready_ || data == nullptr) {
		return false;
	}

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	for (...) { ... }
	return true;
#else
	/*
	 * 板载 EEPROM：ESC 自己走 I2C 主控，应用层在运行期不接管。
	 * 这里的"成功"表示"不需要应用参与"，不是"读到了数据"。
	 * ...
	 */
	(void)word_address;
	(void)length;
	return true;
#endif
}
```

#### 为什么是问题

`EcatSiiService::HandleRead` 不区分模式：

```cpp
// core/mailbox/ecat_sii_service.cpp:142
void EcatSiiService::HandleRead(uint32_t address)
{
	uint8_t bytes[8] = {};

	if (!sii_->ReadWord(address, bytes, static_cast<uint16_t>(read_width_))) {
		PublishStatus(eeprom_config::kErrorCmdAck);
		return;
	}
	esc_->Write(reg::kEepromData, bytes, read_width_);   // ← 把未初始化的 0 写进数据窗口
}
```

板载模式下 `ReadWord` 返回 true 且 `bytes` 全 0，于是 `0` 被写进 ESC 的 SII 数据窗口，主站读到全 0 的 SII 内容。

#### 为什么当前不炸

`EcatEscPortHpm::HardwareInit` 里板载模式调的是 `hpm_esc_configure_eeprom(dev_, false, ...)`（`ecat_esc_port_hpm.cpp:169`），第二个参数 `false` 表示不让 PDI 接管 EEPROM 访问。ESC 自己走 I2C，PDI 侧不会收到 0x0020（EEPROM 命令）事件，`PollCommand` 不会被调用。

**这是一条靠 ESC 配置维持的隐式契约，代码里没有防护**。如果哪天把 `assign_to_pdi` 改成 true（比如为了在 Flash 模拟模式之外也接管 SII），这条路径立刻返回错误数据。

#### 修复方向

在 `EcatSiiService::HandleRead` 开头按模式分流：

```cpp
if (sii_->GetMode() == EcatSii::Mode::OnBoard) {
	// 板载 EEPROM 由 ESC 主控读写，PDI 侧不该收到这个命令
	PublishStatus(eeprom_config::kErrorCmdAck);
	return;
}
```

`GetMode()` 已经存在（`ecat_sii.hpp:47`），只是从没被调用过。

---

### H. `EcatCoe::Continue` 无调用者（中低）

```cpp
// core/mailbox/ecat_coe.cpp:99
uint8_t EcatCoe::Continue(MailboxFrame* frame)
{
	if (sdo_info_ == nullptr || !sdo_info_->HasPendingFragment()) {
		return 1;
	}
	return sdo_info_->Continue(frame);
}
```

`EcatDevice` 里没有任何位置调它。SSC 里对应的 `COE_ContinueInd` 由邮箱层的 `MBX_ContinueInd` 在 SM1 空闲时驱动；本实现的 `EcatMailbox::Service` 没有这个钩子。

#### 当前影响

零。`CollectIndices` 最多枚举 19 个对象，2 字节一项 = 38 字节，远小于单片容量（`0x80 - 6 - 2 - 4 = 116` 字节），`fragments_left_` 恒为 0。

#### 后续风险

一旦对象表扩到 60 项以上（PDO 映射表或厂商对象），SDO Info 列表需要分片，而分片永远发不出去 —— 主站会卡在等后续片。

#### 修复方向

二选一：

1. **接上**：在 `EcatMailbox::Service` 的发送推进处（`:237` 附近，`!send_full_ && read_frame_ == nullptr` 分支）加一次 Continue 回调。需要给 `EcatMailbox` 加一个 `ContinueSource` 回调，与 `Dispatcher` 同一层。
2. **删掉**：如果确认对象表不会超过一片，把 `EcatCoe::Continue` 与 `EcatSdoInfo::Continue` / `HasPendingFragment` / `fragments_left_` 全删，避免留下"看起来支持、实际不工作"的路径。

---

### I. 排版问题（低）

按本项目注释与排版规范（`comment-conventions.md`），以下几处不合格：

1. **for 循环体没缩进** —— `drivers/ethercat/hal/hpm/ecat_esc_port_hpm.cpp:119`

   ```cpp
   for (uintptr_t irq = 0; irq < HPM_ESC_IRQ_COUNT; ++irq) {
   hpm_esc_set_irq_callback(dev_, static_cast<enum hpm_esc_irq>(irq), OnIrq, reinterpret_cast<void*>(irq & kIrqMask));
   }
   ```

2. **数组元素行首多半格/少半格** —— `core/spec/ecat_application_objects.hpp`

   - `:84` ` { DataType::Unsigned32, 0x20, access::kRead }, };`（行首一个空格）
   - `:295`、`:322` 最后三项挤在同一行
   - `:331`、`:342` ` 	{ DataType::Unsigned16, ...`（混合缩进）

3. **行尾空格** —— `core/model/ecat_dictionary.cpp:30-34`、`:47-50`

4. **头文件注释里的表格**（`interface/ecat_esc.hpp:11-19`）属于"文件头写长篇论述"的变体，规范里要求文件头只留 `@file/@author/@brief/@version/@date/@copyright`。取舍：这张表是"新旧接口为什么这么拆"的唯一记录，删了会丢信息 —— 建议搬到本文档或 `.cpp` 里，文件头只留一句。

---

### J. 未使用的 include（低）

`drivers/ethercat/hal/hpm/ecat_esc_port_hpm.cpp:19`：

```cpp
#include "jl1111/phy_jl1111.hpp"
```

PHY 实例实际是从 `DEVICE_DT_GET(DT_NODELABEL(ecat_phy0)).data` 取（`:148`），不需要具体型号的头。删掉即可。

---

## 三、修复顺序建议

依赖关系决定顺序：

| 顺序 | 条目 | 理由 |
|---|---|---|
| 1 | **A** | 不改这一条，任何 SDO 应答都发不出去，后面所有验证都做不了 |
| 2 | **B** | 主站启动时会读 0x1C32/0x1C33，CA 错位直接卡状态机 |
| 3 | **C** | 主站对 0x10F8 的读在 PREOP 之后就会发生；需要先拍板"旁路 vs 迁 RAM" |
| 4 | **D、J** | 一句话改动，顺手 |
| 5 | **E** | 需要设计决策（ISR 独占还是线程独占），影响面最大，单独做 |
| 6 | **F、G、H** | 当前不触发，属于加固；可以等首轮上板验证后再动 |
| 7 | **I** | 纯格式，最后统一处理 |

**A/B 是"不修就必然失败"的级别**；C 取决于主站是否读 0x10F8（TwinCAT 与 IgH 的启动序列都会读）；E 是并发正确性，在单核 RV32 上可能靠运气跑对，但不可依赖。

---

## 四、通读确认事项

以下内容读出后确认**没有问题**，供后续 review 时不再重复检查：

- `EcatSdoInfo::HandleList` 的分片数公式 `(count - per_fragment + per_fragment - 1) / per_fragment` 化简后等于 `(count - 1) / per_fragment` = `ceil(count / per) - 1`，数学正确。
- `EcatMailbox::Submit` 的计数器回绕 `(read_counter_ & kCounterMax) == 0 → 归 1`，配合 `<< 4` 后 `& 0xF0`，取值恒在 1~7，正确。
- `EcatSdo::BuildDownload` 里 `length - kSdoOffset - 8` 不会下溢 —— 前置检查 `length < kInitiateNormalLength` 已拦住。
- `EcatSdo::HandleUploadSegment` 的 `segment_total_size_ - segment_handled_` 不会下溢 —— `last_segment` 时 `Abort()` 已清状态。
- `EcatEscPortHpm::CollectPhys` 用 `DT_NODE_HAS_STATUS` 而非 `DT_NODE_EXISTS` 是安全的 —— 前者内部先判存在性。
- PHY 驱动用 `DEVICE_DT_INST_DEFINE` 的第 4 参（`data`）传实例指针，`CollectPhys` 从 `.data` 取回，类型一致。
- `ecat_phy_mdio.cpp` 里 `ecat_port_log` 的定义继承了 `ecat_log.hpp` 的 `extern "C"` 声明，链接属性正确。
- 三层分层边界干净：`core/` 与 `interface/` 无厂商符号，`drivers/` 无协议符号（对应记忆里"静态验收五项全过"）。
