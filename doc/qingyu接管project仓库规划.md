# qingyu 接管 Project_HPMicro5361 + 新仓库接管当前 project 规划

> 2026-08-14。**背景**：子树 `project` 与 HPM 项目耦合太深，qingyu 是 HPM 项目转移到用户区的版本。
> **目标**：qingyu **整个文件夹**继承 project 仓库（现名 `Project_HPMicro5361`）的**全部文件 + 全部提交**，
> 接管该仓库；新仓库 `Dust_Zephyr_Architecture_Project`（复用旧名）接管当前瘦身后的 project。
> **本轮只上传这两个仓库，不碰主仓库指针 / framework / HPM 树等其他内容。**

---

## 0. 目标与边界

### 目标

1. **qingyu 整个文件夹**（六模块 + `project/` + doc/scripts/temp）变成 git 仓库，继承
   `Project_HPMicro5361` 的全部提交历史（8 线程开发史不丢），并 push 到该仓库接管。
2. **新仓库 `Dust_Zephyr_Architecture_Project`** 接管当前（瘦身后的）project——只含 gpio+test
   验证工具，无 HPM 业务线程。

### 边界（本轮不做）

- **不动主仓库**（zephyr_user）的 project 子模块指针——瘦身提交推到新仓库后，主仓库指针更新是**后续**动作。
- **不动 framework 子模块 / HPM 树 / 其他仓库**。
- commit message **由修改定**（提交时按实际改动写，不预置）。

---

## 1. 现状盘点（证据，2026-08-14 实测）

| 项 | 现状 | 证据 |
| --- | --- | --- |
| 旧仓库 | `Dust_Zephyr_Architecture_Project` 已改名 `Project_HPMicro5361` | `git ls-remote` → main = `eeac0e6` |
| 当前 project 子模块 | `zephyr_user/project` remote 仍是旧名（GitHub 自动重定向），HEAD = `eeac0e6`（含 8 线程） | `git -C project remote -v` / `rev-parse HEAD` |
| project 工作区 | 瘦身**未提交**：删 can/chassis/gimbal/imu/pc/remote + hpm6e00evk；改 Kconfig/prj.conf/thread/CMakeLists/boards/gpio/test | `git -C project status` |
| qingyu 结构 | 根 = algorithm/cmd/doc/drivers/init/modules/project/scripts/temp/topic；`project/` 有 8 线程 + 用户区适配 CMakeLists + `.gitignore` | `ls` |
| qingyu git | **不是 git 仓库**（无 .git） | `ls .git` → 不存在 |
| 工具 | `git subtree` 可用；filter-repo / gh **不可用**（GitHub 侧改名/建仓走网页） | 实测 |

---

## 2. 仓库角色

| 仓库 | 地址 | 角色 |
| --- | --- | --- |
| `Project_HPMicro5361` | `git@github.com:qingyu0310/Project_HPMicro5361.git` | **qingyu 接管**：继承全部提交，push 上去 |
| `Dust_Zephyr_Architecture_Project` | `git@github.com:qingyu0310/Dust_Zephyr_Architecture_Project.git` | **新仓库接管当前 project**：瘦身版（gpio+test） |

---

## 3. 阶段 1：qingyu 继承 Project_HPMicro5361

**原理**：`git subtree add --prefix=project`（不带 `--squash`）把 project 仓库的全部提交作为
`project/` 子树导入 qingyu 根仓库，历史完整保留（merge 第二父）。

```bash
cd E:/Zephyr/projects/qingyu
# ① 备份当前 qingyu/project（用户区适配版，防 subtree 覆盖）
mv project /tmp/qingyu-project-backup

# ② qingyu 根建仓库，导入 Project_HPMicro5361 全部历史到 project/ 子目录
git init -b main
git remote add origin git@github.com:qingyu0310/Project_HPMicro5361.git
git fetch origin
git commit --allow-empty -m "init: qingyu 根（继承 Project_HPMicro5361 前置）"
git subtree add --prefix=project origin/main     # 不带 --squash = 保留全部提交

# ③ 恢复 qingyu 用户区文件（project 适配 + 六模块 + doc/scripts/temp）
rm -rf project
mv /tmp/qingyu-project-backup project
git add -A                                        # build/ 被 project/.gitignore 排除
git commit -m "qingyu: 用户区文件（六模块 + project 适配）"

# ④ 推送接管（⚠️ force push，见 §6）
git push --force origin main
```

**产出**：`E:\Zephyr\projects\qingyu\` 整个文件夹 = git 仓库，`git log` 含 project 全部提交。

---

## 4. 阶段 2：新仓库接管当前 project

**前置**：GitHub 新建空仓库 `Dust_Zephyr_Architecture_Project`（网页 New repository，不加 README/gitignore）。

```bash
cd e:/Zephyr/zephyr_user/project
# ① 当前 project 子模块 remote 切到新仓库
git remote set-url origin git@github.com:qingyu0310/Dust_Zephyr_Architecture_Project.git
# ② 提交瘦身（删 6 线程 + 改装配，只剩 gpio+test）
git add -A
git commit -m "<瘦身提交，按实际改动写>"
# ③ 推送（新空仓库，直推 main 无门禁）
git push -u origin main
```

**产出**：`Dust_Zephyr_Architecture_Project` = 瘦身 project（gpio+test），无 HPM 业务线程。

> 注：当前 project 子模块本地 git 含 eeac0e6 起的完整历史，切 remote 推送会把完整历史带进新仓库；
> 若新仓库要**纯净历史**（只含瘦身提交），改为 §6 风险里的孤儿分支/新 init 方案。

---

## 5. 最终效果

```
E:\Zephyr\projects\qingyu\            ← git 仓库，继承 Project_HPMicro5361 全部提交
    ├── algorithm/ cmd/ drivers/ init/ modules/ topic/    ← 用户层六模块
    ├── project/                        ← HPM 项目（8 线程 + 用户区适配）
    └── doc/ scripts/ temp/
    git log = project 全部提交历史 + init + qingyu 适配提交

Dust_Zephyr_Architecture_Project      ← 新仓库，当前 project（瘦身版）
    └── thread/ = gpio + test（验证工具），无 HPM 业务线程
```

- 业务线程的**提交历史**全部保留在 qingyu（Project_HPMicro5361）。
- 主干 project 脱钩为维护者验证区，后续主仓库指针更新另做。

---

## 6. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| **force push** | qingyu 的子树历史与 Project_HPMicro5361 现扁平历史不是同一条线，push 必须 `--force`。project 旧提交内容以子树形式保留在 qingyu 仓库，**不丢** | 接受 force push；qingyu 已含全部历史 |
| 当前 project 子模块仍指旧仓库 | remote 还是改名前的 URL（GitHub 自动重定向），Phase 2 未切新仓库前读的都是同一内容 | Phase 2 切 remote 后与 qingyu 脱钩 |
| 新仓库历史纯净性 | 切 remote 直推会把完整历史带进新仓库（§4 注） | 若需纯净历史：`git checkout --orphan` 或新 init + 拷贝瘦身文件 |
| 备份丢失 | `mv project /tmp/...` 若中断会丢 qingyu 适配 | 先确认 /tmp 可写；必要时 cp 而非 mv |
| GitHub 侧操作 | 改名/建仓 gh 不可用，需网页 | 用户网页操作，本地命令照 §3/§4 |

---

## 7. 执行清单（逐条勾）

- [ ] 阶段 1 ①：备份 `qingyu/project` → /tmp
- [ ] 阶段 1 ②：qingyu 根 `git init` + fetch + `subtree add --prefix=project origin/main`
- [ ] 阶段 1 ③：恢复 qingyu 文件 + commit
- [ ] 阶段 1 ④：`git push --force origin main`（接管 Project_HPMicro5361）
- [ ] 阶段 2 前置：GitHub 建空仓库 `Dust_Zephyr_Architecture_Project`
- [ ] 阶段 2 ①：当前 project 子模块 remote 切到新仓库
- [ ] 阶段 2 ②：提交瘦身（gpio+test）
- [ ] 阶段 2 ③：push 到新仓库
- [ ] 验证：`git -C qingyu log --oneline` 含 eeac0e6；`git -C zephyr_user/project log --oneline` 顶部是瘦身提交
- [ ] 后续（本轮不做）：主仓库更新 project 子模块指针 + PR

---

## 8. 边界提醒（本轮不动）

主仓库 project 指针、framework 子模块、HPM 树、其他任何仓库——**本轮一律不碰**。
