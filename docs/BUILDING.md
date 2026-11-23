# 构建指南

## 1. 准备工具链

| 工具 | 要求 |
| --- | --- |
| Windows | Windows 10/11 x64 |
| Visual Studio | 2022 或更高版本，安装 Desktop development with C++ |
| CMake | 3.24+ |
| Ninja | 与 CMake 配套 |
| WebView2 | Evergreen Runtime |

仓库内的 `third_party/webview2` 提供 C++ 头文件和 x64 静态 loader，因此编译阶段不需要额外下载 WebView2 SDK。

## 2. 配置与编译

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release --parallel
```

## 3. 测试

```powershell
ctest --preset windows-x64-release --output-on-failure
```

当前核心回归覆盖：

- wxapkg 文件头与索引读取
- 损坏索引、重复插件条目、加密格式与错误 WXID
- 相对路径安全校验
- 运行时名称证据提取
- 基础项目重建路径
- JS 容器、内嵌 JSON、WXSS、配置保留及未恢复文件报告
- 手动导入的版本优先级

核心测试在 Release 模式也会启用断言。真实样本检查见 [解包检查记录](UNPACKING_AUDIT.md)。

## 4. 安装到发布目录

```powershell
cmake --install build/release --prefix build/package
```

`build/package` 中会放置主程序。运行时输出目录仍然是 EXE 同目录下的 `output`。

## 5. Visual Studio 开发者命令行

如果普通 PowerShell 找不到 MSVC，可以先执行：

```powershell
cmd /k "call \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat\" -arch=x64 -host_arch=x64"
```

然后在新开的窗口中执行上面的 CMake 命令。Visual Studio 2022 的安装路径按本机实际版本调整。

## 6. 清理构建

```powershell
cmake --build build/release --target clean
```

或删除仓库内的 `build/` 目录后重新配置。构建目录不应提交到 Git。
