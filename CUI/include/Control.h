#ifndef CUI_CONTROL_H_INCLUDED
#define CUI_CONTROL_H_INCLUDED
#pragma once
#define NOMINMAX
#include "Binding.h"
#include "CuiBuildFeatures.h"
#include "DependencyProperty.h"
#include "Event.h"
#include "RuntimeTypeMetadata.h"
#if CUI_ENABLE_DYNAMIC_XAML
#include "XamlSchema.h"
#include "ComponentBehavior.h"
#endif
#include "ControlTemplate.h"
#include "ObservableCollection.h"
#include "ThemePalette.h"
#include "Brush.h"
#include "Geometry.h"
#include "Transform.h"
#include "FrameworkElement.h"
#include "InputReport.h"
#include "AutomationPeer.h"
#include <Font.h>
#include <Factory.h>
#include <Graphics.h>


#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <wrl/client.h>
#include "Layout/LayoutTypes.h"
#include "Layout/LayoutState.h"
#include "Layout/LayoutDeferral.h"

struct ID2D1Bitmap;
struct IDCompositionVisual;

namespace cui::framework
{
	class ReverseInheritedProperty;
	enum class ReverseInheritedPropertyKind : unsigned char;
}

/**
 * @file Control.h
 * @brief CUI 控件基类及基础事件/枚举定义。
 *
 * 约定：
 * - UI 对象通常具有线程亲和性（应在创建它的 UI 线程访问/更新）。
 * - 资源所有权需显式：字体和派生控件资源由各自的语义所有者管理。
 * - 布局相关属性（Canvas offsets/Margin/Padding/Grid/Dock/Min/Max）由布局引擎与容器协同使用。
 */

/**
 * @brief 运行时 UI 类型标识，用于 RTTI/序列化/设计器等场景。
 */
enum class UIClass : int
{
	/** Internal wildcard/root used by routed infrastructure; never a XAML type. */
	UI_Base,
	/** Abstract WPF FrameworkElement type identity. */
	UI_FrameworkElement,
	/** Abstract WPF Control type identity and native component behavior host. */
	UI_Control,
	UI_Label,
	/** Non-authored semantic base for all push-button controls. */
	UI_ButtonBase,
	/** Non-authored semantic base that owns IsChecked and routed state events. */
	UI_ToggleButton,
	/** Non-authored semantic base that owns Minimum/Maximum/Value. */
	UI_RangeBase,
	UI_Button,
	UI_Image,
	/** Abstract WPF editable-text behavior base. */
	UI_TextBoxBase,
	UI_TextBox,
	UI_RichTextBox,
	UI_PasswordBox,
	UI_ComboBox,
	UI_ListView,
	UI_ListBox,
	UI_CheckBox,
	UI_RadioButton,
	UI_ProgressBar,
	UI_LoadingRing,
	UI_ProgressRing,
	UI_TreeView,
	/** Non-instantiable WPF Panel type shared by panel behavior hosts. */
	UI_Panel,
	/** Non-instantiable single-child FrameworkElement base. */
	UI_Decorator,
	/** WPF decorator that owns Background/BorderBrush/BorderThickness/Padding. */
	UI_Border,
	UI_Canvas,
	UI_GroupBox,
	UI_ScrollViewer,
	/** WPF FrameworkElement-level transient presentation with one Child slot. */
	UI_Popup,
	UI_TabItem,
	UI_TabControl,
	UI_Switch,
	UI_Menu,
	UI_MenuItem,
	UI_Separator,
	UI_ToolBar,
	UI_StatusBar,
	/** Generated content container for StatusBar records. */
	UI_StatusBarItem,
	UI_Slider,
	UI_WebBrowser,
	UI_MediaElement,
	UI_StackPanel,
	UI_Grid,
	UI_DockPanel,
	UI_WrapPanel,
	UI_RelativePanel,
	UI_ContextMenu,
	UI_ChartView,
	/** Compatibility surface retained for existing CalendarView XAML. */
	UI_CalendarView,
	UI_NumericUpDown,
	UI_Expander,
	UI_NativeSurface,
	UI_ItemsControl,
	/** Non-authored semantic base with distinct Header and Items slots. */
	UI_HeaderedItemsControl,
	/** Non-authored semantic base for selectable ItemsControl derivatives. */
	UI_Selector,
	/** Generated item container for Selector controls; not an authored tree node. */
	UI_ListBoxItem,
	/** Generated item container for ListView controls. */
	UI_ListViewItem,
	/** Generated item container for ComboBox controls; not an authored tree node. */
	UI_ComboBoxItem,
	/** Authored or generated hierarchical item container for TreeView controls. */
	UI_TreeViewItem,
	UI_ContentPresenter,
	UI_ContentControl,
	/** Non-authored semantic base with distinct Header and Content slots. */
	UI_HeaderedContentControl,
	/** ControlTemplate slot that receives an ItemsControl's generated ItemsHost. */
	UI_ItemsPresenter,
	/** Top-level FrameworkElement projected through PlatformWindowHost. */
	UI_Window,
	/** Existing custom-control sentinel; its numeric identity is compatibility data. */
	UI_CUSTOM,
	/** WPF Calendar surface backed by the retained calendar behavior host. */
	UI_Calendar,
	/** WPF date-entry control composed from TextBox, Button, Popup and Calendar. */
	UI_DatePicker,
	/** WPF tabular selector backed by ListBox item virtualization. */
	UI_DataGrid,
	/** Generated DataGrid row container. */
	UI_DataGridRow,
	/** Generated DataGrid cell content container. */
	UI_DataGridCell,
	/** Clickable DataGrid column header. */
	UI_DataGridColumnHeader,
	/** Structural presenter for DataGrid column headers. */
	UI_DataGridColumnHeadersPresenter,
	/** Clickable DataGrid row header. */
	UI_DataGridRowHeader,
	/** Inclusive bound for internal UIClass enumeration and validation. */
	UI_Last = UI_DataGridRowHeader
};

/** Returns the nearest framework base represented by UIClass. */
UIClass GetUIClassBase(UIClass type) noexcept;

/** True when baseType is type itself or one of its represented bases. */
bool IsUIClassAssignableFrom(UIClass baseType, UIClass type) noexcept;

/** True for runtime types that correspond to WPF Control rather than a
 *  structural FrameworkElement such as Panel, Popup or a presenter. */
bool IsControlTemplateHostClass(UIClass type) noexcept;

/** Canonical generated container type for an ItemsControl family. */
UIClass GetDefaultItemContainerType(UIClass itemsControlType) noexcept;

/** Zero for an exact match, positive for a base, and -1 when unrelated. */
int GetUIClassInheritanceDistance(UIClass baseType, UIClass type) noexcept;

/**
 * Returns whether native metadata is part of the public XAML surface for the
 * represented WPF type. This deliberately follows UIClass ancestry rather
 * than the private C++ behavior-host implementation graph.
 */
bool IsNativePropertySupportedByUIClass(
	UIClass type,
	const DependencyPropertyMetadata& metadata) noexcept;

/** Stable, value-only accessibility view used by native bridges and tests. */
struct AccessibilitySnapshot
{
	AutomationControlType ControlType = AutomationControlType::Custom;
	std::wstring Name;
	std::wstring Description;
	std::wstring HelpText;
	std::wstring Value;
	std::wstring AutomationId;
	std::wstring KeyboardShortcut;
	bool Enabled = true;
	bool Visible = true;
	bool Focusable = false;
	bool Focused = false;
	bool Checked = false;
	bool Selected = false;
	bool Password = false;
	bool ReadOnly = false;
};

enum class AccessibilityChange : uint8_t
{
	Name,
	Description,
	Help,
	Value,
	State,
	Focus,
	Structure,
	Invoke,
	Toggle,
	ExpandCollapse,
	Selection,
	SelectionAdded,
	SelectionRemoved,
	SelectionSelected,
	Scroll
};

/** Allocates an identity that remains unique for the lifetime of the process. */
uint32_t AllocateAccessibilityVirtualId() noexcept;

enum class AccessibilitySelectionAction : uint8_t
{
	Select,
	Add,
	Remove
};

/** UI Automation-compatible logical scroll increments without COM dependencies. */
enum class AccessibilityScrollAmount : uint8_t
{
	LargeDecrement,
	SmallDecrement,
	NoAmount,
	LargeIncrement,
	SmallIncrement
};

inline constexpr double AccessibilityScrollNoChange = -1.0;

/** Percentage-based snapshot used by native and test accessibility adapters. */
struct AccessibilityScrollInfo
{
	bool HorizontallyScrollable = false;
	double HorizontalScrollPercent = AccessibilityScrollNoChange;
	double HorizontalViewSize = 100.0;
	bool VerticallyScrollable = false;
	double VerticalScrollPercent = AccessibilityScrollNoChange;
	double VerticalViewSize = 100.0;
};

/** Value-only snapshot for an item rendered inside a Control rather than owned as a child. */
struct AccessibilityVirtualNode
{
	uint32_t Id = 0;
	uint32_t ParentId = 0;
	AutomationControlType ControlType = AutomationControlType::ListItem;
	AutomationPattern Patterns = AutomationPattern::None;
	std::wstring Name;
	std::wstring Description;
	std::wstring Value;
	std::wstring AutomationId;
	// TableItem.RowHeaderItems is an association, not an index into the
	// root Table.RowHeaders result. Zero means no realized row header exists.
	uint32_t RowHeaderId = 0;
	std::wstring ClassName;
	D2D1_RECT_F BoundsDip{ 0, 0, 0, 0 };
	// Most virtual nodes use owner-local DIPs. Transient presentation roots,
	// such as ComboBox popup items, can instead publish window render-space
	// DIPs so the UIA bridge does not apply the owner's RenderTransform twice.
	bool BoundsAreRenderSpace = false;
	bool Enabled = true;
	bool Visible = true;
	bool KeyboardFocusable = false;
	bool HasKeyboardFocus = false;
	bool IsControlElement = true;
	bool IsContentElement = true;
	bool SelectionReturnsNullWhenEmpty = false;
	bool Selected = false;
	bool Checked = false;
	bool ReadOnly = true;
	bool Expanded = false;
	int Row = -1;
	int Column = -1;
	int RowSpan = 1;
	int ColumnSpan = 1;
	int Level = 0;
};

struct AccessibilityVirtualContainerInfo
{
	AutomationPattern Patterns = AutomationPattern::None;
	bool CanSelectMultiple = false;
	bool IsSelectionRequired = false;
	int RowCount = 0;
	int ColumnCount = 0;
};

/**
 * @brief 光标类型。
 */
enum class CursorKind : uint8_t
{
	Arrow,
	Cross,
	Hand,
	IBeam,
	SizeWE,
	SizeNS,
	SizeNWSE,
	SizeNESW,
	SizeAll,
	No,
	/** Nullable/default WPF Cursor projection; native behavior chooses the shape. */
	Auto
};

/**
 * @brief 控件通用事件回调类型别名。
 *
 * sender 一般为触发事件的控件指针。
 */
typedef Event<void(class Control*, EventArgs)> EventHandler;

/**
 * Stable payload used by events declared in XAML component contracts.
 * The framework intentionally exposes a value object instead of a C++ member
 * type so dynamic documents never need reflection into application classes.
 */
struct DeclarativeEventArgs : RoutedEventArgs
{
	/** Stable AOT identity; production dispatch never compares event names. */
	const DeclarativeEventDefinition* Definition = nullptr;
	BindingValue Value;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Design/runtime-XAML compatibility sidecar. */
	std::wstring Name;
	RuntimeTypeId OwnerType;
#endif
};

typedef Event<void(class Control*, DeclarativeEventArgs&)>
	DeclarativeEvent;

/**
 * Stable, compiled identity for one name in a ControlTemplate namescope.
 *
 * Zero is reserved for "no part" / the template owner.  Compiled code hashes
 * authored names once and the production runtime retains only this token and
 * the resolved pointer.
 */
struct TemplatePartToken final
{
	uint64_t Value = 0;

	constexpr TemplatePartToken() noexcept = default;
	explicit constexpr TemplatePartToken(uint64_t value) noexcept
		: Value(value)
	{
	}

	[[nodiscard]] explicit constexpr operator bool() const noexcept
	{
		return Value != 0;
	}

	friend constexpr bool operator==(
		TemplatePartToken left, TemplatePartToken right) noexcept = default;
};

/** Stable FNV-1a identity over the wchar_t code units emitted by the AOT compiler. */
[[nodiscard]] constexpr TemplatePartToken MakeTemplatePartToken(
	std::wstring_view localName) noexcept
{
	if (localName.empty()) return {};
	uint64_t hash = 14695981039346656037ull;
	for (const auto codeUnit : localName)
	{
		hash ^= static_cast<uint64_t>(codeUnit);
		hash *= 1099511628211ull;
	}
	// Token zero is the template owner.  The design/compiler collision gate
	// still compares the original names for every non-empty token.
	return TemplatePartToken(hash == 0 ? 1 : hash);
}

/**
 * Stable, name-free identity for one property declared by a compiled
 * ComponentDefinition.
 *
 * Tokens are scoped to the generated component type.  The AOT compiler
 * rejects collisions inside each component before emitting the direct
 * token-to-metadata dispatch table.
 */
struct ComponentPropertyToken final
{
	std::uint64_t Value = 0;

	constexpr ComponentPropertyToken() noexcept = default;
	explicit constexpr ComponentPropertyToken(std::uint64_t value) noexcept
		: Value(value)
	{
	}

	[[nodiscard]] explicit constexpr operator bool() const noexcept
	{
		return Value != 0;
	}

	friend constexpr bool operator==(
		ComponentPropertyToken left,
		ComponentPropertyToken right) noexcept = default;
};

/** 64-bit FNV-1a over stable UTF-32 code units. */
[[nodiscard]] constexpr ComponentPropertyToken MakeComponentPropertyToken(
	std::wstring_view propertyName) noexcept
{
	if (propertyName.empty()) return {};
	std::uint64_t hash = 14695981039346656037ull;
	for (const wchar_t character : propertyName)
	{
		const auto codeUnit = static_cast<std::uint32_t>(character);
		for (unsigned shift = 0; shift != 32; shift += 8)
		{
			hash ^= static_cast<std::uint8_t>(codeUnit >> shift);
			hash *= 1099511628211ull;
		}
	}
	return ComponentPropertyToken(hash == 0 ? 1ull : hash);
}

/** Strong process-stable identities emitted for compiled visual-state programs. */
struct VisualStateGroupToken final
{
	uint64_t Value = 0;

	constexpr VisualStateGroupToken() noexcept = default;
	explicit constexpr VisualStateGroupToken(uint64_t value) noexcept
		: Value(value)
	{
	}
	[[nodiscard]] explicit constexpr operator bool() const noexcept
	{
		return Value != 0;
	}
	friend constexpr bool operator==(
		VisualStateGroupToken left,
		VisualStateGroupToken right) noexcept = default;
};

struct VisualStateToken final
{
	uint64_t Value = 0;

	constexpr VisualStateToken() noexcept = default;
	explicit constexpr VisualStateToken(uint64_t value) noexcept
		: Value(value)
	{
	}
	[[nodiscard]] explicit constexpr operator bool() const noexcept
	{
		return Value != 0;
	}
	friend constexpr bool operator==(
		VisualStateToken left, VisualStateToken right) noexcept = default;
};

[[nodiscard]] constexpr uint64_t MakeCompiledInteractionNameToken(
	std::wstring_view name) noexcept
{
	uint64_t hash = 14695981039346656037ULL;
	for (const auto character : name)
	{
		const auto codeUnit = static_cast<uint32_t>(character);
		for (unsigned int byte = 0; byte < sizeof(wchar_t); ++byte)
		{
			hash ^= static_cast<unsigned char>(codeUnit >> (byte * 8));
			hash *= 1099511628211ULL;
		}
	}
	return hash == 0 ? 1 : hash;
}

[[nodiscard]] constexpr VisualStateGroupToken MakeVisualStateGroupToken(
	std::wstring_view name) noexcept
{
	return VisualStateGroupToken(MakeCompiledInteractionNameToken(name));
}

[[nodiscard]] constexpr VisualStateToken MakeVisualStateToken(
	std::wstring_view name) noexcept
{
	return VisualStateToken(MakeCompiledInteractionNameToken(name));
}

#if CUI_ENABLE_DYNAMIC_XAML
/** Design-only predicate accepted by the dynamic-XAML interaction adapter. */
struct DeclarativeVisualStateCondition
{
	DependencyPropertyReference Property;
	BindingValue Value;

	DeclarativeVisualStateCondition() = default;
#if CUI_ENABLE_DYNAMIC_XAML
	DeclarativeVisualStateCondition(
		std::wstring propertyName, BindingValue value)
		: Property(DependencyPropertyReference(std::move(propertyName))),
		  Value(std::move(value))
	{
	}
#endif
	DeclarativeVisualStateCondition(
		const DependencyProperty& property, BindingValue value)
		: Property(DependencyPropertyReference(property)),
		  Value(std::move(value))
	{
	}
	DeclarativeVisualStateCondition(
		DependencyPropertyReference property, BindingValue value)
		: Property(std::move(property)),
		  Value(std::move(value))
	{
	}
};

/** One property value applied to the component host or a named template part. */
struct DeclarativeVisualStateSetter
{
	/** Null targets the component host; otherwise this is the resolved template part. */
	Control* Target = nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic-XAML compatibility sidecar; compiled definitions never retain it. */
	std::wstring TargetName;
#endif
	DependencyPropertyReference Property;
	BindingValue Value;

	DeclarativeVisualStateSetter() = default;
#if CUI_ENABLE_DYNAMIC_XAML
	DeclarativeVisualStateSetter(
		std::wstring targetName,
		std::wstring propertyName,
		BindingValue value)
		: TargetName(std::move(targetName)),
		  Property(DependencyPropertyReference(std::move(propertyName))),
		  Value(std::move(value))
	{
	}
	DeclarativeVisualStateSetter(
		std::wstring targetName,
		const DependencyProperty& property,
		BindingValue value)
		: TargetName(std::move(targetName)),
		  Property(DependencyPropertyReference(property)),
		  Value(std::move(value))
	{
	}
	DeclarativeVisualStateSetter(
		std::wstring targetName,
		DependencyPropertyReference property,
		BindingValue value)
		: TargetName(std::move(targetName)),
		  Property(std::move(property)),
		  Value(std::move(value))
	{
	}
#endif
	DeclarativeVisualStateSetter(
		Control* target,
		DependencyPropertyReference property,
		BindingValue value)
		: Target(target),
		  Property(std::move(property)),
		  Value(std::move(value))
	{
	}
	DeclarativeVisualStateSetter(
		Control* target,
		const DependencyProperty& property,
		BindingValue value)
		: Target(target),
		  Property(DependencyPropertyReference(property)),
		  Value(std::move(value))
	{
	}
};
#endif

enum class DeclarativeAnimationKind : unsigned char
{
	Double,
	Color,
	Thickness,
	Point,
	Vector,
	Rect,
	Size,
	Matrix,
	Object,
	Int32,
	Int64,
	Single,
};

enum class DeclarativePathAnimationSource : unsigned char
{
	X,
	Y,
	Angle,
};

enum class DeclarativePathSegmentKind : unsigned char
{
	Move,
	Line,
	CubicBezier,
};

/** One source-ordered segment in a WPF path-animation figure. */
struct DeclarativePathAnimationSegment final
{
	DeclarativePathSegmentKind Kind = DeclarativePathSegmentKind::Line;
	/** Move/Line use Point3 as destination/endpoint; cubic uses all three points. */
	cui::core::Point Point1;
	cui::core::Point Point2;
	cui::core::Point Point3;

	bool operator==(const DeclarativePathAnimationSegment&) const = default;
};

/**
 * Output-kind-orthogonal WPF path metadata. Segment storage is owned by the
 * dynamic definition or addressed through a compiled range, so this header
 * stays fixed-size and trivially copyable.
 */
struct DeclarativePathAnimationHeader final
{
	bool Enabled = false;
	cui::core::Point Start;
	DeclarativePathAnimationSource Source =
		DeclarativePathAnimationSource::X;
	bool DoesRotateWithTangent = false;
	bool IsOffsetCumulative = false;
	bool IsAngleCumulative = false;

	bool operator==(const DeclarativePathAnimationHeader&) const = default;
};

enum class DeclarativeEasingKind : unsigned char
{
	Linear,
	Quadratic,
	Cubic,
	Sine,
	Back,
	Bounce,
	Circle,
	Elastic,
	Exponential,
	Power,
	Quartic,
	Quintic,
};

enum class DeclarativeEasingMode : unsigned char
{
	EaseIn,
	EaseOut,
	EaseInOut,
};

enum class DeclarativeKeyFrameKind : unsigned char
{
	Discrete,
	Linear,
	Easing,
	Spline,
};

/** WPF RepeatBehavior discriminator for one animation Timeline. */
enum class DeclarativeRepeatBehaviorKind : unsigned char
{
	Count,
	Duration,
	Forever,
};

enum class DeclarativeTimelineFillBehavior : unsigned char
{
	HoldEnd,
	Stop,
};

/**
 * Compact type-specific easing payload. Primary is Amplitude, Bounciness,
 * Springiness, Exponent, or Power. Secondary is Bounces or Oscillations.
 */
struct DeclarativeEasingParameters final
{
	double Primary = 0.0;
	double Secondary = 0.0;
};

/** Authored timing on a Storyboard/ParallelTimeline root. */
struct DeclarativeStoryboardTimingDefinition final
{
	unsigned long long BeginTimeMilliseconds = 0;
	/** True preserves WPF's ParallelTimeline natural-duration resolution. */
	bool DurationAutomatic = true;
	unsigned long long DurationMilliseconds = 0;
	DeclarativeRepeatBehaviorKind RepeatBehavior =
		DeclarativeRepeatBehaviorKind::Count;
	double RepeatCount = 1.0;
	unsigned long long RepeatDurationMilliseconds = 0;
	bool AutoReverse = false;
	DeclarativeTimelineFillBehavior FillBehavior =
		DeclarativeTimelineFillBehavior::HoldEnd;
	double SpeedRatio = 1.0;
	double AccelerationRatio = 0.0;
	double DecelerationRatio = 0.0;
};

#if CUI_ENABLE_DYNAMIC_XAML
/** Design-only key-frame definition accepted by the dynamic-XAML adapter. */
struct DeclarativeAnimationKeyFrame
{
	DeclarativeKeyFrameKind Kind = DeclarativeKeyFrameKind::Linear;
	unsigned long long KeyTimeMilliseconds = 0;
	BindingValue Value;
	DeclarativeEasingKind Easing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode EasingMode = DeclarativeEasingMode::EaseOut;
	/** Cubic Bezier control points used only by Spline key frames. */
	float KeySplineX1 = 0.0f;
	float KeySplineY1 = 0.0f;
	float KeySplineX2 = 1.0f;
	float KeySplineY2 = 1.0f;
	uint16_t KeyTimeSubMillisecondTicks = 0;
	DeclarativeEasingParameters EasingParameters;
};

/** One finite Timeline in a VisualState Storyboard. */
struct DeclarativeVisualStateAnimation
{
	DeclarativeAnimationKind Kind = DeclarativeAnimationKind::Double;
	/** Null targets the component host; otherwise this is the resolved template part. */
	Control* Target = nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic-XAML compatibility sidecar; compiled definitions never retain it. */
	std::wstring TargetName;
#endif
	/** Direct dependency-property operand used by compiled documents. */
	DependencyPropertyReference Property;
	/** Non-empty only for a multi-segment WPF Storyboard.TargetProperty path. */
	std::wstring ObjectPath;
	/** WPF From/To/By endpoints; missing values use the current/base animation inputs. */
	std::optional<BindingValue> From;
	std::optional<BindingValue> To;
	std::optional<BindingValue> By;
	/** Adds the timeline's local output to its default origin when WPF does so. */
	bool IsAdditive = false;
	/** Accumulates the timeline's iteration delta across repeat cycles. */
	bool IsCumulative = false;
	DeclarativePathAnimationHeader Path;
	std::vector<DeclarativePathAnimationSegment> PathSegments;
	unsigned long long BeginTimeMilliseconds = 0;
	unsigned long long DurationMilliseconds = 0;
	DeclarativeRepeatBehaviorKind RepeatBehavior =
		DeclarativeRepeatBehaviorKind::Count;
	/** May be fractional; the WPF default is one repetition. */
	double RepeatCount = 1.0;
	unsigned long long RepeatDurationMilliseconds = 0;
	bool AutoReverse = false;
	DeclarativeTimelineFillBehavior FillBehavior =
		DeclarativeTimelineFillBehavior::HoldEnd;
	/** Timeline-local speed; BeginTime and duration-form RepeatBehavior stay in parent time. */
	double SpeedRatio = 1.0;
	/** Fractions of one simple duration; their sum may not exceed one. */
	double AccelerationRatio = 0.0;
	double DecelerationRatio = 0.0;
	DeclarativeEasingKind Easing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode EasingMode = DeclarativeEasingMode::EaseOut;
	DeclarativeEasingParameters EasingParameters;
	/** Non-empty selects the corresponding UsingKeyFrames timeline. */
	std::vector<DeclarativeAnimationKeyFrame> KeyFrames;

	[[nodiscard]] const std::wstring& PropertyPath() const noexcept
	{
		return ObjectPath.empty() ? Property.Name() : ObjectPath;
	}
};

/** Design-only nested ParallelTimeline definition. */
struct DeclarativeTimelineGroupDefinition
{
	DeclarativeStoryboardTimingDefinition Timing;
	std::vector<DeclarativeVisualStateAnimation> Animations;
	std::vector<DeclarativeTimelineGroupDefinition> Children;
};
#endif

/** WPF-style action executed by a component EventTrigger. */
enum class DeclarativeStoryboardActionKind : unsigned char
{
	Begin,
	Pause,
	Resume,
	Stop,
	Remove,
	Seek,
	SetSpeedRatio,
	SkipToFill,
};

enum class DeclarativeHandoffBehavior : unsigned char
{
	SnapshotAndReplace,
	Compose,
};

enum class DeclarativeClockState : unsigned char
{
	Active,
	Filling,
	Stopped,
};

struct DeclarativeClockObservation final
{
	DeclarativeClockState State = DeclarativeClockState::Stopped;
	std::optional<unsigned long long> CurrentTimeMilliseconds;
	std::optional<double> Progress;
	std::optional<double> GlobalSpeed;

	bool operator==(const DeclarativeClockObservation&) const = default;
};

enum class DeclarativeClockTimingEventKind : unsigned char
{
	CurrentTimeInvalidated,
	CurrentGlobalSpeedInvalidated,
	CurrentStateInvalidated,
	Completed,
	RemoveRequested,
};

enum class DeclarativeClockOwnerKind : unsigned char
{
	VisualStateGroup,
	ComponentStoryboard,
	StyleTrigger,
	CompiledInteractionStoryboard,
};

struct DeclarativeClockTimingEventArgs final
{
	DeclarativeClockTimingEventKind Kind =
		DeclarativeClockTimingEventKind::CurrentTimeInvalidated;
	/** Unique for one Begin/restart lifetime on this Control. */
	uint64_t ClockInstanceId = 0;
	DeclarativeClockOwnerKind OwnerKind =
		DeclarativeClockOwnerKind::VisualStateGroup;
	uint64_t OwnerScopeId = 0;
	uint64_t StoryboardDefinitionId = 0;
};

typedef Event<void(class Control*, const DeclarativeClockTimingEventArgs&)>
	DeclarativeClockTimingEvent;

#if CUI_ENABLE_DYNAMIC_XAML
struct DeclarativeEventTriggerActionDefinition
{
	DeclarativeStoryboardActionKind Kind =
		DeclarativeStoryboardActionKind::Begin;
	/** BeginStoryboard x:Name, or the referenced BeginStoryboardName. */
	std::wstring StoryboardName;
	/** Populated only for BeginStoryboard. */
	std::vector<DeclarativeVisualStateAnimation> Animations;
	std::vector<DeclarativeTimelineGroupDefinition> TimelineGroups;
	DeclarativeStoryboardTimingDefinition StoryboardTiming;
	DeclarativeHandoffBehavior Handoff =
		DeclarativeHandoffBehavior::SnapshotAndReplace;
	/** SeekStoryboard offset relative to the controllable root BeginTime. */
	unsigned long long SeekOffsetMilliseconds = 0;
	double SpeedRatio = 1.0;
};

/** Design-only EventTrigger graph accepted by the dynamic-XAML adapter. */
struct DeclarativeEventTriggerDefinition
{
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring EventName;
#endif
	/** Exactly one of Event and RoutedEvent is populated by compiled code. */
	const DeclarativeEventDefinition* Event = nullptr;
	RoutedEventId RoutedEvent = RoutedEventId::None;
	std::vector<DeclarativeEventTriggerActionDefinition> Actions;
};

/** One mutually exclusive state inside a declarative visual-state group. */
struct DeclarativeVisualStateDefinition
{
	std::wstring Name;
	/** All conditions must match. Empty conditions and events identify the fallback state. */
	std::vector<DeclarativeVisualStateCondition> Conditions;
	/** A matching component-owned routed event enters this state. */
	std::vector<const DeclarativeEventDefinition*> Events;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Design/runtime-XAML compatibility sidecar resolved once during Build. */
	std::vector<std::wstring> EventNames;
#endif
	std::vector<DeclarativeVisualStateSetter> Setters;
	std::vector<DeclarativeVisualStateAnimation> Animations;
	std::vector<DeclarativeTimelineGroupDefinition> TimelineGroups;
	DeclarativeStoryboardTimingDefinition StoryboardTiming;
};

/** One WPF-style route between states in the same visual-state group. */
struct DeclarativeVisualTransitionDefinition
{
	/** Empty means any source/target state. */
	std::wstring FromState;
	std::wstring ToState;
	unsigned long long GeneratedDurationMilliseconds = 0;
	DeclarativeEasingKind GeneratedEasing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode GeneratedEasingMode = DeclarativeEasingMode::EaseOut;
	DeclarativeEasingParameters GeneratedEasingParameters;
	/** Explicit transition timelines override generated timelines per target. */
	std::vector<DeclarativeVisualStateAnimation> Animations;
	std::vector<DeclarativeTimelineGroupDefinition> TimelineGroups;
	DeclarativeStoryboardTimingDefinition StoryboardTiming;
};

struct DeclarativeVisualStateGroupDefinition
{
	std::wstring Name;
	std::vector<DeclarativeVisualStateDefinition> States;
	std::vector<DeclarativeVisualTransitionDefinition> Transitions;
};
#endif

inline constexpr uint32_t CompiledInteractionInvalidIndex = UINT32_MAX;

struct CompiledInteractionRange final
{
	uint32_t Offset = 0;
	uint32_t Count = 0;
};

/** String-free adapter selected by the AOT compiler for a complex property path. */
enum class CompiledStoryboardObjectPathKind : uint8_t
{
	Transform,
	Geometry,
	PathGeometry,
	GeometryTransform,
	Brush,
	BrushTransform,
};

enum class CompiledStoryboardObjectPathMember : uint8_t
{
	TransformX,
	TransformY,
	TransformScaleX,
	TransformScaleY,
	TransformAngle,
	TransformAngleX,
	TransformAngleY,
	TransformCenterX,
	TransformCenterY,
	TransformMatrix,
	GeometryRect,
	GeometryCenter,
	GeometryRadiusX,
	GeometryRadiusY,
	GeometryFillRule,
	PathFigureStartPoint,
	PathFigureIsClosed,
	PathFigureIsFilled,
	PathSegmentPoint,
	PathSegmentPoint1,
	PathSegmentPoint2,
	PathSegmentPoint3,
	PathArcSize,
	PathArcRotationAngle,
	PathArcIsLargeArc,
	PathArcSweepDirection,
	BrushSolidColor,
	BrushOpacity,
	BrushStartPoint,
	BrushEndPoint,
	BrushCenter,
	BrushGradientOrigin,
	BrushRadiusX,
	BrushRadiusY,
	BrushGradientStopColor,
	BrushGradientStopOffset,
};

enum class CompiledStoryboardObjectPathFlags : uint8_t
{
	None = 0,
	RelativeTransform = 1u << 0,
	HasPathSegment = 1u << 1,
	DirectTransform = 1u << 2,
};

constexpr CompiledStoryboardObjectPathFlags operator|(
	CompiledStoryboardObjectPathFlags left,
	CompiledStoryboardObjectPathFlags right) noexcept
{
	return static_cast<CompiledStoryboardObjectPathFlags>(
		static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr bool HasCompiledStoryboardObjectPathFlag(
	CompiledStoryboardObjectPathFlags value,
	CompiledStoryboardObjectPathFlags flag) noexcept
{
	return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

struct CompiledStoryboardObjectPathOp final
{
	CompiledStoryboardObjectPathKind Kind =
		CompiledStoryboardObjectPathKind::Transform;
	CompiledStoryboardObjectPathMember Member =
		CompiledStoryboardObjectPathMember::TransformX;
	/** Numeric drawing kind, or UINT8_MAX when the canonical owner is generic. */
	uint8_t ExpectedObjectKind = UINT8_MAX;
	/** TransformKind for nested transforms; zero when not applicable. */
	uint8_t ExpectedAuxiliaryKind = 0;
	CompiledStoryboardObjectPathFlags Flags =
		CompiledStoryboardObjectPathFlags::None;
	uint8_t Reserved = 0;
	/** Transform operation, gradient-stop, or path-figure index. */
	uint32_t Index0 = 0;
	/** Path-segment index; zero for all other adapters. */
	uint32_t Index1 = 0;
	CompiledInteractionRange ChildIndices;
	/** Stable canonical identity used for same-root ownership checks. */
	uint64_t Identity = 0;
};

struct CompiledInteractionPropertyOperand final
{
	uint32_t TargetSlot = 0;
	DependencyPropertyReference Property;
};

struct CompiledInteractionConditionOp final
{
	uint32_t OperandIndex = CompiledInteractionInvalidIndex;
	uint32_t ValueIndex = CompiledInteractionInvalidIndex;
};

struct CompiledInteractionSetterOp final
{
	uint32_t OperandIndex = CompiledInteractionInvalidIndex;
	uint32_t ValueIndex = CompiledInteractionInvalidIndex;
};

struct CompiledInteractionKeyFrameOp final
{
	DeclarativeKeyFrameKind Kind = DeclarativeKeyFrameKind::Linear;
	unsigned long long KeyTimeMilliseconds = 0;
	uint32_t ValueIndex = CompiledInteractionInvalidIndex;
	DeclarativeEasingKind Easing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode EasingMode = DeclarativeEasingMode::EaseOut;
	float KeySplineX1 = 0.0f;
	float KeySplineY1 = 0.0f;
	float KeySplineX2 = 1.0f;
	float KeySplineY2 = 1.0f;
	uint16_t KeyTimeSubMillisecondTicks = 0;
	DeclarativeEasingParameters EasingParameters;
};

struct CompiledInteractionAnimationOp final
{
	DeclarativeAnimationKind Kind = DeclarativeAnimationKind::Double;
	uint32_t OperandIndex = CompiledInteractionInvalidIndex;
	uint32_t ObjectPathIndex = CompiledInteractionInvalidIndex;
	uint32_t FromValueIndex = CompiledInteractionInvalidIndex;
	uint32_t ToValueIndex = CompiledInteractionInvalidIndex;
	uint32_t ByValueIndex = CompiledInteractionInvalidIndex;
	CompiledInteractionRange KeyFrames;
	bool IsAdditive = false;
	bool IsCumulative = false;
	DeclarativePathAnimationHeader Path;
	CompiledInteractionRange PathSegments;
	unsigned long long BeginTimeMilliseconds = 0;
	unsigned long long DurationMilliseconds = 0;
	DeclarativeRepeatBehaviorKind RepeatBehavior =
		DeclarativeRepeatBehaviorKind::Count;
	double RepeatCount = 1.0;
	unsigned long long RepeatDurationMilliseconds = 0;
	bool AutoReverse = false;
	DeclarativeTimelineFillBehavior FillBehavior =
		DeclarativeTimelineFillBehavior::HoldEnd;
	double SpeedRatio = 1.0;
	double AccelerationRatio = 0.0;
	double DecelerationRatio = 0.0;
	DeclarativeEasingKind Easing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode EasingMode = DeclarativeEasingMode::EaseOut;
	DeclarativeEasingParameters EasingParameters;
};

struct CompiledInteractionStateOp final
{
	VisualStateToken Token;
	CompiledInteractionRange Conditions;
	CompiledInteractionRange Events;
	CompiledInteractionRange Setters;
	CompiledInteractionRange Animations;
	CompiledInteractionRange TimelineGroups;
	DeclarativeStoryboardTimingDefinition StoryboardTiming;
};

struct CompiledInteractionTransitionOp final
{
	/** Group-local indexes; Invalid means the WPF wildcard. */
	uint32_t FromStateIndex = CompiledInteractionInvalidIndex;
	uint32_t ToStateIndex = CompiledInteractionInvalidIndex;
	unsigned long long GeneratedDurationMilliseconds = 0;
	DeclarativeEasingKind GeneratedEasing = DeclarativeEasingKind::Linear;
	DeclarativeEasingMode GeneratedEasingMode =
		DeclarativeEasingMode::EaseOut;
	DeclarativeEasingParameters GeneratedEasingParameters;
	CompiledInteractionRange Animations;
	CompiledInteractionRange TimelineGroups;
	DeclarativeStoryboardTimingDefinition StoryboardTiming;
};

struct CompiledInteractionGroupOp final
{
	VisualStateGroupToken Token;
	CompiledInteractionRange States;
	CompiledInteractionRange Transitions;
	uint32_t FallbackStateIndex = CompiledInteractionInvalidIndex;
	/** Indexes into PropertyOperands, pre-grouped for property-change filtering. */
	CompiledInteractionRange ConditionOperands;
};

struct CompiledInteractionStoryboardOp final
{
	CompiledInteractionRange Animations;
	CompiledInteractionRange TimelineGroups;
	DeclarativeStoryboardTimingDefinition Timing;
};

/** One flattened nested ParallelTimeline; child group ranges form a tree. */
struct CompiledInteractionTimelineGroupOp final
{
	CompiledInteractionRange Animations;
	CompiledInteractionRange Children;
	DeclarativeStoryboardTimingDefinition Timing;
};

struct CompiledInteractionActionOp final
{
	DeclarativeStoryboardActionKind Kind =
		DeclarativeStoryboardActionKind::Begin;
	uint32_t StoryboardIndex = CompiledInteractionInvalidIndex;
	DeclarativeHandoffBehavior Handoff =
		DeclarativeHandoffBehavior::SnapshotAndReplace;
	uint64_t SeekOffsetMilliseconds = 0;
	double SpeedRatio = 1.0;
};

struct CompiledInteractionEventTriggerOp final
{
	const DeclarativeEventDefinition* Event = nullptr;
	RoutedEventId RoutedEvent = RoutedEventId::None;
	CompiledInteractionRange Actions;
};

inline constexpr uint32_t CompiledInteractionProgramViewVersion = 16;

/**
 * Immutable, non-owning AOT interaction program. Every referenced structural
 * opcode table must remain valid for the lifetime of each installed template
 * instance; generated programs therefore use process-lifetime static storage.
 * Per-instance values and target slots are supplied separately and need remain
 * valid only for InstallCompiledInteractions. Target slot zero is always the
 * template owner.
 */
struct CompiledInteractionProgramView final
{
	uint32_t Version = CompiledInteractionProgramViewVersion;
	uint32_t TargetCount = 1;
	std::span<const CompiledInteractionPropertyOperand> PropertyOperands;
	std::span<const uint32_t> ObjectPathChildIndices;
	std::span<const CompiledStoryboardObjectPathOp> ObjectPaths;
	std::span<const CompiledInteractionConditionOp> Conditions;
	std::span<const CompiledInteractionSetterOp> Setters;
	std::span<const CompiledInteractionKeyFrameOp> KeyFrames;
	std::span<const DeclarativePathAnimationSegment> PathSegments;
	std::span<const CompiledInteractionAnimationOp> Animations;
	std::span<const CompiledInteractionTimelineGroupOp> TimelineGroups;
	std::span<const DeclarativeEventDefinition* const> StateEvents;
	std::span<const CompiledInteractionStateOp> States;
	std::span<const CompiledInteractionTransitionOp> Transitions;
	std::span<const uint32_t> GroupConditionOperands;
	std::span<const CompiledInteractionGroupOp> Groups;
	std::span<const CompiledInteractionStoryboardOp> Storyboards;
	std::span<const CompiledInteractionActionOp> Actions;
	std::span<const CompiledInteractionEventTriggerOp> EventTriggers;
};

struct DeclarativeVisualStateChangedEventArgs
{
	VisualStateGroupToken Group;
	VisualStateToken OldStateToken;
	VisualStateToken NewStateToken;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring GroupName;
	std::wstring OldState;
	std::wstring NewState;
#endif
};

typedef Event<void(class Control*, const DeclarativeVisualStateChangedEventArgs&)>
	DeclarativeVisualStateChangedEvent;

class ControlStyleSheet;
struct ControlStyleResolution;
#if CUI_ENABLE_DYNAMIC_XAML
struct ControlVisualStateDesignSidecar;
#endif
class Application;
class Control;
class Canvas;
class Grid;
class DockPanel;

namespace cui::framework
{
	struct TemplateAccess;
	struct PresentationAccess;
	struct InputAccess;
	struct NativeVisualStateAccess;
	struct TreeAccess;
	struct XamlAccess;
	struct DependencyPropertyAccess;
	struct StyleAccess;
}

/**
 * @brief 所有可视控件的基类。
 *
 * 控件是轻量对象，主要职责：
 * - 保存通用几何、外观、输入和依赖属性状态
 * - 消费平台无关输入报告（ProcessInput）并触发相应事件
 * - 参与布局：提供 MeasureCore/Arrange，且通过 RequestLayout 通知父容器重排
 *
 */
class Control : public FrameworkElement
{
protected:
	using FrameworkApplication = Application;
#if CUI_ENABLE_DYNAMIC_XAML
	using DeclarativeType = DeclarativeTypeDescriptor;
	using DeclarativeComponentBehavior = IDeclarativeComponentBehavior;
#endif

	/**
	 * Build-time generated component subclasses override these narrow hooks.
	 * They keep token/event/property identity available to normal WPF runtime
	 * services without attaching a dynamically constructed XAML type
	 * descriptor or QName strings to every instance.
	 */
	virtual ComponentTypeToken GetCompiledComponentTypeTokenCore() const noexcept
	{
		return {};
	}
	virtual const DependencyPropertyMetadata*
		FindCompiledComponentPropertyCore(
			ComponentPropertyToken property) const
	{
		(void)property;
		return nullptr;
	}
	virtual bool IsCompiledComponentPropertyCore(
		const DependencyPropertyMetadata& metadata) const noexcept
	{
		(void)metadata;
		return false;
	}

	friend class Visual;
	friend class Expander;
	friend class Canvas;
	friend class Grid;
	friend class DockPanel;
	friend class PresentationScene;
	friend class TextCompositionManager;
	friend class Window;
	friend class cui::framework::ReverseInheritedProperty;
	friend FrameworkApplication;
	friend struct cui::framework::InputAccess;
	friend struct cui::framework::PresentationAccess;
	friend struct cui::framework::NativeVisualStateAccess;
	friend struct cui::framework::TreeAccess;
	friend struct cui::framework::XamlAccess;
	friend struct cui::framework::DependencyPropertyAccess;
	friend struct cui::framework::StyleAccess;
	/** Creates the semantic peer used by every native accessibility bridge. */
	virtual std::unique_ptr<AutomationPeer> OnCreateAutomationPeer();
	void SetPresentationSuppressed(bool value);
	void SetParticipatesInPresentationScene(bool value);
	/** Current frame-only DrawingContext for native control behavior. */
	D2DGraphics* GetDrawingContext() const noexcept;
	/** Draws this element only; PresentationScene owns all tree traversal. */
	virtual void OnRender();
	void BeginRender();
	void BeginRender(float clipW, float clipH);
	void EndRender();
	/**
	 * Invalidates cached geometry for this visual and every retained descendant.
	 * Containers call this when their render-only child offset/transform changes
	 * without a new layout pass.
	 */
	void InvalidateDescendantRenderGeometry() noexcept;
	D2D1_COLOR_F _backcolor = cui::theme::palette::Surface;
	D2D1_COLOR_F _forecolor = cui::theme::palette::TextPrimary;
	D2D1_COLOR_F _bordercolor = cui::theme::palette::Border;
	bool _participatesInPresentationScene = true;
	/** Native-presenter fallback only; never a XAML/application property. */
	PROPERTY(D2D1_COLOR_F, RendererBackgroundColor);
	GET(D2D1_COLOR_F, RendererBackgroundColor);
	SET(D2D1_COLOR_F, RendererBackgroundColor);
	PROPERTY(D2D1_COLOR_F, RendererForegroundColor);
	GET(D2D1_COLOR_F, RendererForegroundColor);
	SET(D2D1_COLOR_F, RendererForegroundColor);
	PROPERTY(D2D1_COLOR_F, RendererBorderColor);
	GET(D2D1_COLOR_F, RendererBorderColor);
	SET(D2D1_COLOR_F, RendererBorderColor);
	static const DependencyPropertyMetadataRegistration&
		BackgroundPropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		ForegroundPropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		FontFamilyPropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		LanguagePropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		FontSizePropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		BorderBrushPropertyMetadataRelation();
	static const DependencyPropertyMetadataRegistration&
		BorderThicknessPropertyMetadataRelation();
	static const DependencyPropertyKey& IsVisiblePropertyKey();
	static const DependencyPropertyKey& ActualWidthPropertyKey();
	static const DependencyPropertyKey& ActualHeightPropertyKey();
	static const DependencyPropertyKey& ValidationHasErrorPropertyKey();
	static const DependencyPropertyKey& ValidationErrorsPropertyKey();
	static const DependencyPropertyKey& IsFocusedPropertyKey();
	static const DependencyPropertyKey& IsKeyboardFocusedPropertyKey();
	static const DependencyPropertyKey& IsKeyboardFocusVisiblePropertyKey();
	static const DependencyPropertyKey& IsKeyboardFocusWithinPropertyKey();
	static const DependencyPropertyKey& IsMouseOverPropertyKey();
	static const DependencyPropertyKey& IsMouseDirectlyOverPropertyKey();
	static const DependencyPropertyKey& IsMouseCapturedPropertyKey();
	static const DependencyPropertyKey& IsMouseCaptureWithinPropertyKey();
	template<typename TOwner, typename TImmediateBase>
	static const DependencyPropertyMetadataRegistration&
		RegisterControlBorderThicknessMetadata(
		float defaultValue
		CUI_DESIGN_METADATA_ARGUMENTS(int designOrder = 40))
	{
		static const DependencyPropertyMetadataRegistration relation =
			[defaultValue
			CUI_DESIGN_METADATA_ARGUMENTS(designOrder)]
		{
			const auto& property = Control::BorderThicknessProperty();
			DependencyPropertyOptions<TOwner, Thickness> options;
			options.DefaultValue = Thickness(defaultValue);
			options.Flags = DependencyPropertyFlags::AffectsMeasure
				| DependencyPropertyFlags::AffectsArrange
				| DependencyPropertyFlags::AffectsRender;
			CUI_DESIGN_METADATA_ONLY(
			const std::type_index controlOwner[] = {
				std::type_index(typeid(Control))
			};
			const auto* base =
				DependencyPropertyRegistry::FindRegistered(
					controlOwner, L"BorderThickness");
			if (!base)
				throw std::logic_error(
					"Control.BorderThickness must be registered first");
			options.Design = base->Design();
			options.Design.Order = designOrder;
			)
			return DependencyPropertyRegistry::OverrideMetadataStatic<
				TOwner, TImmediateBase, Thickness>(
					property,
					Control::BorderThicknessPropertyMetadataRelation(),
					std::move(options));
		}();
		return relation;
	}
	// Effective WPF-style typography values. The DirectWrite object is a
	// private render projection and never participates in the public value
	// system or external ownership.
	std::wstring _fontName = L"Segoe UI";
	std::wstring _language = L"en-us";
	double _fontSize = 12.0;
	std::unique_ptr<Font> _renderFont;
	std::unique_ptr<Font> _systemScaledFont;
	float _systemScaledFontSourceSize = 0.0f;
	float _systemScaledFontFactor = 1.0f;
	D2D1_RECT_F _caretBlinkRect{ 0,0,0,0 };
	bool _caretBlinkRectValid = false;
	int _caretBlinkSelectionStart = 0;
	int _caretBlinkSelectionEnd = 0;
	bool _caretBlinkFocused = false;
	ULONGLONG _caretBlinkResetTick = 0;
	// Window animation registries are sparse.  Ordinary transient controls
	// (notably DataGrid editing elements) use these bits to avoid rescanning the
	// complete registry while their visual subtrees are attached or detached.
	bool _registeredDeclarativeAnimationWindow = false;
	bool _registeredNativeAnimationWindow = false;
	bool _validationHasError = false;
	std::vector<BindingValidationResult> _validationErrors;
	uint32_t _accessibilityRuntimeId = 0;
	// Native automation calls may re-enter user code and synchronously remove
	// the owner. A shared peer lease keeps the executing semantic object alive
	// until the COM operation unwinds; the peer itself never owns the Control.
	mutable std::shared_ptr<AutomationPeer> _automationPeer;
#if CUI_ENABLE_DYNAMIC_XAML
	struct DynamicXamlPropertyState;
	std::unique_ptr<DynamicXamlPropertyState> _dynamicXamlPropertyState;
	std::unique_ptr<DeclarativeComponentBehavior>
		_declarativeComponentBehavior;
	void InstallDynamicXamlPropertyState(
		std::shared_ptr<const DeclarativeType> descriptor,
		std::vector<BindingValue> values);
	bool TryReadDynamicXamlPropertySlot(
		const DeclarativeType& owner,
		std::size_t slot,
		BindingValue& out) const;
	bool TryWriteDynamicXamlPropertySlot(
		const DeclarativeType& owner,
		std::size_t slot,
		const BindingValue& value);
#endif
	std::vector<std::pair<TemplatePartToken, Control*>> _templateNameScope;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Names are design/dynamic diagnostics only and never enter Production. */
	std::vector<std::pair<TemplatePartToken, std::wstring>>
		_templateNameScopeNames;
#endif
	Control* _controlTemplateRoot = nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic component slots are discovered by authored property name only in
	 * the Design/XAML compatibility runtime. Generated components retain their
	 * presenter pointers in strongly typed members instead. */
	std::vector<std::pair<std::wstring, Control*>>
		_declarativeContentPresenters;
#endif
	std::vector<EventConnection> _templateEventConnections;
	std::vector<EventConnection> _templatePartEventConnections;
	struct DeclarativeVisualStateRuntime;
	std::unique_ptr<DeclarativeVisualStateRuntime> _declarativeVisualStates;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Authored group/state names and Design-owned program lifetime stay out of Production. */
	std::shared_ptr<ControlVisualStateDesignSidecar>
		_visualStateDesignSidecar;
	bool InstallDesignInteractionDefinitions(
		std::vector<DeclarativeVisualStateGroupDefinition> groups,
		std::vector<DeclarativeEventTriggerDefinition> eventTriggers,
		std::wstring* outError);
#endif
	std::wstring _lastTemplateError;
	bool _templateApplied = false;
	bool _applyingTemplate = false;
	bool _templateApplyCallbackPending = false;
	struct PendingVisualChildAttachment final
	{
		Control* Child = nullptr;
		ControlWeakReference LogicalParent;
		// Stack-owned transaction token used by ownership-preserving callers.
		// SynchronizeVisualChildCollection publishes it only after structural
		// validation succeeds and before any parent/public notification runs.
		bool* StructuralCommit = nullptr;
	};
	std::vector<PendingVisualChildAttachment>
		_pendingVisualChildAttachments;
#if CUI_ENABLE_DYNAMIC_XAML
	bool _dispatchingComponentBehaviorInput = false;
#endif
	// Authored/local IsEnabled value.  It is deliberately not exposed as a
	// writable field: every mutation must flow through SetLocalEnabled so the
	// effective routed subtree is republished atomically.
	bool _localEnabled = true;
	// WPF-style command sources contribute a coercion predicate to IsEnabled.
	// The authored/local value remains intact while command availability
	// changes, so a later requery never overwrites XAML, style or binding state.
	bool _commandCanExecute = true;
	bool _hasCommandCanExecute = false;

	friend DataBindingCollection;
	friend class Binding;
	friend class FocusManager;
	friend class ::DependencyPropertyMetadata;
	friend class DependencyPropertyRegistry;
#if CUI_ENABLE_DYNAMIC_XAML
	friend DeclarativeType;
	friend DeclarativeComponentBehavior;
	DeclarativePropertyMetadataPointer FindObjectPropertyMetadataByName(
		const std::wstring& propertyName) const override;
	DeclarativePropertyMetadataCollection
		GetObjectPropertyMetadata() const override;
#endif
	bool SupportsNativeProperty(
		const DeclarativePropertyMetadata& metadata) const override;
#if CUI_ENABLE_DYNAMIC_XAML
	bool TryGetDeclarativePropertyBacking(
		const DeclarativeType& owner,
		std::size_t slot,
		BindingValue& out) const;
	bool TrySetDeclarativePropertyBacking(
		const DeclarativeType& owner,
		std::size_t slot,
		const BindingValue& value);
#endif
	void OnBindingValidationChanged(
		const std::wstring& targetProperty) override;
	EventConnection SubscribeDefaultPropertyChange(
		const DependencyProperty& property,
		DependencyPropertyChangeHandler handler,
		DataSourceUpdateMode updateMode) override;
	void SetIsFocusedCore(bool value);
	void SetIsKeyboardFocusedCore(bool value);
	void SetIsKeyboardFocusVisibleCore(bool value);
	void SetIsMouseDirectlyOverCore(bool value);
	void SetIsMouseCapturedCore(bool value);
	bool StageReverseInheritedPropertyChange(
		cui::framework::ReverseInheritedPropertyKind kind,
		bool value,
		DeferredPropertyChange& change);
	void PublishReverseInheritedPropertyChange(
		cui::framework::ReverseInheritedPropertyKind kind,
		const DeferredPropertyChange& change);
	virtual void OnIsMouseOverChanged(bool, bool) {}
	void ApplyTypographyFont();
	/** Native text realization used only by C++ measure/render implementations. */
	class Font* GetRenderFont();
	void NotifyAccessibilityStructureChanged();
	void NotifyAccessibilityStateChanged();
	void NotifyAccessibilityValueChanged();
	void NotifyAccessibilityScrollChanged();
	void NotifyAccessibilityVirtualChanged(
		uint32_t virtualId, AccessibilityChange change);
	void PublishEffectiveIsEnabledChanges(
		std::vector<std::pair<ControlWeakReference, bool>> snapshot);
	void PublishEffectiveIsVisibleChanges(
		std::vector<std::pair<ControlWeakReference, bool>> snapshot);
	void RequestArrange();
	/** Allocation-free enumeration of inherited DP identities declared by this type. */
	using InheritedPropertyVisitor = void(*)(
		void* context, const DependencyProperty& property);
	const ::DependencyPropertyMetadata* ResolveExactDependencyPropertyMetadata(
		const DependencyProperty& property) const override;
	virtual void VisitDeclaredInheritedProperties(
		void* context, InheritedPropertyVisitor visitor) const;
	void RefreshInheritedPropertyValue(const DependencyProperty& property);
	void RefreshInheritedPropertyRecursive(const DependencyProperty& property);
	void RefreshInheritedPropertiesRecursive();
	void RefreshInheritedPropertyValues();
	void ApplyPropertyMetadataChange(
		const DeclarativePropertyMetadata& metadata,
		const BindingValue& oldValue,
		const BindingValue& newValue) override;
	#if CUI_ENABLE_DYNAMIC_XAML
	bool TrySetDynamicResourceExpressionOwned(
		const std::wstring& propertyName,
		std::wstring resourceKey,
		DependencyPropertyValueSource source);
	#endif
	bool TrySetDynamicResourceExpressionOwned(
		const DependencyPropertyMetadata& metadata,
		std::wstring resourceKey,
		DependencyPropertyValueSource source);
	std::vector<std::shared_ptr<const ControlStyleSheet>>
		VisibleAuthorStyleSheets() const;
	bool RefreshStyleValuesForSource(
		DependencyPropertyValueSource source,
		const std::vector<std::shared_ptr<const ControlStyleSheet>>& sheets,
		std::vector<DependencyPropertyReference>& appliedProperties);
	bool SynchronizeStyleTriggerActions(
		DependencyPropertyValueSource source,
		const std::shared_ptr<const ControlStyleSheet>& sheet,
		const ControlStyleResolution& resolution);
	bool PruneStyleTriggerActions(
		DependencyPropertyValueSource source,
		const std::vector<std::shared_ptr<const ControlStyleSheet>>& sheets);
	bool TryResolveDynamicResource(
		const std::wstring& resourceKey, BindingValue& value) const;
	bool RefreshDynamicResourceValues(bool recursive);
	void RebuildStylePropertyConditionSubscription();
	void RebuildStyleDataContextSubscriptions();
	void RebuildStyleSubscriptions(bool recursive);
	void UpdateEffectiveDataContext(BindingSourceReference value);

	/** Routes Visual::ZIndex's CLR wrapper through this object's DP store. */
	bool RouteVisualZIndexSet(int value);

	/** Internal compatibility spelling; delegates to WPF-style CoerceValue. */
	bool ReevaluatePropertyValue(const DependencyProperty& property)
	{
		return CoerceValue(property);
	}

	cui::core::Size ResolveDesiredSize(
		cui::core::Size intrinsicSize,
		const cui::core::Constraints& available) const;
	virtual cui::core::Size GetRenderSizeDip();

	void UpdateCaretBlinkState(bool focused, int selectionStart, int selectionEnd, bool caretRectValid, const D2D1_RECT_F* caretRect = nullptr);
	bool IsCaretBlinkVisible() const;
	bool HasRetainedCaretBlinkAnimation() const noexcept;
	bool IsCaretBlinkAnimating() const;
	bool GetCaretBlinkInvalidRect(D2D1_RECT_F& outRect) const;
	virtual bool DefaultSelectOnLeftButtonDown() const
	{
		return GetDependencyPropertyValue<bool>(FocusableProperty());
	}
	virtual bool DefaultRaiseClickOnLeftButtonUp() const { return false; }
	virtual bool DefaultInvalidateVisualOnMouseDown(MouseButton button) const { (void)button; return true; }
	virtual bool DefaultInvalidateVisualOnMouseUp(MouseButton button) const { (void)button; return true; }
	virtual void BeforeDefaultMouseMove(MouseEventArgs& e) { (void)e; }
	virtual void BeforeDefaultMouseDown(MouseButton button, MouseEventArgs& e) { (void)button; (void)e; }
	virtual void BeforeDefaultMouseUp(MouseButton button, MouseEventArgs& e, bool hasMatchingPress) { (void)button; (void)e; (void)hasMatchingPress; }
	virtual void BeforeDefaultClick(MouseButton button, MouseEventArgs& e) { (void)button; (void)e; }
	virtual void AfterDefaultClick(MouseButton button, MouseEventArgs& e) { (void)button; (void)e; }
	virtual void OnComputedLayoutSizeChanged() {}
	virtual void PerformPendingLayout() {}
	/** Marks a retained composite's local child-layout transaction pending. */
	virtual void OnLocalMeasurePathInvalidated() {}
	/** Framework-owned visual root generated from this Control's template. */
	virtual Control* GetControlTemplateRoot() const noexcept
	{
		return _controlTemplateRoot;
	}
	virtual Control* SetControlTemplateRoot(std::unique_ptr<Control> value);
	virtual std::unique_ptr<Control> DetachVisualChildTemplateRoot();
	virtual void ConfigureControlTemplateVisual(Control& child);
	void ConfigureControlTemplateVisualPreservingOwnership(
		std::unique_ptr<Control>& value);
	static std::exception_ptr ClearTemplateOwnerSubtree(
		Control* root,
		Control* owner) noexcept;
	static std::exception_ptr
		ClearTemplateOwnerSubtreePreservingOwnership(
			std::unique_ptr<Control>& root,
			Control* owner) noexcept;
	virtual void OnControlTemplatePresentationChanged() {}
	/** WPF lifecycle hook called after a complete template instance is wired. */
	virtual void OnApplyTemplate() {}
	/** Called after Template's effective value changes and the old tree is gone. */
	virtual void OnTemplateChanged(
		const ControlTemplateReference& oldTemplate,
		const ControlTemplateReference& newTemplate)
	{
		(void)oldTemplate;
		(void)newTemplate;
	}
	void MarkControlTemplateRootAttached() noexcept
	{
		_templateApplied = true;
		_templateApplyCallbackPending = true;
	}
	void MarkControlTemplateRootDetached() noexcept
	{
		_templateApplied = false;
		_templateApplyCallbackPending = false;
	}
	void CompleteControlTemplateApplication();
	void AbortControlTemplateApplication() noexcept;
	/** Allows specialized containers to reject structurally invalid child types. */
	virtual bool ValidateVisualChildCollection(
		std::span<Control* const> children,
		std::string& error) const
	{
		(void)children;
		(void)error;
		return true;
	}
	/** Called after visual-parent/presentation-host synchronization and before public notification. */
	virtual void OnVisualChildCollectionChanged(
		const CollectionChangedEventArgs& change,
		std::span<Control* const> previousChildren)
	{
		(void)change;
		(void)previousChildren;
	}
	/** Framework-only propagation of the current Window presentation source. */
	static void PropagatePresentationWindow(
		Control* control, PresentationWindow* window);
	/** Called after this element and its visual descendants enter/leave a Window. */
	virtual void OnPresentationWindowChanged(
		PresentationWindow* previousWindow,
		PresentationWindow* currentWindow);
	/** Stops input reverse inheritance after including this element. */
	virtual bool BlocksReverseInheritance() const noexcept { return false; }
	/** Effective IsEnabled transition hook; callers must revalidate lifetime after it. */
	virtual void OnEffectiveIsEnabledChanged(
		bool previousValue, bool currentValue)
	{
		(void)previousValue;
		(void)currentValue;
	}
	/** Effective IsVisible transition hook; Window projects it to HWND state. */
	virtual void OnEffectiveIsVisibleChanged(
		bool previousValue, bool currentValue)
	{
		(void)previousValue;
		(void)currentValue;
	}
	void SynchronizeVisualChildCollection(const CollectionChangedEventArgs& change);
	Control* InsertVisualChildWithLogicalParent(
		int index,
		Control* child,
		Control* logicalParent,
		bool* structuralCommit = nullptr);
	void SetVisualParentCore(Control* value);
	void SetLogicalParentCore(Control* value);
	void SetLogicalParentCoreObservingVisualOwnership(
		Control* value,
		bool* visualOwnershipCommit);
	void SetTemplatedParentCore(Control* value);
	void SetTemplatedParentCoreObservingVisualOwnership(
		Control* value,
		bool* visualOwnershipCommit);
	void InvokeWithVisualOwnershipObservationCore(
		const std::function<void()>& callback,
		bool* visualOwnershipCommit);
	std::unique_ptr<Control> DetachVisualChildCore(
		Control* child,
		bool* visualOwnershipCommit,
		std::exception_ptr* notificationError);
	void RefreshInheritanceContext(bool recursive);
	void RegisterInheritanceChild(Control* child);
	void UnregisterInheritanceChild(Control* child);

	// 通知父容器（Panel 或 Window）需要重新布局
	virtual void RequestLayout();
	void BeginLayoutUpdateDeferral() noexcept;
	void EndLayoutUpdateDeferral(bool performLayout);
	class ScopedLayoutUpdate final
	{
	public:
		explicit ScopedLayoutUpdate(Control& owner, bool performLayout = true)
			: _owner(&owner), _performLayout(performLayout),
			_uncaughtOnEntry(std::uncaught_exceptions())
		{
			owner.BeginLayoutUpdateDeferral();
		}
		ScopedLayoutUpdate(const ScopedLayoutUpdate&) = delete;
		ScopedLayoutUpdate& operator=(const ScopedLayoutUpdate&) = delete;
		~ScopedLayoutUpdate() noexcept
		{
			auto* owner = _owner.Get();
			if (!owner) return;
			const bool unwinding =
				std::uncaught_exceptions() > _uncaughtOnEntry;
			try
			{
				owner->EndLayoutUpdateDeferral(
					unwinding ? false : _performLayout);
			}
			catch (...) {}
		}

	private:
		ControlWeakReference _owner;
		bool _performLayout = true;
		int _uncaughtOnEntry = 0;
	};
	void BeginVisualInvalidationDeferral() noexcept;
	void EndVisualInvalidationDeferral(bool dispatchVisual);
	class ScopedVisualInvalidation final
	{
	public:
		explicit ScopedVisualInvalidation(
			Control& owner, bool dispatchVisual = true)
			: _owner(&owner), _dispatchVisual(dispatchVisual),
			_uncaughtOnEntry(std::uncaught_exceptions())
		{
			owner.BeginVisualInvalidationDeferral();
		}
		ScopedVisualInvalidation(const ScopedVisualInvalidation&) = delete;
		ScopedVisualInvalidation& operator=(
			const ScopedVisualInvalidation&) = delete;
		~ScopedVisualInvalidation() noexcept
		{
			auto* owner = _owner.Get();
			if (!owner) return;
			const bool unwinding =
				std::uncaught_exceptions() > _uncaughtOnEntry;
			try
			{
				owner->EndVisualInvalidationDeferral(
					unwinding ? true : _dispatchVisual);
			}
			catch (...) {}
		}

	private:
		ControlWeakReference _owner;
		bool _dispatchVisual = true;
		int _uncaughtOnEntry = 0;
	};
	void InvalidateMeasureSubtree();
	/** Marks one owned visual path dirty without propagating a new root request. */
	void InvalidateMeasurePathFromDescendant(Control* descendant);
	void InvalidateVisualSubtree();
	void InvalidateVisualBoundsSubtree();
	void InvalidateVisualRectCore(
		const D2D1_RECT_F& contentRect,
		bool contentChanged);
	void MarkPresentationInvalidation(
		PresentationInvalidationKind kind) noexcept;
	void InvalidatePresentationGeometrySubtree() noexcept;
	void InvalidatePresentationTransformSubtree() noexcept;
	D2D1_MATRIX_3X2_F GetEffectiveDescendantRenderTransform() const;
	D2D1_MATRIX_3X2_F GetInheritedRenderTransformForRecording() const;
	D2D1_MATRIX_3X2_F GetEffectiveDescendantRenderTransformForRecording() const;
	D2D1_MATRIX_3X2_F GetLocalToRenderTransformForRecording() const;
	D2D1_RECT_F GetRenderedAbsoluteRectDipForRecording() const;
	/** Appends retained composition animation targets by property lane. */
	void CollectDeclarativeCompositionAnimationTargets(
		std::vector<Control*>& transformTargets,
		std::vector<Control*>& opacityTargets) const;
	/** UI-thread recording override; returns the previous outer-to-inner chain. */
	static std::span<Control* const>
		ExchangeRenderTransformSuppressionsForRecording(
			std::span<Control* const> roots) noexcept;
	// 将内容区 DIP 矩形统一转换为窗口客户区物理像素，并与上次区域取并集。
	void InvalidateVisualRect(const D2D1_RECT_F& contentRect);
	void DispatchInvalidatedClientRect(const D2D1_RECT_F& clientRect);

	friend class Panel;
	friend class Window;
	friend struct cui::framework::TemplateAccess;
public:
	/** Sets a local DataContext; descendants inherit its effective source. */
	bool SetDataContext(BindingSourceReference value);
	/** Removes the local value and resumes inheritance from the logical/template context. */
	bool ClearDataContext();
	const BindingSourceReference& GetDataContext() const noexcept
	{
		return _effectiveDataContext;
	}
	/** Stable source identity used by bindings across inherited-source changes. */
	IBindingSource& DataContextSource();
	PropertyChangedEvent& DataContextChanged() noexcept { return _dataContextChanged; }
	/** Every node on a declarative routed-event route publishes the event here. */
	DeclarativeEvent OnDeclarativeEvent;
#if CUI_ENABLE_DYNAMIC_XAML
	const DeclarativeEventDefinition* FindDeclarativeEvent(
		const std::wstring& eventName) const noexcept;
	bool RaiseDeclarativeEvent(
		std::wstring eventName,
		BindingValue value = {});
#endif
	/** Compiled component path: dispatch by definition identity, without lookup. */
	bool RaiseDeclarativeEvent(
		const DeclarativeEventDefinition& definition,
		BindingValue value = {});
	/** WPF-style overload that returns routed state, including Handled, to the raiser. */
	bool RaiseDeclarativeEvent(DeclarativeEventArgs& args);
	/** Explicit state entry used by component behavior and declarative event triggers. */
#if CUI_ENABLE_DYNAMIC_XAML
	bool GoToVisualState(
		const std::wstring& groupName,
		const std::wstring& stateName,
		std::wstring* outError = nullptr);
	/** WPF-style overload that can explicitly bypass VisualTransition objects. */
	bool GoToVisualState(
		const std::wstring& groupName,
		const std::wstring& stateName,
		bool useTransitions,
		std::wstring* outError = nullptr);
	/** Searches all groups; fails when the state name is absent or ambiguous. */
	bool GoToVisualState(
		const std::wstring& stateName,
		std::wstring* outError = nullptr);
	bool GoToVisualState(
		const std::wstring& stateName,
		bool useTransitions,
		std::wstring* outError = nullptr);
#endif
	/** Compiled path: direct token lookup without retaining or comparing names. */
	bool GoToVisualState(
		VisualStateGroupToken group,
		VisualStateToken state,
		bool useTransitions = true,
		std::wstring* outError = nullptr);
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring GetCurrentVisualState(
		const std::wstring& groupName) const;
#endif
	VisualStateToken GetCurrentVisualState(
		VisualStateGroupToken group) const noexcept;
	/** True while at least one VisualState Storyboard timeline is active. */
	bool HasActiveVisualStateAnimations() const noexcept;
	DeclarativeVisualStateChangedEvent OnVisualStateChanged;
	/** Root Storyboard timing events, coalesced and dispatched after a committed tick. */
	DeclarativeClockTimingEvent OnStoryboardTimingEvent;
	/** Effective WPF-style IsEnabled value; assignments update the local value. */
	PROPERTY(bool, IsEnabled);
	virtual GET(bool, IsEnabled);
	virtual SET(bool, IsEnabled);
	/** WPF AllowDrop DP; FrameworkElement metadata inherits through the tree. */
	PROPERTY(bool, AllowDrop);
	virtual GET(bool, AllowDrop);
	virtual SET(bool, AllowDrop);
	PROPERTY(::Visibility, Visibility);
	virtual GET(::Visibility, Visibility);
	virtual SET(::Visibility, Visibility);
	READONLY_PROPERTY(bool, IsVisible);
	bool GetIsVisible() const;
	bool IsCollapsed() const noexcept
	{
		return (_presentationSuppressed
				&& PresentationSuppressionAffectsLayout())
			|| _visibility == ::Visibility::Collapsed;
	}
	IsVisibleChangedEvent IsVisibleChanged;
	/**
	 * @brief 可观察的拥有型可视子节点集合。
	 *
	 * 直接 insert/erase/move/swap 会同步 VisualParent；普通插入同时建立
	 * LogicalParent。模板/内容基础设施必须使用显式树关系 API；
	 * erase/clear 只分离对象，不销毁，销毁请使用 DeleteVisualChild。
	 */
	/** @brief 创建基础控件。 */
	Control();
	/** @brief 虚析构：释放控件私有的渲染、输入与表达式资源。 */
	virtual ~Control();
	/** @brief 返回运行时类型标识。 */
	virtual UIClass Type();
	/** Effective WPF Control.Template dependency-property value. */
	ControlTemplateReference GetTemplate() const;
	void SetTemplate(ControlTemplateReference value);
	/**
	 * Creates the template visual tree on this exact control instance.
	 * Returns true only when this call created a new visual subtree.
	 */
	bool ApplyTemplate();
	const std::wstring& LastTemplateError() const noexcept
	{
		return _lastTemplateError;
	}

protected:
	/** Root/runtime hook; normal descendants inherit through the tree. */
	void SetInheritedDataContext(BindingSourceReference value);
	/** Installs one immutable set of XAML-owned visual-state groups. */
#if CUI_ENABLE_DYNAMIC_XAML
	bool DefineVisualStateGroups(
		std::vector<DeclarativeVisualStateGroupDefinition> groups,
		std::wstring* outError = nullptr);
	/** Installs XAML visual states and template EventTrigger actions atomically. */
	bool DefineDeclarativeInteractions(
		std::vector<DeclarativeVisualStateGroupDefinition> groups,
		std::vector<DeclarativeEventTriggerDefinition> eventTriggers,
		std::wstring* outError = nullptr);
#endif
	/** Binds one process-lifetime AOT program to this template instance. */
	bool InstallCompiledInteractions(
		const CompiledInteractionProgramView& program,
		std::span<const BindingValue> values,
		std::span<Control* const> targets,
		std::wstring* outError = nullptr);
	/** Advances XAML-owned timelines from the Window presentation clock. */
	bool AdvanceVisualStateAnimations(unsigned long long nowMilliseconds);
	bool HasRetainedVisualStateAnimationRoots() const noexcept;
	void SynchronizeDeclarativeAnimationWindowRegistration() noexcept;
	void RefreshDeclarativeAnimationWindowScheduling() noexcept;
	void SynchronizeNativeAnimationWindowRegistration() noexcept;
	/** Testing-only clock exchange; state lives only on an installed runtime. */
	std::optional<unsigned long long>
		ExchangeVisualStateAnimationClockOverrideForTesting(
			std::optional<unsigned long long> value) noexcept;
	/** Distinguishes an advance failure from the ordinary no-active-clock result. */
	bool VisualStateAnimationAdvanceFailedForTesting() const noexcept;
	/** Testing-only: rejects the next staged animation frame before DP commit. */
	void FailNextVisualStateAnimationFrameCommitForTesting() noexcept;
	/** Testing-only count of retained declarative animation leaves. */
	size_t VisualStateAnimationLeafCountForTesting() const noexcept;
	/** Testing-only count of distinct declarative root ClockId values. */
	size_t VisualStateAnimationRootClockCountForTesting() const noexcept;
	/** Testing-only synchronous seek for a runtime with exactly one root clock. */
	bool SeekSingleVisualStateAnimationRootAlignedForTesting(
		unsigned long long offsetMilliseconds);
	/** Testing-only ordinary seek; commits on the next explicit animation tick. */
	bool SeekSingleVisualStateAnimationRootForTesting(
		unsigned long long offsetMilliseconds);
	/** Testing-only pending controls for a runtime with exactly one root clock. */
	bool PauseSingleVisualStateAnimationRootForTesting();
	bool ResumeSingleVisualStateAnimationRootForTesting();
	bool SetSpeedRatioSingleVisualStateAnimationRootForTesting(double ratio);
	bool SkipSingleVisualStateAnimationRootToFillForTesting();
	bool StopSingleVisualStateAnimationRootForTesting();
	bool RemoveSingleVisualStateAnimationRootForTesting();
	std::optional<DeclarativeClockObservation>
		QuerySingleVisualStateAnimationRootForTesting() const noexcept;
	/** Testing-only count of occupied root + leaf ClockNode slots. */
	size_t VisualStateAnimationClockNodeCountForTesting() const noexcept;
	/** Testing-only count of explicit per-target-DP animation stacks. */
	size_t VisualStateAnimationLayerStackCountForTesting() const noexcept;
	/** Testing-only count of explicit animation layer owner records. */
	size_t VisualStateAnimationLayerCountForTesting() const noexcept;
	/** Testing-only maximum number of layers owned by one target DP. */
	size_t VisualStateAnimationLayerMaxDepthForTesting() const noexcept;
	/** Testing-only count of children in one committed root node range. */
	size_t VisualStateAnimationRootClockChildCountForTesting(
		size_t rootIndex) const noexcept;
	/** Testing-only: returns the sole root ClockId, or zero for none/multiple. */
	uint64_t VisualStateAnimationSingleRootClockIdForTesting() const noexcept;
	/** Testing-only: generation-safe token for the sole root ClockNode. */
	uint64_t VisualStateAnimationSingleRootClockNodeTokenForTesting() const noexcept;
	/** Testing-only validation of leaf/root identity ownership invariants. */
	bool VisualStateAnimationClockIdentityValidForTesting() const noexcept;
	/** Retains one framework/template subscription for this object's lifetime. */
	void RetainEventConnection(EventConnection connection)
	{
		if (connection.Connected())
			_retainedEventConnections.push_back(std::move(connection));
	}
	void ClearRetainedEventConnections() noexcept
	{
		_retainedEventConnections.clear();
	}
	/** Retains an authored template-event bridge until Template changes. */
	void RetainTemplateEventConnection(EventConnection connection)
	{
		if (connection.Connected())
			_templateEventConnections.push_back(std::move(connection));
	}
	/** Retains a native behavior subscription to one current template part. */
	void RetainTemplatePartEventConnection(EventConnection connection)
	{
		if (connection.Connected())
			_templatePartEventConnections.push_back(std::move(connection));
	}
	void ClearTemplatePartEventConnections() noexcept
	{
		_templatePartEventConnections.clear();
	}
	void SetLogicalParent(Control* value)
	{
		VerifyAccess();
		SetLogicalParentCore(value);
	}
	void SetTemplatedParent(Control* value)
	{
		VerifyAccess();
		SetTemplatedParentCore(value);
	}
	/** Derived state/layout preparation performed before retained scene rendering. */
	virtual void PreparePresentation() { PerformPendingLayout(); }
	/**
	 * Popup-like transient roots can suppress their main-tree projection
	 * without collapsing their independently measured content.
	 */
	virtual bool PresentationSuppressionAffectsLayout() const noexcept
	{
		return true;
	}
	/**
	 * True when a transient root starts a new visual-presentation inheritance
	 * scope instead of inheriting suppression from its visual parent.
	 */
	virtual bool BreaksVisualPresentationInheritance() const noexcept
	{
		return false;
	}
	/** Selects the drawing or native-composition path without runtime type switches. */
	virtual PresentationSurfaceKind GetPresentationSurfaceKind() const noexcept
	{
		return PresentationSurfaceKind::Drawing;
	}
	/**
	 * Establishes the owner-retained root visual before scene group staging.
	 * Native controls must register/unregister this visual through Window so the
	 * render host can transfer grouped ownership atomically.
	 */
	virtual bool PrepareNativeCompositionVisual() { return false; }
	virtual bool SupportsNativeCompositionVisualLease() const noexcept
	{
		return false;
	}
	/** Borrowed pointer valid until the owner calls Window::UnregisterDCompVisual. */
	virtual IDCompositionVisual* GetNativeCompositionVisual() const noexcept
	{
		return nullptr;
	}
	/** Host DPI changed; device-independent controls normally need no work. */
	virtual void NotifyDpiChanged(float dpiScale);
	/** Host render device is about to be discarded after device loss. */
	virtual void NotifyDeviceResourcesInvalidated() noexcept;
	/** Native fallback visual-state cache; public Style conditions observe DPs. */
	ControlStyleState GetStyleState() const noexcept { return _styleState; }
	ControlStyleState GetEffectiveStyleState() const noexcept;
	void SetStyleState(ControlStyleState state, bool enabled = true);
	/** Projects the internal press gesture into ButtonBase.IsPressed. */
	virtual void OnPressedVisualStateChanged(bool) {}
#if CUI_ENABLE_DYNAMIC_XAML
	bool SetDeclarativeComponentBehavior(
		std::unique_ptr<DeclarativeComponentBehavior> behavior,
		const DeclarativeComponentBehaviorContext& context,
		std::wstring* outError = nullptr);
	void ClearDeclarativeComponentBehavior() noexcept;
	bool SetDeclarativeTypeDescriptor(
		std::shared_ptr<const DeclarativeType> descriptor,
		std::wstring* outError = nullptr);
#endif
	bool RegisterDeclarativeTemplatePart(
		TemplatePartToken token,
		Control* instance);
#if CUI_ENABLE_DYNAMIC_XAML
	bool RegisterDeclarativeTemplatePart(
		std::wstring localName,
		Control* instance);
	bool RegisterDeclarativeContentPresenter(
		std::wstring propertyName,
		Control* instance);
#endif
	void ClearDeclarativeTemplateScope();
	#if CUI_ENABLE_DYNAMIC_XAML
	bool SetDynamicResource(
		const std::wstring& propertyName,
		std::wstring resourceKey,
		DependencyPropertyValueSource source);
	#endif
	bool SetDynamicResource(
		const DependencyProperty& property,
		std::wstring resourceKey,
		DependencyPropertyValueSource source);
	#if CUI_ENABLE_DYNAMIC_XAML
	bool ClearDynamicResource(
		const std::wstring& propertyName,
		DependencyPropertyValueSource source);
	#endif
	bool ClearDynamicResource(
		const DependencyProperty& property,
		DependencyPropertyValueSource source);

public:
#if CUI_ENABLE_DYNAMIC_XAML
	/**
	 * Installs one application behavior on this XAML component instance.
	 * The Control owns it and guarantees Detach before template children die.
	 */
	DeclarativeComponentBehavior* GetDeclarativeComponentBehavior() noexcept
	{
		return _declarativeComponentBehavior.get();
	}
	const DeclarativeComponentBehavior*
		GetDeclarativeComponentBehavior() const noexcept
	{
		return _declarativeComponentBehavior.get();
	}
	bool HasDeclarativeComponentBehavior() const noexcept
	{
		return static_cast<bool>(_declarativeComponentBehavior);
	}
#endif
	/** Device-independent WPF brush surfaces; raw colors are renderer fallbacks only. */
	PROPERTY(cui::drawing::Brush, Background);
	GET(cui::drawing::Brush, Background);
	SET(cui::drawing::Brush, Background);
	std::optional<cui::drawing::Brush> GetBackgroundBrush() const;
	/** Effective Brush value used by diagnostics and non-rendering consumers. */
	cui::drawing::Brush GetComputedBackgroundBrush() const;
	ID2D1Brush* CreateBackgroundBrush(
		D2DGraphics& graphics,
		D2D1_SIZE_F bounds) const;
	PROPERTY(cui::drawing::Brush, Foreground);
	GET(cui::drawing::Brush, Foreground);
	SET(cui::drawing::Brush, Foreground);
	std::optional<cui::drawing::Brush> GetForegroundBrush() const;
	cui::drawing::Brush GetComputedForegroundBrush() const;
	/** Returns an owned COM brush reference, or nullptr when no brush is set. */
	ID2D1Brush* CreateForegroundBrush(
		D2DGraphics& graphics,
		D2D1_SIZE_F bounds) const;
	PROPERTY(cui::drawing::Brush, BorderBrush);
	GET(cui::drawing::Brush, BorderBrush);
	SET(cui::drawing::Brush, BorderBrush);
	std::optional<cui::drawing::Brush> GetLocalBorderBrush() const;
	cui::drawing::Brush GetComputedBorderBrush() const;
	ID2D1Brush* CreateBorderBrush(
		D2DGraphics& graphics,
		D2D1_SIZE_F bounds) const;
	/** WPF UIElement.Opacity applied to this complete retained subtree. */
	PROPERTY(double, Opacity);
	GET(double, Opacity);
	SET(double, Opacity);
	/** Sets an additional local geometry clip that also applies to descendants. */
	void SetClip(const cui::drawing::Geometry& geometry);
	void ClearClip();
	const std::optional<cui::drawing::Geometry>& GetClip() const noexcept
	{
		return _clip;
	}
	/**
	 * WPF UIElement.ClipToBounds. Ordinary elements default to false; viewport
	 * controls may still impose an intrinsic child clip through ClipsChildren.
	 */
	PROPERTY(bool, ClipToBounds);
	GET(bool, ClipToBounds);
	SET(bool, ClipToBounds);
	/** Sets a device-independent transform that affects this control and its descendants. */
	void SetRenderTransform(const cui::drawing::Transform& transform);
	void ClearRenderTransform();
	const std::optional<cui::drawing::Transform>& GetRenderTransform() const noexcept
	{
		return _renderTransform;
	}
	/** Relative point in the control bounds used as the transform origin. */
	void SetRenderTransformOrigin(D2D1_POINT_2F origin);
	D2D1_POINT_2F GetRenderTransformOrigin() const noexcept
	{
		return _renderTransformOrigin;
	}
	/** Floating-DIP metadata projection used by XAML, binding and animation. */
	void SetRenderTransformOriginDip(cui::core::Point origin);
	cui::core::Point GetRenderTransformOriginDip() const noexcept
	{
		return { _renderTransformOrigin.x, _renderTransformOrigin.y };
	}
	/** @brief 使控件可视区域失效，并请求窗口在下一次绘制中刷新它。 */
	virtual void InvalidateVisual();
	/**
	 * Requests a compositor-only refresh without changing drawing content.
	 * Native surfaces and behaviors use this when their retained resource is
	 * unchanged but its presentation state must be recommitted.
	 */
	void InvalidateComposition();
	/** @brief 当前控件是否处于活动动画中。 */
	virtual bool IsAnimationRunning() { return false; }
	/** Retained native animation intent, independent of visibility/preferences. */
	virtual bool HasRetainedNativeAnimation() { return IsAnimationRunning(); }
	/** @brief 动画帧间隔（毫秒）。 */
	virtual UINT GetAnimationIntervalMs();
	/**
	 * @brief 获取动画导致的额外无效区域。
	 * @param outRect 输出需要重绘的区域。
	 * @return true 表示 outRect 有效。
	 */
	virtual bool GetAnimatedInvalidRect(D2D1_RECT_F& outRect) { (void)outRect; return false; }
	READONLY_PROPERTY(const std::wstring&, FontFamily);
	GET(const std::wstring&, FontFamily);
	void SetFontFamily(std::wstring value);
	READONLY_PROPERTY(const std::wstring&, Language);
	GET(const std::wstring&, Language);
	void SetLanguage(std::wstring value);
	READONLY_PROPERTY(double, FontSize);
	GET(double, FontSize);
	void SetFontSize(double value);
	READONLY_PROPERTY(DataBindingCollection&, DataBindings);
	GET(DataBindingCollection&, DataBindings);
	/** WPF FrameworkElement.Tag equivalent; retains arbitrary scalar/object values. */
	PROPERTY(BindingValue, Tag);
	GET(BindingValue, Tag);
	SET(BindingValue, Tag);
	/** Whether this element can receive logical and keyboard focus. */
	PROPERTY(bool, Focusable);
	GET(bool, Focusable);
	SET(bool, Focusable);
	/** Whether this focusable element participates in Tab/Shift+Tab navigation. */
	PROPERTY(bool, IsTabStop);
	GET(bool, IsTabStop);
	SET(bool, IsTabStop);
	/** Stable order within a Window; ties preserve tree/insertion order. */
	PROPERTY(int, TabIndex);
	GET(int, TabIndex);
	SET(int, TabIndex);
	/** True while this element owns logical focus in at least one focus scope. */
	READONLY_PROPERTY(bool, IsFocused);
	GET(bool, IsFocused);
	/** True only for the Window's current keyboard-focus element. */
	READONLY_PROPERTY(bool, IsKeyboardFocused);
	GET(bool, IsKeyboardFocused);
	/** Theme-facing WPF focus-visual projection: keyboard input or system cues. */
	READONLY_PROPERTY(bool, IsKeyboardFocusVisible);
	GET(bool, IsKeyboardFocusVisible);
	/** True across the focused element's visual/logical ancestor closure. */
	READONLY_PROPERTY(bool, IsKeyboardFocusWithin);
	GET(bool, IsKeyboardFocusWithin);
	/** True across the directly hit element's visual/logical ancestor closure. */
	READONLY_PROPERTY(bool, IsMouseOver);
	GET(bool, IsMouseOver);
	/** True only for the Window's current pointer hit element. */
	READONLY_PROPERTY(bool, IsMouseDirectlyOver);
	GET(bool, IsMouseDirectlyOver);
	/** WPF FocusManager.IsFocusScope attached-property projection. */
	PROPERTY(bool, IsFocusScope);
	GET(bool, IsFocusScope);
	SET(bool, IsFocusScope);
	/** WPF KeyboardNavigation.TabNavigation attached-property projection. */
	PROPERTY(KeyboardNavigationMode, TabNavigation);
	GET(KeyboardNavigationMode, TabNavigation);
	SET(KeyboardNavigationMode, TabNavigation);
	/** WPF KeyboardNavigation.DirectionalNavigation attached-property projection. */
	PROPERTY(KeyboardNavigationMode, DirectionalNavigation);
	GET(KeyboardNavigationMode, DirectionalNavigation);
	SET(KeyboardNavigationMode, DirectionalNavigation);
	PROPERTY(std::wstring, AutomationName);
	GET(std::wstring, AutomationName);
	SET(std::wstring, AutomationName);
	PROPERTY(std::wstring, AutomationFullDescription);
	GET(std::wstring, AutomationFullDescription);
	SET(std::wstring, AutomationFullDescription);
	PROPERTY(std::wstring, AutomationHelpText);
	GET(std::wstring, AutomationHelpText);
	SET(std::wstring, AutomationHelpText);
	PROPERTY(std::wstring, AutomationId);
	GET(std::wstring, AutomationId);
	SET(std::wstring, AutomationId);
	std::vector<BindingValidationResult> GetValidationResults() const;
	bool HasValidationIssues() const;
	bool HasValidationErrors() const;
	bool TryGetValidationSeverity(BindingValidationSeverity& severity) const;
	std::wstring GetValidationSummary(size_t maxIssues = 0) const;
	std::wstring GetEffectiveAutomationName() const;
	/** Native text projection supplied by the semantic property owner. */
	virtual std::wstring GetSemanticText() const;
	/** Text rendered by WPF AccessText-aware controls ('_' markers removed). */
	std::wstring GetDisplayText() const;
	std::wstring GetEffectiveAutomationFullDescription() const;
	std::wstring GetEffectiveKeyboardShortcut() const;
	wchar_t GetEffectiveAccessKey() const;
	/** Dispatches an AccessKeyManager match through the control's WPF hook. */
	bool InvokeAccessKey(bool isMultiple)
	{
		return OnAccessKey(isMultiple);
	}
	/** Returns the lazily-created WPF-style semantic peer for this instance. */
	AutomationPeer& GetAutomationPeer() const;
	/**
	 * Pins the semantic peer across one external automation operation. The
	 * returned peer may outlive its Control only for destruction-safe unwinding;
	 * callers must still use ControlWeakReference before every owner access.
	 */
	std::shared_ptr<AutomationPeer> AcquireAutomationPeer() const;
	/** Stable per-process id used by native accessibility fragment providers. */
	uint32_t GetAccessibilityRuntimeId() const noexcept
	{
		return _accessibilityRuntimeId;
	}
	AccessibilitySnapshot GetAccessibilitySnapshot() const;
	/** ToggleButton overrides this; ordinary controls never carry toggle state. */
	virtual bool IsCheckedForAccessibility() const noexcept { return false; }
	/**
	 * UI Automation's Toggle pattern is tri-state even though the legacy MSAA
	 * snapshot below only exposes a checked bit.
	 */
	virtual AutomationToggleState GetToggleStateForAccessibility() const noexcept
	{
		return IsCheckedForAccessibility()
			? AutomationToggleState::On
			: AutomationToggleState::Off;
	}
	virtual bool IsAccessibilityReadOnly() const { return false; }
	/**
	 * Effective WPF-style IsEnabled value.  The local value, command-source
	 * predicate and every routed ancestor all participate without destroying
	 * the local property value.
	 */
	bool IsEffectivelyEnabled() const noexcept;
	/** Returns the authored/local IsEnabled value before ancestor/command coercion. */
	bool IsLocallyEnabled() const noexcept { return _localEnabled; }
	/** Updates the authored/local IsEnabled value and republishes subtree effects. */
	void SetLocalEnabled(bool value);
	/** Installs/updates the command-source CanExecute coercion predicate. */
	void SetCommandCanExecuteState(bool value);
	/** Removes command participation while preserving the local IsEnabled value. */
	void ClearCommandCanExecuteState();
	/** Applies Focusable, visibility, enabled and ancestor state. */
	bool CanReceiveKeyboardFocus() const;
	/** Adds IsTabStop to the effective keyboard-focus eligibility check. */
	bool CanParticipateInTabNavigation() const;
	/** Moves its owning Window's logical keyboard focus to this control. */
	bool Focus();
	/** Captures pointer delivery through the owning Window InputManager. */
	bool CaptureMouse();
	/** Releases pointer delivery when this control owns capture. */
	bool ReleaseMouseCapture();
	bool IsMouseCaptured() const;
	/** True for the captured element and both visual/logical ancestor graphs. */
	READONLY_PROPERTY(bool, IsMouseCaptureWithin);
	GET(bool, IsMouseCaptureWithin);
	/** Performs the control's primary action; overridden by actionable controls. */
	virtual bool Invoke();
	/** Returns false when Windows requests reduced client-area motion. */
	bool AreSystemAnimationsEnabled() const;
	/** Returns zero when reduced motion is active, otherwise the configured duration. */
	UINT EffectiveAnimationDuration(UINT configuredDurationMs) const;
	BindingValidationChangedEvent OnValidationStateChanged;
#if CUI_ENABLE_DYNAMIC_XAML
	const std::shared_ptr<const DeclarativeType>&
		GetDeclarativeTypeDescriptor() const noexcept;
#endif
	/** Exact, name-free ComponentDefinition identity; zero means native type. */
	ComponentTypeToken GetDeclarativeTypeToken() const noexcept
	{
#if CUI_ENABLE_DYNAMIC_XAML
		const auto& descriptor = GetDeclarativeTypeDescriptor();
		if (descriptor)
		{
			const auto& type = descriptor->TypeId();
			return MakeComponentTypeToken(type.NamespaceUri, type.LocalName);
		}
#endif
		return GetCompiledComponentTypeTokenCore();
	}
#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic-XAML QName sidecar; Production has no reverse token lookup. */
	const RuntimeTypeId& GetDeclarativeTypeId() const noexcept
	{
		static const RuntimeTypeId empty;
		const auto& descriptor = GetDeclarativeTypeDescriptor();
		if (descriptor)
			return descriptor->TypeId();
		return empty;
	}
	const std::wstring& GetDeclarativeTypeNamespace() const noexcept
	{
		return GetDeclarativeTypeId().NamespaceUri;
	}
	const std::wstring& GetDeclarativeTypeName() const noexcept
	{
		return GetDeclarativeTypeId().LocalName;
	}
#endif
	/** Token-only production equivalent of WPF GetTemplateChild. Zero returns this owner. */
	Control* FindDeclarativeTemplatePart(TemplatePartToken token) noexcept;
	const Control* FindDeclarativeTemplatePart(
		TemplatePartToken token) const noexcept;
#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic-XAML/design compatibility lookup with collision validation. */
	Control* FindDeclarativeTemplatePart(const std::wstring& localName) noexcept;
	const Control* FindDeclarativeTemplatePart(
		const std::wstring& localName) const noexcept;
#endif
	#if CUI_ENABLE_DYNAMIC_XAML
	/** Dynamic-XAML compatibility lookup. Generated components address their
	 * strongly typed presenter members directly in Production. */
	Control* FindDeclarativeContentPresenter(
		const std::wstring& propertyName) noexcept;
	const Control* FindDeclarativeContentPresenter(
		const std::wstring& propertyName) const noexcept;
	#endif
private:
	/** XAML Style/Resources lowering surface; only StyleAccess may project it. */
	const std::wstring& GetStyleResourceKey() const noexcept
	{
		return _styleResourceKey;
	}
	bool IsStyleResourceKeyCapturedFromTheme() const noexcept
	{
		return _styleResourceKeyCapturedFromTheme;
	}
	bool IsStyleResourceKeyAutomatic() const noexcept
	{
		return _styleResourceKeyIsAutomatic;
	}
	void SetStyleResourceKey(
		std::wstring value,
		bool capturedFromTheme = false,
		bool automatic = false);
	std::shared_ptr<const ControlStyleSheet> GetThemeStyleSheet() const noexcept
	{
		return _themeStyleSheet;
	}
	std::shared_ptr<const ControlStyleSheet> GetStyleSheet() const noexcept
	{
		return _styleSheet;
	}
	/** Returns this control's local ResourceDictionary, not an inherited one. */
	std::shared_ptr<const ControlStyleSheet> GetResourceDictionary() const noexcept
	{
		return _resourceDictionary;
	}
	bool SetThemeStyleSheet(
		std::shared_ptr<const ControlStyleSheet> value,
		bool recursive = true);
	bool SetStyleSheet(
		std::shared_ptr<const ControlStyleSheet> value,
		bool recursive = true);
	bool SetStyleEnvironment(
		std::shared_ptr<const ControlStyleSheet> theme,
		std::shared_ptr<const ControlStyleSheet> styles,
		bool recursive = true);
	bool SetResourceDictionary(
		std::shared_ptr<const ControlStyleSheet> value);
	bool HasVisibleStyleRules() const noexcept;
	bool RefreshStyleValues(bool recursive = true);

public:
	/** WPF-like lookup: self, logical ancestors, document, then theme/application. */
	bool TryFindResource(
		const std::wstring& resourceKey,
		BindingValue& value) const;
	/** Installs a WPF-like DynamicResource expression as a Local value. */
	#if CUI_ENABLE_DYNAMIC_XAML
	bool SetDynamicResource(
		const std::wstring& propertyName,
		std::wstring resourceKey);
	/** Removes a Local DynamicResource expression and its cached value. */
	bool ClearDynamicResource(const std::wstring& propertyName);
	bool TryGetDynamicResourceKey(
		const std::wstring& propertyName,
		std::wstring& resourceKey,
		DependencyPropertyValueSource source =
			DependencyPropertyValueSource::Local);
	#endif
	bool SetDynamicResource(
		const DependencyProperty& property,
		std::wstring resourceKey);
	bool ClearDynamicResource(const DependencyProperty& property);
	bool TryGetDynamicResourceKey(
		const DependencyProperty& property,
		std::wstring& resourceKey,
		DependencyPropertyValueSource source =
			DependencyPropertyValueSource::Local);
	/**
	 * Stable dependency-property identities used by CLR-shaped wrappers and
	 * build-time generated C++.  Keeping these identities public lets AOT
	 * templates write the Template precedence slot without a property-name
	 * lookup.
	 */
	static const DependencyProperty& IsEnabledProperty();
	static const DependencyProperty& IsVisibleProperty();
	static const DependencyProperty& ActualWidthProperty();
	static const DependencyProperty& ActualHeightProperty();
	static const DependencyProperty& ValidationHasErrorProperty();
	static const DependencyProperty& ValidationErrorsProperty();
	static const DependencyProperty& IsFocusedProperty();
	static const DependencyProperty& IsKeyboardFocusedProperty();
	static const DependencyProperty& IsKeyboardFocusVisibleProperty();
	static const DependencyProperty& IsKeyboardFocusWithinProperty();
	static const DependencyProperty& IsMouseOverProperty();
	static const DependencyProperty& IsMouseDirectlyOverProperty();
	static const DependencyProperty& IsMouseCapturedProperty();
	static const DependencyProperty& IsMouseCaptureWithinProperty();
	static const DependencyProperty& AllowDropProperty();
	static const DependencyProperty& VisibilityProperty();
	static const DependencyProperty& BackgroundProperty();
	static const DependencyProperty& ForegroundProperty();
	static const DependencyProperty& BorderBrushProperty();
	static const DependencyProperty& OpacityProperty();
	static const DependencyProperty& ClipToBoundsProperty();
	static const DependencyProperty& ClipProperty();
	static const DependencyProperty& RenderTransformProperty();
	static const DependencyProperty& RenderTransformOriginProperty();
	static const DependencyProperty& ZIndexProperty();
	static const DependencyProperty& TagProperty();
	static const DependencyProperty& FocusableProperty();
	static const DependencyProperty& IsTabStopProperty();
	static const DependencyProperty& TabIndexProperty();
	static const DependencyProperty& IsFocusScopeProperty();
	static const DependencyProperty& TabNavigationProperty();
	static const DependencyProperty& DirectionalNavigationProperty();
	static const DependencyProperty& CursorProperty();
	static const DependencyProperty& AutomationNameProperty();
	static const DependencyProperty& AutomationFullDescriptionProperty();
	static const DependencyProperty& AutomationHelpTextProperty();
	static const DependencyProperty& AutomationIdProperty();
	static const DependencyProperty& FontFamilyProperty();
	static const DependencyProperty& LanguageProperty();
	static const DependencyProperty& FontSizeProperty();
	static const DependencyProperty& WidthProperty();
	static const DependencyProperty& HeightProperty();
	static const DependencyProperty& MinWidthProperty();
	static const DependencyProperty& MinHeightProperty();
	static const DependencyProperty& MaxWidthProperty();
	static const DependencyProperty& MaxHeightProperty();
	static const DependencyProperty& BorderThicknessProperty();
	static const DependencyProperty& MarginProperty();
	static const DependencyProperty& PaddingProperty();
	static const DependencyProperty& HorizontalAlignmentProperty();
	static const DependencyProperty& VerticalAlignmentProperty();
	static const DependencyProperty& HorizontalContentAlignmentProperty();
	static const DependencyProperty& VerticalContentAlignmentProperty();
	static const DependencyProperty& CanvasLeftProperty();
	static const DependencyProperty& CanvasTopProperty();
	static const DependencyProperty& CanvasRightProperty();
	static const DependencyProperty& CanvasBottomProperty();
	static const DependencyProperty& GridRowProperty();
	static const DependencyProperty& GridColumnProperty();
	static const DependencyProperty& GridRowSpanProperty();
	static const DependencyProperty& GridColumnSpanProperty();
	static const DependencyProperty& DockPositionProperty();
	static const DependencyProperty& DataContextProperty();
	static const DependencyProperty& TemplateProperty();
	/** @brief Registers metadata owned by this runtime control type. */
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override { RegisterDependencyProperties(); }
#endif
	/** Number of implementation-owned visual children. This is not an authored
	 *  Items/Content collection; semantic containers expose their own API. */
	int VisualChildCount() const noexcept
	{
		return static_cast<int>(_visualChildren.size());
	}
	Control* GetVisualChild(int index) const noexcept;
	virtual std::span<Control* const> GetLayoutChildrenView() noexcept
	{
		return std::span<Control* const>{
			_visualChildren.data(), _visualChildren.size() };
	}
	std::vector<Control*> GetVisualChildrenInZOrder() const;
	std::vector<Control*> GetVisualChildrenInReverseZOrder() const;

	template<typename T>
	T AdoptVisualChild(T control) {
		return InsertVisualChild(
			static_cast<int>(_visualChildren.size()), control);
	}

	/** @brief 在指定位置挂载控件；成功后当前容器接管所有权。 */
	template<typename T>
	T InsertVisualChild(int index, T control) {
		if (!control)
			throw std::invalid_argument("不能添加空控件");
		if (index < 0 || static_cast<size_t>(index) > _visualChildren.size())
			throw std::out_of_range("子控件索引超出范围");
		for(auto& child : this->_visualChildren) {
			if (child == control)
				throw std::logic_error("不能重复添加同一控件");
		}
		for (Control* ancestor = this; ancestor;
			ancestor = ancestor->GetVisualParent()) {
			if (ancestor == control)
				throw std::logic_error("不能将控件添加到自身或其后代");
		}
		bool explicitLogicalParent = false;
		Control* requestedLogicalParent = nullptr;
		for (auto position = _pendingVisualChildAttachments.rbegin();
			position != _pendingVisualChildAttachments.rend(); ++position) {
			if (position->Child != control) continue;
			explicitLogicalParent = true;
			requestedLogicalParent = position->LogicalParent.Get();
			break;
		}
		if (control->_isWindowRoot
			|| control->_visualParent
			|| (control->_logicalParent
				&& (!explicitLogicalParent
					|| control->_logicalParent != requestedLogicalParent))
			|| (control->GetPresentationWindow() && control->GetPresentationWindow() != this->GetPresentationWindow())) {
			throw std::logic_error("该控件已属于其他容器");
		}
		this->_visualChildren.insert(
			this->_visualChildren.begin() + index, control);
		return control;
	}

	/** Reorders one owned visual child without exposing the backing collection. */
	bool MoveVisualChild(int oldIndex, int newIndex);

	/**
	 * @brief 接收一个尚未挂载的控件；挂载成功后由当前容器接管所有权。
	 * 校验或挂载抛异常时，unique_ptr 仍负责释放对象。
	 */
	template<typename T>
	T* AddOwned(std::unique_ptr<T> control)
	{
		return InsertOwned(
			static_cast<int>(_visualChildren.size()), std::move(control));
	}

	/** @brief 在指定位置接收一个未挂载控件并接管所有权。 */
	template<typename T>
	T* InsertOwned(int index, std::unique_ptr<T> control)
	{
		static_assert(std::is_base_of_v<Control, T>, "T must derive from Control");
		if (!control)
			throw std::invalid_argument("不能添加空控件");
		T* raw = control.release();
		const ControlWeakReference lifetime(raw);
		bool structuralCommit = false;
		try
		{
			(void)this->InsertVisualChildWithLogicalParent(
				index, raw, this, &structuralCommit);
		}
		catch (...)
		{
			auto* live = lifetime.Get();
			const bool requestedParentOwns = live
				&& this->IndexOfVisualChild(live) >= 0;
			if (requestedParentOwns
				&& live->GetVisualParent() == this
				&& live->GetLogicalParent() != this)
			{
				// Visual-parent publication may throw after the collection
				// already accepted the child. Finish the logical edge while
				// ownership remains with this container.
				try { live->SetLogicalParentCore(this); }
				catch (...) {}
			}
			// A post-commit callback may detach the child into an external
			// unique_ptr and leave its final VisualParent null. Only the
			// structural token can distinguish that transfer from a rejected
			// insertion; never reconstruct a second owner after commit.
			if (live && !structuralCommit && !requestedParentOwns
				&& !live->GetVisualParent())
				control.reset(static_cast<T*>(live));
			throw;
		}
		auto* live = lifetime.Get();
		if (!live)
			throw std::logic_error(
				"owned visual child was destroyed during attachment");
		if (live->GetVisualParent() != this
			|| this->IndexOfVisualChild(live) < 0
			|| live->GetLogicalParent() != this)
		{
			// The insertion committed, then a synchronous observer deliberately
			// moved the child again. The callback owns that transfer; return the
			// still-live identity instead of treating it as validation failure.
			if (structuralCommit)
				return static_cast<T*>(live);
			if (live && !structuralCommit && !live->GetVisualParent()
				&& this->IndexOfVisualChild(live) < 0)
				control.reset(static_cast<T*>(live));
			throw std::logic_error(
				"owned visual child attachment did not commit");
		}
		return static_cast<T*>(live);
	}

	/** Programmatic tree construction always starts from a property-neutral host. */
	template<typename T>
	T* Add()
	{
		static_assert(std::is_base_of_v<Control, T>, "T must derive from Control");
		return AddOwned(std::make_unique<T>());
	}
	
	/**
	 * @brief 从当前容器分离一个直接子控件，并将所有权交还给调用方。
	 * @return 成功时返回拥有该控件的 unique_ptr；child 不是直接子控件时返回空。
	 * 结构提交后的观察者异常不会撤销所有权转移；框架内部可通过
	 * TreeAccess 捕获该通知错误并在完成清理后重新抛出。
	 */
	std::unique_ptr<Control> DetachVisualChild(Control* child);
	/** @brief 按索引分离直接子控件；索引无效时返回空。 */
	std::unique_ptr<Control> DetachVisualChildAt(int index);

	/** @brief 类型安全的 DetachVisualChild 重载。 */
	template<typename T>
	std::unique_ptr<T> DetachVisualChild(T* child)
	{
		static_assert(std::is_base_of_v<Control, T>, "T must derive from Control");
		auto detached = DetachVisualChild(static_cast<Control*>(child));
		return std::unique_ptr<T>(static_cast<T*>(detached.release()));
	}

	/** @brief 从当前容器移除并销毁一个直接子控件。 */
	bool DeleteVisualChild(Control* child);
	bool DeleteVisualChildAt(int index);
	/** @brief 移除并销毁全部直接子控件。 */
	void ClearVisualChildren();
	int IndexOfVisualChild(const Control* child) const noexcept;
	bool ContainsControl(const Control* child) const noexcept
	{
		return IndexOfVisualChild(child) >= 0;
	}

private:
	/** Backing wrappers for WPF attached properties owned by Canvas. */
	PROPERTY(float, CanvasLeft);
	GET(float, CanvasLeft);
	SET(float, CanvasLeft);
	PROPERTY(float, CanvasTop);
	GET(float, CanvasTop);
	SET(float, CanvasTop);
	PROPERTY(float, CanvasRight);
	GET(float, CanvasRight);
	SET(float, CanvasRight);
	PROPERTY(float, CanvasBottom);
	GET(float, CanvasBottom);
	SET(float, CanvasBottom);

public:
	/** WPF-style specified dimensions. Auto is represented by Length::Auto(). */
	PROPERTY(cui::layout::Length, Width);
	GET(cui::layout::Length, Width);
	SET(cui::layout::Length, Width);
	PROPERTY(cui::layout::Length, Height);
	GET(cui::layout::Length, Height);
	SET(cui::layout::Length, Height);
	bool IsWidthAuto() const noexcept
	{
		return GetDependencyPropertyValue<cui::layout::Length>(
			WidthProperty()).IsAuto();
	}
	bool IsHeightAuto() const noexcept
	{
		return GetDependencyPropertyValue<cui::layout::Length>(
			HeightProperty()).IsAuto();
	}
	PROPERTY(float, MinWidth);
	GET(float, MinWidth);
	SET(float, MinWidth);
	PROPERTY(float, MinHeight);
	GET(float, MinHeight);
	SET(float, MinHeight);
	PROPERTY(float, MaxWidth);
	GET(float, MaxWidth);
	SET(float, MaxWidth);
	PROPERTY(float, MaxHeight);
	GET(float, MaxHeight);
	SET(float, MaxHeight);
	READONLY_PROPERTY(float, ActualWidth);
	GET(float, ActualWidth);
	READONLY_PROPERTY(float, ActualHeight);
	GET(float, ActualHeight);
	PROPERTY(Thickness, BorderThickness);
	GET(Thickness, BorderThickness);
	SET(Thickness, BorderThickness);
	// 布局属性访问器
	PROPERTY(Thickness, Margin);
	GET(Thickness, Margin);
	SET(Thickness, Margin);
	PROPERTY(Thickness, Padding);
	GET(Thickness, Padding);
	SET(Thickness, Padding);
	PROPERTY(::HorizontalAlignment, HorizontalAlignment);
	GET(::HorizontalAlignment, HorizontalAlignment);
	SET(::HorizontalAlignment, HorizontalAlignment);
	PROPERTY(::VerticalAlignment, VerticalAlignment);
	GET(::VerticalAlignment, VerticalAlignment);
	SET(::VerticalAlignment, VerticalAlignment);
	PROPERTY(::HorizontalAlignment, HorizontalContentAlignment);
	GET(::HorizontalAlignment, HorizontalContentAlignment);
	SET(::HorizontalAlignment, HorizontalContentAlignment);
	PROPERTY(::VerticalAlignment, VerticalContentAlignment);
	GET(::VerticalAlignment, VerticalContentAlignment);
	SET(::VerticalAlignment, VerticalContentAlignment);
private:
	/** Backing wrappers for attached properties owned by Grid and DockPanel. */
	PROPERTY(int, GridRow);
	GET(int, GridRow);
	SET(int, GridRow);
	PROPERTY(int, GridColumn);
	GET(int, GridColumn);
	SET(int, GridColumn);
	PROPERTY(int, GridRowSpan);
	GET(int, GridRowSpan);
	SET(int, GridRowSpan);
	PROPERTY(int, GridColumnSpan);
	GET(int, GridColumnSpan);
	SET(int, GridColumnSpan);
	PROPERTY(::Dock, DockPosition);
	GET(::Dock, DockPosition);
	SET(::Dock, DockPosition);

public:
	cui::core::Size GetMinSizeDip() const noexcept;
	void SetMinSizeDip(cui::core::Size value);
	cui::core::Size GetMaxSizeDip() const noexcept;
	void SetMaxSizeDip(cui::core::Size value);
	
	/**
	 * @brief 测量阶段：返回控件期望尺寸。
	 * @param availableSize 可用空间（由父布局提供）。
	 */
	virtual cui::core::Size MeasureCore(const cui::core::Constraints& available);
	/**
	 * @brief Cached measure entry point used by layout containers.
	 *
	 * Derived controls override the float-DIP MeasureCore entry point; the
	 * wrapper owns dirty-state and constraint caching.
	 */
	cui::core::Size Measure(const cui::core::Constraints& available);
	/**
	 * @brief 立即执行当前控件类型的待处理布局。
	 *
	 * 与直接调用 Panel::PerformLayout 不同，此入口会保留 GroupBox、
	 * Expander、ScrollViewer 等派生容器的专用内容区语义。
	 */
	void UpdateLayout() { PerformPendingLayout(); }
	/**
	 * Read-only snapshot of the effective WPF layout declarations. The
	 * dependency-property store remains their single source of truth.
	 */
	cui::layout::LayoutStyle GetSpecifiedLayout() const;
	const cui::layout::LayoutState& GetComputedLayout() const { return _layoutState; }
	cui::core::Size GetDesiredSizeDip() const noexcept { return _layoutState.desiredSize; }
	cui::core::Point GetActualLocationDip() const;
	cui::core::Size GetActualSizeDip() const override;
	cui::core::Point GetAbsoluteLocationDip() const;
	cui::core::Rect GetAbsoluteRectDip() const;
	D2D1_RECT_F GetAbsoluteBoundsDip() const;
	/**
	 * @brief 可选的后代绘制变换，输入和输出都位于窗体内容区 DIP 坐标。
	 *
	 * 容器可借此实现只影响视图、不改写布局数据的缩放和平移。控件绘制、
	 * 脏矩形和 DComp 裁剪会统一继承该变换。
	 */
	virtual bool TryGetDescendantRenderTransform(
		D2D1_MATRIX_3X2_F& transform) const
	{
		transform = D2D1::Matrix3x2F::Identity();
		return false;
	}
	/** @brief 返回所有祖先为当前控件提供的内容区绘制变换。 */
	D2D1_MATRIX_3X2_F GetInheritedRenderTransform() const;
	/** @brief 返回当前控件局部坐标到实际内容区绘制坐标的完整变换。 */
	D2D1_MATRIX_3X2_F GetLocalToRenderTransform() const;
	/** @brief 把实际绘制坐标反变换为当前控件局部坐标；奇异矩阵返回 false。 */
	bool TryTransformRenderPointToLocal(
		D2D1_POINT_2F renderPoint,
		D2D1_POINT_2F& localPoint) const;
	/** Returns false when the point is outside this control or any ancestor Clip. */
	bool IsRenderPointInsideClip(D2D1_POINT_2F renderPoint) const;
	/** @brief 将绝对内容区矩形映射到当前控件实际的绘制空间。 */
	D2D1_RECT_F TransformAbsoluteRectToRenderSpace(
		const D2D1_RECT_F& rect) const;
	/** @brief 返回应用祖先绘制变换后的绝对内容区边界。 */
	D2D1_RECT_F GetRenderedAbsoluteRectDip();
	/** Commits the sole computed layout rectangle. */
	void Arrange(cui::core::Rect finalRect) override;

	/** WPF-style inherited pointer cursor; authored values participate in the DP pipeline. */
	PROPERTY(CursorKind, Cursor);
	GET(CursorKind, Cursor);
	SET(CursorKind, Cursor);
	/**
	 * Resolves the effective pointer cursor. Any concrete authored/style/theme/
	 * inherited value wins; only Auto delegates to the native behavior hook.
	 */
	CursorKind ResolvePointerCursor(int localX, int localY);
	/**
	 * @brief 根据命中区域返回原生行为建议的光标类型。
	 * @param localX 相对于控件客户区的 X。
	 * @param localY 相对于控件客户区的 Y。
	 */
	virtual CursorKind QueryCursor(int localX, int localY)
	{
		(void)localX;
		(void)localY;
		return CursorKind::Arrow;
	}
	virtual bool TryGetSystemCursorId(UINT32& outId) const { (void)outId; return false; }
	virtual bool ContainsPoint(int localX, int localY)
	{
		auto actualSize = this->GetActualSizeDip();
		return localX >= 0 && localY >= 0
			&& (float)localX <= actualSize.width
			&& (float)localY <= actualSize.height;
	}
	/** False for visuals presented exclusively through a separate overlay/native
	 *  surface. Their subtree is omitted from the retained main scene. */
	virtual bool ParticipatesInPresentationScene() const
	{
		return _participatesInPresentationScene;
	}
	/** Internal input hit-test policy. False keeps the visual rendered while
	 *  allowing its nearest hit-testable ancestor to receive pointer input. */
	virtual bool ParticipatesInInputHitTesting() const noexcept { return true; }
	virtual bool HitTestChildren() const { return true; }
	virtual bool ShouldHitTestChildrenAt(int localX, int localY) const { (void)localX; (void)localY; return this->HitTestChildren(); }
	/**
	 * @brief 子控件布局内容区相对当前控件的原点（DIP）。
	 *
	 * 设计器和容器布局都可用它在“容器局部坐标”与“子控件布局坐标”
	 * 之间换算。滚动等仅影响绘制的位置仍由 GetVisualChildrenRenderOffset 表达。
	 */
	virtual cui::core::Point GetVisualChildrenLayoutOriginDip() { return {}; }
	virtual cui::core::Point GetVisualChildrenRenderOffset() const { return {}; }
	virtual bool ClipsChildren() { return ClipToBounds; }
	virtual D2D1_RECT_F GetVisualChildrenClipRect()
	{
		auto actualSize = this->GetActualSizeDip();
		return D2D1_RECT_F{
			0.0f, 0.0f, actualSize.width, actualSize.height };
	}
	virtual bool HandlesMouseWheel() const { return false; }
	virtual bool CanHandleMouseWheel(int delta, int localX, int localY) { (void)delta; (void)localX; (void)localY; return false; }
	virtual bool HandlesNavigationKey(Key key) const { (void)key; return false; }

protected:
	/** Constraint-dependent cache preparation before the derived measure pass. */
	virtual void PrepareMeasureCore(
		const cui::core::Constraints& available)
	{
		(void)available;
	}
	/**
	 * Finalizes the intrinsic size after either a ControlTemplate root or the
	 * derived MeasureCore has been measured. The default preserves that size.
	 */
	virtual cui::core::Size FinalizeMeasureCore(
		cui::core::Size intrinsic,
		const cui::core::Constraints& available)
	{
		(void)available;
		return intrinsic;
	}
	/** WPF AccessKeyManager callback. */
	virtual bool OnAccessKey(bool isMultiple)
	{
		(void)isMultiple;
		return Invoke();
	}
	void SetPresentationOrderOverride(int order) { _hasPresentationOrderOverride = true; _presentationOrderOverride = order; }
	void ClearPresentationOrderOverride() { _hasPresentationOrderOverride = false; }
	bool TryGetPresentationOrderOverride(int& order) const { if (!_hasPresentationOrderOverride) return false; order = _presentationOrderOverride; return true; }
	/** Applies normalized committed text through behavior, then control logic. */
	bool DispatchTextInput(TextCompositionEventArgs& input);
	/** Resolves behavior-provided or built-in caret geometry. */
	bool ResolveTextInputCaretRect(D2D1_RECT_F& outRect);
	/** Built-in text-client hook; native messages never reach this method. */
	virtual bool ApplyTextInput(const TextCompositionEventArgs& input)
	{
		(void)input;
		return false;
	}
	/** Returns the active caret in top-level client DIPs for IME placement. */
	virtual bool TryGetTextInputCaretRect(D2D1_RECT_F& outRect)
	{
		(void)outRect;
		return false;
	}
	virtual bool ProcessInput(const InputReport& input);
	/** Framework-only behavior dispatch after Window InputManager staging. */
	bool DispatchInput(const InputReport& input);
};

#endif // CUI_CONTROL_H_INCLUDED
