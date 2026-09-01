# SRAM 存档持久化 —— `libz.so.1` 方案（免改 rootfs）

## 结论（一句话）

**唯一可用方案 = 在 `<cubegm>/lib/` 放一份伪装成 zlib 的 shim。**
原厂 `icube` / `rkgame` / `driver.so` / cores 全部零改动，也不改任何 `.sh`。
部署 = 一次 `cp`，卸载 = 一次 `rm`。

所有二进制打补丁的产物（`patched_rkgame` / `sram_patch.*` / `sram_hook_v2.*` /
`sram_hook_v3.S` / `sram_hook_combined.*`）**全部废弃**，原因见文末「废弃清单」。

---

## 根因（反编译实证，非推测）

| # | 证据 | 来源 |
|---|------|------|
| 1 | rkgame 全文 `retro_get_memory_data` **0 处命中** | `decompiled/01-static/*` 全量检索 |
| 2 | rkgame 全文 `save_game` / `save_game_*` **0 处命中** | 同上 |
| 3 | `environment(cmd=0x1f=31)` 实现了 GET_SAVE_DIRECTORY，写入全局 `save_directory`@0x3cf788 | `FUN_002b4f98_environment.c` |
| 4 | `Core_Load`@0x2b6f58 与 `run_process`@0x2b59bc 所有 core 符号都经 `dlsym(handle,name)` 获取 | `FUN_002b6f58_Core_Load.c` / `FUN_002b59bc_run_process.c` |
| 5 | core 侧就绪：28/28 个 `libemu_*.so` 均导出 `retro_get_memory_data`/`retro_get_memory_size` | 符号表实证 |
| 6 | 标准 ABI：`retro_get_memory_data(unsigned id)` **返回指针**；`RETRO_MEMORY_SAVE_RAM = 0` | libretro-common `include/libretro.h`（联网核实） |

**结论**：缺口 100% 在前端。core 的 SRAM 永远不被取回写盘。
`dlsym` 是唯一干净的注入点 —— 不需要知道 rkgame 的任何内部地址。

---

## 注入点为何选 `libz.so.1`（不是 LD_PRELOAD）

### 上一版方案（LD_PRELOAD + 改 `icube_start.sh`）为何无效 —— 实测证明

```
inittab  : ::sysinit:/etc/init.d/rcS
rcS      : for i in /etc/init.d/S??* ; do $i start
S80icube : DAEMON=/sdcard/cubegm/$NAME
           $DAEMON start         ← 直接 exec icube 二进制，不经过任何 .sh
```

SD 卡上 `icube_start.sh` / `icube.sh` **开机时无人调用**。改它们对启动链无影响。
此外挂载点是 `/sdcard` 而非 `/mnt/sdcard`（icube 内部字符串写 `/mnt/sdcard/cubegm/...`
只是它自己拼运行时路径，不是挂载点）。

### 真正可依赖的注入点：`LD_LIBRARY_PATH`

`S80icube`、`/etc/profile`、`S50usbdevice:pre_run_binary` 三处都执行
`export LD_LIBRARY_PATH=/sdcard/cubegm/lib:$LD_LIBRARY_PATH` —— 这是原厂写死的，
`icube` 直接 exec 时也继承下来。而 `rkgame` 的 `DT_NEEDED` 恰好包含
**`libz.so.1`**，是 28 个 core 中唯一依赖 libz 的对象链上的关键点。

### 抢占而不「吃掉」系统 zlib：SONAME 技巧

关键矛盾：劫持 `libz.so.1` 这个名字后，我们的 shim 内部还需要调真实的
`compress` / `uncompress`（SRAM 数据压缩用）。

**解法**：文件名叫 `libz.so.1`，但 ELF 里 `DT_SONAME = libsramzlib.so`（≠ `libz.so.1`）。
这样 ld.so 把我们的对象注册为 `libsramzlib.so`，名字槽位 `libz.so.1` **保持空闲**。
构造函数里就能 `dlopen("/usr/lib/libz.so.1", RTLD_LAZY|RTLD_GLOBAL)` 拿到系统 zlib
的真身（`/usr/lib/libz.so.1.2.11`），然后把 `compress` / `uncompress` 缓存成函数指针
`real_compress` / `real_uncompress`，本 shim 导出的同名函数转发到这两个指针。

```
rkgame  →  dlsym(handle, "compress")  →  shim 导出 compress()
                                        →  real_compress(...)     ← 系统 zlib
                                        →  顺带挂 SRAM 钩子
```

---

## 实现原理

```
retro_set_environment  ←（未包装，核心自行协商 save 目录）
retro_load_game        → 成功且 ret!=0 后：解析 .srm 路径，读盘回填 SRAM
retro_run              → 透传；每 256 帧查一次时间，到间隔就落盘
retro_unload_game      → 先落盘再调原函数（顺序关键：卸载后 SRAM 已被释放）
```

SRAM 接口由 shim 自己从 `handle` 取（rkgame 从不取）：
`retro_get_memory_data(0)` 取缓冲区指针，`retro_get_memory_size(0)` 取大小。

### 存档路径规则

```
<rkgame 所在目录>/saves/<ROM 父目录名>/<文件名去扩展>.srm
例：/sdcard/cubegm/rkgame + /sdcard/003/langrisser.zip
    → /sdcard/cubegm/saves/003/langrisser.srm
```

用 ROM 的父目录（`000`..`008`）做子目录，与 SD 卡实际分类一致，且不会
污染原厂残留的 `saves/FC`、`saves/MD`、`saves/fbalpha` 等目录。

> 注：原厂 `saves/` 下是**按核心名**分子目录（libretro 生态惯例）。但 rkgame
> 从不调用 `retro_get_system_info`（0 处命中），shim 拿不到核心名，因此改用
> ROM 父目录。两者不冲突，可并存。

---

## 部署

```sh
sh build_libz.sh            # 交叉编译（Windows 侧，输出 libz.so.1，9648 字节）
sh deploy.sh /mnt/sdcard/cubegm    # 拷贝到 SD 卡（Linux 上）
```

Windows 上（SD 卡挂载为 E:）：

```sh
sh deploy.sh /e/cubegm
```

**只需一步 `cp`，不需要改任何脚本。** 部署脚本会：

1. 备份目标位置原有的 `libz.so.1` 到 `libz.so.1.stock`（若非同一份）
2. 拷贝我们的 shim 到 `<cubegm>/lib/libz.so.1`
3. 顺手清理上版方案的 `LD_PRELOAD` 遗留（如果有）

### 调试（建议第一次这样跑）

临时手动编辑 SD 卡上任意脚本、加进 rcS 里，或者临时改一下 `S80icube`：

```sh
export SRAMSHIM_DEBUG=1
```

日志写到 `/sdcard/cubegm/sramshim.log`，能看到每次 load/save 的字节数。

### 卸载 / 回滚

```sh
sh uninstall.sh /mnt/sdcard/cubegm
# 或手动：
rm /sdcard/cubegm/lib/libz.so.1
# 若 deploy.sh 备份了 .stock，脚本会先从 .stock 还原
```

---

## 可调环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `SRAMSHIM_DIR` | `<rkgame 目录>/saves` | 存档根目录 |
| `SRAMSHIM_INTERVAL` | `10` | 自动落盘间隔（秒），`0` = 只在退出时存 |
| `SRAMSHIM_DEBUG` | 未设 | 设为 `1` 写日志 |
| `SRAMSHIM_ANY_PROC` | 未设 | 设为 `1` 关掉「只认 rkgame」门槛（调试用） |

---

## 构建与校验

```sh
sh build_libz.sh
```

`build_libz.sh` 会：

1. 用 `arm-none-linux-gnueabihf-gcc` 14.2.1
   （`-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2`）
2. `-Wl,-soname,libsramzlib.so` —— **SONAME 必须≠libz.so.1**
3. `-Wl,--no-as-needed $LDDIR/libdl.so.2` —— 显式链接 libdl，不能 `-ldl`
   （工具链 `libdl.a` 是空的，不产生 `DT_NEEDED`）
4. 内置三道断言：SONAME 正确 / `DT_NEEDED` 含 `libdl.so.2` / **绝不**含 `libz.so.1`
5. 跑 `verify_abi.py` + `check_syms_on_device.py` 对照设备 rootfs 核对

### 实测校验结果（2026-08-31，产物 9648 字节，sha256=`ce1c595769f624e7a2751dab88591d94db946df29aa1a77daac3a49b6497e476`）

```
DT_SONAME : libsramzlib.so      ✅（不是 libz.so.1）
DT_NEEDED : libdl.so.2 libc.so.6 ✅（无 libz.so.1 → 不自我引用）
导出      : dlsym / compress / uncompress  ✅
版本      : 全部 @GLIBC_2.4，无 2.31/2.34/2.38 ✅
ABI       : EM_ARM / e_flags=0x5000400 / ET_DYN  ✅
```

**版本锁定（设备能否启动的命门）**：

```c
__asm__(".symver shim_real_dlsym,  dlsym@GLIBC_2.4");
__asm__(".symver shim_real_dlopen, dlopen@GLIBC_2.4");
```

不加这两行 GCC 14 会生成 `dlopen@GLIBC_2.34`，实测设备 `libdl-2.29.so`
**只**导出 `dlopen@@GLIBC_2.4` → ld.so 报 `symbol lookup error` → `rkgame` 起不来。

GCC 14 还有两个坑（工具链无 `zlib.h`，必须自己声明 zlib ABI）：

- 禁用 `atoi` / `strtol`：GCC 14 会优化成 `__isoc23_strtol@GLIBC_2.38`
  → shim 全部用 `int x = 0; for (...) x = x*10 + c-'0';` 手写解析
- 用 `const char *const *` 遍历 `const char *const paths[]`
  （`const char *p` 类型不符，`-Wincompatible-pointer-types` 在 C23 下为 error）

### 为什么 shim 里不用 `dlvsym` 或 `dlsym` 递归

构造函数里取「真实 dlsym」的方式：

```c
static void *real_dlsym = NULL;
__attribute__((constructor)) static void init(void) {
    real_dlsym = (void*)shim_real_dlsym(RTLD_DEFAULT, "dlsym");
    ...
}
```

- `shim_real_dlsym` 由 `.symver` 锁到 `dlsym@GLIBC_2.4`，
  本 shim 导出的是默认版本的 `dlsym`，请求 `dlsym@GLIBC_2.4`
  **只匹配**系统 libdl，不回来调本 shim → 无递归
- 拿到的 `real_dlsym` 是函数指针，后续所有转发都走它，也绕开符号解析

同理 `compress` / `uncompress` 也用函数指针（`real_compress`）转发。

---

## 已修复的真实缺陷（2026-08-31）

| # | 原缺陷 | 后果 | 修复 |
|---|--------|------|------|
| 1 | `on_signal` 里调 `shim_log`/`fwrite`/`sync` | glibc stdio 非 async-signal-safe，信号打断持锁瞬间会**自死锁**，掌机挂死 | 信号里只置 `volatile sig_atomic_t g_flush_req`，落盘移到 `my_retro_run`；最坏丢 1 帧（~16ms），换取绝不挂死 |
| 2 | `g_sram` 指针只在 `load_game` 取一次 | 部分核心运行中会 realloc SRAM（换卡带/切换磁盘），旧指针指向已释放内存，**写出垃圾或写坏相邻堆块** | `sram_save`/`sram_load` 每次都重新 `sram_pull()` 取指针与大小 |
| 3 | 用 `sync()` | 同步整块设备，每 10 秒一次可能产生可感知卡顿 | 改 `fileno(f)` + `fsync(fd)`，只同步目标文件 |
| 4 | GCC 14 默认 C23 下 `dlopen` 生成 `@GLIBC_2.34` | 设备 glibc 2.29 无此版本 → `symbol lookup error` → rkgame 完全起不来 | 加 `.symver` 锁 `dlopen@GLIBC_2.4`；`.gnu.version_r` 复核确认无 2.31/2.34/2.38 |
| 5 | `grep -v pattern file > tmp && mv tmp file` | `grep -v` 全匹配返回 1，`&&` 短路，注入行残留 | deploy/uninstall 改用 `awk '!/pattern/'` |

---

## 废弃清单（不要再用，不要在此基础上继续改）

| 产物 | 废弃原因（均有实证） |
|------|---------------------|
| `sram_hook_v3.S` / `sram_hook_v2.S` / `sram_hook_combined.S` | 假设存在 `state` 结构体（`state->save_game @+0x244`、`state->core_get_memory_data @+0x2f8`、`state->retro_load_game @+0x224`）。**这些字段全部不存在**：`save_game` 0 处命中；core 指针是**独立全局变量**（`_retro_run`@0x3e1a24 等，共 7 个，且**没有** `_retro_get_memory_data`） |
| `sram_hook_v3.S` 的 `state @0x2dbcb4` | `0x2dbcb4` 落在 **`.rodata` 字符串常量区**（相邻是 `"displayfps"`@0x2dbcbc、`"open config.xml fail!"`@0x2dbcc8）。往 `state+0x244` 写指针 = **写只读数据段 → 必然 SIGSEGV** |
| `sram_patch.S` / `patched_rkgame` | 用 `retro_get_memory_size(2)` / `get_memory_data(2)` —— **2 是 SYSTEM_RAM 不是 SAVE_RAM**（标准：SAVE_RAM=0, RTC=1, SYSTEM_RAM=2, VIDEO_RAM=3）。即使跑起来，写出的是系统 RAM 而非存档 |
| 同上 | 把 `handle` 硬编码为 `0x3b21c8`。设备符号表里存在两个同名 `handle`（0x3b21c8 与 0x3cf988），地址真伪无法确认 |
| 上一版 `deploy.sh`（LD_PRELOAD + 注入 `icube_start.sh`） | 实测开机链 `inittab → rcS → S80icube` **直接 exec icube**，SD 卡上的 `icube_start.sh` 无人调用；且挂载点是 `/sdcard` 不是 `/mnt/sdcard` |
| `src/libretro.h` | `#define RETRO_MEMORY_SAVE_RAM 3` **错误**（3 = VIDEO_RAM）。正确值 0。仅影响阅读，不参与编译 |

### 二进制打补丁路线为何整体不可行

1. **空间不足**：hook 代码约 440–600 字节，而 `Core_Load` 仅 900 字节、`run_process` 仅 124 字节，无内联空间
2. **地址依赖**：补丁硬编码 PLT 地址（dlsym@0x98a4 等）和全局变量地址，任何重新编译都会失效
3. **违反项目铁律**：修改原厂二进制（`icube`/`rkgame`/`driver.so`）是明令禁止的
4. **libz.so.1 方案不需要任何 rkgame 内部地址** —— 通过 `LD_LIBRARY_PATH` 在启动链最外圈注入，天然免维护

---

## 已知限制

1. 只保存 `RETRO_MEMORY_SAVE_RAM`（=0）。`RTC`（时间芯片，多为 SFC 时钟存档）未覆盖 —— 需求出现时补 `sram_pull` 的 fallback 即可
2. 存档路径按 ROM 父目录组织，与 libretro 生态的核心名约定不同（因 rkgame 不取 `retro_get_system_info`）
3. 硬断电（拔电/低电量）无法捕获信号，最坏丢 `SRAMSHIM_INTERVAL` 秒的进度 —— 这是定期落盘语义的固有权衡，非缺陷
4. 若 `rkgame` 退出时不走 `retro_unload_game` 路径（如被 `kill -9`），则依赖定期落盘兜底
5. 若 SD 卡 `lib/` 目录不存在（`S80icube` 里 `LD_LIBRARY_PATH` 指向无效路径），则本方案不适用 —— 部署脚本会立即报错退出，不会留下半截状态
