# SSMT 分支基线

本分支基于 `Dagherbou/OptiScaler_DLSSNR` 的
`973761621353b99bee3dc7d4bb27b117fef2644f`，保留上游许可证和版权。

汉化之前先迁入 SSMT 当前使用的 GIMI 集成包补丁，按以下顺序应用：

| 补丁 | SHA-256 |
| --- | --- |
| `OptiScaler-DLSSOn12-GIMI-pre-NR.patch` | `437a643e15f661b412f18caffa03fb331850cd8ec93dff8fa91bbdb47ab6e0b5` |
| `OptiScaler-GIMI-RTX30-dual-mode.delta.patch` | `69d4d6cc2aa732c771e8a5851a5a0954656d377cf16b76723eda21adb72642a9` |

补丁来源为 GIMI 集成包 `cc4697187cb57bacc7c932b7f53f16be24af152a` 的
`src/patches`。这些改动包括原生 Device / Context 解析、DX11 Present 类型检查、
共享颜色格式转换、原生 NGX 参数与 queue-affinity，以及私有 NR 桥名称排除。
只迁入源码不代表已完成游戏内验收。

SSMT 的原神路径仍通过热注入部署；不要执行上游向游戏目录复制代理 DLL 的安装步骤。
沿用插件管理的 DX11 输入、DLSS-on-D3D12 输出，以及外部 ReShade / NR 配置。
汉化不得改变技术名称、INI 键、模型预设值或渲染路线。
