# 输出格式

## 根目录

```text
<exe-directory>/output/
```

输出目录不存在时会自动创建。每个批次按当前时间生成目录名，格式为：

```text
yyyyMMdd-HHmmss
```

当只处理一个已经识别出名称的自动扫描项目时，程序可以直接使用项目名称作为最终目录；多个项目或手动导入批次会使用时间目录包裹项目目录。

## 项目目录

```text
项目名/
├─ 解包清单.json
├─ reconstruction-report.json      # 源码恢复数量及尚未恢复的文件
├─ app-config.json                 # 如果输入包包含
├─ app.json                        # 能从配置重建时生成
├─ pages/                          # 以实际包内容为准
├─ components/
├─ common/
├─ __PLUGINCODE__/
└─ runtime-cache/
```

目录名称会经过 Windows 文件名清理；重复名称会自动追加稳定后缀，避免覆盖已有项目。

## `解包清单.json`

清单用于描述证据和结果，而不是替代源码。字段包括：

| 字段 | 含义 |
| --- | --- |
| `name` | 项目展示名称 |
| `wxid` | 项目的 WXID |
| `packages` | 实际处理的 wxapkg 列表 |
| `dependencies` | 随项目闭包处理的通用插件 |
| `declaredSubpackages` | `app-config.json` 声明的分包 |
| `subpackagesNotPresentInLocalWxapkgCache` | 声明但本机未找到的分包 |
| `sourceCompleteness` | wxapkg 源文件完整性 |
| `analysisCompleteness` | 是否有运行时缓存补充 |
| `sourceReconstruction` | 当前项目的源码恢复状态、报告路径和缺失文件数 |

## 完整性判读

- `declared-packages-present`：声明的源包都能在当前输入范围找到并成功处理；不表示编译后的模板已经还原。
- `partial`：部分 wxapkg 缺失或处理失败，但已有包仍然输出。
- `available-content-recovered`：已提取当前实现能恢复的内容，不保证能直接运行。
- `source-recovery-partial`：源码仍有未恢复的文件，具体见源码恢复报告。
- `runtime-cache-supplemented`：运行时缓存为源码分析提供了额外证据，不代表缺失 wxapkg 被恢复。

日志和清单会保留缺失原因。工具不会用占位文件冒充缺失分包。

## `reconstruction-report.json`

`javascriptModules`、`jsonFiles`、`stylesheets` 分别记录从容器中新增恢复的 JS 模块、JSON 配置和 WXSS 数量，不含包内原本就有的文件。

`missingSourceFiles` 列出页面或组件尚未恢复的源文件，`warnings` 记录不能解析的配置、样式等问题。存在缺失或警告时，`status` 为 `partial`，批处理也会计入“部分完成”。

恢复支持 `define()` 容器、`__wxAppCode__` 内嵌 JSON、`setCssToHead()` 及公共样式数组。WXSS 可恢复 rpx、组件作用域标记和 import 引用；原有文件和编译容器保留。编译后的 WXML 尚未实现完整反编译，不生成空页面或占位模板；查看原始 `app-wxss.js`、`page-frame.js` 和页面 HTML 可继续分析。

手动导入同一 WXID 的多个数字版本目录时，同名包优先采用较大的版本号，其余包保存在 `_历史版本/<版本>/<包名>/`。
