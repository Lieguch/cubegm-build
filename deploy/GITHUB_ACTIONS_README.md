# GitHub Actions 一键构建指南（CubeGM / RK3036G 开源替代系统）

> 目标：**让 GitHub 的免费 Linux runner 替你把固件编出来**，你全程只需浏览器操作，本机零负担。
> 这是当前唯一"我（Agent）编排配方 + 云端自动执行 + 确定可行"的方式（沙箱无 make/gcc 跑不了，远程登录架构不可达）。

---

## 前提
- 一个免费 GitHub 账号（github.com 注册即可，无需付费）。
- 浏览器。本机不用装任何东西。

---

## 0. 一次性准备：建仓库并上传包内容

1. 登录 github.com → 右上 **+** → **New repository**。
   - 名称随意，如 `cubegm-build`。
   - 选 **Private**（推荐，避免源码/补丁公开）。
   - **不要**勾 "Add a README" 等初始化选项（保持空仓库）。
   - 点 **Create repository**。

2. 把交接包内容上传到仓库（两种方式任选）：

   **方式 A（网页拖拽，最简单）**：
   - 解压 `cubegm-handoff.zip` 到本地一个文件夹。
   - 在 GitHub 仓库页面点 **Add file → Upload files**，把解压出来的**所有文件/文件夹**拖进去（含 `.github/`、`deploy/`、`build/`、`docs/`、`patch/`、`source/`）。
   - **重点确认 `.github/workflows/build.yml` 已上传**（网页上传有时会跳过点开头的文件夹——若拖不进去，改用方式 B）。
   - 点 **Commit changes**。

   **方式 B（命令行，最稳）**：
   ```bash
   cd 解压后的 cubegm-handoff 目录
   git init
   git add -A
   git commit -m "cubegm build"
   git branch -M main
   git remote add origin https://github.com/<你的用户名>/cubegm-build.git
   git push -u origin main
   ```

> ⚠️ 网页上传可能漏掉 `.github/`（隐藏文件夹）。**务必确认 `.github/workflows/build.yml` 在仓库里**，否则不会触发构建。漏了就用命令行补推。

---

## 1. 自动触发构建

- 推送（push）完成后，GitHub 会**自动**触发 "Build CubeGM firmware" workflow。
- 也可手动触发：仓库页 **Actions** 标签 → 左侧选 "Build CubeGM firmware" → 右侧 **Run workflow** → 选 main 分支 → Run。
- 进入正在跑的 job，能实时看每一步日志（STAGE 0 apt → STAGE 1 crosstool 自举 ~30–90 分钟 → STAGE 2 SDL → STAGE 3 picoarch/cores）。

> 首次构建约 60–120 分钟（crosstool 自举最慢）。第二次起命中缓存，sysroot + SDL 段跳过，只需几分钟。

---

## 2. 下载产物

构建完成（绿勾）或失败（红叉）后：
- job 页面底部 **Artifacts** 区有两个文件可下载：
  - **`cubegm-device-payload`** —— 设备烧录包（`deploy/cubegm/`，成功才有）。解压后整体拷到 SD 卡根目录覆盖原 `cubegm/`。
  - **`build-report`** —— `BUILD_REPORT.txt`（**无论成功失败都有**），含 sysroot 的 glibc 天花板、产物 GLIBC 需求、各阶段状态、退出码。
- 点 artifact 名即下载 zip。

---

## 3. 失败时怎么办（把 report 发回给 Agent）

- 若红叉，下载 **`build-report`**，把 `BUILD_REPORT.txt` 内容整段发回给我。
- 我据此判断卡在哪：
  - **STAGE 1（sysroot）失败** → 多半 crosstool 拉源码超时，我给镜像源/重试方案。
  - **STAGE 2（SDL）失败** → 我给老版 armhf .deb 解包命令（glibc ≤ 2.17，须 Debian 7 wheezy / Ubuntu 12.04 precise，**不能用 jessie/trusty——它们 glibc 2.19 超 2.17**）。
  - **STAGE 3（picoarch/cores）失败** → 多半补丁应用或某 core Makefile，我针对性修。
- 修好后你重新 push（或 Run workflow），缓存会让重跑很快。

---

## 4. 烧录到设备

构建成功后，`cubegm-device-payload` 解压出的 `cubegm/` 整体拷到 SD 卡根目录，覆盖原 `cubegm/`。原厂 `rkgame/icube/driver.so/root.dat` 不动 → 设备不会报 "sdcard is damaged"。

---

## 5. 真机验收
- [ ] 设备经 `zhijack.sh` 进 FrogUI，不报 sdcard is damaged
- [ ] 插任意标准 USB 手柄即被 evdev 识别（全手柄支持）
- [ ] RTC 时间正确（策略游戏真实时钟，补丁 core.c `GET_SYSTEM_TIME`）
- [ ] 策略游戏退出后 `.srm` 电池存档不丢
- [ ] `verify_target_abi.sh` 对每个产物 = PASS（EM_ARM / 0x5000400 / ≤ GLIBC_2.17）

详见 `docs/qa_verification.md`；所有坑见 `HANDOFF.md` §5。
