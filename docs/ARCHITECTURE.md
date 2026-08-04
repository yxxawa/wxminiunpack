# 架构说明

## 总体结构

应用由 Win32 外壳、WebView2 前端和可测试的 C++ 核心组成：

```text
Win32 AppWindow
├─ WebView2 controller
├─ OLE drag/drop overlay
├─ folder picker
└─ message bridge
   └─ WebView2 app.html

wedecode_core
├─ wechat_cache_scanner
├─ wxapkg
├─ batch_runner
├─ reconstructor
└─ common
```

## 数据流

1. `WechatCacheScanner` 枚举本机微信账号目录和版本目录。
2. 扫描器以路径、WXID、版本、包名和文件证据构建 `InputFolder`。
3. 依赖解析把项目引用的通用插件加入同一处理闭包，并保留来源信息。
4. `BatchRunner` 按主包、分包、插件和运行时缓存的顺序处理输入。
5. `Wxapkg` 读取文件索引，必要时使用目录外层 WXID 推导包密钥，然后写出安全相对路径。
6. `SourceReconstructor` 生成页面默认文件、应用配置和重建目录。
7. 前端收到增量扫描、名称补全、任务状态和日志消息，避免等待全部项目完成才显示列表。

## 线程模型

- UI 线程：窗口消息、WebView2 事件、拖放和界面更新。
- 扫描线程：账号/缓存发现与项目聚合。
- 名称线程：后台名称与运行时证据补全。
- 任务线程：wxapkg 解包、依赖合并和输出。

跨线程结果通过 `PostMessageW` 和受互斥保护的待处理队列回到 UI 线程。取消请求使用原子标志，不直接从后台线程操作 WebView2。

## 边界

- `wechat_cache_scanner` 不负责输出文件。
- `wxapkg` 不负责项目名称识别。
- `batch_runner` 不依赖 WebView2。
- `app.cpp` 只负责桌面生命周期和消息桥接。

这样可以在没有 UI 的情况下编译和测试核心解包逻辑，也方便以后替换前端。
