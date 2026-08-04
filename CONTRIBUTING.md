# 贡献指南

感谢参与改进微信小程序一键反编译。

## 提交前检查

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release --parallel
ctest --preset windows-x64-release --output-on-failure
```

## 代码约定

- C++ 使用 C++20，保持现有 MSVC 警告级别和 UTF-8 源文件编码。
- 文件系统路径使用 `std::filesystem::path`，不要手工拼接 Windows 分隔符。
- 后台线程不要直接调用 WebView2；通过窗口消息回到 UI 线程。
- 处理来自缓存的文件名时使用安全相对路径校验。
- 新增日志应说明阶段、输入对象和可操作的结果。
- 不提交真实微信缓存、个人账号路径、输出源码或构建目录。

## 提交问题

请附上：

- Windows 版本和 WebView2 Runtime 版本
- 程序版本或 commit
- 脱敏后的日志片段
- `unpack-manifest.json` 中的完整性字段
- 最小可复现步骤

不要上传真实账号目录、Cookie、Token、个人信息或未授权的第三方源码。

## Pull Request

PR 描述建议包括：

1. 改动目的
2. 受影响的模块
3. 测试命令及结果
4. 是否改变输出格式或兼容性
5. 是否需要更新文档
