# DataGrid 设计约束

本文档记录 `DataGrid` 当前实现中需要长期保持的边界。它不是 WPF 成员清单；当 WPF 语义与 C++
所有权、AOT 或虚拟化发生冲突时，以可预测的原生实现和规模性能为先。

## 能力范围

当前实现包括：

- Text、CheckBox、ComboBox、Hyperlink 和 Template 列；
- 自动列、逻辑顺序与显示顺序分离、可见性、冻结、重排和完整列宽模式；
- Cell、FullRow、CellOrRowHeader 选择，CurrentCell、键盘导航和多列排序；
- Cell/Row 编辑事务、新增和删除行、行级验证及错误模板；
- 普通 Binding、非嵌套 MultiBinding，以及受约束的 ElementName 和 FindAncestor 源；
- Grid/Column/Row/Cell/Header/Element 的样式与模板；
- RowDetails、分组、行高调整、template/style selector 和生命周期事件；
- 复制命令、TSV 剪贴板、ScrollIntoView 和虚拟 UI Automation 节点。

公开行为由 `CUI/include/DataGrid.h` 定义。运行时细节集中在 `CUI/src/DataGrid.cpp`，声明式解析位于
`CuiDesigner/DesignerModel`。`CuiDesigner/DesignerModel/DesignCodeGenerationService.cpp` 负责 AOT 入口和
调度，DataGrid 的具体静态 lowering 在 `CuiDesigner/CodeGenerator.cpp` 中实现。

## 三个列投影

DataGrid 对同一批 `DataGridColumn` 维护三种顺序：

1. **逻辑顺序**保存集合插入顺序。
2. **显示顺序**包含全部列，并提供 `DisplayIndex` 语义。
3. **可见顺序**只包含 `Visibility::Visible` 的列，供布局、导航和 UIA 使用。

投影变化时可以执行一次 `O(C)` 重建；滚动、布局、命中测试和键盘导航不能重复扫描或排序全部列。
列对象的地址和身份在重排时保持稳定，CurrentCell、选择和 UIA 节点都依赖这一点。

Hidden 与 Collapsed 列不进入可见投影，但仍保留逻辑位置和 DisplayIndex。重新显示一列时恢复其身份，
不保留隐藏期间的 Cell 或 Header 容器。

## 虚拟化与冻结

普通滚动、滚动条拖动、布局和命中测试不得扫描总行数。常驻状态应限制在：

```text
O(列数 + 已实现行数 × 已实现列数 + 稀疏选择/覆盖状态)
```

全选和大范围 Cell 选择使用区间表达，不展开成 `行数 × 列数`。未实现的行或单元格不能拥有容器级状态。

冻结区和滚动区是同一数据模型的两个视觉投影。它们共享列、绑定、宽度、排序和选择身份，不复制单元格树。
水平滚动只更新已有变换和连续列带；PointerMove 与滚动热路径不应产生逐帧堆分配。

Auto 与 SizeToCells 的内容采样只访问已经实现的行列。中间未显示的列不能因为冻结或滚动而触发数据源扫描。

## 滚动渲染

保留场景（`PresentationScene`）对普通内容段的节点做刚性平移复用：滚动或虚拟化行重定基只改变节点
局部到渲染变换的平移分量时，直接以位移回放已录制的命令列表，不再调用 `OnRender`。前提是录制时和
回放时所有祖先的 `ClipsChildren` 矩形裁剪都没有裁到该节点；视口边缘被裁的行、带显式 `Clip` 几何的
节点（例如水平滚动中被冻结列遮挡的单元格）仍按原路径重新录制。结构重建（行实现/回收）会保留同一
视觉父级下存活节点的命令列表。

因此行和单元格模板不应无必要地设置 `Clip`；需要裁剪时优先用 `ClipToBounds`（轴对齐时录制为廉价
的 axis-aligned clip，而不是 D2D layer）。帧统计中的 `CommandTranslatedNodes`、
`CommandCacheRetainedNodes` 和 `CommandRecordedNodes` 可用于确认滚动帧只录制新进入视口的行。

## 编辑与数据源

`IBindingList` 保持只读访问与通知的最小接口。新增、删除和可回滚行编辑通过可选接口发现；数据源不支持时，
DataGrid 只能提供它实际能够兑现的行为。

同一时间只保留活动行的编辑事务。验证也只汇总活动项、相关 Binding 和已实现容器，不遍历整个数据源。
用户调整的行高、行级 RowDetails 或 selector 覆盖按 item occurrence 稀疏保存；重复对象引用不能共享一份
错误的覆盖状态。

事件处理器可能重入、替换 ItemsSource 或销毁所有者。提交前应重新验证稳定身份；失败后恢复选择、
CurrentCell、编辑状态和已经发布的容器关系。

## Design 与 Production

动态 XAML 可以在运行时发现属性并触发 `AutoGeneratingColumn`。Production AOT 不恢复这条反射路径；
自动列定制由 `CuiDataGridAutoColumns` catalog 在代码生成阶段完成，最终仍生成普通的静态列构造代码。

Binding 计划按列共享。只有已经实现的 Cell 才创建端点和观察连接。ElementName 与 FindAncestor 只接受
代码生成器能够验证并降为静态 endpoint 的子集；不允许在 Production 中回退到属性名或路径字符串查找。

应用提供的 template/style selector 是 C++ 对象。生成树建立后由宿主挂接，DataGrid 不按名称实例化应用类型，
也不为每一行复制 selector。

## 修改后的检查

从仓库根目录先运行 DataGrid 相关 Core case，再跑完整测试；只跑过滤集不能代替全量回归。

```powershell
$env:CUI_TEST_FILTER = 'DataGrid'
.\x64\Debug\CUICoreTests.exe
Remove-Item Env:CUI_TEST_FILTER
.\x64\Debug\CUICoreTests.exe
```

涉及 XAML、主题或生成代码的修改还需要运行 `CUITest` 的四个无交互模式，并重建
`CuiAotCompileGate`。涉及虚拟化热路径时，除耗时外还应检查已实现行列数、布局次数、数据源读取次数和分配次数。
