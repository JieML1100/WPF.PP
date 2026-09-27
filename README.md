# CUI

[简体中文](README.md) | [English](README.en.md)

CUI 是一个使用 C++20、Direct2D 和 DirectComposition 实现的 Windows 原生 UI 框架。它借鉴
WPF 的属性、资源、模板、布局、输入和路由事件语义，但不以逐项复制 WPF API 或内部实现为目标。

项目仍在持续开发，公开接口和 XAML Schema 可能随架构调整而变化。

## 运行模型

CUI 有两条明确分开的运行路径：

- **Design**：`CuiRuntime`、Designer 和动态样例在进程内解析 XAML，用于编辑、预览、诊断与热重载。
- **Production AOT**：构建时由 `CuiCodeGen` 把 XAML 编译成 `.g.h/.g.cpp`。应用只链接生成的 C++、
  CUI 核心库和需要的主题闭包；运行时不读取 authored XAML，也不依赖按名称查找类型、属性或 converter。

Production 使用经过冲突检查的类型/属性 token 和静态 C++ endpoint。QName 与 `RuntimeTypeId` 保留在
Design 工具链中；`UIClass` 只是 native behavior host 的内部分类，不是 XAML 类型身份。

属性有效值的优先级为：

```text
Animation > Local > VisualState > Template > Style > Theme > Inherited > Default
```

Binding、DynamicResource、TemplateBinding 和 Animation 都保存在属性值槽中，不维护一套绕过属性系统的
平行状态。

## 已实现的主要能力

- 依赖属性、继承、coercion、资源、Style、ControlTemplate 和 VisualState
- Measure/Arrange 布局、变换、裁剪、Direct2D 绘制与 DirectComposition 呈现
- 鼠标、键盘、焦点、文本输入、拖放、路由事件和路由命令
- Binding、MultiBinding、DataTemplate、ItemsPanelTemplate 和集合视图
- UI Automation peer 与常用自动化 pattern
- 原生窗口、弹出层、菜单、任务栏、托盘、媒体和 WebView2 集成
- 带行列虚拟化、编辑事务、验证、分组和 UIA 的 DataGrid
- Designer、动态 XAML 运行时和 Production AOT 代码生成

## XAML 示例

下面是一个可编译的文档片段。Production 构建会先把它转成 C++，不会在程序启动时再解析这段文本。

```xml
<Window xmlns="urn:cui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        x:Class="Sample.MainWindow"
        Title="CUI" Width="640" Height="360">
  <Window.Resources>
    <Color x:Key="Accent">#FF2F6FE4</Color>
    <Style TargetType="Button">
      <Setter Property="Background" Value="{StaticResource Accent}" />
    </Style>
  </Window.Resources>

  <StackPanel Margin="20">
    <TextBlock Text="C++ WPF semantics" FontSize="24" />
    <TextBox x:Name="nameEditor" Text="{Binding Name, Mode=TwoWay}" />
    <Button x:Name="saveButton" Content="_Save" Click="OnSave"
            AutomationProperties.Name="Save" />
  </StackPanel>
</Window>
```

完整的静态宿主见 [`CuiStaticGeneratedSample`](CuiStaticGeneratedSample)，动态解析示例见
[`CuiRuntimeSample`](CuiRuntimeSample)，综合控件与 XAML 示例见 [`CUITest`](CUITest)。

## 目录

- `CUI/`：属性系统、元素树、布局、输入、命令、自动化与渲染核心。
- `D2DGraphics/`、`Utils/`、`XmlLite/`：图形和基础设施库。
- `CuiRuntime/`：Design 路径使用的 Schema、XAML materializer、文档会话与热重载。
- `CuiDesigner/`：可视化设计器、规范化序列化和代码生成前端。
- `CuiCodeGenCore/`、`CuiCodeGen/`：XAML/AOT 代码生成库与命令行工具。
- `CuiGeneratedTheme/`：`Themes/Generic.xaml` 的静态主题产物。
- `CuiAotCompileGate/`：Production 生成代码和链接边界的编译门禁。
- `CUICoreTests/`：核心语义与回归测试。
- `CUITest/`、`CuiRuntimeSample/`、`CuiStaticGeneratedSample/`：演示与可执行 smoke test。

## 构建环境

完整矩阵需要 Windows SDK、MSVC。

以下 PowerShell 片段不依赖 Visual Studio 的 edition 或安装目录：

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
  'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
  -property installationPath | Select-Object -First 1
$msbuild = Join-Path $install 'MSBuild\Current\Bin\MSBuild.exe'
```

日常开发通常构建 `Debug|x64`：

```powershell
& $msbuild .\CUI.sln /t:Build /m:1 /nr:false `
  /p:Configuration=Debug /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

提交前应重建四个 solution 配置。串行构建可以避免多个项目同时更新示例生成文件：

```powershell
foreach ($configuration in 'Debug', 'Release') {
  foreach ($platform in 'x86', 'x64') {
    & $msbuild .\CUI.sln /t:Rebuild /m:1 /nr:false `
      "/p:Configuration=$configuration" "/p:Platform=$platform" `
      /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  }
}
```

## 验证

Solution 构建会编译 AOT gate，并自动执行生成代码、typed converter、Binding scope 和依赖属性存储边界检查。
下面这些命令是无交互的可执行验证入口；示例使用 `Debug|x64` 的 solution 输出目录：

```powershell
.\x64\Debug\CUICoreTests.exe
.\x64\Debug\Designer.exe --self-test
.\x64\Debug\CUITest.exe --construct-xaml
.\x64\Debug\CUITest.exe --validate-xaml
.\x64\Debug\CUITest.exe --smoke-xaml
.\x64\Debug\CUITest.exe --render-smoke
.\x64\Debug\CuiRuntimeSample.exe
.\x64\Debug\CuiStaticGeneratedSample.exe
.\x64\Debug\CuiCodeGen.exe --version
```

测试可以用 `CUI_TEST_FILTER` 选择名称中包含指定文本的 Core case：

```powershell
$env:CUI_TEST_FILTER = 'DataGrid'
.\x64\Debug\CUICoreTests.exe
Remove-Item Env:CUI_TEST_FILTER
```

静态源所有权检查可单独运行：

```powershell
.\build\VerifyCuiA8fPhysicalBoundary.ps1 -SourceOnly
```

## AOT 扩展

自定义 Binding converter 通过构建期 catalog 声明，生成代码直接调用具名 C++ factory；Production 不注册或
按字符串查找 converter。可运行示例在
[`TypedBindingConverters.cui.xml`](CuiAotCompileGate/TypedBindingConverters.cui.xml) 和
[`TypedConverterContract.cui.xaml`](CuiAotCompileGate/TypedConverterContract.cui.xaml)。DataGrid 自动列的
生成期规则示例在 [`DataGridAutoColumns.cui.xml`](CUITest/DataGridAutoColumns.cui.xml)。

内部设计说明只记录仍需维护的约束：

- [DataGrid 设计约束](CUI/DataGrid.md)
- [Designer Undo 契约](CuiDesigner/DesignerCore/Undo.md)
- [静态框架主题](CuiGeneratedTheme/README.md)

## 社区

欢迎加入我们的 QQ 技术交流群：522222570

为营造良好的沟通环境，群内统一使用简体中文进行交流，请勿使用其他语言或方言。感谢您的理解与配合。

## 许可证

CUI 使用 [MIT License](LICENSE)。
