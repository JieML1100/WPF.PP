# CUI

[English](README.en.md) | [简体中文](README.md)

CUI is a native Windows UI framework built with C++20, Direct2D, and DirectComposition. It borrows the
property, resource, template, layout, input, and routed-event semantics of WPF without trying to reproduce
every WPF API or implementation detail.

The project is under active development, so public APIs and the XAML schema may change as the architecture
evolves.

## Runtime model

CUI keeps two runtime paths separate:

- **Design**: `CuiRuntime`, the Designer, and the dynamic sample parse XAML in-process for editing, preview,
  diagnostics, and hot reload.
- **Production AOT**: `CuiCodeGen` compiles XAML into `.g.h/.g.cpp` during the build. Applications link the
  generated C++, the CUI core, and the required theme closure. They do not read authored XAML or resolve
  types, properties, or converters by name at run time.

Production uses collision-checked type/property tokens and static C++ endpoints. QNames and `RuntimeTypeId`
remain in the Design toolchain; `UIClass` is an internal native behavior-host category, not a XAML type
identity.

Effective property values use this precedence:

```text
Animation > Local > VisualState > Template > Style > Theme > Inherited > Default
```

Binding, DynamicResource, TemplateBinding, and Animation live in property value slots rather than in a
second state system that bypasses dependency properties.

## Main features

- Dependency properties, inheritance, coercion, resources, styles, control templates, and visual states
- Measure/arrange layout, transforms, clipping, Direct2D drawing, and DirectComposition presentation
- Mouse, keyboard, focus, text input, drag and drop, routed events, and routed commands
- Binding, MultiBinding, data templates, items-panel templates, and collection views
- UI Automation peers and common automation patterns
- Native windows, popups, menus, taskbar and tray integration, media, and WebView2
- A DataGrid with row/column virtualization, edit transactions, validation, grouping, and UIA
- A Designer, a dynamic XAML runtime, and Production AOT code generation

## XAML example

This is a compilable document fragment. A Production build converts it to C++ instead of parsing the text
when the application starts.

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

See [`CuiStaticGeneratedSample`](CuiStaticGeneratedSample) for a complete static host,
[`CuiRuntimeSample`](CuiRuntimeSample) for dynamic parsing, and [`CUITest`](CUITest) for the control and XAML
gallery.

## Repository layout

- `CUI/`: property system, element tree, layout, input, commands, automation, and rendering core.
- `D2DGraphics/`, `Utils/`, `XmlLite/`: graphics and infrastructure libraries.
- `CuiRuntime/`: schema, XAML materializer, document sessions, and hot reload for the Design path.
- `CuiDesigner/`: visual designer, canonical serialization, and the code-generation front end.
- `CuiCodeGenCore/`, `CuiCodeGen/`: XAML/AOT generation library and command-line tool.
- `CuiGeneratedTheme/`: static output for `Themes/Generic.xaml`.
- `CuiAotCompileGate/`: compile-time gates for generated Production code and link boundaries.
- `CUICoreTests/`: semantic and regression tests.
- `CUITest/`, `CuiRuntimeSample/`, `CuiStaticGeneratedSample/`: samples and executable smoke tests.

## Build requirements

The full matrix requires Windows SDK, MSVC v145.

This PowerShell snippet finds MSBuild without assuming a Visual Studio edition or install directory:

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
  'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
  -property installationPath | Select-Object -First 1
$msbuild = Join-Path $install 'MSBuild\Current\Bin\MSBuild.exe'
```

For day-to-day development, build `Debug|x64`:

```powershell
& $msbuild .\CUI.sln /t:Build /m:1 /nr:false `
  /p:Configuration=Debug /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Rebuild all four solution configurations before submitting a change. Serial builds prevent projects from
updating sample-generated files at the same time:

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

## Verification

The solution build compiles the AOT gate and automatically runs the generated-code, typed-converter,
binding-scope, and dependency-property-storage checks. The following non-interactive entry points use the
`Debug|x64` solution output directory:

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

Set `CUI_TEST_FILTER` to run Core cases whose names contain a given string:

```powershell
$env:CUI_TEST_FILTER = 'DataGrid'
.\x64\Debug\CUICoreTests.exe
Remove-Item Env:CUI_TEST_FILTER
```

Run the static source-ownership gate separately when changing the Production/Design boundary:

```powershell
.\build\VerifyCuiA8fPhysicalBoundary.ps1 -SourceOnly
```

## AOT extensions

Custom Binding converters are declared in a build-time catalog, and generated code calls named C++
factories directly. Production does not register or look up converters by string. Runnable examples live in
[`TypedBindingConverters.cui.xml`](CuiAotCompileGate/TypedBindingConverters.cui.xml) and
[`TypedConverterContract.cui.xaml`](CuiAotCompileGate/TypedConverterContract.cui.xaml). The build-time
DataGrid auto-column rules are demonstrated in
[`DataGridAutoColumns.cui.xml`](CUITest/DataGridAutoColumns.cui.xml).

The maintained internal notes are intentionally short:

- [DataGrid design constraints](CUI/DataGrid.md)
- [Designer undo contract](CuiDesigner/DesignerCore/Undo.md)
- [Static framework theme](CuiGeneratedTheme/README.md)

## Community

Join our QQ technical discussion group: 522222570

To ensure a smooth and constructive communication experience, all conversations in the group are conducted in Simplified Chinese. Please refrain from using other languages or dialects. Your cooperation is greatly appreciated.

## License

CUI is available under the [MIT License](LICENSE).
