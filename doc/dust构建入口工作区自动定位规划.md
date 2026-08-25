# dust 构建入口工作区自动定位规划

> 日期：2026-08-25
> 目标：把 `dust build` 从“必须先手动 `cd` 到项目 `project/` 目录”改成“可在工作区根直接执行”，同时**不能把所有工作区都硬编码回 `E:\Zephyr\zephyr_user\project`**。

## 1. 现状与问题

当前入口链路是：

```text
dust build
-> framework\cmd\build\dust.cmd
-> framework\cmd\build\build.bat
```

当前三个脚本的行为不一致：

1. `framework/cmd/build/dust.cmd`
   只做子命令分发，不处理工作目录。

2. `framework/cmd/build/build.bat`
   只接受“当前目录本身就是项目根，且包含 `CMakeLists.txt`”。
   否则直接报错退出。

3. `framework/cmd/build/build.ps1`
   已经做了自动跳转，但策略是：
   - 如果当前目录是 `E:\Zephyr\zephyr_user`，跳到 `E:\Zephyr\zephyr_user\project`
   - 如果当前目录本身有 `CMakeLists.txt`，留在当前目录
   - 否则仍然跳到 `E:\Zephyr\zephyr_user\project`

这会带来两个问题：

1. `bat` 和 `ps1` 行为不一致。
2. `ps1` 的默认目标写死成维护者工作区 `zephyr_user\project`，会吞掉用户工作区语义。

也就是说，真正的问题不是“要不要自动进入 `project/`”，而是：

**`dust build` 缺少“我当前在哪个工作区层级”的判断逻辑。**

## 2. 这次规划要守住的边界

### 2.1 要支持的场景

`dust build` 至少要支持下面三种启动位置：

1. 维护者工作区根  
   例：`E:\Zephyr\zephyr_user`
   期望：自动进入 `E:\Zephyr\zephyr_user\project`

2. 任意工作区项目根  
   例：`E:\Zephyr\zephyr_user\project`
   例：`E:\Zephyr\projects\qingyu\project`
   期望：直接在当前目录构建

3. 任意工作区根  
   例：`E:\Zephyr\projects\qingyu`
   例：`E:\Zephyr\projects\detected`
   期望：自动进入该工作区自己的 `project\`

### 2.2 不该做的事

下面这些策略都不应该采用：

1. 只要不是项目根，就一律跳到 `E:\Zephyr\zephyr_user\project`
2. 通过路径字符串硬编码“只有 `zephyr_user` 能自动跳转”
3. 在 `dust.cmd` 里把所有路径逻辑写死，导致 `bat` 和 `ps1` 再次分叉

## 3. 推荐的定位语义

推荐把“目标构建目录解析”抽象成统一规则：

### 规则 A：当前目录本身是项目根

判定条件：

```text
<cwd>\CMakeLists.txt 存在
```

行为：

```text
target_dir = <cwd>
```

### 规则 B：当前目录是工作区根，且其下存在 project

判定条件：

```text
<cwd>\project\CMakeLists.txt 存在
```

行为：

```text
target_dir = <cwd>\project
```

这条规则同时覆盖：

1. `E:\Zephyr\zephyr_user`
2. `E:\Zephyr\projects\<user>`

所以它才是我们真正想要的“自动进入 project”，而不是“自动进入 zephyr_user\project”。

### 规则 C：以上都不满足

行为：

报错，并把探测到的路径打印清楚，例如：

```text
[ERROR] dust build cannot locate a project root from:
        current dir: <cwd>
        expected one of:
        - <cwd>\CMakeLists.txt
        - <cwd>\project\CMakeLists.txt
```

## 4. 脚本层的改动范围

### 4.1 `framework/cmd/build/build.bat`

这是 `dust build` 当前真实入口，必须先修这里。

改动目标：

1. 加入 `TARGET_DIR` 解析逻辑
2. 若当前目录是工作区根，则 `cd /d` 到 `TARGET_DIR`
3. 后续 `boards\*` 扫描和 `west build` 都基于 `TARGET_DIR`
4. 报错信息从“你没进 project”改成“我没找到项目根”

### 4.2 `framework/cmd/build/build.ps1`

要和 `build.bat` 统一成同一语义，不能继续保留：

```powershell
else { Set-Location "E:\Zephyr\zephyr_user\project" }
```

改动目标：

1. 删除对 `E:\Zephyr\zephyr_user\project` 的硬编码兜底
2. 与 `build.bat` 使用同一套解析优先级
3. 错误提示和成功路径提示尽量一致

### 4.3 `framework/cmd/build/dust.cmd`

原则上只做分发，不建议把工作区判断逻辑挪到这里。

原因：

1. `dust.cmd` 只是入口壳
2. 真正构建语义应该落在 `build.bat`
3. 否则将来 `dust build` 与直接调 `build.bat` 的行为又会分叉

## 5. 推荐的实现阶段

### 阶段 1：统一“项目根解析”语义

文件：

1. `framework/cmd/build/build.bat`
2. `framework/cmd/build/build.ps1`

目标：

把两者统一成下面顺序：

1. 若 `<cwd>\CMakeLists.txt` 存在，使用 `<cwd>`
2. 否则若 `<cwd>\project\CMakeLists.txt` 存在，使用 `<cwd>\project`
3. 否则报错

产出：

`dust build` 可以直接在工作区根运行，不再要求手动 `cd project`。

### 阶段 2：明确输出路径和提示

文件：

1. `framework/cmd/build/build.bat`
2. `framework/cmd/build/build.ps1`

目标：

在真正执行 `west build` 前打印一行当前解析结果，例如：

```text
[dust] project root: E:\Zephyr\zephyr_user\project
```

好处：

1. 用户一眼能看出脚本选中了哪个工作区
2. 出现误判时，第一时间能定位

### 阶段 3：同步文档

文件：

1. `framework/cmd/README.md`
2. `doc/zephyr_子树架构搭建指南.md`

目标：

把旧说法：

```text
build.bat 要求当前目录是项目根
```

改成新说法：

```text
dust build 支持在工作区根或 project 根执行；
若在工作区根执行，会自动解析到该工作区自己的 project 目录。
```

## 6. 关键细节

### 6.1 为什么优先判当前目录的 `CMakeLists.txt`

因为用户可能已经在：

```text
E:\Zephyr\projects\qingyu\project
```

这时如果还强行往下拼 `project\`，就会错到：

```text
E:\Zephyr\projects\qingyu\project\project
```

所以必须先判“当前目录本身是不是项目根”。

### 6.2 为什么不用只识别 `zephyr_user`

因为这会把“自动进入 project”做成维护者专属特判，用户工作区完全吃不到同样能力。

我们真正需要识别的不是某个固定路径，而是：

```text
当前目录是不是一个工作区根
```

而“工作区根”的最直接证据就是：

```text
其下存在 project\CMakeLists.txt
```

### 6.3 `build\` 产物应该落在哪

应继续落在**最终解析到的项目根**下：

1. `E:\Zephyr\zephyr_user\project\build`
2. `E:\Zephyr\projects\qingyu\project\build`

不要落回调用命令时的工作区根。

## 7. 建议的静态验证

实施后至少检查这几种路径：

1. 在 `E:\Zephyr\zephyr_user` 执行 `dust build`
   期望：解析到 `E:\Zephyr\zephyr_user\project`

2. 在 `E:\Zephyr\zephyr_user\project` 执行 `dust build`
   期望：留在当前目录

3. 在 `E:\Zephyr\projects\qingyu` 执行 `dust build`
   期望：解析到 `E:\Zephyr\projects\qingyu\project`

4. 在 `E:\Zephyr\projects\qingyu\project` 执行 `dust build`
   期望：留在当前目录

5. 在一个既没有 `CMakeLists.txt`、也没有 `project\CMakeLists.txt` 的目录执行
   期望：明确报错，不误跳到 `zephyr_user\project`

## 8. 结论

这次该改的不是“从根目录自动进 `project`”这一条特判，而是：

**把 `dust build` 的入口语义改成“先解析当前工作区，再进入该工作区自己的项目根”。**

这样才能同时满足：

1. 在 `E:\Zephyr\zephyr_user` 直接 `dust build`
2. 在用户工作区根直接 `dust build`
3. 不把所有人都偷偷送回维护者 `project`

