# switch2android 八方旅人 II 可玩版 —— 蒸馏与复用包

本目录是把「用 switch2android 把《八方旅人 II》跑到小米 17 Ultra 上并真正可玩」的完整成果蒸馏成的可复用资产。

- 日期：2026-10-18
- 设备：小米 17 Ultra（2512BPNDAC，HyperOS 3.0 / Android 16 / arm64 / Adreno 840）
- 上游：https://github.com/n06ody/switch2android ，基线提交 `46bc3f9`
- 本包对应补丁：`patches/0001-ot2-playable.patch`（基于 46bc3f9）

---

## 一、目录说明

```
dist/
  README.md                  ← 本文件（总说明 + 复用指南）
  docs/BUILD-NOTES.md        ← 完整构建/排错记录（含版本与坑）
  patches/0001-ot2-playable.patch   ← 全部代码改动（git format-patch）
  apk/game.apk               ← 已可玩的最终 APK（4.7 MB）
  icons/ot2-icon-256.png     ← 从 NACP 提取的官方图标（256×256）
  mipmaps/                   ← 各密度启动图标（48/72/96/144/192）
  scripts/env.sh             ← 本机环境变量（换机需按实际路径改）
  scripts/build-game.sh      ← 通用构建脚本（换游戏只改 4 个参数）
```

> **注意**：`game.s2a`（6.7 GB 资源包）不在本目录内（体积太大）。它在本机
> `out-diag11/0100a3501946e000-v131072/game.s2a`，上传云盘时请单独上传这个文件，
> 并与 `apk/game.apk` 放在同一目录（两者必须来自**同一次构建**）。

---

## 二、这套补丁解决了什么问题

| 补丁 | 文件 | 作用 |
|---|---|---|
| ① 单核上报 | `native/guest.cpp` | 把给游戏的核心掩码从 3 核（7）改为 **1 核（1）**。游戏 job 系统串行化，绕过启动期并发死锁 |
| ② SIGPROF 采样 | `native/guest.cpp`、`thread_services.c`、`probe.cpp` | 每 500 ms 用 `SIGPROF` 打断所有线程并记录 PC；经验上能"震开"启动期的时序敏感死锁 |
| ③ uinput 手柄忽略 | `GameInputView.java` | `uinput-xiaomi` 等注入的假手柄不再被当成真手柄，屏幕虚拟按键正常显示 |
| ④ HUD 自动隐藏 | `MainActivity.java` | 性能 HUD 6 秒后自动隐藏，点它立即隐藏 |
| ⑤ 应用图标 | `AndroidManifest.xml` + `mipmap-*` | 用游戏 NACP 里的官方图标替换默认矢量图 |
| ⑥ 调试注入 | `guest.cpp`、`thread_services.*`、`backend.cpp`、`gpu_completion.inc` | SVC 序列日志、线程生命周期、STATE_DUMP、GPU fence 20s 超时。**正式版可保留（开销很小）也可裁剪** |

---

## 三、从零复现（同一台 Mac）

```sh
# 1. 克隆上游并应用补丁
git clone https://github.com/n06ody/switch2android switch2android
cd switch2android
git checkout 46bc3f9
git apply /path/to/dist/patches/0001-ot2-playable.patch

# 2. 环境（按本机实际路径修改 scripts/env.sh）
source /path/to/dist/scripts/env.sh

# 3. 准备工具（一次性）
./scripts/bootstrap-tools.sh "$S2A_WORK"
.venv/bin/python switch2android.py doctor    # 全部 OK 才继续

# 4. 校验输入
.venv/bin/python switch2android.py inspect "$S2A_INPUT" \
  --keys "$S2A_KEYS" --hactool "$S2A_WORK/tools/hactool/hactool"

# 5. 构建
.venv/bin/python switch2android.py build "$S2A_INPUT" \
  --keys "$S2A_KEYS" --work "$S2A_WORK" --output "$S2A_OUT" \
  --title-id 0100a3501946e000 --version 131072
```

构建依赖（版本必须匹配）：JDK 21、.NET SDK 10、Rust、SPIRV-Tools（spirv-val）、
Android SDK platform-36 / NDK 28.2.13676358 / cmake 3.22.1 / build-tools 36.0.0。

---

## 四、安装到 HyperOS（关键坑）

1. `adb install` 会被 MIUI/HyperOS 拒绝（`INSTALL_FAILED_USER_RESTRICTED`）。**必须手动装**：
   ```sh
   adb push game.apk /sdcard/Download/
   ```
   在手机文件管理器点 APK 安装。
2. 资源包 `game.s2a` 推到 `/sdcard/Download/`，首次启动点「选择本地资源包」选中它。
   **不要** shell 直推 `/sdcard/Android/data/...`（SELinux 标签会导致 App 读不了）。
3. 同签名覆盖安装会保留存档；**卸载会连导入的资源和存档一起删掉**。

---

## 五、复用到其他游戏

### 5.1 代码补丁是通用的

①③④ 与游戏无关，直接复用；②（SIGPROF/单核）是稳定性兜底，也建议保留。
⑤ 图标需要**按游戏提取**（见 5.3）。⑥ 调试注入可保留，出问题时可开日志排查。

### 5.2 换游戏只需改参数

```sh
# 例：三角战略（switch2android 已适配）
dist/scripts/build-game.sh 0100cc80140f8000 262144 /path/to/input /path/to/prod.keys /path/to/work /path/to/out
```

前提：该游戏必须在 `profiles/` 里有配置（否则 `build` 会拒绝，`inspect` 仍可用）。
目前上游支持：八方旅人 II（0100a3501946e000 / 131072）、三角战略（0100cc80140f8000 / 262144）。

### 5.3 提取任意游戏的图标（NACP）

游戏 Control 数据里每个语言一个 `icon_<Language>.dat`，就是 256×256 JPEG 官方图标：

```sh
# 构建后 work/games/<id>/Control/ 下就有
sips -s format png work/games/<id>/Control/icon_Japanese.dat --out icon.png
# 再按 5.4 生成 mipmap
```

### 5.4 生成 mipmap 并替换

```sh
for pair in "mdpi 48" "hdpi 72" "xhdpi 96" "xxhdpi 144" "xxxhdpi 192"; do
  set -- $pair
  mkdir -p app/src/main/res/mipmap-$1
  sips -z $2 $2 icon.png --out app/src/main/res/mipmap-$1/ic_launcher.png
done
```
Manifest 里改 `android:icon="@mipmap/ic_launcher"`。

### 5.5 新增游戏的适配（上游未支持时）

`build` 会因 `profiles/` 无配置而拒绝——这是上游的安全设计，**不要绕过**。
需要上游新增该游戏的 `profiles/<title_id>.json`（含 `main_build_ids`、资源适配器、帧率），
属于真正的「新增适配」，成本高、不确定性大。应先用 `inspect` 确认版本/title，
再评估是否有相近基础。

---

## 六、已知问题速查

- **sdkmanager 联网失败**：设 `ANDROID_USER_HOME` 到可写目录（见 env.sh）。
- **cargo 写 ~/.rustup 被拒**：必须带 `RUSTUP_HOME`/`CARGO_HOME`。
- **NSZ 不支持**：先 `nsz -D` 转 NSP；keys 需过滤掉非 hex 行。
- **启动黑屏卡住**：先看 `OT2Guest` 日志最后一条；本包的 ① ② 已针对该问题。
- **触摸无效/无按钮**：检查是否检测到 `uinput-*` 假手柄（本包 ③ 已修）。
- **换机（如 18 Fold）**：CPU 同为 ARM64，同一 APK 可装；重点验证 GPU 的 Vulkan 驱动表现与页大小（16KB）。

---

## 七、隐私与合规

- 本包**不含**游戏本体、密钥、存档；`game.apk` 与 `game.s2a` 含游戏内容，仅供你本机自用，**不要公开分发或上传到公开仓库**。
- 上传 Git 时请只提交 `patches/` 与 `scripts/`（源码级改动），不要提交 APK/s2a/密钥。
- 上传迅雷云盘请设为**私有**，仅自己可见。
