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
├─ unpack-manifest.json
├─ app-config.json                 # 如果输入包包含
├─ app.json                        # 能从配置重建时生成
├─ pages/                          # 以实际包内容为准
├─ components/
├─ common/
├─ __PLUGINCODE__/
└─ runtime-cache/
```

目录名称会经过 Windows 文件名清理；重复名称会自动追加稳定后缀，避免覆盖已有项目。

## `unpack-manifest.json`

清单用于描述证据和结果，而不是替代源码。字段包括：

| 字段 | 含义 |
| --- | --- |
| `name` | 项目展示名称 |
| `wxid` | 项目的 WXID |
| `packages` | 实际处理的 wxapkg 列表 |
| `dependencies` | 随项目闭包处理的通用插件 |
| `subpackagesDeclaredByConfig` | `app-config.json` 声明的分包 |
| `subpackagesNotPresentInLocalWxapkgCache` | 声明但本机未找到的分包 |
| `sourceCompleteness` | wxapkg 源文件完整性 |
| `analysisCompleteness` | 是否有运行时缓存补充 |
| `status` | 成功、部分完成或失败 |

## 完整性判读

- `source-complete`：声明的源包都能在当前输入范围找到。
- `package-partial`：部分 wxapkg 缺失或处理失败，但已有包仍然输出。
- `runtime-cache-supplemented`：运行时缓存为源码分析提供了额外证据，不代表缺失 wxapkg 被恢复。

日志和清单会保留缺失原因。工具不会用占位文件冒充缺失分包。
