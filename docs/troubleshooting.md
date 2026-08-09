# StreamBridge Android 端问题排查记录

## 问题 1：APK 安装失败 — INSTALL_FAILED_USER_RESTRICTED

**现象**：`adb install` 报 `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`

**原因**：小米手机（MIUI）默认禁止 USB 安装。需要在开发者选项中开启"USB 安装"或"通过 USB 安装应用"。

**解决**：用户在手机上允许 USB 安装权限后解决。

**面试要点**：Android 设备管理策略，不同厂商（MIUI/ColorOS/EMUI）对 USB 调试的限制差异。

---

## 问题 2：App 闪退 — DT_NEEDED 嵌入 Windows 绝对路径

**现象**：
```
UnsatisfiedLinkError: dlopen failed: library "D:\code\StreamBridge\android\app\build\intermediates\..."
not found: needed by libstreambridge_android.so
```

**根因分析**：
1. FFmpeg 交叉编译时，`.a` → `.so` 的链接步骤**没有设置 DT_SONAME**（即 `-Wl,-soname,libavcodec.so`）
2. ndk-build 的 `PREBUILT_SHARED_LIBRARY` 会将预编译 `.so` 复制到 NDK 中间目录
3. 链接 `libstreambridge_android.so` 时，由于 FFmpeg `.so` 缺少 SONAME，链接器**将 Windows 绝对路径写入了 ELF 的 DT_NEEDED 字段**
4. Android 设备上的动态链接器 `dlopen()` 找不到这个 Windows 路径 → 抛 UnsatisfiedLinkError

**验证方法**：
```bash
# 查看 ELF 动态段，确认 DT_NEEDED 是库名还是完整路径
llvm-readobj --dynamic-table libstreambridge_android.so | grep NEEDED
# 错误输出：NEEDED Shared library: [D:\code\StreamBridge\...\libavformat.so]
# 正确输出：NEEDED Shared library: [libavformat.so]
```

**修复**：用已有的 `.a` 文件重新链接 `.so`，加上 `-Wl,-soname,libavXXX.so`：
```bash
clang --target=aarch64-linux-android21 -shared \
    -Wl,-soname,libavformat.so \
    -Wl,--whole-archive libavformat.a -Wl,--no-whole-archive \
    -L. -lavcodec -lswresample -lavutil -o libavformat.so -lm -latomic -lz
```

**面试要点**：ELF 文件格式、动态链接过程、DT_SONAME / DT_NEEDED 的含义、Android 的动态链接器 vs GNU ld.so。

---

## 问题 3：libavformat.so 缺少 zlib 符号

**现象**：
```
UnsatisfiedLinkError: dlopen failed: cannot locate symbol "uncompress"
referenced by libavformat.so
```

**原因**：FFmpeg 的 RTMP 协议实现内部使用 zlib 进行握手数据压缩/解压。`.so` 链接时没有链接 `-lz`，导致 `uncompress` 符号未定义。

**修复**：重新链接时添加 `-lz`。`libz.so` 是 Android 系统自带库，无需打包进 APK。

**面试要点**：FFmpeg 模块间依赖关系（RTMP → zlib），Android 系统库 vs 应用打包库，符号解析过程。

---

## 问题 4：RTMP 连接失败 — EADDRNOTAVAIL（排查中）

**现象**：
```
avformat_open_input failed: Cannot assign requested address
```
而从同一设备执行 `nc 192.168.31.57 1935` 可以成功建立 TCP 连接。

**已排查方向**：
1. ✅ Android 网络权限（INTERNET + usesCleartextTraffic）— 已配置
2. ✅ SRS 服务器可用性 — Windows ffplay 可正常播放
3. ✅ 手机到 Linux 的网络连通性 — ping 和 nc 都成功
4. ✅ RTMP 协议已编译进 FFmpeg（`llvm-nm` 确认 `ff_rtmp_protocol` 符号存在）
5. ✅ FFmpeg SONAME 问题已修复 — DT_NEEDED 使用库名而非路径
6. 🔄 FFmpeg `rtmp_transport` 等选项 — 尝试中
7. 🔄 Java Socket 同进程 TCP 连通性测试 — 待验证

**下一步**：确认 Java 层 Socket 是否能从 App 进程内连接同一地址，排除 Android 进程级网络隔离。

---

## 问题 5：Gradle 构建 — JDK 环境

**现象**：本机无 Java，`gradlew.bat` 无法运行。

**解决**：使用 Android Studio 自带的 JBR (JetBrains Runtime)：
```
JAVA_HOME=D:/soft/AS/jbr  # JDK 25
```

**面试要点**：Gradle Wrapper 机制、JAVA_HOME 的作用、Android Studio 与 JDK 的捆绑关系。

---

## 经验教训

1. **交叉编译产物必须验证 ELF 元数据**：不仅检查架构（arm64），还要检查 SONAME、NEEDED、依赖库等动态段信息。
2. **`--disable-all` 有风险**：FFmpeg 最小化构建容易遗漏隐藏依赖（如 RTMP → zlib），需要仔细梳理依赖链。
3. **Android 动态库加载顺序**：FFmpeg 库之间有依赖关系（avformat → avcodec → swresample → avutil），加载顺序不对也会失败。使用 `System.loadLibrary()` 按拓扑序加载是常见方案。
4. **Windows + NDK 交叉编译的路径陷阱**：Windows 路径（`D:\...`）可能被嵌入 ELF，必须确保使用相对路径或 SONAME。
