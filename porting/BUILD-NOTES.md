# 八方旅人 II（OCTOPATH TRAVELER II）Android 构建与运行记录

日期：2026-10-18　设备：小米 17 Ultra（2512BPNDAC / HyperOS 3.0 / Android 16 API36 / arm64 / Adreno 840）

## 一、已打通的链路（可复用）

| 环节 | 结果 |
|---|---|
| NSZ → NSP 解压 | ✅ base 5.6GB / update 911MB / DLC 153KB，全部 common ticket |
| 工具链安装（可移植，全在工作区） | ✅ |
| switch2android bootstrap（hactool + repak + .venv） | ✅ |
| `doctor` 全项 | ✅ |
| `inspect` 版本/title 校验 | ✅ 0100a3501946e000 / update 131072 |
| `build` 生成 game.apk + game.s2a | ✅ 哈希/清单/签名校验通过 |
| 安装到手机 | ✅（绕开 HyperOS ADB 安装封锁，手动安装） |
| 6.7GB 资源导入 | ✅ |
| 游戏原生初始化 | ✅ 音频、Vulkan、GPU 内存池、纹理、7 次提交、读取 RomFS/PAK |
| **游戏首帧** | ❌ **job system 死锁** |

## 二、工具链位置与版本

全部位于 `/Users/tanyifan/Documents/deepseek-harness/default-workspace/`（本会话只能写这里）：

- `toolchain/jdk/Contents/Home`　JDK 21.0.12.1（Adoptium Temurin）
- `toolchain/cargo`、`toolchain/rustup`　Rust 1.99.1（rustup minimal）
- `toolchain/dotnet`　.NET SDK 10.0.402
- `toolchain/spirv/bin/spirv-val`　SPIRV-Tools v2026.4（源码编译，原生 arm64）
- `toolchain/cmake/CMake.app/Contents/bin/cmake`　CMake 3.31.8（仅用于编译 SPIRV-Tools）
- `sdk/`　Android SDK：platforms;android-36、ndk;28.2.13676358、cmake;3.22.1、build-tools;36.0.0、platform-tools
- `repo/switch2android`　源码副本（main，提交 46bc3f9）
- `input/`　解压后的三个 NSP
- `keys/prod.keys`　过滤后的有效密钥（仅保留 32/64 位 hex 行，原文件未改）
- `work/`　构建 work 目录（提取、着色器缓存、gradle 缓存）
- `out/`　干净构建产物（game.apk + game.s2a）

## 三、关键环境变量（`env.sh`）

```sh
export WS=/Users/tanyifan/Documents/deepseek-harness/default-workspace
export JAVA_HOME="$WS/toolchain/jdk/Contents/Home"
export ANDROID_HOME="$WS/sdk"
export ANDROID_USER_HOME="$WS/android-user-home"   # 关键：否则 sdkmanager/AGP 写 ~/.android 被拒
export RUSTUP_HOME="$WS/toolchain/rustup"
export CARGO_HOME="$WS/toolchain/cargo"
export DOTNET_ROOT="$WS/toolchain/dotnet"
export DOTNET_CLI_HOME="$WS/toolchain/dotnet-cli"
export NUGET_PACKAGES="$WS/toolchain/nuget"
export GRADLE_USER_HOME="$WS/work/gradle-home"
export PATH="$WS/toolchain/bin:$WS/toolchain/cargo/bin:$WS/toolchain/dotnet:$WS/toolchain/spirv/bin:$WS/sdk/cmake/3.22.1/bin:$WS/sdk/platform-tools:$WS/sdk/cmdline-tools/latest/bin:/opt/homebrew/opt/python@3.11/bin:$PATH"
```

## 四、构建命令

```sh
source "$WS/env.sh"
cd "$WS/repo/switch2android"
.venv/bin/python switch2android.py build "$WS/input" --keys "$WS/keys/prod.keys" \
  --work "$WS/work" --output "$WS/out" \
  --title-id 0100a3501946e000 --version 131072
```

约 1–7 分钟；复用 work 目录时跳过解压/着色器，只重打包 s2a + gradle。

## 五、安装（HyperOS 必读）

- `adb install` 被 HyperOS 拒绝（`INSTALL_FAILED_USER_RESTRICTED`），`am start` 调安装器报 "Shell 没有权限安装"。
- **可行做法**：`adb push game.apk /sdcard/Download/`，在手机文件管理器点 APK 手动安装（同签名覆盖安装保留数据）。
- 8. 资源包：`adb push game.s2a /sdcard/Download/`，App 首次启动点「选择本地资源包」选中它（content URI 读取，绕过 Android/data 的 SELinux 限制）。
- 直接推 `/sdcard/Android/data/<pkg>/files/incoming.s2a` 会 EACCES（shell 标签），不可用。

## 六、已确认的问题与排查方法

1. **NSZ 不支持**：switch2android 只收 NSP。用 `work/nsz-venv/bin/nsz -D x.nsz -o outdir --keys filtered.keys` 解压。
2. **prod.keys 异常条目**：原文件有 35 行非 hex（eticket_rsa_keypair、mariko/master_kek_source 高位、ssl_rsa_key）。switch2android 的 Hactool 封装只挑相关键并校验长度，自动跳过；nsz 需手动过滤成只含 32/64 位 hex 的副本。
3. **sdkmanager 联网失败**：根因是写 `~/.android` 被沙箱拒绝。设 `ANDROID_USER_HOME` 到可写目录即恢复。
4. **AGP debug keystore**：同样依赖 `ANDROID_USER_HOME`，已随环境变量解决。
5. **Vulkan SDK mac 下载是 GUI 安装器**（Qt installer.dat），无法直接取 spirv-val → 改为源码编译 SPIRV-Tools。
6. **cargo/rustc 是 rustup shim**：必须带 `RUSTUP_HOME`/`CARGO_HOME`，否则试图写 `~/.rustup`。
7. **设备导入 EACCES**：不要 shell 直推 Android/data，用 App 文件选择器。

## 七、最终结果：已可玩

2026-10-18 通过以下三项改动，游戏在小米 17 Ultra 上稳定运行：

1. **单核上报**（`native/guest.cpp`：核心掩码 7→1）——job 系统串行化，绕过启动期并发死锁。
2. **SIGPROF 采样**（`native/thread_services.c` + `guest.cpp` + `probe.cpp`）——每 500ms 打断所有线程，
   经验上能震开时序敏感死锁；同时用于导出线程 PC 状态。
3. **uinput 手柄忽略**（`GameInputView.java`）——`uinput-xiaomi` 假手柄不再隐藏虚拟按键。

结果：进入游戏、30 FPS 稳定（`PRESENT_RATE 30.00` / `DISPLAY_RATE 29.97`）、虚拟按键可用、
NACP 官方图标已替换、HUD 6 秒后自动隐藏。

## 八、诊断注入点（保留，开销很小）

- `SVC#` 每调用序号+参数；`MAIN_INIT`；`CREATE_THREAD`/`START_THREAD`/`THREAD_MAIN`；
- `g_G_holder` + `ot2_thread_dump_state()`（UI 线程每 500ms 导出线程等待/PC）；
- GPU fence 20s 超时（`gpu_completion.inc`、`backend.cpp`）。

## 九、换机/换游戏

- 换机（如 18 Fold，同为 ARM64）：同一 APK 可直接装，重点验证 GPU Vulkan 驱动与 16KB 页大小。
- 换游戏：补丁通用；用 `dist/scripts/build-game.sh <title_id> <version> ...`；
  图标从该游戏 `Control/icon_*.dat` 提取。
