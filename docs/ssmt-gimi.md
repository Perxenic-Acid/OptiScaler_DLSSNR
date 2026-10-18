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

## 中文面板

Insert 面板按 Windows 界面语言选择中文或英文，也可在“界面语言、主题与颜色”中
切换，再点击“保存设置”。INI 对应 `[Menu] Language=auto|zh-cn|en`。
缺少中文字体时回退英文；从 Windows Fonts 读取字体，不打包或再分发系统字体。
原有快捷键、滑块、预设编号、技术名称与渲染后端保持原义。

显示适配集中在 `OptiScaler/menu`，未修改 ImGui 源码。
词条位于 `translations_zh_cn.inc`；新增显示文本没有词条时回退原文。
中英文控件保留一致的 `###` 标识；动态重置按钮的 `##` 后缀继续区分不同设置。
显示帮助时使用非格式化文本，避免百分号被当作没有实参的 printf 占位符。

## 构建与检查

安装 Visual Studio C++ 工具、CMake、Python，并初始化 Git submodule 后运行：

```powershell
git submodule update --init --recursive
.\scripts\build-ssmt.ps1
```

脚本先执行无图形设备的 ImGui 测试和词条检查，再串行构建 Release DLL。
串行选项同时限制 MSBuild 和项目内的 `/MP`，避免并行编译耗尽预编译头内存。
产物位于 `x64/Release/a/OptiScaler.dll`，来源与 SHA-256 位于
`build/ssmt/artifact.json`。`dirty=true` 表示构建时源码未提交，不能冒充固定提交的发布产物。

离线检查覆盖词条占位符、全部中文字形、语言切换后的实际控件 ID、重复按钮区分、
字体不可用回退及 ImGui 绘制数据，不创建 D3D 设备、不启动游戏。
它不能证明游戏中的 FSR2 dispatch、NR 或与 3DMigoto 的硬件共存已通过。
