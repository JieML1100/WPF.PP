#include "Control.h"
#include "EventInfrastructure.h"
#include "Binding.h"
#include "Window.h"
#include "WindowInfrastructure.h"
#include "Panel.h"
#include "Border.h"
#include "TextElement.h"
#include "PropertyPath.h"
#include "Style.h"
#include "Core/Threading.h"
#include "InputManager.h"
#include "ReverseInheritedProperty.h"
#include "TreeInfrastructure.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <numbers>
#include <cwctype>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <variant>

#pragma warning(disable: 4267)
#pragma warning(disable: 4244)
#pragma warning(disable: 4018)

#if CUI_ENABLE_DYNAMIC_XAML
namespace cui::framework::design
{
	bool ResolveVisualStateAnimationOperands(
		const DeclarativeVisualStateAnimation& source,
		Control& owner,
		Control*& target,
		const DependencyPropertyMetadata*& metadata,
		std::optional<CompiledStoryboardObjectPathOp>& objectPath,
		std::vector<uint32_t>& objectPathChildIndices,
		std::wstring& propertyPath,
		std::wstring* outError);
}
#endif

namespace
{
	thread_local std::span<Control* const>
		RenderTransformSuppressionRoots{};

	bool IsRenderTransformSuppressedForRecording(
		const Control* control) noexcept
	{
		return control && std::find(
			RenderTransformSuppressionRoots.begin(),
			RenderTransformSuppressionRoots.end(), control)
			!= RenderTransformSuppressionRoots.end();
	}

	/**
	 * Visual/inheritance chains are normally fewer than a dozen controls deep.
	 * Using unordered_set for cycle protection on every coordinate transform or
	 * damage dispatch therefore paid a heap allocation for the overwhelmingly
	 * common acyclic case. Keep the exact cycle contract while storing ordinary
	 * chains inline; only a genuinely deep tree falls back to hashing.
	 */
	template<typename TObject, size_t InlineCapacity = 32>
	class InlinePointerSet final
	{
	public:
		bool Insert(TObject* value)
		{
			if (_overflow) return _overflow->insert(value).second;
			for (size_t index = 0; index < _count; ++index)
				if (_items[index] == value) return false;
			if (_count < InlineCapacity)
			{
				_items[_count++] = value;
				return true;
			}
			_overflow = std::make_unique<std::unordered_set<TObject*>>();
			_overflow->reserve(InlineCapacity * 2);
			for (size_t index = 0; index < _count; ++index)
				_overflow->insert(_items[index]);
			return _overflow->insert(value).second;
		}

	private:
		std::array<TObject*, InlineCapacity> _items;
		size_t _count = 0;
		std::unique_ptr<std::unordered_set<TObject*>> _overflow;
	};

	struct SystemMessageFontDefaults final
	{
		std::wstring Family = L"Segoe UI";
		double Size = 12.0;
	};

	const SystemMessageFontDefaults& GetSystemMessageFontDefaults()
	{
		static const auto defaults = []
		{
			SystemMessageFontDefaults result;
			NONCLIENTMETRICSW metrics{};
			metrics.cbSize = sizeof(metrics);
			if (!::SystemParametersInfoW(
				SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
				return result;
			if (metrics.lfMessageFont.lfFaceName[0] != L'\0')
				result.Family = metrics.lfMessageFont.lfFaceName;

			UINT dpi = 96;
			if (const auto getDpiForSystem = reinterpret_cast<UINT(WINAPI*)()>(
				::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
					"GetDpiForSystem")))
				dpi = (std::max)(96U, getDpiForSystem());
			const auto pixelHeight = std::abs(metrics.lfMessageFont.lfHeight);
			if (pixelHeight > 0)
				result.Size = static_cast<double>(pixelHeight) * 96.0
					/ static_cast<double>(dpi);
			return result;
		}();
		return defaults;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	template<typename TValue>
	TValue GetProjectedPropertyValueOr(
		const DependencyObject& target,
		const std::wstring& propertyName,
		TValue fallback)
	{
		BindingValue value;
		TValue typed{};
		return target.TryGetValue(propertyName, value)
			&& value.TryGet(typed)
			? typed : std::move(fallback);
	}
#endif

	template<typename TAction>
	struct ControlScopeExit final
	{
		TAction Action;
		~ControlScopeExit() { Action(); }
	};

	std::atomic<uint32_t> NextAccessibilityRuntimeId{ 1 };
	std::atomic<uint32_t> NextAccessibilityVirtualRuntimeId{ 1 };

	std::vector<std::pair<ControlWeakReference, bool>>
		CaptureEffectiveIsEnabledSubtree(Control& root);
	std::vector<std::pair<ControlWeakReference, bool>>
		CaptureEffectiveIsVisibleSubtree(Control& root);

	uint32_t AllocateAccessibilityRuntimeId() noexcept
	{
		uint32_t value = NextAccessibilityRuntimeId.fetch_add(
			1, std::memory_order_relaxed);
		if (value == 0)
			value = NextAccessibilityRuntimeId.fetch_add(
				1, std::memory_order_relaxed);
		return value;
	}

	D2D1::Matrix3x2F AsMatrix(const D2D1_MATRIX_3X2_F& value) noexcept
	{
		return D2D1::Matrix3x2F(
			value._11, value._12, value._21,
			value._22, value._31, value._32);
	}

	bool IsFiniteMatrix(const D2D1_MATRIX_3X2_F& value) noexcept
	{
		return std::isfinite(value._11) && std::isfinite(value._12)
			&& std::isfinite(value._21) && std::isfinite(value._22)
			&& std::isfinite(value._31) && std::isfinite(value._32);
	}

	int StoredPropertySourceIndex(DependencyPropertyValueSource source) noexcept
	{
		const int value = static_cast<int>(source);
		return value >= static_cast<int>(DependencyPropertyValueSource::Inherited)
			&& value <= static_cast<int>(DependencyPropertyValueSource::Animation)
			? value - static_cast<int>(DependencyPropertyValueSource::Inherited)
			: -1;
	}

	cui::layout::Alignment ToLayoutAlignment(HorizontalAlignment value)
	{
		switch (value)
		{
		case HorizontalAlignment::Center: return cui::layout::Alignment::Center;
		case HorizontalAlignment::Right: return cui::layout::Alignment::End;
		case HorizontalAlignment::Stretch: return cui::layout::Alignment::Stretch;
		case HorizontalAlignment::Left:
		default: return cui::layout::Alignment::Start;
		}
	}

	cui::layout::Alignment ToLayoutAlignment(VerticalAlignment value)
	{
		switch (value)
		{
		case VerticalAlignment::Center: return cui::layout::Alignment::Center;
		case VerticalAlignment::Bottom: return cui::layout::Alignment::End;
		case VerticalAlignment::Stretch: return cui::layout::Alignment::Stretch;
		case VerticalAlignment::Top:
		default: return cui::layout::Alignment::Start;
		}
	}

	cui::core::Constraints ElementSizeConstraints(
		const cui::layout::LayoutStyle& style)
	{
		auto result = style.SizeConstraints();
		const auto applyExplicitLength = [](
			const cui::layout::Length& length,
			float& minimum,
			float& maximum)
			{
				if (!length.IsFixed()) return;
				maximum = (std::max)(
					(std::min)(length.value, maximum), minimum);
				minimum = (std::max)(
					(std::min)(maximum, length.value), minimum);
			};
		applyExplicitLength(
			style.width, result.minimum.width, result.maximum.width);
		applyExplicitLength(
			style.height, result.minimum.height, result.maximum.height);
		return result;
	}

	cui::core::Dip ToMaximumDip(LONG value)
	{
		return value >= INT_MAX
			? cui::core::Infinity
			: (std::max)(0.0f, (float)value);
	}

	LONG ToLayoutLong(cui::core::Dip value)
	{
		if (!(value > 0.0f)) return 0;
		const auto maximum = (cui::core::Dip)(std::numeric_limits<LONG>::max)();
		return value >= maximum ? (std::numeric_limits<LONG>::max)() : (LONG)value;
	}

	LONG ToMeasureLong(cui::core::Dip value)
	{
		if (!(value > 0.0f)) return 0;
		const auto maximum = (cui::core::Dip)(std::numeric_limits<LONG>::max)();
		return value >= maximum
			? (std::numeric_limits<LONG>::max)()
			: static_cast<LONG>(std::ceil(value));
	}

	LONG ToCoordinateLong(cui::core::Dip value)
	{
		if (value != value) return 0;
		const auto maximum = (cui::core::Dip)(std::numeric_limits<LONG>::max)();
		const auto minimum = (cui::core::Dip)(std::numeric_limits<LONG>::min)();
		if (value >= maximum) return (std::numeric_limits<LONG>::max)();
		if (value <= minimum) return (std::numeric_limits<LONG>::min)();
		return (LONG)value;
	}

	std::optional<cui::layout::Length> ConvertLayoutLength(
		const BindingValue& value)
	{
		cui::layout::Length exact;
		if (value.TryGet(exact))
		{
			if (exact.IsAuto()) return cui::layout::Length::Auto();
			if (!exact.IsFixed() || !std::isfinite(exact.value)
				|| exact.value < 0.0f) return std::nullopt;
			return cui::layout::Length::Fixed(exact.value);
		}

		BindingValue numericValue = value;
		std::wstring text;
		if (value.Kind() == BindingValueKind::String
			&& value.TryGetString(text))
		{
			const auto first = std::find_if_not(
				text.begin(), text.end(), [](wchar_t ch) { return std::iswspace(ch); });
			const auto last = std::find_if_not(
				text.rbegin(), text.rend(), [](wchar_t ch) { return std::iswspace(ch); }).base();
			text = first < last ? std::wstring(first, last) : std::wstring{};
			if (_wcsicmp(text.c_str(), L"Auto") == 0)
				return cui::layout::Length::Auto();
			numericValue = BindingValue(std::move(text));
		}

		float numeric = 0.0f;
		if (!numericValue.TryGetFloat(numeric)
			|| !std::isfinite(numeric) || numeric < 0.0f)
			return std::nullopt;
		return cui::layout::Length::Fixed(numeric);
	}

	cui::core::Rect ToCoreRect(D2D1_RECT_F value)
	{
		return cui::core::Rect::FromLTRB(
			value.left, value.top, value.right, value.bottom);
	}

	D2D1_RECT_F ToD2DRect(cui::core::Rect value)
	{
		return D2D1_RECT_F{
			value.Left(), value.Top(), value.Right(), value.Bottom() };
	}

	int ValidationSeverityRank(BindingValidationSeverity severity) noexcept
	{
		switch (severity)
		{
		case BindingValidationSeverity::Info: return 0;
		case BindingValidationSeverity::Warning: return 1;
		case BindingValidationSeverity::Error: return 2;
		}
		return 0;
	}

	D2D1_COLOR_F DefaultValidationColor(
		BindingValidationSeverity severity) noexcept
	{
		switch (severity)
		{
		case BindingValidationSeverity::Info:
			return D2D1_COLOR_F{ 0.12f, 0.52f, 0.88f, 1.0f };
		case BindingValidationSeverity::Warning:
			return D2D1_COLOR_F{ 0.95f, 0.62f, 0.12f, 1.0f };
		case BindingValidationSeverity::Error:
		default:
			return D2D1_COLOR_F{ 0.90f, 0.20f, 0.24f, 1.0f };
		}
	}

#if CUI_ENABLE_DESIGN_METADATA
	DependencyPropertyDesignMetadata PropertyDesign(
		std::wstring category,
		int categoryOrder,
		int order,
		DependencyPropertyPersistence persistence,
		DependencyPropertyEditorKind editor = DependencyPropertyEditorKind::Auto,
		std::wstring displayName = {})
	{
		DependencyPropertyDesignMetadata design;
		design.DisplayName = std::move(displayName);
		design.Category = std::move(category);
		design.CategoryOrder = categoryOrder;
		design.Order = order;
		design.Editor = editor;
		design.Persistence = persistence;
		return design;
	}

	template<typename TValue>
	DependencyPropertyChoice PropertyChoice(std::wstring displayName, TValue value)
	{
		return { std::move(displayName), BindingValue(std::move(value)) };
	}
#endif

	std::optional<cui::drawing::Brush> ConvertControlBrushValue(
		const BindingValue& value)
	{
		cui::drawing::Brush brush;
		if (value.TryGet(brush)) return brush;
		D2D1_COLOR_F color{};
		if (value.TryGet(color))
			return cui::drawing::MakeSolidColorBrush(color);
		return std::nullopt;
	}

	std::wstring StripAccessKeyMarkers(const std::wstring& text)
	{
		std::wstring result;
		result.reserve(text.size());
		for (size_t index = 0; index < text.size(); ++index)
		{
			if (text[index] != L'_')
			{
				result.push_back(text[index]);
				continue;
			}
			if (index + 1 >= text.size())
			{
				result.push_back(L'_');
				continue;
			}
			if (text[index + 1] == L'_')
			{
				result.push_back(L'_');
				++index;
			}
		}
		return result;
	}

	wchar_t FindAccessKeyMarker(const std::wstring& text)
	{
		for (size_t index = 0; index + 1 < text.size(); ++index)
		{
			if (text[index] != L'_') continue;
			if (text[index + 1] == L'_')
			{
				++index;
				continue;
			}
			if (!std::iswspace(text[index + 1]))
				return static_cast<wchar_t>(std::towupper(text[index + 1]));
		}
		return L'\0';
	}
}

uint32_t AllocateAccessibilityVirtualId() noexcept
{
	uint32_t value = NextAccessibilityVirtualRuntimeId.fetch_add(
		1, std::memory_order_relaxed);
	if (value == 0)
		value = NextAccessibilityVirtualRuntimeId.fetch_add(
			1, std::memory_order_relaxed);
	return value;
}

#if CUI_ENABLE_DYNAMIC_XAML
struct Control::DynamicXamlPropertyState final
{
	std::shared_ptr<const DeclarativeType> TypeDescriptor;
	std::vector<BindingValue> PropertyValues;
};

const std::shared_ptr<const Control::DeclarativeType>&
Control::GetDeclarativeTypeDescriptor() const noexcept
{
	static const std::shared_ptr<const DeclarativeType> empty;
	return _dynamicXamlPropertyState
		? _dynamicXamlPropertyState->TypeDescriptor
		: empty;
}

void Control::InstallDynamicXamlPropertyState(
	std::shared_ptr<const DeclarativeType> descriptor,
	std::vector<BindingValue> values)
{
	auto state = std::make_unique<DynamicXamlPropertyState>();
	state->TypeDescriptor = std::move(descriptor);
	state->PropertyValues = std::move(values);
	_dynamicXamlPropertyState = std::move(state);
}

bool Control::TryReadDynamicXamlPropertySlot(
	const DeclarativeType& owner,
	std::size_t slot,
	BindingValue& out) const
{
	const auto* state = _dynamicXamlPropertyState.get();
	if (!state || state->TypeDescriptor.get() != &owner
		|| slot >= state->PropertyValues.size()) return false;
	out = state->PropertyValues[slot];
	return true;
}

bool Control::TryWriteDynamicXamlPropertySlot(
	const DeclarativeType& owner,
	std::size_t slot,
	const BindingValue& value)
{
	auto* state = _dynamicXamlPropertyState.get();
	if (!state || state->TypeDescriptor.get() != &owner
		|| slot >= state->PropertyValues.size()) return false;
	state->PropertyValues[slot] = value;
	return true;
}
#endif

Control::Control()
{
	const auto& systemFont = GetSystemMessageFontDefaults();
	_fontName = systemFont.Family;
	_fontSize = systemFont.Size;
	_visualChildren.SetOwnerSynchronizationDuringUpdates(true);
	_visualChildren.SetOwnerChangedHandler(
		[this](const CollectionChangedEventArgs& change)
		{ SynchronizeVisualChildCollection(change); });
	this->_accessibilityRuntimeId = AllocateAccessibilityRuntimeId();
	_styleStateConnections.reserve(2);
	_styleStateConnections.push_back(OnMouseDown.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.OriginalSource == this
				&& (DefaultRaiseClickOnLeftButtonUp() || Focusable))
				SetStyleState(ControlStyleState::Pressed, true);
		}));
	_styleStateConnections.push_back(OnMouseUp.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.OriginalSource == this)
				SetStyleState(ControlStyleState::Pressed, false);
		}));
}

std::unique_ptr<AutomationPeer> Control::OnCreateAutomationPeer()
{
	return std::make_unique<AutomationPeer>(
		*this, AutomationControlType::Custom, L"Control");
}

std::shared_ptr<AutomationPeer> Control::AcquireAutomationPeer() const
{
	const ControlWeakReference ownerLifetime(
		const_cast<Control*>(this));
	if (const auto established = _automationPeer) return established;
	auto peer = const_cast<Control*>(this)->OnCreateAutomationPeer();
	auto* live = ownerLifetime.Get();
	if (!live) return {};
	if (!peer)
		peer = std::make_unique<AutomationPeer>(
			*live, AutomationControlType::Custom, L"Control");
	live = ownerLifetime.Get();
	if (!live) return {};
	if (!live->_automationPeer)
		live->_automationPeer = std::shared_ptr<AutomationPeer>(
			std::move(peer));
	return live->_automationPeer;
}

AutomationPeer& Control::GetAutomationPeer() const
{
	auto peer = AcquireAutomationPeer();
	if (!peer)
	{
		throw std::runtime_error(
			"AutomationPeer owner was destroyed during peer creation");
	}
	return *peer;
}

Control::~Control()
{
	// A routed callback may delete this control. Publish death before Control's
	// teardown mutates any tree/event storage so frozen routes cannot re-enter a
	// partially destroyed base object.
	InvalidateLifetimeToken();
	InvalidateCommandInfrastructureForDestruction();
	_isDestroying = true;
#if CUI_ENABLE_DYNAMIC_XAML
	ClearDeclarativeComponentBehavior();
#endif
	_declarativeVisualStates.reset();
	_visualChildren.SetOwnerChangedHandler({});
	if (auto* inheritanceParent = GetInheritanceParent())
		inheritanceParent->UnregisterInheritanceChild(this);
	if (_logicalParent)
	{
		auto& siblings = _logicalParent->_logicalChildren;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), this),
			siblings.end());
		_logicalParent = nullptr;
	}
	const auto logicalChildren = _logicalChildren;
	for (auto* child : logicalChildren)
		if (child && child->_logicalParent == this)
			child->SetLogicalParentCore(nullptr);
	const auto inheritanceChildren = _inheritanceChildren;
	for (auto* child : inheritanceChildren)
		if (child && child->_templatedParent == this)
			child->SetTemplatedParentCore(nullptr);
	_logicalChildren.clear();
	_inheritanceChildren.clear();
	_templateNameScope.clear();
#if CUI_ENABLE_DYNAMIC_XAML
	_templateNameScopeNames.clear();
#endif
	_dataBindings.reset();
	for (auto child : this->_visualChildren)
	{
		if (child && child->_visualParent == this)
			child->_visualParent = nullptr;
		delete child;
	}
	_observedVisualChildren.clear();
}
UIClass GetUIClassBase(UIClass type) noexcept
{
	switch (type)
	{
	case UIClass::UI_FrameworkElement:
		return UIClass::UI_Base;
	case UIClass::UI_Control:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_Button:
		return UIClass::UI_ButtonBase;
	case UIClass::UI_CheckBox:
	case UIClass::UI_RadioButton:
	case UIClass::UI_Switch:
		return UIClass::UI_ToggleButton;
	case UIClass::UI_ToggleButton:
		return UIClass::UI_ButtonBase;
	case UIClass::UI_ButtonBase:
		return UIClass::UI_ContentControl;
	case UIClass::UI_Slider:
	case UIClass::UI_ProgressBar:
	case UIClass::UI_ProgressRing:
	case UIClass::UI_NumericUpDown:
		return UIClass::UI_RangeBase;
	case UIClass::UI_RangeBase:
		return UIClass::UI_Control;
	case UIClass::UI_TextBox:
	case UIClass::UI_RichTextBox:
		return UIClass::UI_TextBoxBase;
	case UIClass::UI_TextBoxBase:
		return UIClass::UI_Control;
	case UIClass::UI_ListViewItem:
		return UIClass::UI_ListBoxItem;
	case UIClass::UI_ListBoxItem:
	case UIClass::UI_StatusBarItem:
	case UIClass::UI_Window:
		return UIClass::UI_ContentControl;
	case UIClass::UI_ComboBoxItem:
		return UIClass::UI_ListBoxItem;
	case UIClass::UI_GroupBox:
	case UIClass::UI_Expander:
	case UIClass::UI_TabItem:
		return UIClass::UI_HeaderedContentControl;
	case UIClass::UI_HeaderedContentControl:
		return UIClass::UI_ContentControl;
	case UIClass::UI_ScrollViewer:
		return UIClass::UI_ContentControl;
	case UIClass::UI_Popup:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_ContentControl:
		return UIClass::UI_Control;
	case UIClass::UI_ContentPresenter:
	case UIClass::UI_ItemsPresenter:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_Border:
		return UIClass::UI_Decorator;
	case UIClass::UI_Decorator:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_Grid:
	case UIClass::UI_StackPanel:
	case UIClass::UI_DockPanel:
	case UIClass::UI_WrapPanel:
	case UIClass::UI_RelativePanel:
	case UIClass::UI_Canvas:
		return UIClass::UI_Panel;
	case UIClass::UI_Panel:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_ItemsControl:
		return UIClass::UI_Control;
	case UIClass::UI_HeaderedItemsControl:
		return UIClass::UI_ItemsControl;
	case UIClass::UI_StatusBar:
	case UIClass::UI_Menu:
	case UIClass::UI_ContextMenu:
	case UIClass::UI_TreeView:
		return UIClass::UI_ItemsControl;
	case UIClass::UI_ToolBar:
		return UIClass::UI_HeaderedItemsControl;
	case UIClass::UI_MenuItem:
	case UIClass::UI_TreeViewItem:
		return UIClass::UI_HeaderedItemsControl;
	case UIClass::UI_Separator:
		return UIClass::UI_Control;
	case UIClass::UI_Selector:
		return UIClass::UI_ItemsControl;
	case UIClass::UI_ListBox:
	case UIClass::UI_ComboBox:
	case UIClass::UI_TabControl:
		return UIClass::UI_Selector;
	case UIClass::UI_ListView:
		return UIClass::UI_ListBox;
	case UIClass::UI_DataGrid:
		return UIClass::UI_ListBox;
	case UIClass::UI_DataGridRow:
		return UIClass::UI_ListBoxItem;
	case UIClass::UI_DataGridCell:
		return UIClass::UI_ContentControl;
	case UIClass::UI_DataGridColumnHeader:
		return UIClass::UI_Button;
	case UIClass::UI_DataGridColumnHeadersPresenter:
		return UIClass::UI_Grid;
	case UIClass::UI_DataGridRowHeader:
		return UIClass::UI_Button;
	case UIClass::UI_Label:
	case UIClass::UI_Image:
	case UIClass::UI_MediaElement:
	case UIClass::UI_WebBrowser:
	case UIClass::UI_NativeSurface:
		return UIClass::UI_FrameworkElement;
	case UIClass::UI_Calendar:
		return UIClass::UI_CalendarView;
	case UIClass::UI_DatePicker:
		return UIClass::UI_Control;
	case UIClass::UI_Base:
		return UIClass::UI_Base;
	case UIClass::UI_CUSTOM:
		return UIClass::UI_Control;
	default:
		return UIClass::UI_Control;
	}
}

int GetUIClassInheritanceDistance(
	UIClass baseType,
	UIClass type) noexcept
{
	int distance = 0;
	for (;;)
	{
		if (type == baseType) return distance;
		if (type == UIClass::UI_Base) return -1;
		const auto next = GetUIClassBase(type);
		if (next == type) return -1;
		type = next;
		++distance;
	}
}

bool IsUIClassAssignableFrom(UIClass baseType, UIClass type) noexcept
{
	return GetUIClassInheritanceDistance(baseType, type) >= 0;
}

namespace
{
#if CUI_ENABLE_DYNAMIC_XAML
	bool IsFrameworkElementNativeProperty(std::wstring_view name) noexcept
	{
		static constexpr std::wstring_view properties[] = {
			L"DataContext",
			L"Language",
			L"Visibility",
			L"IsVisible",
			L"IsEnabled",
			L"AllowDrop",
			L"Canvas.Left",
			L"Canvas.Top",
			L"Canvas.Right",
			L"Canvas.Bottom",
			L"Width",
			L"Height",
			L"ActualWidth",
			L"ActualHeight",
			L"Margin",
			L"HorizontalAlignment",
			L"VerticalAlignment",
			L"ZIndex",
			L"Grid.Row",
			L"Grid.Column",
			L"Grid.RowSpan",
			L"Grid.ColumnSpan",
			L"DockPanel.Dock",
			L"MinWidth",
			L"MinHeight",
			L"MaxWidth",
			L"MaxHeight",
			L"Opacity",
			L"ClipToBounds",
			L"Clip",
			L"RenderTransform",
			L"RenderTransformOrigin",
			L"Validation.HasError",
			L"Validation.Errors",
			L"Tag",
			L"Cursor",
			L"Focusable",
			L"IsTabStop",
			L"TabIndex",
			L"IsFocused",
			L"IsKeyboardFocused",
			L"IsKeyboardFocusVisible",
			L"IsKeyboardFocusWithin",
			L"IsMouseOver",
			L"IsMouseDirectlyOver",
			L"IsMouseCaptured",
			L"IsMouseCaptureWithin",
			L"FocusManager.IsFocusScope",
			L"KeyboardNavigation.TabNavigation",
			L"KeyboardNavigation.DirectionalNavigation",
			L"AutomationProperties.Name",
			L"AutomationProperties.FullDescription",
			L"AutomationProperties.HelpText",
			L"AutomationProperties.AutomationId",
		};
		return std::find(
			std::begin(properties), std::end(properties), name)
			!= std::end(properties);
	}

	bool IsTextBlockNativeProperty(std::wstring_view name) noexcept
	{
		return name == L"FontFamily"
			|| name == L"FontSize"
			|| name == L"Padding";
	}
#else
	bool IsFrameworkElementNativeProperty(
		BindingSourcePropertyToken token) noexcept
	{
		static constexpr BindingSourcePropertyToken properties[] = {
			MakeBindingSourcePropertyToken(L"DataContext"),
			MakeBindingSourcePropertyToken(L"Language"),
			MakeBindingSourcePropertyToken(L"Visibility"),
			MakeBindingSourcePropertyToken(L"IsVisible"),
			MakeBindingSourcePropertyToken(L"IsEnabled"),
			MakeBindingSourcePropertyToken(L"AllowDrop"),
			MakeBindingSourcePropertyToken(L"Canvas.Left"),
			MakeBindingSourcePropertyToken(L"Canvas.Top"),
			MakeBindingSourcePropertyToken(L"Canvas.Right"),
			MakeBindingSourcePropertyToken(L"Canvas.Bottom"),
			MakeBindingSourcePropertyToken(L"Width"),
			MakeBindingSourcePropertyToken(L"Height"),
			MakeBindingSourcePropertyToken(L"ActualWidth"),
			MakeBindingSourcePropertyToken(L"ActualHeight"),
			MakeBindingSourcePropertyToken(L"Margin"),
			MakeBindingSourcePropertyToken(L"HorizontalAlignment"),
			MakeBindingSourcePropertyToken(L"VerticalAlignment"),
			MakeBindingSourcePropertyToken(L"ZIndex"),
			MakeBindingSourcePropertyToken(L"Grid.Row"),
			MakeBindingSourcePropertyToken(L"Grid.Column"),
			MakeBindingSourcePropertyToken(L"Grid.RowSpan"),
			MakeBindingSourcePropertyToken(L"Grid.ColumnSpan"),
			MakeBindingSourcePropertyToken(L"DockPanel.Dock"),
			MakeBindingSourcePropertyToken(L"MinWidth"),
			MakeBindingSourcePropertyToken(L"MinHeight"),
			MakeBindingSourcePropertyToken(L"MaxWidth"),
			MakeBindingSourcePropertyToken(L"MaxHeight"),
			MakeBindingSourcePropertyToken(L"Opacity"),
			MakeBindingSourcePropertyToken(L"ClipToBounds"),
			MakeBindingSourcePropertyToken(L"Clip"),
			MakeBindingSourcePropertyToken(L"RenderTransform"),
			MakeBindingSourcePropertyToken(L"RenderTransformOrigin"),
			MakeBindingSourcePropertyToken(L"Validation.HasError"),
			MakeBindingSourcePropertyToken(L"Validation.Errors"),
			MakeBindingSourcePropertyToken(L"Tag"),
			MakeBindingSourcePropertyToken(L"Cursor"),
			MakeBindingSourcePropertyToken(L"Focusable"),
			MakeBindingSourcePropertyToken(L"IsTabStop"),
			MakeBindingSourcePropertyToken(L"TabIndex"),
			MakeBindingSourcePropertyToken(L"IsFocused"),
			MakeBindingSourcePropertyToken(L"IsKeyboardFocused"),
			MakeBindingSourcePropertyToken(L"IsKeyboardFocusVisible"),
			MakeBindingSourcePropertyToken(L"IsKeyboardFocusWithin"),
			MakeBindingSourcePropertyToken(L"IsMouseOver"),
			MakeBindingSourcePropertyToken(L"IsMouseDirectlyOver"),
			MakeBindingSourcePropertyToken(L"IsMouseCaptured"),
			MakeBindingSourcePropertyToken(L"IsMouseCaptureWithin"),
			MakeBindingSourcePropertyToken(L"FocusManager.IsFocusScope"),
			MakeBindingSourcePropertyToken(L"KeyboardNavigation.TabNavigation"),
			MakeBindingSourcePropertyToken(
				L"KeyboardNavigation.DirectionalNavigation"),
			MakeBindingSourcePropertyToken(L"AutomationProperties.Name"),
			MakeBindingSourcePropertyToken(
				L"AutomationProperties.FullDescription"),
			MakeBindingSourcePropertyToken(L"AutomationProperties.HelpText"),
			MakeBindingSourcePropertyToken(L"AutomationProperties.AutomationId"),
		};
		return std::find(std::begin(properties), std::end(properties), token)
			!= std::end(properties);
	}

	bool IsTextBlockNativeProperty(
		BindingSourcePropertyToken token) noexcept
	{
		return token == MakeBindingSourcePropertyToken(L"FontFamily")
			|| token == MakeBindingSourcePropertyToken(L"FontSize")
			|| token == MakeBindingSourcePropertyToken(L"Padding");
	}
#endif
}

bool IsNativePropertySupportedByUIClass(
	UIClass type,
	const DependencyPropertyMetadata& metadata) noexcept
{
	if (type == UIClass::UI_Base) return false;

	// Metadata registered by a concrete behavior host is an explicit member of
	// that type. Shared base-host metadata is projected through the
	// authoritative WPF/XAML type hierarchy.
	if (metadata.OwnerType() != std::type_index(typeid(Control)))
		return true;

	if (IsUIClassAssignableFrom(UIClass::UI_Control, type))
		return true;
	if (IsFrameworkElementNativeProperty(
#if CUI_ENABLE_DYNAMIC_XAML
		metadata.Name()
#else
		metadata.Property().BindingSourceToken()
#endif
		))
		return true;
	if (type == UIClass::UI_Label
		&& IsTextBlockNativeProperty(
#if CUI_ENABLE_DYNAMIC_XAML
			metadata.Name()
#else
			metadata.Property().BindingSourceToken()
#endif
			))
		return true;
	return false;
}

bool IsControlTemplateHostClass(UIClass type) noexcept
{
	return IsUIClassAssignableFrom(UIClass::UI_Control, type);
}

UIClass GetDefaultItemContainerType(UIClass itemsControlType) noexcept
{
	switch (itemsControlType)
	{
	case UIClass::UI_ListView:
		return UIClass::UI_ListViewItem;
	case UIClass::UI_ComboBox:
		return UIClass::UI_ComboBoxItem;
	case UIClass::UI_TreeView:
	case UIClass::UI_TreeViewItem:
		return UIClass::UI_TreeViewItem;
	case UIClass::UI_Menu:
	case UIClass::UI_MenuItem:
	case UIClass::UI_ContextMenu:
		return UIClass::UI_MenuItem;
	case UIClass::UI_TabControl:
		return UIClass::UI_TabItem;
	case UIClass::UI_StatusBar:
		return UIClass::UI_StatusBarItem;
	case UIClass::UI_DataGrid:
		return UIClass::UI_DataGridRow;
	case UIClass::UI_ListBox:
	case UIClass::UI_Selector:
		return UIClass::UI_ListBoxItem;
	default:
		return IsUIClassAssignableFrom(
			UIClass::UI_ItemsControl, itemsControlType)
			? UIClass::UI_ContentPresenter : UIClass::UI_Base;
	}
}

UIClass Control::Type() { return UIClass::UI_Control; }

namespace
{
	struct VisualOwnershipCommitObservation final
	{
		Control* Target = nullptr;
		bool Committed = false;
		bool* Commit = nullptr;
		VisualOwnershipCommitObservation* Previous = nullptr;
	};

	thread_local VisualOwnershipCommitObservation*
		ActiveVisualOwnershipCommitObservation = nullptr;

	struct TemplateOwnerCleanupObservation final
	{
		Control* Owner = nullptr;
		TemplateOwnerCleanupObservation* Previous = nullptr;
	};

	thread_local TemplateOwnerCleanupObservation*
		ActiveTemplateOwnerCleanupObservation = nullptr;

	class TemplateOwnerCleanupScope final
	{
	public:
		explicit TemplateOwnerCleanupScope(Control* owner) noexcept
		{
			_observation.Owner = owner;
			_observation.Previous =
				ActiveTemplateOwnerCleanupObservation;
			ActiveTemplateOwnerCleanupObservation = &_observation;
		}
		~TemplateOwnerCleanupScope()
		{
			ActiveTemplateOwnerCleanupObservation =
				_observation.Previous;
		}

	private:
		TemplateOwnerCleanupObservation _observation;
	};

	bool IsTemplateOwnerCleanupBlocked(Control* owner) noexcept
	{
		for (auto* observation =
			ActiveTemplateOwnerCleanupObservation;
			observation;
			observation = observation->Previous)
			if (observation->Owner == owner) return true;
		return false;
	}

	class VisualOwnershipCommitScope final
	{
	public:
		VisualOwnershipCommitScope(
			Control* target,
			bool* commit) noexcept
		{
			if (!commit) return;
			*commit = false;
			_observation.Target = target;
			_observation.Commit = commit;
			_observation.Previous =
				ActiveVisualOwnershipCommitObservation;
			ActiveVisualOwnershipCommitObservation = &_observation;
			_active = true;
		}

		~VisualOwnershipCommitScope()
		{
			if (!_active) return;
			ActiveVisualOwnershipCommitObservation =
				_observation.Previous;
		}

		VisualOwnershipCommitScope(
			const VisualOwnershipCommitScope&) = delete;
		VisualOwnershipCommitScope& operator=(
			const VisualOwnershipCommitScope&) = delete;

	private:
		VisualOwnershipCommitObservation _observation;
		bool _active = false;
	};

	class VisualOwnershipCommitBatchScope final
	{
	public:
		explicit VisualOwnershipCommitBatchScope(
			std::span<Control* const> targets)
			: _previous(ActiveVisualOwnershipCommitObservation)
		{
			_observations.resize(targets.size());
			auto* previous = _previous;
			for (size_t index = 0; index < targets.size(); ++index)
			{
				auto& observation = _observations[index];
				observation.Target = targets[index];
				observation.Previous = previous;
				previous = &observation;
			}
			if (!_observations.empty())
				ActiveVisualOwnershipCommitObservation =
					&_observations.back();
		}

		~VisualOwnershipCommitBatchScope()
		{
			ActiveVisualOwnershipCommitObservation = _previous;
		}

		bool Committed(size_t index) const noexcept
		{
			return index < _observations.size()
				&& _observations[index].Committed;
		}

		VisualOwnershipCommitBatchScope(
			const VisualOwnershipCommitBatchScope&) = delete;
		VisualOwnershipCommitBatchScope& operator=(
			const VisualOwnershipCommitBatchScope&) = delete;

	private:
		std::vector<VisualOwnershipCommitObservation> _observations;
		VisualOwnershipCommitObservation* _previous = nullptr;
	};

	void PublishVisualOwnershipCommit(Control* child) noexcept
	{
		for (auto* observation =
			ActiveVisualOwnershipCommitObservation;
			observation;
			observation = observation->Previous)
		{
			if (observation->Target != child) continue;
			observation->Committed = true;
			if (observation->Commit)
				*observation->Commit = true;
		}
	}

}

void Control::ConfigureControlTemplateVisual(Control& child)
{
	(void)child;
}

void Control::ConfigureControlTemplateVisualPreservingOwnership(
	std::unique_ptr<Control>& value)
{
	if (!value) return;
	auto* raw = value.release();
	const ControlWeakReference lifetime(raw);
	bool visualOwnershipCommit = false;
	VisualOwnershipCommitScope observation(
		raw, &visualOwnershipCommit);
	try
	{
		ConfigureControlTemplateVisual(*raw);
	}
	catch (...)
	{
		auto* live = lifetime.Get();
		if (live && !visualOwnershipCommit
			&& !live->GetVisualParent()
			&& !live->GetPresentationWindow())
			value.reset(live);
		throw;
	}

	auto* live = lifetime.Get();
	if (!live)
		throw std::logic_error(
			"ControlTemplate root was destroyed during configuration");
	if (visualOwnershipCommit || live->GetVisualParent()
		|| live->GetPresentationWindow())
		throw std::logic_error(
			"ControlTemplate root ownership changed during configuration");
	value.reset(live);
}

std::exception_ptr Control::ClearTemplateOwnerSubtree(
	Control* root,
	Control* owner) noexcept
{
	if (!root || !owner) return {};
	const ControlWeakReference rootLifetime(root);
	TemplateOwnerCleanupScope cleanup(owner);
	std::exception_ptr firstError;
	for (size_t pass = 0; pass < 1024; ++pass)
	{
		auto* liveRoot = rootLifetime.Get();
		if (!liveRoot) return firstError;
		bool clearedAny = false;
		std::vector<ControlWeakReference> stack;
		std::vector<ControlWeakReference> visited;
		stack.emplace_back(liveRoot);
		while (!stack.empty())
		{
			auto currentReference = std::move(stack.back());
			stack.pop_back();
			if (std::find(
				visited.begin(), visited.end(), currentReference)
				!= visited.end())
				continue;
			visited.push_back(currentReference);
			auto* current = currentReference.Get();
			if (!current) continue;
			std::vector<ControlWeakReference> children;
			children.reserve(current->GetVisualChildrenView().size());
			for (auto* child : current->GetVisualChildrenView())
				if (child) children.emplace_back(child);
			if (current->GetTemplatedParent() == owner)
			{
				clearedAny = true;
				try
				{
					cui::framework::TreeAccess::SetTemplatedParent(
						*current, nullptr);
				}
				catch (...)
				{
					if (!firstError)
						firstError = std::current_exception();
				}
			}
			current = currentReference.Get();
			if (current)
			{
				// Merge the post-callback snapshot so descendants added during
				// notification are also cleared. The pre-callback weak snapshot
				// above retains reachability for children that escaped the root
				// during that same notification.
				for (auto* child : current->GetVisualChildrenView())
					if (child) children.emplace_back(child);
			}
			for (auto& child : children)
				stack.push_back(std::move(child));
		}
		if (!clearedAny) return firstError;
	}
	if (!firstError)
		firstError = std::make_exception_ptr(std::logic_error(
			"template owner cleanup did not converge"));
	return firstError;
}

std::exception_ptr
Control::ClearTemplateOwnerSubtreePreservingOwnership(
	std::unique_ptr<Control>& root,
	Control* owner) noexcept
{
	if (!root || !owner) return {};
	auto* raw = root.release();
	const ControlWeakReference lifetime(raw);
	bool visualOwnershipCommit = false;
	VisualOwnershipCommitScope observation(
		raw, &visualOwnershipCommit);
	auto error = ClearTemplateOwnerSubtree(raw, owner);
	auto* live = lifetime.Get();
	if (live && !visualOwnershipCommit
		&& !live->GetVisualParent()
		&& !live->GetPresentationWindow())
		root.reset(live);
	return error;
}

ControlTemplateReference Control::GetTemplate() const
{
	BindingValue value;
	ControlTemplateReference result;
	return const_cast<Control*>(this)->TryGetPropertyValue(
		TemplateProperty(), value)
		&& value.TryGet(result)
		? result : ControlTemplateReference{};
}

void Control::SetTemplate(ControlTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		TemplateProperty(), std::move(value));
}

bool Control::ApplyTemplate()
{
	VerifyAccess();
	if (_applyingTemplate) return false;
	_applyingTemplate = true;
	ControlScopeExit guard{
		[this] { _applyingTemplate = false; } };
	bool createdVisualTree = false;
	// WPF retries once when OnApplyTemplate changes Template (two total
	// attempts). Use the same finite contract instead of an open-ended loop.
	for (size_t attempt = 0; attempt < 2; ++attempt)
	{
		const auto currentTemplate = GetTemplate();
		// A batched Style refresh may coalesce an effective Template value back
		// to the same resource after its old visual tree was removed. A
		// successful applied state is never valid without either a visual root
		// or an explicit failed-template diagnostic; repair that transient state
		// so ApplyTemplate can materialize the retained effective value.
		if (_templateApplied && currentTemplate && !GetControlTemplateRoot()
			&& _lastTemplateError.empty())
			_templateApplied = false;
		if (_templateApplied)
		{
			CompleteControlTemplateApplication();
			// OnApplyTemplate is allowed to replace Template. Match WPF by
			// applying that new effective template before returning.
			if (_templateApplied) return createdVisualTree;
			continue;
		}
		if (!currentTemplate)
		{
			_templateApplied = true;
			_templateApplyCallbackPending = false;
			_lastTemplateError.clear();
			return createdVisualTree;
		}

		const auto applyingTemplate = currentTemplate;
		try
		{
			std::wstring error;
			const bool applied =
				applyingTemplate.Get()->Apply(*this, &error);
			if (!applied || !GetControlTemplateRoot())
			{
				_lastTemplateError = error.empty()
					? L"ControlTemplate 未生成视觉根。" : std::move(error);
				AbortControlTemplateApplication();
				// A broken template must not start an endless layout/apply loop.
				_templateApplied = true;
				return createdVisualTree;
			}
			createdVisualTree = true;
			// The factory may synchronously change Template while building.
			// Never publish that stale tree as the current template instance.
			if (!(GetTemplate() == applyingTemplate))
			{
				AbortControlTemplateApplication();
				continue;
			}
			_lastTemplateError.clear();
			MarkControlTemplateRootAttached();
			CompleteControlTemplateApplication();
			if (_templateApplied) return true;
		}
		catch (const std::exception&)
		{
			_lastTemplateError =
				L"ControlTemplate 应用失败：运行时异常。";
			AbortControlTemplateApplication();
			_templateApplied = true;
			return createdVisualTree;
		}
		catch (...)
		{
			_lastTemplateError =
				L"ControlTemplate 应用失败：未知错误。";
			AbortControlTemplateApplication();
			_templateApplied = true;
			return createdVisualTree;
		}
	}

	_lastTemplateError =
		L"OnApplyTemplate 连续替换 Template，超过安全重试上限。";
	AbortControlTemplateApplication();
	_templateApplied = true;
	return createdVisualTree;
}

void Control::CompleteControlTemplateApplication()
{
	if (!_templateApplyCallbackPending || !GetControlTemplateRoot()) return;
	_templateApplyCallbackPending = false;
	OnApplyTemplate();
}

void Control::AbortControlTemplateApplication() noexcept
{
	try
	{
		(void)DetachVisualChildTemplateRoot();
		if (!GetControlTemplateRoot())
		{
			ClearDeclarativeTemplateScope();
			OnControlTemplatePresentationChanged();
		}
	}
	catch (...)
	{
		// Rollback is best effort during an already failing template build.
		try { ClearDeclarativeTemplateScope(); }
		catch (...) {}
	}
	// A virtual detach can fail after deciding to preserve the existing root.
	// Keep the template state aligned with the tree that actually survived.
	if (GetControlTemplateRoot())
		MarkControlTemplateRootAttached();
	else
		MarkControlTemplateRootDetached();
}

Control* Control::SetControlTemplateRoot(std::unique_ptr<Control> value)
{
	if (value && value.get() == _controlTemplateRoot)
	{
		// The host already owns this pointer. Consume the invalid duplicate
		// unique_ptr without allowing its destructor to delete the live root.
		(void)value.release();
		return _controlTemplateRoot;
	}
	if (!value)
	{
		if (_controlTemplateRoot)
			(void)DetachVisualChildTemplateRoot();
		else
		{
			OnControlTemplatePresentationChanged();
			RequestLayout();
			InvalidateVisual();
		}
		return nullptr;
	}

	// Template roots are an internal single-assignment slot. Normal
	// ApplyTemplate and both code generators explicitly detach the previous
	// instance before installing the next one. Validate the candidate first so
	// a derived preflight failure cannot disturb the established tree.
	ConfigureControlTemplateVisualPreservingOwnership(value);
	if (_controlTemplateRoot)
		throw std::logic_error(
			"Control already owns a ControlTemplate root");

	auto* candidateRoot = value.get();
	const ControlWeakReference candidateLifetime(candidateRoot);
	_controlTemplateRoot = candidateRoot;
	try
	{
		cui::framework::TreeAccess::AddOwnedVisualChild(
			*this, std::move(value), nullptr);
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		auto* live = candidateLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0))
		{
			// Attachment committed before an observer failed. Keep the
			// candidate published while rollback runs; a still-attached child
			// must never become a hidden template root.
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
			try { (void)DetachVisualChildTemplateRoot(); }
			catch (...) {}
		}
		live = candidateLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0))
		{
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
		}
		else
		{
			if (_controlTemplateRoot == candidateRoot)
				_controlTemplateRoot = nullptr;
			(void)ClearTemplateOwnerSubtree(live, this);
			auto* replacement = _controlTemplateRoot;
			if (replacement && replacement != candidateRoot
				&& replacement->GetVisualParent() == this
				&& IndexOfVisualChild(replacement) >= 0)
				MarkControlTemplateRootAttached();
			else
			{
				_controlTemplateRoot = nullptr;
				MarkControlTemplateRootDetached();
			}
		}
		std::rethrow_exception(originalError);
	}

	auto* live = candidateLifetime.Get();
	if (!live || live->GetVisualParent() != this
		|| IndexOfVisualChild(live) < 0)
	{
		// InsertOwnedVisualChild normally enforces this contract itself. Keep a
		// final host-side check because collection observers are allowed to run
		// arbitrary re-entrant tree operations before attachment returns.
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0))
		{
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
			try { (void)DetachVisualChildTemplateRoot(); }
			catch (...) {}
		}
		live = candidateLifetime.Get();
		if (_controlTemplateRoot == candidateRoot)
		{
			if (live && (live->GetVisualParent() == this
				|| IndexOfVisualChild(live) >= 0))
			{
				_controlTemplateRoot = live;
				MarkControlTemplateRootAttached();
			}
			else
			{
				_controlTemplateRoot = nullptr;
				(void)ClearTemplateOwnerSubtree(live, this);
				MarkControlTemplateRootDetached();
			}
		}
		else if (_controlTemplateRoot
			&& (_controlTemplateRoot->GetVisualParent() == this
				|| IndexOfVisualChild(_controlTemplateRoot) >= 0))
		{
			MarkControlTemplateRootAttached();
		}
		else
		{
			_controlTemplateRoot = nullptr;
			MarkControlTemplateRootDetached();
		}
		throw std::logic_error(
			"ControlTemplate root attachment did not commit");
	}

	_controlTemplateRoot = live;
	MarkControlTemplateRootAttached();
	OnControlTemplatePresentationChanged();
	RequestLayout();
	InvalidateVisual();
	return _controlTemplateRoot;
}

std::unique_ptr<Control> Control::DetachVisualChildTemplateRoot()
{
	if (!_controlTemplateRoot) return {};
	auto* previous = _controlTemplateRoot;
	const ControlWeakReference previousLifetime(previous);
	std::unique_ptr<Control> result;
	std::exception_ptr notificationError;
	bool visualOwnershipCommit = false;
	try
	{
		result = DetachVisualChildCore(
			previous, &visualOwnershipCommit,
			&notificationError);
	}
	catch (...)
	{
		notificationError = std::current_exception();
		auto* live = previousLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0))
		{
			// A pre-commit failure leaves the published tree untouched.
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
			std::rethrow_exception(notificationError);
		}
		if (live && !visualOwnershipCommit
			&& !live->GetVisualParent())
			result.reset(live);
	}

	auto* livePrevious = previousLifetime.Get();
	if (livePrevious && (livePrevious->GetVisualParent() == this
		|| IndexOfVisualChild(livePrevious) >= 0))
	{
		_controlTemplateRoot = livePrevious;
		MarkControlTemplateRootAttached();
		if (notificationError)
			std::rethrow_exception(notificationError);
		throw std::logic_error(
			"ControlTemplate root detach did not commit");
	}

	_controlTemplateRoot = nullptr;
	// The detach is committed. Drop every direct pointer into the old template
	// before SetTemplatedParent notifications can re-enter and install a new one.
	std::exception_ptr cleanupError;
	try
	{
		ClearDeclarativeTemplateScope();
	}
	catch (...)
	{
		cleanupError = std::current_exception();
	}
	const auto ownerCleanupError = result
		? ClearTemplateOwnerSubtreePreservingOwnership(result, this)
		: ClearTemplateOwnerSubtree(livePrevious, this);
	if (!cleanupError) cleanupError = ownerCleanupError;
	auto* replacement = _controlTemplateRoot;
	const bool replacementCommitted = replacement
		&& replacement->GetVisualParent() == this
		&& IndexOfVisualChild(replacement) >= 0;
	if (replacementCommitted)
	{
		MarkControlTemplateRootAttached();
	}
	else
	{
		_controlTemplateRoot = nullptr;
		MarkControlTemplateRootDetached();
		try
		{
			OnControlTemplatePresentationChanged();
		}
		catch (...)
		{
			if (!cleanupError) cleanupError = std::current_exception();
		}
	}
	RequestLayout();
	InvalidateVisual();
	if (!cleanupError) cleanupError = notificationError;
	if (cleanupError)
		std::rethrow_exception(cleanupError);
	return result;
}

void Control::RegisterInheritanceChild(Control* child)
{
	if (!child || std::find(_inheritanceChildren.begin(),
		_inheritanceChildren.end(), child) != _inheritanceChildren.end()) return;
	_inheritanceChildren.push_back(child);
}

void Control::UnregisterInheritanceChild(Control* child)
{
	_inheritanceChildren.erase(std::remove(
		_inheritanceChildren.begin(), _inheritanceChildren.end(), child),
		_inheritanceChildren.end());
}

void Control::RefreshInheritanceContext(bool recursive)
{
	RefreshInheritedPropertiesRecursive();
	RebuildStyleSubscriptions(recursive);
	(void)RefreshDynamicResourceValues(recursive);
	(void)RefreshStyleValues(recursive);
}

namespace
{
	bool WouldCreateRoutedParentCycle(
		const Control& child,
		Control* candidateParent) noexcept
	{
		std::unordered_set<const Control*> visited;
		std::vector<const Control*> pending;
		if (candidateParent) pending.push_back(candidateParent);
		while (!pending.empty())
		{
			const auto* current = pending.back();
			pending.pop_back();
			if (current == &child) return true;
			if (!visited.insert(current).second) continue;
			// Inspect the union of all structural parent edges, not only today's
			// precedence-selected GetRoutedParent().  Otherwise a logical/template
			// edge hidden by VisualParent could become a cycle on a later detach.
			if (auto* parent = current->GetVisualParent())
				pending.push_back(parent);
			if (auto* parent = current->GetLogicalParent())
				pending.push_back(parent);
			if (auto* parent = current->GetTemplatedParent())
				pending.push_back(parent);
		}
		return false;
	}
}

Control* Control::InsertVisualChildWithLogicalParent(
	int index,
	Control* child,
	Control* logicalParent,
	bool* structuralCommit)
{
	VerifyAccess();
	if (structuralCommit) *structuralCommit = false;
	if (!child)
		throw std::invalid_argument("不能添加空控件");
	// Observe the entire mutation, including derived collection validation.
	// A validator can legally perform a nested detach and hand ownership to a
	// callback before this insertion reaches its own structural commit point.
	VisualOwnershipCommitScope ownershipObservation(
		child, structuralCommit);
	if (WouldCreateRoutedParentCycle(*child, logicalParent))
		throw std::logic_error("可视/逻辑/模板路由树不能形成循环");

	_pendingVisualChildAttachments.push_back(
		PendingVisualChildAttachment{
			child, ControlWeakReference(logicalParent), structuralCommit });
	ControlScopeExit guard{
		[this, child]
		{
			const auto found = std::find_if(
				_pendingVisualChildAttachments.rbegin(),
				_pendingVisualChildAttachments.rend(),
				[child](const PendingVisualChildAttachment& item)
				{ return item.Child == child; });
			if (found != _pendingVisualChildAttachments.rend())
				_pendingVisualChildAttachments.erase(
					std::next(found).base());
		} };
	return InsertVisualChild(index, child);
}

void Control::SetVisualParentCore(Control* value)
{
	if (_visualParent == value) return;
	if (WouldCreateRoutedParentCycle(*this, value))
		throw std::logic_error("可视/逻辑/模板路由树不能形成循环");
	// A parent destructor detaches its surviving children after invalidating its
	// lifetime token.  Their presentation-source raw pointer can already name a
	// retired staging Window, so teardown must not dereference it merely to
	// refresh an input projection. Window cleanup retires active capture before
	// visual destruction begins.
	const bool refreshReverseInheritance = !_isDestroying
		&& (!_visualParent || !_visualParent->_isDestroying)
		&& (!_logicalParent || !_logicalParent->_isDestroying)
		&& (!value || !value->_isDestroying);
	const ControlWeakReference selfReference(this);
	const ControlWeakReference previousReference(_visualParent);
	const ControlWeakReference valueReference(value);
	ControlWeakReference previousWindowReference;
	ControlWeakReference valueWindowReference;
	if (refreshReverseInheritance)
	{
		previousWindowReference = GetPresentationWindow();
		valueWindowReference = value ? value->GetPresentationWindow() : nullptr;
	}
	auto enabledSnapshot = CaptureEffectiveIsEnabledSubtree(*this);
	auto visibleSnapshot = CaptureEffectiveIsVisibleSubtree(*this);
	auto* previousInheritanceParent = GetInheritanceParent();
	_visualParent = value;
	auto* currentInheritanceParent = GetInheritanceParent();
	if (previousInheritanceParent != currentInheritanceParent)
	{
		if (previousInheritanceParent)
			previousInheritanceParent->UnregisterInheritanceChild(this);
		if (currentInheritanceParent)
			currentInheritanceParent->RegisterInheritanceChild(this);
		RefreshInheritanceContext(true);
	}
	auto* live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_visualParent != valueReference.Get()) return;
	live->PublishEffectiveIsEnabledChanges(std::move(enabledSnapshot));
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_visualParent != valueReference.Get()) return;
	live->PublishEffectiveIsVisibleChanges(std::move(visibleSnapshot));
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_visualParent != valueReference.Get()) return;
	if (refreshReverseInheritance)
	{
		const ControlWeakReference currentWindowReference(
			live->GetPresentationWindow());
		std::unordered_set<Window*> refreshed;
		const ControlWeakReference candidates[]{
			previousWindowReference,
			currentWindowReference,
			valueWindowReference };
		for (const auto& candidate : candidates)
			if (auto* window = dynamic_cast<Window*>(candidate.Get());
				window && refreshed.insert(window).second)
				window->RefreshReverseInheritedInputProperties();
	}
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_visualParent != valueReference.Get()) return;
	cui::framework::EventAccess::Raise(
		live->OnVisualParentChanged,
		live, previousReference.Get(), valueReference.Get());
}

ControlWeakReference::ControlWeakReference(Control* target) noexcept
	: _target(target),
	_lifetime(target ? target->WeakLifetimeToken()
		: std::weak_ptr<const std::atomic_bool>{})
{
}

ControlWeakReference& ControlWeakReference::operator=(Control* target) noexcept
{
	_target = target;
	_lifetime = target ? target->WeakLifetimeToken()
		: std::weak_ptr<const std::atomic_bool>{};
	return *this;
}

Control* ControlWeakReference::Get() const noexcept
{
	if (!_target) return nullptr;
	const auto lifetime = _lifetime.lock();
	return lifetime
		&& lifetime->load(std::memory_order_acquire) ? _target : nullptr;
}

namespace
{
	thread_local size_t PresentationWindowTransferDepth = 0;
	thread_local std::vector<ControlWeakReference>
		PendingPresentationWindowTransfers;
}

/**
 * Attaching a subtree that kept its Window through a transfer scope. The
 * subtree already carries the right presentation source, command domain and
 * layout caches; only route-dependent state of the new ancestors changes.
 */
bool Control::CompletePendingPresentationWindowTransfer(
	Control& child, PresentationWindow* window)
{
	auto& pending = PendingPresentationWindowTransfers;
	const auto found = std::find_if(pending.begin(), pending.end(),
		[&child](const ControlWeakReference& value)
		{ return value.Get() == &child; });
	if (found == pending.end()) return false;
	pending.erase(found);
	if (!window || child.GetPresentationWindow() != window) return false;
	// Same-domain reparenting changes command routes: requery the Window once
	// instead of once per descendant.
	(void)RoutedCommandManager::InvalidateRequerySuggested(child);
	const ControlWeakReference windowLifetime(window);
	if (auto* live = dynamic_cast<Window*>(windowLifetime.Get()))
		live->RefreshReverseInheritedInputProperties();
	return true;
}

Control::ScopedPresentationWindowTransfer::
	ScopedPresentationWindowTransfer() noexcept
{
	++PresentationWindowTransferDepth;
}

Control::ScopedPresentationWindowTransfer::
	~ScopedPresentationWindowTransfer()
{
	if (PresentationWindowTransferDepth == 0) return;
	if (--PresentationWindowTransferDepth != 0) return;
	auto pending = std::move(PendingPresentationWindowTransfers);
	PendingPresentationWindowTransfers.clear();
	for (const auto& reference : pending)
	{
		auto* control = reference.Get();
		// Still detached (or re-attached outside the framework path): release
		// the Window exactly as an ordinary detach would have.
		if (!control || control->GetVisualParent()
			|| !control->GetPresentationWindow()) continue;
		try
		{
			PropagatePresentationWindow(control, nullptr);
		}
		catch (...)
		{
		}
	}
}

void Control::PropagatePresentationWindow(
	Control* control,
	PresentationWindow* form)
{
	if (!control) return;
	const ControlWeakReference previousRootWindowReference(
		control->GetPresentationWindow());
	const ControlWeakReference requestedWindowReference(form);
	std::unordered_set<Control*> activePath;
	struct ActivePathEntry final
	{
		std::unordered_set<Control*>& Path;
		Control* Value = nullptr;
		~ActivePathEntry() { Path.erase(Value); }
	};
	auto propagate = [&](auto&& self, ControlWeakReference controlReference) -> void
		{
			auto* live = controlReference.Get();
			if (!live || !activePath.insert(live).second) return;
			const ActivePathEntry active{ activePath, live };
			auto* requestedWindow = dynamic_cast<Window*>(
				requestedWindowReference.Get());
			if (form && !requestedWindow) return;

			const ControlWeakReference previousWindowReference(live->GetPresentationWindow());
			const bool windowChanged = live->GetPresentationWindow() != requestedWindow;
			if (windowChanged)
			{
				live->_hasLastInvalidatedClientRect = false;
				if (live->GetPresentationWindow())
					(void)RoutedCommandManager::InvalidateRequerySuggested(*live);
				live = controlReference.Get();
				requestedWindow = dynamic_cast<Window*>(
					requestedWindowReference.Get());
				if (!live || (form && !requestedWindow)
					|| live->GetPresentationWindow()
					!= dynamic_cast<Window*>(previousWindowReference.Get())) return;
			}
			live->SetPresentationWindowCore(requestedWindow);
			RoutedCommandManager::NotifySourceScopeChanged(*live);

			live = controlReference.Get();
			requestedWindow = dynamic_cast<Window*>(
				requestedWindowReference.Get());
			if (!live || (form && !requestedWindow)
				|| live->GetPresentationWindow() != requestedWindow) return;
			live->_layoutState.InvalidateMeasure();

			std::vector<ControlWeakReference> children;
			children.reserve(live->_visualChildren.size());
			for (auto* child : live->_visualChildren)
				if (child) children.emplace_back(child);
			for (const auto& childReference : children)
			{
				live = controlReference.Get();
				requestedWindow = dynamic_cast<Window*>(
					requestedWindowReference.Get());
				auto* child = childReference.Get();
				if (!live || (form && !requestedWindow)
					|| live->GetPresentationWindow() != requestedWindow) return;
				if (!child || child->GetVisualParent() != live) continue;
				self(self, childReference);
			}

			live = controlReference.Get();
			requestedWindow = dynamic_cast<Window*>(
				requestedWindowReference.Get());
			if (!live || (form && !requestedWindow)
				|| live->GetPresentationWindow() != requestedWindow) return;
			if (windowChanged)
				live->OnPresentationWindowChanged(
					dynamic_cast<Window*>(previousWindowReference.Get()),
					requestedWindow);
		};
	propagate(propagate, ControlWeakReference(control));
	std::unordered_set<Window*> refreshed;
	const ControlWeakReference candidates[]{
		previousRootWindowReference,
		requestedWindowReference };
	for (const auto& candidate : candidates)
		if (auto* window = dynamic_cast<Window*>(candidate.Get());
			window && refreshed.insert(window).second)
			window->RefreshReverseInheritedInputProperties();
}

void Control::OnPresentationWindowChanged(
	PresentationWindow* previousWindow,
	PresentationWindow* currentWindow)
{
	if (previousWindow && previousWindow != currentWindow)
	{
		previousWindow->SynchronizeActiveDeclarativeAnimationControl(
			this, false);
		previousWindow->SynchronizeRetainedNativeAnimationControl(
			this, false);
	}
	if (currentWindow)
	{
		currentWindow->SynchronizeActiveDeclarativeAnimationControl(
			this, HasRetainedVisualStateAnimationRoots());
		currentWindow->SynchronizeRetainedNativeAnimationControl(
			this, HasRetainedNativeAnimation());
	}
}

void Control::SetLogicalParentCore(Control* value)
{
	if (_logicalParent == value) return;
	if (WouldCreateRoutedParentCycle(*this, value))
		throw std::logic_error("可视/逻辑/模板路由树不能形成循环");
	// See SetVisualParentCore: reverse-inheritance projection is a live-tree
	// operation.  Logical teardown must remain safe even when a detached
	// design-time subtree still carries a retired staging Window address.
	const bool refreshReverseInheritance = !_isDestroying
		&& (!_visualParent || !_visualParent->_isDestroying)
		&& (!_logicalParent || !_logicalParent->_isDestroying)
		&& (!value || !value->_isDestroying);
	// A logical-only host can be destroyed while its child survives through an
	// independent visual ownership branch.  In that case the cached
	// presentation source may be stale, but the owning visual chain is made of
	// live objects and can recover the actual Window without dereferencing it.
	ControlWeakReference teardownVisualWindowReference;
	if (!_isDestroying && _logicalParent && _logicalParent->_isDestroying
		&& _visualParent && !_visualParent->_isDestroying
		&& (!value || !value->_isDestroying))
	{
		std::unordered_set<Control*> visited;
		ControlWeakReference currentReference(_visualParent);
		while (auto* current = currentReference.Get())
		{
			if (!visited.insert(current).second || current->_isDestroying)
				break;
			if (auto* window = dynamic_cast<Window*>(current))
			{
				teardownVisualWindowReference = window;
				break;
			}
			auto* const visualParent = current->_visualParent;
			if (!visualParent || visualParent->_isDestroying) break;
			currentReference = ControlWeakReference(visualParent);
		}
	}
	const ControlWeakReference selfReference(this);
	const ControlWeakReference previousReference(_logicalParent);
	const ControlWeakReference valueReference(value);
	ControlWeakReference previousWindowReference;
	ControlWeakReference valueWindowReference;
	if (refreshReverseInheritance)
	{
		previousWindowReference = GetPresentationWindow();
		valueWindowReference = value ? value->GetPresentationWindow() : nullptr;
	}
	const bool routedThroughLogicalParent = _visualParent == nullptr;
	auto snapshot = routedThroughLogicalParent
		? CaptureEffectiveIsEnabledSubtree(*this)
		: std::vector<std::pair<ControlWeakReference, bool>>{};
	auto* previousInheritanceParent = GetInheritanceParent();
	if (_logicalParent)
	{
		auto& siblings = _logicalParent->_logicalChildren;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), this),
			siblings.end());
	}
	_logicalParent = value;
	if (value && std::find(value->_logicalChildren.begin(),
		value->_logicalChildren.end(), this) == value->_logicalChildren.end())
		value->_logicalChildren.push_back(this);
	auto* currentInheritanceParent = GetInheritanceParent();
	if (previousInheritanceParent != currentInheritanceParent)
	{
		if (previousInheritanceParent)
			previousInheritanceParent->UnregisterInheritanceChild(this);
		if (currentInheritanceParent)
			currentInheritanceParent->RegisterInheritanceChild(this);
		RefreshInheritanceContext(true);
	}
	auto* live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_logicalParent != valueReference.Get()) return;
	if (routedThroughLogicalParent)
		live->PublishEffectiveIsEnabledChanges(std::move(snapshot));
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_logicalParent != valueReference.Get()) return;
	if (refreshReverseInheritance)
	{
		const ControlWeakReference currentWindowReference(
			live->GetPresentationWindow());
		std::unordered_set<Window*> refreshed;
		const ControlWeakReference candidates[]{
			previousWindowReference,
			currentWindowReference,
			valueWindowReference };
		for (const auto& candidate : candidates)
			if (auto* window = dynamic_cast<Window*>(candidate.Get());
				window && refreshed.insert(window).second)
				window->RefreshReverseInheritedInputProperties();
	}
	else if (auto* window = dynamic_cast<Window*>(
		teardownVisualWindowReference.Get()))
	{
		// This path originates in Control::~Control, which is noexcept. Reverse
		// inheritance commits before publishing notifications, so an observer
		// exception can be swallowed here without leaving stale state behind.
		try { window->RefreshReverseInheritedInputProperties(); }
		catch (...) {}
	}
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_logicalParent != valueReference.Get()) return;
	cui::framework::EventAccess::Raise(
		live->OnLogicalParentChanged,
		live, previousReference.Get(), valueReference.Get());
}

void Control::SetLogicalParentCoreObservingVisualOwnership(
	Control* value,
	bool* visualOwnershipCommit)
{
	VisualOwnershipCommitScope observation(
		this, visualOwnershipCommit);
	SetLogicalParentCore(value);
}

void Control::SetTemplatedParentCore(Control* value)
{
	if (_templatedParent == value) return;
	if (value && IsTemplateOwnerCleanupBlocked(value))
		throw std::logic_error(
			"cannot attach a template owner while its subtree is being cleared");
	if (WouldCreateRoutedParentCycle(*this, value))
		throw std::logic_error("可视/逻辑/模板路由树不能形成循环");
	const bool refreshReverseInheritance = !_isDestroying
		&& (!_visualParent || !_visualParent->_isDestroying)
		&& (!_logicalParent || !_logicalParent->_isDestroying)
		&& (!_templatedParent || !_templatedParent->_isDestroying)
		&& (!value || !value->_isDestroying);
	const ControlWeakReference selfReference(this);
	const ControlWeakReference previousReference(_templatedParent);
	const ControlWeakReference valueReference(value);
	ControlWeakReference previousWindowReference;
	ControlWeakReference valueWindowReference;
	if (refreshReverseInheritance)
	{
		previousWindowReference = GetPresentationWindow();
		valueWindowReference = value ? value->GetPresentationWindow() : nullptr;
	}
	const bool routedThroughTemplatedParent = _visualParent == nullptr
		&& _logicalParent == nullptr;
	auto snapshot = routedThroughTemplatedParent
		? CaptureEffectiveIsEnabledSubtree(*this)
		: std::vector<std::pair<ControlWeakReference, bool>>{};
	auto* previousInheritanceParent = GetInheritanceParent();
	_templatedParent = value;
	auto* currentInheritanceParent = GetInheritanceParent();
	if (previousInheritanceParent != currentInheritanceParent)
	{
		if (previousInheritanceParent)
			previousInheritanceParent->UnregisterInheritanceChild(this);
		if (currentInheritanceParent)
			currentInheritanceParent->RegisterInheritanceChild(this);
		RefreshInheritanceContext(true);
	}
	auto* live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_templatedParent != valueReference.Get()) return;
	if (routedThroughTemplatedParent)
		live->PublishEffectiveIsEnabledChanges(std::move(snapshot));
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_templatedParent != valueReference.Get()) return;
	if (refreshReverseInheritance)
	{
		const ControlWeakReference currentWindowReference(
			live->GetPresentationWindow());
		std::unordered_set<Window*> refreshed;
		const ControlWeakReference candidates[]{
			previousWindowReference,
			currentWindowReference,
			valueWindowReference };
		for (const auto& candidate : candidates)
			if (auto* window = dynamic_cast<Window*>(candidate.Get());
				window && refreshed.insert(window).second)
				window->RefreshReverseInheritedInputProperties();
	}
	live = selfReference.Get();
	if (!live || (value && !valueReference)
		|| live->_templatedParent != valueReference.Get()) return;
	cui::framework::EventAccess::Raise(
		live->OnTemplatedParentChanged,
		live, previousReference.Get(), valueReference.Get());
}

void Control::SetTemplatedParentCoreObservingVisualOwnership(
	Control* value,
	bool* visualOwnershipCommit)
{
	VisualOwnershipCommitScope observation(
		this, visualOwnershipCommit);
	SetTemplatedParentCore(value);
}

void Control::InvokeWithVisualOwnershipObservationCore(
	const std::function<void()>& callback,
	bool* visualOwnershipCommit)
{
	VisualOwnershipCommitScope observation(
		this, visualOwnershipCommit);
	if (callback) callback();
}

void Control::SynchronizeVisualChildCollection(
	const CollectionChangedEventArgs& change)
{
	VerifyAccess();
	const ControlWeakReference selfReference(this);
	const std::vector<Control*> previous = _observedVisualChildren;
	std::vector<ControlWeakReference> previousReferences;
	previousReferences.reserve(previous.size());
	for (auto* child : previous)
		previousReferences.emplace_back(child);
	const std::unordered_set<Control*> previousSet(
		previous.begin(), previous.end());
	std::unordered_set<Control*> currentSet;
	currentSet.reserve(_visualChildren.size());
	auto requestedLogicalParent =
		[](Control& owner, Control* child, bool& explicitParent) -> Control*
		{
			for (auto position =
				owner._pendingVisualChildAttachments.rbegin();
				position != owner._pendingVisualChildAttachments.rend();
				++position)
			{
				if (position->Child != child) continue;
				explicitParent = true;
				return position->LogicalParent.Get();
			}
			explicitParent = false;
			return &owner;
		};
	auto reject = [&](const char* message, bool invalidArgument = false)
		{
			_visualChildren.RestoreRejectedMutation(previous);
			if (invalidArgument) throw std::invalid_argument(message);
			throw std::logic_error(message);
		};

	for (auto* child : _visualChildren)
	{
		if (!child)
			reject("不能添加空控件", true);
		if (!currentSet.insert(child).second)
			reject("不能重复添加同一控件");
		if (WouldCreateRoutedParentCycle(*child, this))
			reject("不能将控件添加到自身或其结构后代");
		const bool alreadyObserved = previousSet.contains(child);
		if (alreadyObserved)
		{
			if (child->_visualParent != this)
				reject("子控件 VisualParent 已在集合外被修改");
		}
		else
		{
			bool explicitLogicalParent = false;
			auto* logicalParent = requestedLogicalParent(
				*this, child, explicitLogicalParent);
			if (child->_isWindowRoot || child->_visualParent
				|| (child->_logicalParent
					&& (!explicitLogicalParent
						|| child->_logicalParent != logicalParent))
				|| (child->GetPresentationWindow()
					&& child->GetPresentationWindow()
					!= this->GetPresentationWindow()))
				reject("该控件已属于其他容器");
		}
	}

	std::string validationError;
	if (!ValidateVisualChildCollection(
		std::span<Control* const>{
		_visualChildren.data(), _visualChildren.size() },
		validationError))
	{
		_visualChildren.RestoreRejectedMutation(previous);
		throw std::logic_error(validationError.empty()
			? "Specialized container rejected the child collection"
			: validationError);
	}

	// Ownership is structurally committed from this point. Publish the token
	// before SetVisualParent/SetLogicalParent, specialized collection hooks, or
	// public observers can synchronously detach, reparent, destroy, or throw.
	for (auto* child : _visualChildren)
	{
		if (child && !previousSet.contains(child))
			PublishVisualOwnershipCommit(child);
	}
	for (auto position = _pendingVisualChildAttachments.rbegin();
		position != _pendingVisualChildAttachments.rend(); ++position)
	{
		if (!position->Child || previousSet.contains(position->Child)
			|| !currentSet.contains(position->Child))
			continue;
		if (position->StructuralCommit)
			*position->StructuralCommit = true;
	}

	// Publish the structural snapshot before invoking parent-change callbacks.
	// A callback may perform a nested detach/reparent; the nested synchronizer
	// must observe the collection that actually triggered this pass.
	_observedVisualChildren.assign(
		_visualChildren.begin(), _visualChildren.end());

	auto stillContains = [](const Control& owner, const Control* child)
		{
			return child && std::find(owner._visualChildren.begin(),
				owner._visualChildren.end(), child) != owner._visualChildren.end();
		};
	for (const auto& childReference : previousReferences)
	{
		auto* owner = selfReference.Get();
		auto* child = childReference.Get();
		if (!owner) return;
		if (!child || currentSet.contains(child)
			|| stillContains(*owner, child)) continue;
		const ControlWeakReference windowReference(owner->GetPresentationWindow());
		if (auto* window = dynamic_cast<Window*>(windowReference.Get()))
			window->ClearDetachedControlReferences(child);
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		if (!child || stillContains(*owner, child)) continue;
		if (child->_visualParent == owner)
			child->SetVisualParentCore(nullptr);
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		if (!child || stillContains(*owner, child)) continue;
		if (child->_logicalParent == owner)
			child->SetLogicalParentCore(nullptr);
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		if (!child || stillContains(*owner, child)) continue;
		child->_isWindowRoot = false;
		if (!child->_visualParent)
		{
			if (PresentationWindowTransferDepth > 0
				&& child->GetPresentationWindow())
				PendingPresentationWindowTransfers.emplace_back(child);
			else
				PropagatePresentationWindow(child, nullptr);
		}
	}

	std::vector<ControlWeakReference> currentReferences;
	if (auto* owner = selfReference.Get())
	{
		currentReferences.reserve(owner->_visualChildren.size());
		for (auto* child : owner->_visualChildren)
			if (child) currentReferences.emplace_back(child);
	}
	else return;
	for (const auto& childReference : currentReferences)
	{
		auto* owner = selfReference.Get();
		auto* child = childReference.Get();
		if (!owner) return;
		if (!child || previousSet.contains(child)
			|| !stillContains(*owner, child)) continue;
		if (child->_visualParent && child->_visualParent != owner) continue;
		if (child->_visualParent != owner)
			child->SetVisualParentCore(owner);
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		if (!child || !stillContains(*owner, child)
			|| child->_visualParent != owner) continue;
		bool explicitLogicalParent = false;
		auto* logicalParent = requestedLogicalParent(
			*owner, child, explicitLogicalParent);
		if (child->_logicalParent
			&& child->_logicalParent != logicalParent) continue;
		if (child->_logicalParent != logicalParent)
			child->SetLogicalParentCore(logicalParent);
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		logicalParent = requestedLogicalParent(
			*owner, child, explicitLogicalParent);
		if (!child || !stillContains(*owner, child)
			|| child->_visualParent != owner
			|| child->_logicalParent != logicalParent) continue;
		child->_isWindowRoot = false;
		if (!CompletePendingPresentationWindowTransfer(
			*child, owner->GetPresentationWindow()))
			PropagatePresentationWindow(child, owner->GetPresentationWindow());
		owner = selfReference.Get();
		child = childReference.Get();
		if (!owner) return;
		if (!child || !stillContains(*owner, child)
			|| child->_visualParent != owner) continue;
		if (owner->_themeStyleSheet || owner->_styleSheet)
		{
			// Publish the inherited Theme/Document pair in one refresh. Preserve
			// an explicitly staged child side when the owner has no counterpart.
			const auto theme = owner->_themeStyleSheet
				? owner->_themeStyleSheet : child->_themeStyleSheet;
			const auto styles = owner->_styleSheet
				? owner->_styleSheet : child->_styleSheet;
			// A recycled virtualized container re-enters with the very same
			// sheets it left with. SetVisualParentCore has already refreshed its
			// inheritance context against the new ancestors, so repeating the
			// recursive subscription/resource/style refresh for every descendant
			// on each attach only multiplies the cost of scrolling.
			if ((child->_themeStyleSheet != theme
				|| child->_styleSheet != styles)
				&& !child->SetStyleEnvironment(theme, styles, true))
				throw std::runtime_error(
					"Inherited Theme/Document style environment failed");
		}
	}

	auto* owner = selfReference.Get();
	if (!owner) return;
	owner->_observedVisualChildren.assign(
		owner->_visualChildren.begin(), owner->_visualChildren.end());
	std::vector<Control*> callbackPrevious;
	callbackPrevious.reserve(previousReferences.size());
	for (const auto& childReference : previousReferences)
		callbackPrevious.push_back(childReference.Get());
	owner->OnVisualChildCollectionChanged(
		change,
		std::span<Control* const>{
		callbackPrevious.data(), callbackPrevious.size() });
	owner = selfReference.Get();
	if (!owner) return;
	if (const ControlWeakReference windowReference(owner->GetPresentationWindow());
		auto* window = dynamic_cast<Window*>(windowReference.Get()))
		window->InvalidatePresentationStructure();
	owner = selfReference.Get();
	if (!owner) return;
	owner->RequestLayout();
	owner = selfReference.Get();
	if (!owner) return;
	owner->NotifyAccessibilityStructureChanged();
}

void Control::OnRender() {}

void Control::RequestLayout()
{
	this->_layoutState.InvalidateMeasure();
	if (this->_layoutDeferral.IsSuspended())
	{
		this->_layoutDeferral.QueueLayout();
		return;
	}
	if (this->_visualParent)
	{
		auto* panelParent = dynamic_cast<Panel*>(this->_visualParent);
		if (panelParent)
		{
			panelParent->InvalidateLayout();
		}
		else
		{
			// Some composite controls are not Panel-derived but still participate in
			// the visual tree. Keep walking until a real layout boundary is found.
			this->_visualParent->RequestLayout();
		}
		return;
	}

	if (this->GetPresentationWindow())
	{
		this->GetPresentationWindow()->RequestLayout();
	}
}

void Control::RequestArrange()
{
	this->_layoutState.InvalidateArrange();
	if (this->_layoutDeferral.IsSuspended())
	{
		this->_layoutDeferral.QueueLayout();
		return;
	}
	if (this->_visualParent)
	{
		if (auto* panelParent = dynamic_cast<Panel*>(this->_visualParent))
			panelParent->InvalidateArrangeLayout();
		else
			this->_visualParent->RequestArrange();
		return;
	}
	if (this->GetPresentationWindow())
		this->GetPresentationWindow()->RequestArrangeLayout();
}

void Control::BeginLayoutUpdateDeferral() noexcept
{
	_layoutDeferral.Suspend();
}

void Control::EndLayoutUpdateDeferral(bool performLayout)
{
	const auto work = _layoutDeferral.Resume();
	if (!work.ready) return;
	if (work.layoutRequested)
	{
		RequestLayout();
		if (performLayout) PerformPendingLayout();
	}
	if (work.visualRequested && !work.visualBounds.IsEmpty())
		DispatchInvalidatedClientRect(ToD2DRect(work.visualBounds));
}

void Control::BeginVisualInvalidationDeferral() noexcept
{
	_visualInvalidationDeferral.Suspend();
}

void Control::EndVisualInvalidationDeferral(bool dispatchVisual)
{
	const auto work = _visualInvalidationDeferral.Resume();
	if (!work.ready || !dispatchVisual || !work.visualRequested
		|| work.visualBounds.IsEmpty()) return;
	DispatchInvalidatedClientRect(ToD2DRect(work.visualBounds));
}

void Control::InvalidateMeasureSubtree()
{
	_layoutState.InvalidateMeasure();
	for (auto* child : _visualChildren)
	{
		if (child)
			child->InvalidateMeasureSubtree();
	}
}

void Control::InvalidateMeasurePathFromDescendant(Control* descendant)
{
	if (!descendant) return;
	std::vector<Control*> path;
	for (auto* current = descendant; current; current = current->_visualParent)
	{
		path.push_back(current);
		if (current == this) break;
	}
	if (path.empty() || path.back() != this) return;
	for (auto* current : path)
	{
		current->_layoutState.InvalidateMeasure();
		current->OnLocalMeasurePathInvalidated();
		if (auto* panel = dynamic_cast<Panel*>(current))
		{
			panel->_needsMeasure = true;
			panel->_needsArrange = true;
			if (panel->_layoutEngine) panel->_layoutEngine->Invalidate();
		}
	}
}

void Control::InvalidateVisualSubtree()
{
	InvalidateVisual();
	for (auto* child : _visualChildren)
		if (child) child->InvalidateVisualSubtree();
}

void Control::InvalidateVisualBoundsSubtree()
{
	const auto bounds = GetAbsoluteRectDip();
	InvalidateVisualRectCore(D2D1::RectF(
		bounds.Left(), bounds.Top(), bounds.Right(), bounds.Bottom()), false);
	for (auto* child : _visualChildren)
		if (child) child->InvalidateVisualBoundsSubtree();
}

void Control::MarkPresentationInvalidation(
	PresentationInvalidationKind kind) noexcept
{
	auto advance = [](uint64_t& revision) noexcept
		{
			++revision;
			if (revision == 0) ++revision;
		};
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Content))
		advance(_presentationRevisions.Content);
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Geometry))
		advance(_presentationRevisions.Geometry);
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Composition))
		advance(_presentationRevisions.Composition);
	if (GetPresentationWindow())
		GetPresentationWindow()->InvalidatePresentationNode(this, kind);
}

void Control::InvalidatePresentationGeometrySubtree() noexcept
{
	MarkPresentationInvalidation(PresentationInvalidationKind::Geometry);
}

void Control::InvalidatePresentationTransformSubtree() noexcept
{
	MarkPresentationInvalidation(
		PresentationInvalidationKind::Geometry
		| PresentationInvalidationKind::Transform);
}

void Control::InvalidateDescendantRenderGeometry() noexcept
{
	InvalidatePresentationGeometrySubtree();
}

D2DGraphics* Control::GetDrawingContext() const noexcept
{
	auto* window = GetPresentationWindow();
	return window ? window->GetCurrentDrawingContext() : nullptr;
}

namespace
{
	bool TryPushAxisAlignedChildrenClip(
		D2DGraphics& graphics,
		D2D1_RECT_F rect,
		const D2D1_MATRIX_3X2_F& transform)
	{
		auto* context = graphics.GetDeviceContextRaw();
		if (!context || !std::isfinite(rect.left) || !std::isfinite(rect.top)
			|| !std::isfinite(rect.right) || !std::isfinite(rect.bottom)
			|| rect.right <= rect.left || rect.bottom <= rect.top
			|| transform._12 != 0.0f || transform._21 != 0.0f
			|| !std::isfinite(transform._11) || !std::isfinite(transform._22)
			|| !std::isfinite(transform._31) || !std::isfinite(transform._32))
			return false;
		// Exact only while the device transform also preserves axes; otherwise
		// Direct2D would clip to the transformed bounding box.
		D2D1_MATRIX_3X2_F current{};
		context->GetTransform(&current);
		if (current._12 != 0.0f || current._21 != 0.0f) return false;
		const float x0 = rect.left * transform._11 + transform._31;
		const float x1 = rect.right * transform._11 + transform._31;
		const float y0 = rect.top * transform._22 + transform._32;
		const float y1 = rect.bottom * transform._22 + transform._32;
		const D2D1_RECT_F local{
			(std::min)(x0, x1), (std::min)(y0, y1),
			(std::max)(x0, x1), (std::max)(y0, y1) };
		if (!std::isfinite(local.left) || !std::isfinite(local.top)
			|| !std::isfinite(local.right) || !std::isfinite(local.bottom))
			return false;
		// Popped by D2DGraphics::PopDrawRect (PopAxisAlignedClip) in EndRender.
		context->PushAxisAlignedClip(local, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		return true;
	}
}

void Control::BeginRender()
{
	auto renderSize = GetRenderSizeDip();
	BeginRender(renderSize.width, renderSize.height);
}
void Control::BeginRender(float clipW, float clipH)
{
	_activeGeometryClipCount = 0;
	_activeAxisAlignedClipMask = 0;
	if (!this->GetPresentationWindow() || !this->GetDrawingContext()) return;
	const float titleBarOffset = static_cast<float>(
		this->GetPresentationWindow()->GetTitleBarHeightDip());
	// Layout coordinates are relative to the form content. The control-local
	// transform is followed by ancestor transforms and finally the title bar.
	const auto transform = AsMatrix(GetLocalToRenderTransformForRecording())
		* D2D1::Matrix3x2F::Translation(0.0f, titleBarOffset);
	this->GetDrawingContext()->PushLocalTransform(transform, clipW, clipH);

	struct ClipOwner
	{
		Control* Owner = nullptr;
		bool IsChildrenRectangle = false;
	};
	std::vector<ClipOwner> clipOwners;
	InlinePointerSet<Control> visited;
	auto* clipSuppressionRoot = RenderTransformSuppressionRoots.empty()
		? nullptr : RenderTransformSuppressionRoots.back();
	bool insideSuppressedIsolation = clipSuppressionRoot != nullptr;
	for (auto* current = this;
		current && visited.Insert(current);
		current = current->_visualParent)
	{
		const bool recordClip = !clipSuppressionRoot
			|| insideSuppressedIsolation;
		if (recordClip && current->_clip)
			clipOwners.push_back({ current, false });
		// ClipToBounds clips descendants, not the owner's own rendering. Express
		// the rectangle in this control's local recording coordinates so rotated
		// and skewed ancestors retain their true geometry instead of an AABB.
		if (recordClip && current != this && current->ClipsChildren())
			clipOwners.push_back({ current, true });
		if (current == clipSuppressionRoot)
			insideSuppressedIsolation = false;
	}
	if (clipOwners.empty()) return;
	std::reverse(clipOwners.begin(), clipOwners.end());
	auto renderToLocal = AsMatrix(GetLocalToRenderTransformForRecording());
	if (!renderToLocal.Invert()) return;
	for (const auto& item : clipOwners)
	{
		auto* owner = item.Owner;
		const auto ownerToLocal = AsMatrix(
			owner->GetLocalToRenderTransformForRecording())
			* renderToLocal;
		if (item.IsChildrenRectangle)
		{
			const auto childrenClip = owner->GetVisualChildrenClipRect();
			// Scrolling hosts and ClipToBounds panels are almost always
			// axis-aligned. A layer here costs two factory geometries while
			// recording and an intermediate mask on every retained replay, so
			// prefer the exact axis-aligned clip. Isolated recordings keep the
			// layer: they may be replayed under an animated rotation.
			if (!clipSuppressionRoot && _activeGeometryClipCount < 64u
				&& TryPushAxisAlignedChildrenClip(
					*this->GetDrawingContext(), childrenClip, ownerToLocal))
			{
				_activeAxisAlignedClipMask |=
					uint64_t{ 1 } << _activeGeometryClipCount;
				++_activeGeometryClipCount;
			}
			else if (this->GetDrawingContext()->PushTransformedRectangleClip(
				childrenClip, ownerToLocal))
				++_activeGeometryClipCount;
			continue;
		}
		Microsoft::WRL::ComPtr<ID2D1Geometry> native;
		native.Attach(owner->_clip->CreateD2DGeometry(&ownerToLocal));
		if (native && this->GetDrawingContext()->PushGeometryClip(native.Get()))
			++_activeGeometryClipCount;
	}
}
void Control::NotifyDpiChanged(float dpiScale)
{
#if CUI_ENABLE_DYNAMIC_XAML
	if (!_declarativeComponentBehavior) return;
	try
	{
		_declarativeComponentBehavior->DpiChanged(*this, dpiScale);
	}
	catch (...)
	{
	}
#else
	(void)dpiScale;
#endif
}

void Control::NotifyDeviceResourcesInvalidated() noexcept
{
#if CUI_ENABLE_DYNAMIC_XAML
	if (!_declarativeComponentBehavior) return;
	try
	{
		_declarativeComponentBehavior->DeviceResourcesInvalidated(*this);
	}
	catch (...)
	{
	}
#endif
}
GET_CPP(Control, cui::drawing::Brush, Background)
{
	return GetComputedBackgroundBrush();
}
SET_CPP(Control, cui::drawing::Brush, Background)
{
	(void)SetDependencyPropertyValue(BackgroundProperty(), std::move(value));
}
ID2D1Brush* Control::CreateBackgroundBrush(
	D2DGraphics& graphics,
	D2D1_SIZE_F bounds) const
{
	const auto brush = GetComputedBackgroundBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? nullptr : brush.CreateBrush(graphics, bounds);
}
cui::drawing::Brush Control::GetComputedBackgroundBrush() const
{
	BindingValue value;
	cui::drawing::Brush brush = cui::drawing::NoBrush();
	if (const_cast<Control*>(this)->TryGetPropertyValue(
		BackgroundProperty(), value))
		(void)value.TryGet(brush);
	return brush;
}
std::optional<cui::drawing::Brush> Control::GetBackgroundBrush() const
{
	auto brush = GetComputedBackgroundBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? std::nullopt
		: std::optional<cui::drawing::Brush>(std::move(brush));
}
GET_CPP(Control, cui::drawing::Brush, Foreground)
{
	return GetComputedForegroundBrush();
}
SET_CPP(Control, cui::drawing::Brush, Foreground)
{
	(void)SetDependencyPropertyValue(ForegroundProperty(), std::move(value));
}
ID2D1Brush* Control::CreateForegroundBrush(
	D2DGraphics& graphics,
	D2D1_SIZE_F bounds) const
{
	const auto brush = GetComputedForegroundBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? nullptr : brush.CreateBrush(graphics, bounds);
}
cui::drawing::Brush Control::GetComputedForegroundBrush() const
{
	BindingValue value;
	cui::drawing::Brush brush = cui::drawing::NoBrush();
	if (const_cast<Control*>(this)->TryGetPropertyValue(
		ForegroundProperty(), value))
		(void)value.TryGet(brush);
	return brush;
}
std::optional<cui::drawing::Brush> Control::GetForegroundBrush() const
{
	auto brush = GetComputedForegroundBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? std::nullopt
		: std::optional<cui::drawing::Brush>(std::move(brush));
}
GET_CPP(Control, cui::drawing::Brush, BorderBrush)
{
	return GetComputedBorderBrush();
}
SET_CPP(Control, cui::drawing::Brush, BorderBrush)
{
	(void)SetDependencyPropertyValue(BorderBrushProperty(), std::move(value));
}
ID2D1Brush* Control::CreateBorderBrush(
	D2DGraphics& graphics,
	D2D1_SIZE_F bounds) const
{
	const auto brush = GetComputedBorderBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? nullptr : brush.CreateBrush(graphics, bounds);
}
cui::drawing::Brush Control::GetComputedBorderBrush() const
{
	BindingValue value;
	cui::drawing::Brush brush = cui::drawing::NoBrush();
	if (const_cast<Control*>(this)->TryGetPropertyValue(
		BorderBrushProperty(), value))
		(void)value.TryGet(brush);
	return brush;
}
std::optional<cui::drawing::Brush> Control::GetLocalBorderBrush() const
{
	auto brush = GetComputedBorderBrush();
	return brush.Kind == cui::drawing::BrushKind::None
		? std::nullopt
		: std::optional<cui::drawing::Brush>(std::move(brush));
}
GET_CPP(Control, double, Opacity)
{
	static const auto& property = OpacityProperty();
	return GetDependencyPropertyValue<double>(property);
}
SET_CPP(Control, double, Opacity)
{
	static const auto& property = OpacityProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, bool, ClipToBounds)
{
	static const auto& property =
		ClipToBoundsProperty();
	return GetDependencyPropertyValue<bool>(property);
}
SET_CPP(Control, bool, ClipToBounds)
{
	static const auto& property =
		ClipToBoundsProperty();
	(void)SetDependencyPropertyValue(property, value);
}
void Control::SetClip(const cui::drawing::Geometry& geometry)
{
	if (_clip && *_clip == geometry) return;
	InvalidateVisualBoundsSubtree();
	_clip = geometry;
	if (auto* window = GetPresentationWindow())
		window->InvalidatePresentationStructure();
	InvalidatePresentationGeometrySubtree();
	InvalidateVisualBoundsSubtree();
}
void Control::ClearClip()
{
	if (!_clip) return;
	InvalidateVisualBoundsSubtree();
	_clip.reset();
	if (auto* window = GetPresentationWindow())
		window->InvalidatePresentationStructure();
	InvalidatePresentationGeometrySubtree();
	InvalidateVisualBoundsSubtree();
}
void Control::SetRenderTransform(const cui::drawing::Transform& transform)
{
	if (transform.Empty())
	{
		ClearRenderTransform();
		return;
	}
	if (_renderTransform && *_renderTransform == transform) return;
	const bool shapeChanged = !_renderTransform
		|| _renderTransform->Operations.size() != transform.Operations.size()
		|| !std::equal(
			_renderTransform->Operations.begin(),
			_renderTransform->Operations.end(), transform.Operations.begin(),
			[](const auto& left, const auto& right)
				{ return left.Kind == right.Kind; });
	InvalidateVisualBoundsSubtree();
	_renderTransform = transform;
	if (shapeChanged)
		if (auto* window = GetPresentationWindow())
			window->InvalidatePresentationStructure();
	InvalidatePresentationTransformSubtree();
	InvalidateVisualBoundsSubtree();
}
void Control::ClearRenderTransform()
{
	if (!_renderTransform) return;
	InvalidateVisualBoundsSubtree();
	_renderTransform.reset();
	if (auto* window = GetPresentationWindow())
		window->InvalidatePresentationStructure();
	InvalidatePresentationTransformSubtree();
	InvalidateVisualBoundsSubtree();
}
void Control::SetRenderTransformOrigin(D2D1_POINT_2F origin)
{
	SetRenderTransformOriginDip(cui::core::Point{ origin.x, origin.y });
}
void Control::SetRenderTransformOriginDip(cui::core::Point origin)
{
	if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) return;
	if (_renderTransformOrigin.x == origin.x
		&& _renderTransformOrigin.y == origin.y) return;
	InvalidateVisualBoundsSubtree();
	_renderTransformOrigin = D2D1::Point2F(origin.x, origin.y);
	InvalidatePresentationTransformSubtree();
	InvalidateVisualBoundsSubtree();
}
bool Control::TryCompleteEmptyRender()
{
#if CUI_ENABLE_DYNAMIC_XAML
	if (_declarativeComponentBehavior) return false;
#endif
	// Mirrors EndRender's bookkeeping. The retained scene records an empty list
	// for this node; skipping BeginRender avoids walking every ancestor for a
	// transform and pushing one clip per ancestor around zero primitives.
	_activeGeometryClipCount = 0;
	_activeAxisAlignedClipMask = 0;
	this->_layoutState.CommitPaint();
	_hasLastInvalidatedClientRect = false;
	return true;
}

void Control::EndRender()
{
	if (!this->GetPresentationWindow() || !this->GetDrawingContext()) return;
#if CUI_ENABLE_DYNAMIC_XAML
	if (_declarativeComponentBehavior)
	{
		try
		{
			_declarativeComponentBehavior->RenderOverlay(
				*this, *this->GetDrawingContext());
		}
		catch (...)
		{
		}
	}
#endif
	while (_activeGeometryClipCount > 0)
	{
		--_activeGeometryClipCount;
		const bool axisAligned = _activeGeometryClipCount < 64u
			&& (_activeAxisAlignedClipMask
				& (uint64_t{ 1 } << _activeGeometryClipCount)) != 0u;
		if (axisAligned) this->GetDrawingContext()->PopDrawRect();
		else this->GetDrawingContext()->PopGeometryClip();
	}
	_activeAxisAlignedClipMask = 0;
	this->GetDrawingContext()->PopLocalTransform();
	this->_layoutState.CommitPaint();
	// The previous invalidation is only a coalescing aid while a visual is
	// waiting to be painted.  Keeping it after a successful paint causes every
	// later region-only request to be unioned with stale (often full-control)
	// damage and eventually promotes local rendering back to a full frame.
	_hasLastInvalidatedClientRect = false;
}

void Control::InvalidateVisual()
{
	this->InvalidateVisualRect(ToD2DRect(GetAbsoluteRectDip()));
}

void Control::InvalidateVisualRect(const D2D1_RECT_F& contentRect)

{
	InvalidateVisualRectCore(contentRect, true);
}

void Control::InvalidateComposition()
{
	VerifyAccess();
	MarkPresentationInvalidation(PresentationInvalidationKind::Composition);
	InvalidateVisualRectCore(ToD2DRect(GetAbsoluteRectDip()), false);
}

void Control::InvalidateVisualRectCore(
	const D2D1_RECT_F& contentRect,
	bool contentChanged)
{
	VerifyAccess();
	if (contentChanged)
		MarkPresentationInvalidation(PresentationInvalidationKind::Content);
	this->_layoutState.InvalidatePaint();
	if (!this->IsVisible || !this->GetPresentationWindow()) return;
	const auto renderedRect = TransformAbsoluteRectToRenderSpace(contentRect);
	const RECT currentClientPixels = this->GetPresentationWindow()->ContentDipRectToClientPixels(renderedRect);
	const D2D1_RECT_F currentRect{
		(float)currentClientPixels.left,
		(float)currentClientPixels.top,
		(float)currentClientPixels.right,
		(float)currentClientPixels.bottom
	};

	D2D1_RECT_F invalidRect = currentRect;
	if (_hasLastInvalidatedClientRect)
	{
		invalidRect.left = (std::min)(_lastInvalidatedClientRect.left, currentRect.left);
		invalidRect.top = (std::min)(_lastInvalidatedClientRect.top, currentRect.top);
		invalidRect.right = (std::max)(_lastInvalidatedClientRect.right, currentRect.right);
		invalidRect.bottom = (std::max)(_lastInvalidatedClientRect.bottom, currentRect.bottom);
	}
	DispatchInvalidatedClientRect(invalidRect);

	_lastInvalidatedClientRect = currentRect;
	_hasLastInvalidatedClientRect = true;
}

void Control::DispatchInvalidatedClientRect(const D2D1_RECT_F& clientRect)
{
	InlinePointerSet<Control> visited;
	for (Control* current = this;
		current && visited.Insert(current);
		current = current->_visualParent)
	{
		if (current->_visualInvalidationDeferral.IsSuspended())
		{
			current->_visualInvalidationDeferral.QueueVisual(
				ToCoreRect(clientRect));
			return;
		}
		if (current->_layoutDeferral.IsSuspended())
		{
			current->_layoutDeferral.QueueVisual(ToCoreRect(clientRect));
			return;
		}
	}
	if (this->GetPresentationWindow())
		this->GetPresentationWindow()->Invalidate(clientRect, false);
}

void Control::UpdateCaretBlinkState(bool focused, int selectionStart, int selectionEnd, bool caretRectValid, const D2D1_RECT_F* caretRect)
{
	bool shouldResetBlink = false;
	if (focused != _caretBlinkFocused)
		shouldResetBlink = focused;
	if (selectionStart != _caretBlinkSelectionStart || selectionEnd != _caretBlinkSelectionEnd)
		shouldResetBlink = true;
	if (caretRectValid != _caretBlinkRectValid)
		shouldResetBlink = true;
	if (caretRectValid && caretRect)
	{
		if (!_caretBlinkRectValid ||
			std::fabs(_caretBlinkRect.left - caretRect->left) > 0.1f ||
			std::fabs(_caretBlinkRect.top - caretRect->top) > 0.1f ||
			std::fabs(_caretBlinkRect.right - caretRect->right) > 0.1f ||
			std::fabs(_caretBlinkRect.bottom - caretRect->bottom) > 0.1f)
		{
			shouldResetBlink = true;
		}
		_caretBlinkRect = *caretRect;
	}
	else
	{
		_caretBlinkRect = { 0,0,0,0 };
	}

	_caretBlinkFocused = focused;
	_caretBlinkSelectionStart = selectionStart;
	_caretBlinkSelectionEnd = selectionEnd;
	_caretBlinkRectValid = caretRectValid;

	if (shouldResetBlink || _caretBlinkResetTick == 0)
		_caretBlinkResetTick = ::GetTickCount64();
	SynchronizeNativeAnimationWindowRegistration();
}

bool Control::IsCaretBlinkVisible() const
{
	if (!_caretBlinkFocused) return false;
	if (!_caretBlinkRectValid) return false;
	if (_caretBlinkSelectionStart != _caretBlinkSelectionEnd) return false;

	const UINT blinkTime = ::GetCaretBlinkTime();
	if (blinkTime == INFINITE || blinkTime == 0)
		return true;

	const ULONGLONG elapsed = ::GetTickCount64() - _caretBlinkResetTick;
	return ((elapsed / blinkTime) % 2ULL) == 0;
}

bool Control::IsCaretBlinkAnimating() const
{
	if (!HasRetainedCaretBlinkAnimation()) return false;

	const UINT blinkTime = ::GetCaretBlinkTime();
	return blinkTime != 0 && blinkTime != INFINITE;
}

UINT Control::GetAnimationIntervalMs()
{
	// A retained caret is a phase animation, not a 60 Hz animation.  Driving it
	// at the generic 16 ms cadence forces the Window to synchronously repaint a
	// focused editor every frame.  That is especially costly when a DataGrid
	// replaces a display cell with a TextBox and can starve subsequent input.
	if (HasRetainedCaretBlinkAnimation())
	{
		const UINT blinkTime = ::GetCaretBlinkTime();
		if (blinkTime != 0 && blinkTime != INFINITE)
			return blinkTime;
	}
	return 16;
}

bool Control::HasRetainedCaretBlinkAnimation() const noexcept
{
	return _caretBlinkFocused && _caretBlinkRectValid
		&& _caretBlinkSelectionStart == _caretBlinkSelectionEnd;
}

bool Control::GetCaretBlinkInvalidRect(D2D1_RECT_F& outRect) const
{
	if (!_caretBlinkFocused) return false;
	if (!_caretBlinkRectValid) return false;
	if (_caretBlinkSelectionStart != _caretBlinkSelectionEnd) return false;
	outRect = _caretBlinkRect;
	return true;
}

Font* Control::GetRenderFont()
{
	if (!this->_renderFont)
		this->ApplyTypographyFont();
	if (!this->_renderFont) return nullptr;
	const float factor = this->GetPresentationWindow()
		? this->GetPresentationWindow()->GetTextScaleFactor() : 1.0f;
	if (!(factor > 1.0001f)) return this->_renderFont.get();
	const float sourceSize = this->_renderFont->FontSize;
	if (!this->_systemScaledFont
		|| std::fabs(this->_systemScaledFontSourceSize - sourceSize) > 0.001f
		|| std::fabs(this->_systemScaledFontFactor - factor) > 0.001f)
	{
		this->_systemScaledFont = std::make_unique<::Font>(
			this->_renderFont->FontFamily, sourceSize * factor);
		this->_systemScaledFontSourceSize = sourceSize;
		this->_systemScaledFontFactor = factor;
	}
	return this->_systemScaledFont.get();
}

GET_CPP(Control, const std::wstring&, FontFamily)
{
	return _fontName;
}

void Control::SetFontFamily(std::wstring value)
{
	static const auto& property =
		FontFamilyProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, const std::wstring&, Language)
{
	return _language;
}

void Control::SetLanguage(std::wstring value)
{
	static const auto& property = LanguageProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, double, FontSize)
{
	return _fontSize;
}

void Control::SetFontSize(double value)
{
	static const auto& property =
		FontSizeProperty();
	(void)SetDependencyPropertyValue(property, value);
}

void Control::ApplyTypographyFont()
{
	_systemScaledFont.reset();
	if (_renderFont)
	{
		_renderFont->FontFamily = _fontName;
		_renderFont->FontSize = static_cast<float>(_fontSize);
	}
	else
	{
		_renderFont = std::make_unique<::Font>(
			_fontName, static_cast<float>(_fontSize));
	}
}

auto Control::GetDataBindings() -> DataBindingCollection&
{
	if (!this->_dataBindings)
		this->_dataBindings = std::make_unique<DataBindingCollection>(this);
	return *this->_dataBindings;
}

bool Control::SupportsNativeProperty(
	const DeclarativePropertyMetadata& metadata) const
{
	if (IsCompiledComponentPropertyCore(metadata))
		return true;
	// A ComponentDefinition is a declarative control type even when its private
	// C++ behavior host reuses a structural Canvas/Panel implementation. Its
	// XAML ControlTemplate contract therefore owns Control.Template; this does
	// not project Template onto the built-in Canvas QName.
	bool componentType = static_cast<bool>(
		GetCompiledComponentTypeTokenCore());
#if CUI_ENABLE_DYNAMIC_XAML
	componentType = componentType
		|| static_cast<bool>(GetDeclarativeTypeDescriptor());
#endif
	if (componentType
		&& metadata.OwnerType() == std::type_index(typeid(Control))
		&& &metadata.Property() == &Control::TemplateProperty())
		return true;
	return IsNativePropertySupportedByUIClass(
		const_cast<Control*>(this)->Type(), metadata);
}

Control* Control::FindDeclarativeTemplatePart(TemplatePartToken token) noexcept
{
	return const_cast<Control*>(static_cast<const Control*>(this)
		->FindDeclarativeTemplatePart(token));
}

const Control* Control::FindDeclarativeTemplatePart(
	TemplatePartToken token) const noexcept
{
	if (!token) return this;
	const auto found = std::find_if(
		_templateNameScope.begin(), _templateNameScope.end(),
		[token](const auto& item) { return item.first == token; });
	return found == _templateNameScope.end() ? nullptr : found->second;
}

bool Control::RegisterDeclarativeTemplatePart(
	TemplatePartToken token,
	Control* instance)
{
	if (!token || !instance
		|| instance->GetTemplatedParent() != this) return false;
	if (std::any_of(_templateNameScope.begin(), _templateNameScope.end(),
		[token](const auto& item) { return item.first == token; })) return false;
	_templateNameScope.emplace_back(token, instance);
	return true;
}

void Control::ClearDeclarativeTemplateScope()
{
	_templateNameScope.clear();
#if CUI_ENABLE_DYNAMIC_XAML
	_templateNameScopeNames.clear();
	_declarativeContentPresenters.clear();
#endif
	_templateEventConnections.clear();
	_templatePartEventConnections.clear();
	_declarativeVisualStates.reset();
}

const ::DependencyPropertyMetadata*
Control::ResolveExactDependencyPropertyMetadata(
	const DependencyProperty& property) const
{
	if (&property == &Panel::BackgroundProperty())
		return &BackgroundPropertyMetadataRelation().Metadata();
	if (&property == &TextElement::ForegroundProperty())
		return &ForegroundPropertyMetadataRelation().Metadata();
	if (&property == &TextElement::FontFamilyProperty())
		return &FontFamilyPropertyMetadataRelation().Metadata();
	if (&property == &TextElement::LanguageProperty())
		return &LanguagePropertyMetadataRelation().Metadata();
	if (&property == &TextElement::FontSizeProperty())
		return &FontSizePropertyMetadataRelation().Metadata();
	if (&property == &Border::BorderBrushProperty())
		return &BorderBrushPropertyMetadataRelation().Metadata();
	if (&property == &Border::BorderThicknessProperty())
		return &BorderThicknessPropertyMetadataRelation().Metadata();
	return UIElement::ResolveExactDependencyPropertyMetadata(property);
}

void Control::VisitDeclaredInheritedProperties(
	void* context, InheritedPropertyVisitor visitor) const
{
	if (!visitor) return;
	visitor(context, DataContextProperty());
	visitor(context, AllowDropProperty());
	visitor(context, FontFamilyProperty());
	visitor(context, LanguageProperty());
	visitor(context, FontSizeProperty());
	visitor(context, ForegroundProperty());
	visitor(context, CursorProperty());
}

void Control::RefreshInheritedPropertyValue(
	const DependencyProperty& property)
{
#if CUI_ENABLE_DYNAMIC_XAML
	const auto* metadata = FindPropertyMetadata(property.Name());
#else
	const auto* metadata = GetPropertyMetadata(property);
#endif
	if (!metadata) return;
	if (!HasDependencyPropertyFlag(
		metadata->Flags(), DependencyPropertyFlags::Inherits))
	{
		(void)ClearPropertyValueOwned(
			*metadata, DependencyPropertyValueSource::Inherited, nullptr, true);
		return;
	}

	BindingValue inheritedValue;
	bool found = false;
	std::unordered_set<Control*> visited;
	for (auto* ancestor = GetInheritanceParent();
		ancestor && visited.insert(ancestor).second;
		ancestor = ancestor->GetInheritanceParent())
	{
#if CUI_ENABLE_DYNAMIC_XAML
		// Runtime-authored component schemas create a distinct DP identity for
		// each descriptor. Design therefore locates the corresponding member by
		// authored name, then verifies the explicit inheritance identity below.
		const auto* candidate = ancestor->FindPropertyMetadata(metadata->Name());
#else
		const auto* candidate = ancestor->GetPropertyMetadata(property);
#endif
		if (!candidate
			|| !HasDependencyPropertyFlag(
				candidate->Flags(), DependencyPropertyFlags::Inherits)
			|| !metadata->HasSameInheritanceIdentity(*candidate))
			continue;
		if (candidate->TryGet(*ancestor, inheritedValue))
		{
			found = true;
			break;
		}
	}

	if (found)
		(void)TrySetPropertyValueOwned(
			*metadata, inheritedValue,
			DependencyPropertyValueSource::Inherited, nullptr, true);
	else
		(void)ClearPropertyValueOwned(
			*metadata, DependencyPropertyValueSource::Inherited, nullptr, true);
}

void Control::RefreshInheritedPropertyValues()
{
#if CUI_ENABLE_DYNAMIC_XAML
	// Design/dynamic-XAML keeps registry discovery for runtime-authored metadata.
	const auto properties = DependencyPropertyRegistry::GetProperties(*this);
	for (const auto* metadata : properties)
	{
		if (!metadata || !HasDependencyPropertyFlag(
			metadata->Flags(), DependencyPropertyFlags::Inherits)) continue;
		RefreshInheritedPropertyValue(metadata->Property());
	}
#else
	// Production discovers only static declarations and sparse values already
	// present in this inheritance chain; it never enumerates the global registry.
	std::vector<const DependencyProperty*> properties;
	auto appendUnique = [](void* context, const DependencyProperty& property)
	{
		auto& values = *static_cast<
			std::vector<const DependencyProperty*>*>(context);
		if (std::find(values.begin(), values.end(), &property) == values.end())
			values.push_back(&property);
	};

	VisitDeclaredInheritedProperties(&properties, appendUnique);
	for (const auto& [property, entry] : _propertyValues)
	{
		// A higher-precedence source may hide this slot.  Occupancy, rather than
		// EffectiveSource, is what preserves the identity needed for stale clear.
		if (property && entry.Slots.front().IsOccupied())
			appendUnique(&properties, *property);
	}

	std::unordered_set<Control*> visited;
	for (auto* ancestor = GetInheritanceParent();
		ancestor && visited.insert(ancestor).second;
		ancestor = ancestor->GetInheritanceParent())
	{
		ancestor->VisitDeclaredInheritedProperties(&properties, appendUnique);
		for (const auto& [property, entry] : ancestor->_propertyValues)
		{
			if (!property || !entry.HasSources()) continue;
			const auto* metadata = ancestor->GetPropertyMetadata(*property);
			if (metadata && HasDependencyPropertyFlag(
				metadata->Flags(), DependencyPropertyFlags::Inherits))
				appendUnique(&properties, *property);
		}
	}

	for (const auto* property : properties)
		if (property) RefreshInheritedPropertyValue(*property);
#endif
}

void Control::RefreshInheritedPropertyRecursive(
	const DependencyProperty& property)
{
	const ControlWeakReference selfReference(this);
	const bool previous = _refreshingInheritedProperties;
	_refreshingInheritedProperties = true;
	RefreshInheritedPropertyValue(property);
	auto* live = selfReference.Get();
	if (!live) return;
	std::vector<ControlWeakReference> children;
	children.reserve(live->_inheritanceChildren.size());
	for (auto* child : live->_inheritanceChildren)
		if (child) children.emplace_back(child);
	for (const auto& childReference : children)
	{
		live = selfReference.Get();
		if (!live) return;
		auto* child = childReference.Get();
		if (!child || std::find(live->_inheritanceChildren.begin(),
			live->_inheritanceChildren.end(), child)
			== live->_inheritanceChildren.end()) continue;
		child->RefreshInheritedPropertyRecursive(property);
	}
	live = selfReference.Get();
	if (live) live->_refreshingInheritedProperties = previous;
}

void Control::RefreshInheritedPropertiesRecursive()
{
	const ControlWeakReference selfReference(this);
	const bool previous = _refreshingInheritedProperties;
	_refreshingInheritedProperties = true;
	RefreshInheritedPropertyValues();
	auto* live = selfReference.Get();
	if (!live) return;
	std::vector<ControlWeakReference> children;
	children.reserve(live->_inheritanceChildren.size());
	for (auto* child : live->_inheritanceChildren)
		if (child) children.emplace_back(child);
	for (const auto& childReference : children)
	{
		live = selfReference.Get();
		if (!live) return;
		auto* child = childReference.Get();
		if (!child || std::find(live->_inheritanceChildren.begin(),
			live->_inheritanceChildren.end(), child)
			== live->_inheritanceChildren.end()) continue;
		child->RefreshInheritedPropertiesRecursive();
	}
	live = selfReference.Get();
	if (live) live->_refreshingInheritedProperties = previous;
}

void Control::ApplyPropertyMetadataChange(
	const DeclarativePropertyMetadata& metadata,
	const BindingValue& oldValue,
	const BindingValue& newValue)
{
	if (DeferPropertyMetadataChange(metadata, oldValue, newValue)) return;
	const ControlWeakReference selfReference(this);
	const auto flags = metadata.Flags();
	#if CUI_ENABLE_DYNAMIC_XAML
	const auto& propertyName = metadata.Name();
	#endif
	++_propertyChangeVersion;
	metadata.NotifyChanged(*this, oldValue, newValue);
	auto* live = selfReference.Get();
	if (!live) return;
	if (HasDependencyPropertyFlag(flags, DependencyPropertyFlags::AffectsMeasure))
		live->RequestLayout();
	else if (HasDependencyPropertyFlag(flags, DependencyPropertyFlags::AffectsArrange))
		live->RequestArrange();
	live = selfReference.Get();
	if (!live) return;
	if (HasDependencyPropertyFlag(flags, DependencyPropertyFlags::AffectsRender))
		live->InvalidateVisual();
	live = selfReference.Get();
	if (!live) return;
	const ControlWeakReference visualParentReference(live->_visualParent);
	if (auto* visualParent = visualParentReference.Get())
	{
		if (HasDependencyPropertyFlag(
			flags, DependencyPropertyFlags::AffectsParentMeasure))
			visualParent->RequestLayout();
		else if (HasDependencyPropertyFlag(
			flags, DependencyPropertyFlags::AffectsParentArrange))
		{
			// Attached layout properties such as Canvas.Left invalidate the
			// parent's child-arrangement policy.  A Panel must mark that policy
			// dirty; invalidating only its own arrange slot does not rerun the
			// panel engine when the slot itself is unchanged.
			if (auto* panelParent = dynamic_cast<Panel*>(visualParent))
				panelParent->InvalidateArrangeLayout();
			else
				visualParent->RequestArrange();
		}
	}
	live = selfReference.Get();
	if (!live) return;
	if (!live->_refreshingInheritedProperties
		&& HasDependencyPropertyFlag(flags, DependencyPropertyFlags::Inherits))
	{
		std::vector<ControlWeakReference> children;
		children.reserve(live->_inheritanceChildren.size());
		for (auto* child : live->_inheritanceChildren)
			if (child) children.emplace_back(child);
		for (const auto& childReference : children)
		{
			live = selfReference.Get();
			if (!live) return;
			auto* child = childReference.Get();
			if (!child || std::find(live->_inheritanceChildren.begin(),
				live->_inheritanceChildren.end(), child)
				== live->_inheritanceChildren.end()) continue;
#if CUI_ENABLE_DYNAMIC_XAML
			// Declarative schemas do not share the parent's DP object. Discover
			// each descendant's member and match its InheritanceKey in the
			// Design-only refresh path.
			child->RefreshInheritedPropertiesRecursive();
#else
			child->RefreshInheritedPropertyRecursive(metadata.Property());
#endif
		}
	}

	DependencyPropertyChangedEventArgs args{
		metadata.Property(), oldValue, newValue };
	live = selfReference.Get();
	if (!live) return;
	cui::framework::EventAccess::Raise(
		live->OnPropertyValueChanged, live, args);
	live = selfReference.Get();
	if (!live) return;
#if CUI_ENABLE_DYNAMIC_XAML
	live->_bindingSourcePropertyChanged.Notify(propertyName);
#else
	live->_bindingSourcePropertyChanged.Notify(
		metadata.Property().BindingSourceToken());
#endif
	live = selfReference.Get();
	if (!live) return;
#if CUI_ENABLE_DYNAMIC_XAML
	if (propertyName != L"Text") return;
#else
	if (metadata.Property().BindingSourceToken()
		!= MakeBindingSourcePropertyToken(L"Text")) return;
#endif
	if (auto* window = live->GetPresentationWindow())
	{
		window->NotifyAccessibilityEvent(live, AccessibilityChange::Name);
		window->NotifyAccessibilityEvent(live, AccessibilityChange::Value);
	}
}

bool Control::RaiseDeclarativeEvent(
	const DeclarativeEventDefinition& definition,
	BindingValue value)
{
	DeclarativeEventArgs args;
	args.Definition = &definition;
	args.Value = std::move(value);
#if CUI_ENABLE_DYNAMIC_XAML
	args.Name = definition.Name;
#endif
	return RaiseDeclarativeEvent(args);
}

bool Control::RaiseDeclarativeEvent(DeclarativeEventArgs& args)
{
	const auto* definition = args.Definition;
#if CUI_ENABLE_DYNAMIC_XAML
	if (!definition && !args.Name.empty())
		definition = FindDeclarativeEvent(args.Name);
#endif
	if (!definition || definition->PayloadKind != args.Value.Kind()) return false;
	args.Definition = definition;
#if CUI_ENABLE_DYNAMIC_XAML
	args.Name = definition->Name;
	args.OwnerType = GetDeclarativeTypeId();
#endif
	args.RoutingStrategy = definition->RoutingStrategy;
	args.OriginalSource = this;
	args.Source = this;

	const auto route = BuildRoutedEventRoute(
		this, definition->RoutingStrategy);
	const ControlWeakReference sourceLifetime(this);
	for (const auto& currentReference : route)
	{
		if (!sourceLifetime) break;
		auto* current = currentReference.Get();
		if (!current) continue;
		args.CurrentTarget = current;
		cui::framework::EventAccess::Raise(
			current->OnDeclarativeEvent, current, args);
	}
	args.CurrentTarget = nullptr;
	if (!sourceLifetime)
	{
		args.OriginalSource = nullptr;
		args.Source = nullptr;
	}
	return true;
}

struct Control::DeclarativeVisualStateRuntime
{
	struct RuntimeCondition
	{
		const DependencyPropertyMetadata* Metadata = nullptr;
		BindingValue Value;
	};

	struct RuntimeSetter
	{
		Control* Target = nullptr;
		const DependencyPropertyMetadata* Metadata = nullptr;
		BindingValue Value;
	};

	enum class TransformMember : unsigned char
	{
		X,
		Y,
		ScaleX,
		ScaleY,
		Angle,
		AngleX,
		AngleY,
		CenterX,
		CenterY,
		Matrix,
	};

	struct TransformAccessor
	{
		size_t OperationIndex = 0;
		cui::drawing::TransformKind OperationKind =
			cui::drawing::TransformKind::Translate;
		TransformMember Member = TransformMember::X;
		uint64_t Identity = 0;
	};

	enum class GeometryMember : unsigned char
	{
		Rect,
		Center,
		RadiusX,
		RadiusY,
		FillRule,
	};

	struct GeometryAccessor
	{
		std::vector<size_t> ChildIndices;
		cui::drawing::GeometryKind GeometryKind =
			cui::drawing::GeometryKind::Rectangle;
		GeometryMember Member = GeometryMember::Rect;
		uint64_t Identity = 0;
	};

	enum class PathGeometryMember : unsigned char
	{
		FigureStartPoint,
		FigureIsClosed,
		FigureIsFilled,
		SegmentPoint,
		SegmentPoint1,
		SegmentPoint2,
		SegmentPoint3,
		ArcSize,
		ArcRotationAngle,
		ArcIsLargeArc,
		ArcSweepDirection,
	};

	struct PathGeometryAccessor
	{
		std::vector<size_t> ChildIndices;
		size_t FigureIndex = 0;
		size_t SegmentIndex = 0;
		bool HasSegment = false;
		cui::drawing::PathSegmentKind SegmentKind =
			cui::drawing::PathSegmentKind::Line;
		PathGeometryMember Member = PathGeometryMember::FigureStartPoint;
		uint64_t Identity = 0;
	};

	struct GeometryTransformAccessor
	{
		std::vector<size_t> ChildIndices;
		cui::drawing::GeometryKind GeometryKind =
			cui::drawing::GeometryKind::Rectangle;
		TransformAccessor Transform;
		uint64_t Identity = 0;
	};

	enum class BrushMember : unsigned char
	{
		SolidColor,
		Opacity,
		StartPoint,
		EndPoint,
		Center,
		GradientOrigin,
		RadiusX,
		RadiusY,
		GradientStopColor,
		GradientStopOffset,
	};

	struct BrushAccessor
	{
		size_t StopIndex = 0;
		cui::drawing::BrushKind BrushKind =
			cui::drawing::BrushKind::LinearGradient;
		BrushMember Member = BrushMember::GradientStopColor;
		uint64_t Identity = 0;
	};

	struct BrushTransformAccessor
	{
		cui::drawing::BrushKind BrushKind =
			cui::drawing::BrushKind::LinearGradient;
		bool Relative = false;
		TransformAccessor Transform;
		uint64_t Identity = 0;
	};

	/**
	 * Identifies an adapter from a Storyboard object-property path to the
	 * animatable value at its leaf. New object graphs extend this single
	 * boundary instead of adding parallel path fields throughout the runtime.
	 */
	using ObjectPathAccessor = std::variant<
		TransformAccessor, GeometryAccessor, PathGeometryAccessor,
		GeometryTransformAccessor, BrushAccessor, BrushTransformAccessor>;

	/** Runtime-owned, already converted key frame; no XAML definition payload. */
	struct RuntimeAnimationKeyFrame
	{
		DeclarativeKeyFrameKind Kind = DeclarativeKeyFrameKind::Linear;
		unsigned long long KeyTimeMilliseconds = 0;
		BindingValue Value;
		DeclarativeEasingKind Easing = DeclarativeEasingKind::Linear;
		DeclarativeEasingMode EasingMode = DeclarativeEasingMode::EaseOut;
		float KeySplineX1 = 0.0f;
		float KeySplineY1 = 0.0f;
		float KeySplineX2 = 1.0f;
		float KeySplineY2 = 1.0f;
		uint16_t KeyTimeSubMillisecondTicks = 0;
		DeclarativeEasingParameters EasingParameters;
	};

	/** WPF CAnimationSegment setup cached once per immutable runtime definition. */
	struct RuntimePathSegment
	{
		DeclarativePathSegmentKind Kind = DeclarativePathSegmentKind::Line;
		std::array<cui::core::Point, 4> Points{};
		std::array<cui::core::Point, 3> D1{};
		std::array<cui::core::Point, 2> D2{};
		cui::core::Point D3;
		cui::core::Point InitialTangent;
		std::array<float, 5> Breaks{};
		std::array<float, 5> Lengths{};
		std::array<float, 4> Mids{};
		uint32_t BreakCount = 2;
		float BaseLength = 0.0f;
		float Length = 0.0f;
	};

	/**
	 * Target-independent, normalized timeline data shared by Design and flat
	 * AOT.  This record owns no clock or target state; a definition may be
	 * materialized into more than one clock instance.
	 */
	struct TimelineDefinition
	{
		/** Invalid identifies a direct child of the root Storyboard. */
		uint32_t TimelineGroupIndex = CompiledInteractionInvalidIndex;
		DeclarativeAnimationKind Kind = DeclarativeAnimationKind::Double;
		bool IsAdditive = false;
		bool IsCumulative = false;
		DeclarativePathAnimationHeader Path;
		std::vector<DeclarativePathAnimationSegment> PathSegments;
		std::vector<RuntimePathSegment> PreparedPathSegments;
		float PathTotalLength = 0.0f;
		std::vector<RuntimeAnimationKeyFrame> KeyFrames;
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

	/** Endpoint expressions are definition data, but are animation-specific. */
	struct RuntimeAnimationDefinition : TimelineDefinition
	{
		std::optional<BindingValue> From;
		std::optional<BindingValue> To;
		std::optional<BindingValue> By;
	};

	/** A definition after its target/property path has been resolved. */
	struct RuntimeAnimation : RuntimeAnimationDefinition
	{
		Control* Target = nullptr;
		const DependencyPropertyMetadata* Metadata = nullptr;
		std::optional<ObjectPathAccessor> ObjectPath;
	};

	/** One resolved ParallelTimeline definition shared by every root instance. */
	struct RuntimeTimelineGroup
	{
		uint32_t ParentIndex = CompiledInteractionInvalidIndex;
		DeclarativeStoryboardTimingDefinition Timing;
		unsigned long long ResolvedSimpleDurationMilliseconds = 0;
		bool ResolvedSimpleDurationForever = false;
	};

	static bool TimelineGroupsContainForever(
		const std::vector<RuntimeTimelineGroup>& groups) noexcept
	{
		return std::any_of(groups.begin(), groups.end(), [](const auto& group)
		{
			return group.ResolvedSimpleDurationForever
				|| group.Timing.RepeatBehavior
					== DeclarativeRepeatBehaviorKind::Forever;
		});
	}

	struct RuntimeState
	{
		VisualStateToken Token;
#if CUI_ENABLE_DYNAMIC_XAML
		std::wstring Name;
#endif
		std::vector<RuntimeCondition> Conditions;
		std::vector<const DeclarativeEventDefinition*> Events;
		std::vector<RuntimeSetter> Setters;
		std::vector<RuntimeAnimation> Animations;
		std::vector<RuntimeTimelineGroup> TimelineGroups;
		DeclarativeStoryboardTimingDefinition StoryboardTiming;
	};

	/**
	 * Static identity needed while leaving a state.  Unlike RuntimeAnimation,
	 * this record does not bind an object path against the control's current
	 * Brush/Geometry/Transform shape; an old state's object graph may have
	 * legitimately changed after a FillBehavior=Stop clock released it.
	 */
	struct RuntimeAnimationFootprint
	{
		Control* Target = nullptr;
		const DependencyPropertyMetadata* Metadata = nullptr;
		DeclarativeAnimationKind Kind = DeclarativeAnimationKind::Double;
		uint64_t ObjectPathIdentity = 0;
		const RuntimeAnimation* Resolved = nullptr;
		uint32_t CompiledAnimationIndex = CompiledInteractionInvalidIndex;
		std::optional<RuntimeAnimation> Materialized;
	};

	struct RuntimeStateFootprint
	{
		VisualStateToken Token;
#if CUI_ENABLE_DYNAMIC_XAML
		std::wstring Name;
#endif
		std::vector<RuntimeSetter> Setters;
		std::vector<RuntimeAnimationFootprint> Animations;
	};

	struct RuntimeTransition
	{
		std::optional<size_t> FromState;
		std::optional<size_t> ToState;
		unsigned long long GeneratedDurationMilliseconds = 0;
		DeclarativeEasingKind GeneratedEasing = DeclarativeEasingKind::Linear;
		DeclarativeEasingMode GeneratedEasingMode =
			DeclarativeEasingMode::EaseOut;
		DeclarativeEasingParameters GeneratedEasingParameters;
		std::vector<RuntimeAnimation> Animations;
		std::vector<RuntimeTimelineGroup> TimelineGroups;
		DeclarativeStoryboardTimingDefinition StoryboardTiming;
	};

#if CUI_ENABLE_DYNAMIC_XAML
	struct RuntimeEventStoryboard
	{
		std::wstring Name;
		std::vector<RuntimeAnimation> Animations;
		std::vector<RuntimeTimelineGroup> TimelineGroups;
		DeclarativeStoryboardTimingDefinition Timing;
		bool IsStyleStoryboard = false;
		DeclarativeHandoffBehavior Handoff =
			DeclarativeHandoffBehavior::SnapshotAndReplace;
	};

	struct RuntimeEventTriggerAction
	{
		DeclarativeStoryboardActionKind Kind =
			DeclarativeStoryboardActionKind::Begin;
		size_t StoryboardIndex = 0;
		std::wstring PendingStoryboardName;
		DeclarativeHandoffBehavior Handoff =
			DeclarativeHandoffBehavior::SnapshotAndReplace;
		unsigned long long SeekOffsetMilliseconds = 0;
		double SpeedRatio = 1.0;
	};

	struct RuntimeEventTrigger
	{
		const DeclarativeEventDefinition* Event = nullptr;
		/** None identifies a compiled/dynamic component event. */
		RoutedEventId RoutedEvent = RoutedEventId::None;
		std::vector<RuntimeEventTriggerAction> Actions;
	};
#endif

	struct ClockId
	{
		uint64_t Value = 0;
		bool operator==(const ClockId&) const noexcept = default;
	};

	enum class ClockOwnerKind : unsigned char
	{
		VisualStateGroup,
		ComponentStoryboard,
		StyleTrigger,
		CompiledInteractionStoryboard,
	};

	struct ClockOwnerScope
	{
		ClockOwnerKind Kind = ClockOwnerKind::VisualStateGroup;
		uint64_t Value = 0;
		bool operator==(const ClockOwnerScope&) const noexcept = default;
	};

	struct StoryboardDefinitionId
	{
		uint64_t Value = 0;
		bool operator==(const StoryboardDefinitionId&) const noexcept = default;
	};

	struct StoryboardClockKey
	{
		ClockOwnerScope Owner;
		StoryboardDefinitionId Definition;
		bool operator==(const StoryboardClockKey&) const noexcept = default;
	};

	struct ClockIdentity
	{
		ClockId Instance;
		StoryboardClockKey Storyboard;
	};

	struct RuntimeStyleTriggerScope
	{
		DependencyPropertyValueSource Source = DependencyPropertyValueSource::Style;
		/** Keeps compiled program/value spans alive across style replacement/prune. */
		std::shared_ptr<const ControlStyleSheet> Sheet;
		size_t RuleId = 0;
		bool Active = false;
		const CompiledStyleProgramView* CompiledProgram = nullptr;
		std::span<const BindingValue> CompiledValues;
		CompiledStyleRange CompiledEnterActions;
		CompiledStyleRange CompiledExitActions;
		ClockOwnerScope ClockOwner{
			ClockOwnerKind::StyleTrigger, 0 };
#if CUI_ENABLE_DYNAMIC_XAML
		std::vector<size_t> StoryboardIndices;
		std::vector<RuntimeEventTriggerAction> EnterActions;
		std::vector<RuntimeEventTriggerAction> ExitActions;
#endif
	};

	struct PropertyKey
	{
		Control* Target = nullptr;
		const DependencyProperty* Property = nullptr;
	};

	struct PropertyKeyHash
	{
		size_t operator()(const PropertyKey& value) const noexcept
		{
			const auto target = std::hash<Control*>{}(value.Target);
			const auto property =
				std::hash<const DependencyProperty*>{}(value.Property);
			return target ^ (property + static_cast<size_t>(0x9e3779b9u)
				+ (target << 6) + (target >> 2));
		}
	};

	struct PropertyKeyEqual
	{
		bool operator()(const PropertyKey& left,
			const PropertyKey& right) const noexcept
		{
			return left.Target == right.Target
				&& left.Property == right.Property;
		}
	};

	struct ExactAnimationKey
	{
		PropertyKey Root;
		bool HasObjectPath = false;
		uint64_t ObjectPath = 0;
		bool operator==(const ExactAnimationKey& other) const noexcept
		{
			return PropertyKeyEqual{}(Root, other.Root)
				&& HasObjectPath == other.HasObjectPath
				&& ObjectPath == other.ObjectPath;
		}
	};

	struct ExactAnimationKeyHash
	{
		size_t operator()(const ExactAnimationKey& value) const noexcept
		{
			auto hash = PropertyKeyHash{}(value.Root);
			const auto path = std::hash<uint64_t>{}(value.ObjectPath);
			hash ^= path + static_cast<size_t>(0x9e3779b9u)
				+ (hash << 6) + (hash >> 2);
			return hash ^ (value.HasObjectPath
				? static_cast<size_t>(0x85ebca6bu) : 0u);
		}
	};

	struct PendingTransition
	{
		size_t TargetState = 0;
		unsigned long long EndTick = 0;
		ClockId RootClockId;
		std::vector<PropertyKey> Properties;
	};

	struct RuntimeGroup
	{
		static constexpr size_t DynamicGroupIndex =
			(std::numeric_limits<size_t>::max)();

#if CUI_ENABLE_DYNAMIC_XAML
		VisualStateGroupToken Token;
		std::wstring Name;
		std::vector<RuntimeState> States;
		std::vector<RuntimeTransition> Transitions;
		size_t FallbackState = 0;
#endif
		/**
		 * A compiled group retains only its structural program index and live
		 * state-machine data.  State/transition definitions stay in the static AOT
		 * program and are materialized only while crossing an engine boundary.
		 */
		size_t CompiledGroupIndex = DynamicGroupIndex;
		std::optional<size_t> CurrentState;
		std::optional<PendingTransition> Pending;
#if CUI_ENABLE_DYNAMIC_XAML
		std::vector<const DependencyProperty*> ConditionProperties;
#endif
	};

	struct CompiledInteractionInstance
	{
		CompiledInteractionProgramView Program;
		std::vector<BindingValue> Values;
		std::vector<Control*> Targets;
	};

	struct PropertySnapshot
	{
		PropertyKey Key;
		std::optional<BindingValue> Value;
		DependencyPropertyValueSource Source =
			DependencyPropertyValueSource::VisualState;
	};

	/** Mutable parent-time/control state shared by every leaf in one root. */
	struct ClockRootState
	{
		unsigned long long StartTick = 0;
		unsigned long long LastTick = 0;
		bool Paused = false;
		unsigned long long PauseTick = 0;
		bool Stopped = false;
		// Parent-time origin introduced by an interactive seek. Keeping it
		// separate from StartTick supports destinations beyond process uptime.
		// Stored in the root's parent-time coordinate. Interactive Seek accepts
		// root-local Clock time, so authored SpeedRatio can require a fractional
		// parent-time anchor (WPF ultimately quantizes this to 100 ns ticks).
		long double SeekOffsetMilliseconds = 0.0L;
		double InteractiveSpeedRatio = 1.0;
		std::optional<long double> PendingSeekOffsetMilliseconds;
		std::optional<double> PendingSpeedRatio;
		bool PendingPause = false;
		bool PendingResume = false;
		bool PendingStop = false;
		bool PendingRemove = false;
		// WPF Remove drops the controllable Storyboard lookup immediately,
		// while its value/source mutation remains pending until the timing tick.
		bool ControlLookupAvailable = true;
		/** False until the first committed presentation tick for this Begin. */
		bool HasEvaluated = false;
	};

	static bool HasPendingRootControl(
		const ClockRootState& state) noexcept
	{
		return state.PendingSeekOffsetMilliseconds.has_value()
			|| (state.PendingSpeedRatio.has_value()
				&& (!state.Stopped
					|| state.PendingSeekOffsetMilliseconds.has_value()))
			|| state.PendingPause || state.PendingResume
			|| state.PendingStop || state.PendingRemove;
	}

	/** Root-owned parent time, computed once for all children at one sample tick. */
	struct ClockRootProjection
	{
		long double ElapsedMilliseconds = 0.0L;
		bool BeforeBegin = false;
		bool ActivePeriodComplete = false;
		bool FillStopped = false;
		bool ReentersChildren = false;
		bool Reversed = false;
	};

	static long double ProjectInteractiveRootElapsed(
		const ClockRootState& root,
		unsigned long long nowMilliseconds) noexcept
	{
		const auto clockTick = root.Paused ? root.PauseTick : nowMilliseconds;
		const auto elapsed = clockTick >= root.StartTick
			? clockTick - root.StartTick : 0;
		const auto maximum = static_cast<long double>(
			(std::numeric_limits<unsigned long long>::max)());
		const auto scaledExact = static_cast<long double>(elapsed)
			* static_cast<long double>(root.InteractiveSpeedRatio);
		const auto scaled = !std::isfinite(scaledExact)
			|| scaledExact >= maximum ? maximum : scaledExact;
		return scaled > maximum - root.SeekOffsetMilliseconds
			? maximum : scaled + root.SeekOffsetMilliseconds;
	}

	/** Mutable state that belongs to one animation leaf clock. */
	struct ClockLeafState
	{
		bool Completed = false;
		/** Per-leaf WPF KeySpline solve cache; definition key frames stay immutable. */
		mutable std::vector<double> KeySplineParameters;
	};

	struct ClockNodeId
	{
		uint32_t Index = (std::numeric_limits<uint32_t>::max)();
		uint32_t Generation = 0;

		bool IsValid() const noexcept
		{
			return Generation != 0
				&& Index != (std::numeric_limits<uint32_t>::max)();
		}

		bool operator==(const ClockNodeId&) const noexcept = default;
	};

	enum class ClockNodeKind : unsigned char
	{
		Root,
		Leaf,
	};

	struct ActiveClockRoot
	{
		ClockNodeId NodeId;
		ClockIdentity Identity;
		ClockRootState State;
		DeclarativeStoryboardTimingDefinition Timing;
		unsigned long long ResolvedSimpleDurationMilliseconds = 0;
		bool ResolvedSimpleDurationForever = false;
		std::vector<RuntimeTimelineGroup> TimelineGroups;
	};

	static long double ApplyRootAcceleration(
		long double simpleElapsed,
		long double simpleDuration,
		double accelerationRatio,
		double decelerationRatio) noexcept
	{
		if (simpleDuration <= 0.0L) return 0.0L;
		const long double transition = static_cast<long double>(
			accelerationRatio + decelerationRatio);
		if (transition <= 0.0L) return simpleElapsed;
		auto progress = (std::clamp)(
			simpleElapsed / simpleDuration, 0.0L, 1.0L);
		const auto acceleration = static_cast<long double>(accelerationRatio);
		const auto deceleration = static_cast<long double>(decelerationRatio);
		const auto maximumRate = 2.0L / (2.0L - transition);
		if (progress < acceleration)
			progress = maximumRate * progress * progress
				/ (2.0L * acceleration);
		else if (progress <= 1.0L - deceleration)
			progress = maximumRate * (progress - acceleration / 2.0L);
		else
		{
			const auto complement = 1.0L - progress;
			progress = 1.0L - maximumRate * complement * complement
				/ (2.0L * deceleration);
		}
		return (std::clamp)(progress, 0.0L, 1.0L) * simpleDuration;
	}

	static long double QuantizeWpfTimeSpanTicks(
		long double milliseconds) noexcept
	{
		// WPF Clock parent time is a TimeSpan (100 ns ticks). Preserve fractional
		// milliseconds between root and child, but do not claim more precision
		// than the reference clock can represent.
		constexpr long double ticksPerMillisecond = 10000.0L;
		return std::round(milliseconds * ticksPerMillisecond)
			/ ticksPerMillisecond;
	}

	static ClockRootProjection ProjectClockRoot(
		const ActiveClockRoot& root,
		unsigned long long nowMilliseconds) noexcept
	{
		ClockRootProjection result;
		const auto parentElapsed = ProjectInteractiveRootElapsed(
			root.State, nowMilliseconds);
		if (parentElapsed < root.Timing.BeginTimeMilliseconds)
		{
			result.BeforeBegin = true;
			return result;
		}
		const auto activeParentElapsed = parentElapsed
			- root.Timing.BeginTimeMilliseconds;
		if (root.ResolvedSimpleDurationForever)
		{
			const auto scaled = static_cast<long double>(activeParentElapsed)
				* static_cast<long double>(root.Timing.SpeedRatio);
			result.ElapsedMilliseconds = !std::isfinite(scaled)
				? static_cast<long double>(
					(std::numeric_limits<unsigned long long>::max)())
				: QuantizeWpfTimeSpanTicks((std::min)(scaled,
					static_cast<long double>(
						(std::numeric_limits<unsigned long long>::max)())));
			return result;
		}
		const auto simpleDuration = static_cast<long double>(
			root.ResolvedSimpleDurationMilliseconds);
		const auto repetitionDuration = simpleDuration
			* (root.Timing.AutoReverse ? 2.0L : 1.0L);
		long double activeDuration = 0.0L;
		if (root.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Forever)
			activeDuration = std::numeric_limits<long double>::infinity();
		else if (root.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Duration)
			activeDuration = static_cast<long double>(
				root.Timing.RepeatDurationMilliseconds);
		else
			activeDuration = repetitionDuration
				* static_cast<long double>(root.Timing.RepeatCount)
				/ static_cast<long double>(root.Timing.SpeedRatio);
		result.ActivePeriodComplete = std::isfinite(activeDuration)
			&& static_cast<long double>(activeParentElapsed) >= activeDuration;
		result.ReentersChildren = root.Timing.RepeatBehavior
					!= DeclarativeRepeatBehaviorKind::Count
				|| root.Timing.RepeatCount != 1.0
				|| root.Timing.AutoReverse;
		auto activeElapsed = static_cast<long double>(activeParentElapsed);
		if (std::isfinite(activeDuration))
			activeElapsed = (std::min)(activeElapsed, activeDuration);
		auto simpleElapsed = activeElapsed
			* static_cast<long double>(root.Timing.SpeedRatio);
		if (simpleDuration > 0.0L && repetitionDuration > 0.0L)
		{
			auto local = std::fmod(simpleElapsed, repetitionDuration);
			if (result.ActivePeriodComplete && simpleElapsed > 0.0L
				&& std::fabs(local) < 0.0000001L)
				local = repetitionDuration;
			result.Reversed = root.Timing.AutoReverse
				&& local >= simpleDuration;
			if (result.Reversed)
				local = repetitionDuration - local;
			simpleElapsed = (std::clamp)(local, 0.0L, simpleDuration);
		}
		else simpleElapsed = 0.0L;
		simpleElapsed = ApplyRootAcceleration(simpleElapsed, simpleDuration,
			root.Timing.AccelerationRatio, root.Timing.DecelerationRatio);
		result.ElapsedMilliseconds = QuantizeWpfTimeSpanTicks(simpleElapsed);
		result.FillStopped = result.ActivePeriodComplete
			&& root.Timing.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop;
		return result;
	}

	static ClockRootProjection ProjectTimelineGroup(
		const RuntimeTimelineGroup& group,
		const ClockRootProjection& parent) noexcept
	{
		ClockRootProjection result;
		result.ReentersChildren = parent.ReentersChildren;
		if (parent.BeforeBegin)
		{
			result.BeforeBegin = true;
			return result;
		}
		if (parent.FillStopped)
		{
			result.FillStopped = true;
			return result;
		}
		const auto parentElapsed = parent.ElapsedMilliseconds;
		if (parentElapsed < group.Timing.BeginTimeMilliseconds)
		{
			result.BeforeBegin = true;
			return result;
		}
		const auto activeParentElapsed = parentElapsed
			- group.Timing.BeginTimeMilliseconds;
		if (group.ResolvedSimpleDurationForever)
		{
			const auto scaled = activeParentElapsed
				* static_cast<long double>(group.Timing.SpeedRatio);
			result.ElapsedMilliseconds = !std::isfinite(scaled)
				? static_cast<long double>(
					(std::numeric_limits<unsigned long long>::max)())
				: QuantizeWpfTimeSpanTicks((std::min)(scaled,
					static_cast<long double>(
						(std::numeric_limits<unsigned long long>::max)())));
			return result;
		}
		const auto simpleDuration = static_cast<long double>(
			group.ResolvedSimpleDurationMilliseconds);
		const auto repetitionDuration = simpleDuration
			* (group.Timing.AutoReverse ? 2.0L : 1.0L);
		long double activeDuration = 0.0L;
		if (group.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Forever)
			activeDuration = std::numeric_limits<long double>::infinity();
		else if (group.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Duration)
			activeDuration = static_cast<long double>(
				group.Timing.RepeatDurationMilliseconds);
		else
			activeDuration = repetitionDuration
				* static_cast<long double>(group.Timing.RepeatCount)
				/ static_cast<long double>(group.Timing.SpeedRatio);
		result.ActivePeriodComplete = std::isfinite(activeDuration)
			&& activeParentElapsed >= activeDuration;
		result.ReentersChildren = result.ReentersChildren
			|| group.Timing.RepeatBehavior
				!= DeclarativeRepeatBehaviorKind::Count
			|| group.Timing.RepeatCount != 1.0
			|| group.Timing.AutoReverse;
		auto activeElapsed = activeParentElapsed;
		if (std::isfinite(activeDuration))
			activeElapsed = (std::min)(activeElapsed, activeDuration);
		auto simpleElapsed = activeElapsed
			* static_cast<long double>(group.Timing.SpeedRatio);
		if (simpleDuration > 0.0L && repetitionDuration > 0.0L)
		{
			auto local = std::fmod(simpleElapsed, repetitionDuration);
			if (result.ActivePeriodComplete && simpleElapsed > 0.0L
				&& std::fabs(local) < 0.0000001L)
				local = repetitionDuration;
			result.Reversed = group.Timing.AutoReverse
				&& local >= simpleDuration;
			if (result.Reversed) local = repetitionDuration - local;
			simpleElapsed = (std::clamp)(local, 0.0L, simpleDuration);
		}
		else simpleElapsed = 0.0L;
		simpleElapsed = ApplyRootAcceleration(simpleElapsed, simpleDuration,
			group.Timing.AccelerationRatio,
			group.Timing.DecelerationRatio);
		result.ElapsedMilliseconds = QuantizeWpfTimeSpanTicks(simpleElapsed);
		result.FillStopped = result.ActivePeriodComplete
			&& group.Timing.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop;
		return result;
	}

	/** Target/interpolation payload shared by transaction candidates and clocks. */
	struct AnimationPayload : TimelineDefinition, ClockLeafState
	{
		Control* Target = nullptr;
		const DependencyPropertyMetadata* Metadata = nullptr;
		BindingValue From;
		BindingValue To;
		std::optional<ObjectPathAccessor> ObjectPath;
		DeclarativeHandoffBehavior Handoff =
			DeclarativeHandoffBehavior::SnapshotAndReplace;
	};

	/** Snapshot/default-origin values owned by a property animation layer. */
	struct AnimationLayerValueState
	{
		BindingValue Snapshot;
		BindingValue Base;
		BindingValue Foundation;
		bool DynamicFrom = false;
		bool DynamicFoundation = false;
	};

	/** Committed leaf: the generation-safe parent NodeId is its only root link. */
	struct ActiveAnimation : AnimationPayload
	{
		ClockNodeId NodeId;
		ClockNodeId Parent;
	};

	/** Transaction-local leaf before its committed parent node has been bound. */
	struct CandidateAnimation : AnimationPayload
	{
		ClockId RootClockId;
		AnimationLayerValueState LayerValues;
	};

	/** One committed property-layer owner with rollback-safe value state. */
	struct AnimationLayerRecord
	{
		ClockNodeId Owner;
		bool HasObjectPath = false;
		uint64_t ObjectPath = 0;
		DeclarativeHandoffBehavior Handoff =
			DeclarativeHandoffBehavior::SnapshotAndReplace;
		uint32_t ExactKeyIndex = 0;
		AnimationLayerValueState Values;
	};

	/** Explicit ordered layers for one target root dependency property. */
	struct AnimationLayerStack
	{
		PropertyKey Root;
		uint32_t LayerOffset = 0;
		uint32_t LayerCount = 0;
	};

	struct ClockNodeSlot
	{
		uint32_t Generation = 1;
		bool Occupied = false;
		ClockNodeKind Kind = ClockNodeKind::Root;
		ClockNodeId Parent;
		ClockId RootClockId;
		uint32_t PayloadIndex = (std::numeric_limits<uint32_t>::max)();
		uint32_t ChildOffset = 0;
		uint32_t ChildCount = 0;
	};

	struct ClockNodeArena
	{
		std::vector<ClockNodeSlot> Slots;
		std::vector<uint32_t> FreeIndices;
		std::vector<ActiveClockRoot> Roots;
		std::vector<ActiveAnimation> Leaves;
		std::vector<ClockNodeId> ChildOrder;

		static uint32_t NextGeneration(uint32_t value) noexcept
		{
			++value;
			if (value == 0) ++value;
			return value;
		}

		const ClockNodeSlot* Resolve(
			ClockNodeId id, ClockNodeKind kind) const noexcept
		{
			if (!id.IsValid() || id.Index >= Slots.size()) return nullptr;
			const auto& slot = Slots[id.Index];
			return slot.Occupied && slot.Generation == id.Generation
				&& slot.Kind == kind ? &slot : nullptr;
		}

		bool Allocate(
			ClockNodeKind kind,
			ClockNodeId parent,
			ClockId rootClockId,
			uint32_t payloadIndex,
			ClockNodeId& result)
		{
			uint32_t index = 0;
			if (!FreeIndices.empty())
			{
				index = FreeIndices.back();
				FreeIndices.pop_back();
			}
			else
			{
				if (Slots.size()
					>= (std::numeric_limits<uint32_t>::max)()) return false;
				index = static_cast<uint32_t>(Slots.size());
				Slots.emplace_back();
			}
			auto& slot = Slots[index];
			if (slot.Occupied || slot.Generation == 0) return false;
			slot.Occupied = true;
			slot.Kind = kind;
			slot.Parent = parent;
			slot.RootClockId = rootClockId;
			slot.PayloadIndex = payloadIndex;
			slot.ChildOffset = 0;
			slot.ChildCount = 0;
			result = { index, slot.Generation };
			return true;
		}

		void Release(uint32_t index)
		{
			if (index >= Slots.size() || !Slots[index].Occupied) return;
			// Reserve the reusable index before invalidating the slot so an
			// allocation failure cannot leave an unoccupied, unreachable slot.
			FreeIndices.push_back(index);
			auto& slot = Slots[index];
			slot.Occupied = false;
			slot.Generation = NextGeneration(slot.Generation);
			slot.Kind = ClockNodeKind::Root;
			slot.Parent = {};
			slot.RootClockId = {};
			slot.PayloadIndex = (std::numeric_limits<uint32_t>::max)();
			slot.ChildOffset = 0;
			slot.ChildCount = 0;
		}

		size_t OccupiedCount() const noexcept
		{
			return static_cast<size_t>(std::count_if(
				Slots.begin(), Slots.end(),
				[](const auto& slot) { return slot.Occupied; }));
		}
	};

	static const TransformAccessor* AsTransformPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<TransformAccessor>(&*path) : nullptr;
	}

	static const GeometryAccessor* AsGeometryPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<GeometryAccessor>(&*path) : nullptr;
	}

	static const GeometryTransformAccessor* AsGeometryTransformPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<GeometryTransformAccessor>(&*path) : nullptr;
	}

	static const PathGeometryAccessor* AsPathGeometryPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<PathGeometryAccessor>(&*path) : nullptr;
	}

	static const BrushAccessor* AsBrushPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<BrushAccessor>(&*path) : nullptr;
	}

	static const BrushTransformAccessor* AsBrushTransformPath(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		return path ? std::get_if<BrushTransformAccessor>(&*path) : nullptr;
	}

	static uint64_t ObjectPathIdentity(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		if (const auto* transform = AsTransformPath(path))
			return transform->Identity;
		if (const auto* geometry = AsGeometryPath(path))
			return geometry->Identity;
		if (const auto* pathGeometry = AsPathGeometryPath(path))
			return pathGeometry->Identity;
		if (const auto* geometryTransform = AsGeometryTransformPath(path))
			return geometryTransform->Identity;
		if (const auto* brush = AsBrushPath(path))
			return brush->Identity;
		if (const auto* brushTransform = AsBrushTransformPath(path))
			return brushTransform->Identity;
		return 0;
	}

	static uint64_t MakeObjectPathIdentity(
		std::wstring_view canonicalPath) noexcept
	{
		return canonicalPath.empty()
			? 0 : MakeCompiledInteractionNameToken(canonicalPath);
	}

	static bool ObjectPathUsesFloat(
		const std::optional<ObjectPathAccessor>& path) noexcept
	{
		if (const auto* transform = AsTransformPath(path))
			return transform->Member != TransformMember::Matrix;
		if (const auto* transform = AsGeometryTransformPath(path))
			return transform->Transform.Member != TransformMember::Matrix;
		if (const auto* transform = AsBrushTransformPath(path))
			return transform->Transform.Member != TransformMember::Matrix;
		if (const auto* pathGeometry = AsPathGeometryPath(path))
			return pathGeometry->Member == PathGeometryMember::ArcRotationAngle;
		if (const auto* geometry = AsGeometryPath(path))
			return geometry->Member == GeometryMember::RadiusX
			|| geometry->Member == GeometryMember::RadiusY;
		const auto* brush = AsBrushPath(path);
		return brush && (brush->Member == BrushMember::Opacity
			|| brush->Member == BrushMember::RadiusX
			|| brush->Member == BrushMember::RadiusY
			|| brush->Member == BrushMember::GradientStopOffset);
	}

	Control* Owner = nullptr;
	std::optional<unsigned long long> AnimationClockOverride;
	bool LastAdvanceFailed = false;
	bool FailNextFrameCommitForTesting = false;
	std::vector<RuntimeGroup> Groups;
	std::vector<EventConnection> Connections;
	ClockNodeArena ClockNodes;
	std::vector<AnimationLayerStack> AnimationLayerStacks;
	std::vector<AnimationLayerRecord> AnimationLayers;
	std::vector<uint32_t> AnimationLayerByLeafPayload;
	size_t AnimationExactKeyCount = 0;
	bool ClockNodesDirty = true;
#if CUI_ENABLE_DYNAMIC_XAML
	std::vector<RuntimeEventStoryboard> EventStoryboards;
	std::vector<RuntimeEventTrigger> EventTriggers;
#endif
	std::vector<RuntimeStyleTriggerScope> StyleTriggerScopes;
	std::optional<CompiledInteractionInstance> CompiledInteractions;
#if CUI_ENABLE_DYNAMIC_XAML
	std::vector<size_t> FreeStyleStoryboardIndices;
#endif
	uint64_t NextClockId = 1;
	uint64_t NextStyleOwnerScopeId = 1;
	bool DeclarativeInteractionsDefined = false;
	bool InstallingInteractions = false;
	bool SuppressStateChangedEvents = false;
	std::vector<PropertySnapshot> FailedCompiledSnapshots;
	bool Applying = false;

	unsigned long long NowMilliseconds() const noexcept
	{
		return AnimationClockOverride.value_or(::GetTickCount64());
	}

	static ClockOwnerScope VisualStateOwner(size_t groupIndex) noexcept
	{
		return { ClockOwnerKind::VisualStateGroup,
			static_cast<uint64_t>(groupIndex) };
	}

	static StoryboardClockKey VisualStateClockKey(
		size_t groupIndex,
		size_t stateIndex) noexcept
	{
		return { VisualStateOwner(groupIndex),
			{ static_cast<uint64_t>(stateIndex) } };
	}

	static StoryboardClockKey ComponentStoryboardClockKey(
		size_t storyboardIndex) noexcept
	{
		return { { ClockOwnerKind::ComponentStoryboard, 0 },
			{ static_cast<uint64_t>(storyboardIndex) } };
	}

	static StoryboardClockKey CompiledInteractionStoryboardClockKey(
		uint32_t storyboardIndex) noexcept
	{
		return { { ClockOwnerKind::CompiledInteractionStoryboard, 0 },
			{ static_cast<uint64_t>(storyboardIndex) } };
	}

	static StoryboardClockKey StyleStoryboardClockKey(
		const RuntimeStyleTriggerScope& scope,
		uint64_t storyboardIndex) noexcept
	{
		return { scope.ClockOwner, { storyboardIndex } };
	}

	bool TryAllocateClockIdentity(
		const StoryboardClockKey& storyboard,
		ClockIdentity& identity) noexcept
	{
		if (NextClockId == 0) return false;
		identity = { { NextClockId }, storyboard };
		++NextClockId;
		return true;
	}

	bool TryAllocateStyleOwnerScope(ClockOwnerScope& owner) noexcept
	{
		if (NextStyleOwnerScopeId == 0) return false;
		owner = { ClockOwnerKind::StyleTrigger, NextStyleOwnerScopeId };
		++NextStyleOwnerScopeId;
		return true;
	}

	static const ActiveClockRoot* FindClockRoot(
		const std::vector<ActiveClockRoot>& roots,
		ClockId id) noexcept
	{
		const auto found = std::find_if(roots.begin(), roots.end(),
			[&](const auto& root) { return root.Identity.Instance == id; });
		return found == roots.end() ? nullptr : &*found;
	}

	static ActiveClockRoot* FindClockRoot(
		std::vector<ActiveClockRoot>& roots,
		ClockId id) noexcept
	{
		const auto found = std::find_if(roots.begin(), roots.end(),
			[&](const auto& root) { return root.Identity.Instance == id; });
		return found == roots.end() ? nullptr : &*found;
	}

	static const ActiveClockRoot* FindClockRootByNode(
		const std::vector<ActiveClockRoot>& roots,
		ClockNodeId nodeId) noexcept
	{
		const auto found = std::find_if(roots.begin(), roots.end(),
			[&](const auto& root) { return root.NodeId == nodeId; });
		return found == roots.end() ? nullptr : &*found;
	}

	ActiveClockRoot* FindClockRoot(ClockId id) noexcept
	{
		return FindClockRoot(ClockNodes.Roots, id);
	}

	const ActiveClockRoot* FindClockRoot(ClockId id) const noexcept
	{
		return FindClockRoot(ClockNodes.Roots, id);
	}

	bool AnimationLayerStacksValid() const noexcept
	{
		if (AnimationLayerStacks.empty())
			return ClockNodes.Leaves.empty() && AnimationLayers.empty()
				&& AnimationLayerByLeafPayload.empty()
				&& AnimationExactKeyCount == 0;
		if (ClockNodes.Leaves.empty()
			|| AnimationLayers.size() != ClockNodes.Leaves.size()
			|| AnimationLayerByLeafPayload.size()
				!= ClockNodes.Leaves.size()
			|| AnimationExactKeyCount == 0
			|| AnimationExactKeyCount > AnimationLayers.size()) return false;
		try
		{
			std::vector<bool> owned(ClockNodes.Slots.size(), false);
			std::vector<bool> exactKeyOwned(AnimationExactKeyCount, false);
			std::unordered_set<PropertyKey,
				PropertyKeyHash, PropertyKeyEqual> roots;
			roots.reserve(AnimationLayerStacks.size());
			std::unordered_map<ExactAnimationKey, uint32_t,
				ExactAnimationKeyHash> exactKeys;
			exactKeys.reserve(AnimationExactKeyCount);
			size_t expectedOffset = 0;
			for (const auto& stack : AnimationLayerStacks)
			{
				if (!stack.Root.Target || !stack.Root.Property
					|| stack.LayerCount == 0
					|| stack.LayerOffset != expectedOffset
					|| stack.LayerCount > AnimationLayers.size() - expectedOffset
					|| !roots.insert(stack.Root).second
					|| !stack.Root.Target->HasPropertyValue(
						*stack.Root.Property,
						DependencyPropertyValueSource::Animation)) return false;
				for (size_t layerIndex = 0;
					layerIndex < stack.LayerCount; ++layerIndex)
				{
					const auto absoluteLayerIndex =
						stack.LayerOffset + layerIndex;
					const auto& layer = AnimationLayers[absoluteLayerIndex];
					if (layer.ExactKeyIndex >= AnimationExactKeyCount) return false;
					const ExactAnimationKey exactKey{
						stack.Root, layer.HasObjectPath, layer.ObjectPath };
					const auto [exactPosition, exactInserted] =
						exactKeys.try_emplace(exactKey, layer.ExactKeyIndex);
					if ((!exactInserted
							&& exactPosition->second != layer.ExactKeyIndex)
						|| (exactInserted
							&& exactKeyOwned[layer.ExactKeyIndex])) return false;
					if (exactInserted) exactKeyOwned[layer.ExactKeyIndex] = true;
					const auto* slot = ClockNodes.Resolve(
						layer.Owner, ClockNodeKind::Leaf);
					if (!slot || slot->PayloadIndex >= ClockNodes.Leaves.size()
						|| owned[layer.Owner.Index]
						|| layer.Values.Snapshot.Kind() == BindingValueKind::Empty
						|| layer.Values.Base.Kind() == BindingValueKind::Empty
						|| AnimationLayerByLeafPayload[slot->PayloadIndex]
							!= absoluteLayerIndex
						|| (layer.Handoff
							!= DeclarativeHandoffBehavior::SnapshotAndReplace
							&& layer.Handoff != DeclarativeHandoffBehavior::Compose))
						return false;
					const auto& animation = ClockNodes.Leaves[slot->PayloadIndex];
					if (animation.NodeId != layer.Owner
						|| layer.Handoff != animation.Handoff
						|| animation.Target != stack.Root.Target
						|| !animation.Metadata
						|| &animation.Metadata->Property() != stack.Root.Property
						|| layer.HasObjectPath != animation.ObjectPath.has_value()
						|| layer.ObjectPath
							!= ObjectPathIdentity(animation.ObjectPath)) return false;
					owned[layer.Owner.Index] = true;
				}
				expectedOffset += stack.LayerCount;
			}
			if (expectedOffset != AnimationLayers.size()) return false;
			if (exactKeys.size() != AnimationExactKeyCount
				|| std::find(exactKeyOwned.begin(), exactKeyOwned.end(), false)
					!= exactKeyOwned.end()) return false;
			for (const auto& animation : ClockNodes.Leaves)
				if (!animation.NodeId.IsValid()
					|| animation.NodeId.Index >= owned.size()
					|| !owned[animation.NodeId.Index]) return false;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool RebuildAnimationLayerStacks(
		std::vector<AnimationLayerValueState>* replacementValues = nullptr) noexcept
	{
		try
		{
			if (ClockNodes.Leaves.size()
				> (std::numeric_limits<uint32_t>::max)()
				|| (replacementValues
					&& replacementValues->size()
						!= ClockNodes.Leaves.size())) return false;
			const auto invalidIndex =
				(std::numeric_limits<uint32_t>::max)();
			std::vector<uint32_t> oldLayerBySlot;
			if (!replacementValues)
			{
				oldLayerBySlot.assign(ClockNodes.Slots.size(), invalidIndex);
				for (size_t index = 0; index < AnimationLayers.size(); ++index)
				{
					const auto owner = AnimationLayers[index].Owner;
					if (!owner.IsValid() || owner.Index >= oldLayerBySlot.size()
						|| oldLayerBySlot[owner.Index] != invalidIndex) return false;
					oldLayerBySlot[owner.Index] = static_cast<uint32_t>(index);
				}
			}
			std::vector<AnimationLayerStack> rebuilt;
			rebuilt.reserve(ClockNodes.Leaves.size());
			std::unordered_map<PropertyKey, size_t,
				PropertyKeyHash, PropertyKeyEqual> stackIndices;
			stackIndices.reserve(ClockNodes.Leaves.size());
			for (const auto& animation : ClockNodes.Leaves)
			{
				if (!animation.Target || !animation.Metadata
					|| !animation.NodeId.IsValid()) return false;
				PropertyKey root{
					animation.Target, &animation.Metadata->Property() };
				const auto [position, inserted] = stackIndices.try_emplace(
					root, rebuilt.size());
				if (inserted)
					rebuilt.push_back({ root, 0, 0 });
				auto& count = rebuilt[position->second].LayerCount;
				if (count == (std::numeric_limits<uint32_t>::max)()) return false;
				++count;
			}
			std::vector<uint32_t> cursors(rebuilt.size());
			uint32_t offset = 0;
			for (size_t index = 0; index < rebuilt.size(); ++index)
			{
				rebuilt[index].LayerOffset = offset;
				cursors[index] = offset;
				offset += rebuilt[index].LayerCount;
			}
			if (offset != ClockNodes.Leaves.size()) return false;
			std::vector<AnimationLayerRecord> layers(
				ClockNodes.Leaves.size());
			std::vector<uint32_t> layerByPayload(
				ClockNodes.Leaves.size(), invalidIndex);
			std::unordered_map<ExactAnimationKey, uint32_t,
				ExactAnimationKeyHash> exactKeyIndices;
			exactKeyIndices.reserve(ClockNodes.Leaves.size());
			for (size_t animationIndex = 0;
				animationIndex < ClockNodes.Leaves.size(); ++animationIndex)
			{
				const auto& animation = ClockNodes.Leaves[animationIndex];
				const PropertyKey root{
					animation.Target, &animation.Metadata->Property() };
				const auto position = stackIndices.find(root);
				if (position == stackIndices.end()
					|| position->second >= cursors.size()) return false;
				const auto layerIndex = cursors[position->second]++;
				if (layerIndex >= layers.size()) return false;
				AnimationLayerValueState values;
				if (replacementValues)
					values = std::move((*replacementValues)[animationIndex]);
				else
				{
					if (animation.NodeId.Index >= oldLayerBySlot.size()) return false;
					const auto oldIndex = oldLayerBySlot[animation.NodeId.Index];
					if (oldIndex == invalidIndex || oldIndex >= AnimationLayers.size()
						|| AnimationLayers[oldIndex].Owner
							!= animation.NodeId) return false;
					values = AnimationLayers[oldIndex].Values;
				}
				if (values.Snapshot.Kind() == BindingValueKind::Empty
					|| values.Base.Kind() == BindingValueKind::Empty) return false;
				const auto hasObjectPath = animation.ObjectPath.has_value();
				const auto objectPath = ObjectPathIdentity(animation.ObjectPath);
				const ExactAnimationKey exactKey{
					root, hasObjectPath, objectPath };
				const auto exactPosition = exactKeyIndices.try_emplace(exactKey,
					static_cast<uint32_t>(exactKeyIndices.size())).first;
				layers[layerIndex] = {
					animation.NodeId,
					hasObjectPath,
					objectPath,
					animation.Handoff,
					exactPosition->second,
					std::move(values) };
				layerByPayload[animationIndex] = layerIndex;
			}
			AnimationLayerStacks = std::move(rebuilt);
			AnimationLayers = std::move(layers);
			AnimationLayerByLeafPayload = std::move(layerByPayload);
			AnimationExactKeyCount = exactKeyIndices.size();
			// ClockNodeArenaValid has already proved unique live owner tokens and
			// exact payload coverage. This single construction pass adds every leaf
			// once and uses a unique property-key index; the more expensive
			// independent audit remains available through the testing invariant.
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool ClockNodeArenaValid() const noexcept
	{
		if (ClockNodes.OccupiedCount()
			!= ClockNodes.Roots.size() + ClockNodes.Leaves.size()) return false;
		if (ClockNodes.ChildOrder.size() != ClockNodes.Leaves.size()
			|| ClockNodes.Roots.size()
				> (std::numeric_limits<uint32_t>::max)()
			|| ClockNodes.Leaves.size()
				> (std::numeric_limits<uint32_t>::max)()) return false;
		std::vector<bool> referenced;
		std::vector<bool> free;
		std::vector<bool> childReferenced;
		try
		{
			referenced.assign(ClockNodes.Slots.size(), false);
			free.assign(ClockNodes.Slots.size(), false);
			childReferenced.assign(ClockNodes.Slots.size(), false);
		}
		catch (...)
		{
			return false;
		}
		for (const auto index : ClockNodes.FreeIndices)
		{
			if (index >= ClockNodes.Slots.size() || free[index]
				|| ClockNodes.Slots[index].Occupied) return false;
			free[index] = true;
		}
		size_t expectedChildOffset = 0;
		for (size_t rootIndex = 0; rootIndex < ClockNodes.Roots.size(); ++rootIndex)
		{
			const auto& root = ClockNodes.Roots[rootIndex];
			const auto* slot = ClockNodes.Resolve(
				root.NodeId, ClockNodeKind::Root);
			if (!slot || slot->Parent.IsValid()
				|| slot->RootClockId != root.Identity.Instance
				|| slot->PayloadIndex != rootIndex
				|| slot->ChildOffset != expectedChildOffset
				|| slot->ChildCount > ClockNodes.ChildOrder.size()
					- expectedChildOffset
				|| referenced[root.NodeId.Index]) return false;
			referenced[root.NodeId.Index] = true;
			for (size_t childIndex = 0; childIndex < slot->ChildCount;
				++childIndex)
			{
				const auto childId = ClockNodes.ChildOrder[
					expectedChildOffset + childIndex];
				const auto* child = ClockNodes.Resolve(
					childId, ClockNodeKind::Leaf);
				if (!child || child->Parent != root.NodeId
					|| child->RootClockId != root.Identity.Instance
					|| childReferenced[childId.Index]) return false;
				childReferenced[childId.Index] = true;
			}
			expectedChildOffset += slot->ChildCount;
		}
		if (expectedChildOffset != ClockNodes.ChildOrder.size()) return false;
		for (size_t animationIndex = 0;
			animationIndex < ClockNodes.Leaves.size(); ++animationIndex)
		{
			const auto& animation = ClockNodes.Leaves[animationIndex];
			const auto* slot = ClockNodes.Resolve(
				animation.NodeId, ClockNodeKind::Leaf);
			if (!slot || slot->Parent != animation.Parent
				|| slot->PayloadIndex != animationIndex
				|| slot->ChildOffset != 0 || slot->ChildCount != 0
				|| !childReferenced[animation.NodeId.Index]
				|| referenced[animation.NodeId.Index]) return false;
			const auto* parent = ClockNodes.Resolve(
				slot->Parent, ClockNodeKind::Root);
			if (!parent) return false;
			const auto* root = FindClockRootByNode(
				ClockNodes.Roots, animation.Parent);
			if (!root || root->NodeId != slot->Parent) return false;
			if (parent->RootClockId != root->Identity.Instance) return false;
			referenced[animation.NodeId.Index] = true;
		}
		for (size_t index = 0; index < ClockNodes.Slots.size(); ++index)
			if (ClockNodes.Slots[index].Occupied != referenced[index]
				|| (!ClockNodes.Slots[index].Occupied && !free[index]))
				return false;
		return true;
	}

	bool SynchronizeClockNodeArena(
		std::vector<AnimationLayerValueState>* replacementValues = nullptr) noexcept
	{
		if (!ClockNodesDirty) return replacementValues == nullptr;
		try
		{
			if (ClockNodes.Roots.size()
					> (std::numeric_limits<uint32_t>::max)()
				|| ClockNodes.Leaves.size()
					> (std::numeric_limits<uint32_t>::max)()) return false;
			std::vector<bool> referenced(ClockNodes.Slots.size(), false);
			auto markExisting = [&](ClockNodeId id, ClockNodeKind kind,
				ClockId rootClockId)
				{
					if (!id.IsValid()) return id == ClockNodeId{};
					const auto* slot = ClockNodes.Resolve(id, kind);
					if (!slot || slot->RootClockId != rootClockId
						|| referenced[id.Index]) return false;
					referenced[id.Index] = true;
					return true;
				};
			for (const auto& root : ClockNodes.Roots)
				if (!markExisting(root.NodeId, ClockNodeKind::Root,
					root.Identity.Instance)) return false;
			for (const auto& animation : ClockNodes.Leaves)
			{
				const auto* root = FindClockRootByNode(
					ClockNodes.Roots, animation.Parent);
				if (!root || !markExisting(animation.NodeId,
					ClockNodeKind::Leaf, root->Identity.Instance)) return false;
			}
			for (uint32_t index = 0; index < ClockNodes.Slots.size(); ++index)
				if (ClockNodes.Slots[index].Occupied && !referenced[index])
					ClockNodes.Release(index);
			for (size_t rootIndex = 0;
				rootIndex < ClockNodes.Roots.size(); ++rootIndex)
			{
				auto& root = ClockNodes.Roots[rootIndex];
				if (!root.NodeId.IsValid()
					&& !ClockNodes.Allocate(ClockNodeKind::Root, {},
						root.Identity.Instance, static_cast<uint32_t>(rootIndex),
						root.NodeId)) return false;
				auto& slot = ClockNodes.Slots[root.NodeId.Index];
				slot.PayloadIndex = static_cast<uint32_t>(rootIndex);
				slot.ChildOffset = 0;
				slot.ChildCount = 0;
			}
			for (size_t animationIndex = 0;
				animationIndex < ClockNodes.Leaves.size(); ++animationIndex)
			{
				auto& animation = ClockNodes.Leaves[animationIndex];
				const auto* root = FindClockRootByNode(
					ClockNodes.Roots, animation.Parent);
				if (!root || !root->NodeId.IsValid()) return false;
				if (!animation.NodeId.IsValid())
				{
					if (!ClockNodes.Allocate(ClockNodeKind::Leaf,
						root->NodeId, root->Identity.Instance,
						static_cast<uint32_t>(animationIndex),
						animation.NodeId)) return false;
				}
				else
				{
					const auto* slot = ClockNodes.Resolve(
						animation.NodeId, ClockNodeKind::Leaf);
					if (!slot || slot->Parent != animation.Parent
						|| animation.Parent != root->NodeId) return false;
				}
				auto& slot = ClockNodes.Slots[animation.NodeId.Index];
				slot.PayloadIndex = static_cast<uint32_t>(animationIndex);
				slot.ChildOffset = 0;
				slot.ChildCount = 0;
			}
			ClockNodes.ChildOrder.clear();
			ClockNodes.ChildOrder.reserve(ClockNodes.Leaves.size());
			for (auto& root : ClockNodes.Roots)
			{
				auto& slot = ClockNodes.Slots[root.NodeId.Index];
				slot.ChildOffset = static_cast<uint32_t>(
					ClockNodes.ChildOrder.size());
				for (const auto& animation : ClockNodes.Leaves)
					if (animation.Parent == root.NodeId)
						ClockNodes.ChildOrder.push_back(animation.NodeId);
				slot.ChildCount = static_cast<uint32_t>(
					ClockNodes.ChildOrder.size() - slot.ChildOffset);
			}
			if (!ClockNodeArenaValid()
				|| !RebuildAnimationLayerStacks(replacementValues)) return false;
			ClockNodesDirty = false;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	const ActiveClockRoot* ResolveClockRoot(
		const ActiveAnimation& animation) const noexcept
	{
		const auto* leaf = ClockNodes.Resolve(
			animation.NodeId, ClockNodeKind::Leaf);
		if (!leaf || leaf->Parent != animation.Parent) return nullptr;
		const auto* rootNode = ClockNodes.Resolve(
			leaf->Parent, ClockNodeKind::Root);
		if (!rootNode) return nullptr;
		if (rootNode->PayloadIndex >= ClockNodes.Roots.size()) return nullptr;
		const auto& root = ClockNodes.Roots[rootNode->PayloadIndex];
		return root.NodeId == leaf->Parent
			&& root.Identity.Instance == rootNode->RootClockId ? &root : nullptr;
	}

	ActiveAnimation* ResolveClockLeaf(ClockNodeId id) noexcept
	{
		const auto* slot = ClockNodes.Resolve(id, ClockNodeKind::Leaf);
		if (!slot || slot->PayloadIndex >= ClockNodes.Leaves.size()) return nullptr;
		auto& animation = ClockNodes.Leaves[slot->PayloadIndex];
		return animation.NodeId == id && animation.Parent == slot->Parent
			? &animation : nullptr;
	}

	const AnimationLayerRecord* ResolveAnimationLayer(
		const ActiveAnimation& animation) const noexcept
	{
		const auto* slot = ClockNodes.Resolve(
			animation.NodeId, ClockNodeKind::Leaf);
		if (!slot || slot->PayloadIndex >= AnimationLayerByLeafPayload.size())
			return nullptr;
		const auto layerIndex =
			AnimationLayerByLeafPayload[slot->PayloadIndex];
		if (layerIndex >= AnimationLayers.size()) return nullptr;
		const auto& layer = AnimationLayers[layerIndex];
		return layer.Owner == animation.NodeId ? &layer : nullptr;
	}

	static CandidateAnimation MakeCandidateAnimation(
		const ActiveAnimation& animation,
		ClockId rootClockId,
		const AnimationLayerValueState& values)
	{
		CandidateAnimation result;
		static_cast<AnimationPayload&>(result) =
			static_cast<const AnimationPayload&>(animation);
		result.RootClockId = rootClockId;
		result.LayerValues = values;
		return result;
	}

	bool SnapshotClockCandidates(
		std::vector<CandidateAnimation>& output) const noexcept
	{
		try
		{
			output.clear();
			output.reserve(ClockNodes.Leaves.size());
			for (const auto& animation : ClockNodes.Leaves)
			{
				const auto* root = ResolveClockRoot(animation);
				const auto* layer = ResolveAnimationLayer(animation);
				if (!root || !layer) return false;
				output.push_back(MakeCandidateAnimation(
					animation, root->Identity.Instance, layer->Values));
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool CommitClockCandidates(
		std::vector<CandidateAnimation>&& candidates,
		std::vector<ActiveClockRoot>&& roots) noexcept
	{
		try
		{
			ClockNodes.Roots = std::move(roots);
			ClockNodes.Leaves.clear();
			ClockNodesDirty = true;
			if (!SynchronizeClockNodeArena()) return false;
			ClockNodes.Leaves.reserve(candidates.size());
			std::vector<AnimationLayerValueState> layerValues;
			layerValues.reserve(candidates.size());
			for (auto& candidate : candidates)
			{
				const auto* root = FindClockRoot(candidate.RootClockId);
				if (!root || !root->NodeId.IsValid()) return false;
				ActiveAnimation active;
				layerValues.push_back(std::move(candidate.LayerValues));
				static_cast<AnimationPayload&>(active) = std::move(
					static_cast<AnimationPayload&>(candidate));
				active.Parent = root->NodeId;
				ClockNodes.Leaves.push_back(std::move(active));
			}
			ClockNodesDirty = true;
			return SynchronizeClockNodeArena(&layerValues);
		}
		catch (...)
		{
			return false;
		}
	}

	template<typename TVisitor>
	bool VisitClockLeavesByRoot(
		std::span<const ClockRootProjection> rootProjections,
		TVisitor&& visitor)
	{
		for (auto& root : ClockNodes.Roots)
		{
			const auto* rootNode = ClockNodes.Resolve(
				root.NodeId, ClockNodeKind::Root);
			if (!rootNode || rootNode->PayloadIndex >= ClockNodes.Roots.size()
				|| rootNode->PayloadIndex >= rootProjections.size()
				|| rootNode->ChildOffset > ClockNodes.ChildOrder.size()
				|| rootNode->ChildCount > ClockNodes.ChildOrder.size()
					- rootNode->ChildOffset) return false;
			for (size_t offset = 0; offset < rootNode->ChildCount; ++offset)
			{
				const auto childId = ClockNodes.ChildOrder[
					rootNode->ChildOffset + offset];
				const auto* childNode = ClockNodes.Resolve(
					childId, ClockNodeKind::Leaf);
				auto* animation = ResolveClockLeaf(childId);
				if (!animation || animation->Parent != root.NodeId || !childNode
					|| !visitor(root, rootProjections[rootNode->PayloadIndex],
						*animation, childNode->PayloadIndex))
					return false;
			}
		}
		return true;
	}

	static bool IsVisualStateClock(const ActiveClockRoot& root) noexcept
	{
		return root.Identity.Storyboard.Owner.Kind
			== ClockOwnerKind::VisualStateGroup;
	}

	bool IsVisualStateClock(const ActiveAnimation& animation) const noexcept
	{
		const auto* root = ResolveClockRoot(animation);
		return root && IsVisualStateClock(*root);
	}

	bool IsEventStoryboardClock(
		const ActiveAnimation& animation) const noexcept
	{
		const auto* root = ResolveClockRoot(animation);
		return root && !IsVisualStateClock(*root);
	}

	bool IsOwnedByVisualStateGroup(
		const ActiveAnimation& animation,
		size_t groupIndex) const noexcept
	{
		const auto* root = ResolveClockRoot(animation);
		return root && root->Identity.Storyboard.Owner
			== VisualStateOwner(groupIndex);
	}

	bool IsStoryboardClock(
		const ActiveAnimation& animation,
		const StoryboardClockKey& storyboard) const noexcept
	{
		const auto* root = ResolveClockRoot(animation);
		return root && root->Identity.Storyboard == storyboard;
	}

	static void EraseClockRootsByOwner(
		std::vector<ActiveClockRoot>& roots,
		const ClockOwnerScope& owner)
	{
		roots.erase(std::remove_if(roots.begin(), roots.end(),
			[&](const auto& root)
			{ return root.Identity.Storyboard.Owner == owner; }), roots.end());
	}

	static void EraseClockRootsByStoryboard(
		std::vector<ActiveClockRoot>& roots,
		const StoryboardClockKey& storyboard)
	{
		roots.erase(std::remove_if(roots.begin(), roots.end(),
			[&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; }), roots.end());
	}

	static void EraseClockRoot(
		std::vector<ActiveClockRoot>& roots,
		ClockId id)
	{
		roots.erase(std::remove_if(roots.begin(), roots.end(),
			[&](const auto& root)
			{ return root.Identity.Instance == id; }), roots.end());
	}

	void PruneCandidateRoots(
		std::vector<ActiveClockRoot>& roots,
		const std::vector<CandidateAnimation>& animations) const
	{
		roots.erase(std::remove_if(roots.begin(), roots.end(),
			[&](const auto& root)
			{
				const bool hasLeaf = std::any_of(
					animations.begin(), animations.end(),
					[&](const auto& animation)
					{ return animation.RootClockId == root.Identity.Instance; });
				const bool hasPending = std::any_of(
					Groups.begin(), Groups.end(), [&](const auto& group)
					{ return group.Pending
						&& group.Pending->RootClockId
							== root.Identity.Instance; });
				return !hasLeaf && !hasPending;
			}), roots.end());
	}

	static bool AppendClockRoot(
		std::vector<ActiveClockRoot>& roots,
		const ClockIdentity& identity,
		unsigned long long startTick,
		const DeclarativeStoryboardTimingDefinition& timing,
		unsigned long long resolvedSimpleDurationMilliseconds,
		bool resolvedSimpleDurationForever,
		const std::vector<RuntimeTimelineGroup>& timelineGroups = {})
	{
		try
		{
			ClockRootState state;
			state.StartTick = startTick;
			state.LastTick = startTick;
			roots.push_back({ {}, identity, std::move(state), timing,
				resolvedSimpleDurationMilliseconds,
				resolvedSimpleDurationForever, timelineGroups });
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	static bool InstallClockRoot(
		std::vector<ActiveClockRoot>& roots,
		const ClockIdentity& identity,
		unsigned long long startTick,
		const DeclarativeStoryboardTimingDefinition& timing,
		unsigned long long resolvedSimpleDurationMilliseconds,
		bool resolvedSimpleDurationForever,
		const std::vector<RuntimeTimelineGroup>& timelineGroups = {})
	{
		EraseClockRootsByStoryboard(roots, identity.Storyboard);
		return AppendClockRoot(roots, identity, startTick, timing,
			resolvedSimpleDurationMilliseconds, resolvedSimpleDurationForever,
			timelineGroups);
	}

	void PruneClockRoots() noexcept
	{
		const auto previousSize = ClockNodes.Roots.size();
		ClockNodes.Roots.erase(std::remove_if(
			ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
			[&](const auto& root)
			{
				const bool hasLeaf = std::any_of(
					ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
					[&](const auto& animation)
					{ return animation.Parent == root.NodeId; });
				const bool hasPending = std::any_of(
					Groups.begin(), Groups.end(), [&](const auto& group)
					{ return group.Pending
						&& group.Pending->RootClockId
							== root.Identity.Instance; });
				return !hasLeaf && !hasPending;
			}), ClockNodes.Roots.end());
		if (ClockNodes.Roots.size() != previousSize) ClockNodesDirty = true;
	}

	void PublishClockRootRegistry() noexcept
	{
		if (!SynchronizeClockNodeArena())
			LastAdvanceFailed = true;
		if (Owner)
			Owner->SynchronizeDeclarativeAnimationWindowRegistration();
	}

	~DeclarativeVisualStateRuntime()
	{
		Connections.clear();
		ClearAppliedValues();
		AnimationLayerStacks.clear();
		AnimationLayers.clear();
		AnimationLayerByLeafPayload.clear();
		AnimationExactKeyCount = 0;
		ClockNodes.Leaves.clear();
		ClockNodes.Roots.clear();
	}

	static bool EqualName(
		std::wstring_view left,
		std::wstring_view right) noexcept
	{
		return left == right;
	}

	static const DependencyPropertyMetadata* FindKnownStoryboardRootMetadata(
		Control& target,
		std::wstring_view propertyName)
	{
		const DependencyProperty* property = nullptr;
		if (EqualName(propertyName, L"Background"))
			property = &Control::BackgroundProperty();
		else if (EqualName(propertyName, L"Foreground"))
			property = &Control::ForegroundProperty();
		else if (EqualName(propertyName, L"BorderBrush"))
			property = &Control::BorderBrushProperty();
		else if (EqualName(propertyName, L"Opacity"))
			property = &Control::OpacityProperty();
		else if (EqualName(propertyName, L"RenderTransform"))
			property = &Control::RenderTransformProperty();
		else if (EqualName(propertyName, L"Clip"))
			property = &Control::ClipProperty();
		if (property) return target.GetPropertyMetadata(*property);
#if CUI_ENABLE_DYNAMIC_XAML
		return target.FindPropertyMetadata(std::wstring(propertyName));
#else
		return nullptr;
#endif
	}

	static bool EqualValueToken(
		std::wstring_view left,
		std::wstring_view right) noexcept
	{
		return left.size() == right.size()
			&& std::equal(left.begin(), left.end(), right.begin(),
				[](wchar_t l, wchar_t r)
				{ return std::towlower(l) == std::towlower(r); });
	}

	static bool SameProperty(
		const PropertyKey& left,
		const PropertyKey& right) noexcept
	{
		return left.Target == right.Target
			&& left.Property == right.Property;
	}

	static const DependencyProperty* PropertyIdentity(
		const DependencyPropertyMetadata* metadata) noexcept
	{
		return metadata ? &metadata->Property() : nullptr;
	}

	static bool ContainsName(
		const std::vector<std::wstring>& values,
		const std::wstring& value)
	{
		return std::any_of(values.begin(), values.end(),
			[&](const auto& existing) { return EqualName(existing, value); });
	}

	static bool ContainsObjectPathIdentity(
		const std::vector<uint64_t>& values,
		uint64_t value) noexcept
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	static bool IsNumericKind(BindingValueKind kind) noexcept
	{
		return kind == BindingValueKind::Int
			|| kind == BindingValueKind::Int64
			|| kind == BindingValueKind::Float
			|| kind == BindingValueKind::Double;
	}

	static bool AnimationMatchesMetadata(
		DeclarativeAnimationKind kind,
		const DependencyPropertyMetadata& metadata) noexcept
	{
		if (kind == DeclarativeAnimationKind::Object) return true;
		if (kind == DeclarativeAnimationKind::Int32)
			return metadata.ValueKind() == BindingValueKind::Int;
		if (kind == DeclarativeAnimationKind::Int64)
			return metadata.ValueKind() == BindingValueKind::Int64;
		if (kind == DeclarativeAnimationKind::Single)
			return metadata.ValueKind() == BindingValueKind::Float;
		if (kind == DeclarativeAnimationKind::Double)
			return IsNumericKind(metadata.ValueKind());
		if (kind == DeclarativeAnimationKind::Thickness)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(Thickness));
		if (kind == DeclarativeAnimationKind::Point)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(cui::core::Point));
		if (kind == DeclarativeAnimationKind::Vector)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(cui::core::Vector));
		if (kind == DeclarativeAnimationKind::Rect)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(cui::core::Rect));
		if (kind == DeclarativeAnimationKind::Size)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(cui::core::Size));
		if (kind == DeclarativeAnimationKind::Matrix)
			return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType()
			== std::type_index(typeid(D2D1_MATRIX_3X2_F));
		return metadata.ValueKind() == BindingValueKind::Object
			&& metadata.ValueType() == std::type_index(typeid(D2D1_COLOR_F));
	}

	static std::optional<DeclarativeAnimationKind> GeneratedAnimationKind(
		const DependencyPropertyMetadata& metadata) noexcept
	{
		if (IsNumericKind(metadata.ValueKind()))
			return DeclarativeAnimationKind::Double;
		if (metadata.ValueKind() != BindingValueKind::Object)
			return std::nullopt;
		const auto type = metadata.ValueType();
		if (type == std::type_index(typeid(D2D1_COLOR_F)))
			return DeclarativeAnimationKind::Color;
		if (type == std::type_index(typeid(Thickness)))
			return DeclarativeAnimationKind::Thickness;
		if (type == std::type_index(typeid(cui::core::Point)))
			return DeclarativeAnimationKind::Point;
		if (type == std::type_index(typeid(cui::core::Vector)))
			return DeclarativeAnimationKind::Vector;
		if (type == std::type_index(typeid(cui::core::Rect)))
			return DeclarativeAnimationKind::Rect;
		if (type == std::type_index(typeid(cui::core::Size)))
			return DeclarativeAnimationKind::Size;
		if (type == std::type_index(typeid(D2D1_MATRIX_3X2_F)))
			return DeclarativeAnimationKind::Matrix;
		return std::nullopt;
	}

	static std::wstring_view LocalTypeName(std::wstring_view value) noexcept
	{
		const auto separator = value.rfind(L':');
		return separator == std::wstring_view::npos
			? value : value.substr(separator + 1);
	}

	static bool IsKnownStoryboardBrushRootOwner(
		std::wstring_view owner,
		std::wstring_view property) noexcept
	{
		owner = LocalTypeName(owner);
		return EqualName(owner, L"Control")
			|| EqualName(owner, L"UIElement")
			|| (EqualName(property, L"Background")
				&& (EqualName(owner, L"Panel")
					|| EqualName(owner, L"Border")))
			|| (EqualName(property, L"BorderBrush")
				&& EqualName(owner, L"Border"));
	}

	static bool TryResolveTransformOperationAccessor(
		const cui::drawing::Transform& transform,
		size_t operationIndex,
		std::wstring_view owner,
		std::wstring_view property,
		std::wstring canonicalPrefix,
		TransformAccessor& output,
		std::wstring* outError,
		bool directTransform = false)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		if (operationIndex >= transform.Operations.size())
			return fail(L"动画目标没有路径所需的 Transform 操作。");
		const auto& operation = transform.Operations[operationIndex];
		auto assign = [&](TransformMember member,
			std::wstring_view canonicalOwner,
			std::wstring_view canonicalProperty)
			{
				output.OperationIndex = operationIndex;
				output.OperationKind = operation.Kind;
				output.Member = member;
				const auto canonicalPath = directTransform
					? std::move(canonicalPrefix) + L".("
						+ std::wstring(canonicalOwner) + L"."
						+ std::wstring(canonicalProperty) + L")"
					: std::move(canonicalPrefix) + L"["
						+ std::to_wstring(operationIndex) + L"].("
						+ std::wstring(canonicalOwner) + L"."
						+ std::wstring(canonicalProperty) + L")";
				output.Identity = MakeObjectPathIdentity(canonicalPath);
				if (outError) outError->clear();
				return true;
			};
		switch (operation.Kind)
		{
		case cui::drawing::TransformKind::Translate:
			if (!EqualName(owner, L"TranslateTransform")) break;
			if (EqualName(property, L"X")) return assign(
				TransformMember::X, L"TranslateTransform", L"X");
			if (EqualName(property, L"Y")) return assign(
				TransformMember::Y, L"TranslateTransform", L"Y");
			break;
		case cui::drawing::TransformKind::Scale:
			if (!EqualName(owner, L"ScaleTransform")) break;
			if (EqualName(property, L"ScaleX")) return assign(
				TransformMember::ScaleX, L"ScaleTransform", L"ScaleX");
			if (EqualName(property, L"ScaleY")) return assign(
				TransformMember::ScaleY, L"ScaleTransform", L"ScaleY");
			if (EqualName(property, L"CenterX")) return assign(
				TransformMember::CenterX, L"ScaleTransform", L"CenterX");
			if (EqualName(property, L"CenterY")) return assign(
				TransformMember::CenterY, L"ScaleTransform", L"CenterY");
			break;
		case cui::drawing::TransformKind::Rotate:
			if (!EqualName(owner, L"RotateTransform")) break;
			if (EqualName(property, L"Angle")) return assign(
				TransformMember::Angle, L"RotateTransform", L"Angle");
			if (EqualName(property, L"CenterX")) return assign(
				TransformMember::CenterX, L"RotateTransform", L"CenterX");
			if (EqualName(property, L"CenterY")) return assign(
				TransformMember::CenterY, L"RotateTransform", L"CenterY");
			break;
		case cui::drawing::TransformKind::Skew:
			if (!EqualName(owner, L"SkewTransform")) break;
			if (EqualName(property, L"AngleX")) return assign(
				TransformMember::AngleX, L"SkewTransform", L"AngleX");
			if (EqualName(property, L"AngleY")) return assign(
				TransformMember::AngleY, L"SkewTransform", L"AngleY");
			if (EqualName(property, L"CenterX")) return assign(
				TransformMember::CenterX, L"SkewTransform", L"CenterX");
			if (EqualName(property, L"CenterY")) return assign(
				TransformMember::CenterY, L"SkewTransform", L"CenterY");
			break;
		case cui::drawing::TransformKind::Matrix:
			if (EqualName(owner, L"MatrixTransform")
				&& EqualName(property, L"Matrix"))
				return assign(TransformMember::Matrix,
					L"MatrixTransform", L"Matrix");
			break;
		default:
			break;
		}
		return fail(L"动画路径的 Transform 类型或末端属性与实际操作不匹配"
			L"（operationKind="
			+ std::to_wstring(static_cast<int>(operation.Kind))
			+ L"，owner=" + std::wstring(owner)
			+ L"，property=" + std::wstring(property) + L"）。");
	}

	static bool TryResolveTransformPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		TransformAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const bool renderTransformRoot = !path.Segments.empty()
			&& path.Segments[0].Kind
			== cui::xaml::PropertyPathSegmentKind::Property
			&& EqualName(path.Segments[0].Name, L"RenderTransform")
			&& (EqualName(
				LocalTypeName(path.Segments[0].OwnerType), L"Control")
				|| EqualName(
					LocalTypeName(path.Segments[0].OwnerType), L"UIElement"));
		const bool directTransform = renderTransformRoot
			&& path.Segments.size() == 2
			&& path.Segments[1].Kind
			== cui::xaml::PropertyPathSegmentKind::Property;
		const bool groupedTransform = renderTransformRoot
			&& path.Segments.size() == 4
			&& path.Segments[1].Kind
			== cui::xaml::PropertyPathSegmentKind::Property
			&& path.Segments[2].Kind
			== cui::xaml::PropertyPathSegmentKind::Index
			&& path.Segments[3].Kind
			== cui::xaml::PropertyPathSegmentKind::Property
			&& EqualName(
				LocalTypeName(path.Segments[1].OwnerType), L"TransformGroup")
			&& EqualName(path.Segments[1].Name, L"Children");
		if (!directTransform && !groupedTransform)
			return fail(L"Transform 动画路径必须是 "
				L"(Control.RenderTransform).(TransformType.Property) 或 "
				L"(Control.RenderTransform).(TransformGroup.Children)[n]."
				L"(TransformType.Property)。");

		const auto* metadata = target.GetPropertyMetadata(
			Control::RenderTransformProperty());
		BindingValue current;
		cui::drawing::Transform transform;
		const size_t operationIndex =
			directTransform ? 0 : path.Segments[2].Index;
		if (!metadata || !metadata->CanWrite()
			|| metadata->ValueType()
			!= std::type_index(typeid(cui::drawing::Transform))
			|| !metadata->TryGet(target, current)
			|| !current.TryGet(transform)
			|| (directTransform && transform.Operations.size() != 1)
			|| operationIndex >= transform.Operations.size())
			return fail(L"动画目标没有路径所需的 RenderTransform 操作。");

		const auto& terminal =
			directTransform ? path.Segments[1] : path.Segments[3];
		const auto owner = LocalTypeName(terminal.OwnerType);
		const auto& property = terminal.Name;
		if (!TryResolveTransformOperationAccessor(transform,
			operationIndex, owner, property,
			directTransform
				? L"(Control.RenderTransform)"
				: L"(Control.RenderTransform).(TransformGroup.Children)",
			output, outError, directTransform)) return false;
		outMetadata = metadata;
		return true;
	}

	static const cui::drawing::Geometry* TryGetGeometryChild(
		const cui::drawing::Geometry& root,
		const std::vector<size_t>& childIndices) noexcept
	{
		const auto* current = &root;
		for (const auto index : childIndices)
		{
			if (current->Kind != cui::drawing::GeometryKind::Group
				|| index >= current->Children.size()) return nullptr;
			current = &current->Children[index];
		}
		return current;
	}

	static cui::drawing::Geometry* TryGetGeometryChild(
		cui::drawing::Geometry& root,
		const std::vector<size_t>& childIndices) noexcept
	{
		auto* current = &root;
		for (const auto index : childIndices)
		{
			if (current->Kind != cui::drawing::GeometryKind::Group
				|| index >= current->Children.size()) return nullptr;
			current = &current->Children[index];
		}
		return current;
	}

	/**
	 * Resolves zero or more GeometryGroup.Children[index] hops after
	 * Control.Clip and returns the concrete Geometry that owns the leaf.
	 */
	static bool TryResolveGeometryTreeTarget(
		Control& target,
		const cui::xaml::PropertyPath& path,
		const DependencyPropertyMetadata*& outMetadata,
		const cui::drawing::Geometry*& outGeometry,
		std::vector<size_t>& outChildIndices,
		size_t& outLeafStart,
		std::wstring& outCanonicalPrefix,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		if (path.Segments.empty()
			|| path.Segments[0].Kind
			!= cui::xaml::PropertyPathSegmentKind::Property
			|| !EqualName(path.Segments[0].Name, L"Clip")
			|| (!EqualName(LocalTypeName(path.Segments[0].OwnerType), L"Control")
				&& !EqualName(LocalTypeName(path.Segments[0].OwnerType), L"UIElement")))
			return fail(L"Geometry 复合动画路径必须以 (Control.Clip) 开始。");

		const auto* metadata = target.GetPropertyMetadata(
			Control::ClipProperty());
		const auto& currentClip = target.GetClip();
		if (!metadata || !metadata->CanWrite()
			|| metadata->ValueType()
			!= std::type_index(typeid(cui::drawing::Geometry))
			|| !currentClip)
			return fail(L"动画目标必须显式持有 Control.Clip Geometry。");

		outChildIndices.clear();
		outCanonicalPrefix = L"(Control.Clip)";
		const auto* geometry = &*currentClip;
		size_t cursor = 1;
		while (cursor < path.Segments.size()
			&& path.Segments[cursor].Kind
			== cui::xaml::PropertyPathSegmentKind::Property
			&& EqualName(
				LocalTypeName(path.Segments[cursor].OwnerType), L"GeometryGroup")
			&& EqualName(path.Segments[cursor].Name, L"Children"))
		{
			if (cursor + 1 >= path.Segments.size()
				|| path.Segments[cursor + 1].Kind
				!= cui::xaml::PropertyPathSegmentKind::Index)
				return fail(L"GeometryGroup.Children 后必须提供有效索引。");
			const auto index = path.Segments[cursor + 1].Index;
			if (geometry->Kind != cui::drawing::GeometryKind::Group)
				return fail(L"GeometryGroup.Children 路径所有者与实际 Geometry 类型不匹配。");
			if (index >= geometry->Children.size())
				return fail(L"GeometryGroup.Children 动画索引超出范围。");
			outChildIndices.push_back(index);
			geometry = &geometry->Children[index];
			outCanonicalPrefix += L".(GeometryGroup.Children)["
				+ std::to_wstring(index) + L"]";
			cursor += 2;
		}
		if (cursor >= path.Segments.size())
			return fail(L"GeometryGroup.Children 索引后缺少动画末端属性。");
		outMetadata = metadata;
		outGeometry = geometry;
		outLeafStart = cursor;
		return true;
	}

	static bool TryResolveGeometryPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		GeometryAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const DependencyPropertyMetadata* metadata = nullptr;
		const cui::drawing::Geometry* resolvedGeometry = nullptr;
		std::vector<size_t> childIndices;
		size_t leafStart = 0;
		std::wstring canonicalPrefix;
		if (!TryResolveGeometryTreeTarget(target, path, metadata,
			resolvedGeometry, childIndices, leafStart, canonicalPrefix, outError))
			return false;
		if (path.Segments.size() != leafStart + 1
			|| path.Segments[leafStart].Kind
			!= cui::xaml::PropertyPathSegmentKind::Property)
			return fail(L"Geometry 动画路径必须在目标 Geometry 后定位一个公开成员。");
		const auto owner = LocalTypeName(path.Segments[leafStart].OwnerType);
		const auto& property = path.Segments[leafStart].Name;
		const auto& geometry = *resolvedGeometry;
		output = {};
		output.ChildIndices = std::move(childIndices);
		output.GeometryKind = geometry.Kind;
		auto assign = [&](GeometryMember member, std::wstring_view type,
			std::wstring_view canonicalProperty)
			{
				output.Member = member;
				output.Identity = MakeObjectPathIdentity(canonicalPrefix + L".("
					+ std::wstring(type) + L"."
					+ std::wstring(canonicalProperty) + L")");
				outMetadata = metadata;
				if (outError) outError->clear();
				return true;
			};
		auto validRadius = [](float value)
			{
				return std::isfinite(value) && value >= 0.0f;
			};
		if (geometry.Kind == cui::drawing::GeometryKind::Rectangle
			&& EqualName(owner, L"RectangleGeometry"))
		{
			if (EqualName(property, L"Rect"))
			{
				const auto rect = ToCoreRect(geometry.Rect);
				if (!std::isfinite(rect.x) || !std::isfinite(rect.y)
					|| !std::isfinite(rect.width) || !std::isfinite(rect.height)
					|| rect.width < 0.0f || rect.height < 0.0f)
					return fail(L"动画目标的 RectangleGeometry.Rect 无效。");
				return assign(GeometryMember::Rect,
					L"RectangleGeometry", L"Rect");
			}
			if (EqualName(property, L"RadiusX")
				|| EqualName(property, L"RadiusY"))
			{
				const bool x = EqualName(property, L"RadiusX");
				if (!validRadius(x ? geometry.RadiusX : geometry.RadiusY))
					return fail(L"动画目标的 RectangleGeometry 圆角半径无效。");
				return assign(x ? GeometryMember::RadiusX : GeometryMember::RadiusY,
					L"RectangleGeometry", x ? L"RadiusX" : L"RadiusY");
			}
		}
		if (geometry.Kind == cui::drawing::GeometryKind::Ellipse
			&& EqualName(owner, L"EllipseGeometry"))
		{
			if (EqualName(property, L"Center"))
			{
				if (!std::isfinite(geometry.Center.x)
					|| !std::isfinite(geometry.Center.y))
					return fail(L"动画目标的 EllipseGeometry.Center 无效。");
				return assign(GeometryMember::Center,
					L"EllipseGeometry", L"Center");
			}
			if (EqualName(property, L"RadiusX")
				|| EqualName(property, L"RadiusY"))
			{
				const bool x = EqualName(property, L"RadiusX");
				if (!validRadius(x ? geometry.RadiusX : geometry.RadiusY))
					return fail(L"动画目标的 EllipseGeometry 半径无效。");
				return assign(x ? GeometryMember::RadiusX : GeometryMember::RadiusY,
					L"EllipseGeometry", x ? L"RadiusX" : L"RadiusY");
			}
		}
		if ((geometry.Kind == cui::drawing::GeometryKind::Path
			&& EqualName(owner, L"PathGeometry")
			|| geometry.Kind == cui::drawing::GeometryKind::Group
			&& EqualName(owner, L"GeometryGroup"))
			&& EqualName(property, L"FillRule"))
			return assign(GeometryMember::FillRule, owner, L"FillRule");
		return fail(L"Geometry 动画路径所有者或末端属性与实际 Clip 类型不匹配。");
	}

	static bool TryResolvePathGeometryPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		PathGeometryAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const DependencyPropertyMetadata* metadata = nullptr;
		const cui::drawing::Geometry* resolvedGeometry = nullptr;
		std::vector<size_t> childIndices;
		size_t leafStart = 0;
		std::wstring canonicalPrefix;
		if (!TryResolveGeometryTreeTarget(target, path, metadata,
			resolvedGeometry, childIndices, leafStart, canonicalPrefix, outError))
			return false;
		const auto remaining = path.Segments.size() - leafStart;
		const bool figurePath = remaining == 3;
		const bool segmentPath = remaining == 5;
		if ((!figurePath && !segmentPath)
			|| path.Segments[leafStart].Kind
			!= cui::xaml::PropertyPathSegmentKind::Property
			|| path.Segments[leafStart + 1].Kind
			!= cui::xaml::PropertyPathSegmentKind::Index
			|| path.Segments[leafStart + 2].Kind
			!= cui::xaml::PropertyPathSegmentKind::Property
			|| (segmentPath
				&& (path.Segments[leafStart + 3].Kind
					!= cui::xaml::PropertyPathSegmentKind::Index
					|| path.Segments[leafStart + 4].Kind
					!= cui::xaml::PropertyPathSegmentKind::Property))
			|| !EqualName(
				LocalTypeName(path.Segments[leafStart].OwnerType), L"PathGeometry")
			|| !EqualName(path.Segments[leafStart].Name, L"Figures")
			|| !EqualName(
				LocalTypeName(path.Segments[leafStart + 2].OwnerType), L"PathFigure")
			|| (segmentPath
				&& !EqualName(path.Segments[leafStart + 2].Name, L"Segments")))
			return fail(L"PathGeometry 动画路径必须定位 Figures[n] 的 PathFigure "
				L"成员，或继续定位 Segments[n] 的具体 PathSegment 成员。");
		if (resolvedGeometry->Kind != cui::drawing::GeometryKind::Path)
			return fail(L"动画目标必须显式持有 PathGeometry 类型的 Control.Clip。");
		const auto figureIndex = path.Segments[leafStart + 1].Index;
		if (figureIndex >= resolvedGeometry->Figures.size())
			return fail(L"PathGeometry.Figures 动画索引超出范围。");
		const auto& figure = resolvedGeometry->Figures[figureIndex];
		output = {};
		output.ChildIndices = std::move(childIndices);
		output.FigureIndex = figureIndex;
		auto finitePoint = [](D2D1_POINT_2F point)
			{
				return std::isfinite(point.x) && std::isfinite(point.y);
			};
		auto assignFigure = [&](PathGeometryMember member,
			std::wstring_view property)
			{
				output.Member = member;
				output.Identity = MakeObjectPathIdentity(
					canonicalPrefix + L".(PathGeometry.Figures)["
					+ std::to_wstring(figureIndex) + L"].(PathFigure."
					+ std::wstring(property) + L")");
				outMetadata = metadata;
				if (outError) outError->clear();
				return true;
			};
		if (figurePath)
		{
			const auto& property = path.Segments[leafStart + 2].Name;
			if (EqualName(property, L"StartPoint"))
			{
				if (!finitePoint(figure.StartPoint))
					return fail(L"动画目标的 PathFigure.StartPoint 无效。");
				return assignFigure(
					PathGeometryMember::FigureStartPoint, L"StartPoint");
			}
			if (EqualName(property, L"IsClosed"))
				return assignFigure(
					PathGeometryMember::FigureIsClosed, L"IsClosed");
			if (EqualName(property, L"IsFilled"))
				return assignFigure(
					PathGeometryMember::FigureIsFilled, L"IsFilled");
			return fail(L"尚未支持该 PathFigure 动画成员。");
		}

		const auto segmentIndex = path.Segments[leafStart + 3].Index;
		if (segmentIndex >= figure.Segments.size())
			return fail(L"PathFigure.Segments 动画索引超出范围。");
		const auto& segment = figure.Segments[segmentIndex];
		const auto owner = LocalTypeName(path.Segments[leafStart + 4].OwnerType);
		const auto& property = path.Segments[leafStart + 4].Name;
		output.HasSegment = true;
		output.SegmentIndex = segmentIndex;
		output.SegmentKind = segment.Kind;
		auto assignSegment = [&](PathGeometryMember member,
			std::wstring_view objectType, std::wstring_view canonicalProperty)
			{
				output.Member = member;
				output.Identity = MakeObjectPathIdentity(
					canonicalPrefix + L".(PathGeometry.Figures)["
					+ std::to_wstring(figureIndex)
					+ L"].(PathFigure.Segments)["
					+ std::to_wstring(segmentIndex) + L"].("
					+ std::wstring(objectType) + L"."
					+ std::wstring(canonicalProperty) + L")");
				outMetadata = metadata;
				if (outError) outError->clear();
				return true;
			};
		auto pointMember = [&](PathGeometryMember member,
			std::wstring_view type, std::wstring_view name,
			D2D1_POINT_2F point)
			{
				if (!finitePoint(point))
					return fail(L"动画目标的 PathSegment Point 无效。");
				return assignSegment(member, type, name);
			};
		switch (segment.Kind)
		{
		case cui::drawing::PathSegmentKind::Line:
			if (EqualName(owner, L"LineSegment")
				&& EqualName(property, L"Point"))
				return pointMember(PathGeometryMember::SegmentPoint,
					L"LineSegment", L"Point", segment.Point);
			break;
		case cui::drawing::PathSegmentKind::Bezier:
			if (EqualName(owner, L"BezierSegment"))
			{
				if (EqualName(property, L"Point1")) return pointMember(
					PathGeometryMember::SegmentPoint1,
					L"BezierSegment", L"Point1", segment.Point1);
				if (EqualName(property, L"Point2")) return pointMember(
					PathGeometryMember::SegmentPoint2,
					L"BezierSegment", L"Point2", segment.Point2);
				if (EqualName(property, L"Point3")) return pointMember(
					PathGeometryMember::SegmentPoint3,
					L"BezierSegment", L"Point3", segment.Point3);
			}
			break;
		case cui::drawing::PathSegmentKind::QuadraticBezier:
			if (EqualName(owner, L"QuadraticBezierSegment"))
			{
				if (EqualName(property, L"Point1")) return pointMember(
					PathGeometryMember::SegmentPoint1,
					L"QuadraticBezierSegment", L"Point1", segment.Point1);
				if (EqualName(property, L"Point2")) return pointMember(
					PathGeometryMember::SegmentPoint2,
					L"QuadraticBezierSegment", L"Point2", segment.Point2);
			}
			break;
		case cui::drawing::PathSegmentKind::Arc:
			if (EqualName(owner, L"ArcSegment"))
			{
				if (EqualName(property, L"Point")) return pointMember(
					PathGeometryMember::SegmentPoint,
					L"ArcSegment", L"Point", segment.Point);
				if (EqualName(property, L"Size"))
				{
					if (!std::isfinite(segment.Size.width)
						|| !std::isfinite(segment.Size.height)
						|| segment.Size.width < 0.0f
						|| segment.Size.height < 0.0f)
						return fail(L"动画目标的 ArcSegment.Size 无效。");
					return assignSegment(PathGeometryMember::ArcSize,
						L"ArcSegment", L"Size");
				}
				if (EqualName(property, L"RotationAngle"))
				{
					if (!std::isfinite(segment.RotationAngle))
						return fail(L"动画目标的 ArcSegment.RotationAngle 无效。");
					return assignSegment(PathGeometryMember::ArcRotationAngle,
						L"ArcSegment", L"RotationAngle");
				}
				if (EqualName(property, L"IsLargeArc"))
					return assignSegment(PathGeometryMember::ArcIsLargeArc,
						L"ArcSegment", L"IsLargeArc");
				if (EqualName(property, L"SweepDirection"))
					return assignSegment(PathGeometryMember::ArcSweepDirection,
						L"ArcSegment", L"SweepDirection");
			}
			break;
		default:
			break;
		}
		return fail(L"PathSegment 动画路径所有者或末端属性与实际段类型不匹配。");
	}

	static bool TryResolveGeometryTransformPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		GeometryTransformAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const DependencyPropertyMetadata* metadata = nullptr;
		const cui::drawing::Geometry* resolvedGeometry = nullptr;
		std::vector<size_t> childIndices;
		size_t leafStart = 0;
		std::wstring canonicalPrefix;
		if (!TryResolveGeometryTreeTarget(target, path, metadata,
			resolvedGeometry, childIndices, leafStart, canonicalPrefix, outError))
			return false;
		const auto geometryOwner = LocalTypeName(
			path.Segments[leafStart].OwnerType);
		const bool direct = path.Segments.size() == leafStart + 2
			&& path.Segments[leafStart + 1].Kind
				== cui::xaml::PropertyPathSegmentKind::Property;
		const bool grouped = path.Segments.size() == leafStart + 4
			&& path.Segments[leafStart + 1].Kind
				== cui::xaml::PropertyPathSegmentKind::Property
			&& path.Segments[leafStart + 2].Kind
				== cui::xaml::PropertyPathSegmentKind::Index
			&& path.Segments[leafStart + 3].Kind
				== cui::xaml::PropertyPathSegmentKind::Property
			&& EqualName(
				LocalTypeName(path.Segments[leafStart + 1].OwnerType),
				L"TransformGroup")
			&& EqualName(path.Segments[leafStart + 1].Name, L"Children");
		if ((!direct && !grouped)
			|| path.Segments[leafStart].Kind
				!= cui::xaml::PropertyPathSegmentKind::Property
			|| (!EqualName(geometryOwner, L"Geometry")
				&& !EqualName(geometryOwner, L"RectangleGeometry")
				&& !EqualName(geometryOwner, L"EllipseGeometry")
				&& !EqualName(geometryOwner, L"PathGeometry")
				&& !EqualName(geometryOwner, L"GeometryGroup"))
			|| !EqualName(path.Segments[leafStart].Name, L"Transform"))
			return fail(L"Geometry Transform 动画路径必须是 "
				L"(Control.Clip)...(Geometry.Transform).(TransformType.Property) 或 "
				L"(Control.Clip)...(Geometry.Transform)."
				L"(TransformGroup.Children)[n].(TransformType.Property)。");
		auto ownerMatches = [&]()
			{
				if (EqualName(geometryOwner, L"Geometry")) return true;
				switch (resolvedGeometry->Kind)
				{
				case cui::drawing::GeometryKind::Rectangle:
					return EqualName(geometryOwner, L"RectangleGeometry");
				case cui::drawing::GeometryKind::Ellipse:
					return EqualName(geometryOwner, L"EllipseGeometry");
				case cui::drawing::GeometryKind::Path:
					return EqualName(geometryOwner, L"PathGeometry");
				case cui::drawing::GeometryKind::Group:
					return EqualName(geometryOwner, L"GeometryGroup");
				default:
					return false;
				}
			};
		if (!ownerMatches())
			return fail(L"Geometry.Transform 路径所有者与实际 Geometry 类型不匹配。");
		if (!resolvedGeometry->LocalTransform)
			return fail(L"动画目标没有路径所需的 Geometry.Transform。");
		if (direct && resolvedGeometry->LocalTransform->Operations.size() != 1)
			return fail(L"直接 Geometry.Transform 动画路径要求单一 Transform 操作。");

		TransformAccessor transformAccessor;
		if (!TryResolveTransformOperationAccessor(
			*resolvedGeometry->LocalTransform,
			direct ? 0 : path.Segments[leafStart + 2].Index,
			LocalTypeName(path.Segments[direct
				? leafStart + 1 : leafStart + 3].OwnerType),
			path.Segments[direct ? leafStart + 1 : leafStart + 3].Name,
			canonicalPrefix + L".(Geometry.Transform)"
				+ (direct ? std::wstring{}
					: L".(TransformGroup.Children)"),
			transformAccessor, outError, direct)) return false;
		output.ChildIndices = std::move(childIndices);
		output.GeometryKind = resolvedGeometry->Kind;
		output.Transform = std::move(transformAccessor);
		output.Identity = output.Transform.Identity;
		outMetadata = metadata;
		if (outError) outError->clear();
		return true;
	}

	static bool TryResolveBrushPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		BrushAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const auto brushOwner = path.Segments.size() > 1
			? LocalTypeName(path.Segments[1].OwnerType) : std::wstring_view{};
		if (path.Segments.size() < 2
			|| path.Segments[0].Kind != cui::xaml::PropertyPathSegmentKind::Property
			|| path.Segments[1].Kind != cui::xaml::PropertyPathSegmentKind::Property
			|| !IsKnownStoryboardBrushRootOwner(
				path.Segments[0].OwnerType, path.Segments[0].Name))
			return fail(L"Brush 复合动画路径必须以 "
				L"(Control.BrushProperty).(BrushProperty) 开始。");
		const auto& rootProperty = path.Segments[0].Name;
		const auto* metadata = FindKnownStoryboardRootMetadata(
			target, rootProperty);
		BindingValue currentValue;
		cui::drawing::Brush currentBrushValue;
		if (!metadata || !metadata->CanWrite()
			|| metadata->ValueType()
			!= std::type_index(typeid(cui::drawing::Brush))
			|| !metadata->TryGet(target, currentValue)
			|| !currentValue.TryGet(currentBrushValue))
			return fail(L"动画目标必须持有 Control." + rootProperty + L" 画刷。");
		if (currentBrushValue.Kind == cui::drawing::BrushKind::None
			&& EqualName(rootProperty, L"Foreground"))
		{
			// Foreground has a WPF-style effective theme brush even when no
			// local/inherited Brush object is stored. Resolve the object path
			// against that effective value without manufacturing a Local or
			// Inherited dependency-property source.
			currentBrushValue = cui::drawing::MakeSolidColorBrush(
				target.RendererForegroundColor);
		}
		const auto* currentBrush = &currentBrushValue;

		auto ownerMatches = [&](std::wstring_view owner)
			{
				if (EqualName(owner, L"Brush")) return true;
				switch (currentBrush->Kind)
				{
				case cui::drawing::BrushKind::Solid:
					return EqualName(owner, L"SolidColorBrush");
				case cui::drawing::BrushKind::LinearGradient:
					return EqualName(owner, L"GradientBrush")
						|| EqualName(owner, L"LinearGradientBrush");
				case cui::drawing::BrushKind::RadialGradient:
					return EqualName(owner, L"GradientBrush")
						|| EqualName(owner, L"RadialGradientBrush");
				case cui::drawing::BrushKind::Image:
					return EqualName(owner, L"ImageBrush");
				default:
					return false;
				}
			};
		if (!ownerMatches(brushOwner))
			return fail(L"Brush 动画路径所有者与实际画刷类型不匹配。");

		output = {};
		output.BrushKind = currentBrush->Kind;
		if (path.Segments.size() == 2)
		{
			const auto& property = path.Segments[1].Name;
			auto assign = [&](BrushMember member, std::wstring_view owner,
				std::wstring_view canonicalProperty)
				{
					output.Member = member;
					output.Identity = MakeObjectPathIdentity(
						L"(Control." + rootProperty + L").("
						+ std::wstring(owner) + L"."
						+ std::wstring(canonicalProperty) + L")");
					outMetadata = metadata;
					if (outError) outError->clear();
					return true;
				};
			if (EqualName(property, L"Opacity"))
			{
				if (!std::isfinite(currentBrush->Opacity)
					|| currentBrush->Opacity < 0.0f || currentBrush->Opacity > 1.0f)
					return fail(L"动画目标的 Brush.Opacity 无效。");
				return assign(BrushMember::Opacity, L"Brush", L"Opacity");
			}
			if (currentBrush->Kind == cui::drawing::BrushKind::Solid
				&& EqualName(brushOwner, L"SolidColorBrush")
				&& EqualName(property, L"Color"))
			{
				const auto& color = currentBrush->Color;
				if (!std::isfinite(color.r) || !std::isfinite(color.g)
					|| !std::isfinite(color.b) || !std::isfinite(color.a))
					return fail(L"动画目标的 SolidColorBrush.Color 无效。");
				return assign(BrushMember::SolidColor,
					L"SolidColorBrush", L"Color");
			}
			auto finitePoint = [](D2D1_POINT_2F point)
				{
					return std::isfinite(point.x) && std::isfinite(point.y);
				};
			if (currentBrush->Kind == cui::drawing::BrushKind::LinearGradient
				&& EqualName(brushOwner, L"LinearGradientBrush"))
			{
				if (EqualName(property, L"StartPoint"))
				{
					if (!finitePoint(currentBrush->StartPoint))
						return fail(L"动画目标的 LinearGradientBrush.StartPoint 无效。");
					return assign(BrushMember::StartPoint,
						L"LinearGradientBrush", L"StartPoint");
				}
				if (EqualName(property, L"EndPoint"))
				{
					if (!finitePoint(currentBrush->EndPoint))
						return fail(L"动画目标的 LinearGradientBrush.EndPoint 无效。");
					return assign(BrushMember::EndPoint,
						L"LinearGradientBrush", L"EndPoint");
				}
			}
			if (currentBrush->Kind == cui::drawing::BrushKind::RadialGradient
				&& EqualName(brushOwner, L"RadialGradientBrush"))
			{
				if (EqualName(property, L"Center"))
				{
					if (!finitePoint(currentBrush->Center))
						return fail(L"动画目标的 RadialGradientBrush.Center 无效。");
					return assign(BrushMember::Center,
						L"RadialGradientBrush", L"Center");
				}
				if (EqualName(property, L"GradientOrigin"))
				{
					if (!finitePoint(currentBrush->GradientOrigin))
						return fail(L"动画目标的 RadialGradientBrush.GradientOrigin 无效。");
					return assign(BrushMember::GradientOrigin,
						L"RadialGradientBrush", L"GradientOrigin");
				}
				if (EqualName(property, L"RadiusX")
					|| EqualName(property, L"RadiusY"))
				{
					const bool x = EqualName(property, L"RadiusX");
					const auto radius = x ? currentBrush->RadiusX : currentBrush->RadiusY;
					if (!std::isfinite(radius) || radius < 0.0f)
						return fail(L"动画目标的 RadialGradientBrush 半径无效。");
					return assign(x ? BrushMember::RadiusX : BrushMember::RadiusY,
						L"RadialGradientBrush", x ? L"RadiusX" : L"RadiusY");
				}
			}
			return fail(L"Brush 动画路径末端属性与实际画刷类型不匹配。");
		}

		if (path.Segments.size() != 4
			|| path.Segments[2].Kind != cui::xaml::PropertyPathSegmentKind::Index
			|| path.Segments[3].Kind != cui::xaml::PropertyPathSegmentKind::Property
			|| (!EqualName(brushOwner, L"GradientBrush")
				&& !EqualName(brushOwner, L"LinearGradientBrush")
				&& !EqualName(brushOwner, L"RadialGradientBrush"))
			|| !EqualName(path.Segments[1].Name, L"GradientStops")
			|| !EqualName(LocalTypeName(path.Segments[3].OwnerType), L"GradientStop")
			|| (!EqualName(path.Segments[3].Name, L"Color")
				&& !EqualName(path.Segments[3].Name, L"Offset"))
			|| (currentBrush->Kind != cui::drawing::BrushKind::LinearGradient
				&& currentBrush->Kind != cui::drawing::BrushKind::RadialGradient)
			|| path.Segments[2].Index >= currentBrush->GradientStops.size())
			return fail(L"GradientStop 复合动画路径必须是 "
				L"(Control.BrushProperty).(GradientBrush.GradientStops)[n]."
				L"(GradientStop.Color|Offset)。");
		const auto& stop = currentBrush->GradientStops[path.Segments[2].Index];
		if (!std::isfinite(stop.Offset) || stop.Offset < 0.0f || stop.Offset > 1.0f
			|| !std::isfinite(stop.Color.r) || !std::isfinite(stop.Color.g)
			|| !std::isfinite(stop.Color.b) || !std::isfinite(stop.Color.a))
			return fail(L"动画目标的 GradientStop 值无效。");

		output.StopIndex = path.Segments[2].Index;
		output.Member = EqualName(path.Segments[3].Name, L"Color")
			? BrushMember::GradientStopColor : BrushMember::GradientStopOffset;
		output.Identity = MakeObjectPathIdentity(L"(Control." + rootProperty + L")."
			L"(GradientBrush.GradientStops)["
			+ std::to_wstring(output.StopIndex) + L"].(GradientStop."
			+ (output.Member == BrushMember::GradientStopColor
				? std::wstring(L"Color") : std::wstring(L"Offset")) + L")");
		outMetadata = metadata;
		if (outError) outError->clear();
		return true;
	}

	static bool TryResolveBrushTransformPath(
		Control& target,
		const std::wstring& text,
		const DependencyPropertyMetadata*& outMetadata,
		BrushTransformAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError))
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const auto brushOwner = path.Segments.size() > 1
			? LocalTypeName(path.Segments[1].OwnerType) : std::wstring_view{};
		const bool direct = path.Segments.size() == 3
			&& path.Segments[2].Kind
				== cui::xaml::PropertyPathSegmentKind::Property;
		const bool grouped = path.Segments.size() == 5
			&& path.Segments[2].Kind
				== cui::xaml::PropertyPathSegmentKind::Property
			&& path.Segments[3].Kind
				== cui::xaml::PropertyPathSegmentKind::Index
			&& path.Segments[4].Kind
				== cui::xaml::PropertyPathSegmentKind::Property
			&& EqualName(LocalTypeName(path.Segments[2].OwnerType), L"TransformGroup")
			&& EqualName(path.Segments[2].Name, L"Children");
		if ((!direct && !grouped)
			|| path.Segments[0].Kind != cui::xaml::PropertyPathSegmentKind::Property
			|| path.Segments[1].Kind != cui::xaml::PropertyPathSegmentKind::Property
			|| !IsKnownStoryboardBrushRootOwner(
				path.Segments[0].OwnerType, path.Segments[0].Name)
			|| (!EqualName(brushOwner, L"Brush")
				&& !EqualName(brushOwner, L"SolidColorBrush")
				&& !EqualName(brushOwner, L"GradientBrush")
				&& !EqualName(brushOwner, L"LinearGradientBrush")
				&& !EqualName(brushOwner, L"RadialGradientBrush")
				&& !EqualName(brushOwner, L"ImageBrush"))
			|| (!EqualName(path.Segments[1].Name, L"Transform")
				&& !EqualName(path.Segments[1].Name, L"RelativeTransform")))
			return fail(L"Brush Transform 动画路径必须是 "
				L"(Control.BrushProperty).(Brush.Transform|RelativeTransform)."
				L"(TransformType.Property) 或 ..."
				L"(TransformGroup.Children)[n].(TransformType.Property)。");

		const auto& rootProperty = path.Segments[0].Name;
		const auto* metadata = FindKnownStoryboardRootMetadata(
			target, rootProperty);
		BindingValue currentValue;
		cui::drawing::Brush currentBrushValue;
		if (!metadata || !metadata->CanWrite()
			|| metadata->ValueType() != std::type_index(typeid(cui::drawing::Brush))
			|| !metadata->TryGet(target, currentValue)
			|| !currentValue.TryGet(currentBrushValue))
			return fail(L"动画目标必须持有包含变换的 Control."
				+ rootProperty + L"。");
		const auto* currentBrush = &currentBrushValue;
		auto ownerMatches = [&]()
			{
				if (EqualName(brushOwner, L"Brush")) return true;
				switch (currentBrush->Kind)
				{
				case cui::drawing::BrushKind::Solid:
					return EqualName(brushOwner, L"SolidColorBrush");
				case cui::drawing::BrushKind::LinearGradient:
					return EqualName(brushOwner, L"GradientBrush")
						|| EqualName(brushOwner, L"LinearGradientBrush");
				case cui::drawing::BrushKind::RadialGradient:
					return EqualName(brushOwner, L"GradientBrush")
						|| EqualName(brushOwner, L"RadialGradientBrush");
				case cui::drawing::BrushKind::Image:
					return EqualName(brushOwner, L"ImageBrush");
				default:
					return false;
				}
			};
		if (!ownerMatches())
			return fail(L"Brush Transform 路径所有者与实际画刷类型不匹配。");
		const bool relative = EqualName(
			path.Segments[1].Name, L"RelativeTransform");
		const auto& transform = relative
			? currentBrush->RelativeTransform : currentBrush->Transform;
		if (!transform)
			return fail(L"动画目标没有路径所需的 Brush Transform。");
		if (direct && transform->Operations.size() != 1)
			return fail(L"直接 Brush Transform 动画路径要求单一 Transform 操作。");

		TransformAccessor transformAccessor;
		const auto canonicalPrefix = L"(Control." + rootProperty + L").(Brush."
			+ std::wstring(relative ? L"RelativeTransform" : L"Transform")
			+ L")" + (direct ? std::wstring{}
				: L".(TransformGroup.Children)");
		if (!TryResolveTransformOperationAccessor(
			*transform, direct ? 0 : path.Segments[3].Index,
			LocalTypeName(path.Segments[direct ? 2 : 4].OwnerType),
			path.Segments[direct ? 2 : 4].Name,
			canonicalPrefix, transformAccessor, outError, direct)) return false;
		output.BrushKind = currentBrush->Kind;
		output.Relative = relative;
		output.Transform = std::move(transformAccessor);
		output.Identity = output.Transform.Identity;
		outMetadata = metadata;
		if (outError) outError->clear();
		return true;
	}

	static bool TryResolveObjectPath(
		Control& target,
		const std::wstring& text,
		DeclarativeAnimationKind animationKind,
		const DependencyPropertyMetadata*& outMetadata,
		ObjectPathAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		cui::xaml::PropertyPath path;
		std::wstring parseError;
		if (!cui::xaml::TryParsePropertyPath(text, path, &parseError)
			|| path.Segments.empty()
			|| path.Segments.front().Kind
			!= cui::xaml::PropertyPathSegmentKind::Property)
			return fail(L"Storyboard.TargetProperty 路径无效：" + parseError);
		const auto& root = path.Segments.front().Name;
		if (EqualName(root, L"RenderTransform"))
		{
			TransformAccessor accessor;
			if (!TryResolveTransformPath(
				target, text, outMetadata, accessor, outError)) return false;
			const auto expected = accessor.Member == TransformMember::Matrix
				? DeclarativeAnimationKind::Matrix
				: DeclarativeAnimationKind::Double;
			if (animationKind != expected)
				return fail(L"RenderTransform 数值末端需要 DoubleAnimation，"
					L"MatrixTransform.Matrix 末端需要 MatrixAnimation。");
			output = std::move(accessor);
			return true;
		}
		if (EqualName(root, L"Clip"))
		{
			size_t geometryLeafStart = 1;
			while (geometryLeafStart + 1 < path.Segments.size()
				&& path.Segments[geometryLeafStart].Kind
				== cui::xaml::PropertyPathSegmentKind::Property
				&& EqualName(LocalTypeName(
					path.Segments[geometryLeafStart].OwnerType), L"GeometryGroup")
				&& EqualName(path.Segments[geometryLeafStart].Name, L"Children")
				&& path.Segments[geometryLeafStart + 1].Kind
				== cui::xaml::PropertyPathSegmentKind::Index)
				geometryLeafStart += 2;
			if (geometryLeafStart < path.Segments.size()
				&& EqualName(path.Segments[geometryLeafStart].Name, L"Figures"))
			{
				PathGeometryAccessor accessor;
				if (!TryResolvePathGeometryPath(
					target, text, outMetadata, accessor, outError)) return false;
				const bool pointMember = accessor.Member
					== PathGeometryMember::FigureStartPoint
					|| accessor.Member == PathGeometryMember::SegmentPoint
					|| accessor.Member == PathGeometryMember::SegmentPoint1
					|| accessor.Member == PathGeometryMember::SegmentPoint2
					|| accessor.Member == PathGeometryMember::SegmentPoint3;
				const bool sizeMember = accessor.Member
					== PathGeometryMember::ArcSize;
				const bool doubleMember = accessor.Member
					== PathGeometryMember::ArcRotationAngle;
				const bool objectMember = accessor.Member
					== PathGeometryMember::FigureIsClosed
					|| accessor.Member == PathGeometryMember::FigureIsFilled
					|| accessor.Member == PathGeometryMember::ArcIsLargeArc
					|| accessor.Member == PathGeometryMember::ArcSweepDirection;
				if ((pointMember && animationKind != DeclarativeAnimationKind::Point)
					|| (sizeMember && animationKind != DeclarativeAnimationKind::Size)
					|| (doubleMember && animationKind != DeclarativeAnimationKind::Double)
					|| (objectMember && animationKind != DeclarativeAnimationKind::Object))
					return fail(L"Path Point 末端需要 PointAnimation，Size 末端需要 "
						L"SizeAnimation，角度末端需要 DoubleAnimation，布尔/枚举末端"
						L"需要 ObjectAnimationUsingKeyFrames。");
				output = std::move(accessor);
				return true;
			}
			if (geometryLeafStart < path.Segments.size()
				&& EqualName(path.Segments[geometryLeafStart].Name, L"Transform"))
			{
				GeometryTransformAccessor accessor;
				if (!TryResolveGeometryTransformPath(
					target, text, outMetadata, accessor, outError)) return false;
				const auto expected = accessor.Transform.Member
					== TransformMember::Matrix
					? DeclarativeAnimationKind::Matrix
					: DeclarativeAnimationKind::Double;
				if (animationKind != expected)
					return fail(L"Geometry.Transform 数值末端需要 DoubleAnimation，"
						L"MatrixTransform.Matrix 末端需要 MatrixAnimation。");
				output = std::move(accessor);
				return true;
			}
			GeometryAccessor accessor;
			if (!TryResolveGeometryPath(
				target, text, outMetadata, accessor, outError)) return false;
			const bool rectMember = accessor.Member == GeometryMember::Rect;
			const bool pointMember = accessor.Member == GeometryMember::Center;
			const bool objectMember = accessor.Member == GeometryMember::FillRule;
			if ((rectMember && animationKind != DeclarativeAnimationKind::Rect)
				|| (pointMember && animationKind != DeclarativeAnimationKind::Point)
				|| (objectMember && animationKind != DeclarativeAnimationKind::Object)
				|| (!rectMember && !pointMember && !objectMember
					&& animationKind != DeclarativeAnimationKind::Double))
				return fail(L"Geometry Rect 末端需要 RectAnimation，Center 末端需要 "
					L"PointAnimation，半径末端需要 DoubleAnimation，FillRule 需要 "
					L"ObjectAnimationUsingKeyFrames。");
			output = std::move(accessor);
			return true;
		}
		const auto* rootMetadata = FindKnownStoryboardRootMetadata(target, root);
		if (path.Segments.size() > 1 && rootMetadata
			&& rootMetadata->ValueType()
			== std::type_index(typeid(cui::drawing::Brush)))
		{
			if (path.Segments.size() > 1
				&& (EqualName(path.Segments[1].Name, L"Transform")
					|| EqualName(path.Segments[1].Name, L"RelativeTransform")))
			{
				BrushTransformAccessor accessor;
				if (!TryResolveBrushTransformPath(
					target, text, outMetadata, accessor, outError)) return false;
				const auto expected = accessor.Transform.Member
					== TransformMember::Matrix
					? DeclarativeAnimationKind::Matrix
					: DeclarativeAnimationKind::Double;
				if (animationKind != expected)
					return fail(L"Brush Transform 数值末端需要 DoubleAnimation，"
						L"MatrixTransform.Matrix 末端需要 MatrixAnimation。");
				output = std::move(accessor);
				return true;
			}
			BrushAccessor accessor;
			if (!TryResolveBrushPath(
				target, text, outMetadata, accessor, outError)) return false;
			const bool colorMember = accessor.Member == BrushMember::SolidColor
				|| accessor.Member == BrushMember::GradientStopColor;
			const bool pointMember = accessor.Member == BrushMember::StartPoint
				|| accessor.Member == BrushMember::EndPoint
				|| accessor.Member == BrushMember::Center
				|| accessor.Member == BrushMember::GradientOrigin;
			if ((colorMember && animationKind != DeclarativeAnimationKind::Color)
				|| (pointMember && animationKind != DeclarativeAnimationKind::Point)
				|| (!colorMember && !pointMember
					&& animationKind != DeclarativeAnimationKind::Double))
				return fail(L"Brush Color 末端需要 ColorAnimation，Point 末端需要 "
					L"PointAnimation，其余数值末端需要 DoubleAnimation。");
			output = std::move(accessor);
			return true;
		}
		return fail(L"尚未注册可处理此 Storyboard.TargetProperty 的对象路径适配器。");
	}

	/**
	 * Binds an AOT-lowered, string-free object path to one control instance.
	 * The program carries only stable enum/index data; this boundary validates
	 * that the instance still has the object shape compiled from XAML.
	 */
	static bool TryResolveCompiledObjectPath(
		Control& target,
		const CompiledStoryboardObjectPathOp& source,
		std::span<const uint32_t> childIndexPool,
		const DependencyPropertyMetadata& rootMetadata,
		DeclarativeAnimationKind animationKind,
		ObjectPathAccessor& output,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		if (!rootMetadata.CanWrite())
			return fail(L"编译 Storyboard 对象路径的根属性只读。");
		if (source.Identity == 0 || source.Reserved != 0)
			return fail(L"编译 Storyboard 对象路径描述符无效。");
		constexpr auto knownFlags =
			static_cast<uint8_t>(CompiledStoryboardObjectPathFlags::RelativeTransform)
			| static_cast<uint8_t>(
				CompiledStoryboardObjectPathFlags::HasPathSegment)
			| static_cast<uint8_t>(
				CompiledStoryboardObjectPathFlags::DirectTransform);
		if ((static_cast<uint8_t>(source.Flags) & ~knownFlags) != 0)
			return fail(L"编译 Storyboard 对象路径包含未知标志。");
		const auto childOffset = static_cast<size_t>(source.ChildIndices.Offset);
		const auto childCount = static_cast<size_t>(source.ChildIndices.Count);
		if (childOffset > childIndexPool.size()
			|| childCount > childIndexPool.size() - childOffset)
			return fail(L"编译 Storyboard Geometry 子索引范围越界。");
		std::vector<size_t> childIndices;
		childIndices.reserve(childCount);
		for (size_t index = 0; index < childCount; ++index)
			childIndices.push_back(static_cast<size_t>(
				childIndexPool[childOffset + index]));

		auto transformMember = [&]() -> std::optional<TransformMember>
			{
				switch (source.Member)
				{
				case CompiledStoryboardObjectPathMember::TransformX:
					return TransformMember::X;
				case CompiledStoryboardObjectPathMember::TransformY:
					return TransformMember::Y;
				case CompiledStoryboardObjectPathMember::TransformScaleX:
					return TransformMember::ScaleX;
				case CompiledStoryboardObjectPathMember::TransformScaleY:
					return TransformMember::ScaleY;
				case CompiledStoryboardObjectPathMember::TransformAngle:
					return TransformMember::Angle;
				case CompiledStoryboardObjectPathMember::TransformAngleX:
					return TransformMember::AngleX;
				case CompiledStoryboardObjectPathMember::TransformAngleY:
					return TransformMember::AngleY;
				case CompiledStoryboardObjectPathMember::TransformCenterX:
					return TransformMember::CenterX;
				case CompiledStoryboardObjectPathMember::TransformCenterY:
					return TransformMember::CenterY;
				case CompiledStoryboardObjectPathMember::TransformMatrix:
					return TransformMember::Matrix;
				default:
					return std::nullopt;
				}
			};
		auto geometryMember = [&]() -> std::optional<GeometryMember>
			{
				switch (source.Member)
				{
				case CompiledStoryboardObjectPathMember::GeometryRect:
					return GeometryMember::Rect;
				case CompiledStoryboardObjectPathMember::GeometryCenter:
					return GeometryMember::Center;
				case CompiledStoryboardObjectPathMember::GeometryRadiusX:
					return GeometryMember::RadiusX;
				case CompiledStoryboardObjectPathMember::GeometryRadiusY:
					return GeometryMember::RadiusY;
				case CompiledStoryboardObjectPathMember::GeometryFillRule:
					return GeometryMember::FillRule;
				default:
					return std::nullopt;
				}
			};
		auto pathMember = [&]() -> std::optional<PathGeometryMember>
			{
				switch (source.Member)
				{
				case CompiledStoryboardObjectPathMember::PathFigureStartPoint:
					return PathGeometryMember::FigureStartPoint;
				case CompiledStoryboardObjectPathMember::PathFigureIsClosed:
					return PathGeometryMember::FigureIsClosed;
				case CompiledStoryboardObjectPathMember::PathFigureIsFilled:
					return PathGeometryMember::FigureIsFilled;
				case CompiledStoryboardObjectPathMember::PathSegmentPoint:
					return PathGeometryMember::SegmentPoint;
				case CompiledStoryboardObjectPathMember::PathSegmentPoint1:
					return PathGeometryMember::SegmentPoint1;
				case CompiledStoryboardObjectPathMember::PathSegmentPoint2:
					return PathGeometryMember::SegmentPoint2;
				case CompiledStoryboardObjectPathMember::PathSegmentPoint3:
					return PathGeometryMember::SegmentPoint3;
				case CompiledStoryboardObjectPathMember::PathArcSize:
					return PathGeometryMember::ArcSize;
				case CompiledStoryboardObjectPathMember::PathArcRotationAngle:
					return PathGeometryMember::ArcRotationAngle;
				case CompiledStoryboardObjectPathMember::PathArcIsLargeArc:
					return PathGeometryMember::ArcIsLargeArc;
				case CompiledStoryboardObjectPathMember::PathArcSweepDirection:
					return PathGeometryMember::ArcSweepDirection;
				default:
					return std::nullopt;
				}
			};
		auto brushMember = [&]() -> std::optional<BrushMember>
			{
				switch (source.Member)
				{
				case CompiledStoryboardObjectPathMember::BrushSolidColor:
					return BrushMember::SolidColor;
				case CompiledStoryboardObjectPathMember::BrushOpacity:
					return BrushMember::Opacity;
				case CompiledStoryboardObjectPathMember::BrushStartPoint:
					return BrushMember::StartPoint;
				case CompiledStoryboardObjectPathMember::BrushEndPoint:
					return BrushMember::EndPoint;
				case CompiledStoryboardObjectPathMember::BrushCenter:
					return BrushMember::Center;
				case CompiledStoryboardObjectPathMember::BrushGradientOrigin:
					return BrushMember::GradientOrigin;
				case CompiledStoryboardObjectPathMember::BrushRadiusX:
					return BrushMember::RadiusX;
				case CompiledStoryboardObjectPathMember::BrushRadiusY:
					return BrushMember::RadiusY;
				case CompiledStoryboardObjectPathMember::BrushGradientStopColor:
					return BrushMember::GradientStopColor;
				case CompiledStoryboardObjectPathMember::BrushGradientStopOffset:
					return BrushMember::GradientStopOffset;
				default:
					return std::nullopt;
				}
			};
		auto requireAnimation = [&](DeclarativeAnimationKind expected,
			std::wstring_view leafDescription)
			{
				return animationKind == expected
					? true : fail(L"编译 Storyboard 对象路径的动画类型与 "
						+ std::wstring(leafDescription) + L" 不匹配。");
			};

		const bool hasPathSegment = HasCompiledStoryboardObjectPathFlag(
			source.Flags, CompiledStoryboardObjectPathFlags::HasPathSegment);
		const bool relativeTransform = HasCompiledStoryboardObjectPathFlag(
			source.Flags, CompiledStoryboardObjectPathFlags::RelativeTransform);
		const bool directTransform = HasCompiledStoryboardObjectPathFlag(
			source.Flags, CompiledStoryboardObjectPathFlags::DirectTransform);
		if (&rootMetadata.Property() == &Control::ClipProperty()
			&& !target.GetClip())
			return fail(L"编译 Storyboard Geometry 对象路径要求目标显式持有 Clip。");
		BindingValue root;
		if (!rootMetadata.TryGet(target, root))
			return fail(L"无法读取编译 Storyboard 对象路径的根属性。");
		if (source.Kind == CompiledStoryboardObjectPathKind::Brush
			|| source.Kind == CompiledStoryboardObjectPathKind::BrushTransform)
		{
			cui::drawing::Brush brush;
			if (!root.TryGet(brush))
				return fail(L"编译 Storyboard 对象路径的根值不是 Brush。");
			if (brush.Kind == cui::drawing::BrushKind::None
				&& &rootMetadata.Property() == &Control::ForegroundProperty())
			{
				brush = cui::drawing::MakeSolidColorBrush(
					target.RendererForegroundColor);
				root = BindingValue(std::move(brush));
			}
		}

		constexpr uint8_t anyObjectKind =
			(std::numeric_limits<uint8_t>::max)();
		auto resolveTransformKind = [&](uint8_t encoded,
			cui::drawing::TransformKind actual,
			cui::drawing::TransformKind& resolvedKind)
			{
				if (encoded == anyObjectKind)
				{
					resolvedKind = actual;
					return true;
				}
				if (encoded > static_cast<uint8_t>(
					cui::drawing::TransformKind::Skew)) return false;
				resolvedKind = static_cast<cui::drawing::TransformKind>(encoded);
				return resolvedKind == actual;
			};
		auto resolveGeometryKind = [&](uint8_t encoded,
			cui::drawing::GeometryKind actual,
			cui::drawing::GeometryKind& resolvedKind)
			{
				if (encoded == anyObjectKind)
				{
					resolvedKind = actual;
					return true;
				}
				if (encoded > static_cast<uint8_t>(
					cui::drawing::GeometryKind::Group)) return false;
				resolvedKind = static_cast<cui::drawing::GeometryKind>(encoded);
				return resolvedKind == actual;
			};
		auto resolveBrushKind = [&](uint8_t encoded,
			cui::drawing::BrushKind actual,
			cui::drawing::BrushKind& resolvedKind)
			{
				if (encoded == anyObjectKind)
				{
					resolvedKind = actual;
					return true;
				}
				if (encoded > static_cast<uint8_t>(
					cui::drawing::BrushKind::Image)) return false;
				resolvedKind = static_cast<cui::drawing::BrushKind>(encoded);
				return resolvedKind == actual;
			};
		auto resolvePathSegmentKind = [&](uint8_t encoded,
			cui::drawing::PathSegmentKind actual,
			cui::drawing::PathSegmentKind& resolvedKind)
			{
				if (encoded == anyObjectKind)
				{
					resolvedKind = actual;
					return true;
				}
				if (encoded > static_cast<uint8_t>(
					cui::drawing::PathSegmentKind::Arc)) return false;
				resolvedKind = static_cast<cui::drawing::PathSegmentKind>(encoded);
				return resolvedKind == actual;
			};
		ObjectPathAccessor resolved;
		switch (source.Kind)
		{
		case CompiledStoryboardObjectPathKind::Transform:
		{
			if (childCount != 0 || hasPathSegment || relativeTransform
				|| source.ExpectedAuxiliaryKind != 0
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Transform)))
				return fail(L"编译 RenderTransform 对象路径描述符无效。");
			const auto member = transformMember();
			if (!member || !requireAnimation(
				*member == TransformMember::Matrix
					? DeclarativeAnimationKind::Matrix
					: DeclarativeAnimationKind::Double,
				L"Transform 末端")) return false;
			cui::drawing::Transform transform;
			const auto operationIndex = static_cast<size_t>(source.Index0);
			cui::drawing::TransformKind operationKind{};
			if (!root.TryGet(transform)
				|| (directTransform
					&& (operationIndex != 0 || transform.Operations.size() != 1))
				|| operationIndex >= transform.Operations.size()
				|| !resolveTransformKind(source.ExpectedObjectKind,
					transform.Operations[operationIndex].Kind, operationKind))
				return fail(L"编译 Transform kind 与目标实例不匹配。");
			TransformAccessor accessor;
			accessor.OperationIndex = operationIndex;
			accessor.OperationKind = operationKind;
			accessor.Member = *member;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		case CompiledStoryboardObjectPathKind::Geometry:
		{
			if (hasPathSegment || relativeTransform || directTransform
				|| source.ExpectedAuxiliaryKind != 0
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Geometry)))
				return fail(L"编译 Geometry 对象路径描述符无效。");
			const auto member = geometryMember();
			if (!member) return fail(L"编译 Geometry 末端成员无效。");
			const auto expected = *member == GeometryMember::Rect
				? DeclarativeAnimationKind::Rect
				: *member == GeometryMember::Center
					? DeclarativeAnimationKind::Point
					: *member == GeometryMember::FillRule
						? DeclarativeAnimationKind::Object
						: DeclarativeAnimationKind::Double;
			if (!requireAnimation(expected, L"Geometry 末端")) return false;
			cui::drawing::Geometry geometry;
			const cui::drawing::Geometry* leafGeometry = nullptr;
			cui::drawing::GeometryKind geometryKind{};
			if (!root.TryGet(geometry)
				|| !(leafGeometry = TryGetGeometryChild(geometry, childIndices))
				|| !resolveGeometryKind(source.ExpectedObjectKind,
					leafGeometry->Kind, geometryKind))
				return fail(L"编译 Geometry kind 与目标实例不匹配。");
			GeometryAccessor accessor;
			accessor.ChildIndices = childIndices;
			accessor.GeometryKind = geometryKind;
			accessor.Member = *member;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		case CompiledStoryboardObjectPathKind::PathGeometry:
		{
			if (relativeTransform || directTransform
				|| source.ExpectedAuxiliaryKind != 0
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Geometry)))
				return fail(L"编译 PathGeometry 对象路径描述符无效。");
			const auto member = pathMember();
			if (!member) return fail(L"编译 PathGeometry 末端成员无效。");
			const bool segmentMember = *member != PathGeometryMember::FigureStartPoint
				&& *member != PathGeometryMember::FigureIsClosed
				&& *member != PathGeometryMember::FigureIsFilled;
			if (segmentMember != hasPathSegment
				|| (!hasPathSegment && source.Index1 != 0))
				return fail(L"编译 PathGeometry segment 描述符不一致。");
			const bool pointMember = *member == PathGeometryMember::FigureStartPoint
				|| *member == PathGeometryMember::SegmentPoint
				|| *member == PathGeometryMember::SegmentPoint1
				|| *member == PathGeometryMember::SegmentPoint2
				|| *member == PathGeometryMember::SegmentPoint3;
			const auto expected = pointMember
				? DeclarativeAnimationKind::Point
				: *member == PathGeometryMember::ArcSize
					? DeclarativeAnimationKind::Size
					: *member == PathGeometryMember::ArcRotationAngle
						? DeclarativeAnimationKind::Double
						: DeclarativeAnimationKind::Object;
			if (!requireAnimation(expected, L"PathGeometry 末端")) return false;
			cui::drawing::Geometry geometry;
			const cui::drawing::Geometry* leafGeometry = nullptr;
			const auto figureIndex = static_cast<size_t>(source.Index0);
			const auto segmentIndex = static_cast<size_t>(source.Index1);
			if (!root.TryGet(geometry)
				|| !(leafGeometry = TryGetGeometryChild(geometry, childIndices))
				|| leafGeometry->Kind != cui::drawing::GeometryKind::Path
				|| figureIndex >= leafGeometry->Figures.size())
				return fail(L"编译 PathGeometry 与目标实例不匹配。");
			cui::drawing::PathSegmentKind segmentKind =
				cui::drawing::PathSegmentKind::Line;
			if (hasPathSegment)
			{
				const auto& segments = leafGeometry->Figures[figureIndex].Segments;
				if (segmentIndex >= segments.size()
					|| !resolvePathSegmentKind(source.ExpectedObjectKind,
						segments[segmentIndex].Kind, segmentKind))
					return fail(L"编译 PathSegment kind 与目标实例不匹配。");
			}
			else if (source.ExpectedObjectKind != 0
				&& source.ExpectedObjectKind != anyObjectKind)
				return fail(L"编译 PathFigure 描述符包含不适用的 segment kind。");
			PathGeometryAccessor accessor;
			accessor.ChildIndices = childIndices;
			accessor.FigureIndex = figureIndex;
			accessor.SegmentIndex = segmentIndex;
			accessor.HasSegment = hasPathSegment;
			accessor.SegmentKind = segmentKind;
			accessor.Member = *member;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		case CompiledStoryboardObjectPathKind::GeometryTransform:
		{
			if (hasPathSegment || relativeTransform
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Geometry)))
				return fail(L"编译 Geometry.Transform 对象路径描述符无效。");
			const auto member = transformMember();
			if (!member || !requireAnimation(
				*member == TransformMember::Matrix
					? DeclarativeAnimationKind::Matrix
					: DeclarativeAnimationKind::Double,
				L"Geometry.Transform 末端")) return false;
			cui::drawing::Geometry geometry;
			const cui::drawing::Geometry* leafGeometry = nullptr;
			const auto operationIndex = static_cast<size_t>(source.Index0);
			cui::drawing::GeometryKind geometryKind{};
			cui::drawing::TransformKind operationKind{};
			if (!root.TryGet(geometry)
				|| !(leafGeometry = TryGetGeometryChild(geometry, childIndices))
				|| !resolveGeometryKind(source.ExpectedObjectKind,
					leafGeometry->Kind, geometryKind)
				|| !leafGeometry->LocalTransform
				|| (directTransform && (operationIndex != 0
					|| leafGeometry->LocalTransform->Operations.size() != 1))
				|| operationIndex >= leafGeometry->LocalTransform->Operations.size()
				|| !resolveTransformKind(source.ExpectedAuxiliaryKind,
					leafGeometry->LocalTransform->Operations[operationIndex].Kind,
					operationKind))
				return fail(L"编译 Geometry.Transform kind 与目标实例不匹配。");
			GeometryTransformAccessor accessor;
			accessor.ChildIndices = childIndices;
			accessor.GeometryKind = geometryKind;
			accessor.Transform.OperationIndex = operationIndex;
			accessor.Transform.OperationKind = operationKind;
			accessor.Transform.Member = *member;
			accessor.Transform.Identity = source.Identity;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		case CompiledStoryboardObjectPathKind::Brush:
		{
			if (childCount != 0 || hasPathSegment || relativeTransform
				|| directTransform
				|| source.ExpectedAuxiliaryKind != 0
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Brush)))
				return fail(L"编译 Brush 对象路径描述符无效。");
			const auto member = brushMember();
			if (!member) return fail(L"编译 Brush 末端成员无效。");
			const bool colorMember = *member == BrushMember::SolidColor
				|| *member == BrushMember::GradientStopColor;
			const bool pointMember = *member == BrushMember::StartPoint
				|| *member == BrushMember::EndPoint
				|| *member == BrushMember::Center
				|| *member == BrushMember::GradientOrigin;
			const auto expected = colorMember
				? DeclarativeAnimationKind::Color
				: pointMember ? DeclarativeAnimationKind::Point
					: DeclarativeAnimationKind::Double;
			if (!requireAnimation(expected, L"Brush 末端")) return false;
			cui::drawing::Brush brush;
			cui::drawing::BrushKind brushKind{};
			if (!root.TryGet(brush)
				|| !resolveBrushKind(source.ExpectedObjectKind,
					brush.Kind, brushKind))
				return fail(L"编译 Brush kind 与目标实例不匹配。");
			BrushAccessor accessor;
			accessor.StopIndex = static_cast<size_t>(source.Index0);
			accessor.BrushKind = brushKind;
			accessor.Member = *member;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		case CompiledStoryboardObjectPathKind::BrushTransform:
		{
			if (childCount != 0 || hasPathSegment
				|| rootMetadata.ValueType()
				!= std::type_index(typeid(cui::drawing::Brush)))
				return fail(L"编译 Brush.Transform 对象路径描述符无效。");
			const auto member = transformMember();
			if (!member || !requireAnimation(
				*member == TransformMember::Matrix
					? DeclarativeAnimationKind::Matrix
					: DeclarativeAnimationKind::Double,
				L"Brush.Transform 末端")) return false;
			cui::drawing::Brush brush;
			cui::drawing::BrushKind brushKind{};
			const auto operationIndex = static_cast<size_t>(source.Index0);
			if (!root.TryGet(brush)
				|| !resolveBrushKind(source.ExpectedObjectKind,
					brush.Kind, brushKind))
				return fail(L"编译 Brush kind 与目标实例不匹配。");
			const auto& transform = relativeTransform
				? brush.RelativeTransform : brush.Transform;
			cui::drawing::TransformKind operationKind{};
			if (!transform
				|| (directTransform
					&& (operationIndex != 0 || transform->Operations.size() != 1))
				|| operationIndex >= transform->Operations.size()
				|| !resolveTransformKind(source.ExpectedAuxiliaryKind,
					transform->Operations[operationIndex].Kind, operationKind))
				return fail(L"编译 Brush.Transform kind 与目标实例不匹配。");
			BrushTransformAccessor accessor;
			accessor.BrushKind = brushKind;
			accessor.Relative = relativeTransform;
			accessor.Transform.OperationIndex = operationIndex;
			accessor.Transform.OperationKind = operationKind;
			accessor.Transform.Member = *member;
			accessor.Transform.Identity = source.Identity;
			accessor.Identity = source.Identity;
			resolved = std::move(accessor);
			break;
		}
		default:
			return fail(L"编译 Storyboard 对象路径 kind 无效。");
		}

		BindingValue leaf;
		if (!TryReadObjectPathMember(root, resolved, leaf))
			return fail(L"编译 Storyboard 对象路径与目标实例的对象图不匹配。");
		output = std::move(resolved);
		if (outError) outError->clear();
		return true;
	}

	static bool TryReadTransformMember(
		const cui::drawing::Transform& transform,
		const TransformAccessor& accessor,
		BindingValue& output) noexcept
	{
		if (accessor.OperationIndex >= transform.Operations.size()) return false;
		const auto& operation = transform.Operations[accessor.OperationIndex];
		if (operation.Kind != accessor.OperationKind) return false;
		float number = 0.0f;
		switch (accessor.Member)
		{
		case TransformMember::X: number = operation.X; break;
		case TransformMember::Y: number = operation.Y; break;
		case TransformMember::ScaleX: number = operation.ScaleX; break;
		case TransformMember::ScaleY: number = operation.ScaleY; break;
		case TransformMember::Angle: number = operation.Angle; break;
		case TransformMember::AngleX: number = operation.AngleX; break;
		case TransformMember::AngleY: number = operation.AngleY; break;
		case TransformMember::CenterX: number = operation.CenterX; break;
		case TransformMember::CenterY: number = operation.CenterY; break;
		case TransformMember::Matrix:
			if (!IsFiniteMatrix(operation.Matrix)) return false;
			output = BindingValue(operation.Matrix);
			return true;
		default: return false;
		}
		if (!std::isfinite(number)) return false;
		output = BindingValue(number);
		return true;
	}

	static bool TryWriteTransformMember(
		cui::drawing::Transform& transform,
		const TransformAccessor& accessor,
		const BindingValue& value) noexcept
	{
		if (accessor.OperationIndex >= transform.Operations.size()) return false;
		auto& operation = transform.Operations[accessor.OperationIndex];
		if (operation.Kind != accessor.OperationKind) return false;
		if (accessor.Member == TransformMember::Matrix)
		{
			D2D1_MATRIX_3X2_F matrix{};
			if (!value.TryGet(matrix) || !IsFiniteMatrix(matrix)) return false;
			operation.Matrix = matrix;
			return true;
		}
		double number = 0.0;
		if (!value.TryGetDouble(number) || !std::isfinite(number)
			|| number < -(std::numeric_limits<float>::max)()
			|| number >(std::numeric_limits<float>::max)()) return false;
		const auto result = static_cast<float>(number);
		switch (accessor.Member)
		{
		case TransformMember::X: operation.X = result; break;
		case TransformMember::Y: operation.Y = result; break;
		case TransformMember::ScaleX: operation.ScaleX = result; break;
		case TransformMember::ScaleY: operation.ScaleY = result; break;
		case TransformMember::Angle: operation.Angle = result; break;
		case TransformMember::AngleX: operation.AngleX = result; break;
		case TransformMember::AngleY: operation.AngleY = result; break;
		case TransformMember::CenterX: operation.CenterX = result; break;
		case TransformMember::CenterY: operation.CenterY = result; break;
		case TransformMember::Matrix: return false;
		default: return false;
		}
		return true;
	}

	static bool TryReadGeometryMember(
		const cui::drawing::Geometry& root,
		const GeometryAccessor& accessor,
		BindingValue& output) noexcept
	{
		const auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		const auto& geometry = *resolved;
		if (geometry.Kind != accessor.GeometryKind) return false;
		switch (accessor.Member)
		{
		case GeometryMember::Rect:
		{
			if (geometry.Kind != cui::drawing::GeometryKind::Rectangle) return false;
			const auto rect = ToCoreRect(geometry.Rect);
			if (!std::isfinite(rect.x) || !std::isfinite(rect.y)
				|| !std::isfinite(rect.width) || !std::isfinite(rect.height)
				|| rect.width < 0.0f || rect.height < 0.0f) return false;
			output = BindingValue(rect);
			return true;
		}
		case GeometryMember::Center:
			if (geometry.Kind != cui::drawing::GeometryKind::Ellipse
				|| !std::isfinite(geometry.Center.x)
				|| !std::isfinite(geometry.Center.y)) return false;
			output = BindingValue(cui::core::Point{
				geometry.Center.x, geometry.Center.y });
			return true;
		case GeometryMember::RadiusX:
		case GeometryMember::RadiusY:
		{
			if (geometry.Kind != cui::drawing::GeometryKind::Rectangle
				&& geometry.Kind != cui::drawing::GeometryKind::Ellipse) return false;
			const auto radius = accessor.Member == GeometryMember::RadiusX
				? geometry.RadiusX : geometry.RadiusY;
			if (!std::isfinite(radius) || radius < 0.0f) return false;
			output = BindingValue(radius);
			return true;
		}
		case GeometryMember::FillRule:
			if (geometry.Kind != cui::drawing::GeometryKind::Path
				&& geometry.Kind != cui::drawing::GeometryKind::Group) return false;
			output = BindingValue(std::wstring(geometry.FillRule
				== cui::drawing::GeometryFillRule::Nonzero
				? L"Nonzero" : L"EvenOdd"));
			return true;
		default:
			return false;
		}
	}

	static bool TryWriteGeometryMember(
		cui::drawing::Geometry& root,
		const GeometryAccessor& accessor,
		const BindingValue& value) noexcept
	{
		auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		auto& geometry = *resolved;
		if (geometry.Kind != accessor.GeometryKind) return false;
		switch (accessor.Member)
		{
		case GeometryMember::Rect:
		{
			cui::core::Rect rect{};
			if (geometry.Kind != cui::drawing::GeometryKind::Rectangle
				|| !value.TryGet(rect)
				|| !std::isfinite(rect.x) || !std::isfinite(rect.y)
				|| !std::isfinite(rect.width) || !std::isfinite(rect.height)
				|| rect.width < 0.0f || rect.height < 0.0f) return false;
			const auto converted = ToD2DRect(rect);
			if (!std::isfinite(converted.left) || !std::isfinite(converted.top)
				|| !std::isfinite(converted.right) || !std::isfinite(converted.bottom))
				return false;
			geometry.Rect = converted;
			return true;
		}
		case GeometryMember::Center:
		{
			cui::core::Point point{};
			if (geometry.Kind != cui::drawing::GeometryKind::Ellipse
				|| !value.TryGet(point) || !std::isfinite(point.x)
				|| !std::isfinite(point.y)) return false;
			geometry.Center = D2D1::Point2F(point.x, point.y);
			return true;
		}
		case GeometryMember::RadiusX:
		case GeometryMember::RadiusY:
		{
			double radius = 0.0;
			if ((geometry.Kind != cui::drawing::GeometryKind::Rectangle
				&& geometry.Kind != cui::drawing::GeometryKind::Ellipse)
				|| !value.TryGetDouble(radius) || !std::isfinite(radius)
				|| radius < -(std::numeric_limits<float>::max)()
				|| radius >(std::numeric_limits<float>::max)()) return false;
			const auto result = static_cast<float>((std::max)(radius, 0.0));
			if (accessor.Member == GeometryMember::RadiusX)
				geometry.RadiusX = result;
			else geometry.RadiusY = result;
			return true;
		}
		case GeometryMember::FillRule:
		{
			std::wstring converted;
			if ((geometry.Kind != cui::drawing::GeometryKind::Path
				&& geometry.Kind != cui::drawing::GeometryKind::Group)
				|| !value.TryGet(converted)) return false;
			if (EqualValueToken(converted, L"Nonzero"))
				geometry.FillRule = cui::drawing::GeometryFillRule::Nonzero;
			else if (EqualValueToken(converted, L"EvenOdd"))
				geometry.FillRule = cui::drawing::GeometryFillRule::EvenOdd;
			else return false;
			return true;
		}
		default:
			return false;
		}
	}

	static bool TryReadPathGeometryMember(
		const cui::drawing::Geometry& root,
		const PathGeometryAccessor& accessor,
		BindingValue& output) noexcept
	{
		const auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		const auto& geometry = *resolved;
		if (geometry.Kind != cui::drawing::GeometryKind::Path
			|| accessor.FigureIndex >= geometry.Figures.size()) return false;
		const auto& figure = geometry.Figures[accessor.FigureIndex];
		auto readPoint = [&](D2D1_POINT_2F point)
			{
				if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
				output = BindingValue(cui::core::Point{ point.x, point.y });
				return true;
			};
		if (!accessor.HasSegment)
		{
			switch (accessor.Member)
			{
			case PathGeometryMember::FigureStartPoint:
				return readPoint(figure.StartPoint);
			case PathGeometryMember::FigureIsClosed:
				output = BindingValue(figure.IsClosed); return true;
			case PathGeometryMember::FigureIsFilled:
				output = BindingValue(figure.IsFilled); return true;
			default:
				return false;
			}
		}
		if (accessor.SegmentIndex >= figure.Segments.size()) return false;
		const auto& segment = figure.Segments[accessor.SegmentIndex];
		if (segment.Kind != accessor.SegmentKind) return false;
		switch (accessor.Member)
		{
		case PathGeometryMember::SegmentPoint:
			return readPoint(segment.Point);
		case PathGeometryMember::SegmentPoint1:
			return readPoint(segment.Point1);
		case PathGeometryMember::SegmentPoint2:
			return readPoint(segment.Point2);
		case PathGeometryMember::SegmentPoint3:
			return readPoint(segment.Point3);
		case PathGeometryMember::ArcSize:
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !std::isfinite(segment.Size.width)
				|| !std::isfinite(segment.Size.height)
				|| segment.Size.width < 0.0f || segment.Size.height < 0.0f)
				return false;
			output = BindingValue(cui::core::Size{
				segment.Size.width, segment.Size.height });
			return true;
		case PathGeometryMember::ArcRotationAngle:
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !std::isfinite(segment.RotationAngle)) return false;
			output = BindingValue(segment.RotationAngle);
			return true;
		case PathGeometryMember::ArcIsLargeArc:
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc) return false;
			output = BindingValue(segment.IsLargeArc);
			return true;
		case PathGeometryMember::ArcSweepDirection:
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc) return false;
			output = BindingValue(std::wstring(segment.Sweep
				== cui::drawing::SweepDirection::Clockwise
				? L"Clockwise" : L"Counterclockwise"));
			return true;
		default:
			return false;
		}
	}

	static bool TryWritePathGeometryMember(
		cui::drawing::Geometry& root,
		const PathGeometryAccessor& accessor,
		const BindingValue& value) noexcept
	{
		auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		auto& geometry = *resolved;
		if (geometry.Kind != cui::drawing::GeometryKind::Path
			|| accessor.FigureIndex >= geometry.Figures.size()) return false;
		auto& figure = geometry.Figures[accessor.FigureIndex];
		auto readPoint = [&](D2D1_POINT_2F& point)
			{
				cui::core::Point converted{};
				if (!value.TryGet(converted) || !std::isfinite(converted.x)
					|| !std::isfinite(converted.y)) return false;
				point = D2D1::Point2F(converted.x, converted.y);
				return true;
			};
		if (!accessor.HasSegment)
		{
			switch (accessor.Member)
			{
			case PathGeometryMember::FigureStartPoint:
				return readPoint(figure.StartPoint);
			case PathGeometryMember::FigureIsClosed:
			{
				bool converted = false;
				if (!value.TryGet(converted)) return false;
				figure.IsClosed = converted;
				return true;
			}
			case PathGeometryMember::FigureIsFilled:
			{
				bool converted = false;
				if (!value.TryGet(converted)) return false;
				figure.IsFilled = converted;
				return true;
			}
			default:
				return false;
			}
		}
		if (accessor.SegmentIndex >= figure.Segments.size()) return false;
		auto& segment = figure.Segments[accessor.SegmentIndex];
		if (segment.Kind != accessor.SegmentKind) return false;
		switch (accessor.Member)
		{
		case PathGeometryMember::SegmentPoint:
			return readPoint(segment.Point);
		case PathGeometryMember::SegmentPoint1:
			return readPoint(segment.Point1);
		case PathGeometryMember::SegmentPoint2:
			return readPoint(segment.Point2);
		case PathGeometryMember::SegmentPoint3:
			return readPoint(segment.Point3);
		case PathGeometryMember::ArcSize:
		{
			cui::core::Size converted{};
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !value.TryGet(converted)
				|| !std::isfinite(converted.width)
				|| !std::isfinite(converted.height)) return false;
			segment.Size = D2D1::SizeF(
				(std::max)(converted.width, 0.0f),
				(std::max)(converted.height, 0.0f));
			return true;
		}
		case PathGeometryMember::ArcRotationAngle:
		{
			double converted = 0.0;
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !value.TryGetDouble(converted) || !std::isfinite(converted)
				|| converted < -(std::numeric_limits<float>::max)()
				|| converted >(std::numeric_limits<float>::max)()) return false;
			segment.RotationAngle = static_cast<float>(converted);
			return true;
		}
		case PathGeometryMember::ArcIsLargeArc:
		{
			bool converted = false;
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !value.TryGet(converted)) return false;
			segment.IsLargeArc = converted;
			return true;
		}
		case PathGeometryMember::ArcSweepDirection:
		{
			std::wstring converted;
			if (segment.Kind != cui::drawing::PathSegmentKind::Arc
				|| !value.TryGet(converted)) return false;
			if (EqualValueToken(converted, L"Clockwise"))
				segment.Sweep = cui::drawing::SweepDirection::Clockwise;
			else if (EqualValueToken(converted, L"Counterclockwise"))
				segment.Sweep = cui::drawing::SweepDirection::Counterclockwise;
			else return false;
			return true;
		}
		default:
			return false;
		}
	}

	static bool TryReadGeometryTransformMember(
		const cui::drawing::Geometry& root,
		const GeometryTransformAccessor& accessor,
		BindingValue& output) noexcept
	{
		const auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		const auto& geometry = *resolved;
		if (geometry.Kind != accessor.GeometryKind) return false;
		if (!geometry.LocalTransform
			|| !TryReadTransformMember(
				*geometry.LocalTransform, accessor.Transform, output)) return false;
		return true;
	}

	static bool TryWriteGeometryTransformMember(
		cui::drawing::Geometry& root,
		const GeometryTransformAccessor& accessor,
		const BindingValue& value) noexcept
	{
		auto* resolved = TryGetGeometryChild(root, accessor.ChildIndices);
		if (!resolved) return false;
		auto& geometry = *resolved;
		return geometry.Kind == accessor.GeometryKind
			&& geometry.LocalTransform
			&& TryWriteTransformMember(
				*geometry.LocalTransform, accessor.Transform, value);
	}

	static bool TryReadBrushMember(
		const cui::drawing::Brush& brush,
		const BrushAccessor& accessor,
		BindingValue& output) noexcept
	{
		if (brush.Kind != accessor.BrushKind) return false;
		auto finiteColor = [](const D2D1_COLOR_F& color)
			{
				return std::isfinite(color.r) && std::isfinite(color.g)
					&& std::isfinite(color.b) && std::isfinite(color.a);
			};
		auto readPoint = [&](D2D1_POINT_2F point)
			{
				if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
				output = BindingValue(cui::core::Point{ point.x, point.y });
				return true;
			};
		switch (accessor.Member)
		{
		case BrushMember::SolidColor:
			if (brush.Kind != cui::drawing::BrushKind::Solid
				|| !finiteColor(brush.Color)) return false;
			output = BindingValue(brush.Color);
			return true;
		case BrushMember::Opacity:
			if (!std::isfinite(brush.Opacity)
				|| brush.Opacity < 0.0f || brush.Opacity > 1.0f) return false;
			output = BindingValue(brush.Opacity);
			return true;
		case BrushMember::StartPoint:
			return brush.Kind == cui::drawing::BrushKind::LinearGradient
				&& readPoint(brush.StartPoint);
		case BrushMember::EndPoint:
			return brush.Kind == cui::drawing::BrushKind::LinearGradient
				&& readPoint(brush.EndPoint);
		case BrushMember::Center:
			return brush.Kind == cui::drawing::BrushKind::RadialGradient
				&& readPoint(brush.Center);
		case BrushMember::GradientOrigin:
			return brush.Kind == cui::drawing::BrushKind::RadialGradient
				&& readPoint(brush.GradientOrigin);
		case BrushMember::RadiusX:
		case BrushMember::RadiusY:
		{
			if (brush.Kind != cui::drawing::BrushKind::RadialGradient) return false;
			const auto radius = accessor.Member == BrushMember::RadiusX
				? brush.RadiusX : brush.RadiusY;
			if (!std::isfinite(radius) || radius < 0.0f) return false;
			output = BindingValue(radius);
			return true;
		}
		case BrushMember::GradientStopColor:
		case BrushMember::GradientStopOffset:
		{
			if ((brush.Kind != cui::drawing::BrushKind::LinearGradient
				&& brush.Kind != cui::drawing::BrushKind::RadialGradient)
				|| accessor.StopIndex >= brush.GradientStops.size()) return false;
			const auto& stop = brush.GradientStops[accessor.StopIndex];
			if (accessor.Member == BrushMember::GradientStopColor)
			{
				if (!finiteColor(stop.Color)) return false;
				output = BindingValue(stop.Color);
				return true;
			}
			if (!std::isfinite(stop.Offset)
				|| stop.Offset < 0.0f || stop.Offset > 1.0f) return false;
			output = BindingValue(stop.Offset);
			return true;
		}
		default:
			return false;
		}
	}

	static bool TryWriteBrushMember(
		cui::drawing::Brush& brush,
		const BrushAccessor& accessor,
		const BindingValue& value) noexcept
	{
		if (brush.Kind != accessor.BrushKind) return false;
		auto readColor = [&](D2D1_COLOR_F& color)
			{
				return value.TryGet(color)
					&& std::isfinite(color.r) && std::isfinite(color.g)
					&& std::isfinite(color.b) && std::isfinite(color.a);
			};
		auto readPoint = [&](D2D1_POINT_2F& point)
			{
				cui::core::Point parsed{};
				if (!value.TryGet(parsed) || !std::isfinite(parsed.x)
					|| !std::isfinite(parsed.y)) return false;
				point = D2D1::Point2F(parsed.x, parsed.y);
				return true;
			};
		auto readNumber = [&](double& number)
			{
				return value.TryGetDouble(number) && std::isfinite(number)
					&& number >= -(std::numeric_limits<float>::max)()
					&& number <= (std::numeric_limits<float>::max)();
			};
		switch (accessor.Member)
		{
		case BrushMember::SolidColor:
		{
			D2D1_COLOR_F color{};
			if (brush.Kind != cui::drawing::BrushKind::Solid || !readColor(color))
				return false;
			brush.Color = color;
			return true;
		}
		case BrushMember::Opacity:
		{
			double opacity = 0.0;
			if (!readNumber(opacity)) return false;
			brush.Opacity = static_cast<float>((std::clamp)(opacity, 0.0, 1.0));
			return true;
		}
		case BrushMember::StartPoint:
			return brush.Kind == cui::drawing::BrushKind::LinearGradient
				&& readPoint(brush.StartPoint);
		case BrushMember::EndPoint:
			return brush.Kind == cui::drawing::BrushKind::LinearGradient
				&& readPoint(brush.EndPoint);
		case BrushMember::Center:
			return brush.Kind == cui::drawing::BrushKind::RadialGradient
				&& readPoint(brush.Center);
		case BrushMember::GradientOrigin:
			return brush.Kind == cui::drawing::BrushKind::RadialGradient
				&& readPoint(brush.GradientOrigin);
		case BrushMember::RadiusX:
		case BrushMember::RadiusY:
		{
			double radius = 0.0;
			if (brush.Kind != cui::drawing::BrushKind::RadialGradient
				|| !readNumber(radius)) return false;
			const auto result = static_cast<float>((std::max)(radius, 0.0));
			if (accessor.Member == BrushMember::RadiusX) brush.RadiusX = result;
			else brush.RadiusY = result;
			return true;
		}
		case BrushMember::GradientStopColor:
		case BrushMember::GradientStopOffset:
		{
			if ((brush.Kind != cui::drawing::BrushKind::LinearGradient
				&& brush.Kind != cui::drawing::BrushKind::RadialGradient)
				|| accessor.StopIndex >= brush.GradientStops.size()) return false;
			auto& stop = brush.GradientStops[accessor.StopIndex];
			if (accessor.Member == BrushMember::GradientStopColor)
			{
				D2D1_COLOR_F color{};
				if (!readColor(color)) return false;
				stop.Color = color;
				return true;
			}
			double offset = 0.0;
			if (!readNumber(offset)) return false;
			// GradientStop.Offset is authored in 0..1 in CUI. Animation results
			// pass through the same target-property coercion at frame write time.
			stop.Offset = static_cast<float>((std::clamp)(offset, 0.0, 1.0));
			return true;
		}
		default:
			return false;
		}
	}

	static bool TryReadBrushTransformMember(
		const cui::drawing::Brush& brush,
		const BrushTransformAccessor& accessor,
		BindingValue& output) noexcept
	{
		if (brush.Kind != accessor.BrushKind) return false;
		const auto& transform = accessor.Relative
			? brush.RelativeTransform : brush.Transform;
		if (!transform
			|| !TryReadTransformMember(*transform, accessor.Transform, output))
			return false;
		return true;
	}

	static bool TryWriteBrushTransformMember(
		cui::drawing::Brush& brush,
		const BrushTransformAccessor& accessor,
		const BindingValue& value) noexcept
	{
		if (brush.Kind != accessor.BrushKind) return false;
		auto& transform = accessor.Relative
			? brush.RelativeTransform : brush.Transform;
		return transform
			&& TryWriteTransformMember(*transform, accessor.Transform, value);
	}

	static bool TryReadObjectPathMember(
		const BindingValue& root,
		const ObjectPathAccessor& accessor,
		BindingValue& output) noexcept
	{
		return std::visit([&](const auto& typed)
			{
				using T = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<T, TransformAccessor>)
				{
					cui::drawing::Transform transform;
					if (!root.TryGet(transform)
						|| !TryReadTransformMember(transform, typed, output)) return false;
					return true;
				}
				else if constexpr (std::is_same_v<T, GeometryAccessor>)
				{
					cui::drawing::Geometry geometry;
					return root.TryGet(geometry)
						&& TryReadGeometryMember(geometry, typed, output);
				}
				else if constexpr (std::is_same_v<T, PathGeometryAccessor>)
				{
					cui::drawing::Geometry geometry;
					return root.TryGet(geometry)
						&& TryReadPathGeometryMember(geometry, typed, output);
				}
				else if constexpr (std::is_same_v<T, GeometryTransformAccessor>)
				{
					cui::drawing::Geometry geometry;
					return root.TryGet(geometry)
						&& TryReadGeometryTransformMember(geometry, typed, output);
				}
				else if constexpr (std::is_same_v<T, BrushAccessor>)
				{
					cui::drawing::Brush brush;
					return root.TryGet(brush)
						&& TryReadBrushMember(brush, typed, output);
				}
				else if constexpr (std::is_same_v<T, BrushTransformAccessor>)
				{
					cui::drawing::Brush brush;
					return root.TryGet(brush)
						&& TryReadBrushTransformMember(brush, typed, output);
				}
				else return false;
			}, accessor);
	}

	static bool TryWriteObjectPathMember(
		BindingValue& root,
		const ObjectPathAccessor& accessor,
		const BindingValue& member) noexcept
	{
		return std::visit([&](const auto& typed)
			{
				using T = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<T, TransformAccessor>)
				{
					cui::drawing::Transform transform;
					if (!root.TryGet(transform)
						|| !TryWriteTransformMember(transform, typed, member)) return false;
					root = BindingValue(std::move(transform));
					return true;
				}
				else if constexpr (std::is_same_v<T, GeometryAccessor>)
				{
					cui::drawing::Geometry geometry;
					if (!root.TryGet(geometry)
						|| !TryWriteGeometryMember(geometry, typed, member)) return false;
					root = BindingValue(std::move(geometry));
					return true;
				}
				else if constexpr (std::is_same_v<T, PathGeometryAccessor>)
				{
					cui::drawing::Geometry geometry;
					if (!root.TryGet(geometry)
						|| !TryWritePathGeometryMember(
							geometry, typed, member)) return false;
					root = BindingValue(std::move(geometry));
					return true;
				}
				else if constexpr (std::is_same_v<T, GeometryTransformAccessor>)
				{
					cui::drawing::Geometry geometry;
					if (!root.TryGet(geometry)
						|| !TryWriteGeometryTransformMember(
							geometry, typed, member)) return false;
					root = BindingValue(std::move(geometry));
					return true;
				}
				else if constexpr (std::is_same_v<T, BrushAccessor>)
				{
					cui::drawing::Brush brush;
					if (!root.TryGet(brush)
						|| !TryWriteBrushMember(brush, typed, member)) return false;
					root = BindingValue(std::move(brush));
					return true;
				}
				else if constexpr (std::is_same_v<T, BrushTransformAccessor>)
				{
					cui::drawing::Brush brush;
					if (!root.TryGet(brush)
						|| !TryWriteBrushTransformMember(brush, typed, member)) return false;
					root = BindingValue(std::move(brush));
					return true;
				}
				else return false;
			}, accessor);
	}

	static double Ease(
		double progress,
		DeclarativeEasingKind kind,
		DeclarativeEasingMode mode,
		DeclarativeEasingParameters parameters) noexcept
	{
		progress = (std::clamp)(progress, 0.0, 1.0);
		if (kind == DeclarativeEasingKind::Linear) return progress;
		auto easeIn = [kind, parameters](double value)
			{
				constexpr double Pi = 3.14159265358979323846;
				constexpr double TwoPi = Pi * 2.0;
				constexpr double CloseEpsilon =
					10.0 * std::numeric_limits<double>::epsilon();
				switch (kind)
				{
				case DeclarativeEasingKind::Quadratic: return value * value;
				case DeclarativeEasingKind::Cubic: return value * value * value;
				case DeclarativeEasingKind::Sine:
					return 1.0 - std::cos(value * 1.57079632679489661923);
				case DeclarativeEasingKind::Back:
					return std::pow(value, 3.0) - value
						* (std::max)(0.0, parameters.Primary)
						* std::sin(Pi * value);
				case DeclarativeEasingKind::Bounce:
				{
					const auto bounces = (std::max)(0.0, parameters.Secondary);
					auto bounciness = parameters.Primary;
					if (bounciness < 1.0
						|| std::abs(bounciness - 1.0) < CloseEpsilon)
						bounciness = 1.001;
					const auto power = std::pow(bounciness, bounces);
					const auto oneMinusBounciness = 1.0 - bounciness;
					const auto sumOfUnits = (1.0 - power) / oneMinusBounciness
						+ power * 0.5;
					const auto unitAtTime = value * sumOfUnits;
					const auto bounceAtTime = std::log(
						-unitAtTime * oneMinusBounciness + 1.0)
						/ std::log(bounciness);
					const auto start = std::floor(bounceAtTime);
					const auto end = start + 1.0;
					const auto startTime = (1.0 - std::pow(bounciness, start))
						/ (oneMinusBounciness * sumOfUnits);
					const auto endTime = (1.0 - std::pow(bounciness, end))
						/ (oneMinusBounciness * sumOfUnits);
					const auto middleTime = (startTime + endTime) * 0.5;
					const auto timeFromPeak = value - middleTime;
					const auto radius = middleTime - startTime;
					const auto amplitude = std::pow(
						1.0 / bounciness, bounces - start);
					return (-amplitude / (radius * radius))
						* (timeFromPeak - radius) * (timeFromPeak + radius);
				}
				case DeclarativeEasingKind::Circle:
					return 1.0 - std::sqrt(1.0 - value * value);
				case DeclarativeEasingKind::Elastic:
				{
					const auto oscillations =
						(std::max)(0.0, parameters.Secondary);
					const auto springiness =
						(std::max)(0.0, parameters.Primary);
					const auto exponential = std::abs(springiness) < CloseEpsilon
						? value
						: (std::exp(springiness * value) - 1.0)
							/ (std::exp(springiness) - 1.0);
					return exponential * std::sin(
						(TwoPi * oscillations + Pi * 0.5) * value);
				}
				case DeclarativeEasingKind::Exponential:
					return std::abs(parameters.Primary) < CloseEpsilon
						? value
						: (std::exp(parameters.Primary * value) - 1.0)
							/ (std::exp(parameters.Primary) - 1.0);
				case DeclarativeEasingKind::Power:
					return std::pow(value,
						(std::max)(0.0, parameters.Primary));
				case DeclarativeEasingKind::Quartic:
					return value * value * value * value;
				case DeclarativeEasingKind::Quintic:
					return value * value * value * value * value;
				case DeclarativeEasingKind::Linear:
				default: return value;
				}
			};
		switch (mode)
		{
		case DeclarativeEasingMode::EaseIn:
			return easeIn(progress);
		case DeclarativeEasingMode::EaseInOut:
			return progress < 0.5
				? easeIn(progress * 2.0) * 0.5
				: 1.0 - easeIn((1.0 - progress) * 2.0) * 0.5;
		case DeclarativeEasingMode::EaseOut:
		default:
			return 1.0 - easeIn(1.0 - progress);
		}
	}

	static double CubicBezier(double t, double p1, double p2) noexcept
	{
		const auto inverse = 1.0 - t;
		return 3.0 * inverse * inverse * t * p1
			+ 3.0 * inverse * t * t * p2 + t * t * t;
	}

	static double CubicBezierDerivative(double t, double p1, double p2) noexcept
	{
		const auto inverse = 1.0 - t;
		return 3.0 * inverse * inverse * p1
			+ 6.0 * inverse * t * (p2 - p1)
			+ 3.0 * t * t * (1.0 - p2);
	}

	static double KeySplineProgress(
		double progress,
		const RuntimeAnimationKeyFrame& keyFrame,
		double& parameter) noexcept
	{
		progress = (std::clamp)(progress, 0.0, 1.0);
		if (keyFrame.KeySplineX1 == 0.0f
			&& keyFrame.KeySplineY1 == 0.0f
			&& keyFrame.KeySplineX2 == 1.0f
			&& keyFrame.KeySplineY2 == 1.0f)
			return progress;

		if (progress == 0.0)
			parameter = 0.0;
		else if (progress == 1.0)
			parameter = 1.0;
		else
		{
			// Match WPF KeySpline.SetParameterFromX: its stopping rule is
			// expressed in eventual Y accuracy and its previous parameter is a
			// deliberate part of the clock's runtime state.
			constexpr double accuracy = 0.001;
			constexpr double fuzz = 0.000001;
			double bottom = 0.0;
			double top = 1.0;
			while (top - bottom > fuzz)
			{
				const auto x = CubicBezier(
					parameter, keyFrame.KeySplineX1, keyFrame.KeySplineX2);
				const auto derivative = CubicBezierDerivative(
					parameter, keyFrame.KeySplineX1, keyFrame.KeySplineX2);
				const auto absoluteDerivative = std::fabs(derivative);
				if (x > progress) top = parameter;
				else bottom = parameter;
				if (std::fabs(x - progress) < accuracy * absoluteDerivative)
					break;
				if (absoluteDerivative > fuzz)
				{
					const auto next = parameter - (x - progress) / derivative;
					if (next >= top) parameter = (parameter + top) * 0.5;
					else if (next <= bottom)
						parameter = (parameter + bottom) * 0.5;
					else parameter = next;
				}
				else parameter = (bottom + top) * 0.5;
			}
		}
		return CubicBezier(
			parameter, keyFrame.KeySplineY1, keyFrame.KeySplineY2);
	}

	static float StoredSrgbToScRgbChannel(float value)
	{
		if (value <= 0.04045f) return value / 12.92f;
		return static_cast<float>(std::pow(
			(static_cast<double>(value) + 0.055) / 1.055, 2.4));
	}

	static float ScRgbToStoredSrgbChannel(float value)
	{
		if (value <= 0.0031308f) return value * 12.92f;
		return static_cast<float>(1.055 * std::pow(
			static_cast<double>(value), 1.0 / 2.4) - 0.055);
	}

	static bool InterpolateValues(
		DeclarativeAnimationKind kind,
		const DependencyPropertyMetadata* metadata,
		bool transformPath,
		const BindingValue& fromValue,
		const BindingValue& toValue,
		double progress,
		BindingValue& output)
	{
		if (!std::isfinite(progress)) return false;
		if (kind == DeclarativeAnimationKind::Int32)
		{
			int from = 0, to = 0;
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			if (progress == 0.0) { output = BindingValue(from); return true; }
			if (progress == 1.0) { output = BindingValue(to); return true; }
			const auto differenceBits = static_cast<std::uint32_t>(to)
				- static_cast<std::uint32_t>(from);
			const auto difference = differenceBits
				<= static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
				? static_cast<double>(differenceBits)
				: -static_cast<double>((~differenceBits) + 1u);
			double addend = difference * progress;
			addend += addend > 0.0 ? 0.5 : -0.5;
			if (!std::isfinite(addend)
				|| addend < static_cast<double>((std::numeric_limits<int>::min)())
				|| addend > static_cast<double>((std::numeric_limits<int>::max)()))
				return false;
			const auto rounded = static_cast<int>(addend);
			const auto resultBits = static_cast<std::uint32_t>(from)
				+ static_cast<std::uint32_t>(rounded);
			const auto result = resultBits
				<= static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
				? static_cast<int>(resultBits)
				: -1 - static_cast<int>(~resultBits);
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Int64)
		{
			long long from = 0, to = 0;
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			if (progress == 0.0) { output = BindingValue(from); return true; }
			if (progress == 1.0) { output = BindingValue(to); return true; }
			const auto differenceBits = static_cast<std::uint64_t>(to)
				- static_cast<std::uint64_t>(from);
			const auto difference = differenceBits
				<= static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())
				? static_cast<double>(differenceBits)
				: -static_cast<double>((~differenceBits) + 1ull);
			double addend = difference * progress;
			addend += addend > 0.0 ? 0.5 : -0.5;
			if (!std::isfinite(addend)
				|| static_cast<long double>(addend)
					< static_cast<long double>((std::numeric_limits<long long>::min)())
				|| static_cast<long double>(addend)
					> static_cast<long double>((std::numeric_limits<long long>::max)()))
				return false;
			const auto rounded = static_cast<long long>(addend);
			const auto resultBits = static_cast<std::uint64_t>(from)
				+ static_cast<std::uint64_t>(rounded);
			const auto result = resultBits
				<= static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())
				? static_cast<long long>(resultBits)
				: -1 - static_cast<long long>(~resultBits);
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Single)
		{
			float from = 0.0f, to = 0.0f;
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			const float difference = to - from;
			const float increment = static_cast<float>(
				static_cast<double>(difference) * progress);
			const float result = from + increment;
			if (!std::isfinite(result)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Thickness)
		{
			Thickness from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			Thickness result;
			if (!lerp(from.Left, to.Left, result.Left)
				|| !lerp(from.Top, to.Top, result.Top)
				|| !lerp(from.Right, to.Right, result.Right)
				|| !lerp(from.Bottom, to.Bottom, result.Bottom)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Size)
		{
			cui::core::Size from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Size result;
			if (!lerp(from.width, to.width, result.width)
				|| !lerp(from.height, to.height, result.height)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Matrix)
		{
			D2D1_MATRIX_3X2_F from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)
				|| !IsFiniteMatrix(from) || !IsFiniteMatrix(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			D2D1_MATRIX_3X2_F result{};
			if (!lerp(from._11, to._11, result._11)
				|| !lerp(from._12, to._12, result._12)
				|| !lerp(from._21, to._21, result._21)
				|| !lerp(from._22, to._22, result._22)
				|| !lerp(from._31, to._31, result._31)
				|| !lerp(from._32, to._32, result._32)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Point)
		{
			cui::core::Point from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Point result;
			if (!lerp(from.x, to.x, result.x)
				|| !lerp(from.y, to.y, result.y)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Vector)
		{
			cui::core::Vector from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Vector result;
			if (!lerp(from.x, to.x, result.x)
				|| !lerp(from.y, to.y, result.y)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Rect)
		{
			cui::core::Rect from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerp = [progress](float left, float right, float& result)
				{
					const auto exact = static_cast<long double>(left)
						+ (static_cast<long double>(right)
							- static_cast<long double>(left)) * progress;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Rect result;
			if (!lerp(from.x, to.x, result.x)
				|| !lerp(from.y, to.y, result.y)
				|| !lerp(from.width, to.width, result.width)
				|| !lerp(from.height, to.height, result.height)) return false;
			output = BindingValue(result);
			return true;
		}
		if (kind == DeclarativeAnimationKind::Color)
		{
			D2D1_COLOR_F from{}, to{};
			if (!fromValue.TryGet(from) || !toValue.TryGet(to)) return false;
			auto lerpLinear = [progress](
				float left, float right)
				{
					return ScRgbToStoredSrgbChannel(static_cast<float>(
						StoredSrgbToScRgbChannel(left)
						+ (StoredSrgbToScRgbChannel(right)
							- StoredSrgbToScRgbChannel(left)) * progress));
				};
			auto lerpAlpha = [progress](float left, float right)
				{
					return static_cast<float>(left + (right - left) * progress);
				};
			const D2D1_COLOR_F result{
				lerpLinear(from.r, to.r), lerpLinear(from.g, to.g),
				lerpLinear(from.b, to.b), lerpAlpha(from.a, to.a) };
			if (!std::isfinite(result.r) || !std::isfinite(result.g)
				|| !std::isfinite(result.b) || !std::isfinite(result.a)) return false;
			output = BindingValue(result);
			return true;
		}

		double from = 0.0;
		double to = 0.0;
		if (!fromValue.TryGetDouble(from)
			|| !toValue.TryGetDouble(to)) return false;
		const double value = from + (to - from) * progress;
		if (transformPath)
		{
			if (!std::isfinite(value)
				|| value < -(std::numeric_limits<float>::max)()
				|| value >(std::numeric_limits<float>::max)()) return false;
			output = BindingValue(static_cast<float>(value));
			return true;
		}
		if (!metadata) return false;
		switch (metadata->ValueKind())
		{
		case BindingValueKind::Int:
			if (value <= static_cast<double>((std::numeric_limits<int>::min)()))
				output = BindingValue((std::numeric_limits<int>::min)());
			else if (value >= static_cast<double>((std::numeric_limits<int>::max)()))
				output = BindingValue((std::numeric_limits<int>::max)());
			else
				output = BindingValue(static_cast<int>(std::llround(value)));
			return true;
		case BindingValueKind::Int64:
			// The double representation of LLONG_MAX rounds to 2^63, which is
			// outside llround's domain. Clamp before rounding so a legal endpoint
			// cannot introduce undefined behaviour on the final animation frame.
			if (value <= static_cast<double>((std::numeric_limits<long long>::min)()))
				output = BindingValue((std::numeric_limits<long long>::min)());
			else if (value >= static_cast<double>((std::numeric_limits<long long>::max)()))
				output = BindingValue((std::numeric_limits<long long>::max)());
			else
				output = BindingValue(static_cast<long long>(std::llround(value)));
			return true;
		case BindingValueKind::Float:
			output = BindingValue(static_cast<float>(value));
			return true;
		case BindingValueKind::Double:
			output = BindingValue(value);
			return true;
		default:
			return false;
		}
	}

	template<typename TAnimation>
	static bool CombineAnimationValues(
		const TAnimation& animation,
		const BindingValue& left,
		const BindingValue& right,
		long double rightScale,
		BindingValue& output)
	{
		if (animation.Kind == DeclarativeAnimationKind::Int32)
		{
			int base = 0, increment = 0;
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			const double scaled = static_cast<double>(increment)
				* static_cast<double>(rightScale);
			if (!std::isfinite(scaled)
				|| scaled < static_cast<double>((std::numeric_limits<int>::min)())
				|| scaled > static_cast<double>((std::numeric_limits<int>::max)()))
				return false;
			const auto resultBits = static_cast<std::uint32_t>(base)
				+ static_cast<std::uint32_t>(static_cast<int>(scaled));
			output = BindingValue(resultBits
				<= static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
				? static_cast<int>(resultBits)
				: -1 - static_cast<int>(~resultBits));
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Int64)
		{
			long long base = 0, increment = 0;
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			const double scaled = static_cast<double>(increment)
				* static_cast<double>(rightScale);
			if (!std::isfinite(scaled)
				|| static_cast<long double>(scaled)
					< static_cast<long double>((std::numeric_limits<long long>::min)())
				|| static_cast<long double>(scaled)
					> static_cast<long double>((std::numeric_limits<long long>::max)()))
				return false;
			const auto resultBits = static_cast<std::uint64_t>(base)
				+ static_cast<std::uint64_t>(static_cast<long long>(scaled));
			output = BindingValue(resultBits
				<= static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())
				? static_cast<long long>(resultBits)
				: -1 - static_cast<long long>(~resultBits));
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Single)
		{
			float base = 0.0f, increment = 0.0f;
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			const float scaled = static_cast<float>(static_cast<double>(increment)
				* static_cast<double>(rightScale));
			const float result = base + scaled;
			if (!std::isfinite(result)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Thickness)
		{
			Thickness base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			Thickness result;
			if (!combine(base.Left, increment.Left, result.Left)
				|| !combine(base.Top, increment.Top, result.Top)
				|| !combine(base.Right, increment.Right, result.Right)
				|| !combine(base.Bottom, increment.Bottom, result.Bottom)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Size)
		{
			cui::core::Size base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Size result;
			if (!combine(base.width, increment.width, result.width)
				|| !combine(base.height, increment.height, result.height)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Matrix)
		{
			D2D1_MATRIX_3X2_F base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)
				|| !IsFiniteMatrix(base) || !IsFiniteMatrix(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			D2D1_MATRIX_3X2_F result{};
			if (!combine(base._11, increment._11, result._11)
				|| !combine(base._12, increment._12, result._12)
				|| !combine(base._21, increment._21, result._21)
				|| !combine(base._22, increment._22, result._22)
				|| !combine(base._31, increment._31, result._31)
				|| !combine(base._32, increment._32, result._32)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Point)
		{
			cui::core::Point base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Point result;
			if (!combine(base.x, increment.x, result.x)
				|| !combine(base.y, increment.y, result.y)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Vector)
		{
			cui::core::Vector base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Vector result;
			if (!combine(base.x, increment.x, result.x)
				|| !combine(base.y, increment.y, result.y)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Rect)
		{
			cui::core::Rect base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](float leftValue, float rightValue,
				float& result)
				{
					const auto exact = static_cast<long double>(leftValue)
						+ static_cast<long double>(rightValue) * rightScale;
					if (!std::isfinite(exact)
						|| exact < -(std::numeric_limits<float>::max)()
						|| exact >(std::numeric_limits<float>::max)()) return false;
					result = static_cast<float>(exact);
					return std::isfinite(result);
				};
			cui::core::Rect result;
			if (!combine(base.x, increment.x, result.x)
				|| !combine(base.y, increment.y, result.y)
				|| !combine(base.width, increment.width, result.width)
				|| !combine(base.height, increment.height, result.height)) return false;
			output = BindingValue(result);
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Color)
		{
			D2D1_COLOR_F base{}, increment{};
			if (!left.TryGet(base) || !right.TryGet(increment)) return false;
			auto combine = [rightScale](
				float leftValue, float rightValue)
				{
					return ScRgbToStoredSrgbChannel(static_cast<float>(
						StoredSrgbToScRgbChannel(leftValue)
						+ StoredSrgbToScRgbChannel(rightValue) * rightScale));
				};
			const D2D1_COLOR_F result{
				combine(base.r, increment.r),
				combine(base.g, increment.g),
				combine(base.b, increment.b),
				static_cast<float>(base.a + increment.a * rightScale) };
			if (!std::isfinite(result.r) || !std::isfinite(result.g)
				|| !std::isfinite(result.b) || !std::isfinite(result.a)) return false;
			output = BindingValue(result);
			return true;
		}

		double base = 0.0;
		double increment = 0.0;
		if (!left.TryGetDouble(base) || !right.TryGetDouble(increment)) return false;
		// Preserve legal sentinel values such as Canvas.Left=NaN when composing
		// the zero foundation of a non-additive key-frame animation. Arithmetic
		// would turn the unchanged value into an interpolation failure.
		if (rightScale == 0.0L || increment == 0.0)
		{
			output = left;
			return true;
		}
		const long double exact = static_cast<long double>(base)
			+ static_cast<long double>(increment) * rightScale;
		if (!std::isfinite(exact)
			|| exact < -(std::numeric_limits<double>::max)()
			|| exact >(std::numeric_limits<double>::max)()) return false;
		const double result = static_cast<double>(exact);
		if (!std::isfinite(result)) return false;
		if ((ObjectPathUsesFloat(animation.ObjectPath)
			|| (animation.Metadata
				&& animation.Metadata->ValueKind() == BindingValueKind::Float))
			&& (result < -(std::numeric_limits<float>::max)()
				|| result >(std::numeric_limits<float>::max)())) return false;
		output = BindingValue(result);
		return true;
	}

	template<typename TAnimation>
	static BindingValue ZeroAnimationValue(const TAnimation& animation)
	{
		if (animation.Kind == DeclarativeAnimationKind::Object)
			return BindingValue{};
		if (animation.Kind == DeclarativeAnimationKind::Int32)
			return BindingValue(0);
		if (animation.Kind == DeclarativeAnimationKind::Int64)
			return BindingValue(0ll);
		if (animation.Kind == DeclarativeAnimationKind::Single)
			return BindingValue(0.0f);
		if (animation.Kind == DeclarativeAnimationKind::Thickness)
			return BindingValue(Thickness{});
		if (animation.Kind == DeclarativeAnimationKind::Point)
			return BindingValue(cui::core::Point{});
		if (animation.Kind == DeclarativeAnimationKind::Vector)
			return BindingValue(cui::core::Vector{});
		if (animation.Kind == DeclarativeAnimationKind::Rect)
			return BindingValue(cui::core::Rect{});
		if (animation.Kind == DeclarativeAnimationKind::Size)
			return BindingValue(cui::core::Size{});
		if (animation.Kind == DeclarativeAnimationKind::Matrix)
			return BindingValue(D2D1_MATRIX_3X2_F{});
		return animation.Kind == DeclarativeAnimationKind::Color
			? BindingValue(D2D1_COLOR_F{}) : BindingValue(0.0);
	}

	template<typename TAnimation>
	static bool AddAnimationValues(
		const TAnimation& animation,
		const BindingValue& left,
		const BindingValue& delta,
		BindingValue& output)
	{
		return CombineAnimationValues(animation, left, delta, 1.0L, output);
	}

	template<typename TAnimation>
	static bool MatrixKeyFramesUseDirectValues(const TAnimation& animation) noexcept
	{
		// WPF's MatrixAnimationUsingKeyFrames returns the resolved key-frame
		// value directly. The inherited additive/cumulative flags are accepted
		// by XAML but its generated GetCurrentValueCore does not apply them.
		return animation.Kind == DeclarativeAnimationKind::Matrix
			&& !animation.KeyFrames.empty();
	}

	static bool ResolveAnimationEndpoints(
		const RuntimeAnimation& animation,
		const BindingValue& defaultOrigin,
		const BindingValue& defaultDestination,
		BindingValue& from,
		BindingValue& to,
		BindingValue& foundation)
	{
		if (defaultOrigin.Kind() == BindingValueKind::Empty
			|| defaultDestination.Kind() == BindingValueKind::Empty) return false;
		const auto zero = ZeroAnimationValue(animation);
		if (animation.Path.Enabled)
		{
			from = defaultOrigin;
			to = defaultDestination;
			foundation = defaultOrigin;
			return true;
		}
		if (!animation.KeyFrames.empty())
		{
			if (animation.Kind == DeclarativeAnimationKind::Object)
			{
				from = defaultOrigin;
				to = defaultDestination;
				foundation = BindingValue{};
				return true;
			}
			if (MatrixKeyFramesUseDirectValues(animation))
			{
				from = defaultOrigin;
				to = defaultDestination;
				foundation = zero;
				return true;
			}
			from = animation.IsAdditive ? zero : defaultOrigin;
			to = defaultDestination;
			foundation = animation.IsAdditive ? defaultOrigin : zero;
			return true;
		}
		const bool hasFrom = animation.From.has_value();
		const bool hasTo = animation.To.has_value();
		const bool hasBy = animation.By.has_value() && !hasTo;
		if (!hasFrom && hasBy)
		{
			from = zero;
			to = *animation.By;
			foundation = defaultOrigin;
			return true;
		}
		from = hasFrom ? *animation.From : defaultOrigin;
		if (animation.To)
			to = *animation.To;
		else if (hasBy)
		{
			if (!AddAnimationValues(animation, from, *animation.By, to)) return false;
		}
		else to = defaultDestination;
		foundation = animation.IsAdditive && hasFrom && (hasTo || hasBy)
			? defaultOrigin : zero;
		return true;
	}

	static long double TimelineActiveDurationExact(
		unsigned long long durationMilliseconds,
		DeclarativeRepeatBehaviorKind repeatBehavior,
		double repeatCount,
		unsigned long long repeatDurationMilliseconds,
		bool autoReverse,
		double speedRatio) noexcept
	{
		if (repeatBehavior == DeclarativeRepeatBehaviorKind::Forever)
			return std::numeric_limits<long double>::infinity();
		if (repeatBehavior == DeclarativeRepeatBehaviorKind::Duration)
			return static_cast<long double>(repeatDurationMilliseconds);
		return static_cast<long double>(durationMilliseconds)
			* (autoReverse ? 2.0L : 1.0L)
			* static_cast<long double>(repeatCount)
			/ static_cast<long double>(speedRatio);
	}

	static long double TimelineActiveDurationExact(
		const TimelineDefinition& timeline) noexcept
	{
		return TimelineActiveDurationExact(timeline.DurationMilliseconds,
			timeline.RepeatBehavior, timeline.RepeatCount,
			timeline.RepeatDurationMilliseconds, timeline.AutoReverse,
			timeline.SpeedRatio);
	}

	static unsigned long long TimelineActiveDurationMilliseconds(
		const TimelineDefinition& timeline) noexcept
	{
		const auto duration = TimelineActiveDurationExact(timeline);
		const auto maximum = (std::numeric_limits<unsigned long long>::max)();
		if (!std::isfinite(duration)
			|| duration >= static_cast<long double>(maximum)) return maximum;
		return static_cast<unsigned long long>(std::ceil(duration));
	}

	template<typename TAnimations>
	static bool ResolveStoryboardTiming(
		const DeclarativeStoryboardTimingDefinition& authored,
		const TAnimations& animations,
		unsigned long long& simpleDurationMilliseconds,
		bool& simpleDurationForever) noexcept
	{
		if (!std::isfinite(authored.SpeedRatio) || authored.SpeedRatio <= 0.0
			|| !std::isfinite(authored.AccelerationRatio)
			|| !std::isfinite(authored.DecelerationRatio)
			|| authored.AccelerationRatio < 0.0
			|| authored.DecelerationRatio < 0.0
			|| authored.AccelerationRatio + authored.DecelerationRatio > 1.0)
			return false;
		if (static_cast<unsigned char>(authored.RepeatBehavior)
			> static_cast<unsigned char>(
				DeclarativeRepeatBehaviorKind::Forever)
			|| (authored.RepeatBehavior
					== DeclarativeRepeatBehaviorKind::Count
				&& (!std::isfinite(authored.RepeatCount)
					|| authored.RepeatCount <= 0.0))
			|| (authored.RepeatBehavior
					== DeclarativeRepeatBehaviorKind::Duration
				&& authored.RepeatDurationMilliseconds == 0))
			return false;
		simpleDurationForever = false;
		if (!authored.DurationAutomatic)
		{
			simpleDurationMilliseconds = authored.DurationMilliseconds;
			return true;
		}
		simpleDurationMilliseconds = 0;
		for (const auto& animation : animations)
		{
			const auto duration = TimelineActiveDurationMilliseconds(animation);
			if (duration == (std::numeric_limits<unsigned long long>::max)())
			{
				simpleDurationForever = true;
				simpleDurationMilliseconds = 0;
				return true;
			}
			const auto maximum = (std::numeric_limits<unsigned long long>::max)();
			const auto end = animation.BeginTimeMilliseconds > maximum - duration
				? maximum : animation.BeginTimeMilliseconds + duration;
			if (end == maximum)
			{
				simpleDurationForever = true;
				simpleDurationMilliseconds = 0;
				return true;
			}
			simpleDurationMilliseconds = (std::max)(
				simpleDurationMilliseconds, end);
		}
		return true;
	}

	static bool ExtendParallelNaturalDuration(
		const RuntimeTimelineGroup& child,
		unsigned long long& simpleDurationMilliseconds,
		bool& simpleDurationForever) noexcept
	{
		if (simpleDurationForever) return true;
		long double activeDuration = 0.0L;
		if (child.ResolvedSimpleDurationForever
			|| child.Timing.RepeatBehavior
				== DeclarativeRepeatBehaviorKind::Forever)
		{
			simpleDurationForever = true;
			simpleDurationMilliseconds = 0;
			return true;
		}
		if (child.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Duration)
			activeDuration = child.Timing.RepeatDurationMilliseconds;
		else
			activeDuration = static_cast<long double>(
				child.ResolvedSimpleDurationMilliseconds)
				* (child.Timing.AutoReverse ? 2.0L : 1.0L)
				* static_cast<long double>(child.Timing.RepeatCount)
				/ static_cast<long double>(child.Timing.SpeedRatio);
		const auto maximum = (std::numeric_limits<unsigned long long>::max)();
		if (!std::isfinite(activeDuration)
			|| activeDuration >= static_cast<long double>(maximum))
		{
			simpleDurationForever = true;
			simpleDurationMilliseconds = 0;
			return true;
		}
		const auto rounded = static_cast<unsigned long long>(
			std::ceil(activeDuration));
		const auto end = child.Timing.BeginTimeMilliseconds > maximum - rounded
			? maximum : child.Timing.BeginTimeMilliseconds + rounded;
		if (end == maximum)
		{
			simpleDurationForever = true;
			simpleDurationMilliseconds = 0;
			return true;
		}
		simpleDurationMilliseconds = (std::max)(
			simpleDurationMilliseconds, end);
		return true;
	}

	template<typename TAnimations>
	static bool ResolveStoryboardTiming(
		const DeclarativeStoryboardTimingDefinition& authored,
		const TAnimations& animations,
		const std::vector<RuntimeTimelineGroup>& timelineGroups,
		unsigned long long& simpleDurationMilliseconds,
		bool& simpleDurationForever) noexcept
	{
		std::vector<TimelineDefinition> directAnimations;
		try
		{
			directAnimations.reserve(animations.size());
			for (const auto& animation : animations)
				if (animation.TimelineGroupIndex
					== CompiledInteractionInvalidIndex)
					directAnimations.push_back(
						static_cast<const TimelineDefinition&>(animation));
		}
		catch (...)
		{
			return false;
		}
		if (!ResolveStoryboardTiming(authored, directAnimations,
			simpleDurationMilliseconds, simpleDurationForever)) return false;
		if (!authored.DurationAutomatic || simpleDurationForever) return true;
		for (const auto& group : timelineGroups)
			if (group.ParentIndex == CompiledInteractionInvalidIndex
				&& !ExtendParallelNaturalDuration(group,
					simpleDurationMilliseconds, simpleDurationForever)) return false;
		return true;
	}

	/** Pure parent-time projection for one leaf clock at a sampling tick. */
	struct ClockProjection
	{
		unsigned long long ActiveDurationMilliseconds = 0;
		long double ActiveElapsedMilliseconds = 0.0L;
		bool BeforeBegin = true;
		bool ActivePeriodComplete = false;
		bool RootFillStopped = false;
	};

	static ClockProjection ProjectClock(
		const TimelineDefinition& timeline,
		const ClockRootProjection& root,
		const ClockLeafState& leaf) noexcept
	{
		ClockProjection projection;
		if (root.BeforeBegin)
		{
			projection.BeforeBegin = true;
			return projection;
		}
		projection.RootFillStopped = root.FillStopped;
		projection.ActiveDurationMilliseconds =
			TimelineActiveDurationMilliseconds(timeline);
		const auto elapsed = root.ElapsedMilliseconds;
		projection.BeforeBegin = elapsed < timeline.BeginTimeMilliseconds;
		if (projection.BeforeBegin) return projection;
		const auto activeElapsed = elapsed - timeline.BeginTimeMilliseconds;
		const bool completed = leaf.Completed
			&& !(root.ReentersChildren
				&& activeElapsed < static_cast<long double>(
					projection.ActiveDurationMilliseconds));
		projection.ActivePeriodComplete = completed
			|| activeElapsed >= projection.ActiveDurationMilliseconds;
		projection.ActiveElapsedMilliseconds = completed
			? static_cast<long double>(projection.ActiveDurationMilliseconds)
			: (std::min)(activeElapsed, static_cast<long double>(
				projection.ActiveDurationMilliseconds));
		return projection;
	}

	static ClockProjection ProjectAnimationClock(
		const TimelineDefinition& timeline,
		std::span<const RuntimeTimelineGroup> timelineGroups,
		const ClockRootProjection& rootProjection,
		const ClockLeafState& leaf) noexcept
	{
		auto parent = rootProjection;
		std::array<uint32_t, 64> chain{};
		size_t depth = 0;
		auto index = timeline.TimelineGroupIndex;
		while (index != CompiledInteractionInvalidIndex)
		{
			if (index >= timelineGroups.size() || depth >= chain.size())
			{
				ClockProjection invalid;
				invalid.BeforeBegin = true;
				invalid.RootFillStopped = true;
				return invalid;
			}
			chain[depth++] = index;
			index = timelineGroups[index].ParentIndex;
		}
		while (depth > 0)
			parent = ProjectTimelineGroup(
				timelineGroups[chain[--depth]], parent);
		return ProjectClock(timeline, parent, leaf);
	}

	static ClockProjection ProjectAnimationClock(
		const TimelineDefinition& timeline,
		const ActiveClockRoot& root,
		const ClockRootProjection& rootProjection,
		const ClockLeafState& leaf) noexcept
	{
		return ProjectAnimationClock(timeline, root.TimelineGroups,
			rootProjection, leaf);
	}

	static ClockProjection ProjectAnimationAtRootBegin(
		const TimelineDefinition& timeline,
		const ClockLeafState& leaf,
		const DeclarativeStoryboardTimingDefinition& timing,
		unsigned long long resolvedSimpleDurationMilliseconds,
		bool resolvedSimpleDurationForever,
		const std::vector<RuntimeTimelineGroup>& timelineGroups) noexcept
	{
		ActiveClockRoot root;
		root.Timing = timing;
		root.ResolvedSimpleDurationMilliseconds =
			resolvedSimpleDurationMilliseconds;
		root.ResolvedSimpleDurationForever = resolvedSimpleDurationForever;
		return ProjectAnimationClock(timeline, timelineGroups,
			ProjectClockRoot(root, 0), leaf);
	}

	static std::vector<ClockRootProjection> ProjectClockRoots(
		const std::vector<ActiveClockRoot>& roots,
		unsigned long long nowMilliseconds)
	{
		std::vector<ClockRootProjection> result;
		result.reserve(roots.size());
		for (const auto& root : roots)
			result.push_back(ProjectClockRoot(root, nowMilliseconds));
		return result;
	}

	static long double ApplyTimelineAcceleration(
		long double simpleElapsed,
		long double simpleDuration,
		double accelerationRatio,
		double decelerationRatio) noexcept
	{
		if (simpleDuration <= 0.0L) return 0.0L;
		const long double transition = static_cast<long double>(
			accelerationRatio + decelerationRatio);
		if (transition <= 0.0L) return simpleElapsed;
		auto progress = (std::clamp)(
			simpleElapsed / simpleDuration, 0.0L, 1.0L);
		const auto acceleration = static_cast<long double>(accelerationRatio);
		const auto deceleration = static_cast<long double>(decelerationRatio);
		const auto maximumRate = 2.0L / (2.0L - transition);
		if (progress < acceleration)
			progress = maximumRate * progress * progress
			/ (2.0L * acceleration);
		else if (progress <= 1.0L - deceleration)
			progress = maximumRate * (progress - acceleration / 2.0L);
		else
		{
			const auto complement = 1.0L - progress;
			progress = 1.0L - maximumRate * complement * complement
				/ (2.0L * deceleration);
		}
		return (std::clamp)(progress, 0.0L, 1.0L) * simpleDuration;
	}

	static bool ComposeAnimationValue(
		const AnimationPayload& animation,
		const BindingValue& from,
		const BindingValue& foundation,
		const BindingValue& localValue,
		long double completedIterations,
		BindingValue& output)
	{
		if (animation.Kind == DeclarativeAnimationKind::Object)
		{
			output = localValue;
			return true;
		}
		if (MatrixKeyFramesUseDirectValues(animation))
		{
			output = localValue;
			return true;
		}
		BindingValue value = localValue;
		if (animation.IsCumulative && completedIterations > 0.0L)
		{
			BindingValue delta;
			if (!animation.KeyFrames.empty())
				delta = animation.KeyFrames.back().Value;
			else if (!CombineAnimationValues(
				animation, animation.To, from, -1.0L, delta))
				return false;
			BindingValue accumulated;
			if (!CombineAnimationValues(animation, value, delta,
				completedIterations, accumulated)) return false;
			value = std::move(accumulated);
		}
		return AddAnimationValues(
			animation, value, foundation, output);
	}

	static cui::core::Point PathSubtract(
		cui::core::Point left, cui::core::Point right) noexcept
	{
		return { left.x - right.x, left.y - right.y };
	}

	static cui::core::Point PathAdd(
		cui::core::Point left, cui::core::Point right) noexcept
	{
		return { left.x + right.x, left.y + right.y };
	}

	static cui::core::Point PathScale(
		cui::core::Point value, float scale) noexcept
	{
		return { value.x * scale, value.y * scale };
	}

	static float PathDot(
		cui::core::Point left, cui::core::Point right) noexcept
	{
		return left.x * right.x + left.y * right.y;
	}

	static float PathNorm(cui::core::Point value) noexcept
	{
		return std::sqrt(value.x * value.x + value.y * value.y);
	}

	static bool PathUnitize(cui::core::Point& value) noexcept
	{
		float length = PathNorm(value);
		if (!(length >= 0.000001f)) return false;
		length = 1.0f / length;
		value.x *= length;
		value.y *= length;
		return true;
	}

	static cui::core::Point PathVelocity(
		const RuntimePathSegment& segment, float t) noexcept
	{
		const float s = 1.0f - t;
		auto value = PathScale(segment.D1[0], s * s);
		value = PathAdd(value, PathScale(segment.D1[1], s * t));
		return PathAdd(value, PathScale(segment.D1[2], t * t));
	}

	static void PathDerivatives(
		const RuntimePathSegment& segment,
		float t,
		cui::core::Point& velocity,
		cui::core::Point& acceleration) noexcept
	{
		const float s = 1.0f - t;
		velocity = PathVelocity(segment, t);
		acceleration = PathAdd(
			PathScale(segment.D2[0], s), PathScale(segment.D2[1], t));
	}

	static float PathSpeed(
		const RuntimePathSegment& segment, float t) noexcept
	{
		return PathNorm(PathVelocity(segment, t));
	}

	static void PathSpeedAndDerivative(
		const RuntimePathSegment& segment,
		float t,
		float& speed,
		float& derivative) noexcept
	{
		cui::core::Point velocity;
		cui::core::Point acceleration;
		PathDerivatives(segment, t, velocity, acceleration);
		speed = PathNorm(velocity);
		derivative = PathDot(velocity, acceleration);
		if (speed > std::fabs(derivative) * 0.000001)
			derivative /= speed;
		else derivative = 0.0f;
	}

	static float PathCubicLength(
		const RuntimePathSegment& segment, float from, float to) noexcept
	{
		constexpr float samples[] = {
			0.2113248654051875f, 0.7886751345948125f };
		const float first = (1.0f - samples[0]) * from + samples[0] * to;
		const float second = (1.0f - samples[1]) * from + samples[1] * to;
		const float integral = PathSpeed(segment, first)
			+ PathSpeed(segment, second);
		return integral * (to - from) * 0.5f;
	}

	static bool PrepareWpfPath(TimelineDefinition& animation) noexcept
	{
		try
		{
			animation.PreparedPathSegments.clear();
			animation.PathTotalLength = 0.0f;
			if (!animation.Path.Enabled) return animation.PathSegments.empty();
			auto current = animation.Path.Start;
			bool movePending = false;
			for (const auto& source : animation.PathSegments)
			{
				if (source.Kind == DeclarativePathSegmentKind::Move)
				{
					if (animation.PreparedPathSegments.empty() || movePending)
						return false;
					current = source.Point3;
					movePending = true;
					continue;
				}
				RuntimePathSegment segment;
				segment.Kind = source.Kind;
				segment.Points[0] = current;
				segment.Points[1] = source.Point1;
				segment.Points[2] = source.Point2;
				segment.Points[3] = source.Point3;
				segment.BaseLength = animation.PathTotalLength;
				if (source.Kind == DeclarativePathSegmentKind::Line)
				{
					segment.InitialTangent = PathSubtract(source.Point3, current);
					segment.Length = PathNorm(segment.InitialTangent);
					if (segment.Length < 0.000001f) continue;
					segment.InitialTangent = PathScale(
						segment.InitialTangent, 1.0f / segment.Length);
					segment.Lengths[1] = segment.Length;
				}
				else if (source.Kind == DeclarativePathSegmentKind::CubicBezier)
				{
					segment.D1[0] = PathScale(
						PathSubtract(segment.Points[1], segment.Points[0]), 3.0f);
					segment.D1[1] = PathScale(
						PathSubtract(segment.Points[2], segment.Points[1]), 3.0f);
					segment.D1[2] = PathScale(
						PathSubtract(segment.Points[3], segment.Points[2]), 3.0f);
					segment.D2[0] = PathScale(
						PathSubtract(segment.D1[1], segment.D1[0]), 2.0f);
					segment.D2[1] = PathScale(
						PathSubtract(segment.D1[2], segment.D1[1]), 2.0f);
					segment.D3 = PathSubtract(segment.D2[1], segment.D2[0]);
					segment.D1[1] = PathScale(segment.D1[1], 2.0f);
					segment.InitialTangent = segment.D1[0];
					if (!PathUnitize(segment.InitialTangent))
					{
						segment.InitialTangent = PathSubtract(
							segment.Points[2], segment.Points[1]);
						if (!PathUnitize(segment.InitialTangent))
						{
							segment.InitialTangent = PathSubtract(
								segment.Points[3], segment.Points[1]);
							if (!PathUnitize(segment.InitialTangent)) continue;
						}
					}

					segment.Breaks[0] = 0.0f;
					segment.Breaks[1] = 1.0f;
					segment.BreakCount = 2;
					auto acceptBreak = [&](double value)
					{
						if (!(value > 0.01f && value < 1.0f - 0.01f)
							|| segment.BreakCount >= 5) return;
						uint32_t index = 0;
						do ++index; while (index < segment.BreakCount
							&& value > segment.Breaks[index]);
						if ((index != 0
								&& !(value > segment.Breaks[index - 1] + 0.01f))
							|| (index != segment.BreakCount
								&& !(value < segment.Breaks[index] - 0.01f))) return;
						for (uint32_t move = segment.BreakCount; move > index; --move)
							segment.Breaks[move] = segment.Breaks[move - 1];
						segment.Breaks[index] = static_cast<float>(value);
						++segment.BreakCount;
					};
					auto solveSpeedExtremum = [&](double seed, double& root)
					{
						const double from = 0.0;
						const double to = 1.0;
						const double delta = static_cast<float>(0.000001 * 10.0);
						float xMin = segment.Points[0].x;
						float xMax = xMin;
						float yMin = segment.Points[0].y;
						float yMax = yMin;
						for (uint32_t point = 1; point < 4; ++point)
						{
							xMin = (std::min)(xMin, segment.Points[point].x);
							xMax = (std::max)(xMax, segment.Points[point].x);
							yMin = (std::min)(yMin, segment.Points[point].y);
							yMax = (std::max)(yMax, segment.Points[point].y);
						}
						float extent = (std::max)(xMax - xMin, yMax - yMin);
						const float epsilon = extent * extent
							* static_cast<float>(delta);
						bool topClamped = false;
						bool bottomClamped = false;
						double absolute = 0.0;
						root = seed;
						for (int iteration = 1; iteration < 100; ++iteration)
						{
							const auto t = static_cast<float>(root);
							cui::core::Point velocity;
							cui::core::Point acceleration;
							PathDerivatives(segment, t, velocity, acceleration);
							const double value = PathDot(acceleration, velocity);
							const double derivative = PathDot(segment.D3, velocity)
								+ PathDot(acceleration, acceleration);
							absolute = std::fabs(value);
							if (absolute < epsilon) break;
							if (std::fabs(derivative) <= absolute * 0.000001) break;
							const double correction = -value / derivative;
							if (std::fabs(correction) < delta) break;
							root += correction;
							if (root < from)
							{
								root = from;
								if (bottomClamped) break;
								bottomClamped = true;
							}
							else if (root > to)
							{
								root = to;
								if (topClamped) break;
								topClamped = true;
							}
						}
						return absolute < epsilon;
					};
					for (const double seed : { 0.0, 0.5, 1.0 })
					{
						double root = seed;
						if (solveSpeedExtremum(seed, root)) acceptBreak(root);
					}
					if (segment.BreakCount == 2)
					{
						segment.Breaks[4] = segment.Breaks[1];
						segment.Breaks[2] = (segment.Breaks[0]
							+ segment.Breaks[4]) / 2.0f;
						segment.Breaks[1] = (segment.Breaks[0]
							+ segment.Breaks[2]) / 2.0f;
						segment.Breaks[3] = (segment.Breaks[2]
							+ segment.Breaks[4]) / 2.0f;
						segment.BreakCount = 5;
					}
					else if (segment.BreakCount == 3)
					{
						segment.Breaks[4] = segment.Breaks[2];
						segment.Breaks[2] = segment.Breaks[1];
						segment.Breaks[1] = (segment.Breaks[0]
							+ segment.Breaks[2]) / 2.0f;
						segment.Breaks[3] = (segment.Breaks[2]
							+ segment.Breaks[4]) / 2.0f;
						segment.BreakCount = 5;
					}
					for (uint32_t index = 1; index < segment.BreakCount; ++index)
					{
						segment.Mids[index - 1] = (segment.Breaks[index - 1]
							+ segment.Breaks[index]) / 2.0f;
						segment.Lengths[index] = segment.Lengths[index - 1]
							+ PathCubicLength(segment, segment.Breaks[index - 1],
								segment.Mids[index - 1])
							+ PathCubicLength(segment, segment.Mids[index - 1],
								segment.Breaks[index]);
					}
					segment.Length = segment.Lengths[segment.BreakCount - 1];
					if (!(segment.Length > 0.0f) || !std::isfinite(segment.Length))
						continue;
				}
				else return false;
				animation.PathTotalLength += segment.Length;
				animation.PreparedPathSegments.push_back(segment);
				current = source.Point3;
				movePending = false;
			}
			return !movePending && !animation.PreparedPathSegments.empty()
				&& std::isfinite(animation.PathTotalLength)
				&& animation.PathTotalLength > 0.0f;
		}
		catch (...)
		{
			return false;
		}
	}

	struct PathSample final
	{
		cui::core::Point Point;
		cui::core::Point Tangent;
	};

	static float PathParameterFromLength(
		const RuntimePathSegment& segment, float targetLength) noexcept
	{
		if (targetLength <= 0.0f) return 0.0f;
		if (targetLength >= segment.Length) return 1.0f;
		uint32_t span = 0;
		while (targetLength < segment.Lengths[span]) --span;
		while (span < 3 && targetLength > segment.Lengths[span + 1]) ++span;
		double root = segment.Breaks[span];
		double top = segment.Breaks[span + 1];
		double bottom = segment.Breaks[span];
		double absolute = (std::numeric_limits<double>::max)();
		for (int iteration = 1;
			top - bottom > 0.000001 && iteration < 100; ++iteration)
		{
			const uint32_t reference = root > segment.Mids[span]
				? span + 1 : span;
			double value = 0.0;
			double derivative = 0.0;
			for (const float sample : {
				0.2113248654051875f, 0.7886751345948125f })
			{
				const float t = static_cast<float>(root);
				const float at = (1.0f - sample) * segment.Breaks[reference]
					+ sample * t;
				float speed = 0.0f;
				float speedDerivative = 0.0f;
				PathSpeedAndDerivative(
					segment, at, speed, speedDerivative);
				speedDerivative *= sample;
				value += speed;
				derivative += speedDerivative;
			}
			value *= 0.5;
			derivative *= 0.5;
			const double relative = root - segment.Breaks[reference];
			derivative = relative * derivative + value;
			value = segment.Lengths[reference] + relative * value - targetLength;
			absolute = std::fabs(value);
			if (absolute < 0.000001 * segment.Length) break;
			if (value > 0.0) top = root;
			else bottom = root;
			if (std::fabs(derivative) <= absolute * 0.000001)
				root = (bottom + top) / 2.0;
			else
			{
				root -= value / derivative;
				if (root < segment.Breaks[span]
					|| root > segment.Breaks[span + 1])
					root = (bottom + top) / 2.0;
			}
		}
		return static_cast<float>(root);
	}

	static bool SampleWpfPath(
		const AnimationPayload& animation, float fraction, PathSample& output) noexcept
	{
		if (animation.PreparedPathSegments.empty()
			|| !(animation.PathTotalLength > 0.0f)) return false;
		float requested = 0.0f;
		size_t segmentIndex = 0;
		if (fraction <= 0.0f) requested = 0.0f;
		else if (fraction >= 1.0f)
		{
			requested = animation.PathTotalLength;
			segmentIndex = animation.PreparedPathSegments.size() - 1;
		}
		else
		{
			requested = fraction * animation.PathTotalLength;
			while (segmentIndex + 1 < animation.PreparedPathSegments.size()
				&& requested > animation.PreparedPathSegments[segmentIndex].BaseLength
					+ animation.PreparedPathSegments[segmentIndex].Length)
				++segmentIndex;
		}
		const auto& segment = animation.PreparedPathSegments[segmentIndex];
		float localLength = requested - segment.BaseLength;
		if (localLength > segment.Length) localLength = segment.Length;
		if (segment.Kind == DeclarativePathSegmentKind::Line)
		{
			output.Point = localLength == 0.0f
				? segment.Points[0]
				: PathAdd(segment.Points[0],
					PathScale(segment.InitialTangent, localLength));
			output.Tangent = segment.InitialTangent;
			return true;
		}
		const float t = PathParameterFromLength(segment, localLength);
		if (t <= 0.0f)
		{
			output.Point = segment.Points[0];
			output.Tangent = segment.D1[0];
		}
		else if (t >= 1.0f)
		{
			output.Point = segment.Points[3];
			output.Tangent = segment.D1[2];
		}
		else
		{
			const float s = 1.0f - t;
			const float s2 = s * s;
			const float t2 = t * t;
			output.Point = PathScale(segment.Points[0], s2 * s);
			output.Point = PathAdd(output.Point,
				PathScale(segment.Points[1], 3.0f * s2 * t));
			output.Point = PathAdd(output.Point,
				PathScale(segment.Points[2], 3.0f * s * t2));
			output.Point = PathAdd(output.Point,
				PathScale(segment.Points[3], t * t2));
			output.Tangent = PathScale(segment.D1[0], s2);
			output.Tangent = PathAdd(output.Tangent,
				PathScale(segment.D1[1], s * t));
			output.Tangent = PathAdd(output.Tangent,
				PathScale(segment.D1[2], t2));
		}
		if (!PathUnitize(output.Tangent))
			output.Tangent = segment.InitialTangent;
		return true;
	}

	static double PathTangentAngle(cui::core::Point tangent) noexcept
	{
		double angle = std::acos(static_cast<double>(tangent.x))
			* (180.0 / std::numbers::pi_v<double>);
		if (tangent.y < 0.0f) angle = 360.0 - angle;
		return angle;
	}

	static bool EvaluatePathAnimation(
		const AnimationPayload& animation,
		const BindingValue& defaultOrigin,
		double progress,
		long double completedIterations,
		BindingValue& output)
	{
		const auto& path = animation.Path;
		if (!path.Enabled || !std::isfinite(progress)) return false;
		PathSample sample;
		PathSample start;
		PathSample end;
		if (!SampleWpfPath(animation, static_cast<float>(progress), sample)
			|| !SampleWpfPath(animation, 0.0f, start)
			|| !SampleWpfPath(animation, 1.0f, end)) return false;
		const double iteration = static_cast<double>(completedIterations);
		if (animation.Kind == DeclarativeAnimationKind::Double)
		{
			auto valueAt = [&](const PathSample& value)
			{
				switch (path.Source)
				{
				case DeclarativePathAnimationSource::Y:
					return static_cast<double>(value.Point.y);
				case DeclarativePathAnimationSource::Angle:
					return PathTangentAngle(value.Tangent);
				case DeclarativePathAnimationSource::X:
				default: return static_cast<double>(value.Point.x);
				}
			};
			double value = valueAt(sample);
			if (animation.IsCumulative && completedIterations > 0.0L)
				value += (valueAt(end) - valueAt(start)) * iteration;
			if (animation.IsAdditive)
			{
				double origin = 0.0;
				if (!defaultOrigin.TryGetDouble(origin)) return false;
				value += origin;
			}
			if (!std::isfinite(value)) return false;
			if (animation.Metadata
				&& animation.Metadata->ValueKind() == BindingValueKind::Double)
				output = BindingValue(value);
			else
				output = BindingValue(static_cast<float>(value));
			return true;
		}
		if (animation.Kind == DeclarativeAnimationKind::Point)
		{
			double x = sample.Point.x;
			double y = sample.Point.y;
			if (animation.IsCumulative && completedIterations > 0.0L)
			{
				x += static_cast<double>(end.Point.x - start.Point.x) * iteration;
				y += static_cast<double>(end.Point.y - start.Point.y) * iteration;
			}
			if (animation.IsAdditive)
			{
				cui::core::Point origin;
				if (!defaultOrigin.TryGet(origin)) return false;
				x += origin.x;
				y += origin.y;
			}
			if (!std::isfinite(x) || !std::isfinite(y)) return false;
			output = BindingValue(cui::core::Point{
				static_cast<float>(x), static_cast<float>(y) });
			return true;
		}
		if (animation.Kind != DeclarativeAnimationKind::Matrix) return false;
		double angle = path.DoesRotateWithTangent
			? PathTangentAngle(sample.Tangent) : 0.0;
		double x = sample.Point.x;
		double y = sample.Point.y;
		if (completedIterations > 0.0L)
		{
			if (path.IsOffsetCumulative)
			{
				x += static_cast<double>(end.Point.x - start.Point.x) * iteration;
				y += static_cast<double>(end.Point.y - start.Point.y) * iteration;
			}
			if (path.DoesRotateWithTangent && path.IsAngleCumulative)
				angle += (PathTangentAngle(end.Tangent)
					- PathTangentAngle(start.Tangent)) * iteration;
		}
		const double radians = angle * std::numbers::pi_v<double> / 180.0;
		const auto cosine = static_cast<float>(std::cos(radians));
		const auto sine = static_cast<float>(std::sin(radians));
		D2D1_MATRIX_3X2_F value{
			cosine, sine, -sine, cosine,
			static_cast<float>(x), static_cast<float>(y) };
		if (animation.IsAdditive)
		{
			D2D1_MATRIX_3X2_F origin{};
			if (!defaultOrigin.TryGet(origin)) return false;
			const auto left = value;
			value = {
				left._11 * origin._11 + left._12 * origin._21,
				left._11 * origin._12 + left._12 * origin._22,
				left._21 * origin._11 + left._22 * origin._21,
				left._21 * origin._12 + left._22 * origin._22,
				left._31 * origin._11 + left._32 * origin._21 + origin._31,
				left._31 * origin._12 + left._32 * origin._22 + origin._32 };
		}
		if (!IsFiniteMatrix(value)) return false;
		output = BindingValue(value);
		return true;
	}

	static bool Interpolate(
		const AnimationPayload& animation,
		const AnimationLayerValueState& layerValues,
		long double activeElapsedMilliseconds,
		BindingValue& output,
		const BindingValue* composedOrigin = nullptr)
	{
		const BindingValue& from = composedOrigin && layerValues.DynamicFrom
			? *composedOrigin : animation.From;
		const BindingValue& foundation = composedOrigin
			&& layerValues.DynamicFoundation
			? *composedOrigin : layerValues.Foundation;
		const auto activeDurationExact = TimelineActiveDurationExact(animation);
		long double parentElapsed = activeElapsedMilliseconds;
		const bool atActiveBoundary = std::isfinite(activeDurationExact)
			&& parentElapsed >= activeDurationExact;
		if (std::isfinite(activeDurationExact))
			parentElapsed = (std::min)(parentElapsed, activeDurationExact);
		long double simpleElapsed = parentElapsed
			* static_cast<long double>(animation.SpeedRatio);
		const auto simpleDuration = static_cast<long double>(
			animation.DurationMilliseconds);
		long double completedIterations = 0.0L;
		if (simpleDuration > 0.0L)
		{
			const auto repetitionDuration = simpleDuration
				* (animation.AutoReverse ? 2.0L : 1.0L);
			if (!std::isfinite(simpleElapsed)) return false;
			completedIterations = std::floor(simpleElapsed / repetitionDuration);
			auto local = std::fmod(simpleElapsed, repetitionDuration);
			// Interior repetition boundaries begin the next iteration. At the
			// finite active-period boundary, sample the completed repetition so
			// HoldEnd gets To (or From after an auto-reverse), matching WPF.
			if (atActiveBoundary
				&& simpleElapsed > 0.0L
				&& std::fabs(local) < 0.0000001L)
			{
				local = repetitionDuration;
				completedIterations = (std::max)(
					0.0L, completedIterations - 1.0L);
			}
			if (animation.AutoReverse && local > simpleDuration)
				local = repetitionDuration - local;
			simpleElapsed = (std::clamp)(
				local, 0.0L, simpleDuration);
		}
		else simpleElapsed = 0.0L;
		simpleElapsed = ApplyTimelineAcceleration(simpleElapsed, simpleDuration,
			animation.AccelerationRatio, animation.DecelerationRatio);
		if (animation.KeyFrames.empty())
		{
			const double progress = animation.DurationMilliseconds == 0
				? 1.0
				: (std::min)(1.0,
					static_cast<double>(simpleElapsed)
					/ static_cast<double>(animation.DurationMilliseconds));
			if (animation.Path.Enabled)
				return EvaluatePathAnimation(animation, from,
					progress, completedIterations, output);
			BindingValue localValue;
			if (!InterpolateValues(animation.Kind, animation.Metadata,
				ObjectPathUsesFloat(animation.ObjectPath),
				from, animation.To,
				Ease(progress, animation.Easing, animation.EasingMode,
					animation.EasingParameters), localValue))
				return false;
			return ComposeAnimationValue(
				animation, from, foundation, localValue,
				completedIterations, output);
		}

		if (animation.KeySplineParameters.size()
			!= animation.KeyFrames.size()) return false;
		auto keyTimeMilliseconds = [](const RuntimeAnimationKeyFrame& keyFrame)
			{
				return static_cast<long double>(keyFrame.KeyTimeMilliseconds)
					+ static_cast<long double>(
						keyFrame.KeyTimeSubMillisecondTicks) / 10000.0L;
			};
		BindingValue previousValue = from;
		long double previousTime = 0.0L;
		for (size_t keyFrameIndex = 0;
			keyFrameIndex < animation.KeyFrames.size(); ++keyFrameIndex)
		{
			const auto& keyFrame = animation.KeyFrames[keyFrameIndex];
			const auto keyTime = keyTimeMilliseconds(keyFrame);
			if (simpleElapsed < keyTime)
			{
				if (keyFrame.Kind == DeclarativeKeyFrameKind::Discrete)
				{
					return ComposeAnimationValue(animation, from, foundation, previousValue,
						completedIterations, output);
				}
				const auto span = keyTime - previousTime;
				const double segmentProgress = span == 0 ? 1.0
					: static_cast<double>(simpleElapsed - previousTime)
					/ static_cast<double>(span);
				double eased = segmentProgress;
				if (keyFrame.Kind == DeclarativeKeyFrameKind::Easing)
					eased = Ease(segmentProgress,
						keyFrame.Easing, keyFrame.EasingMode,
						keyFrame.EasingParameters);
				else if (keyFrame.Kind == DeclarativeKeyFrameKind::Spline)
					eased = KeySplineProgress(segmentProgress, keyFrame,
						animation.KeySplineParameters[keyFrameIndex]);
				BindingValue localValue;
				if (!InterpolateValues(animation.Kind, animation.Metadata,
					ObjectPathUsesFloat(animation.ObjectPath), previousValue,
					keyFrame.Value, eased, localValue)) return false;
				return ComposeAnimationValue(animation, from, foundation, localValue,
					completedIterations, output);
			}
			previousTime = keyTime;
			previousValue = keyFrame.Value;
		}
		return ComposeAnimationValue(animation, from, foundation, previousValue,
			completedIterations, output);
	}

	static bool Interpolate(
		const CandidateAnimation& animation,
		long double activeElapsedMilliseconds,
		BindingValue& output)
	{
		return Interpolate(animation, animation.LayerValues,
			activeElapsedMilliseconds, output);
	}

	struct AnimationFrameValue
	{
		const AnimationPayload* Animation = nullptr;
		BindingValue Value;
	};

	struct AnimationClockView
	{
		const AnimationPayload* Animation = nullptr;
		const AnimationLayerValueState* Values = nullptr;
		ClockId RootClockId;
	};

	struct AnimationClockLayerStack
	{
		PropertyKey Root;
		uint32_t LayerOffset = 0;
		uint32_t LayerCount = 0;
	};

	static bool BuildAnimationClockLayerStacks(
		const std::vector<AnimationClockView>& animations,
		std::vector<AnimationClockLayerStack>& output,
		std::vector<AnimationClockView>& outputLayers) noexcept
	{
		try
		{
			if (animations.size()
				> (std::numeric_limits<uint32_t>::max)()) return false;
			output.clear();
			output.reserve(animations.size());
			outputLayers.clear();
			std::unordered_map<PropertyKey, size_t,
				PropertyKeyHash, PropertyKeyEqual> stackIndices;
			stackIndices.reserve(animations.size());
			for (const auto& view : animations)
			{
				const auto* animation = view.Animation;
				if (!animation || !animation->Target || !animation->Metadata)
					return false;
				PropertyKey root{ animation->Target,
					PropertyIdentity(animation->Metadata) };
				const auto [position, inserted] = stackIndices.try_emplace(
					root, output.size());
				if (inserted)
					output.push_back({ root, 0, 0 });
				auto& count = output[position->second].LayerCount;
				if (count == (std::numeric_limits<uint32_t>::max)()) return false;
				++count;
			}
			std::vector<uint32_t> cursors(output.size());
			uint32_t offset = 0;
			for (size_t index = 0; index < output.size(); ++index)
			{
				output[index].LayerOffset = offset;
				cursors[index] = offset;
				offset += output[index].LayerCount;
			}
			if (offset != animations.size()) return false;
			outputLayers.resize(animations.size());
			for (const auto& view : animations)
			{
				const auto* animation = view.Animation;
				const PropertyKey root{ animation->Target,
					PropertyIdentity(animation->Metadata) };
				const auto position = stackIndices.find(root);
				if (position == stackIndices.end()
					|| position->second >= cursors.size()) return false;
				const auto layerIndex = cursors[position->second]++;
				if (layerIndex >= outputLayers.size()) return false;
				outputLayers[layerIndex] = view;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	static AnimationClockView CandidateClockView(
		const CandidateAnimation& animation) noexcept
	{
		return { &animation, &animation.LayerValues, animation.RootClockId };
	}

	bool ActiveClockView(
		const ActiveAnimation& animation,
		AnimationClockView& output) const noexcept
	{
		const auto* root = ResolveClockRoot(animation);
		const auto* layer = ResolveAnimationLayer(animation);
		if (!root || !layer) return false;
		output = { &animation, &layer->Values, root->Identity.Instance };
		return true;
	}

	static void ConfigureComposeDynamicValues(
		const RuntimeAnimation& animation,
		AnimationLayerValueState& values) noexcept
	{
		if (animation.Path.Enabled)
		{
			values.DynamicFrom = animation.IsAdditive;
			values.DynamicFoundation = false;
			return;
		}
		if (!animation.KeyFrames.empty())
		{
			if (MatrixKeyFramesUseDirectValues(animation))
			{
				values.DynamicFrom = true;
				values.DynamicFoundation = false;
				return;
			}
			values.DynamicFrom = !animation.IsAdditive;
			values.DynamicFoundation = animation.IsAdditive;
			return;
		}
		const bool hasFrom = animation.From.has_value();
		const bool hasTo = animation.To.has_value();
		const bool hasBy = animation.By.has_value() && !hasTo;
		values.DynamicFrom = !hasFrom && !hasBy;
		values.DynamicFoundation = (!hasFrom && hasBy)
			|| (hasFrom && (hasTo || hasBy) && animation.IsAdditive);
	}

	template<typename TAnimations>
	static std::vector<AnimationClockView> CandidateClockViews(
		const TAnimations& animations)
	{
		std::vector<AnimationClockView> result;
		result.reserve(animations.size());
		for (const auto& animation : animations)
			result.push_back(CandidateClockView(animation));
		return result;
	}

	bool ActiveClockViews(std::vector<AnimationClockView>& output) const
	{
		output.clear();
		output.reserve(ClockNodes.Leaves.size());
		for (const auto& animation : ClockNodes.Leaves)
		{
			AnimationClockView view;
			if (!ActiveClockView(animation, view)) return false;
			output.push_back(view);
		}
		return true;
	}

	static DependencyPropertyValueSource AnimationValueSource(
		const AnimationPayload&) noexcept
	{
		return DependencyPropertyValueSource::Animation;
	}

	bool TryReadAnimationFrameRoot(
		Control* target,
		const DependencyPropertyMetadata* metadata,
		DependencyPropertyValueSource source,
		BindingValue& output)
	{
		if (!target || !metadata) return false;
		return target->TryGetPropertyValue(
			metadata->Property(), source, output)
			|| metadata->TryGet(*target, output);
	}

	struct AnimationPropertyFrame
	{
		Control* Target = nullptr;
		const DependencyPropertyMetadata* Metadata = nullptr;
		DependencyPropertyValueSource Source =
			DependencyPropertyValueSource::Animation;
		BindingValue Value;
		bool Initialized = false;
		bool ObjectPathReady = false;
	};

	bool AppendAnimationLayerFrame(
		AnimationPropertyFrame& object,
		const AnimationPayload& animation,
		const BindingValue& value)
	{
		if (!animation.Target || !animation.Metadata) return false;
		const auto source = AnimationValueSource(animation);
		if (!object.Target)
		{
			object.Target = animation.Target;
			object.Metadata = animation.Metadata;
			object.Source = source;
		}
		else if (object.Target != animation.Target
			|| !object.Metadata
			|| &object.Metadata->Property()
				!= &animation.Metadata->Property()
			|| object.Source != source) return false;
		if (!animation.ObjectPath)
		{
			object.Value = value;
			object.Initialized = true;
			object.ObjectPathReady = false;
			return true;
		}
		if (!object.Initialized)
		{
			if (!TryReadAnimationFrameRoot(animation.Target,
				animation.Metadata, source, object.Value)
				|| !NormalizeObjectPathRoot(animation, object.Value)) return false;
			object.Initialized = true;
			object.ObjectPathReady = true;
		}
		else if (!object.ObjectPathReady)
		{
			if (!NormalizeObjectPathRoot(animation, object.Value)) return false;
			object.ObjectPathReady = true;
		}
		return TryWriteObjectPathMember(
			object.Value, *animation.ObjectPath, value);
	}

	bool CommitAnimationPropertyFrames(
		std::vector<AnimationPropertyFrame>& objects)
	{
		std::vector<PropertySnapshot> snapshots;
		snapshots.reserve(objects.size());
		for (const auto& object : objects)
		{
			if (!object.Initialized || !object.Metadata) return false;
			CapturePropertySnapshot({ object.Target,
				PropertyIdentity(object.Metadata) }, object.Source, snapshots);
		}
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed) (void)RestoreSnapshots(snapshots);
			} };
		for (auto& object : objects)
			if (!object.Target->TrySetPropertyValue(
				object.Metadata->Property(), object.Value,
				object.Source)) return false;
		committed = true;
		return true;
	}

	bool ApplyAnimationFrame(
		const std::vector<AnimationFrameValue>& values)
	{
		std::vector<AnimationPropertyFrame> objects;
		objects.reserve(values.size());
		std::unordered_map<PropertyKey, size_t,
			PropertyKeyHash, PropertyKeyEqual> objectIndices;
		objectIndices.reserve(values.size());
		for (const auto& frame : values)
		{
			const auto* animation = frame.Animation;
			if (!animation || !animation->Target || !animation->Metadata)
				return false;
			const auto source = AnimationValueSource(*animation);
			const PropertyKey root{
				animation->Target, PropertyIdentity(animation->Metadata) };
			const auto [position, inserted] = objectIndices.try_emplace(
				root, objects.size());
			if (inserted)
			{
				objects.push_back({ animation->Target,
					animation->Metadata, source });
			}
			if (!AppendAnimationLayerFrame(
				objects[position->second], *animation, frame.Value)) return false;
		}
		return CommitAnimationPropertyFrames(objects);
	}

	bool ApplyCommittedAnimationFrame(
		const std::vector<AnimationFrameValue>& values)
	{
		// Full one-to-one validation runs when a dirty arena rebuilds and through
		// the testing invariant. A clean frame only follows stable owner tokens.
		if (ClockNodesDirty || values.size() != ClockNodes.Leaves.size()
			|| (AnimationLayerStacks.empty()
				!= ClockNodes.Leaves.empty())) return false;
		std::vector<AnimationPropertyFrame> objects;
		objects.reserve(AnimationLayerStacks.size());
		for (const auto& stack : AnimationLayerStacks)
		{
			AnimationPropertyFrame object;
			if (stack.LayerOffset > AnimationLayers.size()
				|| stack.LayerCount
					> AnimationLayers.size() - stack.LayerOffset) return false;
			for (size_t layerIndex = 0;
				layerIndex < stack.LayerCount; ++layerIndex)
			{
				const auto& layer = AnimationLayers[
					stack.LayerOffset + layerIndex];
				const auto* slot = ClockNodes.Resolve(
					layer.Owner, ClockNodeKind::Leaf);
				if (!slot || slot->PayloadIndex >= values.size()) return false;
				const auto& animation = ClockNodes.Leaves[slot->PayloadIndex];
				const auto& frame = values[slot->PayloadIndex];
				if (!frame.Animation) continue;
				if (frame.Animation != &animation
					|| !AppendAnimationLayerFrame(
						object, animation, frame.Value)) return false;
			}
			if (object.Initialized) objects.push_back(std::move(object));
		}
		return CommitAnimationPropertyFrames(objects);
	}

	bool ReleaseStoppedAnimationValues(
		const std::vector<AnimationClockView>& stoppingAnimations,
		const std::vector<AnimationClockView>& animations,
		const std::vector<ActiveClockRoot>& roots,
		unsigned long long nowMilliseconds,
		std::span<const ClockRootProjection> suppliedRootProjections = {})
	{
		std::vector<AnimationClockLayerStack> layerStacks;
		std::vector<AnimationClockView> layers;
		if (!BuildAnimationClockLayerStacks(
			animations, layerStacks, layers)) return false;
		std::unordered_map<PropertyKey, size_t,
			PropertyKeyHash, PropertyKeyEqual> layerStackIndices;
		layerStackIndices.reserve(layerStacks.size());
		for (size_t index = 0; index < layerStacks.size(); ++index)
			if (!layerStackIndices.emplace(
				layerStacks[index].Root, index).second) return false;
		std::vector<ClockRootProjection> ownedRootProjections;
		if (suppliedRootProjections.size() != roots.size())
		{
			ownedRootProjections = ProjectClockRoots(roots, nowMilliseconds);
			suppliedRootProjections = ownedRootProjections;
		}
		for (const auto& stoppingView : stoppingAnimations)
		{
			const auto* stopping = stoppingView.Animation;
			const auto* stoppingValues = stoppingView.Values;
			if (!stopping || !stoppingValues
				|| !stopping->Target || !stopping->Metadata) continue;
			const auto* stoppingRoot = FindClockRoot(
				roots, stoppingView.RootClockId);
			if (!stoppingRoot) return false;
			const auto source = AnimationValueSource(*stopping);
			const PropertyKey stoppingKey{
				stopping->Target, PropertyIdentity(stopping->Metadata) };
			const auto stack = layerStackIndices.find(stoppingKey);
			const auto siblingAffectsRoot = stack != layerStackIndices.end()
				&& std::any_of(
				layers.begin()
					+ layerStacks[stack->second].LayerOffset,
				layers.begin() + layerStacks[stack->second].LayerOffset
					+ layerStacks[stack->second].LayerCount,
				[&](const auto& candidateView)
				{
					const auto* candidate = candidateView.Animation;
					if (!candidate) return false;
					const auto* candidateRoot = FindClockRoot(
						roots, candidateView.RootClockId);
					if (candidate == stopping
						|| !candidateRoot
						|| IsVisualStateClock(*candidateRoot)
						!= IsVisualStateClock(*stoppingRoot)
						|| candidate->Target != stopping->Target
						|| PropertyIdentity(candidate->Metadata)
							!= PropertyIdentity(stopping->Metadata)) return false;
					const auto rootIndex = static_cast<size_t>(
						candidateRoot - roots.data());
					if (rootIndex >= suppliedRootProjections.size()) return false;
					const auto projection = ProjectAnimationClock(
						*candidate, *candidateRoot,
						suppliedRootProjections[rootIndex], *candidate);
					// A live delayed clock owns its Base frame from Begin until its
					// active period starts. Treat it as a sibling so replacing or
					// stopping another clock cannot clear that staged value.
					if (projection.BeforeBegin) return true;
					if (candidate->Completed)
						return candidate->FillBehavior
							== DeclarativeTimelineFillBehavior::HoldEnd
							|| candidate->Handoff
								== DeclarativeHandoffBehavior::SnapshotAndReplace;
					return !projection.ActivePeriodComplete || candidate->FillBehavior
						== DeclarativeTimelineFillBehavior::HoldEnd;
				});
			if (!stopping->ObjectPath)
			{
				if (!siblingAffectsRoot && stopping->Target->HasPropertyValue(
					stopping->Metadata->Property(), source))
					if (!stopping->Target->ClearPropertyValue(
						stopping->Metadata->Property(), source)) return false;
				continue;
			}
			if (!siblingAffectsRoot)
			{
				if (stopping->Target->HasPropertyValue(
					stopping->Metadata->Property(), source)
					&& !stopping->Target->ClearPropertyValue(
						stopping->Metadata->Property(), source)) return false;
				continue;
			}
			BindingValue root;
			if (!(stopping->Target->TryGetPropertyValue(
				stopping->Metadata->Property(), source, root)
				|| stopping->Metadata->TryGet(*stopping->Target, root))) return false;
			if (!NormalizeObjectPathRoot(*stopping, root)
				|| !TryWriteObjectPathMember(
					root, *stopping->ObjectPath, stoppingValues->Base)
				|| !stopping->Target->TrySetPropertyValue(
					stopping->Metadata->Property(), root, source)) return false;
		}
		return true;
	}

	bool HasActiveAnimations() const noexcept
	{
		return std::any_of(ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](const auto& animation)
			{
				const auto* root = ResolveClockRoot(animation);
				return root
					&& !ProjectClockRoot(
						*root, root->State.LastTick).ActivePeriodComplete
					&& !root->State.Paused && !root->State.Stopped
					&& root->State.InteractiveSpeedRatio > 0.0;
			})
			|| std::any_of(ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
				[](const auto& root)
				{ return HasPendingRootControl(root.State); })
			|| std::any_of(Groups.begin(), Groups.end(),
				[](const auto& group) { return group.Pending.has_value(); });
	}

	void CollectCompositionAnimationTargets(
		std::vector<Control*>& transformTargets,
		std::vector<Control*>& opacityTargets) const
	{
		const auto* renderTransform = &Control::RenderTransformProperty();
		const auto* opacity = &Control::OpacityProperty();
		for (const auto& stack : AnimationLayerStacks)
		{
			if (!stack.Root.Target) continue;
			if (stack.Root.Property == renderTransform)
				transformTargets.push_back(stack.Root.Target);
			else if (stack.Root.Property == opacity)
				opacityTargets.push_back(stack.Root.Target);
		}
	}

	size_t AnimationLeafCountForTesting() const noexcept
	{
		return ClockNodes.Leaves.size();
	}

	size_t AnimationRootClockCountForTesting() const noexcept
	{
		return ClockNodes.Roots.size();
	}

	std::optional<unsigned long long> RootActiveDuration(
		const ActiveClockRoot& root) const noexcept
	{
		if (root.ResolvedSimpleDurationForever
			|| root.Timing.RepeatBehavior
				== DeclarativeRepeatBehaviorKind::Forever)
			return std::nullopt;
		const auto exact = root.Timing.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Duration
			? static_cast<long double>(
				root.Timing.RepeatDurationMilliseconds)
			: static_cast<long double>(
				root.ResolvedSimpleDurationMilliseconds)
				* (root.Timing.AutoReverse ? 2.0L : 1.0L)
				* static_cast<long double>(root.Timing.RepeatCount)
				/ static_cast<long double>(root.Timing.SpeedRatio);
		if (!std::isfinite(exact)
			|| exact >= static_cast<long double>(
				(std::numeric_limits<unsigned long long>::max)()))
			return std::nullopt;
		return static_cast<unsigned long long>(std::ceil(exact));
	}

	static long double RootParentOffsetForSeek(
		const ActiveClockRoot& root,
		unsigned long long rootLocalOffsetMilliseconds) noexcept
	{
		// WPF SeekOrigin.BeginTime measures the offset in the target Clock's
		// local-time coordinate. BeginTime remains parent time; authored
		// SpeedRatio only maps the requested local offset back to that parent.
		return static_cast<long double>(root.Timing.BeginTimeMilliseconds)
			+ static_cast<long double>(rootLocalOffsetMilliseconds)
				/ static_cast<long double>(root.Timing.SpeedRatio);
	}

	enum class RootTimingPhase : unsigned char
	{
		Active,
		Filling,
		Stopped,
	};

	struct RootTimingSnapshot final
	{
		ClockIdentity Identity;
		RootTimingPhase Phase = RootTimingPhase::Stopped;
		double GlobalSpeed = 0.0;
		bool Evaluated = false;
		bool PendingSeek = false;
		bool PendingSpeed = false;
		bool PendingPause = false;
		bool PendingResume = false;
		bool PendingStop = false;
		bool PendingRemove = false;
	};

	RootTimingPhase TimingPhaseAt(
		const ActiveClockRoot& root,
		unsigned long long tick) const noexcept
	{
		if (root.State.Stopped) return RootTimingPhase::Stopped;
		const auto duration = RootActiveDuration(root);
		if (duration && ProjectClockRoot(
			root, tick).ActivePeriodComplete)
			return RootTimingPhase::Filling;
		return RootTimingPhase::Active;
	}

	double TimingGlobalSpeedAt(
		const ActiveClockRoot& root,
		RootTimingPhase phase) const noexcept
	{
		return phase != RootTimingPhase::Active || root.State.Paused
			? 0.0 : root.State.InteractiveSpeedRatio;
	}

	std::vector<RootTimingSnapshot> CaptureRootTimingSnapshots() const
	{
		std::vector<RootTimingSnapshot> result;
		result.reserve(ClockNodes.Roots.size());
		for (const auto& root : ClockNodes.Roots)
		{
			RootTimingSnapshot snapshot;
			snapshot.Identity = root.Identity;
			snapshot.Evaluated = root.State.HasEvaluated;
			snapshot.Phase = snapshot.Evaluated
				? TimingPhaseAt(root, root.State.LastTick)
				: RootTimingPhase::Stopped;
			snapshot.GlobalSpeed = snapshot.Evaluated
				? TimingGlobalSpeedAt(root, snapshot.Phase) : 0.0;
			snapshot.PendingSeek =
				root.State.PendingSeekOffsetMilliseconds.has_value();
			snapshot.PendingSpeed = root.State.PendingSpeedRatio.has_value();
			snapshot.PendingPause = root.State.PendingPause;
			snapshot.PendingResume = root.State.PendingResume;
			snapshot.PendingStop = root.State.PendingStop;
			snapshot.PendingRemove = root.State.PendingRemove;
			result.push_back(std::move(snapshot));
		}
		return result;
	}

	static DeclarativeClockOwnerKind PublicClockOwnerKind(
		ClockOwnerKind kind) noexcept
	{
		switch (kind)
		{
		case ClockOwnerKind::ComponentStoryboard:
			return DeclarativeClockOwnerKind::ComponentStoryboard;
		case ClockOwnerKind::StyleTrigger:
			return DeclarativeClockOwnerKind::StyleTrigger;
		case ClockOwnerKind::CompiledInteractionStoryboard:
			return DeclarativeClockOwnerKind::CompiledInteractionStoryboard;
		case ClockOwnerKind::VisualStateGroup:
		default:
			return DeclarativeClockOwnerKind::VisualStateGroup;
		}
	}

	static void AppendTimingEvent(
		std::vector<DeclarativeClockTimingEventArgs>& events,
		const ClockIdentity& identity,
		DeclarativeClockTimingEventKind kind)
	{
		events.push_back({
			kind,
			identity.Instance.Value,
			PublicClockOwnerKind(identity.Storyboard.Owner.Kind),
			identity.Storyboard.Owner.Value,
			identity.Storyboard.Definition.Value });
	}

	void FinalizeRootTimingEvents(
		const std::vector<RootTimingSnapshot>& before,
		unsigned long long nowMilliseconds,
		std::vector<DeclarativeClockTimingEventArgs>& events)
	{
		events.clear();
		for (const auto& snapshot : before)
		{
			const auto found = std::find_if(
				ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
				[&](const auto& root)
				{ return root.Identity.Instance == snapshot.Identity.Instance; });
			const bool anyPending = snapshot.PendingSeek
				|| snapshot.PendingSpeed || snapshot.PendingPause
				|| snapshot.PendingResume || snapshot.PendingStop
				|| snapshot.PendingRemove;
			bool timeInvalidated = false;
			bool speedInvalidated = false;
			bool stateInvalidated = false;
			bool completed = false;
			bool removeRequested = snapshot.PendingRemove;

			if (snapshot.PendingRemove)
			{
				timeInvalidated = speedInvalidated = stateInvalidated = true;
				completed = true;
			}
			else if (found != ClockNodes.Roots.end())
			{
				const auto nextPhase = TimingPhaseAt(*found, nowMilliseconds);
				const auto nextSpeed = TimingGlobalSpeedAt(*found, nextPhase);
				const bool participates = !snapshot.Evaluated || anyPending
					|| snapshot.Phase == RootTimingPhase::Active
					|| nextPhase == RootTimingPhase::Active;
				timeInvalidated = participates;
				speedInvalidated = !snapshot.Evaluated
					|| snapshot.GlobalSpeed != nextSpeed
					|| snapshot.PendingSeek || snapshot.PendingSpeed
					|| snapshot.PendingPause || snapshot.PendingResume
					|| snapshot.PendingStop;
				stateInvalidated = !snapshot.Evaluated
					|| snapshot.Phase != nextPhase
					|| snapshot.PendingSeek || snapshot.PendingStop;
				completed = nextPhase == RootTimingPhase::Filling
					&& snapshot.Phase != RootTimingPhase::Filling;
			}

			if (timeInvalidated) AppendTimingEvent(events, snapshot.Identity,
				DeclarativeClockTimingEventKind::CurrentTimeInvalidated);
			if (speedInvalidated) AppendTimingEvent(events, snapshot.Identity,
				DeclarativeClockTimingEventKind::CurrentGlobalSpeedInvalidated);
			if (stateInvalidated) AppendTimingEvent(events, snapshot.Identity,
				DeclarativeClockTimingEventKind::CurrentStateInvalidated);
			if (completed) AppendTimingEvent(events, snapshot.Identity,
				DeclarativeClockTimingEventKind::Completed);
			if (removeRequested) AppendTimingEvent(events, snapshot.Identity,
				DeclarativeClockTimingEventKind::RemoveRequested);
		}
		for (auto& root : ClockNodes.Roots)
		{
			root.State.LastTick = nowMilliseconds;
			root.State.HasEvaluated = true;
		}
	}

	void PublishRootTimingEvents(
		std::vector<DeclarativeClockTimingEventArgs> events)
	{
		if (!Owner || events.empty()) return;
		const ControlWeakReference ownerLifetime(Owner);
		for (const auto& event : events)
		{
			auto* owner = ownerLifetime.Get();
			if (!owner) return;
			cui::framework::EventAccess::Raise(
				owner->OnStoryboardTimingEvent, owner, event);
		}
	}

	ActiveClockRoot* SingleControllableRootForTesting() noexcept
	{
		if (ClockNodes.Roots.size() != 1u || ClockNodes.Leaves.empty())
			return nullptr;
		auto& root = ClockNodes.Roots.front();
		return root.State.ControlLookupAvailable ? &root : nullptr;
	}

	bool SeekSingleRootForTesting(unsigned long long offsetMilliseconds)
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		root->State.PendingSeekOffsetMilliseconds =
			RootParentOffsetForSeek(*root, offsetMilliseconds);
		// WPF ordinary Seek re-enables a stopped root and cancels a Stop that
		// has not reached the timing tick yet. The cached observation remains
		// unchanged until that tick.
		root->State.PendingStop = false;
		return true;
	}

	bool PauseSingleRootForTesting()
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		if (root->State.PendingResume)
			root->State.PendingResume = false;
		else if (!root->State.Paused)
			root->State.PendingPause = true;
		return true;
	}

	bool ResumeSingleRootForTesting()
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		if (root->State.PendingPause)
			root->State.PendingPause = false;
		else if (root->State.Paused)
			root->State.PendingResume = true;
		return true;
	}

	bool SetSpeedRatioSingleRootForTesting(double ratio)
	{
		auto* root = SingleControllableRootForTesting();
		if (!root || !std::isfinite(ratio) || ratio < 0.0) return false;
		root->State.PendingSpeedRatio = ratio;
		return true;
	}

	bool SkipSingleRootToFillForTesting()
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		const auto duration = RootActiveDuration(*root);
		if (!duration) return false;
		root->State.PendingSeekOffsetMilliseconds =
			static_cast<long double>(root->Timing.BeginTimeMilliseconds)
				+ static_cast<long double>(*duration);
		root->State.PendingStop = false;
		return true;
	}

	bool StopSingleRootForTesting()
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		root->State.PendingStop = true;
		root->State.PendingSeekOffsetMilliseconds.reset();
		return true;
	}

	bool RemoveSingleRootForTesting()
	{
		auto* root = SingleControllableRootForTesting();
		if (!root) return false;
		root->State.PendingRemove = true;
		root->State.PendingStop = true;
		root->State.PendingSeekOffsetMilliseconds.reset();
		root->State.ControlLookupAvailable = false;
		return true;
	}

	bool SeekSingleRootAlignedForTesting(unsigned long long offsetMilliseconds)
	{
		auto* controllableRoot = SingleControllableRootForTesting();
		if (!controllableRoot) return false;
		const auto now = NowMilliseconds();
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations)) return false;
		auto previousRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed)
					RestoreActiveAnimationTransaction(
						previousAnimations, previousRoots,
						snapshots, previousApplying);
				else
					Applying = previousApplying;
			} };
		auto& root = *controllableRoot;
		root.State.StartTick = now;
		root.State.SeekOffsetMilliseconds =
			RootParentOffsetForSeek(root, offsetMilliseconds);
		root.State.PauseTick = root.State.Paused ? now : 0;
		root.State.Stopped = false;
		root.State.LastTick = now;
		root.State.HasEvaluated = true;
		root.State.PendingStop = false;
		for (auto& animation : ClockNodes.Leaves)
			if (animation.Parent == root.NodeId)
			{
				animation.Completed = false;
				std::fill(animation.KeySplineParameters.begin(),
					animation.KeySplineParameters.end(), 0.0);
			}
		if (!ApplyRetainedAnimationFrame(now)) return false;
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	std::optional<DeclarativeClockObservation>
	QuerySingleRootForTesting() const noexcept
	{
		if (ClockNodes.Roots.size() != 1u || ClockNodes.Leaves.empty())
			return std::nullopt;
		const auto& root = ClockNodes.Roots.front();
		if (!root.State.ControlLookupAvailable) return std::nullopt;
		if (root.State.Stopped)
			return DeclarativeClockObservation{
				DeclarativeClockState::Stopped,
				std::nullopt, std::nullopt, std::nullopt };
		const auto duration = root.ResolvedSimpleDurationForever
			? (std::numeric_limits<unsigned long long>::max)()
			: root.ResolvedSimpleDurationMilliseconds;
		const auto projection = ProjectClockRoot(root, root.State.LastTick);
		if (projection.BeforeBegin || projection.FillStopped)
			return DeclarativeClockObservation{
				DeclarativeClockState::Stopped,
				std::nullopt, std::nullopt, std::nullopt };
		const auto finiteDuration = RootActiveDuration(root);
		const auto outerDuration = finiteDuration.value_or(
			(std::numeric_limits<unsigned long long>::max)());
		if (outerDuration == 0) return DeclarativeClockObservation{
			DeclarativeClockState::Filling, 0ull, 1.0, 0.0 };
		const auto elapsed = projection.ElapsedMilliseconds;
		const bool filling = projection.ActivePeriodComplete;
		const auto current = elapsed;
		std::optional<double> progress;
		if (duration != (std::numeric_limits<unsigned long long>::max)())
			progress = static_cast<double>(current)
				/ static_cast<double>(duration);
		return DeclarativeClockObservation{
			filling ? DeclarativeClockState::Filling
				: DeclarativeClockState::Active,
			current,
			progress,
			filling || root.State.Paused
				? 0.0 : root.State.InteractiveSpeedRatio
					* root.Timing.SpeedRatio
					* (projection.Reversed ? -1.0 : 1.0) };
	}

	size_t AnimationClockNodeCountForTesting() const noexcept
	{
		return ClockNodes.OccupiedCount();
	}

	size_t AnimationLayerStackCountForTesting() const noexcept
	{
		return AnimationLayerStacks.size();
	}

	size_t AnimationLayerCountForTesting() const noexcept
	{
		return AnimationLayers.size();
	}

	size_t AnimationLayerMaxDepthForTesting() const noexcept
	{
		size_t result = 0;
		for (const auto& stack : AnimationLayerStacks)
			result = (std::max)(result,
				static_cast<size_t>(stack.LayerCount));
		return result;
	}

	size_t RootClockChildCountForTesting(size_t rootIndex) const noexcept
	{
		if (rootIndex >= ClockNodes.Roots.size()) return 0;
		const auto* slot = ClockNodes.Resolve(
			ClockNodes.Roots[rootIndex].NodeId, ClockNodeKind::Root);
		return slot ? slot->ChildCount : 0;
	}

	uint64_t SingleRootClockIdForTesting() const noexcept
	{
		return ClockNodes.Roots.size() == 1
			? ClockNodes.Roots.front().Identity.Instance.Value : 0;
	}

	uint64_t SingleRootClockNodeTokenForTesting() const noexcept
	{
		if (ClockNodes.Roots.size() != 1
			|| !ClockNodes.Roots.front().NodeId.IsValid()) return 0;
		const auto id = ClockNodes.Roots.front().NodeId;
		return (static_cast<uint64_t>(id.Generation) << 32)
			| (static_cast<uint64_t>(id.Index) + 1u);
	}

	bool ClockIdentityValidForTesting() const noexcept
	{
		if (ClockNodesDirty || !ClockNodeArenaValid()
			|| !AnimationLayerStacksValid()) return false;
		for (size_t index = 0; index < ClockNodes.Roots.size(); ++index)
		{
			const auto& root = ClockNodes.Roots[index];
			if (root.Identity.Instance.Value == 0) return false;
			if (IsVisualStateClock(root)
				&& root.Identity.Storyboard.Owner.Value >= Groups.size())
				return false;
			for (size_t previous = 0; previous < index; ++previous)
			{
				const auto& other = ClockNodes.Roots[previous];
				if (root.Identity.Instance == other.Identity.Instance
					|| root.Identity.Storyboard
						== other.Identity.Storyboard) return false;
			}
			const bool referencedByLeaf = std::any_of(
				ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
				[&](const auto& animation)
				{ return animation.Parent == root.NodeId; });
			const bool referencedByPending = std::any_of(
				Groups.begin(), Groups.end(), [&](const auto& group)
				{ return group.Pending
					&& group.Pending->RootClockId == root.Identity.Instance; });
			if (!referencedByLeaf && !referencedByPending) return false;
		}
		for (const auto& animation : ClockNodes.Leaves)
			if (!ResolveClockRoot(animation)) return false;
		for (size_t groupIndex = 0; groupIndex < Groups.size(); ++groupIndex)
		{
			const auto& pending = Groups[groupIndex].Pending;
			if (!pending) continue;
			const auto* root = FindClockRoot(pending->RootClockId);
			if (!root || root->Identity.Storyboard.Owner
				!= VisualStateOwner(groupIndex)) return false;
			for (const auto& animation : ClockNodes.Leaves)
				if (IsOwnedByVisualStateGroup(animation, groupIndex)
					&& animation.Parent != root->NodeId) return false;
		}
		return true;
	}

	bool CommitPendingRootControls(
		unsigned long long nowMilliseconds,
		bool& committedAny)
	{
		committedAny = std::any_of(
			ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
			[](const auto& root)
			{ return HasPendingRootControl(root.State); });
		if (!committedAny) return true;

		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations)) return false;
		auto previousRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed)
					RestoreActiveAnimationTransaction(
						previousAnimations, previousRoots,
						snapshots, previousApplying);
				else
					Applying = previousApplying;
			} };

		std::vector<StoryboardClockKey> removals;
		std::vector<StoryboardClockKey> stops;
		for (const auto& root : ClockNodes.Roots)
			if (root.State.PendingRemove)
				removals.push_back(root.Identity.Storyboard);
			else if (root.State.PendingStop)
				stops.push_back(root.Identity.Storyboard);

		for (const auto& storyboard : removals)
			if (!CommitRemoveEventStoryboard(
				storyboard, nowMilliseconds)) return false;
		for (const auto& storyboard : stops)
		{
			auto found = std::find_if(ClockNodes.Roots.begin(),
				ClockNodes.Roots.end(), [&](const auto& root)
				{ return root.Identity.Storyboard == storyboard; });
			if (found == ClockNodes.Roots.end()) continue;
			found->State.PendingStop = false;
			if (!CommitStopEventStoryboard(
				storyboard, nowMilliseconds)) return false;
		}

		bool directStateChanged = false;
		for (auto& root : ClockNodes.Roots)
		{
			auto currentAnchor = ProjectInteractiveRootElapsed(
				root.State, nowMilliseconds);
			const bool sought =
				root.State.PendingSeekOffsetMilliseconds.has_value();
			if (sought)
			{
				root.State.StartTick = nowMilliseconds;
				root.State.SeekOffsetMilliseconds =
					*root.State.PendingSeekOffsetMilliseconds;
				root.State.PendingSeekOffsetMilliseconds.reset();
				root.State.Stopped = false;
				if (root.State.Paused)
					root.State.PauseTick = nowMilliseconds;
				for (auto& animation : ClockNodes.Leaves)
					if (animation.Parent == root.NodeId)
					{
						animation.Completed = false;
						std::fill(animation.KeySplineParameters.begin(),
							animation.KeySplineParameters.end(), 0.0);
				}
				directStateChanged = true;
				currentAnchor = root.State.SeekOffsetMilliseconds;
			}
			const bool speedChanged = root.State.PendingSpeedRatio.has_value()
				&& (!root.State.Stopped || sought);
			if (speedChanged)
			{
				root.State.StartTick = nowMilliseconds;
				root.State.SeekOffsetMilliseconds = currentAnchor;
				root.State.InteractiveSpeedRatio =
					*root.State.PendingSpeedRatio;
				root.State.PendingSpeedRatio.reset();
				if (root.State.Paused)
					root.State.PauseTick = nowMilliseconds;
				directStateChanged = true;
			}
			if (root.State.PendingPause)
			{
				root.State.PendingPause = false;
				if (!root.State.Paused)
				{
					root.State.Paused = true;
					root.State.PauseTick = nowMilliseconds;
					directStateChanged = true;
				}
			}
			if (root.State.PendingResume)
			{
				root.State.PendingResume = false;
				if (root.State.Paused)
				{
					if (!sought && !speedChanged)
					{
						const auto pausedFor = nowMilliseconds
							>= root.State.PauseTick
							? nowMilliseconds - root.State.PauseTick : 0;
						root.State.StartTick = SaturatingAdd(
							root.State.StartTick, pausedFor);
					}
					root.State.Paused = false;
					root.State.PauseTick = 0;
					directStateChanged = true;
				}
			}
		}
		if (directStateChanged
			&& !ApplyRetainedAnimationFrame(nowMilliseconds)) return false;
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool AdvanceAnimations(unsigned long long nowMilliseconds)
	{
		LastAdvanceFailed = false;
		if (!SynchronizeClockNodeArena())
		{
			LastAdvanceFailed = true;
			return false;
		}
		std::vector<RootTimingSnapshot> timingBefore;
		std::vector<DeclarativeClockTimingEventArgs> timingEvents;
		try
		{
			timingBefore = CaptureRootTimingSnapshots();
			if (timingBefore.size()
				> (std::numeric_limits<size_t>::max)() / 5u)
				throw std::length_error("Clock timing event batch is too large.");
			timingEvents.reserve(timingBefore.size() * 5u);
		}
		catch (...)
		{
			LastAdvanceFailed = true;
			return false;
		}
		bool committedRootControl = false;
		if (!CommitPendingRootControls(
			nowMilliseconds, committedRootControl))
		{
			LastAdvanceFailed = true;
			return false;
		}
		const auto rootProjections = ProjectClockRoots(
			ClockNodes.Roots, nowMilliseconds);
		if (!HasActiveAnimations())
		{
			FinalizeRootTimingEvents(
				timingBefore, nowMilliseconds, timingEvents);
			PublishRootTimingEvents(std::move(timingEvents));
			return committedRootControl;
		}
		const bool hadActive = true;
		auto fail = [&]() noexcept
			{
				LastAdvanceFailed = true;
				return false;
			};
		const bool previousApplying = Applying;
		Applying = true;
		ControlScopeExit restoreApplying{ [&]
			{ Applying = previousApplying; } };
		std::vector<AnimationFrameValue> frameValues;
		std::vector<AnimationClockView> stoppingAnimations;
		std::vector<ActiveAnimation*> reenteredAnimations;
		std::vector<BindingValue> composedValues(AnimationExactKeyCount);
		std::vector<bool> composedHasFrame(AnimationExactKeyCount, false);
		frameValues.resize(ClockNodes.Leaves.size());
		if (!VisitClockLeavesByRoot(rootProjections,
			[&](auto& root, const auto& rootProjection, auto& animation,
				uint32_t payloadIndex)
			{
			const auto projection = ProjectAnimationClock(
				animation, root, rootProjection, animation);
			const auto* layer = ResolveAnimationLayer(animation);
			if (!layer) return false;
			if (layer->ExactKeyIndex >= composedValues.size()) return false;
			auto& composed = composedValues[layer->ExactKeyIndex];
			if (composed.Kind() == BindingValueKind::Empty)
				composed = layer->Values.Base;
			if (root.State.Stopped)
			{
				if (!composedHasFrame[layer->ExactKeyIndex])
				{
					BindingValue stoppedBase;
					if (!TryReadBaseAnimationValue(
						animation, stoppedBase)) return false;
					frameValues[payloadIndex] = {
						&animation, std::move(stoppedBase) };
					composed = frameValues[payloadIndex].Value;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			if (projection.BeforeBegin)
			{
				if (layer->Handoff
					== DeclarativeHandoffBehavior::SnapshotAndReplace)
				{
					frameValues[payloadIndex] = {
						&animation, layer->Values.Snapshot };
					composed = layer->Values.Snapshot;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			if (projection.RootFillStopped)
			{
				if (layer->Handoff == DeclarativeHandoffBehavior::Compose)
					return true;
				frameValues[payloadIndex] = {
					&animation, layer->Values.Snapshot };
				composed = layer->Values.Snapshot;
				composedHasFrame[layer->ExactKeyIndex] = true;
				return true;
			}
			BindingValue value;
			const bool reentered = animation.Completed
				&& !projection.ActivePeriodComplete;
			if (reentered) reenteredAnimations.push_back(&animation);
			if (animation.Completed && !reentered && animation.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop)
			{
				if (layer->Handoff == DeclarativeHandoffBehavior::Compose)
					return true;
				value = layer->Values.Snapshot;
			}
			else if (!Interpolate(animation, layer->Values,
				projection.ActiveElapsedMilliseconds, value,
				layer->Handoff == DeclarativeHandoffBehavior::Compose
					? &composed : nullptr)) return false;
			frameValues[payloadIndex] = { &animation, std::move(value) };
			composed = frameValues[payloadIndex].Value;
			composedHasFrame[layer->ExactKeyIndex] = true;
			if ((!animation.Completed || reentered) && animation.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop
				&& projection.ActivePeriodComplete)
				stoppingAnimations.push_back(
					{ &animation, &layer->Values, root.Identity.Instance });
			return true;
		})) return fail();
		// The ordinary frame path only stages DP writes in the explicit
		// per-property layer stacks. BeforeBegin leaves keep an empty frame so
		// their layer remains owned without overwriting a live sibling.
		// Deep clock/property snapshots are needed on a FillBehavior=Stop boundary
		// and when a repeating root re-enters a previously completed child. Both
		// paths mutate retained clock state after the frame commit.
		std::vector<PropertySnapshot> stopSnapshots;
		std::vector<CandidateAnimation> stopAnimations;
		std::vector<ActiveClockRoot> stopRoots;
		bool stopCommitted = stoppingAnimations.empty()
			&& reenteredAnimations.empty();
		if (!stopCommitted)
		{
			CaptureActiveAnimationSnapshots(ClockNodes.Leaves, stopSnapshots);
			if (!SnapshotClockCandidates(stopAnimations)) return fail();
			stopRoots = ClockNodes.Roots;
		}
		ControlScopeExit rollbackStop{ [&]
			{
				if (!stopCommitted)
					RestoreActiveAnimationTransaction(
						stopAnimations, stopRoots,
						stopSnapshots, previousApplying);
			} };
		if (std::exchange(FailNextFrameCommitForTesting, false)) return fail();
		if (!ApplyCommittedAnimationFrame(frameValues)) return fail();
		for (auto* animation : reenteredAnimations)
			if (animation) animation->Completed = false;
		std::vector<AnimationClockView> activeClockViews;
		if (!ActiveClockViews(activeClockViews)
			|| !ReleaseStoppedAnimationValues(
			stoppingAnimations, activeClockViews,
			ClockNodes.Roots, nowMilliseconds, rootProjections)) return fail();
		const auto animationCountBeforePrune = ClockNodes.Leaves.size();
		ClockNodes.Leaves.erase(std::remove_if(
			ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](auto& animation)
			{
				const auto* root = ResolveClockRoot(animation);
				if (!root)
				{
					LastAdvanceFailed = true;
					return false;
				}
				const auto rootIndex = static_cast<size_t>(
					root - ClockNodes.Roots.data());
				if (rootIndex >= rootProjections.size())
				{
					LastAdvanceFailed = true;
					return false;
				}
				const auto projection = ProjectAnimationClock(
					animation, *root, rootProjections[rootIndex], animation);
				if (!projection.ActivePeriodComplete) return false;
				if (IsVisualStateClock(*root))
				{
					const auto groupIndex =
						root->Identity.Storyboard.Owner.Value;
					if (groupIndex < Groups.size())
					{
						const auto& pending = Groups[
							static_cast<size_t>(groupIndex)].Pending;
						if (pending && pending->RootClockId
							== root->Identity.Instance) return false;
					}
				}
				animation.Completed = true;
				return false;
			}), ClockNodes.Leaves.end());
		if (ClockNodes.Leaves.size() != animationCountBeforePrune)
			ClockNodesDirty = true;
		if (LastAdvanceFailed) return fail();
		if (!stoppingAnimations.empty()
			&& !ApplyRetainedAnimationFrame(nowMilliseconds)) return fail();
		stopCommitted = true;
		Applying = previousApplying;
		for (size_t groupIndex = 0; groupIndex < Groups.size(); ++groupIndex)
		{
			auto& group = Groups[groupIndex];
			if (!group.Pending || nowMilliseconds < group.Pending->EndTick) continue;
			const auto targetState = group.Pending->TargetState;
			RuntimeState targetStorage;
			const auto* target = ResolveState(
				groupIndex, targetState, targetStorage, nullptr);
			if (!target)
			{
				LastAdvanceFailed = true;
				continue;
			}
			auto transitionProperties = group.Pending->Properties;
			auto previousPending = group.Pending;
			std::vector<CandidateAnimation> previousAnimations;
			if (!SnapshotClockCandidates(previousAnimations))
			{
				LastAdvanceFailed = true;
				continue;
			}
			auto previousRoots = ClockNodes.Roots;
			const auto previousState = group.CurrentState;
			bool committed = false;
			ControlScopeExit rollbackCompletion{ [&]
				{
					if (committed) return;
					group.CurrentState = previousState;
					group.Pending = std::move(previousPending);
					(void)CommitClockCandidates(
						std::move(previousAnimations),
						std::move(previousRoots));
				} };
			group.Pending.reset();
			ClockNodes.Leaves.erase(std::remove_if(
				ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
				[&](const auto& animation)
				{ return IsOwnedByVisualStateGroup(animation, groupIndex); }),
				ClockNodes.Leaves.end());
			if (!GoToImmediate(groupIndex, targetState, nullptr,
				nowMilliseconds, true, &transitionProperties))
			{
				LastAdvanceFailed = true;
				continue;
			}
			committed = true;
		}
		FinalizeRootTimingEvents(
			timingBefore, nowMilliseconds, timingEvents);
		PruneClockRoots();
		PublishClockRootRegistry();
		PublishRootTimingEvents(std::move(timingEvents));
		return hadActive;
	}

	void ClearAppliedValues(bool includeEventStoryboards = true) noexcept
	{
		if (!Owner) return;
		Applying = true;
		std::vector<PropertyKey> stateValuesCleared;
		std::vector<PropertyKey> animationValuesCleared;
		auto clearOnce = [](std::vector<PropertyKey>& cleared,
			const PropertyKey& key, DependencyPropertyValueSource source)
			{
				if (!key.Target || !key.Property
					|| std::any_of(cleared.begin(), cleared.end(),
					[&](const auto& existing) { return SameProperty(existing, key); }))
					return;
				cleared.push_back(key);
				(void)key.Target->ClearPropertyValue(*key.Property, source);
			};
		for (const auto& animation : ClockNodes.Leaves)
			if (includeEventStoryboards || IsVisualStateClock(animation))
				clearOnce(animationValuesCleared,
					{ animation.Target, PropertyIdentity(animation.Metadata) },
					DependencyPropertyValueSource::Animation);
		for (size_t groupIndex = 0; groupIndex < Groups.size(); ++groupIndex)
		{
			const auto& group = Groups[groupIndex];
			if (group.Pending)
				for (const auto& key : group.Pending->Properties)
					clearOnce(animationValuesCleared, key,
						DependencyPropertyValueSource::Animation);
			if (!group.CurrentState || *group.CurrentState >= StateCount(groupIndex))
				continue;
			if (const auto* compiledGroup = CompiledGroupAt(groupIndex))
			{
				if (!CompiledInteractions) continue;
				const auto& instance = *CompiledInteractions;
				const auto absolute = static_cast<size_t>(compiledGroup->States.Offset)
					+ *group.CurrentState;
				if (absolute >= instance.Program.States.size()) continue;
				const auto& state = instance.Program.States[absolute];
				auto clearOperand = [&](uint32_t operandIndex,
					DependencyPropertyValueSource source)
					{
						if (operandIndex >= instance.Program.PropertyOperands.size())
							return;
						const auto& operand =
							instance.Program.PropertyOperands[operandIndex];
						if (operand.TargetSlot >= instance.Targets.size()) return;
						PropertyKey key{ instance.Targets[operand.TargetSlot],
							operand.Property.Identity() };
						if (source == DependencyPropertyValueSource::VisualState)
							clearOnce(stateValuesCleared, key, source);
						else
							clearOnce(animationValuesCleared, key, source);
					};
				if (ValidCompiledRange(state.Setters, instance.Program.Setters.size()))
					for (uint32_t offset = 0; offset < state.Setters.Count; ++offset)
						clearOperand(instance.Program.Setters[
							state.Setters.Offset + offset].OperandIndex,
							DependencyPropertyValueSource::VisualState);
				if (ValidCompiledRange(
					state.Animations, instance.Program.Animations.size()))
					for (uint32_t offset = 0; offset < state.Animations.Count; ++offset)
						clearOperand(instance.Program.Animations[
							state.Animations.Offset + offset].OperandIndex,
							DependencyPropertyValueSource::Animation);
				continue;
			}
#if CUI_ENABLE_DYNAMIC_XAML
			for (const auto& setter : group.States[*group.CurrentState].Setters)
				clearOnce(stateValuesCleared,
					{ setter.Target, PropertyIdentity(setter.Metadata) },
					DependencyPropertyValueSource::VisualState);
			for (const auto& animation : group.States[*group.CurrentState].Animations)
				clearOnce(animationValuesCleared,
					{ animation.Target, PropertyIdentity(animation.Metadata) },
					DependencyPropertyValueSource::Animation);
#endif
		}
		#if CUI_ENABLE_DYNAMIC_XAML
		if (includeEventStoryboards)
			for (const auto& storyboard : EventStoryboards)
				for (const auto& animation : storyboard.Animations)
					clearOnce(animationValuesCleared,
						{ animation.Target, PropertyIdentity(animation.Metadata) },
						DependencyPropertyValueSource::Animation);
		#endif
		Applying = false;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	bool StateMatches(const RuntimeState& state) const
	{
		if (state.Conditions.empty()) return false;
		for (const auto& condition : state.Conditions)
		{
			BindingValue actual;
			if (!condition.Metadata
				|| !condition.Metadata->TryGet(*Owner, actual)
				|| !condition.Metadata->ValuesEqual(actual, condition.Value))
				return false;
		}
		return true;
	}
#endif

	size_t EvaluateState(size_t groupIndex) const
	{
		if (const auto* compiled = CompiledGroupAt(groupIndex))
		{
			for (size_t index = 0; index < compiled->States.Count; ++index)
				if (CompiledStateMatches(groupIndex, index)) return index;
			return compiled->FallbackStateIndex;
		}
	#if CUI_ENABLE_DYNAMIC_XAML
		if (groupIndex >= Groups.size()) return 0;
		const auto& group = Groups[groupIndex];
		for (size_t index = 0; index < group.States.size(); ++index)
			if (!group.States[index].Conditions.empty()
				&& StateMatches(group.States[index])) return index;
		return group.FallbackState;
	#else
		return 0;
	#endif
	}

	__declspec(noinline) bool RestoreSnapshots(
		const std::vector<PropertySnapshot>& snapshots) noexcept
	{
		bool restored = true;
		for (const auto& snapshot : snapshots)
		{
			if (!snapshot.Key.Target || !snapshot.Key.Property) continue;
			if (snapshot.Value)
				restored = snapshot.Key.Target->TrySetPropertyValue(
					*snapshot.Key.Property, *snapshot.Value,
					snapshot.Source) && restored;
			else if (snapshot.Key.Target->HasPropertyValue(
				*snapshot.Key.Property, snapshot.Source))
				restored = snapshot.Key.Target->ClearPropertyValue(
					*snapshot.Key.Property, snapshot.Source) && restored;
		}
		return restored;
	}

	__declspec(noinline) void CapturePropertySnapshot(
		const PropertyKey& key,
		DependencyPropertyValueSource source,
		std::vector<PropertySnapshot>& snapshots) const
	{
		if (!key.Target || !key.Property
			|| std::any_of(snapshots.begin(), snapshots.end(),
				[&](const auto& existing)
				{
					return existing.Source == source
						&& SameProperty(existing.Key, key);
				})) return;
		PropertySnapshot snapshot;
		snapshot.Key = key;
		snapshot.Source = source;
		BindingValue value;
		if (key.Target->TryGetPropertyValue(*key.Property, source, value))
			snapshot.Value = std::move(value);
		snapshots.push_back(std::move(snapshot));
	}

	__declspec(noinline) void CaptureActiveAnimationSnapshots(
		const std::vector<ActiveAnimation>& animations,
		std::vector<PropertySnapshot>& snapshots) const
	{
		snapshots.reserve(snapshots.size() + animations.size());
		for (const auto& animation : animations)
			CapturePropertySnapshot(
				{ animation.Target, PropertyIdentity(animation.Metadata) },
				DependencyPropertyValueSource::Animation, snapshots);
	}

	__declspec(noinline) void RestoreActiveAnimationTransaction(
		std::vector<CandidateAnimation>& savedAnimations,
		std::vector<ActiveClockRoot>& savedRoots,
		const std::vector<PropertySnapshot>& snapshots,
		bool previousApplying) noexcept
	{
		Applying = true;
		if (!CommitClockCandidates(
			std::move(savedAnimations), std::move(savedRoots)))
			LastAdvanceFailed = true;
		(void)RestoreSnapshots(snapshots);
		Applying = previousApplying;
		PublishClockRootRegistry();
	}

	bool GoToImmediate(
		size_t groupIndex,
		size_t stateIndex,
		std::wstring* outError,
		std::optional<unsigned long long> requestedStartTick,
		bool force,
		const std::vector<PropertyKey>* transitionPropertiesToClear)
	{
		if (groupIndex >= Groups.size()
			|| stateIndex >= StateCount(groupIndex))
		{
			if (outError) *outError = L"视觉状态索引无效。";
			return false;
		}
		auto& group = Groups[groupIndex];
		if (!force && group.CurrentState && *group.CurrentState == stateIndex)
		{
			if (outError) outError->clear();
			return true;
		}
		RuntimeStateFootprint previousStorage;
		RuntimeState nextStorage;
		const RuntimeStateFootprint* previous = nullptr;
		if (group.CurrentState)
		{
			if (!TryBuildStateFootprint(groupIndex, *group.CurrentState,
				previousStorage, outError)) return false;
			previous = &previousStorage;
		}
		const auto* nextState = ResolveState(
			groupIndex, stateIndex, nextStorage, outError);
		if (!nextState) return false;
		const auto& next = *nextState;
		ClockIdentity clockIdentity;
		if (!TryAllocateClockIdentity(
			VisualStateClockKey(groupIndex, stateIndex), clockIdentity))
		{
			if (outError) *outError = L"视觉状态 ClockId 已耗尽。";
			return false;
		}

		std::vector<PropertyKey> affected;
		auto addAffected = [&](Control* target,
			const DependencyPropertyMetadata* metadata)
			{
				PropertyKey key{ target, PropertyIdentity(metadata) };
				if (!key.Target || !key.Property) return;
				if (std::none_of(affected.begin(), affected.end(),
					[&](const auto& existing) { return SameProperty(existing, key); }))
					affected.push_back(std::move(key));
			};
		if (previous)
		{
			for (const auto& setter : previous->Setters)
				addAffected(setter.Target, setter.Metadata);
			for (const auto& animation : previous->Animations)
				addAffected(animation.Target, animation.Metadata);
		}
		for (const auto& setter : next.Setters)
			addAffected(setter.Target, setter.Metadata);
		for (const auto& animation : next.Animations)
			addAffected(animation.Target, animation.Metadata);
		if (transitionPropertiesToClear)
			for (const auto& key : *transitionPropertiesToClear)
				if (key.Target && key.Property
					&& std::none_of(affected.begin(), affected.end(),
						[&](const auto& existing)
						{ return SameProperty(existing, key); }))
					affected.push_back(key);

		std::vector<PropertySnapshot> snapshots;
		snapshots.reserve(affected.size() * 2);
		for (const auto& key : affected)
			for (const auto source : {
				DependencyPropertyValueSource::VisualState,
				DependencyPropertyValueSource::Animation })
				CapturePropertySnapshot(key, source, snapshots);

		unsigned long long startTick = requestedStartTick.value_or(0);
		std::vector<CandidateAnimation> pendingAnimations;
		pendingAnimations.reserve(next.Animations.size());
		for (const auto& animation : next.Animations)
		{
			BindingValue current;
			BindingValue base;
			if (!TryReadAnimationSnapshotAndBase(animation, current, base))
			{
				if (outError) *outError = L"视觉状态动画无法捕获快照/基础值："
					+ animation.Metadata->Name();
				return false;
			}
			BindingValue from;
			BindingValue to;
			BindingValue foundation;
			if (!ResolveAnimationEndpoints(
				animation, current, base, from, to, foundation))
			{
				if (outError) *outError = L"视觉状态动画无法解析 From/To/By："
					+ animation.Metadata->Name();
				return false;
			}
			CandidateAnimation candidate;
			static_cast<TimelineDefinition&>(candidate) =
				static_cast<const TimelineDefinition&>(animation);
			candidate.RootClockId = clockIdentity.Instance;
			candidate.Target = animation.Target;
			candidate.Metadata = animation.Metadata;
			candidate.LayerValues.Snapshot = std::move(current);
			candidate.LayerValues.Base = std::move(base);
			candidate.LayerValues.Foundation = std::move(foundation);
			candidate.From = std::move(from);
			candidate.To = std::move(to);
			candidate.ObjectPath = animation.ObjectPath;
			candidate.KeySplineParameters.assign(
				candidate.KeyFrames.size(), 0.0);
			pendingAnimations.push_back(std::move(candidate));
		}

		auto stateHasSetter = [](const auto& state,
			Control* target, const DependencyProperty* property)
			{
				return std::any_of(state.Setters.begin(), state.Setters.end(),
					[&](const auto& candidate)
					{
						return candidate.Target == target
							&& PropertyIdentity(candidate.Metadata) == property;
					});
			};
		auto stateHasAnimation = [](const auto& state,
			Control* target, const DependencyProperty* property)
			{
				return std::any_of(state.Animations.begin(), state.Animations.end(),
					[&](const auto& candidate)
					{
						return candidate.Target == target
							&& PropertyIdentity(candidate.Metadata) == property;
					});
			};

		const bool animationsEnabled = Owner->AreSystemAnimationsEnabled();
		std::vector<CandidateAnimation> candidateAnimations;
		if (!SnapshotClockCandidates(candidateAnimations))
		{
			if (outError) *outError = L"视觉状态动画候选快照失败。";
			return false;
		}
		candidateAnimations.erase(std::remove_if(
			candidateAnimations.begin(), candidateAnimations.end(),
			[&](const auto& animation)
			{
				const auto* root = FindClockRoot(animation.RootClockId);
				return (root && root->Identity.Storyboard.Owner
					== VisualStateOwner(groupIndex))
					|| ConflictsAnyAnimationLayer(
						animation, pendingAnimations);
			}),
			candidateAnimations.end());
		auto candidateRoots = ClockNodes.Roots;
		EraseClockRootsByOwner(candidateRoots, VisualStateOwner(groupIndex));
		unsigned long long rootSimpleDuration = 0;
		bool rootSimpleDurationForever = false;
		if (!ResolveStoryboardTiming(next.StoryboardTiming, pendingAnimations,
			next.TimelineGroups,
			rootSimpleDuration, rootSimpleDurationForever))
		{
			if (outError) *outError = L"视觉状态 Storyboard 根时序尚不受支持。";
			return false;
		}
		for (auto& animation : pendingAnimations)
		{
			const bool active = animationsEnabled
				&& (animation.TimelineGroupIndex
						!= CompiledInteractionInvalidIndex
					|| animation.BeginTimeMilliseconds > 0
					|| TimelineActiveDurationMilliseconds(animation) > 0);
			if (active)
				candidateAnimations.push_back(animation);
			else
			{
				animation.Completed = true;
				candidateAnimations.push_back(animation);
			}
		}
		const bool previousApplying = Applying;
		Applying = true;
		ControlScopeExit restoreApplying{ [&]
			{ Applying = previousApplying; } };
		bool success = true;
		if (previous)
		{
			for (const auto& key : affected)
			{
				if (!key.Target || !key.Property) continue;
				if (stateHasSetter(*previous, key.Target, key.Property)
					&& !stateHasSetter(next, key.Target, key.Property)
					&& key.Target->HasPropertyValue(
						*key.Property, DependencyPropertyValueSource::VisualState)
					&& !key.Target->ClearPropertyValue(
						*key.Property,
						DependencyPropertyValueSource::VisualState))
				{
					success = false;
					break;
				}
				if (stateHasAnimation(*previous, key.Target, key.Property)
					&& key.Target->HasPropertyValue(
						*key.Property, DependencyPropertyValueSource::Animation)
					&& !key.Target->ClearPropertyValue(
						*key.Property, DependencyPropertyValueSource::Animation))
				{
					success = false;
					break;
				}
			}
		}
		if (success)
		{
			for (const auto& setter : next.Setters)
				if (!setter.Target || !setter.Metadata
					|| !setter.Target->TrySetPropertyValue(
					setter.Metadata->Property(), setter.Value,
					DependencyPropertyValueSource::VisualState))
				{
					success = false;
					break;
				}
		}
		if (success)
		{
			std::vector<AnimationFrameValue> initialValues;
			initialValues.reserve(pendingAnimations.size());
			for (const auto& animation : pendingAnimations)
			{
				const auto activeDuration =
					TimelineActiveDurationMilliseconds(animation);
				const bool active = animationsEnabled
					&& (animation.TimelineGroupIndex
							!= CompiledInteractionInvalidIndex
						|| animation.BeginTimeMilliseconds > 0
						|| activeDuration > 0);
				BindingValue value;
				if (animationsEnabled)
				{
					const auto projection = ProjectAnimationAtRootBegin(
						animation, animation, next.StoryboardTiming,
						rootSimpleDuration, rootSimpleDurationForever,
						next.TimelineGroups);
					if (projection.BeforeBegin || projection.RootFillStopped)
						value = animation.LayerValues.Snapshot;
					else if (projection.ActivePeriodComplete
						&& animation.FillBehavior
							== DeclarativeTimelineFillBehavior::Stop)
						value = animation.LayerValues.Snapshot;
					else if (!Interpolate(animation,
						projection.ActiveElapsedMilliseconds, value))
					{
						success = false;
						break;
					}
				}
				else if (!active && animation.FillBehavior
					== DeclarativeTimelineFillBehavior::Stop)
					value = animation.LayerValues.Snapshot;
				else if (!Interpolate(animation,
					active ? 0 : activeDuration, value))
				{
					success = false;
					break;
				}
				initialValues.push_back({ &animation, std::move(value) });
			}
			if (success) success = ApplyAnimationFrame(initialValues);
			if (success)
			{
				startTick = requestedStartTick.value_or(NowMilliseconds());
				if (!InstallClockRoot(candidateRoots, clockIdentity, startTick,
					next.StoryboardTiming, rootSimpleDuration,
					rootSimpleDurationForever,
					next.TimelineGroups)) success = false;
				std::vector<AnimationClockView> stopped;
				// A stopped child retains the SnapshotAndReplace snapshot until
				// the owning Storyboard is explicitly stopped or replaced.
				const auto pendingViews = CandidateClockViews(pendingAnimations);
				if (success && !ReleaseStoppedAnimationValues(
					stopped, pendingViews, candidateRoots,
					animationsEnabled ? startTick
					: (std::numeric_limits<unsigned long long>::max)()))
					success = false;
				PruneCandidateRoots(candidateRoots, candidateAnimations);
			}
		}
		if (success && transitionPropertiesToClear
			&& !ClearTransitionOnlyProperties(
				*transitionPropertiesToClear, next)) success = false;
		if (!success)
		{
			(void)RestoreSnapshots(snapshots);
			if (outError) *outError = L"视觉状态 Setter/Storyboard 无法事务性应用。";
			return false;
		}

		const auto oldStateToken = previous
			? previous->Token : VisualStateToken{};
#if CUI_ENABLE_DYNAMIC_XAML
		const auto oldState = previous ? previous->Name : std::wstring{};
#endif
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations))
		{
			(void)RestoreSnapshots(snapshots);
			if (outError) *outError = L"视觉状态动画回滚快照失败。";
			return false;
		}
		auto previousRoots = ClockNodes.Roots;
		if (!CommitClockCandidates(
			std::move(candidateAnimations), std::move(candidateRoots))
			|| !ApplyRetainedAnimationFrame(startTick))
		{
			RestoreActiveAnimationTransaction(
				previousAnimations, previousRoots,
				snapshots, previousApplying);
			if (outError) *outError = L"视觉状态动画时钟无法重组。";
			return false;
		}
		group.CurrentState = stateIndex;
		// The implicit clock begins when the state transaction is committed,
		// after initial frame composition. Otherwise materialization work between
		// capturing the presentation clock and returning from GoToVisualState
		// leaks into
		// the first externally advanced frame. Transition completion supplies an
		// explicit tick and intentionally keeps that shared timeline origin.
		if (!requestedStartTick)
		{
			startTick = NowMilliseconds();
			if (auto* root = FindClockRoot(clockIdentity.Instance))
				root->State.StartTick = startTick;
		}
		Applying = previousApplying;
		PublishClockRootRegistry();
		if (std::any_of(ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](const auto& animation)
			{ return IsOwnedByVisualStateGroup(animation, groupIndex); }))
			Owner->RefreshDeclarativeAnimationWindowScheduling();
		if (!SuppressStateChangedEvents && oldStateToken != next.Token)
		{
			DeclarativeVisualStateChangedEventArgs args;
			args.Group = GroupTokenAt(groupIndex);
			args.OldStateToken = oldStateToken;
			args.NewStateToken = next.Token;
#if CUI_ENABLE_DYNAMIC_XAML
			args.GroupName = group.Name;
			args.OldState = oldState;
			args.NewState = next.Name;
#endif
			cui::framework::EventAccess::Raise(
				Owner->OnVisualStateChanged, Owner, args);
		}
		if (outError) outError->clear();
		return true;
	}

	static unsigned long long SaturatingAdd(
		unsigned long long left,
		unsigned long long right) noexcept
	{
		const auto maximum = (std::numeric_limits<unsigned long long>::max)();
		return right > maximum - left ? maximum : left + right;
	}

	static bool SameAnimationTarget(
		const RuntimeAnimation& left,
		const RuntimeAnimation& right) noexcept
	{
		if (left.Target != right.Target
			|| PropertyIdentity(left.Metadata)
				!= PropertyIdentity(right.Metadata)) return false;
		return ObjectPathIdentity(left.ObjectPath)
			== ObjectPathIdentity(right.ObjectPath);
	}

	static bool SameAnimationTarget(
		const RuntimeAnimationFootprint& left,
		const RuntimeAnimation& right) noexcept
	{
		return left.Target == right.Target
			&& PropertyIdentity(left.Metadata) == PropertyIdentity(right.Metadata)
			&& left.ObjectPathIdentity == ObjectPathIdentity(right.ObjectPath);
	}

	static bool StateAnimatesProperty(
		const RuntimeState& state,
		const PropertyKey& key) noexcept
	{
		return std::any_of(state.Animations.begin(), state.Animations.end(),
			[&](const auto& animation)
			{
				return animation.Target == key.Target
					&& PropertyIdentity(animation.Metadata) == key.Property;
			});
	}

	template<typename TAnimation>
	bool NormalizeObjectPathRoot(
		const TAnimation& animation,
		BindingValue& root) const
	{
		if (!AsBrushPath(animation.ObjectPath)
			&& !AsBrushTransformPath(animation.ObjectPath)) return true;
		cui::drawing::Brush brush;
		if (!root.TryGet(brush)) return false;
		if (brush.Kind == cui::drawing::BrushKind::None
			&& PropertyIdentity(animation.Metadata)
				== &Control::ForegroundProperty())
		{
			if (!animation.Target) return false;
			root = BindingValue(cui::drawing::MakeSolidColorBrush(
				animation.Target->RendererForegroundColor));
		}
		return true;
	}

	bool TryReadAnimationValue(
		const RuntimeAnimation& animation,
		BindingValue& output) const
	{
		if (!animation.Target || !animation.Metadata) return false;
		BindingValue root;
		if (!animation.Metadata->TryGet(*animation.Target, root)) return false;
		if (!animation.ObjectPath)
		{
			output = std::move(root);
			return true;
		}
		if (!NormalizeObjectPathRoot(animation, root)) return false;
		return TryReadObjectPathMember(
			root, *animation.ObjectPath, output);
	}

	template<typename TAnimation>
	bool TryReadBaseAnimationValue(
		const TAnimation& animation,
		BindingValue& output) const
	{
		if (!animation.Target || !animation.Metadata) return false;
		Control::EffectiveValueEntry candidate;
		const auto stored = animation.Target->_propertyValues.find(
			&animation.Metadata->Property());
		if (stored != animation.Target->_propertyValues.end())
			candidate = stored->second;
		const int animationIndex = StoredPropertySourceIndex(
			DependencyPropertyValueSource::Animation);
		if (animationIndex >= 0)
			candidate.Slots[static_cast<size_t>(animationIndex)].Reset();
		BindingValue root;
		DependencyPropertyValueSource ignoredSource =
			DependencyPropertyValueSource::Default;
		if (!animation.Target->TryEvaluateEffectivePropertyValue(
			*animation.Metadata, candidate, root, ignoredSource)) return false;
		if (!animation.ObjectPath)
		{
			output = std::move(root);
			return true;
		}
		if (!NormalizeObjectPathRoot(animation, root)) return false;
		return TryReadObjectPathMember(
			root, *animation.ObjectPath, output);
	}

	template<typename TLeft, typename TRight>
	static bool SameAnimationLayerKey(
		const TLeft& left,
		const TRight& right) noexcept
	{
		return left.Target == right.Target
			&& PropertyIdentity(left.Metadata)
				== PropertyIdentity(right.Metadata)
			&& left.ObjectPath.has_value() == right.ObjectPath.has_value()
			&& ObjectPathIdentity(left.ObjectPath)
				== ObjectPathIdentity(right.ObjectPath);
	}

	template<typename TAnimation, typename TAnimations>
	static bool ConflictsAnyAnimationLayer(
		const TAnimation& animation,
		const TAnimations& candidates) noexcept
	{
		return std::any_of(candidates.begin(), candidates.end(),
			[&](const auto& candidate)
			{ return SameAnimationLayerKey(animation, candidate); });
	}

	template<typename TAnimation>
	const AnimationLayerRecord* FindTopAnimationLayer(
		const TAnimation& key) const noexcept
	{
		for (size_t index = AnimationLayers.size(); index-- > 0;)
		{
			const auto& layer = AnimationLayers[index];
			const auto* slot = ClockNodes.Resolve(
				layer.Owner, ClockNodeKind::Leaf);
			if (!slot || slot->PayloadIndex >= ClockNodes.Leaves.size())
				continue;
			const auto& animation = ClockNodes.Leaves[slot->PayloadIndex];
			if (animation.NodeId == layer.Owner
				&& SameAnimationLayerKey(animation, key)) return &layer;
		}
		return nullptr;
	}

	bool TryReadAnimationSnapshotAndBase(
		const RuntimeAnimation& animation,
		BindingValue& snapshot,
		BindingValue& base) const
	{
		if (!TryReadAnimationValue(animation, snapshot)) return false;
		if (const auto* layer = FindTopAnimationLayer(animation))
		{
			base = layer->Values.Base;
			return true;
		}
		if (!TryReadBaseAnimationValue(animation, base))
			base = snapshot;
		return base.Kind() != BindingValueKind::Empty;
	}

	bool TryReadValueBelowVisualState(
		const RuntimeSetter& setter,
		BindingValue& output)
	{
		if (!setter.Target || !setter.Metadata) return false;
		Control::EffectiveValueEntry candidate;
		const auto stored = setter.Target->_propertyValues.find(
			&setter.Metadata->Property());
		if (stored != setter.Target->_propertyValues.end())
			candidate = stored->second;
		const int visualStateIndex = StoredPropertySourceIndex(
			DependencyPropertyValueSource::VisualState);
		const int animationIndex = StoredPropertySourceIndex(
			DependencyPropertyValueSource::Animation);
		if (visualStateIndex >= 0)
			candidate.Slots[static_cast<size_t>(visualStateIndex)].Reset();
		if (animationIndex >= 0)
			candidate.Slots[static_cast<size_t>(animationIndex)].Reset();
		DependencyPropertyValueSource ignoredSource =
			DependencyPropertyValueSource::Default;
		return setter.Target->TryEvaluateEffectivePropertyValue(
			*setter.Metadata, candidate, output, ignoredSource);
	}

	static bool EnteringAnimationValue(
		const RuntimeAnimation& animation,
		const BindingValue& current,
		BindingValue& output)
	{
		if (!animation.KeyFrames.empty())
		{
			output = animation.KeyFrames.front().Value;
			if (animation.IsAdditive
				&& !MatrixKeyFramesUseDirectValues(animation))
				return AddAnimationValues(animation, output, current, output);
			return true;
		}
		if (animation.From)
		{
			output = *animation.From;
			if (animation.IsAdditive && (animation.To || animation.By))
				return AddAnimationValues(animation, output, current, output);
			return true;
		}
		if (animation.To)
		{
			output = *animation.To;
			return true;
		}
		output = current;
		return true;
	}

	static CandidateAnimation MakeCandidateAnimation(
		const ClockIdentity& identity,
		const RuntimeAnimation& animation,
		BindingValue snapshot,
		BindingValue base,
		BindingValue foundation,
		BindingValue from,
		BindingValue to)
	{
		CandidateAnimation result;
		static_cast<TimelineDefinition&>(result) =
			static_cast<const TimelineDefinition&>(animation);
		result.RootClockId = identity.Instance;
		result.Target = animation.Target;
		result.Metadata = animation.Metadata;
		result.LayerValues.Snapshot = std::move(snapshot);
		result.LayerValues.Base = std::move(base);
		result.LayerValues.Foundation = std::move(foundation);
		result.From = std::move(from);
		result.To = std::move(to);
		result.ObjectPath = animation.ObjectPath;
		result.KeySplineParameters.assign(
			result.KeyFrames.size(), 0.0);
		return result;
	}

	static CandidateAnimation MakeCandidateAnimation(
		const ClockIdentity& identity,
		RuntimeAnimation&& animation,
		BindingValue snapshot,
		BindingValue base,
		BindingValue foundation,
		BindingValue from,
		BindingValue to)
	{
		CandidateAnimation result;
		static_cast<TimelineDefinition&>(result) = std::move(
			static_cast<TimelineDefinition&>(animation));
		result.RootClockId = identity.Instance;
		result.Target = animation.Target;
		result.Metadata = animation.Metadata;
		result.LayerValues.Snapshot = std::move(snapshot);
		result.LayerValues.Base = std::move(base);
		result.LayerValues.Foundation = std::move(foundation);
		result.From = std::move(from);
		result.To = std::move(to);
		result.ObjectPath = std::move(animation.ObjectPath);
		result.KeySplineParameters.assign(
			result.KeyFrames.size(), 0.0);
		return result;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	const RuntimeTransition* FindTransition(
		const RuntimeGroup& group,
		std::optional<size_t> fromState,
		size_t toState) const noexcept
	{
		const RuntimeTransition* best = nullptr;
		const RuntimeTransition* fallback = nullptr;
		int bestScore = -1;
		for (const auto& transition : group.Transitions)
		{
			if (!transition.FromState && !transition.ToState)
			{
				if (!fallback) fallback = &transition;
				continue;
			}
			int score = -1;
			if (transition.FromState == fromState) ++score;
			else if (transition.FromState) continue;
			if (transition.ToState && *transition.ToState == toState) score += 2;
			else if (transition.ToState) continue;
			if (score > bestScore)
			{
				bestScore = score;
				best = &transition;
			}
		}
		return best ? best : fallback;
	}
#endif

	bool TryResolveTransition(
		size_t groupIndex,
		std::optional<size_t> fromState,
		size_t toState,
		RuntimeTransition& storage,
		const RuntimeTransition*& result,
		std::wstring* outError)
	{
		result = nullptr;
		if (groupIndex >= Groups.size()) return true;
		if (!IsCompiledGroup(groupIndex))
		{
#if CUI_ENABLE_DYNAMIC_XAML
			result = FindTransition(Groups[groupIndex], fromState, toState);
			return true;
#else
			if (outError) *outError = L"Production 视觉状态组缺少编译程序。";
			return false;
#endif
		}
		const auto* group = CompiledGroupAt(groupIndex);
		if (!group || !CompiledInteractions
			|| !ValidCompiledRange(group->Transitions,
				CompiledInteractions->Program.Transitions.size()))
		{
			if (outError) *outError = L"编译 VisualTransition range 无效。";
			return false;
		}
		const CompiledInteractionTransitionOp* best = nullptr;
		const CompiledInteractionTransitionOp* fallback = nullptr;
		int bestScore = -1;
		for (uint32_t offset = 0; offset < group->Transitions.Count; ++offset)
		{
			const auto& candidate = CompiledInteractions->Program.Transitions[
				group->Transitions.Offset + offset];
			const auto candidateFrom = candidate.FromStateIndex
				== CompiledInteractionInvalidIndex
				? std::optional<size_t>{}
				: std::optional<size_t>{ candidate.FromStateIndex };
			const auto candidateTo = candidate.ToStateIndex
				== CompiledInteractionInvalidIndex
				? std::optional<size_t>{}
				: std::optional<size_t>{ candidate.ToStateIndex };
			if (!candidateFrom && !candidateTo)
			{
				if (!fallback) fallback = &candidate;
				continue;
			}
			int score = -1;
			if (candidateFrom == fromState) ++score;
			else if (candidateFrom) continue;
			if (candidateTo && *candidateTo == toState) score += 2;
			else if (candidateTo) continue;
			if (score > bestScore)
			{
				bestScore = score;
				best = &candidate;
			}
		}
		const auto* selected = best ? best : fallback;
		if (!selected) return true;
		if (!TryBuildCompiledTransition(
			groupIndex, *selected, storage, outError)) return false;
		result = &storage;
		return true;
	}

	bool ClearTransitionOnlyProperties(
		const std::vector<PropertyKey>& properties,
		const RuntimeState& state)
	{
		for (const auto& key : properties)
			if (key.Target && key.Property && !StateAnimatesProperty(state, key))
				if (key.Target->HasPropertyValue(
					*key.Property, DependencyPropertyValueSource::Animation)
					&& !key.Target->ClearPropertyValue(
					*key.Property,
					DependencyPropertyValueSource::Animation)) return false;
		return true;
	}

	bool GoTo(
		size_t groupIndex,
		size_t stateIndex,
		bool useTransitions,
		std::wstring* outError)
	{
		if (groupIndex >= Groups.size()
			|| stateIndex >= StateCount(groupIndex))
		{
			if (outError) *outError = L"视觉状态索引无效。";
			return false;
		}
		auto& group = Groups[groupIndex];
		const auto logicalCurrent = group.Pending
			? std::optional<size_t>(group.Pending->TargetState)
			: group.CurrentState;
		if (logicalCurrent && *logicalCurrent == stateIndex
			&& (!group.Pending || useTransitions))
		{
			if (outError) outError->clear();
			return true;
		}
		const auto now = NowMilliseconds();
		RuntimeTransition transitionStorage;
		const RuntimeTransition* transition = nullptr;
		if (useTransitions && Owner->AreSystemAnimationsEnabled()
			&& !TryResolveTransition(groupIndex, logicalCurrent, stateIndex,
				transitionStorage, transition, outError)) return false;
		unsigned long long totalDuration = transition
			? transition->GeneratedDurationMilliseconds : 0;
		if (transition)
			for (const auto& animation : transition->Animations)
				totalDuration = (std::max)(totalDuration,
					SaturatingAdd(animation.BeginTimeMilliseconds,
						TimelineActiveDurationMilliseconds(animation)));

		if (!transition || totalDuration == 0)
		{
			RuntimeState nextStorage;
			const auto* next = ResolveState(
				groupIndex, stateIndex, nextStorage, outError);
			if (!next) return false;
			std::vector<PropertyKey> oldTransitionProperties;
			const bool force = group.Pending.has_value();
			if (group.Pending)
				oldTransitionProperties = group.Pending->Properties;
			auto previousPending = group.Pending;
			std::vector<CandidateAnimation> previousAnimations;
			if (!SnapshotClockCandidates(previousAnimations)) return false;
			auto previousRoots = ClockNodes.Roots;
			bool committed = false;
			ControlScopeExit rollback{ [&]
				{
					if (committed) return;
					group.Pending = std::move(previousPending);
					(void)CommitClockCandidates(
						std::move(previousAnimations),
						std::move(previousRoots));
				} };
			group.Pending.reset();
			ClockNodes.Leaves.erase(std::remove_if(
				ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
				[&](const auto& animation)
				{ return IsOwnedByVisualStateGroup(animation, groupIndex); }),
				ClockNodes.Leaves.end());
			if (!GoToImmediate(groupIndex, stateIndex, outError,
				std::nullopt, force, &oldTransitionProperties)) return false;
			committed = true;
			return true;
		}

		RuntimeStateFootprint fromStorage;
		RuntimeState toStorage;
		const RuntimeStateFootprint* fromState = nullptr;
		if (logicalCurrent)
		{
			if (!TryBuildStateFootprint(
				groupIndex, *logicalCurrent, fromStorage, outError)) return false;
			fromState = &fromStorage;
		}
		const auto* resolvedToState = ResolveState(
			groupIndex, stateIndex, toStorage, outError);
		if (!resolvedToState) return false;
		const auto& toState = *resolvedToState;
		ClockIdentity transitionClockIdentity;
		if (!TryAllocateClockIdentity(
			VisualStateClockKey(groupIndex, stateIndex),
			transitionClockIdentity))
		{
			if (outError) *outError = L"VisualTransition ClockId 已耗尽。";
			return false;
		}
		std::vector<CandidateAnimation> pendingAnimations;
		std::vector<PropertyKey> pendingProperties;
		auto addProperty = [&](const RuntimeAnimation& animation)
			{
				PropertyKey key{
					animation.Target, PropertyIdentity(animation.Metadata) };
				if (!key.Target || !key.Property) return;
				if (std::none_of(pendingProperties.begin(), pendingProperties.end(),
					[&](const auto& existing) { return SameProperty(existing, key); }))
					pendingProperties.push_back(std::move(key));
			};
		auto explicitlyControls = [&](const auto& candidate)
			{
				return std::any_of(transition->Animations.begin(),
					transition->Animations.end(), [&](const auto& explicitAnimation)
					{ return SameAnimationTarget(candidate, explicitAnimation); });
			};
		auto explicitlyControlsProperty = [&](Control* target,
			const DependencyProperty* property)
			{
				return std::any_of(transition->Animations.begin(),
					transition->Animations.end(), [&](const auto& explicitAnimation)
					{
						return explicitAnimation.Target == target
							&& PropertyIdentity(explicitAnimation.Metadata)
								== property;
					});
			};
		auto stateControlsProperty = [](const RuntimeState& state,
			Control* target, const DependencyProperty* property)
			{
				return std::any_of(state.Setters.begin(), state.Setters.end(),
					[&](const auto& setter)
					{
						return setter.Target == target
							&& PropertyIdentity(setter.Metadata) == property;
					}) || std::any_of(state.Animations.begin(), state.Animations.end(),
						[&](const auto& animation)
						{
							return animation.Target == target
								&& PropertyIdentity(animation.Metadata) == property;
						});
			};
		auto addGeneratedSetterAnimation = [&](const RuntimeSetter& setter,
			BindingValue destination, const std::wstring& context)
			{
				if (!setter.Target || !setter.Metadata) return false;
				const auto* metadata = setter.Metadata;
				if (!metadata) return false;
				const auto kind = GeneratedAnimationKind(*metadata);
				if (!kind) return true;
				RuntimeAnimation generated;
				generated.Kind = *kind;
				generated.Target = setter.Target;
				generated.Metadata = metadata;
				generated.DurationMilliseconds =
					transition->GeneratedDurationMilliseconds;
				generated.Easing = transition->GeneratedEasing;
				generated.EasingMode = transition->GeneratedEasingMode;
				generated.EasingParameters =
					transition->GeneratedEasingParameters;
				BindingValue current;
				BindingValue base;
				if (!TryReadAnimationSnapshotAndBase(
					generated, current, base))
				{
					if (outError) *outError = L"VisualTransition 无法读取"
						+ context + L" Setter 起始值：" + metadata->Name();
					return false;
				}
				generated.From = current;
				generated.To = destination;
				BindingValue foundation = ZeroAnimationValue(generated);
				pendingAnimations.push_back(MakeCandidateAnimation(
					transitionClockIdentity, generated, current, std::move(base),
					std::move(foundation),
					std::move(current), std::move(destination)));
				addProperty(generated);
				return true;
			};
		if (transition->GeneratedDurationMilliseconds > 0)
		{
			for (const auto& setter : toState.Setters)
			{
				if (explicitlyControlsProperty(
					setter.Target, PropertyIdentity(setter.Metadata))) continue;
				if (!addGeneratedSetterAnimation(
					setter, setter.Value, L"进入"))
				{
					return false;
				}
			}
			if (fromState)
				for (const auto& setter : fromState->Setters)
				{
					if (stateControlsProperty(toState,
						setter.Target, PropertyIdentity(setter.Metadata))
						|| explicitlyControlsProperty(
							setter.Target, PropertyIdentity(setter.Metadata))) continue;
					BindingValue destination;
					if (!TryReadValueBelowVisualState(setter, destination)
						|| !addGeneratedSetterAnimation(
							setter, std::move(destination), L"退出"))
					{
						if (outError && outError->empty())
							*outError = L"VisualTransition 无法读取退出 Setter 基础值："
							+ setter.Metadata->Name();
						return false;
					}
				}
		}
		auto addObjectBaseHold = [&](const RuntimeAnimation& animation)
			{
				BindingValue current;
				BindingValue base;
				if (!TryReadAnimationSnapshotAndBase(
					animation, current, base)) return false;
				auto generated = animation;
				generated.TimelineGroupIndex = CompiledInteractionInvalidIndex;
				generated.From.reset();
				generated.To.reset();
				generated.By.reset();
				generated.IsAdditive = false;
				generated.IsCumulative = false;
				generated.KeyFrames = { RuntimeAnimationKeyFrame{
					DeclarativeKeyFrameKind::Discrete, 0, base } };
				generated.BeginTimeMilliseconds = 0;
				generated.DurationMilliseconds = totalDuration;
				generated.RepeatBehavior = DeclarativeRepeatBehaviorKind::Count;
				generated.RepeatCount = 1.0;
				generated.RepeatDurationMilliseconds = 0;
				generated.AutoReverse = false;
				generated.FillBehavior = DeclarativeTimelineFillBehavior::HoldEnd;
				generated.SpeedRatio = 1.0;
				generated.AccelerationRatio = 0.0;
				generated.DecelerationRatio = 0.0;
				generated.Easing = DeclarativeEasingKind::Linear;
				generated.EasingMode = DeclarativeEasingMode::EaseOut;
				generated.EasingParameters = {};
				pendingAnimations.push_back(MakeCandidateAnimation(
					transitionClockIdentity, generated, current, base, BindingValue{},
					std::move(current), std::move(base)));
				addProperty(generated);
				return true;
			};
		for (const auto& animation : toState.Animations)
			if (animation.Kind == DeclarativeAnimationKind::Object
				&& !explicitlyControls(animation)
				&& !addObjectBaseHold(animation))
			{
				if (outError) *outError = L"VisualTransition 无法释放 Object 动画基础值："
					+ animation.Metadata->Name();
				return false;
			}
		if (fromState)
			for (const auto& footprint : fromState->Animations)
			{
				if (footprint.Kind != DeclarativeAnimationKind::Object
					|| explicitlyControls(footprint)
					|| std::any_of(toState.Animations.begin(),
						toState.Animations.end(), [&](const auto& nextAnimation)
						{ return SameAnimationTarget(footprint, nextAnimation); }))
					continue;
				RuntimeAnimation animation;
				if (!TryMaterializeAnimationFootprint(
					footprint, animation, nullptr)) continue;
				// An old object-path leaf may no longer exist.  Leaving that state
				// releases the obsolete path instead of making it a transition error.
				(void)addObjectBaseHold(animation);
			}
		if (transition->GeneratedDurationMilliseconds > 0)
		{
			for (const auto& animation : toState.Animations)
			{
				if (animation.Kind == DeclarativeAnimationKind::Object
					|| explicitlyControls(animation)) continue;
				BindingValue from;
				BindingValue base;
				if (!TryReadAnimationSnapshotAndBase(animation, from, base))
				{
					if (outError) *outError = L"VisualTransition 无法读取进入动画起始值："
						+ animation.Metadata->Name();
					return false;
				}
				auto generated = animation;
				generated.TimelineGroupIndex = CompiledInteractionInvalidIndex;
				generated.From = from;
				BindingValue to;
				if (!EnteringAnimationValue(animation, from, to))
				{
					if (outError) *outError = L"VisualTransition 无法合成进入动画值："
						+ animation.Metadata->Name();
					return false;
				}
				generated.To = to;
				generated.By.reset();
				generated.IsAdditive = false;
				generated.IsCumulative = false;
				generated.KeyFrames.clear();
				generated.BeginTimeMilliseconds = 0;
				generated.DurationMilliseconds =
					transition->GeneratedDurationMilliseconds;
				generated.RepeatBehavior =
					DeclarativeRepeatBehaviorKind::Count;
				generated.RepeatCount = 1.0;
				generated.RepeatDurationMilliseconds = 0;
				generated.AutoReverse = false;
				generated.FillBehavior =
					DeclarativeTimelineFillBehavior::HoldEnd;
				generated.SpeedRatio = 1.0;
				generated.AccelerationRatio = 0.0;
				generated.DecelerationRatio = 0.0;
				generated.Easing = transition->GeneratedEasing;
				generated.EasingMode = transition->GeneratedEasingMode;
				generated.EasingParameters =
					transition->GeneratedEasingParameters;
				BindingValue foundation = ZeroAnimationValue(generated);
				pendingAnimations.push_back(MakeCandidateAnimation(
					transitionClockIdentity, generated, from, std::move(base),
					std::move(foundation),
					std::move(from), std::move(to)));
				addProperty(generated);
			}
			if (fromState)
				for (const auto& footprint : fromState->Animations)
				{
					if (footprint.Kind == DeclarativeAnimationKind::Object
						|| explicitlyControls(footprint)
						|| std::any_of(toState.Animations.begin(),
							toState.Animations.end(), [&](const auto& nextAnimation)
							{ return SameAnimationTarget(footprint, nextAnimation); }))
						continue;
					RuntimeAnimation animation;
					if (!TryMaterializeAnimationFootprint(
						footprint, animation, nullptr)) continue;
					BindingValue from;
					BindingValue to;
					if (!TryReadAnimationSnapshotAndBase(animation, from, to))
						continue;
					auto generated = animation;
					generated.TimelineGroupIndex = CompiledInteractionInvalidIndex;
					generated.From = from;
					generated.To = to;
					generated.By.reset();
					generated.IsAdditive = false;
					generated.IsCumulative = false;
					generated.KeyFrames.clear();
					generated.BeginTimeMilliseconds = 0;
					generated.DurationMilliseconds =
						transition->GeneratedDurationMilliseconds;
					generated.RepeatBehavior =
						DeclarativeRepeatBehaviorKind::Count;
					generated.RepeatCount = 1.0;
					generated.RepeatDurationMilliseconds = 0;
					generated.AutoReverse = false;
					generated.FillBehavior =
						DeclarativeTimelineFillBehavior::HoldEnd;
					generated.SpeedRatio = 1.0;
					generated.AccelerationRatio = 0.0;
					generated.DecelerationRatio = 0.0;
					generated.Easing = transition->GeneratedEasing;
					generated.EasingMode = transition->GeneratedEasingMode;
					generated.EasingParameters =
						transition->GeneratedEasingParameters;
					BindingValue base = to;
					BindingValue foundation = ZeroAnimationValue(generated);
					pendingAnimations.push_back(MakeCandidateAnimation(
						transitionClockIdentity, generated, from, std::move(base),
						std::move(foundation),
						std::move(from), std::move(to)));
					addProperty(generated);
				}
		}
		for (const auto& animation : transition->Animations)
		{
			BindingValue snapshot;
			BindingValue base;
			if (!TryReadAnimationSnapshotAndBase(animation, snapshot, base))
			{
				if (outError) *outError = L"VisualTransition Storyboard 无法捕获基础值："
					+ animation.Metadata->Name();
				return false;
			}
			BindingValue from;
			BindingValue to;
			BindingValue foundation;
			if (!ResolveAnimationEndpoints(
				animation, snapshot, base, from, to, foundation))
			{
				if (outError) *outError = L"VisualTransition Storyboard 无法解析 From/To/By："
					+ animation.Metadata->Name();
				return false;
			}
			pendingAnimations.push_back(MakeCandidateAnimation(
				transitionClockIdentity, animation, std::move(snapshot),
				std::move(base),
				std::move(foundation),
				std::move(from), std::move(to)));
			addProperty(animation);
		}

		std::vector<PropertyKey> oldTransitionProperties;
		if (group.Pending)
			oldTransitionProperties = group.Pending->Properties;
		std::vector<PropertyKey> changedProperties = oldTransitionProperties;
		for (const auto& key : pendingProperties)
			if (std::none_of(changedProperties.begin(), changedProperties.end(),
				[&](const auto& existing) { return SameProperty(existing, key); }))
				changedProperties.push_back(key);
		std::vector<PropertySnapshot> snapshots;
		snapshots.reserve(changedProperties.size() * 2);
		for (const auto& key : changedProperties)
			for (const auto source : {
				DependencyPropertyValueSource::VisualState,
				DependencyPropertyValueSource::Animation })
				CapturePropertySnapshot(key, source, snapshots);
		std::vector<CandidateAnimation> candidateAnimations;
		if (!SnapshotClockCandidates(candidateAnimations))
		{
			if (outError) *outError = L"VisualTransition 动画候选快照失败。";
			return false;
		}
		candidateAnimations.erase(std::remove_if(
			candidateAnimations.begin(), candidateAnimations.end(),
			[&](const auto& animation)
			{
				const auto* root = FindClockRoot(animation.RootClockId);
				return (root && root->Identity.Storyboard.Owner
					== VisualStateOwner(groupIndex))
					|| ConflictsAnyAnimationLayer(
						animation, pendingAnimations);
			}),
			candidateAnimations.end());
		candidateAnimations.reserve(
			candidateAnimations.size() + pendingAnimations.size());
		for (auto& animation : pendingAnimations)
		{
			if (animation.TimelineGroupIndex
					== CompiledInteractionInvalidIndex
				&& animation.BeginTimeMilliseconds == 0
				&& TimelineActiveDurationMilliseconds(animation) == 0)
				animation.Completed = true;
			candidateAnimations.push_back(animation);
		}
		auto candidateRoots = ClockNodes.Roots;
		EraseClockRootsByOwner(candidateRoots, VisualStateOwner(groupIndex));
		PendingTransition candidatePending{
			stateIndex, 0, transitionClockIdentity.Instance,
			pendingProperties };
		unsigned long long rootSimpleDuration = 0;
		bool rootSimpleDurationForever = false;
		if (!ResolveStoryboardTiming(transition->StoryboardTiming,
			pendingAnimations, transition->TimelineGroups,
			rootSimpleDuration, rootSimpleDurationForever))
		{
			if (outError) *outError = L"VisualTransition Storyboard 根时序无效。";
			return false;
		}
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed) (void)RestoreSnapshots(snapshots);
				Applying = previousApplying;
			} };
		for (const auto& key : oldTransitionProperties)
			if (key.Target && key.Property && key.Target->HasPropertyValue(
				*key.Property, DependencyPropertyValueSource::Animation))
				if (!key.Target->ClearPropertyValue(
					*key.Property, DependencyPropertyValueSource::Animation))
				{
					if (outError) *outError =
						L"VisualTransition 无法清理旧过渡属性。";
					return false;
				}
		std::vector<AnimationFrameValue> initialValues;
		initialValues.reserve(pendingAnimations.size());
		for (const auto& animation : pendingAnimations)
		{
			BindingValue value;
			const auto projection = ProjectAnimationAtRootBegin(
				animation, animation, transition->StoryboardTiming,
				rootSimpleDuration, rootSimpleDurationForever,
				transition->TimelineGroups);
			if (projection.BeforeBegin || projection.RootFillStopped)
				value = animation.LayerValues.Snapshot;
			else if (projection.ActivePeriodComplete
				&& animation.FillBehavior
					== DeclarativeTimelineFillBehavior::Stop)
				value = animation.LayerValues.Snapshot;
			else if (!Interpolate(animation,
				projection.ActiveElapsedMilliseconds, value))
			{
				if (outError) *outError = L"VisualTransition 初始帧无效。";
				return false;
			}
			initialValues.push_back({ &animation, std::move(value) });
		}
		if (!ApplyAnimationFrame(initialValues))
		{
			if (outError) *outError = L"VisualTransition 无法事务性应用。";
			return false;
		}
		const auto startTick = NowMilliseconds();
		if (!InstallClockRoot(candidateRoots, transitionClockIdentity, startTick,
			transition->StoryboardTiming, rootSimpleDuration,
			rootSimpleDurationForever, transition->TimelineGroups))
		{
			if (outError) *outError = L"VisualTransition root registry 已耗尽。";
			return false;
		}
		std::vector<AnimationClockView> initiallyStopped;
		const auto pendingViews = CandidateClockViews(pendingAnimations);
		if (!ReleaseStoppedAnimationValues(initiallyStopped,
			pendingViews, candidateRoots, startTick))
		{
			if (outError) *outError =
				L"VisualTransition 无法释放已停止的动画值。";
			return false;
		}
		PruneCandidateRoots(candidateRoots, candidateAnimations);
		if (rootSimpleDurationForever
			|| transition->StoryboardTiming.RepeatBehavior
				== DeclarativeRepeatBehaviorKind::Forever)
			candidatePending.EndTick =
				(std::numeric_limits<unsigned long long>::max)();
		else
		{
			const auto rootActiveDurationExact =
				transition->StoryboardTiming.RepeatBehavior
					== DeclarativeRepeatBehaviorKind::Duration
				? static_cast<long double>(transition->StoryboardTiming
					.RepeatDurationMilliseconds)
				: static_cast<long double>(rootSimpleDuration)
					* (transition->StoryboardTiming.AutoReverse ? 2.0L : 1.0L)
					* static_cast<long double>(
						transition->StoryboardTiming.RepeatCount)
					/ static_cast<long double>(
						transition->StoryboardTiming.SpeedRatio);
			const auto maximum =
				(std::numeric_limits<unsigned long long>::max)();
			const auto rootActiveDuration = !std::isfinite(rootActiveDurationExact)
				|| rootActiveDurationExact >= static_cast<long double>(maximum)
				? maximum : static_cast<unsigned long long>(
					std::ceil(rootActiveDurationExact));
			candidatePending.EndTick = SaturatingAdd(startTick,
				SaturatingAdd(transition->StoryboardTiming.BeginTimeMilliseconds,
					rootActiveDuration));
		}
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations))
		{
			if (outError) *outError = L"VisualTransition 回滚快照失败。";
			return false;
		}
		auto previousRoots = ClockNodes.Roots;
		if (!CommitClockCandidates(
			std::move(candidateAnimations), std::move(candidateRoots)))
		{
			(void)CommitClockCandidates(
				std::move(previousAnimations), std::move(previousRoots));
			if (outError)
				*outError = L"VisualTransition ClockNode arena 已耗尽。";
			return false;
		}
		group.Pending = std::move(candidatePending);
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		if (outError) outError->clear();
		return true;
	}

	bool EvaluateGroup(size_t groupIndex, std::wstring* outError = nullptr)
	{
		if (groupIndex >= Groups.size()) return false;
		return GoTo(groupIndex, EvaluateState(groupIndex), true, outError);
	}

	void OnHostPropertyChanged(const DependencyPropertyChangedEventArgs& args)
	{
		if (Applying || !args.Property) return;
		for (size_t index = 0; index < Groups.size(); ++index)
		{
			bool observes = false;
			if (const auto* group = CompiledGroupAt(index))
			{
				if (CompiledInteractions && ValidCompiledRange(
					group->ConditionOperands,
					CompiledInteractions->Program.GroupConditionOperands.size()))
					for (uint32_t offset = 0;
						offset < group->ConditionOperands.Count; ++offset)
					{
						const auto operandIndex =
							CompiledInteractions->Program.GroupConditionOperands[
								group->ConditionOperands.Offset + offset];
						if (operandIndex
							>= CompiledInteractions->Program.PropertyOperands.size())
							continue;
						if (CompiledInteractions->Program.PropertyOperands[
							operandIndex].Property.Identity() == args.Property)
						{
							observes = true;
							break;
						}
					}
			}
		#if CUI_ENABLE_DYNAMIC_XAML
			else
				observes = std::find(Groups[index].ConditionProperties.begin(),
					Groups[index].ConditionProperties.end(), args.Property)
					!= Groups[index].ConditionProperties.end();
		#endif
			if (observes) (void)EvaluateGroup(index);
		}
	}

	bool ApplyRetainedAnimationFrame(unsigned long long nowMilliseconds)
	{
		if (!SynchronizeClockNodeArena()) return false;
		const auto rootProjections = ProjectClockRoots(
			ClockNodes.Roots, nowMilliseconds);
		std::vector<AnimationFrameValue> values;
		values.resize(ClockNodes.Leaves.size());
		std::vector<BindingValue> composedValues(AnimationExactKeyCount);
		std::vector<bool> composedHasFrame(AnimationExactKeyCount, false);
		if (!VisitClockLeavesByRoot(rootProjections,
			[&](const auto& root, const auto& rootProjection,
				const auto& animation, uint32_t payloadIndex)
			{
			const auto projection = ProjectAnimationClock(
				animation, root, rootProjection, animation);
			const auto* layer = ResolveAnimationLayer(animation);
			if (!layer) return false;
			if (layer->ExactKeyIndex >= composedValues.size()) return false;
			auto& composed = composedValues[layer->ExactKeyIndex];
			if (composed.Kind() == BindingValueKind::Empty)
				composed = layer->Values.Base;
			if (root.State.Stopped)
			{
				if (!composedHasFrame[layer->ExactKeyIndex])
				{
					BindingValue stoppedBase;
					if (!TryReadBaseAnimationValue(
						animation, stoppedBase)) return false;
					values[payloadIndex] = {
						&animation, std::move(stoppedBase) };
					composed = values[payloadIndex].Value;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			if (projection.BeforeBegin)
			{
				if (layer->Handoff
					!= DeclarativeHandoffBehavior::Compose)
				{
					values[payloadIndex] = { &animation, layer->Values.Snapshot };
					composed = layer->Values.Snapshot;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			if (projection.RootFillStopped)
			{
				if (layer->Handoff
					!= DeclarativeHandoffBehavior::Compose)
				{
					values[payloadIndex] = {
						&animation, layer->Values.Snapshot };
					composed = layer->Values.Snapshot;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			if (animation.Completed && projection.ActivePeriodComplete
				&& animation.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop)
			{
				if (layer->Handoff
					!= DeclarativeHandoffBehavior::Compose)
				{
					values[payloadIndex] = { &animation, layer->Values.Snapshot };
					composed = layer->Values.Snapshot;
					composedHasFrame[layer->ExactKeyIndex] = true;
				}
				return true;
			}
			BindingValue value;
			if (!Interpolate(animation, layer->Values,
				projection.ActiveElapsedMilliseconds, value,
				layer->Handoff == DeclarativeHandoffBehavior::Compose
					? &composed : nullptr)) return false;
			values[payloadIndex] = { &animation, std::move(value) };
			composed = values[payloadIndex].Value;
			composedHasFrame[layer->ExactKeyIndex] = true;
			return true;
		})) return false;
		if (!ApplyCommittedAnimationFrame(values)) return false;
		for (auto& root : ClockNodes.Roots)
			root.State.LastTick = nowMilliseconds;
		return true;
	}

	template<typename TAnimations>
	bool BeginResolvedEventStoryboard(
		const StoryboardClockKey& storyboard,
		TAnimations& animations,
		const std::vector<RuntimeTimelineGroup>& timelineGroups,
		const DeclarativeStoryboardTimingDefinition& timing,
		DeclarativeHandoffBehavior handoff,
		std::wstring* outError)
	{
		if (handoff != DeclarativeHandoffBehavior::SnapshotAndReplace
			&& handoff != DeclarativeHandoffBehavior::Compose)
		{
			if (outError) *outError = L"BeginStoryboard HandoffBehavior 无效。";
			return false;
		}
		ClockIdentity clockIdentity;
		if (!TryAllocateClockIdentity(storyboard, clockIdentity))
		{
			if (outError) *outError = L"BeginStoryboard ClockId 已耗尽。";
			return false;
		}
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		for (const auto& animation : animations)
			CapturePropertySnapshot({ animation.Target,
				PropertyIdentity(animation.Metadata) },
				DependencyPropertyValueSource::Animation, snapshots);

		std::vector<CandidateAnimation> pending;
		pending.reserve(animations.size());
		for (auto& animation : animations)
		{
			BindingValue current;
			BindingValue base;
			if (!TryReadAnimationSnapshotAndBase(animation, current, base))
			{
				if (outError) *outError = L"BeginStoryboard 无法捕获当前值："
					+ animation.Metadata->Name();
				return false;
			}
			BindingValue from;
			BindingValue to;
			BindingValue foundation;
			if (!ResolveAnimationEndpoints(animation, current, base,
				from, to, foundation))
			{
				if (outError) *outError = L"BeginStoryboard 无法解析 From/To/By："
					+ animation.Metadata->Name();
				return false;
			}
			CandidateAnimation candidate;
			AnimationLayerValueState dynamicValues;
			if (handoff == DeclarativeHandoffBehavior::Compose)
				ConfigureComposeDynamicValues(animation, dynamicValues);
			if constexpr (std::is_const_v<
				std::remove_reference_t<decltype(animation)>>)
				candidate = MakeCandidateAnimation(clockIdentity, animation,
					current, std::move(base), std::move(foundation), std::move(from),
					std::move(to));
			else
				candidate = MakeCandidateAnimation(clockIdentity, std::move(animation),
					current, std::move(base), std::move(foundation), std::move(from),
					std::move(to));
			candidate.Handoff = handoff;
			candidate.LayerValues.DynamicFrom = dynamicValues.DynamicFrom;
			candidate.LayerValues.DynamicFoundation =
				dynamicValues.DynamicFoundation;
			pending.push_back(std::move(candidate));
		}
		unsigned long long rootSimpleDuration = 0;
		bool rootSimpleDurationForever = false;
		if (!ResolveStoryboardTiming(timing, pending, timelineGroups,
			rootSimpleDuration, rootSimpleDurationForever))
		{
			if (outError) *outError = L"BeginStoryboard 根时序无效或尚不受支持。";
			return false;
		}

		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations))
		{
			if (outError) *outError = L"BeginStoryboard 回滚快照失败。";
			return false;
		}
		auto previousRoots = ClockNodes.Roots;
		const bool animationsEnabled = Owner->AreSystemAnimationsEnabled();
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed)
					RestoreActiveAnimationTransaction(
						previousAnimations, previousRoots,
						snapshots, previousApplying);
				else
					Applying = previousApplying;
			} };
		std::vector<AnimationFrameValue> initialValues;
		initialValues.reserve(pending.size());
		bool success = true;
		for (const auto& animation : pending)
		{
			const auto activeDuration =
				TimelineActiveDurationMilliseconds(animation);
			const bool active = animationsEnabled
				&& (animation.TimelineGroupIndex
						!= CompiledInteractionInvalidIndex
					|| animation.BeginTimeMilliseconds > 0 || activeDuration > 0);
			BindingValue value;
			if (animationsEnabled)
			{
				const auto projection = ProjectAnimationAtRootBegin(
					animation, animation, timing, rootSimpleDuration,
					rootSimpleDurationForever, timelineGroups);
				const bool childFillStopped =
					projection.ActivePeriodComplete
					&& animation.FillBehavior
						== DeclarativeTimelineFillBehavior::Stop;
				if ((projection.BeforeBegin || projection.RootFillStopped
					|| childFillStopped)
					&& handoff == DeclarativeHandoffBehavior::Compose)
					continue;
				if (projection.BeforeBegin || projection.RootFillStopped
					|| childFillStopped)
					value = animation.LayerValues.Snapshot;
				else if (!Interpolate(animation,
					projection.ActiveElapsedMilliseconds, value))
				{
					success = false;
					break;
				}
			}
			else if (handoff == DeclarativeHandoffBehavior::Compose
				&& !active && animation.FillBehavior
					== DeclarativeTimelineFillBehavior::Stop)
				continue;
			else if (!active && animation.FillBehavior
				== DeclarativeTimelineFillBehavior::Stop)
				value = animation.LayerValues.Snapshot;
			else if (!Interpolate(animation,
				active ? 0 : activeDuration, value))
			{
				success = false;
				break;
			}
			initialValues.push_back({ &animation, std::move(value) });
		}
		if (success) success = ApplyAnimationFrame(initialValues);
		if (!success)
		{
			if (outError) *outError = L"BeginStoryboard 初始帧无法事务性应用。";
			return false;
		}

		const auto startTick = NowMilliseconds();
		std::vector<CandidateAnimation> candidateAnimations;
		if (!SnapshotClockCandidates(candidateAnimations))
		{
			if (outError) *outError = L"BeginStoryboard 动画候选快照失败。";
			return false;
		}
		candidateAnimations.erase(std::remove_if(
			candidateAnimations.begin(), candidateAnimations.end(),
			[&](const auto& animation)
			{
				const auto* root = FindClockRoot(animation.RootClockId);
				return (root && root->Identity.Storyboard == storyboard)
					|| (handoff
						== DeclarativeHandoffBehavior::SnapshotAndReplace
						&& ConflictsAnyAnimationLayer(animation, pending));
			}), candidateAnimations.end());
		auto candidateRoots = ClockNodes.Roots;
		if (!InstallClockRoot(candidateRoots, clockIdentity, startTick, timing,
			rootSimpleDuration, rootSimpleDurationForever, timelineGroups))
		{
			if (outError) *outError = L"BeginStoryboard root registry 已耗尽。";
			return false;
		}
		auto evaluationRoots = ClockNodes.Roots;
		if (!AppendClockRoot(evaluationRoots, clockIdentity, startTick, timing,
			rootSimpleDuration, rootSimpleDurationForever, timelineGroups))
		{
			if (outError) *outError = L"BeginStoryboard evaluation roots 已耗尽。";
			return false;
		}
		std::vector<CandidateAnimation> releaseContext = candidateAnimations;
		for (const auto& animation : pending)
			releaseContext.push_back(animation);
		std::vector<AnimationClockView> stopping;
		stopping.reserve(ClockNodes.Leaves.size() + pending.size());
		for (const auto& animation : ClockNodes.Leaves)
			if (IsStoryboardClock(animation, storyboard)
				|| (handoff == DeclarativeHandoffBehavior::SnapshotAndReplace
					&& ConflictsAnyAnimationLayer(animation, pending)))
			{
				AnimationClockView view;
				if (!ActiveClockView(animation, view)) return false;
				stopping.push_back(view);
			}
		const auto releaseViews = CandidateClockViews(releaseContext);
		if (!ReleaseStoppedAnimationValues(
			stopping, releaseViews, evaluationRoots, startTick))
		{
			if (outError) *outError =
				L"BeginStoryboard 无法释放被替换或已停止的动画值。";
			return false;
		}
		candidateAnimations.reserve(
			candidateAnimations.size() + pending.size());
		for (auto& animation : pending)
		{
			const auto activeDuration =
				TimelineActiveDurationMilliseconds(animation);
			const bool active = animationsEnabled
				&& (animation.TimelineGroupIndex
						!= CompiledInteractionInvalidIndex
					|| animation.BeginTimeMilliseconds > 0 || activeDuration > 0);
			if (active)
				candidateAnimations.push_back(std::move(animation));
			else
			{
				animation.Completed = true;
				candidateAnimations.push_back(std::move(animation));
			}
		}
		PruneCandidateRoots(candidateRoots, candidateAnimations);
		if (!CommitClockCandidates(
			std::move(candidateAnimations), std::move(candidateRoots)))
		{
			if (outError) *outError = L"BeginStoryboard ClockNode arena 已耗尽。";
			return false;
		}
		if (!stopping.empty() && !ApplyRetainedAnimationFrame(startTick))
		{
			if (outError) *outError =
				L"BeginStoryboard 无法重组保留的动画值。";
			return false;
		}
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		if (HasActiveAnimations() || !stopping.empty())
			Owner->RefreshDeclarativeAnimationWindowScheduling();
		if (outError) outError->clear();
		return true;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	bool BeginEventStoryboard(size_t storyboardIndex, std::wstring* outError)
	{
		if (storyboardIndex >= EventStoryboards.size())
		{
			if (outError) *outError = L"BeginStoryboard 索引无效。";
			return false;
		}
		const auto& definition = EventStoryboards[storyboardIndex];
		const auto& animations = definition.Animations;
		return BeginResolvedEventStoryboard(
			ComponentStoryboardClockKey(storyboardIndex), animations,
			definition.TimelineGroups, definition.Timing,
			definition.Handoff, outError);
	}
#endif

	bool PauseEventStoryboard(const StoryboardClockKey& storyboard)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return false;
		if (found->State.PendingResume)
			found->State.PendingResume = false;
		else if (!found->State.Paused)
			found->State.PendingPause = true;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool SeekEventStoryboard(
		const StoryboardClockKey& storyboard,
		unsigned long long offsetMilliseconds)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return false;
		found->State.PendingSeekOffsetMilliseconds =
			RootParentOffsetForSeek(*found, offsetMilliseconds);
		found->State.PendingStop = false;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool SetSpeedRatioEventStoryboard(
		const StoryboardClockKey& storyboard,
		double ratio)
	{
		if (!std::isfinite(ratio) || ratio < 0.0) return false;
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return false;
		found->State.PendingSpeedRatio = ratio;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool SkipEventStoryboardToFill(const StoryboardClockKey& storyboard)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return false;
		const auto duration = RootActiveDuration(*found);
		if (!duration) return false;
		found->State.PendingSeekOffsetMilliseconds =
			static_cast<long double>(found->Timing.BeginTimeMilliseconds)
				+ static_cast<long double>(*duration);
		found->State.PendingStop = false;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool ResumeEventStoryboard(const StoryboardClockKey& storyboard)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return false;
		if (found->State.PendingPause)
			found->State.PendingPause = false;
		else if (found->State.Paused)
			found->State.PendingResume = true;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool CommitRemoveEventStoryboard(
		const StoryboardClockKey& storyboard,
		unsigned long long now)
	{
		std::vector<AnimationClockView> stopping;
		for (const auto& animation : ClockNodes.Leaves)
			if (IsStoryboardClock(animation, storyboard))
			{
				AnimationClockView view;
				if (!ActiveClockView(animation, view)) return false;
				stopping.push_back(view);
			}
		// Stop is an idempotent action.  A missing clock is a successful no-op;
		// false is reserved for a failed retained-frame recomposition so callers
		// with an enclosing transaction can roll the mutation back.
		if (stopping.empty()) return true;
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations)) return false;
		auto previousRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed)
					RestoreActiveAnimationTransaction(
						previousAnimations, previousRoots,
						snapshots, previousApplying);
				else
					Applying = previousApplying;
			} };
		std::vector<AnimationClockView> activeViews;
		if (!ActiveClockViews(activeViews)
			|| !ReleaseStoppedAnimationValues(
				stopping, activeViews, ClockNodes.Roots, now)) return false;
		ClockNodes.Leaves.erase(std::remove_if(
			ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](const auto& animation)
			{
				return IsStoryboardClock(animation, storyboard);
			}), ClockNodes.Leaves.end());
		EraseClockRootsByStoryboard(ClockNodes.Roots, storyboard);
		ClockNodesDirty = true;
		if (!ApplyRetainedAnimationFrame(now)) return false;
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool CommitStopEventStoryboard(
		const StoryboardClockKey& storyboard,
		unsigned long long now)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		// WPF Stop is idempotent and retains the controllable clock tree.
		if (found == ClockNodes.Roots.end() || found->State.Stopped) return true;
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> previousAnimations;
		if (!SnapshotClockCandidates(previousAnimations)) return false;
		auto previousRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		Applying = true;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (!committed)
					RestoreActiveAnimationTransaction(
						previousAnimations, previousRoots,
						snapshots, previousApplying);
				else
					Applying = previousApplying;
			} };
		found->State.Stopped = true;
		if (!ApplyRetainedAnimationFrame(now)) return false;
		committed = true;
		Applying = previousApplying;
		PublishClockRootRegistry();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool StopEventStoryboard(const StoryboardClockKey& storyboard)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return true;
		found->State.PendingStop = true;
		found->State.PendingSeekOffsetMilliseconds.reset();
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

	bool RemoveEventStoryboard(const StoryboardClockKey& storyboard)
	{
		auto found = std::find_if(ClockNodes.Roots.begin(),
			ClockNodes.Roots.end(), [&](const auto& root)
			{ return root.Identity.Storyboard == storyboard; });
		if (found == ClockNodes.Roots.end()
			|| !found->State.ControlLookupAvailable) return true;
		found->State.PendingRemove = true;
		found->State.PendingStop = true;
		found->State.PendingSeekOffsetMilliseconds.reset();
		found->State.ControlLookupAvailable = false;
		Owner->RefreshDeclarativeAnimationWindowScheduling();
		return true;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	void ExecuteEventTriggerAction(const RuntimeEventTriggerAction& action)
	{
		const auto storyboard =
			ComponentStoryboardClockKey(action.StoryboardIndex);
		switch (action.Kind)
		{
		case DeclarativeStoryboardActionKind::Begin:
			(void)BeginEventStoryboard(action.StoryboardIndex, nullptr);
			break;
		case DeclarativeStoryboardActionKind::Pause:
			(void)PauseEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Resume:
			(void)ResumeEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Stop:
			(void)StopEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Remove:
			(void)RemoveEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Seek:
			(void)SeekEventStoryboard(
				storyboard, action.SeekOffsetMilliseconds);
			break;
		case DeclarativeStoryboardActionKind::SetSpeedRatio:
			(void)SetSpeedRatioEventStoryboard(
				storyboard, action.SpeedRatio);
			break;
		case DeclarativeStoryboardActionKind::SkipToFill:
			(void)SkipEventStoryboardToFill(storyboard);
			break;
		}
	}
#endif

	bool TryBuildCompiledEventStoryboard(
		uint32_t storyboardIndex,
		std::vector<RuntimeAnimation>& animations,
		std::vector<RuntimeTimelineGroup>& timelineGroups,
		std::wstring* outError)
	{
		if (!CompiledInteractions
			|| storyboardIndex >= CompiledInteractions->Program.Storyboards.size())
		{
			if (outError) *outError = L"编译 BeginStoryboard 索引无效。";
			return false;
		}
		const auto& instance = *CompiledInteractions;
		const auto& storyboard = instance.Program.Storyboards[storyboardIndex];
		if (!ValidCompiledRange(
			storyboard.Animations, instance.Program.Animations.size())
			|| !ValidCompiledRange(storyboard.TimelineGroups,
				instance.Program.TimelineGroups.size())
			|| (storyboard.Animations.Count == 0
				&& storyboard.TimelineGroups.Count == 0))
		{
			if (outError) *outError = L"编译 Storyboard animation range 无效。";
			return false;
		}
		animations.clear();
		animations.reserve(storyboard.Animations.Count);
		for (uint32_t offset = 0; offset < storyboard.Animations.Count; ++offset)
		{
			RuntimeAnimation animation;
			if (!TryBuildCompiledAnimation(instance.Program, instance.Values,
				instance.Targets, instance.Program.Animations[
					storyboard.Animations.Offset + offset], animation,
				L"编译 EventTrigger Storyboard", outError)) return false;
			animations.push_back(std::move(animation));
		}
		timelineGroups.clear();
		if (!AppendCompiledTimelineGroups(instance.Program, instance.Values,
			instance.Targets, storyboard.TimelineGroups,
			CompiledInteractionInvalidIndex, animations, timelineGroups,
			L"编译 EventTrigger Storyboard", outError)) return false;
		return true;
	}

	void ExecuteCompiledEventTriggerAction(
		const CompiledInteractionActionOp& action)
	{
		if (!CompiledInteractions
			|| action.StoryboardIndex
				>= CompiledInteractions->Program.Storyboards.size())
			return;
		const auto storyboard = CompiledInteractionStoryboardClockKey(
			action.StoryboardIndex);
		switch (action.Kind)
		{
		case DeclarativeStoryboardActionKind::Begin:
		{
			std::vector<RuntimeAnimation> animations;
			std::vector<RuntimeTimelineGroup> timelineGroups;
			if (TryBuildCompiledEventStoryboard(
				action.StoryboardIndex, animations, timelineGroups, nullptr))
				(void)BeginResolvedEventStoryboard(
					storyboard, animations, timelineGroups,
					CompiledInteractions->Program.Storyboards[
						action.StoryboardIndex].Timing,
					action.Handoff, nullptr);
			break;
		}
		case DeclarativeStoryboardActionKind::Pause:
			(void)PauseEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Resume:
			(void)ResumeEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Stop:
			(void)StopEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Remove:
			(void)RemoveEventStoryboard(storyboard);
			break;
		case DeclarativeStoryboardActionKind::Seek:
			(void)SeekEventStoryboard(
				storyboard, action.SeekOffsetMilliseconds);
			break;
		case DeclarativeStoryboardActionKind::SetSpeedRatio:
			(void)SetSpeedRatioEventStoryboard(
				storyboard, action.SpeedRatio);
			break;
		case DeclarativeStoryboardActionKind::SkipToFill:
			(void)SkipEventStoryboardToFill(storyboard);
			break;
		default:
			break;
		}
	}

	void ExecuteCompiledEventTriggerActions(CompiledInteractionRange range)
	{
		if (!CompiledInteractions || !ValidCompiledRange(
			range, CompiledInteractions->Program.Actions.size())) return;
		for (uint32_t offset = 0; offset < range.Count; ++offset)
			ExecuteCompiledEventTriggerAction(
				CompiledInteractions->Program.Actions[range.Offset + offset]);
	}

#if CUI_ENABLE_DYNAMIC_XAML
	bool ExecuteStyleTriggerActions(
		const RuntimeStyleTriggerScope& scope,
		const std::vector<RuntimeEventTriggerAction>& actions,
		std::wstring* outError)
	{
		std::vector<PropertySnapshot> snapshots;
		auto snapshotProperty = [&](Control* target,
			const DependencyPropertyMetadata* metadata)
			{
				PropertyKey key{ target, PropertyIdentity(metadata) };
				if (!key.Target || !key.Property
					|| std::any_of(snapshots.begin(), snapshots.end(),
						[&](const auto& existing)
						{ return SameProperty(existing.Key, key); })) return;
				PropertySnapshot snapshot;
				snapshot.Key = key;
				snapshot.Source = DependencyPropertyValueSource::Animation;
				BindingValue value;
				if (target->TryGetPropertyValue(
					*key.Property, snapshot.Source, value))
					snapshot.Value = std::move(value);
				snapshots.push_back(std::move(snapshot));
			};
		for (const auto& animation : ClockNodes.Leaves)
			snapshotProperty(animation.Target, animation.Metadata);
		for (const auto& action : actions)
		{
			if (action.StoryboardIndex >= EventStoryboards.size())
			{
				if (outError) *outError = L"Style Storyboard action 索引无效。";
				return false;
			}
			for (const auto& animation
				: EventStoryboards[action.StoryboardIndex].Animations)
				snapshotProperty(animation.Target, animation.Metadata);
		}
		std::vector<CandidateAnimation> activeAnimations;
		if (!SnapshotClockCandidates(activeAnimations)) return false;
		auto activeRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (committed) return;
				RestoreActiveAnimationTransaction(
					activeAnimations, activeRoots,
					snapshots, previousApplying);
			} };
		for (const auto& action : actions)
		{
			const auto storyboard = StyleStoryboardClockKey(
				scope, static_cast<uint64_t>(action.StoryboardIndex));
			switch (action.Kind)
			{
			case DeclarativeStoryboardActionKind::Begin:
				if (!BeginResolvedEventStoryboard(storyboard,
					EventStoryboards[action.StoryboardIndex].Animations,
					EventStoryboards[action.StoryboardIndex].TimelineGroups,
					EventStoryboards[action.StoryboardIndex].Timing,
					EventStoryboards[action.StoryboardIndex].Handoff,
					outError))
					return false;
				break;
			case DeclarativeStoryboardActionKind::Pause:
				(void)PauseEventStoryboard(storyboard);
				break;
			case DeclarativeStoryboardActionKind::Resume:
				(void)ResumeEventStoryboard(storyboard);
				break;
			case DeclarativeStoryboardActionKind::Stop:
				if (!StopEventStoryboard(storyboard))
				{
					if (outError)
						*outError = L"Style StopStoryboard 无法重组动画值。";
					return false;
				}
				break;
			case DeclarativeStoryboardActionKind::Remove:
				if (!RemoveEventStoryboard(storyboard))
				{
					if (outError)
						*outError = L"Style RemoveStoryboard 无法重组动画值。";
					return false;
				}
				break;
			case DeclarativeStoryboardActionKind::Seek:
				(void)SeekEventStoryboard(
					storyboard, action.SeekOffsetMilliseconds);
				break;
			case DeclarativeStoryboardActionKind::SetSpeedRatio:
				(void)SetSpeedRatioEventStoryboard(
					storyboard, action.SpeedRatio);
				break;
			case DeclarativeStoryboardActionKind::SkipToFill:
				(void)SkipEventStoryboardToFill(storyboard);
				break;
			default:
				if (outError) *outError = L"Style Storyboard action enum 无效。";
				return false;
			}
		}
		committed = true;
		if (outError) outError->clear();
		return true;
	}
#endif

	bool TryBuildCompiledStyleStoryboard(
		const RuntimeStyleTriggerScope& scope,
		uint32_t storyboardIndex,
		std::vector<RuntimeAnimation>& animations,
		std::vector<RuntimeTimelineGroup>& timelineGroups,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError)
					*outError = L"Style DataTrigger：" + std::move(message);
				return false;
			};
		if (!Owner || !scope.CompiledProgram)
			return fail(L"编译 Style action 程序未绑定。");
		const auto& styleProgram = *scope.CompiledProgram;
		if (storyboardIndex >= styleProgram.Storyboards.size())
			return fail(L"编译 Style storyboard 索引越界。");
		const auto& storyboard = styleProgram.Storyboards[storyboardIndex];
		if (!ValidCompiledRange(
				storyboard.Animations, styleProgram.Animations.size())
			|| !ValidCompiledRange(storyboard.TimelineGroups,
				styleProgram.TimelineGroups.size())
			|| (storyboard.Animations.Count == 0
				&& storyboard.TimelineGroups.Count == 0))
			return fail(L"编译 Style storyboard 动画 range 无效。");

		CompiledInteractionProgramView interactionProgram;
		interactionProgram.TargetCount = 1;
		interactionProgram.PropertyOperands = styleProgram.PropertyOperands;
		interactionProgram.ObjectPathChildIndices =
			styleProgram.ObjectPathChildIndices;
		interactionProgram.ObjectPaths = styleProgram.ObjectPaths;
		interactionProgram.KeyFrames = styleProgram.KeyFrames;
		interactionProgram.PathSegments = styleProgram.PathSegments;
		interactionProgram.Animations = styleProgram.Animations;
		interactionProgram.TimelineGroups = styleProgram.TimelineGroups;
		interactionProgram.Storyboards = styleProgram.Storyboards;
		interactionProgram.Actions = styleProgram.Actions;
		Control* targetSlots[] = { Owner };
		std::span<Control* const> targets(targetSlots, 1);
		animations.clear();
		animations.reserve(storyboard.Animations.Count);
		for (uint32_t offset = 0;
			offset < storyboard.Animations.Count; ++offset)
		{
			const auto& sourceAnimation = styleProgram.Animations[
				storyboard.Animations.Offset + offset];
			if (sourceAnimation.OperandIndex
					>= styleProgram.PropertyOperands.size()
				|| styleProgram.PropertyOperands[
					sourceAnimation.OperandIndex].TargetSlot != 0)
				return fail(L"Style Storyboard 只能以样式宿主为目标。");
			RuntimeAnimation animation;
			if (!TryBuildCompiledAnimation(interactionProgram,
				scope.CompiledValues, targets,
				sourceAnimation, animation,
				L"Style BeginStoryboard", outError)) return false;
			animations.push_back(std::move(animation));
		}
		timelineGroups.clear();
		if (!AppendCompiledTimelineGroups(interactionProgram,
			scope.CompiledValues, targets, storyboard.TimelineGroups,
			CompiledInteractionInvalidIndex, animations, timelineGroups,
			L"Style BeginStoryboard", outError)) return false;
		return true;
	}

	bool ExecuteCompiledStyleTriggerActions(
		const RuntimeStyleTriggerScope& scope,
		CompiledStyleRange range,
		std::wstring* outError)
	{
		if (!scope.CompiledProgram
			|| range.Offset > scope.CompiledProgram->Actions.size()
			|| range.Count > scope.CompiledProgram->Actions.size() - range.Offset)
		{
			if (outError) *outError = L"Style Storyboard action range 无效。";
			return false;
		}
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> activeAnimations;
		if (!SnapshotClockCandidates(activeAnimations)) return false;
		auto activeRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (committed) return;
				RestoreActiveAnimationTransaction(
					activeAnimations, activeRoots,
					snapshots, previousApplying);
			} };
		for (uint32_t offset = 0; offset < range.Count; ++offset)
		{
			const auto& action = scope.CompiledProgram->Actions[
				range.Offset + offset];
			if (!ValidCompiledActionKind(action.Kind)
				|| !ValidCompiledHandoffBehavior(action.Handoff)
				|| action.StoryboardIndex
					>= scope.CompiledProgram->Storyboards.size())
			{
				if (outError) *outError = L"Style Storyboard action 无效。";
				return false;
			}
			const auto storyboard = StyleStoryboardClockKey(
				scope, static_cast<uint64_t>(action.StoryboardIndex));
			switch (action.Kind)
			{
			case DeclarativeStoryboardActionKind::Begin:
			{
				std::vector<RuntimeAnimation> animations;
				std::vector<RuntimeTimelineGroup> timelineGroups;
				if (!TryBuildCompiledStyleStoryboard(
					scope, action.StoryboardIndex, animations,
					timelineGroups, outError))
					return false;
				for (const auto& animation : animations)
					CapturePropertySnapshot({ animation.Target,
						PropertyIdentity(animation.Metadata) },
						DependencyPropertyValueSource::Animation, snapshots);
				if (!BeginResolvedEventStoryboard(
					storyboard, animations, timelineGroups,
					scope.CompiledProgram->Storyboards[
						action.StoryboardIndex].Timing,
					action.Handoff, outError)) return false;
				break;
			}
			case DeclarativeStoryboardActionKind::Pause:
				(void)PauseEventStoryboard(storyboard);
				break;
			case DeclarativeStoryboardActionKind::Resume:
				(void)ResumeEventStoryboard(storyboard);
				break;
			case DeclarativeStoryboardActionKind::Stop:
				if (!StopEventStoryboard(storyboard))
				{
					if (outError)
						*outError = L"Style StopStoryboard 无法重组动画值。";
					return false;
				}
				break;
			case DeclarativeStoryboardActionKind::Remove:
				if (!RemoveEventStoryboard(storyboard))
				{
					if (outError)
						*outError = L"Style RemoveStoryboard 无法重组动画值。";
					return false;
				}
				break;
			case DeclarativeStoryboardActionKind::Seek:
				(void)SeekEventStoryboard(
					storyboard, action.SeekOffsetMilliseconds);
				break;
			case DeclarativeStoryboardActionKind::SetSpeedRatio:
				(void)SetSpeedRatioEventStoryboard(
					storyboard, action.SpeedRatio);
				break;
			case DeclarativeStoryboardActionKind::SkipToFill:
				(void)SkipEventStoryboardToFill(storyboard);
				break;
			default:
				return false;
			}
		}
		committed = true;
		if (outError) outError->clear();
		return true;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	size_t AllocateStyleStoryboardIndex()
	{
		if (!FreeStyleStoryboardIndices.empty())
		{
			const auto index = FreeStyleStoryboardIndices.back();
			FreeStyleStoryboardIndices.pop_back();
			return index;
		}
		EventStoryboards.emplace_back();
		return EventStoryboards.size() - 1;
	}

	bool ReleaseStyleStoryboardIndex(
		size_t index,
		const RuntimeStyleTriggerScope& scope)
	{
		if (index >= EventStoryboards.size()) return false;
		if (!RemoveEventStoryboard(StyleStoryboardClockKey(
			scope, static_cast<uint64_t>(index)))) return false;
		EventStoryboards[index] = {};
		if (std::find(FreeStyleStoryboardIndices.begin(),
			FreeStyleStoryboardIndices.end(), index)
			== FreeStyleStoryboardIndices.end())
			FreeStyleStoryboardIndices.push_back(index);
		return true;
	}
#endif

	bool CompileStyleTriggerScope(
		const ResolvedControlStyleTrigger& source,
		RuntimeStyleTriggerScope& scope,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = L"Style DataTrigger：" + std::move(message);
				return false;
			};
		if (source.CompiledProgram)
		{
			const auto& program = *source.CompiledProgram;
			auto validRange = [](CompiledStyleRange range, size_t size)
				{
					return range.Offset <= size
						&& range.Count <= size - range.Offset;
				};
			if (!Owner || scope.ClockOwner.Value == 0
				|| program.Version != CompiledStyleProgramViewVersion)
				return fail(L"编译 Style action 程序版本无效。");
			if (!validRange(source.CompiledEnterActions, program.Actions.size())
				|| !validRange(
					source.CompiledExitActions, program.Actions.size()))
				return fail(L"编译 Style action range 越界。");
			scope.CompiledProgram = source.CompiledProgram;
			scope.CompiledValues = source.CompiledValues;
			scope.CompiledEnterActions = source.CompiledEnterActions;
			scope.CompiledExitActions = source.CompiledExitActions;
			if (outError) outError->clear();
			return true;
		}
#if CUI_ENABLE_DYNAMIC_XAML
		auto commitScope = [&] (
			std::vector<RuntimeEventStoryboard>&& storyboards,
			std::vector<RuntimeEventTriggerAction>&& enterActions,
			std::vector<RuntimeEventTriggerAction>&& exitActions)
			{
				static_assert(std::is_nothrow_move_assignable_v<
					RuntimeEventStoryboard>);
				static_assert(std::is_nothrow_move_assignable_v<
					std::vector<RuntimeEventTriggerAction>>);
				auto indices = scope.StoryboardIndices;
				const size_t originalStoryboardCount = EventStoryboards.size();
				const size_t additionalCount = storyboards.size() > indices.size()
					? storyboards.size() - indices.size() : 0;
				indices.reserve(storyboards.size());
				EventStoryboards.reserve(
					EventStoryboards.size() + additionalCount);
				FreeStyleStoryboardIndices.reserve(
					FreeStyleStoryboardIndices.size()
						+ indices.size() + additionalCount);
				auto originalFreeIndices = FreeStyleStoryboardIndices;
				std::vector<std::pair<size_t, RuntimeEventStoryboard>>
					originalStoryboards;
				originalStoryboards.reserve(indices.size() + additionalCount);
#if CUI_ENABLE_DYNAMIC_XAML
				const bool shrinking = storyboards.size() < indices.size();
				std::vector<CandidateAnimation> originalActiveAnimations;
				std::vector<ActiveClockRoot> originalActiveRoots;
				std::vector<PropertySnapshot> animationSnapshots;
				const bool previousApplying = Applying;
				if (shrinking)
				{
					if (!SnapshotClockCandidates(originalActiveAnimations))
						return false;
					originalActiveRoots = ClockNodes.Roots;
					animationSnapshots.reserve(ClockNodes.Leaves.size());
					for (const auto& animation : ClockNodes.Leaves)
					{
						PropertyKey key{ animation.Target,
							PropertyIdentity(animation.Metadata) };
						if (!key.Target || !key.Property
							|| std::any_of(animationSnapshots.begin(),
								animationSnapshots.end(), [&](const auto& existing)
								{ return SameProperty(existing.Key, key); }))
							continue;
						PropertySnapshot snapshot;
						snapshot.Key = key;
						snapshot.Source =
							DependencyPropertyValueSource::Animation;
						BindingValue value;
						if (key.Target->TryGetPropertyValue(
							*key.Property, snapshot.Source, value))
							snapshot.Value = std::move(value);
						animationSnapshots.push_back(std::move(snapshot));
					}
				}
#endif
				bool committed = false;
				ControlScopeExit rollback{ [&]
					{
						if (committed) return;
						for (auto& entry : originalStoryboards)
							if (entry.first < EventStoryboards.size())
								EventStoryboards[entry.first] =
									std::move(entry.second);
						if (EventStoryboards.size() > originalStoryboardCount)
							EventStoryboards.resize(originalStoryboardCount);
						FreeStyleStoryboardIndices =
							std::move(originalFreeIndices);
#if CUI_ENABLE_DYNAMIC_XAML
						if (shrinking)
						{
							Applying = true;
							(void)CommitClockCandidates(
								std::move(originalActiveAnimations),
								std::move(originalActiveRoots));
							(void)RestoreSnapshots(animationSnapshots);
							Applying = previousApplying;
						}
#endif
					} };
				while (indices.size() < storyboards.size())
					indices.push_back(AllocateStyleStoryboardIndex());
				for (const auto index : indices)
				{
					if (index >= originalStoryboardCount
						|| std::any_of(originalStoryboards.begin(),
							originalStoryboards.end(), [&](const auto& entry)
							{ return entry.first == index; })) continue;
					originalStoryboards.emplace_back(
						index, std::move(EventStoryboards[index]));
				}
				for (size_t index = 0; index < storyboards.size(); ++index)
					EventStoryboards[indices[index]] =
						std::move(storyboards[index]);
				auto mapIndices = [&](std::vector<RuntimeEventTriggerAction>& actions)
					{
						for (auto& action : actions)
							if (action.StoryboardIndex < storyboards.size())
								action.StoryboardIndex =
									indices[action.StoryboardIndex];
					};
				mapIndices(enterActions);
				mapIndices(exitActions);
				for (size_t index = storyboards.size(); index < indices.size(); ++index)
					if (!ReleaseStyleStoryboardIndex(indices[index], scope))
					{
						if (outError) *outError =
							L"Style Storyboard scope 缩容时无法释放动画时钟。";
						return false;
					}
				indices.resize(storyboards.size());
				scope.StoryboardIndices = std::move(indices);
				scope.EnterActions = std::move(enterActions);
				scope.ExitActions = std::move(exitActions);
				committed = true;
				if (outError) outError->clear();
				return true;
			};
		std::vector<RuntimeEventStoryboard> storyboards;
		std::vector<RuntimeEventTriggerAction> enterActions;
		std::vector<RuntimeEventTriggerAction> exitActions;
		std::vector<std::wstring> beginNames;
		auto compileActions = [&](
			const std::vector<DeclarativeEventTriggerActionDefinition>& definitions,
			std::vector<RuntimeEventTriggerAction>& output) -> bool
			{
				for (const auto& definition : definitions)
				{
					RuntimeEventTriggerAction action;
					action.Kind = definition.Kind;
					action.Handoff = definition.Handoff;
					action.SeekOffsetMilliseconds =
						definition.SeekOffsetMilliseconds;
					action.SpeedRatio = definition.SpeedRatio;
					if (definition.Kind == DeclarativeStoryboardActionKind::Begin)
					{
						if (!definition.StoryboardName.empty()
							&& ContainsName(beginNames, definition.StoryboardName))
							return fail(L"BeginStoryboard x:Name 重复："
								+ definition.StoryboardName);
						if (definition.Animations.empty()
							&& definition.TimelineGroups.empty())
							return fail(L"BeginStoryboard 的 Storyboard 不能为空。");
						RuntimeEventStoryboard storyboard;
						storyboard.Name = definition.StoryboardName;
						storyboard.Timing = definition.StoryboardTiming;
						storyboard.IsStyleStoryboard = true;
						storyboard.Handoff = definition.Handoff;
						struct PropertyOwnership
						{
							PropertyKey Root;
							bool Exclusive = false;
							std::vector<uint64_t> PathIdentities;
						};
						std::vector<PropertyOwnership> properties;
						for (const auto& sourceAnimation : definition.Animations)
						{
							if (sourceAnimation.Target
								&& sourceAnimation.Target != Owner)
								return fail(L"Style Storyboard 只能以样式宿主为目标。");
#if CUI_ENABLE_DYNAMIC_XAML
							if (!sourceAnimation.TargetName.empty())
								return fail(L"Style Storyboard 不支持 TargetName："
									+ sourceAnimation.TargetName);
#endif
							RuntimeAnimation animation;
							if (!TryBuildAnimation(sourceAnimation, animation,
								L"Style BeginStoryboard", outError)) return false;
							PropertyKey key{
								animation.Target, PropertyIdentity(animation.Metadata) };
							const auto pathIdentity =
								ObjectPathIdentity(animation.ObjectPath);
							auto owner = std::find_if(properties.begin(), properties.end(),
								[&](const auto& existing)
								{ return SameProperty(existing.Root, key); });
							if (owner != properties.end())
							{
								if (pathIdentity == 0 || owner->Exclusive
									|| ContainsObjectPathIdentity(
										owner->PathIdentities, pathIdentity))
									return fail(L"BeginStoryboard 目标重复："
										+ sourceAnimation.PropertyPath());
								owner->PathIdentities.push_back(pathIdentity);
							}
							else
							{
								PropertyOwnership ownership;
								ownership.Root = key;
								ownership.Exclusive = pathIdentity == 0;
								if (pathIdentity != 0)
									ownership.PathIdentities.push_back(pathIdentity);
								properties.push_back(std::move(ownership));
							}
							storyboard.Animations.push_back(std::move(animation));
						}
						const auto nestedAnimationOffset =
							storyboard.Animations.size();
						if (!AppendDeclarativeTimelineGroups(
							definition.TimelineGroups,
							CompiledInteractionInvalidIndex,
							storyboard.Animations, storyboard.TimelineGroups,
							L"Style BeginStoryboard", outError)) return false;
						for (size_t animationIndex = nestedAnimationOffset;
							animationIndex < storyboard.Animations.size();
							++animationIndex)
						{
							const auto& animation =
								storyboard.Animations[animationIndex];
							PropertyKey key{ animation.Target,
								PropertyIdentity(animation.Metadata) };
							const auto pathIdentity =
								ObjectPathIdentity(animation.ObjectPath);
							auto owner = std::find_if(properties.begin(),
								properties.end(), [&](const auto& existing)
								{ return SameProperty(existing.Root, key); });
							if (owner != properties.end())
							{
								if (pathIdentity == 0 || owner->Exclusive
									|| ContainsObjectPathIdentity(
										owner->PathIdentities, pathIdentity))
									return fail(L"BeginStoryboard 目标重复："
										+ animation.Metadata->Name());
								owner->PathIdentities.push_back(pathIdentity);
							}
							else
							{
								PropertyOwnership ownership;
								ownership.Root = key;
								ownership.Exclusive = pathIdentity == 0;
								if (pathIdentity != 0)
									ownership.PathIdentities.push_back(pathIdentity);
								properties.push_back(std::move(ownership));
							}
						}
						action.StoryboardIndex = storyboards.size();
						storyboards.push_back(std::move(storyboard));
						if (!definition.StoryboardName.empty())
							beginNames.push_back(definition.StoryboardName);
					}
					else
					{
						if (definition.StoryboardName.empty())
							return fail(L"Storyboard 控制动作缺少 BeginStoryboardName。");
						action.PendingStoryboardName = definition.StoryboardName;
					}
					output.push_back(std::move(action));
				}
				return true;
			};
		if (!compileActions(source.EnterActions, enterActions)
			|| !compileActions(source.ExitActions, exitActions)) return false;
		auto resolveReferences = [&](std::vector<RuntimeEventTriggerAction>& actions)
			{
				for (auto& action : actions)
				{
					if (action.Kind == DeclarativeStoryboardActionKind::Begin) continue;
					const auto found = std::find_if(storyboards.begin(), storyboards.end(),
						[&](const auto& storyboard)
						{ return EqualName(storyboard.Name,
							action.PendingStoryboardName); });
					if (found == storyboards.end())
						return fail(L"Storyboard 控制动作找不到 BeginStoryboard："
							+ action.PendingStoryboardName);
					if (action.Kind == DeclarativeStoryboardActionKind::SkipToFill
						&& (found->Timing.RepeatBehavior
								== DeclarativeRepeatBehaviorKind::Forever
							|| std::any_of(found->Animations.begin(),
							found->Animations.end(), [](const auto& animation)
							{ return animation.RepeatBehavior
								== DeclarativeRepeatBehaviorKind::Forever; })
							|| TimelineGroupsContainForever(
								found->TimelineGroups)))
						return fail(L"SkipStoryboardToFill 不能引用 Forever Storyboard。");
					action.StoryboardIndex = static_cast<size_t>(
						std::distance(storyboards.begin(), found));
					action.PendingStoryboardName.clear();
				}
				return true;
			};
		if (!resolveReferences(enterActions)
			|| !resolveReferences(exitActions)) return false;

		return commitScope(
			std::move(storyboards),
			std::move(enterActions),
			std::move(exitActions));
#else
		return fail(L"Production Style trigger 缺少编译 action 程序。");
#endif
	}

	bool RemoveStyleTriggerScope(size_t index)
	{
		if (index >= StyleTriggerScopes.size()) return false;
		const auto& scope = StyleTriggerScopes[index];
		std::vector<StoryboardClockKey> storyboards;
		if (scope.CompiledProgram)
		{
			storyboards.reserve(scope.CompiledProgram->Storyboards.size());
			for (size_t storyboardIndex = 0;
				storyboardIndex < scope.CompiledProgram->Storyboards.size();
				++storyboardIndex)
				storyboards.push_back(StyleStoryboardClockKey(
					scope, static_cast<uint64_t>(storyboardIndex)));
		}
#if CUI_ENABLE_DYNAMIC_XAML
		else
		{
			FreeStyleStoryboardIndices.reserve(
				FreeStyleStoryboardIndices.size() + scope.StoryboardIndices.size());
			for (const auto storyboardIndex : scope.StoryboardIndices)
			{
				if (storyboardIndex >= EventStoryboards.size()) return false;
				storyboards.push_back(StyleStoryboardClockKey(
					scope, static_cast<uint64_t>(storyboardIndex)));
			}
		}
#endif
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> activeAnimations;
		if (!SnapshotClockCandidates(activeAnimations)) return false;
		auto activeRoots = ClockNodes.Roots;
		const bool previousApplying = Applying;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (committed) return;
				RestoreActiveAnimationTransaction(
					activeAnimations, activeRoots,
					snapshots, previousApplying);
			} };
		for (const auto& storyboard : storyboards)
			if (!RemoveEventStoryboard(storyboard))
				return false;
#if CUI_ENABLE_DYNAMIC_XAML
		if (!scope.CompiledProgram)
			for (const auto storyboardIndex : scope.StoryboardIndices)
			{
				EventStoryboards[storyboardIndex] = {};
				if (std::find(FreeStyleStoryboardIndices.begin(),
					FreeStyleStoryboardIndices.end(), storyboardIndex)
					== FreeStyleStoryboardIndices.end())
					FreeStyleStoryboardIndices.push_back(storyboardIndex);
			}
#endif
		StyleTriggerScopes.erase(StyleTriggerScopes.begin() + index);
		committed = true;
		return true;
	}

	bool SynchronizeStyleTriggerActions(
		DependencyPropertyValueSource source,
		const std::shared_ptr<const ControlStyleSheet>& sheet,
		const std::vector<ResolvedControlStyleTrigger>& triggers,
		std::wstring* outError)
	{
		bool success = true;
		for (const auto& trigger : triggers)
		{
			auto found = std::find_if(StyleTriggerScopes.begin(),
				StyleTriggerScopes.end(), [&](const auto& existing)
				{
					return existing.Source == source
						&& existing.Sheet.get() == sheet.get()
						&& existing.RuleId == trigger.RuleId;
				});
			if (found == StyleTriggerScopes.end())
			{
				// A missing compiled scope represents the inactive state.  Do not
				// materialize per-control state until the first real edge.
				if (trigger.CompiledProgram && !trigger.IsActive) continue;
				static_assert(std::is_nothrow_move_constructible_v<
					RuntimeStyleTriggerScope>);
				StyleTriggerScopes.reserve(StyleTriggerScopes.size() + 1);
				RuntimeStyleTriggerScope scope;
				scope.Source = source;
				scope.Sheet = sheet;
				scope.RuleId = trigger.RuleId;
				if (!TryAllocateStyleOwnerScope(scope.ClockOwner))
				{
					if (outError)
						*outError = L"Style DataTrigger owner scope 已耗尽。";
					success = false;
					continue;
				}
				if (!CompileStyleTriggerScope(trigger, scope, outError))
				{
					success = false;
					continue;
				}
				StyleTriggerScopes.push_back(std::move(scope));
				found = std::prev(StyleTriggerScopes.end());
			}
			else if (!trigger.CompiledProgram
				&& !CompileStyleTriggerScope(trigger, *found, outError))
			{
				success = false;
				continue;
			}
			if (found->Active == trigger.IsActive) continue;
			const bool next = trigger.IsActive;
			bool executed = false;
			if (found->CompiledProgram)
				executed = ExecuteCompiledStyleTriggerActions(
					*found, next
						? found->CompiledEnterActions
						: found->CompiledExitActions,
					outError);
#if CUI_ENABLE_DYNAMIC_XAML
			else
				executed = ExecuteStyleTriggerActions(
					*found,
					next ? found->EnterActions : found->ExitActions,
					outError);
#endif
			if (!executed)
				success = false;
			else found->Active = next;
		}
		for (size_t index = StyleTriggerScopes.size(); index-- > 0;)
		{
			const auto& scope = StyleTriggerScopes[index];
			if (scope.Source != source || scope.Sheet.get() != sheet.get()) continue;
			if (std::none_of(triggers.begin(), triggers.end(),
				[&](const auto& trigger) { return trigger.RuleId == scope.RuleId; }))
				if (!RemoveStyleTriggerScope(index)) success = false;
		}
		if (success && outError) outError->clear();
		return success;
	}

	bool PruneStyleTriggerActions(
		DependencyPropertyValueSource source,
		const std::vector<const ControlStyleSheet*>& visibleSheets)
	{
		std::vector<PropertySnapshot> snapshots;
		CaptureActiveAnimationSnapshots(ClockNodes.Leaves, snapshots);
		std::vector<CandidateAnimation> activeAnimations;
		if (!SnapshotClockCandidates(activeAnimations)) return false;
		auto activeRoots = ClockNodes.Roots;
		auto styleTriggerScopes = StyleTriggerScopes;
#if CUI_ENABLE_DYNAMIC_XAML
		auto eventStoryboards = EventStoryboards;
		auto freeStyleStoryboardIndices = FreeStyleStoryboardIndices;
#endif
		const bool previousApplying = Applying;
		bool committed = false;
		ControlScopeExit rollback{ [&]
			{
				if (committed) return;
				StyleTriggerScopes = std::move(styleTriggerScopes);
#if CUI_ENABLE_DYNAMIC_XAML
				EventStoryboards = std::move(eventStoryboards);
				FreeStyleStoryboardIndices =
					std::move(freeStyleStoryboardIndices);
#endif
				RestoreActiveAnimationTransaction(
					activeAnimations, activeRoots,
					snapshots, previousApplying);
			} };
		for (size_t index = StyleTriggerScopes.size(); index-- > 0;)
		{
			const auto& scope = StyleTriggerScopes[index];
			if (scope.Source != source) continue;
			if (std::find(visibleSheets.begin(), visibleSheets.end(), scope.Sheet.get())
				== visibleSheets.end())
				if (!RemoveStyleTriggerScope(index)) return false;
		}
		committed = true;
		return true;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	void ResetFailedDeclarativeInteractionBuild()
	{
		Connections.clear();
		ClearAppliedValues();
		ClockNodes.Leaves.erase(std::remove_if(
			ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](const auto& animation)
			{ return IsVisualStateClock(animation); }),
			ClockNodes.Leaves.end());
		ClockNodes.Roots.erase(std::remove_if(
			ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
			[](const auto& root) { return IsVisualStateClock(root); }),
			ClockNodes.Roots.end());
		Groups.clear();
		EventTriggers.clear();
		for (auto& storyboard : EventStoryboards)
			if (!storyboard.IsStyleStoryboard) storyboard = {};
		ClockNodesDirty = true;
		PublishClockRootRegistry();
		(void)ApplyRetainedAnimationFrame(NowMilliseconds());
		DeclarativeInteractionsDefined = false;
		InstallingInteractions = false;
		SuppressStateChangedEvents = false;
	}
#endif

	void ResetFailedCompiledInteractionBuild()
	{
		// Compiled EventTriggers are subscribed only after every initial state has
		// committed, so a failed install cannot have started one of its event clocks.
		// Preserve pre-existing Style clocks and restore the two DP sources touched
		// by earlier groups in the failed initial-state transaction.
		Connections.clear();
		ClearAppliedValues(false);
		ClockNodes.Leaves.erase(std::remove_if(
			ClockNodes.Leaves.begin(), ClockNodes.Leaves.end(),
			[&](const auto& animation)
			{
				const auto* root = ResolveClockRoot(animation);
				return !root || IsVisualStateClock(*root)
					|| root->Identity.Storyboard.Owner.Kind
						== ClockOwnerKind::CompiledInteractionStoryboard;
			}),
			ClockNodes.Leaves.end());
		ClockNodes.Roots.erase(std::remove_if(
			ClockNodes.Roots.begin(), ClockNodes.Roots.end(),
			[](const auto& root)
			{
				return IsVisualStateClock(root)
					|| root.Identity.Storyboard.Owner.Kind
						== ClockOwnerKind::CompiledInteractionStoryboard;
			}), ClockNodes.Roots.end());
		Groups.clear();
	#if CUI_ENABLE_DYNAMIC_XAML
		EventTriggers.clear();
	#endif
		(void)RestoreSnapshots(FailedCompiledSnapshots);
		FailedCompiledSnapshots.clear();
		CompiledInteractions.reset();
		ClockNodesDirty = true;
		PublishClockRootRegistry();
		(void)ApplyRetainedAnimationFrame(NowMilliseconds());
		DeclarativeInteractionsDefined = false;
		InstallingInteractions = false;
		SuppressStateChangedEvents = false;
	}

	void OnHostDeclarativeEvent(DeclarativeEventArgs& args)
	{
		if (Applying || args.OriginalSource != Owner || !args.Definition) return;
		for (size_t groupIndex = 0; groupIndex < Groups.size(); ++groupIndex)
		{
			auto& group = Groups[groupIndex];
			if (const auto* compiledGroup = CompiledGroupAt(groupIndex))
			{
				if (!CompiledInteractions) continue;
				const auto& program = CompiledInteractions->Program;
				for (size_t stateIndex = 0;
					stateIndex < compiledGroup->States.Count; ++stateIndex)
				{
					const auto absolute = static_cast<size_t>(
						compiledGroup->States.Offset) + stateIndex;
					if (absolute >= program.States.size()) break;
					const auto& state = program.States[absolute];
					if (!ValidCompiledRange(state.Events, program.StateEvents.size()))
						continue;
					const auto first = program.StateEvents.begin() + state.Events.Offset;
					const auto last = first + state.Events.Count;
					if (std::find(first, last, args.Definition) == last) continue;
					(void)GoTo(groupIndex, stateIndex, true, nullptr);
					break;
				}
				continue;
			}
#if CUI_ENABLE_DYNAMIC_XAML
			for (size_t stateIndex = 0;
				stateIndex < group.States.size(); ++stateIndex)
				if (std::find(group.States[stateIndex].Events.begin(),
					group.States[stateIndex].Events.end(), args.Definition)
					!= group.States[stateIndex].Events.end())
				{
					(void)GoTo(groupIndex, stateIndex, true, nullptr);
					break;
				}
#endif
		}
	#if CUI_ENABLE_DYNAMIC_XAML
		for (const auto& trigger : EventTriggers)
			if (trigger.Event == args.Definition)
				for (const auto& action : trigger.Actions)
					ExecuteEventTriggerAction(action);
	#endif
		if (CompiledInteractions)
			for (const auto& trigger : CompiledInteractions->Program.EventTriggers)
				if (trigger.Event == args.Definition)
					ExecuteCompiledEventTriggerActions(trigger.Actions);
	}

	void OnHostRoutedEvent(RoutedEventArgs& args)
	{
		if (Applying || args.OriginalSource != Owner) return;
	#if CUI_ENABLE_DYNAMIC_XAML
		for (const auto& trigger : EventTriggers)
			if (trigger.RoutedEvent == args.EventId)
				for (const auto& action : trigger.Actions)
					ExecuteEventTriggerAction(action);
	#endif
		if (CompiledInteractions)
			for (const auto& trigger : CompiledInteractions->Program.EventTriggers)
				if (trigger.RoutedEvent == args.EventId)
					ExecuteCompiledEventTriggerActions(trigger.Actions);
	}

	bool TryBuildResolvedAnimation(
		const RuntimeAnimationDefinition& sourceAnimation,
		Control* target,
		const DependencyPropertyMetadata* metadata,
		std::optional<ObjectPathAccessor> objectPath,
		std::wstring propertyPath,
		RuntimeAnimation& animation,
		const std::wstring& context,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = context + L"：" + std::move(message);
				return false;
			};
		// The target and DependencyProperty metadata are the resolved runtime
		// identity. Production deliberately carries no authored property name;
		// propertyPath is diagnostic context only and must not gate execution.
		if (!target || !metadata)
			return fail(L"Storyboard 已解析目标无效。");
		if (sourceAnimation.Path.Enabled)
		{
			const auto& path = sourceAnimation.Path;
			if ((sourceAnimation.Kind != DeclarativeAnimationKind::Double
					&& sourceAnimation.Kind != DeclarativeAnimationKind::Point
					&& sourceAnimation.Kind != DeclarativeAnimationKind::Matrix)
				|| !std::isfinite(path.Start.x) || !std::isfinite(path.Start.y)
				|| sourceAnimation.PathSegments.empty()
				|| static_cast<unsigned char>(path.Source)
					> static_cast<unsigned char>(
						DeclarativePathAnimationSource::Angle)
				|| sourceAnimation.From || sourceAnimation.To || sourceAnimation.By
				|| !sourceAnimation.KeyFrames.empty()
				|| sourceAnimation.Easing != DeclarativeEasingKind::Linear)
				return fail(L"UsingPath 定义无效。");
			for (const auto& segment : sourceAnimation.PathSegments)
				if (static_cast<unsigned char>(segment.Kind)
						> static_cast<unsigned char>(
							DeclarativePathSegmentKind::CubicBezier)
					|| !std::isfinite(segment.Point1.x)
					|| !std::isfinite(segment.Point1.y)
					|| !std::isfinite(segment.Point2.x)
					|| !std::isfinite(segment.Point2.y)
					|| !std::isfinite(segment.Point3.x)
					|| !std::isfinite(segment.Point3.y))
					return fail(L"UsingPath segment 无效。");
			if (sourceAnimation.Kind != DeclarativeAnimationKind::Double
				&& path.Source != DeclarativePathAnimationSource::X)
				return fail(L"仅 DoubleAnimationUsingPath 可声明 Source。");
			if (sourceAnimation.Kind != DeclarativeAnimationKind::Matrix
				&& (path.DoesRotateWithTangent || path.IsOffsetCumulative
					|| path.IsAngleCumulative))
				return fail(L"仅 MatrixAnimationUsingPath 可声明矩阵路径标志。");
			if (sourceAnimation.Kind == DeclarativeAnimationKind::Matrix
				&& sourceAnimation.IsCumulative)
				return fail(L"MatrixAnimationUsingPath 不使用 IsCumulative。");
		}
		else if (!sourceAnimation.PathSegments.empty())
			return fail(L"非路径动画不能携带 path segment。");
		BindingValue convertedScratch;
		auto validTypedAnimationValue = [&](const BindingValue& value)
			{
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Point)
				{
					cui::core::Point point{};
					return value.TryGet(point)
						&& std::isfinite(point.x) && std::isfinite(point.y);
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Vector)
				{
					cui::core::Vector vector{};
					return value.TryGet(vector)
						&& std::isfinite(vector.x) && std::isfinite(vector.y);
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Rect)
				{
					cui::core::Rect rect{};
					return value.TryGet(rect)
						&& std::isfinite(rect.x) && std::isfinite(rect.y)
						&& std::isfinite(rect.width) && std::isfinite(rect.height)
						&& rect.width >= 0.0f && rect.height >= 0.0f;
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Size)
				{
					cui::core::Size size{};
					return value.TryGet(size)
						&& std::isfinite(size.width) && std::isfinite(size.height)
						&& size.width >= 0.0f && size.height >= 0.0f;
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Matrix)
				{
					D2D1_MATRIX_3X2_F matrix{};
					return value.TryGet(matrix) && IsFiniteMatrix(matrix);
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Thickness)
				{
					Thickness thickness;
					return value.TryGet(thickness)
						&& std::isfinite(thickness.Left)
						&& std::isfinite(thickness.Top)
						&& std::isfinite(thickness.Right)
						&& std::isfinite(thickness.Bottom);
				}
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Color)
				{
					D2D1_COLOR_F color{};
					return value.TryGet(color)
						&& std::isfinite(color.r) && std::isfinite(color.g)
						&& std::isfinite(color.b) && std::isfinite(color.a);
				}
				return true;
			};
		auto convertEndpoint = [&](const BindingValue& source,
			BindingValue& output, bool isDelta = false)
			{
				if (!objectPath)
					return metadata->TryConvert(source, convertedScratch)
					&& validTypedAnimationValue(convertedScratch)
					&& metadata->TryCoerce(*target, convertedScratch, output);
				if (sourceAnimation.Kind != DeclarativeAnimationKind::Double)
				{
					if (!validTypedAnimationValue(source)) return false;
					if (const auto* pathGeometry = AsPathGeometryPath(objectPath);
						pathGeometry && sourceAnimation.Kind
						== DeclarativeAnimationKind::Object)
					{
						if (pathGeometry->Member == PathGeometryMember::ArcSweepDirection)
						{
							std::wstring sweep;
							if (!source.TryGet(sweep)
								|| (!EqualValueToken(sweep, L"Clockwise")
									&& !EqualValueToken(
										sweep, L"Counterclockwise")))
								return false;
						}
						else
						{
							bool flag = false;
							if (!source.TryGet(flag)) return false;
						}
					}
					if (const auto* geometry = AsGeometryPath(objectPath);
						geometry && geometry->Member == GeometryMember::FillRule
						&& sourceAnimation.Kind == DeclarativeAnimationKind::Object)
					{
						std::wstring fillRule;
						if (!source.TryGet(fillRule)
							|| (!EqualValueToken(fillRule, L"EvenOdd")
								&& !EqualValueToken(
									fillRule, L"Nonzero"))) return false;
					}
					output = source;
					return true;
				}
				double value = 0.0;
				if (!source.TryGetDouble(value) || !std::isfinite(value)
					|| value < -(std::numeric_limits<float>::max)()
					|| value >(std::numeric_limits<float>::max)()) return false;
				if (!isDelta)
				{
					if (const auto* brushPath = AsBrushPath(objectPath))
					{
						if ((brushPath->Member == BrushMember::GradientStopOffset
							|| brushPath->Member == BrushMember::Opacity)
							&& (value < 0.0 || value > 1.0)) return false;
						if ((brushPath->Member == BrushMember::RadiusX
							|| brushPath->Member == BrushMember::RadiusY)
							&& value < 0.0) return false;
					}
					if (const auto* geometryPath = AsGeometryPath(objectPath);
						geometryPath
						&& (geometryPath->Member == GeometryMember::RadiusX
							|| geometryPath->Member == GeometryMember::RadiusY)
						&& value < 0.0) return false;
				}
				output = BindingValue(static_cast<float>(value));
				return true;
			};
		std::optional<BindingValue> coercedFrom;
		std::optional<BindingValue> coercedTo;
		std::optional<BindingValue> coercedBy;
		std::vector<RuntimeAnimationKeyFrame> keyFrames;
		if (sourceAnimation.KeyFrames.empty())
		{
			if (sourceAnimation.To)
			{
				BindingValue value;
				if (!convertEndpoint(*sourceAnimation.To, value))
					return fail(L"动画 To 无效：" + propertyPath);
				coercedTo = std::move(value);
			}
			if (sourceAnimation.From)
			{
				BindingValue value;
				if (!convertEndpoint(*sourceAnimation.From, value))
					return fail(L"动画 From 无效："
						+ propertyPath);
				coercedFrom = std::move(value);
			}
			if (sourceAnimation.By)
			{
				BindingValue value;
				if (objectPath)
				{
					if (!convertEndpoint(*sourceAnimation.By, value, true))
						return fail(L"动画 By 无效："
							+ propertyPath);
				}
				else if (!metadata->TryConvert(*sourceAnimation.By, value)
					|| !validTypedAnimationValue(value))
					return fail(L"动画 By 无法转换为目标属性类型："
						+ propertyPath);
				coercedBy = std::move(value);
			}
		}
		else
		{
			if (sourceAnimation.From || sourceAnimation.To || sourceAnimation.By)
				return fail(L"关键帧动画不能同时声明 From/To/By："
					+ propertyPath);
			keyFrames.reserve(sourceAnimation.KeyFrames.size());
			for (const auto& sourceKeyFrame : sourceAnimation.KeyFrames)
			{
				RuntimeAnimationKeyFrame keyFrame{
					sourceKeyFrame.Kind,
					sourceKeyFrame.KeyTimeMilliseconds,
					sourceKeyFrame.Value,
					sourceKeyFrame.Easing,
					sourceKeyFrame.EasingMode,
					sourceKeyFrame.KeySplineX1,
					sourceKeyFrame.KeySplineY1,
					sourceKeyFrame.KeySplineX2,
					sourceKeyFrame.KeySplineY2,
					sourceKeyFrame.KeyTimeSubMillisecondTicks,
					sourceKeyFrame.EasingParameters };
				if (sourceAnimation.Kind == DeclarativeAnimationKind::Object
					&& keyFrame.Kind != DeclarativeKeyFrameKind::Discrete)
					return fail(L"ObjectAnimationUsingKeyFrames 只能包含 DiscreteObjectKeyFrame。");
				if (keyFrame.Kind != DeclarativeKeyFrameKind::Discrete
					&& keyFrame.Kind != DeclarativeKeyFrameKind::Linear
					&& keyFrame.Kind != DeclarativeKeyFrameKind::Easing
					&& keyFrame.Kind != DeclarativeKeyFrameKind::Spline)
					return fail(L"关键帧类型无效。");
				if (keyFrame.Kind == DeclarativeKeyFrameKind::Spline
					&& (!std::isfinite(keyFrame.KeySplineX1)
						|| !std::isfinite(keyFrame.KeySplineY1)
						|| !std::isfinite(keyFrame.KeySplineX2)
						|| !std::isfinite(keyFrame.KeySplineY2)
						|| keyFrame.KeySplineX1 < 0.0f
						|| keyFrame.KeySplineX1 > 1.0f
						|| keyFrame.KeySplineY1 < 0.0f
						|| keyFrame.KeySplineY1 > 1.0f
						|| keyFrame.KeySplineX2 < 0.0f
						|| keyFrame.KeySplineX2 > 1.0f
						|| keyFrame.KeySplineY2 < 0.0f
						|| keyFrame.KeySplineY2 > 1.0f))
					return fail(L"KeySpline 控制点必须位于 0..1。");
				BindingValue value;
				if (!convertEndpoint(keyFrame.Value, value))
					return fail(L"关键帧值无效："
						+ propertyPath);
				keyFrame.Value = std::move(value);
				keyFrames.push_back(std::move(keyFrame));
			}
			std::stable_sort(keyFrames.begin(), keyFrames.end(),
				[](const auto& left, const auto& right)
				{
					return std::tie(left.KeyTimeMilliseconds,
						left.KeyTimeSubMillisecondTicks)
						< std::tie(right.KeyTimeMilliseconds,
							right.KeyTimeSubMillisecondTicks);
				});
		}
		animation.Kind = sourceAnimation.Kind;
		animation.Target = target;
		animation.Metadata = metadata;
		animation.ObjectPath = std::move(objectPath);
		animation.From = std::move(coercedFrom);
		animation.To = std::move(coercedTo);
		animation.By = std::move(coercedBy);
		animation.IsAdditive = sourceAnimation.IsAdditive;
		animation.IsCumulative = sourceAnimation.IsCumulative;
		animation.Path = sourceAnimation.Path;
		animation.PathSegments = sourceAnimation.PathSegments;
		if (animation.Path.Enabled && !PrepareWpfPath(animation))
			return fail(L"UsingPath 无法建立WPF长度表。");
		animation.KeyFrames = std::move(keyFrames);
		animation.BeginTimeMilliseconds = sourceAnimation.BeginTimeMilliseconds;
		animation.DurationMilliseconds = sourceAnimation.DurationMilliseconds;
		if (sourceAnimation.Kind == DeclarativeAnimationKind::Object)
		{
			if (sourceAnimation.KeyFrames.empty())
				return fail(L"ObjectAnimationUsingKeyFrames 至少需要一个关键帧。");
			if (sourceAnimation.IsAdditive || sourceAnimation.IsCumulative)
				return fail(L"ObjectAnimationUsingKeyFrames 不支持 IsAdditive/IsCumulative。");
			if (sourceAnimation.Easing != DeclarativeEasingKind::Linear)
				return fail(L"ObjectAnimationUsingKeyFrames 不支持 EasingFunction。");
		}
		if (sourceAnimation.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Count)
		{
			if (!std::isfinite(sourceAnimation.RepeatCount)
				|| sourceAnimation.RepeatCount <= 0.0)
				return fail(L"RepeatBehavior Count 必须是有限正数。");
		}
		else if (sourceAnimation.RepeatBehavior
			== DeclarativeRepeatBehaviorKind::Duration)
		{
			if (sourceAnimation.RepeatDurationMilliseconds == 0)
				return fail(L"RepeatBehavior Duration 必须大于零。");
		}
		else if (sourceAnimation.RepeatBehavior
			!= DeclarativeRepeatBehaviorKind::Forever)
			return fail(L"RepeatBehavior 类型无效。");
		if (sourceAnimation.FillBehavior
			!= DeclarativeTimelineFillBehavior::HoldEnd
			&& sourceAnimation.FillBehavior
			!= DeclarativeTimelineFillBehavior::Stop)
			return fail(L"FillBehavior 类型无效。");
		if (!std::isfinite(sourceAnimation.SpeedRatio)
			|| sourceAnimation.SpeedRatio <= 0.0)
			return fail(L"SpeedRatio 必须是有限正数。");
		if (!std::isfinite(sourceAnimation.AccelerationRatio)
			|| sourceAnimation.AccelerationRatio < 0.0
			|| sourceAnimation.AccelerationRatio > 1.0)
			return fail(L"AccelerationRatio 必须位于 0..1。");
		if (!std::isfinite(sourceAnimation.DecelerationRatio)
			|| sourceAnimation.DecelerationRatio < 0.0
			|| sourceAnimation.DecelerationRatio > 1.0)
			return fail(L"DecelerationRatio 必须位于 0..1。");
		if (sourceAnimation.AccelerationRatio
			+ sourceAnimation.DecelerationRatio > 1.0)
			return fail(L"AccelerationRatio 与 DecelerationRatio 之和不能超过 1。");
		animation.RepeatBehavior = sourceAnimation.RepeatBehavior;
		animation.RepeatCount = sourceAnimation.RepeatCount;
		animation.RepeatDurationMilliseconds =
			sourceAnimation.RepeatDurationMilliseconds;
		animation.AutoReverse = sourceAnimation.AutoReverse;
		animation.FillBehavior = sourceAnimation.FillBehavior;
		animation.SpeedRatio = sourceAnimation.SpeedRatio;
		animation.AccelerationRatio = sourceAnimation.AccelerationRatio;
		animation.DecelerationRatio = sourceAnimation.DecelerationRatio;
		animation.Easing = sourceAnimation.Easing;
		animation.EasingMode = sourceAnimation.EasingMode;
		animation.EasingParameters = sourceAnimation.EasingParameters;
		if (outError) outError->clear();
		return true;
	}

#if CUI_ENABLE_DYNAMIC_XAML
	bool TryBuildAnimation(
		const DeclarativeVisualStateAnimation& sourceAnimation,
		RuntimeAnimation& animation,
		const std::wstring& context,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = context + L"：" + std::move(message);
				return false;
			};
		Control* target = nullptr;
		const DependencyPropertyMetadata* metadata = nullptr;
		std::optional<ObjectPathAccessor> objectPath;
		std::optional<CompiledStoryboardObjectPathOp> compiledObjectPath;
		std::vector<uint32_t> objectPathChildIndices;
		std::wstring propertyPath;
		std::wstring resolutionError;
		if (!Owner || !cui::framework::design::ResolveVisualStateAnimationOperands(
			sourceAnimation, *Owner, target, metadata, compiledObjectPath,
			objectPathChildIndices, propertyPath, &resolutionError))
			return fail(std::move(resolutionError));
		if (!compiledObjectPath)
		{
			if (!metadata || !metadata->CanWrite()
				|| !AnimationMatchesMetadata(sourceAnimation.Kind, *metadata))
				return fail(L"Storyboard.TargetProperty 属性不存在、只读或动画类型不匹配："
					+ propertyPath);
		}
		else
		{
			std::wstring pathError;
			ObjectPathAccessor accessor;
			if (!metadata || !TryResolveCompiledObjectPath(*target,
				*compiledObjectPath, objectPathChildIndices, *metadata,
				sourceAnimation.Kind, accessor, &pathError))
				return fail(pathError + L"：" + propertyPath);
			objectPath = std::move(accessor);
		}

		RuntimeAnimationDefinition definition;
		definition.Kind = sourceAnimation.Kind;
		definition.From = sourceAnimation.From;
		definition.To = sourceAnimation.To;
		definition.By = sourceAnimation.By;
		definition.IsAdditive = sourceAnimation.IsAdditive;
		definition.IsCumulative = sourceAnimation.IsCumulative;
		definition.Path = sourceAnimation.Path;
		definition.PathSegments = sourceAnimation.PathSegments;
		definition.KeyFrames.reserve(sourceAnimation.KeyFrames.size());
		for (const auto& keyFrame : sourceAnimation.KeyFrames)
			definition.KeyFrames.push_back({
				keyFrame.Kind,
				keyFrame.KeyTimeMilliseconds,
				keyFrame.Value,
				keyFrame.Easing,
				keyFrame.EasingMode,
				keyFrame.KeySplineX1,
				keyFrame.KeySplineY1,
				keyFrame.KeySplineX2,
				keyFrame.KeySplineY2,
				keyFrame.KeyTimeSubMillisecondTicks,
				keyFrame.EasingParameters });
		definition.BeginTimeMilliseconds =
			sourceAnimation.BeginTimeMilliseconds;
		definition.DurationMilliseconds = sourceAnimation.DurationMilliseconds;
		definition.RepeatBehavior = sourceAnimation.RepeatBehavior;
		definition.RepeatCount = sourceAnimation.RepeatCount;
		definition.RepeatDurationMilliseconds =
			sourceAnimation.RepeatDurationMilliseconds;
		definition.AutoReverse = sourceAnimation.AutoReverse;
		definition.FillBehavior = sourceAnimation.FillBehavior;
		definition.SpeedRatio = sourceAnimation.SpeedRatio;
		definition.AccelerationRatio = sourceAnimation.AccelerationRatio;
		definition.DecelerationRatio = sourceAnimation.DecelerationRatio;
		definition.Easing = sourceAnimation.Easing;
		definition.EasingMode = sourceAnimation.EasingMode;
		definition.EasingParameters = sourceAnimation.EasingParameters;
		return TryBuildResolvedAnimation(
			definition, target, metadata, std::move(objectPath), propertyPath,
			animation, context, outError);
	}

	bool AppendDeclarativeTimelineGroups(
		const std::vector<DeclarativeTimelineGroupDefinition>& sources,
		uint32_t parentIndex,
		std::vector<RuntimeAnimation>& animations,
		std::vector<RuntimeTimelineGroup>& groups,
		std::wstring_view context,
		std::wstring* outError)
	{
		for (const auto& source : sources)
		{
			if (groups.size() >= CompiledInteractionInvalidIndex)
			{
				if (outError) *outError = std::wstring(context)
					+ L" 的 ParallelTimeline 层级过大。";
				return false;
			}
			const auto groupIndex = static_cast<uint32_t>(groups.size());
			groups.push_back({ parentIndex, source.Timing });
			const auto animationOffset = animations.size();
			for (const auto& sourceAnimation : source.Animations)
			{
				RuntimeAnimation animation;
				if (!TryBuildAnimation(sourceAnimation, animation,
					std::wstring(context), outError)) return false;
				animation.TimelineGroupIndex = groupIndex;
				animations.push_back(std::move(animation));
			}
			const auto animationCount = animations.size() - animationOffset;
			if (!AppendDeclarativeTimelineGroups(source.Children, groupIndex,
				animations, groups, context, outError)) return false;
			auto& group = groups[groupIndex];
			const auto direct = std::span<const RuntimeAnimation>{ animations }
				.subspan(animationOffset, animationCount);
			if (!ResolveStoryboardTiming(group.Timing, direct,
				group.ResolvedSimpleDurationMilliseconds,
				group.ResolvedSimpleDurationForever))
			{
				if (outError) *outError = std::wstring(context)
					+ L" 的 ParallelTimeline 时序无效。";
				return false;
			}
			if (group.Timing.DurationAutomatic
				&& !group.ResolvedSimpleDurationForever)
				for (const auto& child : groups)
					if (child.ParentIndex == groupIndex
						&& !ExtendParallelNaturalDuration(child,
							group.ResolvedSimpleDurationMilliseconds,
							group.ResolvedSimpleDurationForever)) return false;
		}
		return true;
	}

	bool Build(
		std::vector<DeclarativeVisualStateGroupDefinition> definitions,
		std::vector<DeclarativeEventTriggerDefinition> eventTriggerDefinitions,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		if (!Owner || (definitions.empty() && eventTriggerDefinitions.empty()))
			return fail(L"视觉状态组和事件触发器不能同时为空。");
		std::vector<std::pair<PropertyKey, size_t>> groupProperties;
		for (auto& sourceGroup : definitions)
		{
			if (sourceGroup.Name.empty())
				return fail(L"视觉状态组名称不能为空。");
			if (std::any_of(Groups.begin(), Groups.end(), [&](const auto& existing)
				{ return EqualName(existing.Name, sourceGroup.Name); }))
				return fail(L"视觉状态组名称重复：" + sourceGroup.Name);
			if (sourceGroup.States.empty())
				return fail(L"视觉状态组至少需要一个状态：" + sourceGroup.Name);
			RuntimeGroup group;
			group.Name = std::move(sourceGroup.Name);
			group.Token = MakeVisualStateGroupToken(group.Name);
			std::optional<size_t> fallback;
			std::vector<const DeclarativeEventDefinition*> groupEvents;
			for (auto& sourceState : sourceGroup.States)
			{
				if (sourceState.Name.empty())
					return fail(L"视觉状态名称不能为空。");
				if (std::any_of(group.States.begin(), group.States.end(),
					[&](const auto& existing)
					{ return EqualName(existing.Name, sourceState.Name); }))
					return fail(L"视觉状态名称重复：" + sourceState.Name);
				bool hasEvents = !sourceState.Events.empty();
#if CUI_ENABLE_DYNAMIC_XAML
				hasEvents = hasEvents || !sourceState.EventNames.empty();
#endif
				if (!sourceState.Conditions.empty() && hasEvents)
					return fail(L"视觉状态不能同时声明属性和事件触发器："
						+ sourceState.Name);
				RuntimeState state;
				state.Name = std::move(sourceState.Name);
				state.Token = MakeVisualStateToken(state.Name);
				state.StoryboardTiming = sourceState.StoryboardTiming;
				if (sourceState.Conditions.empty() && !hasEvents)
				{
					if (fallback)
						return fail(L"每个视觉状态组只能有一个无触发器的回退状态："
							+ group.Name);
					fallback = group.States.size();
				}
				std::vector<const DependencyProperty*> stateConditions;
				for (auto& sourceCondition : sourceState.Conditions)
				{
					if (sourceCondition.Property.Empty())
						return fail(L"视觉状态条件属性为空或重复：" + state.Name);
					const DependencyPropertyMetadata* metadata = nullptr;
					if (sourceCondition.Property.Identity())
						metadata = Owner->GetPropertyMetadata(
							*sourceCondition.Property.Identity());
#if CUI_ENABLE_DYNAMIC_XAML
					else
						metadata = Owner->FindPropertyMetadata(
							sourceCondition.Property.Name());
#endif
					BindingValue converted;
					BindingValue coerced;
					if (!metadata || !metadata->CanRead()
						|| !metadata->TryConvert(sourceCondition.Value, converted)
						|| !metadata->TryCoerce(*Owner, converted, coerced))
						return fail(L"视觉状态条件属性不存在或值无效："
							+ sourceCondition.Property.Name());
					const auto* property = &metadata->Property();
					if (std::find(stateConditions.begin(),
						stateConditions.end(), property) != stateConditions.end())
						return fail(L"视觉状态条件属性为空或重复：" + state.Name);
					stateConditions.push_back(property);
					if (std::find(group.ConditionProperties.begin(),
						group.ConditionProperties.end(), property)
						== group.ConditionProperties.end())
						group.ConditionProperties.push_back(property);
					state.Conditions.push_back({ metadata, std::move(coerced) });
				}
				for (const auto* event : sourceState.Events)
				{
					if (!event || std::find(groupEvents.begin(), groupEvents.end(), event)
						!= groupEvents.end())
						return fail(L"视觉状态事件不存在或在组内重复。");
					groupEvents.push_back(event);
					state.Events.push_back(event);
				}
#if CUI_ENABLE_DYNAMIC_XAML
				for (auto& eventName : sourceState.EventNames)
				{
					const auto* event = Owner->FindDeclarativeEvent(eventName);
					if (eventName.empty() || !event
						|| std::find(groupEvents.begin(), groupEvents.end(), event)
							!= groupEvents.end())
						return fail(L"视觉状态事件不存在或在组内重复：" + eventName);
					groupEvents.push_back(event);
					state.Events.push_back(event);
				}
#endif
				struct StatePropertyOwnership
				{
					PropertyKey Root;
					bool Exclusive = false;
					std::vector<uint64_t> AnimationPathIdentities;
				};
				std::vector<StatePropertyOwnership> stateProperties;
				auto registerControlledProperty = [&](const PropertyKey& key,
					const std::wstring& source,
					uint64_t animationPathIdentity = 0) -> bool
					{
						auto stateOwner = std::find_if(
							stateProperties.begin(), stateProperties.end(),
							[&](const auto& existing)
							{ return SameProperty(existing.Root, key); });
						if (stateOwner != stateProperties.end())
						{
							if (animationPathIdentity == 0 || stateOwner->Exclusive
								|| ContainsObjectPathIdentity(
									stateOwner->AnimationPathIdentities,
									animationPathIdentity))
								return fail(L"视觉状态 Setter/Storyboard 目标重复："
									+ state.Name + L"." + source);
							stateOwner->AnimationPathIdentities.push_back(
								animationPathIdentity);
						}
						else
						{
							StatePropertyOwnership ownership;
							ownership.Root = key;
							ownership.Exclusive = animationPathIdentity == 0;
							if (animationPathIdentity != 0)
								ownership.AnimationPathIdentities.push_back(
									animationPathIdentity);
							stateProperties.push_back(std::move(ownership));
						}
						const auto owner = std::find_if(
							groupProperties.begin(), groupProperties.end(),
							[&](const auto& existing)
							{ return SameProperty(existing.first, key); });
						if (owner != groupProperties.end()
							&& owner->second != Groups.size())
							return fail(L"不同视觉状态组不能控制同一属性：" + source);
						if (owner == groupProperties.end())
							groupProperties.emplace_back(key, Groups.size());
						return true;
					};
				for (auto& sourceSetter : sourceState.Setters)
				{
					Control* target = sourceSetter.Target
						? sourceSetter.Target : Owner;
#if CUI_ENABLE_DYNAMIC_XAML
					if (!sourceSetter.Target && !sourceSetter.TargetName.empty())
						target = Owner->FindDeclarativeTemplatePart(
							sourceSetter.TargetName);
					if (!target)
						return fail(L"视觉状态 Setter 找不到模板部件："
							+ sourceSetter.TargetName
							+ L"（模板根="
							+ (Owner->GetControlTemplateRoot()
								? L"存在" : L"缺失")
							+ L"，已注册部件="
							+ std::to_wstring(
								Owner->_templateNameScope.size())
							+ L"）");
#endif
					const DependencyPropertyMetadata* metadata = nullptr;
					if (sourceSetter.Property.Identity())
						metadata = target->GetPropertyMetadata(
							*sourceSetter.Property.Identity());
#if CUI_ENABLE_DYNAMIC_XAML
					else
						metadata = target->FindPropertyMetadata(
							sourceSetter.Property.Name());
#endif
					if (!metadata)
						return fail(L"视觉状态 Setter 属性不存在："
							+ sourceSetter.Property.Name());
					if (!metadata->CanWrite())
						return fail(L"视觉状态 Setter 属性只读："
							+ sourceSetter.Property.Name());
					BindingValue converted;
					if (!metadata->TryConvert(sourceSetter.Value, converted))
						return fail(L"视觉状态 Setter 属性值转换失败："
							+ sourceSetter.Property.Name());
					BindingValue coerced;
					if (!metadata->TryCoerce(*target, converted, coerced))
						return fail(L"视觉状态 Setter 属性值无效："
							+ sourceSetter.Property.Name());
					PropertyKey key{ target, &metadata->Property() };
					if (!registerControlledProperty(key, metadata->Name())) return false;
					state.Setters.push_back({
						target, metadata, std::move(coerced) });
				}
				for (auto& sourceAnimation : sourceState.Animations)
				{
					RuntimeAnimation animation;
					if (!TryBuildAnimation(sourceAnimation, animation,
						L"视觉状态 Storyboard", outError)) return false;
					PropertyKey key{
						animation.Target, PropertyIdentity(animation.Metadata) };
					const auto pathIdentity =
						ObjectPathIdentity(animation.ObjectPath);
					if (!registerControlledProperty(key,
						sourceAnimation.PropertyPath(), pathIdentity))
						return false;
					state.Animations.push_back(std::move(animation));
				}
				const auto nestedAnimationOffset = state.Animations.size();
				if (!AppendDeclarativeTimelineGroups(sourceState.TimelineGroups,
					CompiledInteractionInvalidIndex, state.Animations,
					state.TimelineGroups, L"视觉状态 Storyboard", outError))
					return false;
				for (size_t index = nestedAnimationOffset;
					index < state.Animations.size(); ++index)
				{
					const auto& animation = state.Animations[index];
					PropertyKey key{ animation.Target,
						PropertyIdentity(animation.Metadata) };
					if (!registerControlledProperty(key,
						animation.Metadata->Name(),
						ObjectPathIdentity(animation.ObjectPath))) return false;
				}
				group.States.push_back(std::move(state));
			}
			if (!fallback)
				return fail(L"视觉状态组缺少无触发器的回退状态：" + group.Name);
			group.FallbackState = *fallback;
			auto findState = [&](const std::wstring& name) -> std::optional<size_t>
				{
					if (name.empty()) return std::nullopt;
					for (size_t index = 0; index < group.States.size(); ++index)
						if (EqualName(group.States[index].Name, name)) return index;
					return std::nullopt;
				};
			for (const auto& sourceTransition : sourceGroup.Transitions)
			{
				RuntimeTransition transition;
				transition.StoryboardTiming = sourceTransition.StoryboardTiming;
				transition.FromState = findState(sourceTransition.FromState);
				transition.ToState = findState(sourceTransition.ToState);
				if (!sourceTransition.FromState.empty() && !transition.FromState)
					return fail(L"VisualTransition.From 状态不存在："
						+ sourceTransition.FromState);
				if (!sourceTransition.ToState.empty() && !transition.ToState)
					return fail(L"VisualTransition.To 状态不存在："
						+ sourceTransition.ToState);
				if (std::any_of(group.Transitions.begin(), group.Transitions.end(),
					[&](const auto& existing)
					{
						return existing.FromState == transition.FromState
							&& existing.ToState == transition.ToState;
					}))
					return fail(L"VisualTransition From/To 选择器重复："
						+ sourceTransition.FromState + L" -> "
						+ sourceTransition.ToState);
				transition.GeneratedDurationMilliseconds =
					sourceTransition.GeneratedDurationMilliseconds;
				transition.GeneratedEasing = sourceTransition.GeneratedEasing;
				transition.GeneratedEasingMode =
					sourceTransition.GeneratedEasingMode;
				transition.GeneratedEasingParameters =
					sourceTransition.GeneratedEasingParameters;
				struct TransitionPropertyOwnership
				{
					PropertyKey Root;
					bool Exclusive = false;
					std::vector<uint64_t> AnimationPathIdentities;
				};
				std::vector<TransitionPropertyOwnership> transitionProperties;
				for (const auto& sourceAnimation : sourceTransition.Animations)
				{
					RuntimeAnimation animation;
					if (!TryBuildAnimation(sourceAnimation, animation,
						L"VisualTransition Storyboard", outError)) return false;
					PropertyKey key{
						animation.Target, PropertyIdentity(animation.Metadata) };
					const auto pathIdentity =
						ObjectPathIdentity(animation.ObjectPath);
					auto owner = std::find_if(transitionProperties.begin(),
						transitionProperties.end(), [&](const auto& existing)
						{ return SameProperty(existing.Root, key); });
					if (owner != transitionProperties.end())
					{
						if (pathIdentity == 0 || owner->Exclusive
							|| ContainsObjectPathIdentity(
								owner->AnimationPathIdentities, pathIdentity))
							return fail(L"VisualTransition Storyboard 目标重复："
								+ sourceAnimation.PropertyPath());
						owner->AnimationPathIdentities.push_back(pathIdentity);
					}
					else
					{
						TransitionPropertyOwnership ownership;
						ownership.Root = key;
						ownership.Exclusive = pathIdentity == 0;
						if (pathIdentity != 0)
							ownership.AnimationPathIdentities.push_back(pathIdentity);
						transitionProperties.push_back(std::move(ownership));
					}
					const auto groupOwner = std::find_if(groupProperties.begin(),
						groupProperties.end(), [&](const auto& existing)
						{ return SameProperty(existing.first, key); });
					if (groupOwner != groupProperties.end()
						&& groupOwner->second != Groups.size())
						return fail(L"不同视觉状态组不能控制同一 Transition 属性："
							+ sourceAnimation.PropertyPath());
					if (groupOwner == groupProperties.end())
						groupProperties.emplace_back(key, Groups.size());
					transition.Animations.push_back(std::move(animation));
				}
				const auto nestedAnimationOffset = transition.Animations.size();
				if (!AppendDeclarativeTimelineGroups(
					sourceTransition.TimelineGroups,
					CompiledInteractionInvalidIndex, transition.Animations,
					transition.TimelineGroups,
					L"VisualTransition Storyboard", outError)) return false;
				for (size_t index = nestedAnimationOffset;
					index < transition.Animations.size(); ++index)
				{
					const auto& animation = transition.Animations[index];
					PropertyKey key{ animation.Target,
						PropertyIdentity(animation.Metadata) };
					const auto pathIdentity =
						ObjectPathIdentity(animation.ObjectPath);
					auto owner = std::find_if(transitionProperties.begin(),
						transitionProperties.end(), [&](const auto& existing)
						{ return SameProperty(existing.Root, key); });
					if (owner != transitionProperties.end())
					{
						if (pathIdentity == 0 || owner->Exclusive
							|| ContainsObjectPathIdentity(
								owner->AnimationPathIdentities, pathIdentity))
							return fail(L"VisualTransition Storyboard 目标重复："
								+ animation.Metadata->Name());
						owner->AnimationPathIdentities.push_back(pathIdentity);
					}
					else
					{
						TransitionPropertyOwnership ownership;
						ownership.Root = key;
						ownership.Exclusive = pathIdentity == 0;
						if (pathIdentity != 0)
							ownership.AnimationPathIdentities.push_back(pathIdentity);
						transitionProperties.push_back(std::move(ownership));
					}
					const auto groupOwner = std::find_if(groupProperties.begin(),
						groupProperties.end(), [&](const auto& existing)
						{ return SameProperty(existing.first, key); });
					if (groupOwner != groupProperties.end()
						&& groupOwner->second != Groups.size())
						return fail(L"不同视觉状态组不能控制同一 Transition 属性："
							+ animation.Metadata->Name());
					if (groupOwner == groupProperties.end())
						groupProperties.emplace_back(key, Groups.size());
				}
				group.Transitions.push_back(std::move(transition));
			}
			Groups.push_back(std::move(group));
		}

		for (auto& sourceTrigger : eventTriggerDefinitions)
		{
			const auto* sourceEvent = sourceTrigger.Event;
			auto routedEvent = sourceTrigger.RoutedEvent;
#if CUI_ENABLE_DYNAMIC_XAML
			if (!sourceEvent && routedEvent == RoutedEventId::None
				&& !sourceTrigger.EventName.empty())
				sourceEvent = Owner->FindDeclarativeEvent(sourceTrigger.EventName);
			if (!sourceEvent && routedEvent == RoutedEventId::None)
				for (auto candidate = static_cast<unsigned int>(
					RoutedEventId::None) + 1;
					candidate < static_cast<unsigned int>(
						RoutedEventId::Count); ++candidate)
				{
					const auto id = static_cast<RoutedEventId>(candidate);
					if (EqualName(
						GetRoutedEventMetadata(id).Name,
						sourceTrigger.EventName))
					{
						routedEvent = id;
						break;
					}
				}
			if (!sourceEvent && routedEvent == RoutedEventId::None)
				return fail(L"EventTrigger 事件不存在："
					+ sourceTrigger.EventName);
			if (sourceEvent && routedEvent != RoutedEventId::None)
				return fail(L"EventTrigger 不能同时指定组件事件和路由事件："
					+ sourceTrigger.EventName);
			if (sourceTrigger.Actions.empty())
				return fail(L"EventTrigger 至少需要一个 TriggerAction："
					+ sourceTrigger.EventName);
#else
			if ((!sourceEvent && routedEvent == RoutedEventId::None)
				|| (sourceEvent && routedEvent != RoutedEventId::None)
				|| static_cast<unsigned int>(routedEvent)
					>= static_cast<unsigned int>(RoutedEventId::Count))
				return fail(L"EventTrigger 必须指定一个有效的编译期事件标识。");
			if (sourceTrigger.Actions.empty())
				return fail(L"EventTrigger 至少需要一个 TriggerAction。");
#endif
			RuntimeEventTrigger trigger;
			trigger.Event = sourceEvent;
			trigger.RoutedEvent = routedEvent;
			for (auto& sourceAction : sourceTrigger.Actions)
			{
				RuntimeEventTriggerAction action;
				action.Kind = sourceAction.Kind;
				action.Handoff = sourceAction.Handoff;
				action.SeekOffsetMilliseconds =
					sourceAction.SeekOffsetMilliseconds;
				action.SpeedRatio = sourceAction.SpeedRatio;
				if (sourceAction.Kind
					== DeclarativeStoryboardActionKind::Begin)
				{
					if (!sourceAction.StoryboardName.empty()
						&& std::any_of(EventStoryboards.begin(),
							EventStoryboards.end(), [&](const auto& existing)
							{ return !existing.IsStyleStoryboard
							&& EqualName(existing.Name,
								sourceAction.StoryboardName); }))
						return fail(L"BeginStoryboard x:Name 重复："
							+ sourceAction.StoryboardName);
					if (sourceAction.Animations.empty()
						&& sourceAction.TimelineGroups.empty())
						return fail(L"BeginStoryboard 的 Storyboard 不能为空。");
					RuntimeEventStoryboard storyboard;
					storyboard.Name = std::move(sourceAction.StoryboardName);
					storyboard.Timing = sourceAction.StoryboardTiming;
					storyboard.Handoff = sourceAction.Handoff;
					struct StoryboardPropertyOwnership
					{
						PropertyKey Root;
						bool Exclusive = false;
						std::vector<uint64_t> PathIdentities;
					};
					std::vector<StoryboardPropertyOwnership> properties;
					for (const auto& sourceAnimation : sourceAction.Animations)
					{
						RuntimeAnimation animation;
						if (!TryBuildAnimation(sourceAnimation, animation,
							L"BeginStoryboard", outError)) return false;
						PropertyKey key{ animation.Target,
							PropertyIdentity(animation.Metadata) };
						const auto pathIdentity =
							ObjectPathIdentity(animation.ObjectPath);
						auto owner = std::find_if(properties.begin(),
							properties.end(), [&](const auto& existing)
							{ return SameProperty(existing.Root, key); });
						if (owner != properties.end())
						{
							if (pathIdentity == 0 || owner->Exclusive
								|| ContainsObjectPathIdentity(
									owner->PathIdentities, pathIdentity))
								return fail(L"BeginStoryboard 目标重复："
									+ sourceAnimation.PropertyPath());
							owner->PathIdentities.push_back(pathIdentity);
						}
						else
						{
							StoryboardPropertyOwnership ownership;
							ownership.Root = key;
							ownership.Exclusive = pathIdentity == 0;
							if (pathIdentity != 0)
								ownership.PathIdentities.push_back(pathIdentity);
							properties.push_back(std::move(ownership));
						}
						storyboard.Animations.push_back(std::move(animation));
					}
					const auto nestedAnimationOffset = storyboard.Animations.size();
					if (!AppendDeclarativeTimelineGroups(
						sourceAction.TimelineGroups,
						CompiledInteractionInvalidIndex, storyboard.Animations,
						storyboard.TimelineGroups, L"BeginStoryboard", outError))
						return false;
					for (size_t index = nestedAnimationOffset;
						index < storyboard.Animations.size(); ++index)
					{
						const auto& animation = storyboard.Animations[index];
						PropertyKey key{ animation.Target,
							PropertyIdentity(animation.Metadata) };
						const auto pathIdentity =
							ObjectPathIdentity(animation.ObjectPath);
						auto owner = std::find_if(properties.begin(), properties.end(),
							[&](const auto& existing)
							{ return SameProperty(existing.Root, key); });
						if (owner != properties.end())
						{
							if (pathIdentity == 0 || owner->Exclusive
								|| ContainsObjectPathIdentity(
									owner->PathIdentities, pathIdentity))
								return fail(L"BeginStoryboard 目标重复："
									+ animation.Metadata->Name());
							owner->PathIdentities.push_back(pathIdentity);
						}
						else
						{
							StoryboardPropertyOwnership ownership;
							ownership.Root = key;
							ownership.Exclusive = pathIdentity == 0;
							if (pathIdentity != 0)
								ownership.PathIdentities.push_back(pathIdentity);
							properties.push_back(std::move(ownership));
						}
					}
					action.StoryboardIndex = EventStoryboards.size();
					EventStoryboards.push_back(std::move(storyboard));
				}
				else
				{
					if (sourceAction.StoryboardName.empty())
						return fail(L"Storyboard 控制动作缺少 BeginStoryboardName。");
					action.PendingStoryboardName =
						std::move(sourceAction.StoryboardName);
				}
				trigger.Actions.push_back(std::move(action));
			}
			EventTriggers.push_back(std::move(trigger));
		}
		for (auto& trigger : EventTriggers)
			for (auto& action : trigger.Actions)
			{
				if (action.Kind == DeclarativeStoryboardActionKind::Begin)
					continue;
				const auto found = std::find_if(EventStoryboards.begin(),
					EventStoryboards.end(), [&](const auto& storyboard)
					{ return !storyboard.IsStyleStoryboard
					&& EqualName(storyboard.Name,
						action.PendingStoryboardName); });
				if (found == EventStoryboards.end())
					return fail(L"Storyboard 控制动作找不到 BeginStoryboard："
						+ action.PendingStoryboardName);
				if (action.Kind == DeclarativeStoryboardActionKind::SkipToFill
					&& (found->Timing.RepeatBehavior
							== DeclarativeRepeatBehaviorKind::Forever
						|| std::any_of(found->Animations.begin(),
						found->Animations.end(), [](const auto& animation)
						{ return animation.RepeatBehavior
							== DeclarativeRepeatBehaviorKind::Forever; })
						|| TimelineGroupsContainForever(
							found->TimelineGroups)))
					return fail(L"SkipStoryboardToFill 不能引用 Forever Storyboard。");
				action.StoryboardIndex = static_cast<size_t>(
					std::distance(EventStoryboards.begin(), found));
				action.PendingStoryboardName.clear();
			}

		Connections.push_back(Owner->OnPropertyValueChanged.Subscribe(
			[this](DependencyObject*, const DependencyPropertyChangedEventArgs& args)
			{ OnHostPropertyChanged(args); }));
		const bool consumesDeclarativeEvents = std::any_of(
			Groups.begin(), Groups.end(), [](const auto& group)
			{
				return std::any_of(group.States.begin(), group.States.end(),
					[](const auto& state) { return !state.Events.empty(); });
			}) || std::any_of(EventTriggers.begin(), EventTriggers.end(),
			[](const auto& trigger) { return trigger.Event != nullptr; });
		if (consumesDeclarativeEvents)
			Connections.push_back(Owner->OnDeclarativeEvent.Subscribe(
				[this](Control*, DeclarativeEventArgs& args)
				{ OnHostDeclarativeEvent(args); }));
		std::vector<RoutedEventId> subscribedRoutedEvents;
		for (const auto& trigger : EventTriggers)
		{
			if (trigger.RoutedEvent == RoutedEventId::None
				|| std::find(
					subscribedRoutedEvents.begin(),
					subscribedRoutedEvents.end(),
					trigger.RoutedEvent)
				!= subscribedRoutedEvents.end()) continue;
			subscribedRoutedEvents.push_back(trigger.RoutedEvent);
			Connections.push_back(Owner->AddHandler(
				trigger.RoutedEvent,
				[this](Control*, RoutedEventArgs& args)
				{ OnHostRoutedEvent(args); }));
		}
		for (size_t index = 0; index < Groups.size(); ++index)
			if (!GoTo(index, EvaluateState(index), false, outError)) return false;
		if (outError) outError->clear();
		return true;
	}

#endif

	static bool ValidCompiledRange(
		CompiledInteractionRange range, size_t size) noexcept
	{
		return range.Offset <= size
			&& range.Count <= size - range.Offset;
	}

	static bool ValidCompiledAnimationKind(
		DeclarativeAnimationKind value) noexcept
	{
		return static_cast<unsigned char>(value)
			<= static_cast<unsigned char>(DeclarativeAnimationKind::Single);
	}

	static bool ValidCompiledEasingKind(DeclarativeEasingKind value) noexcept
	{
		return static_cast<unsigned char>(value)
			<= static_cast<unsigned char>(DeclarativeEasingKind::Quintic);
	}

	static bool ValidCompiledEasingParameters(
		DeclarativeEasingKind kind,
		DeclarativeEasingParameters parameters) noexcept
	{
		if (!std::isfinite(parameters.Primary)
			|| !std::isfinite(parameters.Secondary)) return false;
		if (kind == DeclarativeEasingKind::Bounce
			|| kind == DeclarativeEasingKind::Elastic)
			return std::trunc(parameters.Secondary) == parameters.Secondary
				&& parameters.Secondary >= static_cast<double>((std::numeric_limits<int>::min)())
				&& parameters.Secondary <= static_cast<double>((std::numeric_limits<int>::max)());
		if (kind == DeclarativeEasingKind::Back
			|| kind == DeclarativeEasingKind::Exponential
			|| kind == DeclarativeEasingKind::Power)
			return parameters.Secondary == 0.0;
		return parameters.Primary == 0.0 && parameters.Secondary == 0.0;
	}

	static bool ValidCompiledEasingMode(DeclarativeEasingMode value) noexcept
	{
		return static_cast<unsigned char>(value)
			<= static_cast<unsigned char>(DeclarativeEasingMode::EaseInOut);
	}

	static bool ValidCompiledActionKind(
		DeclarativeStoryboardActionKind value) noexcept
	{
		return static_cast<unsigned char>(value)
			<= static_cast<unsigned char>(
				DeclarativeStoryboardActionKind::SkipToFill);
	}

	static bool ValidCompiledHandoffBehavior(
		DeclarativeHandoffBehavior value) noexcept
	{
		return static_cast<unsigned char>(value)
			<= static_cast<unsigned char>(DeclarativeHandoffBehavior::Compose);
	}

	static bool ValidCompiledStoryboardTiming(
		const DeclarativeStoryboardTimingDefinition& timing) noexcept
	{
		if (static_cast<unsigned char>(timing.RepeatBehavior)
			> static_cast<unsigned char>(
				DeclarativeRepeatBehaviorKind::Forever)
			|| static_cast<unsigned char>(timing.FillBehavior)
				> static_cast<unsigned char>(
					DeclarativeTimelineFillBehavior::Stop)
			|| !std::isfinite(timing.SpeedRatio) || timing.SpeedRatio <= 0.0
			|| !std::isfinite(timing.AccelerationRatio)
			|| timing.AccelerationRatio < 0.0
			|| timing.AccelerationRatio > 1.0
			|| !std::isfinite(timing.DecelerationRatio)
			|| timing.DecelerationRatio < 0.0
			|| timing.DecelerationRatio > 1.0
			|| timing.AccelerationRatio + timing.DecelerationRatio > 1.0)
			return false;
		if (timing.RepeatBehavior == DeclarativeRepeatBehaviorKind::Count)
			return std::isfinite(timing.RepeatCount)
				&& timing.RepeatCount > 0.0;
		if (timing.RepeatBehavior == DeclarativeRepeatBehaviorKind::Duration)
			return timing.RepeatDurationMilliseconds > 0;
		return true;
	}

	bool TryBuildCompiledAnimation(
		const CompiledInteractionProgramView& program,
		std::span<const BindingValue> values,
		std::span<Control* const> targets,
		const CompiledInteractionAnimationOp& operation,
		RuntimeAnimation& animation,
		const std::wstring& context,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = context + L"：" + std::move(message);
				return false;
			};
		if (!ValidCompiledAnimationKind(operation.Kind)
			|| !ValidCompiledEasingKind(operation.Easing)
			|| !ValidCompiledEasingMode(operation.EasingMode)
			|| !ValidCompiledEasingParameters(
				operation.Easing, operation.EasingParameters))
			return fail(L"动画类型或 easing enum 无效。" );
		if (operation.OperandIndex >= program.PropertyOperands.size())
			return fail(L"属性 operand 索引越界。" );
		const auto& operand = program.PropertyOperands[operation.OperandIndex];
		if (operand.TargetSlot >= targets.size()
			|| !targets[operand.TargetSlot]
			|| !operand.Property.Identity())
			return fail(L"目标 slot 或依赖属性 identity 无效。" );
		auto valueAt = [&](uint32_t index, std::optional<BindingValue>& output)
			{
				if (index == CompiledInteractionInvalidIndex) return true;
				if (index >= values.size()) return false;
				output = values[index];
				return true;
			};
		RuntimeAnimationDefinition source;
		source.Kind = operation.Kind;
		if (!valueAt(operation.FromValueIndex, source.From)
			|| !valueAt(operation.ToValueIndex, source.To)
			|| !valueAt(operation.ByValueIndex, source.By))
			return fail(L"动画端点 value 索引越界。" );
		if (!ValidCompiledRange(operation.KeyFrames, program.KeyFrames.size()))
			return fail(L"关键帧 range 越界。" );
		source.KeyFrames.reserve(operation.KeyFrames.Count);
		for (uint32_t offset = 0; offset < operation.KeyFrames.Count; ++offset)
		{
			const auto& compiled = program.KeyFrames[
				operation.KeyFrames.Offset + offset];
			if (compiled.ValueIndex >= values.size()
				|| compiled.KeyTimeSubMillisecondTicks >= 10000u
				|| static_cast<unsigned char>(compiled.Kind)
					> static_cast<unsigned char>(DeclarativeKeyFrameKind::Spline)
				|| !ValidCompiledEasingKind(compiled.Easing)
				|| !ValidCompiledEasingMode(compiled.EasingMode)
				|| !ValidCompiledEasingParameters(
					compiled.Easing, compiled.EasingParameters))
				return fail(L"关键帧 value 索引越界。" );
			RuntimeAnimationKeyFrame keyFrame;
			keyFrame.Kind = compiled.Kind;
			keyFrame.KeyTimeMilliseconds = compiled.KeyTimeMilliseconds;
			keyFrame.KeyTimeSubMillisecondTicks =
				compiled.KeyTimeSubMillisecondTicks;
			keyFrame.Value = values[compiled.ValueIndex];
			keyFrame.Easing = compiled.Easing;
			keyFrame.EasingMode = compiled.EasingMode;
			keyFrame.EasingParameters = compiled.EasingParameters;
			keyFrame.KeySplineX1 = compiled.KeySplineX1;
			keyFrame.KeySplineY1 = compiled.KeySplineY1;
			keyFrame.KeySplineX2 = compiled.KeySplineX2;
			keyFrame.KeySplineY2 = compiled.KeySplineY2;
			source.KeyFrames.push_back(std::move(keyFrame));
		}
		source.IsAdditive = operation.IsAdditive;
		source.IsCumulative = operation.IsCumulative;
		source.Path = operation.Path;
		if (!ValidCompiledRange(operation.PathSegments,
			program.PathSegments.size()))
			return fail(L"路径动画 segment range 越界。");
		source.PathSegments.assign(
			program.PathSegments.begin() + operation.PathSegments.Offset,
			program.PathSegments.begin() + operation.PathSegments.Offset
				+ operation.PathSegments.Count);
		source.BeginTimeMilliseconds = operation.BeginTimeMilliseconds;
		source.DurationMilliseconds = operation.DurationMilliseconds;
		source.RepeatBehavior = operation.RepeatBehavior;
		source.RepeatCount = operation.RepeatCount;
		source.RepeatDurationMilliseconds = operation.RepeatDurationMilliseconds;
		source.AutoReverse = operation.AutoReverse;
		source.FillBehavior = operation.FillBehavior;
		source.SpeedRatio = operation.SpeedRatio;
		source.AccelerationRatio = operation.AccelerationRatio;
		source.DecelerationRatio = operation.DecelerationRatio;
		source.Easing = operation.Easing;
		source.EasingMode = operation.EasingMode;
		source.EasingParameters = operation.EasingParameters;
		auto* target = targets[operand.TargetSlot];
		const auto* metadata = target->GetPropertyMetadata(
			*operand.Property.Identity());
		if (!metadata || !metadata->CanWrite())
			return fail(L"依赖属性不存在或只读。" );
		std::optional<ObjectPathAccessor> objectPath;
		if (operation.ObjectPathIndex != CompiledInteractionInvalidIndex)
		{
			if (operation.ObjectPathIndex >= program.ObjectPaths.size())
				return fail(L"对象路径索引越界。" );
			std::wstring pathError;
			ObjectPathAccessor accessor;
			if (!TryResolveCompiledObjectPath(
				*target, program.ObjectPaths[operation.ObjectPathIndex],
				program.ObjectPathChildIndices, *metadata,
				operation.Kind, accessor, &pathError))
				return fail(std::move(pathError));
			objectPath = std::move(accessor);
		}
		else if (!AnimationMatchesMetadata(operation.Kind, *metadata))
			return fail(L"动画类型与依赖属性不匹配。" );
		return TryBuildResolvedAnimation(
			source, target, metadata, std::move(objectPath), {},
			animation, context, outError);
	}

	bool IsCompiledGroup(size_t groupIndex) const noexcept
	{
		return CompiledInteractions && groupIndex < Groups.size()
			&& Groups[groupIndex].CompiledGroupIndex
				!= RuntimeGroup::DynamicGroupIndex;
	}

	const CompiledInteractionGroupOp* CompiledGroupAt(
		size_t groupIndex) const noexcept
	{
		if (!IsCompiledGroup(groupIndex)) return nullptr;
		const auto compiledIndex = Groups[groupIndex].CompiledGroupIndex;
		return compiledIndex < CompiledInteractions->Program.Groups.size()
			? &CompiledInteractions->Program.Groups[compiledIndex] : nullptr;
	}

	std::span<const BindingValue> CompiledValues() const noexcept
	{
		return CompiledInteractions
			? std::span<const BindingValue>(CompiledInteractions->Values)
			: std::span<const BindingValue>{};
	}

	std::span<Control* const> CompiledTargets() const noexcept
	{
		return CompiledInteractions
			? std::span<Control* const>(CompiledInteractions->Targets)
			: std::span<Control* const>{};
	}

	size_t StateCount(size_t groupIndex) const noexcept
	{
		if (const auto* compiled = CompiledGroupAt(groupIndex))
			return compiled->States.Count;
#if CUI_ENABLE_DYNAMIC_XAML
		return groupIndex < Groups.size() ? Groups[groupIndex].States.size() : 0;
#else
		return 0;
#endif
	}

	VisualStateGroupToken GroupTokenAt(size_t groupIndex) const noexcept
	{
		if (const auto* compiled = CompiledGroupAt(groupIndex))
			return compiled->Token;
#if CUI_ENABLE_DYNAMIC_XAML
		return groupIndex < Groups.size() ? Groups[groupIndex].Token
			: VisualStateGroupToken{};
#else
		return {};
#endif
	}

	VisualStateToken StateTokenAt(
		size_t groupIndex,
		size_t stateIndex) const noexcept
	{
		if (const auto* group = CompiledGroupAt(groupIndex))
		{
			if (!CompiledInteractions || stateIndex >= group->States.Count)
				return {};
			const auto absolute = static_cast<size_t>(group->States.Offset)
				+ stateIndex;
			return absolute < CompiledInteractions->Program.States.size()
				? CompiledInteractions->Program.States[absolute].Token
				: VisualStateToken{};
		}
#if CUI_ENABLE_DYNAMIC_XAML
		if (groupIndex >= Groups.size()
			|| stateIndex >= Groups[groupIndex].States.size()) return {};
		return Groups[groupIndex].States[stateIndex].Token;
#else
		return {};
#endif
	}

	bool CompiledStateMatches(
		size_t groupIndex,
		size_t stateIndex) const
	{
		const auto* group = CompiledGroupAt(groupIndex);
		if (!group || !CompiledInteractions || stateIndex >= group->States.Count)
			return false;
		const auto& instance = *CompiledInteractions;
		const auto absolute = static_cast<size_t>(group->States.Offset) + stateIndex;
		if (absolute >= instance.Program.States.size()) return false;
		const auto& state = instance.Program.States[absolute];
		if (state.Conditions.Count == 0
			|| !ValidCompiledRange(
				state.Conditions, instance.Program.Conditions.size())) return false;
		for (uint32_t offset = 0; offset < state.Conditions.Count; ++offset)
		{
			const auto& condition = instance.Program.Conditions[
				state.Conditions.Offset + offset];
			if (condition.OperandIndex >= instance.Program.PropertyOperands.size()
				|| condition.ValueIndex >= instance.Values.size()) return false;
			const auto& operand =
				instance.Program.PropertyOperands[condition.OperandIndex];
			if (operand.TargetSlot != 0 || !operand.Property.Identity()) return false;
			const auto* metadata = Owner
				? Owner->GetPropertyMetadata(*operand.Property.Identity()) : nullptr;
			BindingValue converted;
			BindingValue expected;
			BindingValue actual;
			if (!metadata || !metadata->CanRead()
				|| !metadata->TryConvert(instance.Values[condition.ValueIndex], converted)
				|| !metadata->TryCoerce(*Owner, converted, expected)
				|| !metadata->TryGet(*Owner, actual)
				|| !metadata->ValuesEqual(actual, expected)) return false;
		}
		return true;
	}

	bool AppendCompiledTimelineGroups(
		const CompiledInteractionProgramView& program,
		std::span<const BindingValue> values,
		std::span<Control* const> targets,
		CompiledInteractionRange sourceRange,
		uint32_t parentIndex,
		std::vector<RuntimeAnimation>& animations,
		std::vector<RuntimeTimelineGroup>& groups,
		std::wstring_view context,
		std::wstring* outError,
		std::vector<uint8_t>* visitedGroups = nullptr)
	{
		std::vector<uint8_t> ownedVisitedGroups;
		if (!visitedGroups)
		{
			ownedVisitedGroups.resize(program.TimelineGroups.size(), 0);
			visitedGroups = &ownedVisitedGroups;
		}
		if (!ValidCompiledRange(sourceRange, program.TimelineGroups.size()))
		{
			if (outError) *outError = std::wstring(context)
				+ L" 的编译 ParallelTimeline range 无效。";
			return false;
		}
		for (uint32_t offset = 0; offset < sourceRange.Count; ++offset)
		{
			const auto absoluteGroupIndex = sourceRange.Offset + offset;
			if (absoluteGroupIndex >= visitedGroups->size()
				|| (*visitedGroups)[absoluteGroupIndex] != 0)
			{
				if (outError) *outError = std::wstring(context)
					+ L" 的编译 ParallelTimeline 图重复或成环。";
				return false;
			}
			(*visitedGroups)[absoluteGroupIndex] = 1;
			const auto& source = program.TimelineGroups[absoluteGroupIndex];
			if (!ValidCompiledRange(source.Animations, program.Animations.size())
				|| !ValidCompiledRange(source.Children,
					program.TimelineGroups.size())
				|| !ValidCompiledStoryboardTiming(source.Timing)
				|| groups.size() >= CompiledInteractionInvalidIndex)
			{
				if (outError) *outError = std::wstring(context)
					+ L" 的编译 ParallelTimeline 定义无效。";
				return false;
			}
			const auto groupIndex = static_cast<uint32_t>(groups.size());
			groups.push_back({ parentIndex, source.Timing });
			const auto animationOffset = animations.size();
			for (uint32_t animationOffsetInGroup = 0;
				animationOffsetInGroup < source.Animations.Count;
				++animationOffsetInGroup)
			{
				RuntimeAnimation animation;
				if (!TryBuildCompiledAnimation(program, values, targets,
					program.Animations[source.Animations.Offset
						+ animationOffsetInGroup], animation,
					std::wstring(context), outError)) return false;
				animation.TimelineGroupIndex = groupIndex;
				animations.push_back(std::move(animation));
			}
			const auto animationCount = animations.size() - animationOffset;
			if (!AppendCompiledTimelineGroups(program, values, targets,
				source.Children, groupIndex, animations, groups,
				context, outError, visitedGroups)) return false;
			auto& group = groups[groupIndex];
			const auto direct = std::span<const RuntimeAnimation>{ animations }
				.subspan(animationOffset, animationCount);
			if (!ResolveStoryboardTiming(group.Timing, direct,
				group.ResolvedSimpleDurationMilliseconds,
				group.ResolvedSimpleDurationForever))
			{
				if (outError) *outError = std::wstring(context)
					+ L" 的编译 ParallelTimeline 时序无效。";
				return false;
			}
			if (group.Timing.DurationAutomatic
				&& !group.ResolvedSimpleDurationForever)
				for (const auto& child : groups)
					if (child.ParentIndex == groupIndex
						&& !ExtendParallelNaturalDuration(child,
							group.ResolvedSimpleDurationMilliseconds,
							group.ResolvedSimpleDurationForever)) return false;
			(*visitedGroups)[absoluteGroupIndex] = 2;
		}
		return true;
	}

	bool TryBuildCompiledState(
		size_t groupIndex,
		size_t stateIndex,
		RuntimeState& state,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		const auto* group = CompiledGroupAt(groupIndex);
		if (!group || !CompiledInteractions || stateIndex >= group->States.Count)
			return fail(L"编译视觉状态索引无效。");
		const auto& instance = *CompiledInteractions;
		const auto absolute = static_cast<size_t>(group->States.Offset) + stateIndex;
		if (absolute >= instance.Program.States.size())
			return fail(L"编译视觉状态 range 越界。");
		const auto& source = instance.Program.States[absolute];
		if (!ValidCompiledRange(source.Conditions, instance.Program.Conditions.size())
			|| !ValidCompiledRange(source.Events, instance.Program.StateEvents.size())
			|| !ValidCompiledRange(source.Setters, instance.Program.Setters.size())
			|| !ValidCompiledRange(source.Animations, instance.Program.Animations.size())
			|| !ValidCompiledRange(source.TimelineGroups,
				instance.Program.TimelineGroups.size()))
			return fail(L"编译视觉状态结构已失效。");
		state = {};
		state.Token = source.Token;
		state.StoryboardTiming = source.StoryboardTiming;
		state.Conditions.reserve(source.Conditions.Count);
		for (uint32_t offset = 0; offset < source.Conditions.Count; ++offset)
		{
			const auto& condition = instance.Program.Conditions[
				source.Conditions.Offset + offset];
			if (condition.OperandIndex >= instance.Program.PropertyOperands.size()
				|| condition.ValueIndex >= instance.Values.size())
				return fail(L"编译视觉状态 condition 索引越界。");
			const auto& operand =
				instance.Program.PropertyOperands[condition.OperandIndex];
			if (operand.TargetSlot != 0 || !operand.Property.Identity())
				return fail(L"编译视觉状态 condition operand 无效。");
			const auto* metadata = Owner->GetPropertyMetadata(
				*operand.Property.Identity());
			BindingValue converted;
			BindingValue coerced;
			if (!metadata || !metadata->CanRead()
				|| !metadata->TryConvert(instance.Values[condition.ValueIndex], converted)
				|| !metadata->TryCoerce(*Owner, converted, coerced))
				return fail(L"编译视觉状态 condition 无法物化。");
			state.Conditions.push_back({ metadata, std::move(coerced) });
		}
		state.Events.reserve(source.Events.Count);
		for (uint32_t offset = 0; offset < source.Events.Count; ++offset)
			state.Events.push_back(instance.Program.StateEvents[
				source.Events.Offset + offset]);
		state.Setters.reserve(source.Setters.Count);
		for (uint32_t offset = 0; offset < source.Setters.Count; ++offset)
		{
			const auto& setter = instance.Program.Setters[
				source.Setters.Offset + offset];
			if (setter.OperandIndex >= instance.Program.PropertyOperands.size()
				|| setter.ValueIndex >= instance.Values.size())
				return fail(L"编译视觉状态 setter 索引越界。");
			const auto& operand =
				instance.Program.PropertyOperands[setter.OperandIndex];
			if (operand.TargetSlot >= instance.Targets.size()
				|| !operand.Property.Identity())
				return fail(L"编译视觉状态 setter operand 无效。");
			auto* target = instance.Targets[operand.TargetSlot];
			const auto* metadata = target
				? target->GetPropertyMetadata(*operand.Property.Identity()) : nullptr;
			BindingValue converted;
			BindingValue coerced;
			if (!metadata || !metadata->CanWrite()
				|| !metadata->TryConvert(instance.Values[setter.ValueIndex], converted)
				|| !metadata->TryCoerce(*target, converted, coerced))
				return fail(L"编译视觉状态 setter 无法物化。");
			state.Setters.push_back({ target, metadata, std::move(coerced) });
		}
		state.Animations.reserve(source.Animations.Count);
		for (uint32_t offset = 0; offset < source.Animations.Count; ++offset)
		{
			RuntimeAnimation animation;
			if (!TryBuildCompiledAnimation(instance.Program, instance.Values,
				instance.Targets, instance.Program.Animations[
					source.Animations.Offset + offset], animation,
				L"编译视觉状态 Storyboard", outError)) return false;
			state.Animations.push_back(std::move(animation));
		}
		if (!AppendCompiledTimelineGroups(instance.Program, instance.Values,
			instance.Targets, source.TimelineGroups,
			CompiledInteractionInvalidIndex, state.Animations,
			state.TimelineGroups, L"编译视觉状态 Storyboard", outError))
			return false;
		return true;
	}

	bool TryBuildStateFootprint(
		size_t groupIndex,
		size_t stateIndex,
		RuntimeStateFootprint& footprint,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		if (groupIndex >= Groups.size() || stateIndex >= StateCount(groupIndex))
			return fail(L"视觉状态 footprint 索引无效。");
		footprint = {};
		if (!IsCompiledGroup(groupIndex))
		{
#if CUI_ENABLE_DYNAMIC_XAML
			const auto& state = Groups[groupIndex].States[stateIndex];
			footprint.Token = state.Token;
#if CUI_ENABLE_DYNAMIC_XAML
			footprint.Name = state.Name;
#endif
			footprint.Setters.reserve(state.Setters.size());
			for (const auto& setter : state.Setters)
				footprint.Setters.push_back(
					{ setter.Target, setter.Metadata, BindingValue{} });
			footprint.Animations.reserve(state.Animations.size());
			for (const auto& animation : state.Animations)
				footprint.Animations.push_back({ animation.Target,
					animation.Metadata, animation.Kind,
					ObjectPathIdentity(animation.ObjectPath), &animation });
			return true;
#else
			return fail(L"Production 视觉状态 footprint 缺少编译程序。");
#endif
		}

		const auto* group = CompiledGroupAt(groupIndex);
		if (!group || !CompiledInteractions || stateIndex >= group->States.Count)
			return fail(L"编译视觉状态 footprint 索引无效。");
		const auto& instance = *CompiledInteractions;
		const auto absolute = static_cast<size_t>(group->States.Offset) + stateIndex;
		if (absolute >= instance.Program.States.size())
			return fail(L"编译视觉状态 footprint range 越界。");
		const auto& state = instance.Program.States[absolute];
		if (!ValidCompiledRange(state.Setters, instance.Program.Setters.size())
			|| !ValidCompiledRange(
				state.Animations, instance.Program.Animations.size())
			|| !ValidCompiledRange(state.TimelineGroups,
				instance.Program.TimelineGroups.size()))
			return fail(L"编译视觉状态 footprint 结构已失效。");
		footprint.Token = state.Token;
		footprint.Setters.reserve(state.Setters.Count);
		for (uint32_t offset = 0; offset < state.Setters.Count; ++offset)
		{
			const auto& setter = instance.Program.Setters[
				state.Setters.Offset + offset];
			if (setter.OperandIndex >= instance.Program.PropertyOperands.size())
				return fail(L"编译视觉状态 footprint setter 越界。");
			const auto& operand =
				instance.Program.PropertyOperands[setter.OperandIndex];
			if (operand.TargetSlot >= instance.Targets.size()
				|| !operand.Property.Identity())
				return fail(L"编译视觉状态 footprint setter operand 无效。");
			auto* target = instance.Targets[operand.TargetSlot];
			const auto* metadata = target
				? target->GetPropertyMetadata(*operand.Property.Identity()) : nullptr;
			if (!metadata || !metadata->CanWrite())
				return fail(L"编译视觉状态 footprint setter 属性无效。");
			footprint.Setters.push_back({ target, metadata, BindingValue{} });
		}
		footprint.Animations.reserve(state.Animations.Count);
		for (uint32_t offset = 0; offset < state.Animations.Count; ++offset)
		{
			const auto animationIndex = state.Animations.Offset + offset;
			const auto& animation = instance.Program.Animations[animationIndex];
			if (animation.OperandIndex >= instance.Program.PropertyOperands.size())
				return fail(L"编译视觉状态 footprint animation 越界。");
			const auto& operand =
				instance.Program.PropertyOperands[animation.OperandIndex];
			if (operand.TargetSlot >= instance.Targets.size()
				|| !operand.Property.Identity())
				return fail(L"编译视觉状态 footprint animation operand 无效。");
			auto* target = instance.Targets[operand.TargetSlot];
			const auto* metadata = target
				? target->GetPropertyMetadata(*operand.Property.Identity()) : nullptr;
			if (!metadata || !metadata->CanWrite())
				return fail(L"编译视觉状态 footprint animation 属性无效。");
			uint64_t pathIdentity = 0;
			if (animation.ObjectPathIndex != CompiledInteractionInvalidIndex)
			{
				if (animation.ObjectPathIndex >= instance.Program.ObjectPaths.size())
					return fail(L"编译视觉状态 footprint object path 越界。");
				pathIdentity =
					instance.Program.ObjectPaths[animation.ObjectPathIndex].Identity;
			}
			footprint.Animations.push_back({ target, metadata, animation.Kind,
				pathIdentity, nullptr, animationIndex });
		}
		std::vector<RuntimeAnimation> nestedAnimations;
		std::vector<RuntimeTimelineGroup> nestedGroups;
		if (!AppendCompiledTimelineGroups(instance.Program, instance.Values,
			instance.Targets, state.TimelineGroups,
			CompiledInteractionInvalidIndex, nestedAnimations, nestedGroups,
			L"编译视觉状态 footprint", outError)) return false;
		for (auto& animation : nestedAnimations)
		{
			RuntimeAnimationFootprint item;
			item.Target = animation.Target;
			item.Metadata = animation.Metadata;
			item.Kind = animation.Kind;
			item.ObjectPathIdentity = ObjectPathIdentity(animation.ObjectPath);
			item.Materialized = std::move(animation);
			footprint.Animations.push_back(std::move(item));
		}
		return true;
	}

	bool TryMaterializeAnimationFootprint(
		const RuntimeAnimationFootprint& footprint,
		RuntimeAnimation& animation,
		std::wstring* outError)
	{
		if (footprint.Resolved)
		{
			animation = *footprint.Resolved;
			return true;
		}
		if (footprint.Materialized)
		{
			animation = *footprint.Materialized;
			return true;
		}
		if (!CompiledInteractions
			|| footprint.CompiledAnimationIndex
				>= CompiledInteractions->Program.Animations.size()) return false;
		const auto& instance = *CompiledInteractions;
		return TryBuildCompiledAnimation(instance.Program, instance.Values,
			instance.Targets, instance.Program.Animations[
				footprint.CompiledAnimationIndex], animation,
			L"编译旧视觉状态 Storyboard", outError);
	}

	const RuntimeState* ResolveState(
		size_t groupIndex,
		size_t stateIndex,
		RuntimeState& storage,
		std::wstring* outError)
	{
		if (groupIndex >= Groups.size() || stateIndex >= StateCount(groupIndex))
		{
			if (outError) *outError = L"视觉状态索引无效。";
			return nullptr;
		}
		if (IsCompiledGroup(groupIndex))
			return TryBuildCompiledState(
				groupIndex, stateIndex, storage, outError) ? &storage : nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
		return &Groups[groupIndex].States[stateIndex];
#else
		if (outError) *outError = L"Production 视觉状态缺少编译程序。";
		return nullptr;
#endif
	}

	bool TryBuildCompiledTransition(
		size_t groupIndex,
		const CompiledInteractionTransitionOp& source,
		RuntimeTransition& transition,
		std::wstring* outError)
	{
		if (!CompiledInteractions)
		{
			if (outError) *outError = L"编译 VisualTransition 程序未安装。";
			return false;
		}
		const auto& instance = *CompiledInteractions;
		const auto* group = CompiledGroupAt(groupIndex);
		if (!group || !ValidCompiledRange(
			source.Animations, instance.Program.Animations.size())
			|| !ValidCompiledRange(source.TimelineGroups,
				instance.Program.TimelineGroups.size()))
		{
			if (outError) *outError = L"编译 VisualTransition range 无效。";
			return false;
		}
		transition = {};
		if (source.FromStateIndex != CompiledInteractionInvalidIndex)
			transition.FromState = source.FromStateIndex;
		if (source.ToStateIndex != CompiledInteractionInvalidIndex)
			transition.ToState = source.ToStateIndex;
		transition.GeneratedDurationMilliseconds =
			source.GeneratedDurationMilliseconds;
		transition.StoryboardTiming = source.StoryboardTiming;
		transition.GeneratedEasing = source.GeneratedEasing;
		transition.GeneratedEasingMode = source.GeneratedEasingMode;
		transition.GeneratedEasingParameters =
			source.GeneratedEasingParameters;
		transition.Animations.reserve(source.Animations.Count);
		for (uint32_t offset = 0; offset < source.Animations.Count; ++offset)
		{
			RuntimeAnimation animation;
			if (!TryBuildCompiledAnimation(instance.Program, instance.Values,
				instance.Targets, instance.Program.Animations[
					source.Animations.Offset + offset], animation,
				L"编译 VisualTransition Storyboard", outError)) return false;
			transition.Animations.push_back(std::move(animation));
		}
		if (!AppendCompiledTimelineGroups(instance.Program, instance.Values,
			instance.Targets, source.TimelineGroups,
			CompiledInteractionInvalidIndex, transition.Animations,
			transition.TimelineGroups,
			L"编译 VisualTransition Storyboard", outError)) return false;
		return true;
	}

	bool BuildCompiled(
		const CompiledInteractionProgramView& program,
		std::span<const BindingValue> values,
		std::span<Control* const> targets,
		std::wstring* outError)
	{
		auto fail = [&](std::wstring message)
			{
				if (outError) *outError = std::move(message);
				return false;
			};
		struct CompiledPropertyOwnership
		{
			PropertyKey Root;
			bool Exclusive = false;
			std::vector<uint64_t> ObjectPathIdentities;
		};
		std::vector<std::pair<PropertyKey, size_t>> groupProperties;
		std::vector<uint32_t> timelineGroupOwners(
			program.TimelineGroups.size(), CompiledInteractionInvalidIndex);
		uint32_t nextTimelineOwner = 0;
		std::function<bool(CompiledInteractionRange, uint32_t)>
			claimTimelineGroups;
		claimTimelineGroups = [&](CompiledInteractionRange range,
			uint32_t owner)
		{
			if (!ValidCompiledRange(range, program.TimelineGroups.size()))
				return false;
			for (uint32_t offset = 0; offset < range.Count; ++offset)
			{
				const auto index = range.Offset + offset;
				auto& existingOwner = timelineGroupOwners[index];
				if (existingOwner != CompiledInteractionInvalidIndex) return false;
				existingOwner = owner;
				const auto& timelineGroup = program.TimelineGroups[index];
				if (!ValidCompiledRange(
						timelineGroup.Animations, program.Animations.size())
					|| !ValidCompiledRange(timelineGroup.Children,
						program.TimelineGroups.size())
					|| !ValidCompiledStoryboardTiming(timelineGroup.Timing)
					|| !claimTimelineGroups(timelineGroup.Children, owner))
					return false;
			}
			return true;
		};
		auto registerControlledProperty = [&] (
			std::vector<CompiledPropertyOwnership>& localProperties,
			const PropertyKey& key,
			uint64_t objectPathIdentity,
			std::optional<size_t> groupIndex,
			std::wstring_view context) -> bool
			{
				auto local = std::find_if(localProperties.begin(),
					localProperties.end(), [&](const auto& existing)
					{ return SameProperty(existing.Root, key); });
				if (local != localProperties.end())
				{
					if (objectPathIdentity == 0 || local->Exclusive
						|| ContainsObjectPathIdentity(
							local->ObjectPathIdentities, objectPathIdentity))
						return fail(std::wstring(context)
							+ L" 控制同一属性/对象路径多次。" );
					local->ObjectPathIdentities.push_back(objectPathIdentity);
				}
				else
				{
					CompiledPropertyOwnership ownership;
					ownership.Root = key;
					ownership.Exclusive = objectPathIdentity == 0;
					if (objectPathIdentity != 0)
						ownership.ObjectPathIdentities.push_back(objectPathIdentity);
					localProperties.push_back(std::move(ownership));
				}
				if (!groupIndex) return true;
				const auto owner = std::find_if(groupProperties.begin(),
					groupProperties.end(), [&](const auto& existing)
					{ return SameProperty(existing.first, key); });
				if (owner != groupProperties.end() && owner->second != *groupIndex)
					return fail(L"不同编译视觉状态组不能控制同一属性。" );
				if (owner == groupProperties.end())
					groupProperties.emplace_back(key, *groupIndex);
				return true;
			};
		if (!Owner || program.Version != CompiledInteractionProgramViewVersion)
			return fail(L"编译交互程序版本无效。" );
		if (program.TargetCount == 0 || targets.size() != program.TargetCount
			|| targets.empty() || targets.front() != Owner
			|| std::any_of(targets.begin(), targets.end(),
				[](const auto* target) { return target == nullptr; }))
			return fail(L"编译交互 target slot 表无效。" );
		for (size_t index = 1; index < targets.size(); ++index)
		{
			auto* target = targets[index];
			if (target == Owner
				|| std::find(targets.begin(), targets.begin() + index, target)
					!= targets.begin() + index
				|| target->GetTemplatedParent() != Owner
				|| std::none_of(Owner->_templateNameScope.begin(),
					Owner->_templateNameScope.end(),
					[target](const auto& registered)
					{ return registered.second == target; }))
				return fail(L"编译交互 target slot 不属于当前模板实例。" );
		}
		if (program.Groups.empty() && program.EventTriggers.empty())
			return fail(L"编译交互程序不能为空。" );
		FailedCompiledSnapshots.clear();
		std::vector<VisualStateGroupToken> compiledGroupTokens;
		compiledGroupTokens.reserve(program.Groups.size());

		for (const auto& sourceGroup : program.Groups)
		{
			if (!sourceGroup.Token
				|| !ValidCompiledRange(sourceGroup.States, program.States.size())
				|| sourceGroup.States.Count == 0
				|| sourceGroup.FallbackStateIndex >= sourceGroup.States.Count
				|| !ValidCompiledRange(
					sourceGroup.Transitions, program.Transitions.size())
				|| !ValidCompiledRange(sourceGroup.ConditionOperands,
					program.GroupConditionOperands.size()))
				return fail(L"编译视觉状态组 range/token 无效。" );
			if (std::find(compiledGroupTokens.begin(), compiledGroupTokens.end(),
				sourceGroup.Token) != compiledGroupTokens.end())
				return fail(L"编译视觉状态组 token 重复。" );
			const size_t groupIndex = compiledGroupTokens.size();
			compiledGroupTokens.push_back(sourceGroup.Token);
			std::vector<VisualStateToken> stateTokens;
			stateTokens.reserve(sourceGroup.States.Count);
			std::vector<std::pair<uint32_t, uint32_t>> transitionSelectors;
			transitionSelectors.reserve(sourceGroup.Transitions.Count);
			std::vector<const DeclarativeEventDefinition*> groupEvents;
			std::vector<uint32_t> actualGroupConditionOperands;
			for (uint32_t localState = 0;
				localState < sourceGroup.States.Count; ++localState)
			{
				const auto& sourceState = program.States[
					sourceGroup.States.Offset + localState];
				if (!sourceState.Token
					|| !ValidCompiledRange(
						sourceState.Conditions, program.Conditions.size())
					|| !ValidCompiledRange(
						sourceState.Events, program.StateEvents.size())
					|| !ValidCompiledRange(
						sourceState.Setters, program.Setters.size())
					|| !ValidCompiledRange(
						sourceState.Animations, program.Animations.size())
					|| !ValidCompiledRange(
						sourceState.TimelineGroups,
						program.TimelineGroups.size())
					|| !ValidCompiledStoryboardTiming(
						sourceState.StoryboardTiming))
					return fail(L"编译视觉状态 range/token 无效。" );
				if (nextTimelineOwner == CompiledInteractionInvalidIndex
					|| !claimTimelineGroups(
						sourceState.TimelineGroups, nextTimelineOwner++))
					return fail(L"编译视觉状态 ParallelTimeline 图无效。" );
				if (std::find(stateTokens.begin(), stateTokens.end(),
					sourceState.Token) != stateTokens.end())
					return fail(L"编译视觉状态 token 重复。" );
				stateTokens.push_back(sourceState.Token);
				const bool fallback = localState == sourceGroup.FallbackStateIndex;
				if (sourceState.Conditions.Count != 0
					&& sourceState.Events.Count != 0)
					return fail(L"编译视觉状态不能同时声明属性和事件触发器。" );
				if (fallback != (sourceState.Conditions.Count == 0
					&& sourceState.Events.Count == 0))
					return fail(L"编译视觉状态 fallback 索引无效。" );
				std::vector<const DependencyProperty*> stateConditions;
				for (uint32_t offset = 0;
					offset < sourceState.Conditions.Count; ++offset)
				{
					const auto& condition = program.Conditions[
						sourceState.Conditions.Offset + offset];
					if (condition.OperandIndex >= program.PropertyOperands.size()
						|| condition.ValueIndex >= values.size())
						return fail(L"编译视觉状态 condition 索引越界。" );
					const auto& operand =
						program.PropertyOperands[condition.OperandIndex];
					if (operand.TargetSlot != 0 || !operand.Property.Identity())
						return fail(L"视觉状态 condition 必须引用 owner 属性。" );
					const auto* metadata = Owner->GetPropertyMetadata(
						*operand.Property.Identity());
					BindingValue converted;
					BindingValue coerced;
					if (!metadata || !metadata->CanRead()
						|| !metadata->TryConvert(
							values[condition.ValueIndex], converted)
						|| !metadata->TryCoerce(*Owner, converted, coerced))
						return fail(L"编译视觉状态 condition 无效。" );
					const auto* property = &metadata->Property();
					if (std::find(stateConditions.begin(), stateConditions.end(), property)
						!= stateConditions.end())
						return fail(L"编译视觉状态 condition 属性重复。" );
					stateConditions.push_back(property);
					if (std::find(actualGroupConditionOperands.begin(),
						actualGroupConditionOperands.end(), condition.OperandIndex)
						== actualGroupConditionOperands.end())
						actualGroupConditionOperands.push_back(condition.OperandIndex);
				}
				for (uint32_t offset = 0; offset < sourceState.Events.Count; ++offset)
				{
					const auto* event = program.StateEvents[
						sourceState.Events.Offset + offset];
					if (!event || std::find(groupEvents.begin(), groupEvents.end(), event)
						!= groupEvents.end())
						return fail(L"编译视觉状态 event identity 为空或在组内重复。" );
					groupEvents.push_back(event);
				}
				std::vector<CompiledPropertyOwnership> stateProperties;
				for (uint32_t offset = 0; offset < sourceState.Setters.Count; ++offset)
				{
					const auto& setter = program.Setters[
						sourceState.Setters.Offset + offset];
					if (setter.OperandIndex >= program.PropertyOperands.size()
						|| setter.ValueIndex >= values.size())
						return fail(L"编译视觉状态 setter 索引越界。" );
					const auto& operand = program.PropertyOperands[setter.OperandIndex];
					if (operand.TargetSlot >= targets.size()
						|| !operand.Property.Identity())
						return fail(L"编译视觉状态 setter operand 无效。" );
					auto* target = targets[operand.TargetSlot];
					const auto* metadata = target->GetPropertyMetadata(
						*operand.Property.Identity());
					BindingValue converted;
					BindingValue coerced;
					if (!metadata || !metadata->CanWrite()
						|| !metadata->TryConvert(
							values[setter.ValueIndex], converted)
						|| !metadata->TryCoerce(*target, converted, coerced))
						return fail(L"编译视觉状态 setter 无效。" );
					if (!registerControlledProperty(stateProperties,
						{ target, &metadata->Property() }, 0, groupIndex,
						L"编译视觉状态")) return false;
				}
				for (uint32_t offset = 0; offset < sourceState.Animations.Count; ++offset)
				{
					RuntimeAnimation animation;
					if (!TryBuildCompiledAnimation(program, values, targets,
						program.Animations[sourceState.Animations.Offset + offset],
						animation, L"编译视觉状态 Storyboard", outError))
						return false;
					if (!registerControlledProperty(stateProperties,
						{ animation.Target, PropertyIdentity(animation.Metadata) },
						ObjectPathIdentity(animation.ObjectPath), groupIndex,
						L"编译视觉状态 Storyboard")) return false;
				}
				std::vector<RuntimeAnimation> nestedStateAnimations;
				std::vector<RuntimeTimelineGroup> nestedStateGroups;
				if (!AppendCompiledTimelineGroups(program, values, targets,
					sourceState.TimelineGroups,
					CompiledInteractionInvalidIndex, nestedStateAnimations,
					nestedStateGroups, L"编译视觉状态 Storyboard", outError))
					return false;
				for (const auto& animation : nestedStateAnimations)
					if (!registerControlledProperty(stateProperties,
						{ animation.Target, PropertyIdentity(animation.Metadata) },
						ObjectPathIdentity(animation.ObjectPath), groupIndex,
						L"编译视觉状态 Storyboard")) return false;
			}
			if (sourceGroup.ConditionOperands.Count
				!= actualGroupConditionOperands.size())
				return fail(L"编译视觉状态组 condition operand 表不匹配。" );
			for (uint32_t offset = 0;
				offset < sourceGroup.ConditionOperands.Count; ++offset)
			{
				const auto operandIndex = program.GroupConditionOperands[
					sourceGroup.ConditionOperands.Offset + offset];
				if (operandIndex != actualGroupConditionOperands[offset]
					|| operandIndex >= program.PropertyOperands.size())
					return fail(L"编译视觉状态组 condition operand 表无效。" );
				const auto& operand = program.PropertyOperands[operandIndex];
				if (operand.TargetSlot != 0 || !operand.Property.Identity())
					return fail(L"编译视觉状态组 condition operand 必须引用 owner。" );
				const auto* metadata = Owner->GetPropertyMetadata(
					*operand.Property.Identity());
				if (!metadata || !metadata->CanRead())
					return fail(L"编译视觉状态组 condition operand 不可读。" );
			}
			for (uint32_t offset = 0;
				offset < sourceGroup.Transitions.Count; ++offset)
			{
				const auto& sourceTransition = program.Transitions[
					sourceGroup.Transitions.Offset + offset];
				if ((sourceTransition.FromStateIndex
						!= CompiledInteractionInvalidIndex
						&& sourceTransition.FromStateIndex >= sourceGroup.States.Count)
					|| (sourceTransition.ToStateIndex
						!= CompiledInteractionInvalidIndex
						&& sourceTransition.ToStateIndex >= sourceGroup.States.Count)
					|| !ValidCompiledEasingKind(sourceTransition.GeneratedEasing)
					|| !ValidCompiledEasingMode(sourceTransition.GeneratedEasingMode)
					|| !ValidCompiledEasingParameters(
						sourceTransition.GeneratedEasing,
						sourceTransition.GeneratedEasingParameters)
					|| !ValidCompiledRange(
						sourceTransition.Animations, program.Animations.size())
					|| !ValidCompiledRange(
						sourceTransition.TimelineGroups,
						program.TimelineGroups.size())
					|| !ValidCompiledStoryboardTiming(
						sourceTransition.StoryboardTiming))
					return fail(L"编译 VisualTransition 索引无效。" );
				if (nextTimelineOwner == CompiledInteractionInvalidIndex
					|| !claimTimelineGroups(
						sourceTransition.TimelineGroups,
						nextTimelineOwner++))
					return fail(L"编译 VisualTransition ParallelTimeline 图无效。" );
				const std::pair selector{ sourceTransition.FromStateIndex,
					sourceTransition.ToStateIndex };
				if (std::find(transitionSelectors.begin(), transitionSelectors.end(),
					selector) != transitionSelectors.end())
					return fail(L"编译 VisualTransition From/To 选择器重复。" );
				transitionSelectors.push_back(selector);
				std::vector<CompiledPropertyOwnership> transitionProperties;
				for (uint32_t animationOffset = 0;
					animationOffset < sourceTransition.Animations.Count;
					++animationOffset)
				{
					RuntimeAnimation animation;
					if (!TryBuildCompiledAnimation(program, values, targets,
						program.Animations[sourceTransition.Animations.Offset
							+ animationOffset], animation,
						L"编译 VisualTransition Storyboard", outError))
						return false;
					if (!registerControlledProperty(transitionProperties,
						{ animation.Target, PropertyIdentity(animation.Metadata) },
						ObjectPathIdentity(animation.ObjectPath), groupIndex,
						L"编译 VisualTransition Storyboard")) return false;
				}
				std::vector<RuntimeAnimation> nestedTransitionAnimations;
				std::vector<RuntimeTimelineGroup> nestedTransitionGroups;
				if (!AppendCompiledTimelineGroups(program, values, targets,
					sourceTransition.TimelineGroups,
					CompiledInteractionInvalidIndex,
					nestedTransitionAnimations, nestedTransitionGroups,
					L"编译 VisualTransition Storyboard", outError)) return false;
				for (const auto& animation : nestedTransitionAnimations)
					if (!registerControlledProperty(transitionProperties,
						{ animation.Target, PropertyIdentity(animation.Metadata) },
						ObjectPathIdentity(animation.ObjectPath), groupIndex,
						L"编译 VisualTransition Storyboard")) return false;
			}
			// Validation intentionally discards the materialized hierarchy.  Runtime
			// groups below retain only the static program index and live state.
		}

		for (const auto& sourceStoryboard : program.Storyboards)
		{
			if (!ValidCompiledRange(
				sourceStoryboard.Animations, program.Animations.size())
				|| !ValidCompiledRange(sourceStoryboard.TimelineGroups,
					program.TimelineGroups.size())
				|| (sourceStoryboard.Animations.Count == 0
					&& sourceStoryboard.TimelineGroups.Count == 0)
				|| !ValidCompiledStoryboardTiming(sourceStoryboard.Timing))
				return fail(L"编译 Storyboard animation range 越界。" );
			if (nextTimelineOwner == CompiledInteractionInvalidIndex
				|| !claimTimelineGroups(
					sourceStoryboard.TimelineGroups, nextTimelineOwner++))
				return fail(L"编译 Storyboard ParallelTimeline 图无效。" );
			std::vector<CompiledPropertyOwnership> storyboardProperties;
			for (uint32_t offset = 0;
				offset < sourceStoryboard.Animations.Count; ++offset)
			{
				RuntimeAnimation animation;
				if (!TryBuildCompiledAnimation(program, values, targets,
					program.Animations[sourceStoryboard.Animations.Offset + offset],
					animation, L"编译 EventTrigger Storyboard", outError))
					return false;
				if (!registerControlledProperty(storyboardProperties,
					{ animation.Target, PropertyIdentity(animation.Metadata) },
					ObjectPathIdentity(animation.ObjectPath), std::nullopt,
					L"编译 EventTrigger Storyboard")) return false;
			}
			std::vector<RuntimeAnimation> nestedStoryboardAnimations;
			std::vector<RuntimeTimelineGroup> nestedStoryboardGroups;
			if (!AppendCompiledTimelineGroups(program, values, targets,
				sourceStoryboard.TimelineGroups,
				CompiledInteractionInvalidIndex, nestedStoryboardAnimations,
				nestedStoryboardGroups, L"编译 EventTrigger Storyboard", outError))
				return false;
			for (const auto& animation : nestedStoryboardAnimations)
				if (!registerControlledProperty(storyboardProperties,
					{ animation.Target, PropertyIdentity(animation.Metadata) },
					ObjectPathIdentity(animation.ObjectPath), std::nullopt,
					L"编译 EventTrigger Storyboard")) return false;
			// The animation graph is validation scratch; Begin materializes it lazily.
		}
		if (std::any_of(timelineGroupOwners.begin(), timelineGroupOwners.end(),
			[](uint32_t owner)
			{ return owner == CompiledInteractionInvalidIndex; }))
			return fail(L"编译 ParallelTimeline 表含孤儿定义。" );
		std::vector<uint8_t> storyboardHasBegin(program.Storyboards.size(), 0);
		for (const auto& sourceTrigger : program.EventTriggers)
		{
			if ((!sourceTrigger.Event
					&& sourceTrigger.RoutedEvent == RoutedEventId::None)
				|| (sourceTrigger.Event
					&& sourceTrigger.RoutedEvent != RoutedEventId::None)
				|| static_cast<unsigned int>(sourceTrigger.RoutedEvent)
					>= static_cast<unsigned int>(RoutedEventId::Count)
				|| !ValidCompiledRange(sourceTrigger.Actions,
					program.Actions.size()) || sourceTrigger.Actions.Count == 0)
				return fail(L"编译 EventTrigger event/action 无效。" );
			for (uint32_t offset = 0; offset < sourceTrigger.Actions.Count; ++offset)
			{
				const auto& sourceAction = program.Actions[
					sourceTrigger.Actions.Offset + offset];
				if (!ValidCompiledActionKind(sourceAction.Kind)
					|| !ValidCompiledHandoffBehavior(sourceAction.Handoff)
					|| sourceAction.StoryboardIndex >= program.Storyboards.size())
					return fail(L"编译 Storyboard action 索引越界。" );
				if (sourceAction.Kind
					== DeclarativeStoryboardActionKind::SkipToFill)
				{
					const auto& storyboard = program.Storyboards[
						sourceAction.StoryboardIndex];
					if (storyboard.Timing.RepeatBehavior
						== DeclarativeRepeatBehaviorKind::Forever)
						return fail(L"编译 SkipStoryboardToFill 不能引用 Forever Storyboard。" );
					const auto range = storyboard.Animations;
					for (uint32_t animationOffset = 0;
						animationOffset < range.Count; ++animationOffset)
						if (program.Animations[range.Offset + animationOffset]
							.RepeatBehavior
							== DeclarativeRepeatBehaviorKind::Forever)
							return fail(L"编译 SkipStoryboardToFill 不能引用 Forever Storyboard。" );
					std::function<bool(CompiledInteractionRange)> groupForever;
					groupForever = [&](CompiledInteractionRange groups)
					{
						for (uint32_t groupOffset = 0;
							groupOffset < groups.Count; ++groupOffset)
						{
							const auto& group = program.TimelineGroups[
								groups.Offset + groupOffset];
							if (group.Timing.RepeatBehavior
								== DeclarativeRepeatBehaviorKind::Forever)
								return true;
							for (uint32_t animationOffset = 0;
								animationOffset < group.Animations.Count;
								++animationOffset)
								if (program.Animations[group.Animations.Offset
									+ animationOffset].RepeatBehavior
									== DeclarativeRepeatBehaviorKind::Forever)
									return true;
							if (groupForever(group.Children)) return true;
						}
						return false;
					};
					if (groupForever(storyboard.TimelineGroups))
						return fail(L"编译 SkipStoryboardToFill 不能引用 Forever Storyboard。" );
				}
				auto& hasBegin =
					storyboardHasBegin[sourceAction.StoryboardIndex];
				if (sourceAction.Kind == DeclarativeStoryboardActionKind::Begin)
					hasBegin = 1;
			}
		}
		if (std::any_of(storyboardHasBegin.begin(),
			storyboardHasBegin.end(), [](uint8_t hasBegin)
			{ return hasBegin == 0; }))
			return fail(L"编译 Storyboard slot 缺少 Begin action。" );

		CompiledInteractionInstance instance;
		instance.Program = program;
		instance.Values.assign(values.begin(), values.end());
		instance.Targets.assign(targets.begin(), targets.end());
		CompiledInteractions = std::move(instance);
		Groups.reserve(program.Groups.size());
		for (size_t compiledGroupIndex = 0;
			compiledGroupIndex < program.Groups.size(); ++compiledGroupIndex)
		{
			RuntimeGroup group;
			group.CompiledGroupIndex = compiledGroupIndex;
			Groups.push_back(std::move(group));
		}

		std::vector<PropertyKey> initialProperties;
		std::vector<size_t> initialStateIndices;
		initialStateIndices.reserve(Groups.size());
		auto addInitialProperty = [&](Control* target,
			const DependencyPropertyMetadata* metadata)
			{
				PropertyKey key{ target, PropertyIdentity(metadata) };
				if (!key.Target || !key.Property
					|| std::any_of(initialProperties.begin(),
						initialProperties.end(), [&](const auto& existing)
						{ return SameProperty(existing, key); })) return;
				initialProperties.push_back(key);
			};
		for (size_t groupIndex = 0; groupIndex < Groups.size(); ++groupIndex)
		{
			const auto stateIndex = EvaluateState(groupIndex);
			initialStateIndices.push_back(stateIndex);
			RuntimeState state;
			if (stateIndex >= StateCount(groupIndex)
				|| !TryBuildCompiledState(
					groupIndex, stateIndex, state, outError))
				return fail(L"编译视觉状态初始索引无效。" );
			for (const auto& setter : state.Setters)
				addInitialProperty(setter.Target, setter.Metadata);
			for (const auto& animation : state.Animations)
				addInitialProperty(animation.Target, animation.Metadata);
		}
		std::vector<PropertySnapshot> initialSnapshots;
		initialSnapshots.reserve(initialProperties.size() * 2);
		for (const auto& key : initialProperties)
			for (const auto source : {
				DependencyPropertyValueSource::VisualState,
				DependencyPropertyValueSource::Animation })
				CapturePropertySnapshot(key, source, initialSnapshots);

		FailedCompiledSnapshots = std::move(initialSnapshots);
		SuppressStateChangedEvents = true;
		for (size_t index = 0; index < Groups.size(); ++index)
			if (!GoTo(index, initialStateIndices[index], false, outError))
			{
				SuppressStateChangedEvents = false;
				return false;
			}
		SuppressStateChangedEvents = false;

		// Publish compiled-trigger subscriptions only after every initial group has
		// committed, so this runtime cannot react while its program is half built.
		if (std::any_of(program.Groups.begin(), program.Groups.end(),
			[](const auto& group) { return group.ConditionOperands.Count != 0; }))
			Connections.push_back(Owner->OnPropertyValueChanged.Subscribe(
				[this](DependencyObject*, const DependencyPropertyChangedEventArgs& args)
				{ OnHostPropertyChanged(args); }));
		const bool consumesDeclarativeEvents = std::any_of(
			program.Groups.begin(), program.Groups.end(), [&](const auto& group)
			{
				for (uint32_t offset = 0; offset < group.States.Count; ++offset)
					if (program.States[group.States.Offset + offset].Events.Count != 0)
						return true;
				return false;
			}) || std::any_of(program.EventTriggers.begin(),
				program.EventTriggers.end(),
			[](const auto& trigger) { return trigger.Event != nullptr; });
		if (consumesDeclarativeEvents)
			Connections.push_back(Owner->OnDeclarativeEvent.Subscribe(
				[this](Control*, DeclarativeEventArgs& args)
				{ OnHostDeclarativeEvent(args); }));
		std::vector<RoutedEventId> subscribedRoutedEvents;
		for (const auto& trigger : program.EventTriggers)
		{
			if (trigger.RoutedEvent == RoutedEventId::None
				|| std::find(subscribedRoutedEvents.begin(),
					subscribedRoutedEvents.end(), trigger.RoutedEvent)
					!= subscribedRoutedEvents.end()) continue;
			subscribedRoutedEvents.push_back(trigger.RoutedEvent);
			Connections.push_back(Owner->AddHandler(trigger.RoutedEvent,
				[this](Control*, RoutedEventArgs& args)
				{ OnHostRoutedEvent(args); }));
		}
		FailedCompiledSnapshots.clear();
		if (outError) outError->clear();
		return true;
	}
	};

std::optional<unsigned long long>
Control::ExchangeVisualStateAnimationClockOverrideForTesting(
	std::optional<unsigned long long> value) noexcept
{
	if (!_declarativeVisualStates) return std::nullopt;
	auto previous = _declarativeVisualStates->AnimationClockOverride;
	_declarativeVisualStates->AnimationClockOverride = value;
	return previous;
}

bool Control::VisualStateAnimationAdvanceFailedForTesting() const noexcept
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->LastAdvanceFailed;
}

void Control::FailNextVisualStateAnimationFrameCommitForTesting() noexcept
{
	if (_declarativeVisualStates)
		_declarativeVisualStates->FailNextFrameCommitForTesting = true;
}

size_t Control::VisualStateAnimationLeafCountForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationLeafCountForTesting() : 0u;
}

size_t Control::VisualStateAnimationRootClockCountForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationRootClockCountForTesting() : 0u;
}

bool Control::SeekSingleVisualStateAnimationRootAlignedForTesting(
	unsigned long long offsetMilliseconds)
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->SeekSingleRootAlignedForTesting(
			offsetMilliseconds);
}

bool Control::SeekSingleVisualStateAnimationRootForTesting(
	unsigned long long offsetMilliseconds)
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->SeekSingleRootForTesting(
			offsetMilliseconds);
}

bool Control::PauseSingleVisualStateAnimationRootForTesting()
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->PauseSingleRootForTesting();
}

bool Control::ResumeSingleVisualStateAnimationRootForTesting()
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->ResumeSingleRootForTesting();
}

bool Control::SetSpeedRatioSingleVisualStateAnimationRootForTesting(
	double ratio)
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->SetSpeedRatioSingleRootForTesting(ratio);
}

bool Control::SkipSingleVisualStateAnimationRootToFillForTesting()
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->SkipSingleRootToFillForTesting();
}

bool Control::StopSingleVisualStateAnimationRootForTesting()
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->StopSingleRootForTesting();
}

bool Control::RemoveSingleVisualStateAnimationRootForTesting()
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->RemoveSingleRootForTesting();
}

std::optional<DeclarativeClockObservation>
Control::QuerySingleVisualStateAnimationRootForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->QuerySingleRootForTesting()
		: std::nullopt;
}

size_t Control::VisualStateAnimationClockNodeCountForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationClockNodeCountForTesting() : 0u;
}

size_t Control::VisualStateAnimationLayerStackCountForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationLayerStackCountForTesting() : 0u;
}

size_t Control::VisualStateAnimationLayerCountForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationLayerCountForTesting() : 0u;
}

size_t Control::VisualStateAnimationLayerMaxDepthForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->AnimationLayerMaxDepthForTesting() : 0u;
}

size_t Control::VisualStateAnimationRootClockChildCountForTesting(
	size_t rootIndex) const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->RootClockChildCountForTesting(rootIndex) : 0u;
}

uint64_t Control::VisualStateAnimationSingleRootClockIdForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->SingleRootClockIdForTesting() : 0u;
}

uint64_t Control::VisualStateAnimationSingleRootClockNodeTokenForTesting() const noexcept
{
	return _declarativeVisualStates
		? _declarativeVisualStates->SingleRootClockNodeTokenForTesting() : 0u;
}

bool Control::VisualStateAnimationClockIdentityValidForTesting() const noexcept
{
	return !_declarativeVisualStates
		|| _declarativeVisualStates->ClockIdentityValidForTesting();
}

#if CUI_ENABLE_DYNAMIC_XAML
bool Control::InstallDesignInteractionDefinitions(
	std::vector<DeclarativeVisualStateGroupDefinition> groups,
	std::vector<DeclarativeEventTriggerDefinition> eventTriggers,
	std::wstring* outError)
{
	if (_declarativeVisualStates
		&& (_declarativeVisualStates->DeclarativeInteractionsDefined
			|| _declarativeVisualStates->InstallingInteractions))
	{
		if (outError) *outError = L"声明交互已经安装。";
		return false;
	}
	if (!_declarativeVisualStates)
	{
		_declarativeVisualStates =
			std::make_unique<DeclarativeVisualStateRuntime>();
		_declarativeVisualStates->Owner = this;
	}
	_declarativeVisualStates->InstallingInteractions = true;
	bool installed = false;
	try
	{
		installed = _declarativeVisualStates->Build(
			std::move(groups), std::move(eventTriggers), outError);
	}
	catch (...)
	{
		_declarativeVisualStates->ResetFailedDeclarativeInteractionBuild();
		throw;
	}
	if (!installed)
	{
		_declarativeVisualStates->ResetFailedDeclarativeInteractionBuild();
		return false;
	}
	_declarativeVisualStates->DeclarativeInteractionsDefined = true;
	_declarativeVisualStates->InstallingInteractions = false;
	if (outError) outError->clear();
	return true;
}
#endif

bool Control::InstallCompiledInteractions(
	const CompiledInteractionProgramView& program,
	std::span<const BindingValue> values,
	std::span<Control* const> targets,
	std::wstring* outError)
{
	if (_declarativeVisualStates
		&& (_declarativeVisualStates->DeclarativeInteractionsDefined
			|| _declarativeVisualStates->InstallingInteractions))
	{
		if (outError) *outError = L"声明交互已经安装。";
		return false;
	}
	if (!_declarativeVisualStates)
	{
		_declarativeVisualStates =
			std::make_unique<DeclarativeVisualStateRuntime>();
		_declarativeVisualStates->Owner = this;
	}
	_declarativeVisualStates->InstallingInteractions = true;
	bool installed = false;
	try
	{
		installed = _declarativeVisualStates->BuildCompiled(
			program, values, targets, outError);
	}
	catch (...)
	{
		_declarativeVisualStates->ResetFailedCompiledInteractionBuild();
		throw;
	}
	if (!installed)
	{
		_declarativeVisualStates->ResetFailedCompiledInteractionBuild();
		return false;
	}
	_declarativeVisualStates->DeclarativeInteractionsDefined = true;
	_declarativeVisualStates->InstallingInteractions = false;
	if (outError) outError->clear();
	return true;
}

bool Control::SynchronizeStyleTriggerActions(
	DependencyPropertyValueSource source,
	const std::shared_ptr<const ControlStyleSheet>& sheet,
	const ControlStyleResolution& resolution)
{
	if (!_declarativeVisualStates && resolution.Triggers.empty()) return true;
	if (!_declarativeVisualStates)
	{
		_declarativeVisualStates =
			std::make_unique<DeclarativeVisualStateRuntime>();
		_declarativeVisualStates->Owner = this;
	}
	std::wstring ignored;
	return _declarativeVisualStates->SynchronizeStyleTriggerActions(
		source, sheet, resolution.Triggers, &ignored);
}

bool Control::PruneStyleTriggerActions(
	DependencyPropertyValueSource source,
	const std::vector<std::shared_ptr<const ControlStyleSheet>>& sheets)
{
	if (!_declarativeVisualStates) return true;
	std::vector<const ControlStyleSheet*> visible;
	visible.reserve(sheets.size());
	for (const auto& sheet : sheets)
		if (sheet) visible.push_back(sheet.get());
	return _declarativeVisualStates->PruneStyleTriggerActions(source, visible);
}

bool Control::GoToVisualState(
	VisualStateGroupToken groupToken,
	VisualStateToken stateToken,
	bool useTransitions,
	std::wstring* outError)
{
	if (!_declarativeVisualStates || !groupToken || !stateToken)
	{
		if (outError) *outError = L"控件未安装有效的编译视觉状态。";
		return false;
	}
	for (size_t groupIndex = 0;
		groupIndex < _declarativeVisualStates->Groups.size(); ++groupIndex)
	{
		if (_declarativeVisualStates->GroupTokenAt(groupIndex) != groupToken)
			continue;
		for (size_t stateIndex = 0;
			stateIndex < _declarativeVisualStates->StateCount(groupIndex); ++stateIndex)
			if (_declarativeVisualStates->StateTokenAt(
				groupIndex, stateIndex) == stateToken)
				return _declarativeVisualStates->GoTo(
					groupIndex, stateIndex, useTransitions, outError);
		if (outError) *outError = L"编译视觉状态 token 不存在。";
		return false;
	}
	if (outError) *outError = L"编译视觉状态组 token 不存在。";
	return false;
}

VisualStateToken Control::GetCurrentVisualState(
	VisualStateGroupToken groupToken) const noexcept
{
	if (!_declarativeVisualStates || !groupToken) return {};
	for (size_t groupIndex = 0;
		groupIndex < _declarativeVisualStates->Groups.size(); ++groupIndex)
		if (_declarativeVisualStates->GroupTokenAt(groupIndex) == groupToken)
		{
			const auto& group = _declarativeVisualStates->Groups[groupIndex];
			if (group.Pending
				&& group.Pending->TargetState
					< _declarativeVisualStates->StateCount(groupIndex))
				return _declarativeVisualStates->StateTokenAt(
					groupIndex, group.Pending->TargetState);
			if (group.CurrentState && *group.CurrentState
				< _declarativeVisualStates->StateCount(groupIndex))
				return _declarativeVisualStates->StateTokenAt(
					groupIndex, *group.CurrentState);
			return {};
		}
	return {};
}

bool Control::HasActiveVisualStateAnimations() const noexcept
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->HasActiveAnimations();
}

void Control::CollectDeclarativeCompositionAnimationTargets(
	std::vector<Control*>& transformTargets,
	std::vector<Control*>& opacityTargets) const
{
	if (_declarativeVisualStates)
		_declarativeVisualStates->CollectCompositionAnimationTargets(
			transformTargets, opacityTargets);
}

bool Control::HasRetainedVisualStateAnimationRoots() const noexcept
{
	return _declarativeVisualStates
		&& !_declarativeVisualStates->ClockNodes.Roots.empty();
}

void Control::SynchronizeDeclarativeAnimationWindowRegistration() noexcept
{
	if (auto* window = GetPresentationWindow())
		window->SynchronizeActiveDeclarativeAnimationControl(
			this, HasRetainedVisualStateAnimationRoots());
}

void Control::RefreshDeclarativeAnimationWindowScheduling() noexcept
{
	if (auto* window = GetPresentationWindow())
		window->RefreshAnimationTimer();
}

void Control::SynchronizeNativeAnimationWindowRegistration() noexcept
{
	if (auto* window = GetPresentationWindow())
		window->SynchronizeRetainedNativeAnimationControl(
			this, HasRetainedNativeAnimation());
}

bool Control::AdvanceVisualStateAnimations(
	unsigned long long nowMilliseconds)
{
	return _declarativeVisualStates
		&& _declarativeVisualStates->AdvanceAnimations(nowMilliseconds);
}

bool Control::SetDataContext(BindingSourceReference value)
{
	return TrySetPropertyValue(
		DataContextProperty(), BindingValue(std::move(value)),
		DependencyPropertyValueSource::Local);
}

bool Control::ClearDataContext()
{
	return ClearPropertyValue(
		DataContextProperty(), DependencyPropertyValueSource::Local);
}

IBindingSource& Control::DataContextSource()
{
	if (!_dataContextSource)
		_dataContextSource = std::make_unique<BindingSourceProxy>(
			_effectiveDataContext);
	return *_dataContextSource;
}

void Control::SetInheritedDataContext(BindingSourceReference value)
{
	if (value)
		(void)TrySetPropertyValue(
			DataContextProperty(), BindingValue(std::move(value)),
			DependencyPropertyValueSource::Inherited);
	else
		(void)ClearPropertyValue(
			DataContextProperty(), DependencyPropertyValueSource::Inherited);
}

void Control::UpdateEffectiveDataContext(BindingSourceReference value)
{
	if (_effectiveDataContext == value) return;
	_effectiveDataContext = std::move(value);
	if (_dataContextSource)
		_dataContextSource->SetSource(_effectiveDataContext);
	RebuildStyleDataContextSubscriptions();
	RefreshStyleValues(false);
	constexpr auto dataContextToken =
		MakeBindingSourcePropertyToken(L"DataContext");
#if CUI_ENABLE_DYNAMIC_XAML
	_dataContextChanged.Notify(L"DataContext");
#else
	_dataContextChanged.Notify(dataContextToken);
#endif
	if (!_applyingPropertyMetadata)
	{
#if CUI_ENABLE_DYNAMIC_XAML
		_bindingSourcePropertyChanged.Notify(L"DataContext");
#else
		_bindingSourcePropertyChanged.Notify(dataContextToken);
#endif
	}
}

GET_CPP(Control, BindingValue, Tag)
{
	static const auto& property = TagProperty();
	return GetDependencyPropertyValue<BindingValue>(property);
}

SET_CPP(Control, BindingValue, Tag)
{
	static const auto& property = TagProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, CursorKind, Cursor)
{
	static const auto& property = CursorProperty();
	return GetDependencyPropertyValue<CursorKind>(property);
}

SET_CPP(Control, CursorKind, Cursor)
{
	static const auto& property = CursorProperty();
	(void)SetDependencyPropertyValue(property, value);
}

CursorKind Control::ResolvePointerCursor(int localX, int localY)
{
	// Auto is the native projection of WPF's null/default Cursor. It remains
	// inheritable without suppressing a behavior host's region-sensitive cursor.
	// Every concrete shape is authoritative regardless of its value source.
	return Cursor == CursorKind::Auto
		? QueryCursor(localX, localY) : Cursor;
}

GET_CPP(Control, bool, Focusable)
{
	return GetDependencyPropertyValue<bool>(FocusableProperty());
}

SET_CPP(Control, bool, Focusable)
{
	static const auto& property = FocusableProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, bool, IsTabStop)
{
	static const auto& property = IsTabStopProperty();
	return GetDependencyPropertyValue<bool>(property);
}

SET_CPP(Control, bool, IsTabStop)
{
	static const auto& property = IsTabStopProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, int, TabIndex)
{
	static const auto& property = TabIndexProperty();
	return GetDependencyPropertyValue<int>(property);
}

SET_CPP(Control, int, TabIndex)
{
	static const auto& property = TabIndexProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, bool, IsFocused)
{
	return _isFocused;
}

GET_CPP(Control, bool, IsKeyboardFocused)
{
	// WPF computes this seed property from Keyboard.FocusedElement rather than
	// returning its notification cache. During FocusWithin publication the
	// authoritative owner has already changed, while the exact DP notification
	// is intentionally still pending.
	if (auto* window = GetPresentationWindow())
		return window->GetKeyboardFocusedElement() == this;
	return _isKeyboardFocused;
}

GET_CPP(Control, bool, IsKeyboardFocusVisible)
{
	return _isKeyboardFocusVisible;
}

GET_CPP(Control, bool, IsKeyboardFocusWithin)
{
	return _isKeyboardFocusWithin;
}

GET_CPP(Control, bool, IsMouseOver)
{
	return _isMouseOver;
}

GET_CPP(Control, bool, IsMouseDirectlyOver)
{
	return _isMouseDirectlyOver;
}

GET_CPP(Control, bool, IsMouseCaptureWithin)
{
	return _isMouseCaptureWithin;
}

void Control::SetIsFocusedCore(bool value)
{
	if (_isFocused == value) return;
	const ControlWeakReference selfReference(this);
	std::exception_ptr error;
	try
	{
		if (!SetReadOnlyPropertyField(
			IsFocusedPropertyKey(),
			_isFocused, value)) return;
	}
	catch (...) { error = std::current_exception(); }
	if (auto* live = selfReference.Get(); live && live->_isFocused == value)
		live->SetStyleState(ControlStyleState::LogicalFocused, value);
	if (error) std::rethrow_exception(error);
}

void Control::SetIsKeyboardFocusedCore(bool value)
{
	if (_isKeyboardFocused == value) return;
	const ControlWeakReference selfReference(this);
	std::exception_ptr error;
	try
	{
		if (!SetReadOnlyPropertyField(
			IsKeyboardFocusedPropertyKey(),
			_isKeyboardFocused, value)) return;
	}
	catch (...) { error = std::current_exception(); }
	auto* live = selfReference.Get();
	if (!live || live->_isKeyboardFocused != value)
	{
		if (error) std::rethrow_exception(error);
		return;
	}
	if (!value)
	{
		// Caret animation is presentation state owned by the focused element.
		// Do not wait for a later OnRender to retire it: focus can move while the
		// old editor is culled or while input capture keeps producing frames.
		live->_caretBlinkFocused = false;
		live->_caretBlinkRectValid = false;
		live->_caretBlinkRect = { 0, 0, 0, 0 };
		live->SynchronizeNativeAnimationWindowRegistration();
	}

	live->_defaultLeftButtonPressActive = value
		? live->_defaultLeftButtonPressActive : false;
	if (!value) live->SetIsKeyboardFocusVisibleCore(false);
	live->SetStyleState(ControlStyleState::Focused, value);
	if (!value) live->SetStyleState(ControlStyleState::Pressed, false);
	if (error) std::rethrow_exception(error);
}

void Control::SetIsKeyboardFocusVisibleCore(bool value)
{
	if (_isKeyboardFocusVisible == value) return;
	(void)SetReadOnlyPropertyField(
		IsKeyboardFocusVisiblePropertyKey(),
		_isKeyboardFocusVisible, value);
}

void Control::SetIsMouseDirectlyOverCore(bool value)
{
	if (_isMouseDirectlyOver == value) return;
	(void)SetReadOnlyPropertyField(
		IsMouseDirectlyOverPropertyKey(),
		_isMouseDirectlyOver, value);
}

bool Control::StageReverseInheritedPropertyChange(
	cui::framework::ReverseInheritedPropertyKind kind,
	bool value,
	DeferredPropertyChange& change)
{
	using Kind = cui::framework::ReverseInheritedPropertyKind;
	switch (kind)
	{
	case Kind::KeyboardFocusWithin:
		return StageReadOnlyPropertyField(
			IsKeyboardFocusWithinPropertyKey(),
			_isKeyboardFocusWithin, value, change);
	case Kind::MouseOver:
		return StageReadOnlyPropertyField(
			IsMouseOverPropertyKey(),
			_isMouseOver, value, change);
	case Kind::MouseCaptureWithin:
		return StageReadOnlyPropertyField(
			IsMouseCaptureWithinPropertyKey(),
			_isMouseCaptureWithin, value, change);
	}
	return false;
}

void Control::PublishReverseInheritedPropertyChange(
	cui::framework::ReverseInheritedPropertyKind kind,
	const DeferredPropertyChange& change)
{
	if (!change.HasValue()) return;
	bool previous = false;
	bool current = false;
	if (!change.OldValue().TryGet(previous)
		|| !change.NewValue().TryGet(current)) return;
	const ControlWeakReference selfReference(this);
	std::exception_ptr error;
	try { PublishDeferredPropertyChange(change); }
	catch (...) { error = std::current_exception(); }
	auto* live = selfReference.Get();
	if (!live)
	{
		if (error) std::rethrow_exception(error);
		return;
	}
	using Kind = cui::framework::ReverseInheritedPropertyKind;
	const bool stillCurrent = [&]
	{
		switch (kind)
		{
		case Kind::KeyboardFocusWithin:
			return live->_isKeyboardFocusWithin == current;
		case Kind::MouseOver:
			return live->_isMouseOver == current;
		case Kind::MouseCaptureWithin:
			return live->_isMouseCaptureWithin == current;
		}
		return false;
	}();
	if (!stillCurrent)
	{
		if (error) std::rethrow_exception(error);
		return;
	}
	switch (kind)
	{
	case Kind::KeyboardFocusWithin:
		live->SetStyleState(ControlStyleState::KeyboardFocusWithin, current);
		break;
	case Kind::MouseOver:
		live->SetStyleState(ControlStyleState::Hovered, current);
		live = selfReference.Get();
		if (live) live->OnIsMouseOverChanged(previous, current);
		break;
	case Kind::MouseCaptureWithin:
		break;
	}
	if (error) std::rethrow_exception(error);
}

GET_CPP(Control, bool, IsFocusScope)
{
	static const auto& property =
		IsFocusScopeProperty();
	return GetDependencyPropertyValue<bool>(property);
}

SET_CPP(Control, bool, IsFocusScope)
{
	static const auto& property =
		IsFocusScopeProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, KeyboardNavigationMode, TabNavigation)
{
	static const auto& property = TabNavigationProperty();
	return GetDependencyPropertyValue<KeyboardNavigationMode>(property);
}

SET_CPP(Control, KeyboardNavigationMode, TabNavigation)
{
	static const auto& property = TabNavigationProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, KeyboardNavigationMode, DirectionalNavigation)
{
	static const auto& property = DirectionalNavigationProperty();
	return GetDependencyPropertyValue<KeyboardNavigationMode>(property);
}

SET_CPP(Control, KeyboardNavigationMode, DirectionalNavigation)
{
	static const auto& property = DirectionalNavigationProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, std::wstring, AutomationName)
{
	static const auto& property =
		AutomationNameProperty();
	return GetDependencyPropertyValue<std::wstring>(property);
}

SET_CPP(Control, std::wstring, AutomationName)
{
	static const auto& property =
		AutomationNameProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, std::wstring, AutomationFullDescription)
{
	static const auto& property = AutomationFullDescriptionProperty();
	return GetDependencyPropertyValue<std::wstring>(property);
}

SET_CPP(Control, std::wstring, AutomationFullDescription)
{
	static const auto& property = AutomationFullDescriptionProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, std::wstring, AutomationHelpText)
{
	static const auto& property =
		AutomationHelpTextProperty();
	return GetDependencyPropertyValue<std::wstring>(property);
}

SET_CPP(Control, std::wstring, AutomationHelpText)
{
	static const auto& property =
		AutomationHelpTextProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

GET_CPP(Control, std::wstring, AutomationId)
{
	static const auto& property =
		AutomationIdProperty();
	return GetDependencyPropertyValue<std::wstring>(property);
}

SET_CPP(Control, std::wstring, AutomationId)
{
	static const auto& property =
		AutomationIdProperty();
	(void)SetDependencyPropertyValue(property, std::move(value));
}

std::vector<BindingValidationResult> Control::GetValidationResults() const
{
	return _dataBindings
		? _dataBindings->GetValidationResults()
		: std::vector<BindingValidationResult>{};
}

bool Control::HasValidationIssues() const
{
	return _dataBindings && _dataBindings->HasValidationIssues();
}

bool Control::HasValidationErrors() const
{
	return _dataBindings && _dataBindings->HasValidationErrors();
}

bool Control::TryGetValidationSeverity(
	BindingValidationSeverity& severity) const
{
	const auto results = GetValidationResults();
	if (results.empty()) return false;
	severity = BindingValidationSeverity::Info;
	for (const auto& result : results)
	{
		if (ValidationSeverityRank(result.Issue.Severity)
			> ValidationSeverityRank(severity))
			severity = result.Issue.Severity;
	}
	return true;
}

std::wstring Control::GetValidationSummary(size_t maxIssues) const
{
	const auto results = GetValidationResults();
	std::vector<BindingValidationIssue> unique;
	unique.reserve(results.size());
	for (const auto& result : results)
	{
		if (std::find(unique.begin(), unique.end(), result.Issue) == unique.end())
			unique.push_back(result.Issue);
	}

	const size_t visibleCount = maxIssues == 0
		? unique.size()
		: (std::min)(unique.size(), maxIssues);
	std::wstring summary;
	for (size_t index = 0; index < visibleCount; ++index)
	{
		if (!summary.empty()) summary += L"\r\n";
		summary += L"[";
		summary += BindingValidationSeverityName(unique[index].Severity);
		summary += L"] ";
		summary += unique[index].Message;
	}
	if (unique.size() > visibleCount)
	{
		if (!summary.empty()) summary += L"\r\n";
		summary += L"+" + std::to_wstring(unique.size() - visibleCount)
			+ L" more validation issue(s)";
	}
	return summary;
}

std::wstring Control::GetEffectiveAutomationFullDescription() const
{
	const auto validation = GetValidationSummary();
	static const auto& property = AutomationFullDescriptionProperty();
	const auto description =
		GetDependencyPropertyValue<std::wstring>(property);
	if (description.empty()) return validation;
	if (validation.empty()) return description;
	return description + L"\r\n" + validation;
}

std::wstring Control::GetEffectiveAutomationName() const
{
	static const auto& property =
		AutomationNameProperty();
	const auto name = GetDependencyPropertyValue<std::wstring>(property);
	if (!name.empty()) return name;
	// Editable content is a value, not a label. Password content must never leak.
	switch (GetAutomationPeer().GetAutomationControlType())
	{
	case AutomationControlType::Edit:
	case AutomationControlType::ComboBox:
		return {};
	default:
		break;
	}
	return GetDisplayText();
}

std::wstring Control::GetDisplayText() const
{
	const auto semanticText = GetSemanticText();
	switch (GetAutomationPeer().GetAutomationControlType())
	{
	case AutomationControlType::Button:
	case AutomationControlType::Hyperlink:
	case AutomationControlType::CheckBox:
	case AutomationControlType::RadioButton:
	case AutomationControlType::Group:
	case AutomationControlType::MenuItem:
	case AutomationControlType::TabItem:
		return StripAccessKeyMarkers(semanticText);
	default:
		return semanticText;
	}
}

std::wstring Control::GetSemanticText() const
{
#if CUI_ENABLE_DYNAMIC_XAML
	return GetProjectedPropertyValueOr(
		*this, L"Text", std::wstring{});
#else
	return {};
#endif
}

wchar_t Control::GetEffectiveAccessKey() const
{
	switch (GetAutomationPeer().GetAutomationControlType())
	{
	case AutomationControlType::Button:
	case AutomationControlType::Hyperlink:
	case AutomationControlType::CheckBox:
	case AutomationControlType::RadioButton:
	case AutomationControlType::Group:
	case AutomationControlType::MenuItem:
	case AutomationControlType::TabItem:
		return FindAccessKeyMarker(GetSemanticText());
	default:
		return L'\0';
	}
}

std::wstring Control::GetEffectiveKeyboardShortcut() const
{
	const wchar_t key = GetEffectiveAccessKey();
	return key == L'\0' ? std::wstring{} : std::wstring(L"Alt+") + key;
}

bool Control::CanReceiveKeyboardFocus() const
{
	return GetDependencyPropertyValue<bool>(FocusableProperty())
		&& IsEffectivelyEnabled() && GetIsVisible();
}

bool Control::GetIsVisible() const
{
	const Control* current = this;
	const Control* fast = this;
	while (current)
	{
		// A transient root owns a separate presentation scene. Its own
		// visibility still observes its suppression flag, but descendants in
		// that scene do not inherit the wrapper's main-tree projection gate.
		if (current != this
			&& current->BreaksVisualPresentationInheritance()) break;
		if (current->_presentationSuppressed
			|| current->_visibility != ::Visibility::Visible) return false;
		if (current->BreaksVisualPresentationInheritance())
			break;
		current = current->_visualParent;
		if (fast) fast = fast->_visualParent;
		if (fast) fast = fast->_visualParent;
		if (current && current == fast) return false;
	}
	return true;
}

bool Control::IsEffectivelyEnabled() const noexcept
{
	const Control* current = this;
	const Control* fast = this;
	while (current)
	{
		if (!current->_localEnabled
			|| (current->_hasCommandCanExecute
				&& !current->_commandCanExecute))
			return false;
		current = current->GetRoutedParent();
		if (fast) fast = fast->GetRoutedParent();
		if (fast) fast = fast->GetRoutedParent();
		if (current && current == fast) return false;
	}
	return true;
}

namespace
{
	void CaptureEffectiveIsEnabledSubtree(
		Control& root,
		std::vector<std::pair<ControlWeakReference, bool>>& snapshot,
		std::unordered_set<Control*>& visited)
	{
		if (!visited.insert(&root).second) return;
		snapshot.emplace_back(
			ControlWeakReference(&root), root.IsEffectivelyEnabled());
		for (auto* child : root.GetLayoutChildrenView())
			if (child && child->GetRoutedParent() == &root)
				CaptureEffectiveIsEnabledSubtree(*child, snapshot, visited);
		for (auto* child : root.GetLogicalChildrenView())
			if (child && child->GetRoutedParent() == &root)
				CaptureEffectiveIsEnabledSubtree(*child, snapshot, visited);
	}

	std::vector<std::pair<ControlWeakReference, bool>>
		CaptureEffectiveIsEnabledSubtree(
			Control& root)
	{
		std::vector<std::pair<ControlWeakReference, bool>> snapshot;
		std::unordered_set<Control*> visited;
		CaptureEffectiveIsEnabledSubtree(root, snapshot, visited);
		return snapshot;
	}

	void CaptureEffectiveIsVisibleSubtree(
		Control& root,
		std::vector<std::pair<ControlWeakReference, bool>>& snapshot,
		std::unordered_set<Control*>& visited)
	{
		if (!visited.insert(&root).second) return;
		snapshot.emplace_back(
			ControlWeakReference(&root), root.GetIsVisible());
		for (auto* child : root.GetLayoutChildrenView())
			if (child && child->GetVisualParent() == &root)
				CaptureEffectiveIsVisibleSubtree(*child, snapshot, visited);
	}

	std::vector<std::pair<ControlWeakReference, bool>>
		CaptureEffectiveIsVisibleSubtree(Control& root)
	{
		std::vector<std::pair<ControlWeakReference, bool>> snapshot;
		std::unordered_set<Control*> visited;
		CaptureEffectiveIsVisibleSubtree(root, snapshot, visited);
		return snapshot;
	}
}

void Control::PublishEffectiveIsEnabledChanges(
	std::vector<std::pair<ControlWeakReference, bool>> snapshot)
{
	std::vector<ControlWeakReference> cursorRefreshWindows;
	for (const auto& [elementReference, previousValue] : snapshot)
	{
		auto* element = elementReference.Get();
		if (!element || element->IsDestroying()) continue;
		const bool current = element->IsEffectivelyEnabled();
		if (current == previousValue) continue;
		bool publishedThroughProperty = false;
		if (const auto* metadata = element->GetPropertyMetadata(
			Control::IsEnabledProperty()))
		{
			// Command availability is an effective-value coercion, not a new
			// local value. Publish it through the dependency-property channel so
			// bindings, triggers and accessibility stay coherent.
			element->ApplyPropertyMetadataChange(
				*metadata, BindingValue(previousValue), BindingValue(current));
			publishedThroughProperty = true;
		}
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->IsEffectivelyEnabled() != current) continue;
		element->OnEffectiveIsEnabledChanged(previousValue, current);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->IsEffectivelyEnabled() != current) continue;
		// The property notification above already re-resolved every style whose
		// triggers read IsEnabled (style property-condition subscription). A
		// second unconditional resolution per descendant is pure overhead when a
		// large subtree is enabled/disabled at once.
		if (!publishedThroughProperty
			|| !element->_stylePropertyConditionConnection.Connected())
			element->RefreshStyleValues(false);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->IsEffectivelyEnabled() != current) continue;
		element->InvalidateVisual();
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->IsEffectivelyEnabled() != current) continue;
		if (element->GetPresentationWindow())
		{
			ControlWeakReference windowReference(element->GetPresentationWindow());
			auto resolveWindow = [&]() -> Window*
				{
					auto* liveElement = elementReference.Get();
					auto* liveWindow = dynamic_cast<Window*>(windowReference.Get());
					return liveElement && !liveElement->IsDestroying()
						&& liveElement->IsEffectivelyEnabled() == current
						&& liveWindow
						&& liveElement->GetPresentationWindow() == liveWindow
						? liveWindow : nullptr;
				};
			auto* window = resolveWindow();
			if (!window) continue;
			if (!current)
			{
				if (window->GetKeyboardFocusedElement() == element)
					window->SetKeyboardFocus(
						nullptr, true, FocusChangeReason::EligibilityChanged);
				element = elementReference.Get();
				window = resolveWindow();
				if (!element || !window) continue;
				if (window->GetMouseCaptured() == element)
					(void)window->ReleaseMouseCapture(element);
				element = elementReference.Get();
				window = resolveWindow();
				if (!element || !window) continue;
				if (element->IsMouseOver)
					cursorRefreshWindows.emplace_back(window);
			}
			element = elementReference.Get();
			window = resolveWindow();
			if (!element || !window) continue;
			window->NotifyAccessibilityEvent(
				element, AccessibilityChange::State);
		}
	}
	for (const auto& windowReference : cursorRefreshWindows)
		if (auto* window = dynamic_cast<Window*>(windowReference.Get()))
			cui::framework::WindowAccess::UpdateCursorFromCurrentMouse(*window);
}

void Control::PublishEffectiveIsVisibleChanges(
	std::vector<std::pair<ControlWeakReference, bool>> snapshot)
{
	std::vector<ControlWeakReference> cursorRefreshWindows;
	for (const auto& [elementReference, previousValue] : snapshot)
	{
		auto* element = elementReference.Get();
		if (!element || element->IsDestroying()) continue;
		const bool current = element->GetIsVisible();
		if (current == previousValue) continue;
		bool publishedThroughProperty = false;
		if (const auto* metadata = element->GetPropertyMetadata(
			Control::IsVisibleProperty()))
		{
			element->ApplyPropertyMetadataChange(
				*metadata, BindingValue(previousValue), BindingValue(current));
			publishedThroughProperty = true;
		}
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->GetIsVisible() != current) continue;
		element->OnEffectiveIsVisibleChanged(previousValue, current);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->GetIsVisible() != current) continue;
		DependencyPropertyChangedEventArgs visibleChanged{
			Control::IsVisibleProperty(),
			BindingValue(previousValue), BindingValue(current) };
		cui::framework::EventAccess::Raise(
			element->IsVisibleChanged, element, visibleChanged);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->GetIsVisible() != current) continue;
		// Showing or hiding a whole page (TabControl parking, Visibility) visits
		// every descendant. Styles whose triggers read IsVisible were already
		// refreshed by the property notification above.
		if (!publishedThroughProperty
			|| !element->_stylePropertyConditionConnection.Connected())
			element->RefreshStyleValues(false);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->GetIsVisible() != current) continue;
		// Effective visibility changes where an element is presented, not what it
		// draws: real content changes advance the content revision even while the
		// element is hidden. Queue damage only, so retained drawing commands of a
		// page that is shown again stay reusable.
		element->InvalidateVisualRectCore(
			ToD2DRect(element->GetAbsoluteRectDip()), false);
		element = elementReference.Get();
		if (!element || element->IsDestroying()
			|| element->GetIsVisible() != current || !element->GetPresentationWindow()) continue;
		ControlWeakReference windowReference(element->GetPresentationWindow());
		auto* window = dynamic_cast<Window*>(windowReference.Get());
		if (!window || element->GetPresentationWindow() != window) continue;
		if (!current)
		{
			auto* focused = window->GetKeyboardFocusedElement();
			std::unordered_set<Control*> visited;
			for (auto* candidate = focused;
				candidate && visited.insert(candidate).second;
				candidate = candidate->GetVisualParent())
				if (candidate == element)
				{
					window->SetKeyboardFocus(
						nullptr, true, FocusChangeReason::EligibilityChanged);
					break;
				}
			element = elementReference.Get();
			window = dynamic_cast<Window*>(windowReference.Get());
			if (!element || !window || element->GetPresentationWindow() != window) continue;
			if (window->GetMouseCaptured() == element)
				(void)window->ReleaseMouseCapture(element);
			element = elementReference.Get();
			window = dynamic_cast<Window*>(windowReference.Get());
			if (!element || !window || element->GetPresentationWindow() != window) continue;
			if (element->IsMouseOver)
				cursorRefreshWindows.emplace_back(window);
		}
		element = elementReference.Get();
		window = dynamic_cast<Window*>(windowReference.Get());
		if (element && window && element->GetPresentationWindow() == window)
			window->NotifyAccessibilityEvent(
				element, AccessibilityChange::State);
	}
	for (const auto& windowReference : cursorRefreshWindows)
		if (auto* window = dynamic_cast<Window*>(windowReference.Get()))
			cui::framework::WindowAccess::UpdateCursorFromCurrentMouse(*window);
}

void Control::SetLocalEnabled(bool value)
{
	VerifyAccess();
	if (_localEnabled == value) return;
	auto snapshot = CaptureEffectiveIsEnabledSubtree(*this);
	_localEnabled = value;
	PublishEffectiveIsEnabledChanges(std::move(snapshot));
}

void Control::SetCommandCanExecuteState(bool value)
{
	VerifyAccess();
	if (_hasCommandCanExecute && _commandCanExecute == value) return;
	auto snapshot = CaptureEffectiveIsEnabledSubtree(*this);
	_hasCommandCanExecute = true;
	_commandCanExecute = value;
	PublishEffectiveIsEnabledChanges(std::move(snapshot));
}

void Control::ClearCommandCanExecuteState()
{
	VerifyAccess();
	if (!_hasCommandCanExecute) return;
	auto snapshot = CaptureEffectiveIsEnabledSubtree(*this);
	_hasCommandCanExecute = false;
	_commandCanExecute = true;
	PublishEffectiveIsEnabledChanges(std::move(snapshot));
}

bool Control::CanParticipateInTabNavigation() const
{
	static const auto& property = IsTabStopProperty();
	return GetDependencyPropertyValue<bool>(property)
		&& CanReceiveKeyboardFocus();
}

bool Control::Focus()
{
	if (!GetPresentationWindow() || !CanReceiveKeyboardFocus()) return false;
	if (GetPresentationWindow()->Handle && ::GetFocus() != GetPresentationWindow()->Handle)
		::SetFocus(GetPresentationWindow()->Handle);
	GetPresentationWindow()->SetKeyboardFocus(this, true);
	return GetPresentationWindow()->GetKeyboardFocusedElement() == this;
}

bool Control::CaptureMouse()
{
	return GetPresentationWindow() && GetPresentationWindow()->CaptureMouse(this);
}

bool Control::ReleaseMouseCapture()
{
	return GetPresentationWindow() && GetPresentationWindow()->ReleaseMouseCapture(this);
}

bool Control::IsMouseCaptured() const
{
	return _isMouseCaptured;
}

void Control::SetIsMouseCapturedCore(bool value)
{
	if (_isMouseCaptured == value) return;
	(void)SetReadOnlyPropertyField(
		IsMouseCapturedPropertyKey(),
		_isMouseCaptured, value);
}

bool Control::Invoke()
{
	return false;
}

bool Control::AreSystemAnimationsEnabled() const
{
	return !GetPresentationWindow() || GetPresentationWindow()->AreSystemAnimationsEnabled();
}

UINT Control::EffectiveAnimationDuration(UINT configuredDurationMs) const
{
	return AreSystemAnimationsEnabled() ? configuredDurationMs : 0U;
}

AccessibilitySnapshot Control::GetAccessibilitySnapshot() const
{
	AccessibilitySnapshot snapshot;
	const auto unavailableSnapshot = []
	{
		AccessibilitySnapshot unavailable;
		unavailable.Enabled = false;
		unavailable.Visible = false;
		unavailable.ReadOnly = true;
		return unavailable;
	};
	const auto* const identity = this;
	const ControlWeakReference ownerLifetime(
		const_cast<Control*>(this));
	const auto liveOwner = [&]() -> Control*
	{
		auto* owner = ownerLifetime.Get();
		return owner == identity ? owner : nullptr;
	};
	const auto performOwnerQuery = [&](auto&& query) -> bool
	{
		auto* owner = liveOwner();
		if (!owner) return false;
		query(*owner);
		return liveOwner() != nullptr;
	};
	const auto performPeerQuery = [&](auto&& query) -> bool
	{
		auto* owner = liveOwner();
		if (!owner) return false;
		auto peer = owner->AcquireAutomationPeer();
		owner = liveOwner();
		if (!peer || !owner) return false;
		auto* const peerIdentity = peer.get();
		// Every call here is exactly one extensible peer invocation. A peer can
		// synchronously remove its owner, so never batch two virtual calls under
		// one lease or touch the Control again before revalidating its identity.
		query(*peer);
		owner = liveOwner();
		if (!owner) return false;
		auto currentPeer = owner->AcquireAutomationPeer();
		owner = liveOwner();
		return owner && currentPeer
			&& currentPeer.get() == peerIdentity;
	};

	if (!performPeerQuery([&](AutomationPeer& peer)
		{ snapshot.ControlType = peer.GetAutomationControlType(); }))
		return unavailableSnapshot();

	static const auto& nameProperty = AutomationNameProperty();
	std::wstring authoredName;
	if (!performOwnerQuery([&](Control& owner)
		{
			authoredName = owner.GetDependencyPropertyValue<std::wstring>(
				nameProperty);
		})) return unavailableSnapshot();

	const bool usesAccessText = [&]
	{
		switch (snapshot.ControlType)
		{
		case AutomationControlType::Button:
		case AutomationControlType::Hyperlink:
		case AutomationControlType::CheckBox:
		case AutomationControlType::RadioButton:
		case AutomationControlType::Group:
		case AutomationControlType::MenuItem:
		case AutomationControlType::TabItem:
			return true;
		default:
			return false;
		}
	}();
	const bool editableValue =
		snapshot.ControlType == AutomationControlType::Edit
		|| snapshot.ControlType == AutomationControlType::ComboBox;
	std::wstring semanticText;
	if ((authoredName.empty() && !editableValue) || usesAccessText)
	{
		if (!performOwnerQuery([&](Control& owner)
			{ semanticText = owner.GetSemanticText(); }))
			return unavailableSnapshot();
	}
	if (!authoredName.empty()) snapshot.Name = std::move(authoredName);
	else if (!editableValue)
		snapshot.Name = usesAccessText
			? StripAccessKeyMarkers(semanticText) : semanticText;

	if (!performOwnerQuery([&](Control& owner)
		{ snapshot.Description = owner.GetEffectiveAutomationFullDescription(); }))
		return unavailableSnapshot();
	static const auto& helpTextProperty =
		AutomationHelpTextProperty();
	static const auto& automationIdProperty =
		AutomationIdProperty();
	if (!performOwnerQuery([&](Control& owner)
		{
			snapshot.HelpText = owner.GetDependencyPropertyValue<std::wstring>(
				helpTextProperty);
		})) return unavailableSnapshot();
	if (!performOwnerQuery([&](Control& owner)
		{
			snapshot.AutomationId = owner.GetDependencyPropertyValue<std::wstring>(
				automationIdProperty);
		})) return unavailableSnapshot();
	if (usesAccessText)
	{
		const wchar_t key = FindAccessKeyMarker(semanticText);
		if (key != L'\0') snapshot.KeyboardShortcut = std::wstring(L"Alt+") + key;
	}
	if (!performOwnerQuery([&](Control& owner)
		{ snapshot.Enabled = owner.IsEffectivelyEnabled(); }))
		return unavailableSnapshot();
	if (!performOwnerQuery([&](Control& owner)
		{ snapshot.Visible = owner.GetIsVisible(); }))
		return unavailableSnapshot();
	bool locallyFocusable = false;
	if (!performOwnerQuery([&](Control& owner)
		{
			locallyFocusable = owner.GetDependencyPropertyValue<bool>(
				FocusableProperty());
		})) return unavailableSnapshot();
	snapshot.Focusable = locallyFocusable
		&& snapshot.Enabled && snapshot.Visible;
	if (!performOwnerQuery([&](Control& owner)
		{ snapshot.Focused = owner._isKeyboardFocused; }))
		return unavailableSnapshot();

	bool peerSelected = false;
	bool hasPeerSelection = false;
	if (!performPeerQuery([&](AutomationPeer& peer)
		{
			hasPeerSelection = peer.TryGetSelectionItemSelected(peerSelected);
		})) return unavailableSnapshot();
	if (hasPeerSelection) snapshot.Selected = peerSelected;
	else
	{
#if CUI_ENABLE_DYNAMIC_XAML
		BindingValue selectedValue;
		bool hasSelectedValue = false;
		if (!performOwnerQuery([&](Control& owner)
			{
				hasSelectedValue = owner.TryGetValue(
					L"IsSelected", selectedValue);
			})) return unavailableSnapshot();
		if (hasSelectedValue)
			(void)selectedValue.TryGet(snapshot.Selected);
#endif
	}
	AutomationToggleState toggleState = AutomationToggleState::Off;
	bool hasPeerToggleState = false;
	if (!performPeerQuery([&](AutomationPeer& peer)
		{
			hasPeerToggleState = peer.TryGetToggleState(toggleState);
		})) return unavailableSnapshot();
	if (!hasPeerToggleState
		&& !performOwnerQuery([&](Control& owner)
			{ toggleState = owner.GetToggleStateForAccessibility(); }))
		return unavailableSnapshot();
	snapshot.Checked = toggleState == AutomationToggleState::On;
	if (!performPeerQuery([&](AutomationPeer& peer)
		{ snapshot.Password = peer.IsPassword(); }))
		return unavailableSnapshot();
	if (!performPeerQuery([&](AutomationPeer& peer)
		{ snapshot.ReadOnly = peer.IsReadOnly(); }))
		return unavailableSnapshot();
	if (!performPeerQuery([&](AutomationPeer& peer)
		{ snapshot.Value = peer.GetValue(); }))
		return unavailableSnapshot();
#if CUI_ENABLE_DYNAMIC_XAML
	if (snapshot.Value.empty() && !snapshot.Password)
	{
		BindingValue value;
		bool hasValue = false;
		if (!performOwnerQuery([&](Control& owner)
			{ hasValue = owner.TryGetValue(L"Value", value); }))
			return unavailableSnapshot();
		if (hasValue)
			snapshot.Value = value.ToString();
	}
#endif
	return snapshot;
}

EventConnection Control::SubscribeDefaultPropertyChange(
	const DependencyProperty& property,
	DependencyPropertyChangeHandler handler,
	DataSourceUpdateMode updateMode)
{
	if (updateMode == DataSourceUpdateMode::OnValidation)
		return OnLostFocus.Subscribe(
			[handler = std::move(handler)](Control*)
			{
				handler();
			});
	return DependencyObject::SubscribeDefaultPropertyChange(
		property, std::move(handler), updateMode);
}

void Control::OnBindingValidationChanged(
	const std::wstring& targetProperty)
{
	auto nextErrors = GetValidationResults();
	const bool nextHasError = std::any_of(
		nextErrors.begin(), nextErrors.end(),
		[](const BindingValidationResult& result)
		{
			return result.Issue.Severity == BindingValidationSeverity::Error;
		});
	if (_validationErrors != nextErrors)
		(void)SetReadOnlyPropertyField(
			ValidationErrorsPropertyKey(),
			_validationErrors, std::move(nextErrors));
	if (_validationHasError != nextHasError)
		(void)SetReadOnlyPropertyField(
			ValidationHasErrorPropertyKey(),
			_validationHasError, nextHasError);
	InvalidateVisual();
	if (GetPresentationWindow())
		GetPresentationWindow()->NotifyAccessibilityEvent(
			this, AccessibilityChange::Description);
	OnValidationStateChanged.Notify(targetProperty);
}

void Control::NotifyAccessibilityStructureChanged()
{
	if (GetPresentationWindow())
		GetPresentationWindow()->NotifyAccessibilityEvent(nullptr, AccessibilityChange::Structure);
}

void Control::NotifyAccessibilityStateChanged()
{
	if (GetPresentationWindow())
		GetPresentationWindow()->NotifyAccessibilityEvent(this, AccessibilityChange::State);
}

void Control::NotifyAccessibilityValueChanged()
{
	if (GetPresentationWindow())
		GetPresentationWindow()->NotifyAccessibilityEvent(this, AccessibilityChange::Value);
}

void Control::NotifyAccessibilityScrollChanged()
{
	if (GetPresentationWindow())
		GetPresentationWindow()->NotifyAccessibilityEvent(this, AccessibilityChange::Scroll);
}

void Control::NotifyAccessibilityVirtualChanged(
	uint32_t virtualId, AccessibilityChange change)
{
	if (GetPresentationWindow() && virtualId != 0)
		GetPresentationWindow()->NotifyAccessibilityVirtualEvent(this, virtualId, change);
}

const DependencyPropertyMetadataRegistration&
Control::BackgroundPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, cui::drawing::Brush> options;
		options.DefaultValue = cui::drawing::NoBrush();
		options.Flags = DependencyPropertyFlags::None;
		options.Convert = ConvertControlBrushValue;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = PropertyDesign(
			L"Appearance", 200, 10,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Text, L"Background");
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::AddOwnerStatic<
			Control, cui::drawing::Brush>(
				Panel::BackgroundProperty(), std::move(options));
	}();
	return relation;
}

const DependencyProperty& Control::BackgroundProperty()
{
	return BackgroundPropertyMetadataRelation().Property();
}

const DependencyPropertyMetadataRegistration&
Control::ForegroundPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, cui::drawing::Brush> options;
		options.DefaultValue = cui::drawing::NoBrush();
		options.Flags = DependencyPropertyFlags::Inherits
			| DependencyPropertyFlags::AffectsRender;
		options.Convert = ConvertControlBrushValue;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = PropertyDesign(
			L"Appearance", 200, 21,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Text, L"Foreground");
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::AddOwnerStatic<
			Control, cui::drawing::Brush>(
				TextElement::ForegroundProperty(), std::move(options));
	}();
	return relation;
}

const DependencyProperty& Control::ForegroundProperty()
{
	return ForegroundPropertyMetadataRelation().Property();
}

const DependencyPropertyMetadataRegistration&
Control::BorderBrushPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, cui::drawing::Brush> options;
		options.DefaultValue = cui::drawing::NoBrush();
		options.Flags = DependencyPropertyFlags::None;
		options.Convert = ConvertControlBrushValue;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = PropertyDesign(
			L"Appearance", 200, 30,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Text, L"BorderBrush");
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::AddOwnerStatic<
			Control, cui::drawing::Brush>(
				Border::BorderBrushProperty(), std::move(options));
	}();
	return relation;
}

const DependencyProperty& Control::BorderBrushProperty()
{
	return BorderBrushPropertyMetadataRelation().Property();
}

const DependencyPropertyMetadataRegistration&
Control::BorderThicknessPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, Thickness> options;
		options.DefaultValue = Thickness{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = PropertyDesign(
			L"Appearance", 200, 40,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Thickness);
		)
		return DependencyPropertyRegistry::AddOwnerStatic<Control, Thickness>(
			Border::BorderThicknessProperty(), std::move(options));
	}();
	return relation;
}

const DependencyProperty& Control::BorderThicknessProperty()
{
	return BorderThicknessPropertyMetadataRelation().Property();
}

const DependencyProperty& Control::DataContextProperty()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
			CUI_DESIGN_METADATA_ONLY(
			auto dataContextDesign = PropertyDesign(
				L"Data", 250, 0, DependencyPropertyPersistence::Native);
			dataContextDesign.Browsable = false;
			)
			DependencyPropertyOptions<Control, BindingSourceReference> dataContextOptions;
			dataContextOptions.DefaultValue = BindingSourceReference{};
			dataContextOptions.Flags = DependencyPropertyFlags::Inherits;
			dataContextOptions.Equals = [](const BindingSourceReference& left,
				const BindingSourceReference& right) { return left == right; };
			CUI_DESIGN_METADATA_ONLY(
			dataContextOptions.Design = std::move(dataContextDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, BindingSourceReference>(
				DependencyPropertyRegistrationLiteral(L"DataContext"),
				[](Control& target) { return target.GetDataContext(); },
				[](Control& target, const BindingSourceReference& value)
				{
					target.UpdateEffectiveDataContext(value);
				},
				[](Control& target, Handler handler, DataSourceUpdateMode)
				{
					return target.DataContextChanged().Subscribe(
						[handler = std::move(handler)](const PropertyChangedEventArgs&)
						{ handler(); });
				},
				std::move(dataContextOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::TemplateProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto templateDesign = PropertyDesign(
				L"Appearance", 200, 5,
				DependencyPropertyPersistence::Metadata);
			// DesignerControlPropertyCatalog owns the XAML resource picker. The DP
			// metadata remains the runtime value contract and must not add a second
			// object editor beside that structural surface.
			templateDesign.Browsable = false;
			)
			DependencyPropertyOptions<Control, ControlTemplateReference>
				templateOptions;
			templateOptions.DefaultValue = ControlTemplateReference{};
			templateOptions.Flags = DependencyPropertyFlags::AffectsMeasure;
			templateOptions.Equals = [](
				const ControlTemplateReference& left,
				const ControlTemplateReference& right)
				{
					return left == right;
				};
			templateOptions.Changed = [](
				Control& target,
				const ControlTemplateReference& oldTemplate,
				const ControlTemplateReference& newTemplate)
				{
					target.AbortControlTemplateApplication();
					target._lastTemplateError.clear();
					target.OnTemplateChanged(oldTemplate, newTemplate);
				};
			CUI_DESIGN_METADATA_ONLY(
			templateOptions.Design = std::move(templateDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
				Control, ControlTemplateReference>(
					DependencyPropertyRegistrationLiteral(L"Template"),
					std::move(templateOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::VisibilityProperty()
{
	static const auto registration = []
	{

		CUI_DESIGN_METADATA_ONLY(
			auto visibilityDesign = PropertyDesign(L"Common", 0, 30,
				DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			visibilityDesign.Choices = {
				PropertyChoice(L"Visible", std::wstring(L"Visible")),
				PropertyChoice(L"Hidden", std::wstring(L"Hidden")),
				PropertyChoice(L"Collapsed", std::wstring(L"Collapsed"))
			};
			)
			DependencyPropertyOptions<Control, std::wstring> visibilityOptions;
			visibilityOptions.DefaultValue = L"Visible";
			visibilityOptions.Flags = DependencyPropertyFlags::AffectsMeasure;
			CUI_DESIGN_METADATA_ONLY(
			visibilityOptions.Design = std::move(visibilityDesign);
			)
			visibilityOptions.Coerce = [](Control&, const std::wstring& value)
				-> std::optional<std::wstring>
				{
					if (_wcsicmp(value.c_str(), L"Visible") == 0) return L"Visible";
					if (_wcsicmp(value.c_str(), L"Hidden") == 0) return L"Hidden";
					if (_wcsicmp(value.c_str(), L"Collapsed") == 0) return L"Collapsed";
					return std::nullopt;
				};
		return DependencyPropertyRegistry::RegisterStatic<Control, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"Visibility"),
				[](Control& target)
				{
					return std::wstring(VisibilityName(target.Visibility));
				},
				[](Control& target, const std::wstring& value)
				{
					target.Visibility = _wcsicmp(value.c_str(), L"Hidden") == 0
						? ::Visibility::Hidden
						: _wcsicmp(value.c_str(), L"Collapsed") == 0
						? ::Visibility::Collapsed : ::Visibility::Visible;
				}, {}, std::move(visibilityOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::IsVisibleProperty()
{
	return IsVisiblePropertyKey().Property();
}

const DependencyPropertyKey& Control::IsVisiblePropertyKey()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
		CUI_DESIGN_METADATA_ONLY(
			auto isVisibleDesign = PropertyDesign(L"Common", 0, 31,
				DependencyPropertyPersistence::Transient,
				DependencyPropertyEditorKind::Boolean);
			)
			DependencyPropertyOptions<Control, bool> isVisibleOptions;
			isVisibleOptions.DefaultValue = true;
			CUI_DESIGN_METADATA_ONLY(
			isVisibleOptions.Design = std::move(isVisibleDesign);
			)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control, bool>(
					DependencyPropertyRegistrationLiteral(L"IsVisible"),
					[](Control& target) { return target.GetIsVisible(); },
					{},
					[](Control& target, Handler handler, DataSourceUpdateMode)
					{
						return target.IsVisibleChanged.Subscribe(
							[handler = std::move(handler)](
								DependencyObject*,
								const DependencyPropertyChangedEventArgs&)
							{ handler(); });
					}, std::move(isVisibleOptions));
	}();
	return registration.Key();
}

const DependencyProperty& Control::IsEnabledProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, bool> enabledOptions{
				true, DependencyPropertyFlags::AffectsRender };
			CUI_DESIGN_METADATA_ONLY(
			enabledOptions.Design = PropertyDesign(L"Common", 0, 20,
				DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Boolean, L"Is enabled");
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"IsEnabled"),
				[](Control& target) { return target.IsEffectivelyEnabled(); },
				[](Control& target, const bool& value)
				{ target.SetLocalEnabled(value); },
				{}, std::move(enabledOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::AllowDropProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, bool> allowDropOptions{
				false, DependencyPropertyFlags::Inherits };
			CUI_DESIGN_METADATA_ONLY(
			allowDropOptions.Design = PropertyDesign(L"Behavior", 300, 0,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Boolean, L"Allow drop");
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"AllowDrop"),
			std::move(allowDropOptions));
	}();
	return *registration;
}

namespace
{
	DependencyPropertyOptions<Control, float> ControlCanvasOffsetOptions(
				CUI_DESIGN_METADATA_ONLY(
					int order, const wchar_t* displayName))
				{
					DependencyPropertyOptions<Control, float> options;
					options.DefaultValue = cui::layout::UnsetCanvasOffset;
					options.Flags = DependencyPropertyFlags::AffectsParentArrange;
					options.Validate = [](const float& value)
						{
							return std::isfinite(value) || std::isnan(value);
						};
					options.Equals = [](const float& left, const float& right)
						{
							return left == right
								|| (std::isnan(left) && std::isnan(right));
						};
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(
						L"Layout", 100, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Number,
						displayName);
					options.Design.Step = 0.5;
					)
					return options;
				}

	DependencyPropertyOptions<Control, cui::layout::Length> ControlLengthOptions(
				CUI_DESIGN_METADATA_ONLY(int order))
				{
					DependencyPropertyOptions<Control, cui::layout::Length> options;
					options.DefaultValue = cui::layout::Length::Auto();
					options.Flags = DependencyPropertyFlags::AffectsMeasure;
					options.Convert = ConvertLayoutLength;
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(
						L"Layout", 100, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Length);
					)
					return options;
				}

	DependencyPropertyOptions<Control, float> ControlActualSizeOptions(
				CUI_DESIGN_METADATA_ONLY(int order))
				{
					DependencyPropertyOptions<Control, float> options;
					options.DefaultValue = 0.0f;
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(L"Layout", 100, order,
						DependencyPropertyPersistence::Transient,
						DependencyPropertyEditorKind::Number);
					options.Design.Browsable = false;
					)
					return options;
				}
}

#define CUI_DEFINE_CONTROL_CANVAS_PROPERTY(cppName, xamlName, designOrder) \
	const DependencyProperty& Control::cppName##Property() \
	{ \
		static const auto registration = \
			DependencyPropertyRegistry::RegisterStatic<Control, float>( \
				DependencyPropertyRegistrationLiteral(xamlName), \
				ControlCanvasOffsetOptions( \
					CUI_DESIGN_METADATA_ONLY(designOrder, xamlName))); \
		return *registration; \
	}

CUI_DEFINE_CONTROL_CANVAS_PROPERTY(CanvasLeft, L"Canvas.Left", 10)
CUI_DEFINE_CONTROL_CANVAS_PROPERTY(CanvasTop, L"Canvas.Top", 20)
CUI_DEFINE_CONTROL_CANVAS_PROPERTY(CanvasRight, L"Canvas.Right", 30)
CUI_DEFINE_CONTROL_CANVAS_PROPERTY(CanvasBottom, L"Canvas.Bottom", 40)

#undef CUI_DEFINE_CONTROL_CANVAS_PROPERTY

const DependencyProperty& Control::WidthProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<
			Control, cui::layout::Length>(
				DependencyPropertyRegistrationLiteral(L"Width"),
				ControlLengthOptions(CUI_DESIGN_METADATA_ONLY(30)));
	return *registration;
}

const DependencyProperty& Control::HeightProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<
			Control, cui::layout::Length>(
				DependencyPropertyRegistrationLiteral(L"Height"),
				ControlLengthOptions(CUI_DESIGN_METADATA_ONLY(40)));
	return *registration;
}

const DependencyProperty& Control::ActualWidthProperty()
{
	return ActualWidthPropertyKey().Property();
}

const DependencyPropertyKey& Control::ActualWidthPropertyKey()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control, float>(
			DependencyPropertyRegistrationLiteral(L"ActualWidth"),
			[](Control& target) { return target.ActualWidth; }, {},
			[](Control& target, Handler handler, DataSourceUpdateMode)
			{
				return target.SizeChanged.Subscribe(
					[handler = std::move(handler)](
						Control*, SizeChangedEventArgs&) { handler(); });
			}, ControlActualSizeOptions(CUI_DESIGN_METADATA_ONLY(50)));
	}();
	return registration.Key();
}

const DependencyProperty& Control::ActualHeightProperty()
{
	return ActualHeightPropertyKey().Property();
}

const DependencyPropertyKey& Control::ActualHeightPropertyKey()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control, float>(
			DependencyPropertyRegistrationLiteral(L"ActualHeight"),
			[](Control& target) { return target.ActualHeight; }, {},
			[](Control& target, Handler handler, DataSourceUpdateMode)
			{
				return target.SizeChanged.Subscribe(
					[handler = std::move(handler)](
						Control*, SizeChangedEventArgs&) { handler(); });
			}, ControlActualSizeOptions(CUI_DESIGN_METADATA_ONLY(60)));
	}();
	return registration.Key();
}

const DependencyProperty& Control::MarginProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, Thickness> marginOptions{
				Thickness{}, DependencyPropertyFlags::AffectsMeasure };
			CUI_DESIGN_METADATA_ONLY(
			marginOptions.Design = PropertyDesign(
				L"Layout", 100, 70, DependencyPropertyPersistence::Native);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, Thickness>(
			DependencyPropertyRegistrationLiteral(L"Margin"),
			std::move(marginOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::PaddingProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, Thickness> paddingOptions{
				Thickness{},
				DependencyPropertyFlags::AffectsMeasure
				| DependencyPropertyFlags::AffectsRender
				| DependencyPropertyFlags::AffectsParentMeasure };
			CUI_DESIGN_METADATA_ONLY(
			paddingOptions.Design = PropertyDesign(
				L"Layout", 100, 80, DependencyPropertyPersistence::Native);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, Thickness>(
			DependencyPropertyRegistrationLiteral(L"Padding"),
			std::move(paddingOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::HorizontalAlignmentProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto horizontalAlignmentDesign = PropertyDesign(
				L"Layout", 100, 90, DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			horizontalAlignmentDesign.Choices = {
				PropertyChoice(L"Left", ::HorizontalAlignment::Left),
				PropertyChoice(L"Center", ::HorizontalAlignment::Center),
				PropertyChoice(L"Right", ::HorizontalAlignment::Right),
				PropertyChoice(L"Stretch", ::HorizontalAlignment::Stretch)
			};
			)
			DependencyPropertyOptions<Control, ::HorizontalAlignment>
				horizontalAlignmentOptions{
					::HorizontalAlignment::Stretch,
					DependencyPropertyFlags::AffectsArrange };
			horizontalAlignmentOptions.Validate =
				[](const ::HorizontalAlignment& value)
				{
					switch (value)
					{
					case ::HorizontalAlignment::Left:
					case ::HorizontalAlignment::Center:
					case ::HorizontalAlignment::Right:
					case ::HorizontalAlignment::Stretch:
						return true;
					default:
						return false;
					}
				};
			CUI_DESIGN_METADATA_ONLY(
			horizontalAlignmentOptions.Design =
				std::move(horizontalAlignmentDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, ::HorizontalAlignment>(
				DependencyPropertyRegistrationLiteral(L"HorizontalAlignment"),
				std::move(horizontalAlignmentOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::VerticalAlignmentProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto verticalAlignmentDesign = PropertyDesign(
				L"Layout", 100, 100, DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			verticalAlignmentDesign.Choices = {
				PropertyChoice(L"Top", ::VerticalAlignment::Top),
				PropertyChoice(L"Center", ::VerticalAlignment::Center),
				PropertyChoice(L"Bottom", ::VerticalAlignment::Bottom),
				PropertyChoice(L"Stretch", ::VerticalAlignment::Stretch)
			};
			)
			DependencyPropertyOptions<Control, ::VerticalAlignment>
				verticalAlignmentOptions{
					::VerticalAlignment::Stretch,
					DependencyPropertyFlags::AffectsArrange };
			verticalAlignmentOptions.Validate =
				[](const ::VerticalAlignment& value)
				{
					switch (value)
					{
					case ::VerticalAlignment::Top:
					case ::VerticalAlignment::Center:
					case ::VerticalAlignment::Bottom:
					case ::VerticalAlignment::Stretch:
						return true;
					default:
						return false;
					}
				};
			CUI_DESIGN_METADATA_ONLY(
			verticalAlignmentOptions.Design = std::move(verticalAlignmentDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, ::VerticalAlignment>(
				DependencyPropertyRegistrationLiteral(L"VerticalAlignment"),
				std::move(verticalAlignmentOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::HorizontalContentAlignmentProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto horizontalContentAlignmentDesign = PropertyDesign(
				L"Layout", 100, 102, DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			horizontalContentAlignmentDesign.Choices = {
				PropertyChoice(L"Left", ::HorizontalAlignment::Left),
				PropertyChoice(L"Center", ::HorizontalAlignment::Center),
				PropertyChoice(L"Right", ::HorizontalAlignment::Right),
				PropertyChoice(L"Stretch", ::HorizontalAlignment::Stretch)
			};
			)
			DependencyPropertyOptions<Control, ::HorizontalAlignment>
				horizontalContentAlignmentOptions;
			horizontalContentAlignmentOptions.DefaultValue =
				::HorizontalAlignment::Left;
			horizontalContentAlignmentOptions.Validate =
				[](const ::HorizontalAlignment& value)
				{
					switch (value)
					{
					case ::HorizontalAlignment::Left:
					case ::HorizontalAlignment::Center:
					case ::HorizontalAlignment::Right:
					case ::HorizontalAlignment::Stretch:
						return true;
					default:
						return false;
					}
				};
			CUI_DESIGN_METADATA_ONLY(
			horizontalContentAlignmentOptions.Design =
				std::move(horizontalContentAlignmentDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, ::HorizontalAlignment>(
				DependencyPropertyRegistrationLiteral(
					L"HorizontalContentAlignment"),
				std::move(horizontalContentAlignmentOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::VerticalContentAlignmentProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto verticalContentAlignmentDesign = PropertyDesign(
				L"Layout", 100, 104, DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			verticalContentAlignmentDesign.Choices = {
				PropertyChoice(L"Top", ::VerticalAlignment::Top),
				PropertyChoice(L"Center", ::VerticalAlignment::Center),
				PropertyChoice(L"Bottom", ::VerticalAlignment::Bottom),
				PropertyChoice(L"Stretch", ::VerticalAlignment::Stretch)
			};
			)
			DependencyPropertyOptions<Control, ::VerticalAlignment>
				verticalContentAlignmentOptions;
			verticalContentAlignmentOptions.DefaultValue =
				::VerticalAlignment::Top;
			verticalContentAlignmentOptions.Validate =
				[](const ::VerticalAlignment& value)
				{
					switch (value)
					{
					case ::VerticalAlignment::Top:
					case ::VerticalAlignment::Center:
					case ::VerticalAlignment::Bottom:
					case ::VerticalAlignment::Stretch:
						return true;
					default:
						return false;
					}
				};
			CUI_DESIGN_METADATA_ONLY(
			verticalContentAlignmentOptions.Design =
				std::move(verticalContentAlignmentDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, ::VerticalAlignment>(
				DependencyPropertyRegistrationLiteral(
					L"VerticalContentAlignment"),
				std::move(verticalContentAlignmentOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::ZIndexProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, int> zIndexOptions{
				0, DependencyPropertyFlags::None };
			CUI_DESIGN_METADATA_ONLY(
			zIndexOptions.Design = PropertyDesign(L"Layout", 100, 105,
				DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Number);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, int>(
			DependencyPropertyRegistrationLiteral(L"ZIndex"),
				[](Control& target) { return target.ZIndex; },
				[](Control& target, const int& value) { target.ZIndex = value; },
				{}, std::move(zIndexOptions));
	}();
	return *registration;
}

namespace
{
	CUI_DESIGN_METADATA_ONLY(
	DependencyPropertyDesignMetadata ControlGridPlacementDesign(int order)
				{
					auto design = PropertyDesign(L"Layout", 100, order,
						DependencyPropertyPersistence::Native,
						DependencyPropertyEditorKind::Number);
					design.BrowsableWhen = [](DependencyObject& object)
						{
							auto* target = dynamic_cast<Control*>(&object);
							return target && target->GetLogicalParent()
								&& target->GetLogicalParent()->Type() == UIClass::UI_Grid;
						};
					return design;
				}
	)
	DependencyPropertyOptions<Control, int> ControlGridIndexOptions()
				{
					DependencyPropertyOptions<Control, int> options{
						0, DependencyPropertyFlags::AffectsParentMeasure };
					options.Validate = [](const int& value) { return value >= 0; };
					return options;
				}
	DependencyPropertyOptions<Control, int> ControlGridSpanOptions()
				{
					DependencyPropertyOptions<Control, int> options{
						1, DependencyPropertyFlags::AffectsParentMeasure };
					options.Validate = [](const int& value) { return value >= 1; };
					return options;
				}
}

#define CUI_DEFINE_CONTROL_GRID_PROPERTY(cppName, xamlName, optionsFactory, designOrder) \
	const DependencyProperty& Control::cppName##Property() \
	{ \
		static const auto registration = [] \
		{ \
			auto options = optionsFactory(); \
			CUI_DESIGN_METADATA_ONLY( \
			options.Design = ControlGridPlacementDesign(designOrder); \
			) \
			return DependencyPropertyRegistry::RegisterStatic<Control, int>( \
				DependencyPropertyRegistrationLiteral(xamlName), \
				std::move(options)); \
		}(); \
		return *registration; \
	}

CUI_DEFINE_CONTROL_GRID_PROPERTY(
	GridRow, L"Grid.Row", ControlGridIndexOptions, 110)
CUI_DEFINE_CONTROL_GRID_PROPERTY(
	GridColumn, L"Grid.Column", ControlGridIndexOptions, 120)
CUI_DEFINE_CONTROL_GRID_PROPERTY(
	GridRowSpan, L"Grid.RowSpan", ControlGridSpanOptions, 130)
CUI_DEFINE_CONTROL_GRID_PROPERTY(
	GridColumnSpan, L"Grid.ColumnSpan", ControlGridSpanOptions, 140)

#undef CUI_DEFINE_CONTROL_GRID_PROPERTY

const DependencyProperty& Control::DockPositionProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto dockDesign = PropertyDesign(
				L"Layout", 100, 150, DependencyPropertyPersistence::Native,
				DependencyPropertyEditorKind::Choice);
			dockDesign.Choices = {
				PropertyChoice(L"Left", Dock::Left),
				PropertyChoice(L"Top", Dock::Top),
				PropertyChoice(L"Right", Dock::Right),
				PropertyChoice(L"Bottom", Dock::Bottom)
			};
			dockDesign.DisplayName = L"Dock";
			dockDesign.BrowsableWhen = [](DependencyObject& object)
				{
					auto* target = dynamic_cast<Control*>(&object);
					return target && target->GetLogicalParent()
						&& target->GetLogicalParent()->Type() == UIClass::UI_DockPanel;
				};
			)
			DependencyPropertyOptions<Control, Dock> dockOptions{
				Dock::Left, DependencyPropertyFlags::AffectsParentMeasure };
			dockOptions.Validate = [](const Dock& value)
				{
					switch (value)
					{
					case Dock::Left:
					case Dock::Top:
					case Dock::Right:
					case Dock::Bottom:
						return true;
					default:
						return false;
					}
				};
			CUI_DESIGN_METADATA_ONLY(
			dockOptions.Design = std::move(dockDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, Dock>(
			DependencyPropertyRegistrationLiteral(L"DockPanel.Dock"),
			std::move(dockOptions));
	}();
	return *registration;
}

namespace
{
	DependencyPropertyOptions<Control, float> ControlMinimumOptions(
				CUI_DESIGN_METADATA_ONLY(int order))
				{
					DependencyPropertyOptions<Control, float> options;
					options.DefaultValue = 0.0f;
					options.Flags = DependencyPropertyFlags::AffectsMeasure;
					options.Validate = [](const float& value)
						{
							return std::isfinite(value) && value >= 0.0f;
						};
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(L"Layout", 100, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Number);
					)
					return options;
				}
	DependencyPropertyOptions<Control, float> ControlMaximumOptions(
				CUI_DESIGN_METADATA_ONLY(int order))
				{
					DependencyPropertyOptions<Control, float> options;
					options.DefaultValue = cui::core::Infinity;
					options.Flags = DependencyPropertyFlags::AffectsMeasure;
					options.Validate = [](const float& value)
						{
							return !std::isnan(value) && value >= 0.0f;
						};
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(L"Layout", 100, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Number);
					)
					return options;
				}
}

#define CUI_DEFINE_CONTROL_FLOAT_PROPERTY(cppName, xamlName, optionsFactory, designOrder) \
	const DependencyProperty& Control::cppName##Property() \
	{ \
		static const auto registration = \
			DependencyPropertyRegistry::RegisterStatic<Control, float>( \
				DependencyPropertyRegistrationLiteral(xamlName), \
				optionsFactory(CUI_DESIGN_METADATA_ONLY(designOrder))); \
		return *registration; \
	}

CUI_DEFINE_CONTROL_FLOAT_PROPERTY(
	MinWidth, L"MinWidth", ControlMinimumOptions, 160)
CUI_DEFINE_CONTROL_FLOAT_PROPERTY(
	MinHeight, L"MinHeight", ControlMinimumOptions, 170)
CUI_DEFINE_CONTROL_FLOAT_PROPERTY(
	MaxWidth, L"MaxWidth", ControlMaximumOptions, 180)
CUI_DEFINE_CONTROL_FLOAT_PROPERTY(
	MaxHeight, L"MaxHeight", ControlMaximumOptions, 190)

#undef CUI_DEFINE_CONTROL_FLOAT_PROPERTY

const DependencyPropertyMetadataRegistration&
Control::FontFamilyPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, std::wstring> fontNameOptions;
		fontNameOptions.DefaultValue = GetSystemMessageFontDefaults().Family;
		fontNameOptions.Flags = DependencyPropertyFlags::Inherits
				| DependencyPropertyFlags::AffectsMeasure
				| DependencyPropertyFlags::AffectsRender;
			fontNameOptions.Coerce = [](Control&, const std::wstring& proposed)
				-> std::optional<std::wstring>
				{
					auto first = std::find_if_not(
						proposed.begin(), proposed.end(), iswspace);
					auto last = std::find_if_not(
						proposed.rbegin(), proposed.rend(), iswspace).base();
					if (first >= last) return std::nullopt;
					return std::wstring(first, last);
				};
			CUI_DESIGN_METADATA_ONLY(
			fontNameOptions.Design = PropertyDesign(
				L"Appearance", 200, 30,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Text, L"Font name");
			)
		return DependencyPropertyRegistry::AddOwnerStatic<Control, std::wstring>(
			TextElement::FontFamilyProperty(),
				[](Control& target) { return target._fontName; },
				[](Control& target, const std::wstring& value)
				{
					target._fontName = value;
					target.ApplyTypographyFont();
				}, {}, std::move(fontNameOptions));
	}();
	return relation;
}

const DependencyProperty& Control::FontFamilyProperty()
{
	return FontFamilyPropertyMetadataRelation().Property();
}

const DependencyPropertyMetadataRegistration&
Control::LanguagePropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, std::wstring> options;
		options.DefaultValue = std::wstring(L"en-us");
		options.Flags = DependencyPropertyFlags::Inherits
			| DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsRender;
		options.Coerce = [](Control&, const std::wstring& proposed)
			-> std::optional<std::wstring>
		{
			return NormalizeRichTextLanguageTag(proposed);
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design = PropertyDesign(
			L"Appearance", 200, 35,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Text, L"Language");
		)
		return DependencyPropertyRegistry::AddOwnerStatic<Control, std::wstring>(
			TextElement::LanguageProperty(),
				[](Control& target) { return target._language; },
				[](Control& target, const std::wstring& value)
				{
					target._language = value;
				}, {}, std::move(options));
	}();
	return relation;
}

const DependencyProperty& Control::LanguageProperty()
{
	return LanguagePropertyMetadataRelation().Property();
}

const DependencyPropertyMetadataRegistration&
Control::FontSizePropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Control, double> fontSizeOptions;
		fontSizeOptions.DefaultValue = GetSystemMessageFontDefaults().Size;
		fontSizeOptions.Flags = DependencyPropertyFlags::Inherits
				| DependencyPropertyFlags::AffectsMeasure
				| DependencyPropertyFlags::AffectsRender;
			CUI_DESIGN_METADATA_ONLY(
			fontSizeOptions.Design = PropertyDesign(
				L"Appearance", 200, 40,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Number, L"Font size");
			fontSizeOptions.Design.Minimum = 1.0 / 300.0;
			fontSizeOptions.Design.Maximum = 160000.0;
			fontSizeOptions.Design.Step = 0.5;
			)
		return DependencyPropertyRegistry::AddOwnerStatic<Control, double>(
			TextElement::FontSizeProperty(),
				[](Control& target) { return target._fontSize; },
				[](Control& target, const double& value)
				{
					target._fontSize = value;
					target.ApplyTypographyFont();
				}, {}, std::move(fontSizeOptions));
	}();
	return relation;
}

const DependencyProperty& Control::FontSizeProperty()
{
	return FontSizePropertyMetadataRelation().Property();
}

const DependencyProperty& Control::OpacityProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, double> opacityOptions;
		opacityOptions.DefaultValue = 1.0;
		opacityOptions.Flags = DependencyPropertyFlags::None;
		opacityOptions.Validate = [](const double& value)
			{
				return std::isfinite(value)
					&& value >= 0.0 && value <= 1.0;
			};
		opacityOptions.Changed = [](
			Control& target,
			const double& previous,
			const double& current)
			{
				if ((previous == 1.0) != (current == 1.0)
					&& !target.HasPropertyValue(
						OpacityProperty(),
						DependencyPropertyValueSource::Animation))
					if (auto* window = target.GetPresentationWindow())
						window->InvalidatePresentationStructure();
				target.InvalidateComposition();
			};
		CUI_DESIGN_METADATA_ONLY(
		opacityOptions.Design = PropertyDesign(
			L"Appearance", 200, 26,
			DependencyPropertyPersistence::Metadata,
			DependencyPropertyEditorKind::Number,
			L"Whole-element opacity");
		opacityOptions.Design.Minimum = 0.0;
		opacityOptions.Design.Maximum = 1.0;
		opacityOptions.Design.Step = 0.05;
		)
		return DependencyPropertyRegistry::RegisterStatic<Control, double>(
			DependencyPropertyRegistrationLiteral(L"Opacity"),
			std::move(opacityOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::ClipProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, cui::drawing::Geometry> clipOptions;
			clipOptions.DefaultValue = cui::drawing::Geometry{};
			clipOptions.Flags = DependencyPropertyFlags::None;
			clipOptions.Equals = [](const cui::drawing::Geometry& left,
				const cui::drawing::Geometry& right) { return left == right; };
			CUI_DESIGN_METADATA_ONLY(
			clipOptions.Design = PropertyDesign(
				L"Appearance", 200, 27, DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Text, L"Clip geometry");
			clipOptions.Design.Browsable = false;
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, cui::drawing::Geometry>(
				DependencyPropertyRegistrationLiteral(L"Clip"),
				[](Control& target)
				{
					return target.GetClip().value_or(cui::drawing::Geometry{});
				},
				[](Control& target, const cui::drawing::Geometry& value)
				{
					if (value == cui::drawing::Geometry{}) target.ClearClip();
					else target.SetClip(value);
				}, {}, std::move(clipOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::ClipToBoundsProperty()
{
	static const auto registration = []
	{
		CUI_DESIGN_METADATA_ONLY(
			auto clipToBoundsDesign = PropertyDesign(
				L"Layout", 100, 160, DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Boolean, L"Clip to bounds");
			)
			DependencyPropertyOptions<UIElement, bool> clipToBoundsOptions;
			clipToBoundsOptions.DefaultValue = false;
			clipToBoundsOptions.Flags =
				DependencyPropertyFlags::AffectsArrange
				| DependencyPropertyFlags::AffectsRender;
			clipToBoundsOptions.Changed = [](
				UIElement& element, const bool&, const bool&)
				{
					auto* target = dynamic_cast<Control*>(&element);
					if (!target) return;
					if (auto* window = target->GetPresentationWindow())
						window->InvalidatePresentationStructure();
					// The old descendant pixels and the new clipped extent both
					// participate in damage. The layout rectangle itself is unchanged.
					target->InvalidateVisualSubtree();
					target->InvalidateVisualBoundsSubtree();
				};
			CUI_DESIGN_METADATA_ONLY(
			clipToBoundsOptions.Design = std::move(clipToBoundsDesign);
			)
		return DependencyPropertyRegistry::RegisterStatic<UIElement, bool>(
			DependencyPropertyRegistrationLiteral(L"ClipToBounds"),
			std::move(clipToBoundsOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::RenderTransformProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, cui::drawing::Transform> transformOptions;
			transformOptions.DefaultValue = cui::drawing::Transform{};
			transformOptions.Flags = DependencyPropertyFlags::None;
			transformOptions.Equals = [](const cui::drawing::Transform& left,
				const cui::drawing::Transform& right) { return left == right; };
			CUI_DESIGN_METADATA_ONLY(
			transformOptions.Design = PropertyDesign(
				L"Appearance", 200, 27, DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Text, L"Render transform");
			transformOptions.Design.Browsable = false;
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, cui::drawing::Transform>(
				DependencyPropertyRegistrationLiteral(L"RenderTransform"),
				[](Control& target)
				{
					return target.GetRenderTransform().value_or(cui::drawing::Transform{});
				},
				[](Control& target, const cui::drawing::Transform& value)
				{
					target.SetRenderTransform(value);
				}, {}, std::move(transformOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::RenderTransformOriginProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, cui::core::Point>
				transformOriginOptions{
					cui::core::Point{},
					DependencyPropertyFlags::None,
					[](Control&, const cui::core::Point& value)
					-> std::optional<cui::core::Point>
					{
						return std::isfinite(value.x) && std::isfinite(value.y)
							? std::optional<cui::core::Point>(value) : std::nullopt;
					}, {},
					[](const cui::core::Point& left, const cui::core::Point& right)
					{
						return left == right;
					} };
			CUI_DESIGN_METADATA_ONLY(
			transformOriginOptions.Design = PropertyDesign(
				L"Appearance", 200, 28,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Text, L"Transform origin");
			)
		return DependencyPropertyRegistry::RegisterStatic<
			Control, cui::core::Point>(
				DependencyPropertyRegistrationLiteral(L"RenderTransformOrigin"),
				[](Control& target) { return target.GetRenderTransformOriginDip(); },
				[](Control& target, const cui::core::Point& value)
				{ target.SetRenderTransformOriginDip(value); },
				{}, std::move(transformOriginOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::ValidationHasErrorProperty()
{
	return ValidationHasErrorPropertyKey().Property();
}

const DependencyPropertyKey& Control::ValidationHasErrorPropertyKey()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
		DependencyPropertyOptions<Control, bool> hasErrorOptions;
			hasErrorOptions.DefaultValue = false;
			CUI_DESIGN_METADATA_ONLY(
			hasErrorOptions.Design = PropertyDesign(L"Validation", 400, 10,
				DependencyPropertyPersistence::Transient,
				DependencyPropertyEditorKind::Boolean, L"Has validation error");
			hasErrorOptions.Design.Browsable = false;
			)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"Validation.HasError"),
					[](Control& target) { return target._validationHasError; },
					[](Control& target, const bool& value)
					{
						(void)target.SetReadOnlyPropertyField(
							ValidationHasErrorPropertyKey(),
							target._validationHasError, value);
					},
					[](Control& target, Handler handler, DataSourceUpdateMode)
					{
						return target.OnValidationStateChanged.Subscribe(
							[handler = std::move(handler)](
								const BindingValidationChangedEventArgs&)
							{ handler(); });
					}, std::move(hasErrorOptions));
	}();
	return registration.Key();
}

const DependencyProperty& Control::ValidationErrorsProperty()
{
	return ValidationErrorsPropertyKey().Property();
}

const DependencyPropertyKey& Control::ValidationErrorsPropertyKey()
{
	static const auto registration = []
	{
		using Handler = DependencyPropertyMetadata::ChangeHandler;
		DependencyPropertyOptions<Control,
				std::vector<BindingValidationResult>> errorsOptions;
			errorsOptions.DefaultValue = std::vector<BindingValidationResult>{};
			errorsOptions.Equals = [](
				const std::vector<BindingValidationResult>& left,
				const std::vector<BindingValidationResult>& right)
				{
					return left == right;
				};
			CUI_DESIGN_METADATA_ONLY(
			errorsOptions.Design = PropertyDesign(L"Validation", 400, 20,
				DependencyPropertyPersistence::Transient,
				DependencyPropertyEditorKind::Text, L"Validation errors");
			errorsOptions.Design.Browsable = false;
			)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control,
			std::vector<BindingValidationResult>>(
					DependencyPropertyRegistrationLiteral(L"Validation.Errors"),
					[](Control& target) { return target._validationErrors; },
					[](Control& target,
						const std::vector<BindingValidationResult>& value)
					{
						(void)target.SetReadOnlyPropertyField(
							ValidationErrorsPropertyKey(),
							target._validationErrors, value);
					},
					[](Control& target, Handler handler, DataSourceUpdateMode)
					{
						return target.OnValidationStateChanged.Subscribe(
							[handler = std::move(handler)](
								const BindingValidationChangedEventArgs&)
							{ handler(); });
					}, std::move(errorsOptions));
	}();
	return registration.Key();
}

const DependencyProperty& Control::TagProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, BindingValue> tagOptions;
			tagOptions.DefaultValue = BindingValue{};
			tagOptions.Flags = DependencyPropertyFlags::None;
			CUI_DESIGN_METADATA_ONLY(
			tagOptions.Design = PropertyDesign(
				L"Data", 250, 20,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Text);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, BindingValue>(
			DependencyPropertyRegistrationLiteral(L"Tag"),
			std::move(tagOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::CursorProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, CursorKind> cursorOptions;
			cursorOptions.DefaultValue = CursorKind::Auto;
			cursorOptions.Flags = DependencyPropertyFlags::Inherits;
			cursorOptions.Validate = [](const CursorKind& proposed)
				{
					switch (proposed)
					{
					case CursorKind::Auto:
					case CursorKind::Arrow:
					case CursorKind::Cross:
					case CursorKind::Hand:
					case CursorKind::IBeam:
					case CursorKind::SizeWE:
					case CursorKind::SizeNS:
					case CursorKind::SizeNWSE:
					case CursorKind::SizeNESW:
					case CursorKind::SizeAll:
					case CursorKind::No:
						return true;
					}
					return false;
				};
			CUI_DESIGN_METADATA_ONLY(
			cursorOptions.Design = PropertyDesign(
				L"Behavior", 300, 10,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Choice);
			cursorOptions.Design.Choices = {
				PropertyChoice(L"Auto", CursorKind::Auto),
				PropertyChoice(L"Arrow", CursorKind::Arrow),
				PropertyChoice(L"Cross", CursorKind::Cross),
				PropertyChoice(L"Hand", CursorKind::Hand),
				PropertyChoice(L"IBeam", CursorKind::IBeam),
				PropertyChoice(L"SizeWE", CursorKind::SizeWE),
				PropertyChoice(L"SizeNS", CursorKind::SizeNS),
				PropertyChoice(L"SizeNWSE", CursorKind::SizeNWSE),
				PropertyChoice(L"SizeNESW", CursorKind::SizeNESW),
				PropertyChoice(L"SizeAll", CursorKind::SizeAll),
				PropertyChoice(L"No", CursorKind::No)
			};
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, CursorKind>(
			DependencyPropertyRegistrationLiteral(L"Cursor"),
			std::move(cursorOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::FocusableProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, bool> focusableOptions;
			focusableOptions.DefaultValue = false;
			focusableOptions.Changed = [](
				Control& target, const bool&, const bool& focusable)
				{
					auto* window = target.GetPresentationWindow();
					if (!focusable && window
						&& window->GetKeyboardFocusedElement() == &target)
						window->SetKeyboardFocus(
							nullptr, true,
							FocusChangeReason::EligibilityChanged);
					if (window)
						window->NotifyAccessibilityEvent(
							&target, AccessibilityChange::State);
				};
			CUI_DESIGN_METADATA_ONLY(
			focusableOptions.Design = PropertyDesign(
				L"Focus", 310, 0,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Boolean);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"Focusable"),
			std::move(focusableOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::IsTabStopProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, bool> isTabStopOptions;
			isTabStopOptions.DefaultValue = true;
			isTabStopOptions.Changed = [](
				Control& target, const bool&, const bool&)
				{
					if (auto* window = target.GetPresentationWindow())
						window->NotifyAccessibilityEvent(
							&target, AccessibilityChange::State);
				};
			CUI_DESIGN_METADATA_ONLY(
			isTabStopOptions.Design = PropertyDesign(
				L"Behavior", 300, 20,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Boolean);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"IsTabStop"),
			std::move(isTabStopOptions));
	}();
	return *registration;
}

const DependencyProperty& Control::TabIndexProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, int> tabIndexOptions;
			tabIndexOptions.DefaultValue = 0;
			tabIndexOptions.Coerce = [](
				Control&, const int& proposed) -> std::optional<int>
				{
					return (std::max)(0, proposed);
				};
			tabIndexOptions.Changed = [](
				Control& target, const int&, const int&)
				{
					if (auto* window = target.GetPresentationWindow())
						window->NotifyAccessibilityEvent(
							nullptr, AccessibilityChange::Structure);
				};
			CUI_DESIGN_METADATA_ONLY(
			tabIndexOptions.Design = PropertyDesign(
				L"Behavior", 300, 30,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Number);
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, int>(
			DependencyPropertyRegistrationLiteral(L"TabIndex"),
			std::move(tabIndexOptions));
	}();
	return *registration;
}

namespace
{
	DependencyPropertyOptions<Control, bool> ControlFocusStateOptions(
		DependencyPropertyFlags flags
		CUI_DESIGN_METADATA_ARGUMENTS(int order))
				{
					DependencyPropertyOptions<Control, bool> options;
					options.DefaultValue = false;
					options.Flags = flags;
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(L"State", 70, order,
						DependencyPropertyPersistence::Transient,
						DependencyPropertyEditorKind::Boolean);
					options.Design.Browsable = false;
					)
					return options;
				}
}

#define CUI_DEFINE_CONTROL_STATE_PROPERTY( \
	cppName, xamlName, fieldName, getterExpression, flags, designOrder) \
	const DependencyProperty& Control::cppName##Property() \
	{ \
		return cppName##PropertyKey().Property(); \
	} \
	const DependencyPropertyKey& Control::cppName##PropertyKey() \
	{ \
		static const auto registration = [] \
		{ \
			return DependencyPropertyRegistry::RegisterReadOnlyStatic<Control, bool>( \
				DependencyPropertyRegistrationLiteral(xamlName), \
				[](Control& target) { return getterExpression; }, \
				[](Control& target, const bool& value) \
				{ \
					(void)target.SetReadOnlyPropertyField( \
						cppName##PropertyKey(), target.fieldName, value); \
				}, {}, ControlFocusStateOptions( \
					flags CUI_DESIGN_METADATA_ARGUMENTS(designOrder))); \
		}(); \
		return registration.Key(); \
	}

CUI_DEFINE_CONTROL_STATE_PROPERTY(IsFocused, L"IsFocused", _isFocused,
	target.IsFocused, DependencyPropertyFlags::AffectsRender, 10)
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsKeyboardFocused, L"IsKeyboardFocused", _isKeyboardFocused,
	target.IsKeyboardFocused, DependencyPropertyFlags::AffectsRender, 20)
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsKeyboardFocusVisible, L"IsKeyboardFocusVisible", _isKeyboardFocusVisible,
	target.IsKeyboardFocusVisible, DependencyPropertyFlags::AffectsRender, 25)
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsKeyboardFocusWithin, L"IsKeyboardFocusWithin", _isKeyboardFocusWithin,
	target.IsKeyboardFocusWithin, DependencyPropertyFlags::None, 30)
CUI_DEFINE_CONTROL_STATE_PROPERTY(IsMouseOver, L"IsMouseOver", _isMouseOver,
	target.IsMouseOver, DependencyPropertyFlags::None, 40)
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsMouseDirectlyOver, L"IsMouseDirectlyOver", _isMouseDirectlyOver,
	target.IsMouseDirectlyOver, DependencyPropertyFlags::None, 50)
// WPF's direct capture property carries no render/layout flags.
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsMouseCaptured, L"IsMouseCaptured", _isMouseCaptured,
	target.IsMouseCaptured(), DependencyPropertyFlags::None, 60)
// WPF's reverse-inherited capture property is state-only metadata.
CUI_DEFINE_CONTROL_STATE_PROPERTY(
	IsMouseCaptureWithin, L"IsMouseCaptureWithin", _isMouseCaptureWithin,
	target.IsMouseCaptureWithin, DependencyPropertyFlags::None, 70)

#undef CUI_DEFINE_CONTROL_STATE_PROPERTY

const DependencyProperty& Control::IsFocusScopeProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Control, bool> focusScopeOptions;
			focusScopeOptions.DefaultValue = false;
			focusScopeOptions.Changed = [](
				Control& target, const bool&, const bool&)
				{
					if (auto* window = target.GetPresentationWindow())
						window->NotifyAccessibilityEvent(
							nullptr, AccessibilityChange::Structure);
				};
			CUI_DESIGN_METADATA_ONLY(
			focusScopeOptions.Design = PropertyDesign(
				L"Focus", 310, 10,
				DependencyPropertyPersistence::Metadata,
				DependencyPropertyEditorKind::Boolean,
				L"IsFocusScope");
			)
		return DependencyPropertyRegistry::RegisterStatic<Control, bool>(
			DependencyPropertyRegistrationLiteral(L"FocusManager.IsFocusScope"),
			std::move(focusScopeOptions));
	}();
	return *registration;
}

namespace
{
	DependencyPropertyOptions<Control, KeyboardNavigationMode>
		ControlNavigationOptions(
				CUI_DESIGN_METADATA_ONLY(
					int order, std::wstring displayName))
				{
					DependencyPropertyOptions<Control, KeyboardNavigationMode> options;
					options.DefaultValue = KeyboardNavigationMode::Continue;
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(L"Focus", 310, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Choice,
						std::move(displayName));
					options.Design.Choices = {
						{ L"Continue", BindingValue(KeyboardNavigationMode::Continue) },
						{ L"Once", BindingValue(KeyboardNavigationMode::Once) },
						{ L"Cycle", BindingValue(KeyboardNavigationMode::Cycle) },
						{ L"None", BindingValue(KeyboardNavigationMode::None) },
						{ L"Contained", BindingValue(KeyboardNavigationMode::Contained) },
						{ L"Local", BindingValue(KeyboardNavigationMode::Local) },
					};
					)
					return options;
				}

	DependencyPropertyOptions<Control, std::wstring> ControlAutomationOptions(
		CUI_DESIGN_METADATA_ONLY(int order))
				{
					DependencyPropertyOptions<Control, std::wstring> options;
					options.DefaultValue = std::wstring{};
					CUI_DESIGN_METADATA_ONLY(
					options.Design = PropertyDesign(
						L"Automation", 500, order,
						DependencyPropertyPersistence::Metadata,
						DependencyPropertyEditorKind::Text);
					)
					return options;
				}
}

const DependencyProperty& Control::TabNavigationProperty()
{
	static const auto registration = []
	{
		auto options = ControlNavigationOptions(CUI_DESIGN_METADATA_ONLY(
			20, L"TabNavigation"));
		options.Changed = [](Control& target,
			const KeyboardNavigationMode&, const KeyboardNavigationMode&)
		{
			if (auto* window = target.GetPresentationWindow())
				window->NotifyAccessibilityEvent(
					nullptr, AccessibilityChange::Structure);
		};
		return DependencyPropertyRegistry::RegisterStatic<
			Control, KeyboardNavigationMode>(
				DependencyPropertyRegistrationLiteral(
					L"KeyboardNavigation.TabNavigation"),
				std::move(options));
	}();
	return *registration;
}

const DependencyProperty& Control::DirectionalNavigationProperty()
{
	static const auto registration = []
	{
		auto options = ControlNavigationOptions(CUI_DESIGN_METADATA_ONLY(
			30, L"DirectionalNavigation"));
		options.Changed = [](Control& target,
			const KeyboardNavigationMode&, const KeyboardNavigationMode&)
		{
			if (auto* window = target.GetPresentationWindow())
				window->NotifyAccessibilityEvent(
					nullptr, AccessibilityChange::Structure);
		};
		return DependencyPropertyRegistry::RegisterStatic<
			Control, KeyboardNavigationMode>(
				DependencyPropertyRegistrationLiteral(
					L"KeyboardNavigation.DirectionalNavigation"),
				std::move(options));
	}();
	return *registration;
}

#define CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY( \
	cppName, xamlName, accessibilityChange, designOrder) \
	const DependencyProperty& Control::cppName##Property() \
	{ \
		static const auto registration = [] \
		{ \
			auto options = ControlAutomationOptions( \
				CUI_DESIGN_METADATA_ONLY(designOrder)); \
			options.Changed = [](Control& target, const std::wstring&, \
				const std::wstring&) \
			{ \
				if (auto* window = target.GetPresentationWindow()) \
					window->NotifyAccessibilityEvent( \
						&target, accessibilityChange); \
			}; \
			return DependencyPropertyRegistry::RegisterStatic< \
				Control, std::wstring>( \
				DependencyPropertyRegistrationLiteral(xamlName), \
				std::move(options)); \
		}(); \
		return *registration; \
	}

CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY(
	AutomationName, L"AutomationProperties.Name", AccessibilityChange::Name, 20)
CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY(
	AutomationFullDescription, L"AutomationProperties.FullDescription",
	AccessibilityChange::Description, 30)
CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY(
	AutomationHelpText, L"AutomationProperties.HelpText",
	AccessibilityChange::Help, 40)
CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY(
	AutomationId, L"AutomationProperties.AutomationId",
	AccessibilityChange::Structure, 50)

#undef CUI_DEFINE_CONTROL_AUTOMATION_PROPERTY

#if !CUI_ENABLE_DYNAMIC_XAML
void Control::RegisterDependencyProperties()
{
}
#endif

Control* Control::GetVisualChild(int index) const noexcept
{
	if (index < 0 || static_cast<size_t>(index) >= _visualChildren.size())
		return nullptr;
	return _visualChildren[static_cast<size_t>(index)];
}

std::vector<Control*> Control::GetVisualChildrenInZOrder() const
{
	std::vector<Control*> result(
		this->_visualChildren.begin(), this->_visualChildren.end());
	std::stable_sort(result.begin(), result.end(), [](Control* left, Control* right)
		{
			if (!left || !right) return left != nullptr;
			return left->ZIndex < right->ZIndex;
		});
	return result;
}

std::vector<Control*> Control::GetVisualChildrenInReverseZOrder() const
{
	auto result = GetVisualChildrenInZOrder();
	std::reverse(result.begin(), result.end());
	return result;
}

bool Control::MoveVisualChild(int oldIndex, int newIndex)
{
	if (oldIndex < 0 || newIndex < 0)
		return false;
	const auto oldPosition = static_cast<size_t>(oldIndex);
	const auto newPosition = static_cast<size_t>(newIndex);
	if (oldPosition >= _visualChildren.size()
		|| newPosition >= _visualChildren.size())
		return false;
	return _visualChildren.Move(oldPosition, newPosition);
}

std::unique_ptr<Control> Control::DetachVisualChildCore(
	Control* child,
	bool* visualOwnershipCommit,
	std::exception_ptr* notificationError)
{
	if (visualOwnershipCommit) *visualOwnershipCommit = false;
	if (notificationError) *notificationError = {};
	if (!child)
		return {};
	auto position = std::find(
		this->_visualChildren.begin(), this->_visualChildren.end(), child);
	if (position == this->_visualChildren.end())
		return {};

	const ControlWeakReference lifetime(child);
	bool ownershipCommittedElsewhere = false;
	VisualOwnershipCommitScope observation(
		child, &ownershipCommittedElsewhere);
	std::exception_ptr postCommitError;
	try
	{
		this->_visualChildren.erase(position);
	}
	catch (...)
	{
		const auto error = std::current_exception();
		auto* live = lifetime.Get();
		if (live && this->IndexOfVisualChild(live) >= 0)
		{
			if (visualOwnershipCommit)
				*visualOwnershipCommit =
					ownershipCommittedElsewhere;
			throw;
		}
		// The raw store already removed the child. From this point ownership
		// must still be returned (or respected as transferred by a callback);
		// an observer exception cannot be allowed to strand the object.
		postCommitError = error;
	}

	auto* live = lifetime.Get();
	if (live && !ownershipCommittedElsewhere
		&& !live->GetVisualParent()
		&& this->IndexOfVisualChild(live) < 0)
	{
		// A VisualParent observer may fail before the synchronizer publishes
		// the logical edge and presentation-host cleanup. Finish those
		// structural invariants before issuing an owner token.
		if (live->GetLogicalParent() == this)
		{
			try { live->SetLogicalParentCore(nullptr); }
			catch (...)
			{
				if (!postCommitError)
					postCommitError = std::current_exception();
			}
		}
		live = lifetime.Get();
		if (live && !ownershipCommittedElsewhere
			&& !live->GetVisualParent()
			&& this->IndexOfVisualChild(live) < 0)
		{
			live->_isWindowRoot = false;
			try { PropagatePresentationWindow(live, nullptr); }
			catch (...)
			{
				if (!postCommitError)
					postCommitError = std::current_exception();
			}
		}
	}

	if (visualOwnershipCommit)
		*visualOwnershipCommit = ownershipCommittedElsewhere;
	if (notificationError)
		*notificationError = postCommitError;
	live = lifetime.Get();
	if (!live || ownershipCommittedElsewhere
		|| live->GetVisualParent()
		|| this->IndexOfVisualChild(live) >= 0)
		return {};
	// Issue of a unique_ptr is itself an ownership transition. Publish it to
	// any enclosing attach/configuration transaction before the token escapes.
	PublishVisualOwnershipCommit(live);
	return std::unique_ptr<Control>(live);
}

std::unique_ptr<Control> Control::DetachVisualChild(Control* child)
{
	std::exception_ptr ignoredNotificationError;
	return DetachVisualChildCore(
		child, nullptr, &ignoredNotificationError);
}

std::unique_ptr<Control> Control::DetachVisualChildAt(int index)
{
	if (index < 0 || static_cast<size_t>(index) >= _visualChildren.size())
		return {};
	return DetachVisualChild(_visualChildren[static_cast<size_t>(index)]);
}

bool Control::DeleteVisualChild(Control* child)
{
	auto detached = DetachVisualChild(child);
	return detached != nullptr;
}

bool Control::DeleteVisualChildAt(int index)
{
	auto detached = DetachVisualChildAt(index);
	return detached != nullptr;
}

void Control::ClearVisualChildren()
{
	if (_visualChildren.empty()) return;
	std::vector<Control*> removed(
		_visualChildren.begin(), _visualChildren.end());
	std::vector<ControlWeakReference> lifetimes;
	lifetimes.reserve(removed.size());
	for (auto* child : removed)
		lifetimes.emplace_back(child);
	VisualOwnershipCommitBatchScope ownershipObservation(
		std::span<Control* const>{
			removed.data(), removed.size() });

	std::exception_ptr notificationError;
	try
	{
		_visualChildren.clear();
	}
	catch (...)
	{
		notificationError = std::current_exception();
	}

	for (size_t index = 0; index < lifetimes.size(); ++index)
	{
		auto* child = lifetimes[index].Get();
		if (!child || ownershipObservation.Committed(index)
			|| child->GetVisualParent()
			|| IndexOfVisualChild(child) >= 0)
			continue;
		delete child;
	}
	if (notificationError)
		std::rethrow_exception(notificationError);
}

int Control::IndexOfVisualChild(const Control* child) const noexcept
{
	if (!child) return -1;
	auto found = std::find(
		_visualChildren.begin(), _visualChildren.end(), child);
	return found == _visualChildren.end()
		? -1 : static_cast<int>(found - _visualChildren.begin());
}
GET_CPP(Control, bool, IsEnabled)
{
	return IsEffectivelyEnabled();
}
SET_CPP(Control, bool, IsEnabled)
{
	// The CLR-shaped wrapper is SetValue(IsEnabledProperty), not a second
	// native state mutation path. Command and ancestor constraints are folded
	// into the effective projection separately.
	static const auto& property = IsEnabledProperty();
	(void)TrySetPropertyValue(property, BindingValue(value));
}
GET_CPP(Control, bool, AllowDrop)
{
	static const auto& property = AllowDropProperty();
	return GetDependencyPropertyValue<bool>(property);
}
SET_CPP(Control, bool, AllowDrop)
{
	static const auto& property = AllowDropProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, ::Visibility, Visibility)
{
	return _visibility;
}
SET_CPP(Control, ::Visibility, Visibility)
{
	VerifyAccess();
	if (value != ::Visibility::Visible
		&& value != ::Visibility::Hidden
		&& value != ::Visibility::Collapsed)
		throw std::invalid_argument("Visibility value is invalid");
	static const auto& property = VisibilityProperty();
	auto* metadata = GetPropertyMetadata(property);
	if (!metadata) return;
	if (_applyingPropertyMetadata != metadata)
	{
		(void)TrySetPropertyValue(
			property,
			BindingValue(std::wstring(VisibilityName(value))));
		return;
	}
	if (_visibility == value) return;
	const ControlWeakReference lifetime(this);
	auto snapshot = CaptureEffectiveIsVisibleSubtree(*this);
	const bool collapsedChanged = (_visibility == ::Visibility::Collapsed)
		!= (value == ::Visibility::Collapsed);
	_visibility = value;
	if (collapsedChanged) RequestLayout();
	else InvalidateVisual();
	PublishEffectiveIsVisibleChanges(std::move(snapshot));

	auto* self = dynamic_cast<Control*>(lifetime.Get());
	if (!self) return;
	if (auto* window = self->GetPresentationWindow())
	{
		window->InvalidatePresentationStructure();
		window->Invalidate(false);
		window->NotifyAccessibilityEvent(
			self, AccessibilityChange::State);
	}
}

void Control::SetPresentationSuppressed(bool value)
{
	VerifyAccess();
	if (_presentationSuppressed == value) return;
	const ControlWeakReference lifetime(this);
	auto snapshot = CaptureEffectiveIsVisibleSubtree(*this);
	_presentationSuppressed = value;
	if (PresentationSuppressionAffectsLayout())
		RequestLayout();
	PublishEffectiveIsVisibleChanges(std::move(snapshot));
	auto* self = dynamic_cast<Control*>(lifetime.Get());
	if (!self) return;
	if (auto* window = self->GetPresentationWindow())
	{
		window->InvalidatePresentationStructure();
		window->Invalidate(false);
		window->NotifyAccessibilityEvent(
			self, AccessibilityChange::State);
	}
}

void Control::SetParticipatesInPresentationScene(bool value)
{
	VerifyAccess();
	if (_participatesInPresentationScene == value) return;
	_participatesInPresentationScene = value;
	if (GetPresentationWindow())
	{
		GetPresentationWindow()->InvalidatePresentationStructure();
		GetPresentationWindow()->Invalidate(false);
	}
}

GET_CPP(Visual, int, ZIndex)
{
	return _zIndex;
}
SET_CPP(Visual, int, ZIndex)
{
	if (auto* control = dynamic_cast<Control*>(this);
		control && control->RouteVisualZIndexSet(value))
		return;
	if (_zIndex == value) return;
	_zIndex = value;
	if (GetPresentationWindow())
	{
		GetPresentationWindow()->InvalidatePresentationStructure();
		GetPresentationWindow()->Invalidate(false);
	}
}

bool Control::RouteVisualZIndexSet(int value)
{
	auto* metadata = GetPropertyMetadata(ZIndexProperty());
	if (!metadata || _applyingPropertyMetadata == metadata) return false;
	// The Visual backing field is written only by the metadata application
	// re-entry above. A failed conversion/coercion must not fall through and
	// mutate it outside the dependency-property store.
	(void)TrySetPropertyValue(
		ZIndexProperty(), BindingValue(value),
		DependencyPropertyValueSource::Local);
	return true;
}

GET_CPP(Control, float, CanvasLeft)
{
	static const auto& property =
		CanvasLeftProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, CanvasLeft)
{
	static const auto& property =
		CanvasLeftProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, CanvasTop)
{
	static const auto& property =
		CanvasTopProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, CanvasTop)
{
	static const auto& property =
		CanvasTopProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, CanvasRight)
{
	static const auto& property =
		CanvasRightProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, CanvasRight)
{
	static const auto& property =
		CanvasRightProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, CanvasBottom)
{
	static const auto& property =
		CanvasBottomProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, CanvasBottom)
{
	static const auto& property =
		CanvasBottomProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, cui::layout::Length, Width)
{
	static const auto& property = WidthProperty();
	return GetDependencyPropertyValue<cui::layout::Length>(property);
}
SET_CPP(Control, cui::layout::Length, Width)
{
	static const auto& property = WidthProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, cui::layout::Length, Height)
{
	static const auto& property = HeightProperty();
	return GetDependencyPropertyValue<cui::layout::Length>(property);
}
SET_CPP(Control, cui::layout::Length, Height)
{
	static const auto& property = HeightProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, MinWidth)
{
	static const auto& property = MinWidthProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, MinWidth)
{
	static const auto& property = MinWidthProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, MinHeight)
{
	static const auto& property = MinHeightProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, MinHeight)
{
	static const auto& property = MinHeightProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, MaxWidth)
{
	static const auto& property = MaxWidthProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, MaxWidth)
{
	static const auto& property = MaxWidthProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, MaxHeight)
{
	static const auto& property = MaxHeightProperty();
	return GetDependencyPropertyValue<float>(property);
}
SET_CPP(Control, float, MaxHeight)
{
	static const auto& property = MaxHeightProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, float, ActualWidth)
{
	return GetActualSizeDip().width;
}
GET_CPP(Control, float, ActualHeight)
{
	return GetActualSizeDip().height;
}
GET_CPP(Control, D2D1_COLOR_F, RendererBorderColor)
{
	const auto brush = GetComputedBorderBrush();
	if (brush.Kind == cui::drawing::BrushKind::Solid)
	{
		auto color = brush.Color;
		color.a *= (std::clamp)(brush.Opacity, 0.0f, 1.0f);
		return color;
	}
	return _bordercolor;
}
SET_CPP(Control, D2D1_COLOR_F, RendererBorderColor)
{
	if (_bordercolor.r == value.r && _bordercolor.g == value.g
		&& _bordercolor.b == value.b && _bordercolor.a == value.a) return;
	_bordercolor = value;
	InvalidateVisual();
}
GET_CPP(Control, Thickness, BorderThickness)
{
	static const auto& property =
		BorderThicknessProperty();
	BindingValue value;
	Thickness thickness{};
	if (TryGetPropertyValue(property, value))
		(void)value.TryGet(thickness);
	return thickness;
}
SET_CPP(Control, Thickness, BorderThickness)
{
	static const auto& property =
		BorderThicknessProperty();
	(void)SetDependencyPropertyValue(property, value);
}
GET_CPP(Control, D2D1_COLOR_F, RendererBackgroundColor)
{
	const auto brush = GetComputedBackgroundBrush();
	if (brush.Kind == cui::drawing::BrushKind::Solid)
	{
		auto color = brush.Color;
		color.a *= (std::clamp)(brush.Opacity, 0.0f, 1.0f);
		return color;
	}
	return GetPresentationWindow()
		? cui::framework::WindowAccess::EffectiveControlBackColor(
			*GetPresentationWindow(), _backcolor) : _backcolor;
}
SET_CPP(Control, D2D1_COLOR_F, RendererBackgroundColor)
{
	if (_backcolor.r == value.r && _backcolor.g == value.g
		&& _backcolor.b == value.b && _backcolor.a == value.a) return;
	_backcolor = value;
	InvalidateVisual();
}
GET_CPP(Control, D2D1_COLOR_F, RendererForegroundColor)
{
	const auto brush = GetComputedForegroundBrush();
	if (brush.Kind == cui::drawing::BrushKind::Solid)
	{
		auto color = brush.Color;
		color.a *= (std::clamp)(brush.Opacity, 0.0f, 1.0f);
		return color;
	}
	return GetPresentationWindow()
		? cui::framework::WindowAccess::EffectiveControlForeColor(
			*GetPresentationWindow(), _forecolor) : _forecolor;
}
SET_CPP(Control, D2D1_COLOR_F, RendererForegroundColor)
{
	if (_forecolor.r == value.r && _forecolor.g == value.g
		&& _forecolor.b == value.b && _forecolor.a == value.a) return;
	_forecolor = value;
	InvalidateVisual();
}
bool Control::DispatchInput(const InputReport& input)
{
#if CUI_ENABLE_DYNAMIC_XAML
	const ControlWeakReference selfReference(this);
	const bool previousDispatch = _dispatchingComponentBehaviorInput;
	_dispatchingComponentBehaviorInput = true;
	if (!previousDispatch && _declarativeComponentBehavior)
	{
		bool handled = false;
		try
		{
			handled = _declarativeComponentBehavior->HandleInput(*this, input);
		}
		catch (...)
		{
		}
		auto* live = selfReference.Get();
		if (!live) return true;
		if (handled)
		{
			live->_dispatchingComponentBehaviorInput = previousDispatch;
			return true;
		}
	}
	try
	{
		auto* live = selfReference.Get();
		if (!live) return true;
		const bool result = live->ProcessInput(input);
		live = selfReference.Get();
		if (live)
			live->_dispatchingComponentBehaviorInput = previousDispatch;
		return result;
	}
	catch (...)
	{
		if (auto* live = selfReference.Get())
			live->_dispatchingComponentBehaviorInput = previousDispatch;
		throw;
	}
#else
	return ProcessInput(input);
#endif
}

bool Control::DispatchTextInput(TextCompositionEventArgs& input)
{
	const ControlWeakReference selfReference(this);
	if (!IsEffectivelyEnabled() || !this->IsVisible || input.Text.empty()) return false;
#if CUI_ENABLE_DYNAMIC_XAML
	if (_declarativeComponentBehavior)
	{
		try
		{
			if (_declarativeComponentBehavior->HandleTextInput(*this, input))
				return true;
		}
		catch (...)
		{
		}
	}
#endif
	auto* live = selfReference.Get();
	return live ? live->ApplyTextInput(input) : true;
}

bool Control::ResolveTextInputCaretRect(D2D1_RECT_F& outRect)
{
	const ControlWeakReference selfReference(this);
#if CUI_ENABLE_DYNAMIC_XAML
	if (_declarativeComponentBehavior)
	{
		try
		{
			if (_declarativeComponentBehavior->TryGetTextInputCaretRect(
				*this, outRect)) return true;
		}
		catch (...)
		{
		}
	}
#endif
	auto* live = selfReference.Get();
	return live && live->TryGetTextInputCaretRect(outRect);
}

bool Control::ProcessInput(const InputReport& input)
{
	const ControlWeakReference sourceReference(this);
#if CUI_ENABLE_DYNAMIC_XAML
	if (!_dispatchingComponentBehaviorInput
		&& _declarativeComponentBehavior)
		return DispatchInput(input);
#endif
	if (!IsEffectivelyEnabled() || !this->IsVisible) return true;
	auto resolve = [&]() noexcept { return sourceReference.Get(); };
	switch (input.Kind)
	{
	case InputReportKind::MouseWheel:
	case InputReportKind::HorizontalMouseWheel:
	{
		auto eventArgs = input.CreateMouseEventArgs();
		if (auto* source = resolve())
			source->OnMouseWheel(source, eventArgs);
	}
	break;
	case InputReportKind::PointerMove:
	{
		auto eventArgs = input.CreateMouseEventArgs();
		auto* source = resolve();
		if (!source) return true;
		source->BeforeDefaultMouseMove(eventArgs);
		source = resolve();
		if (!source) return true;
		source->OnMouseMove(source, eventArgs);
	}
	break;
	case InputReportKind::PointerDown:
	{
		auto* source = resolve();
		if (!source) return true;
		if (input.ChangedButton == MouseButton::Left
			&& source->DefaultRaiseClickOnLeftButtonUp())
		{
			source->_defaultLeftButtonPressActive = true;
			(void)source->CaptureMouse();
		}
		source = resolve();
		if (!source) return true;
		const bool selectOnDown = input.ChangedButton == MouseButton::Left
			&& source->DefaultSelectOnLeftButtonDown();
		source = resolve();
		if (!source) return true;
		if (selectOnDown && source->GetPresentationWindow())
		{
			const ControlWeakReference windowReference(source->GetPresentationWindow());
			if (auto* window = dynamic_cast<Window*>(windowReference.Get()))
				window->SetKeyboardFocus(source, false);
		}
		auto eventArgs = input.CreateMouseEventArgs();
		source = resolve();
		if (!source) return true;
		source->BeforeDefaultMouseDown(input.ChangedButton, eventArgs);
		source = resolve();
		if (!source) return true;
		source->OnMouseDown(source, eventArgs);
		source = resolve();
		if (!source) return true;
		const bool invalidate = source->DefaultInvalidateVisualOnMouseDown(
			input.ChangedButton);
		source = resolve();
		if (source && invalidate) source->InvalidateVisual();
	}
	break;
	case InputReportKind::PointerUp:
	{
		auto* source = resolve();
		if (!source) return true;
		const bool hasMatchingPress = input.ChangedButton != MouseButton::Left
			|| source->_defaultLeftButtonPressActive;
		if (input.ChangedButton == MouseButton::Left)
			source->_defaultLeftButtonPressActive = false;
		auto eventArgs = input.CreateMouseEventArgs();
		source->BeforeDefaultMouseUp(
			input.ChangedButton, eventArgs, hasMatchingPress);
		source = resolve();
		if (!source) return true;
		const bool raiseClick = input.ChangedButton == MouseButton::Left
			&& hasMatchingPress
			&& source->DefaultRaiseClickOnLeftButtonUp();
		source = resolve();
		if (!source) return true;
		if (raiseClick)
		{
			source->BeforeDefaultClick(input.ChangedButton, eventArgs);
			source = resolve();
			if (!source) return true;
			RoutedEventArgs clickArgs;
			source->Click(source, clickArgs);
			source = resolve();
			if (!source) return true;
			source->AfterDefaultClick(input.ChangedButton, eventArgs);
			source = resolve();
			if (!source) return true;
		}
		source->OnMouseUp(source, eventArgs);
		source = resolve();
		if (!source) return true;
		const bool invalidate = source->DefaultInvalidateVisualOnMouseUp(
			input.ChangedButton);
		source = resolve();
		if (source && invalidate) source->InvalidateVisual();
		source = resolve();
		if (source && input.ChangedButton == MouseButton::Left
			&& source->IsMouseCaptured())
			(void)source->ReleaseMouseCapture();
	}
	break;
	case InputReportKind::PointerDoubleClick:
	{
		auto* source = resolve();
		if (!source) return true;
		if (input.ChangedButton == MouseButton::Left
			&& source->DefaultRaiseClickOnLeftButtonUp())
		{
			source->_defaultLeftButtonPressActive = true;
			(void)source->CaptureMouse();
		}
		source = resolve();
		if (!source) return true;
		const bool selectOnDoubleClick = input.ChangedButton == MouseButton::Left
			&& source->DefaultSelectOnLeftButtonDown();
		source = resolve();
		if (!source) return true;
		if (selectOnDoubleClick && source->GetPresentationWindow())
		{
			const ControlWeakReference windowReference(source->GetPresentationWindow());
			if (auto* window = dynamic_cast<Window*>(windowReference.Get()))
				window->SetKeyboardFocus(source, false);
		}
		auto eventArgs = input.CreateMouseEventArgs();
		source = resolve();
		if (!source) return true;
		source->BeforeDefaultMouseDown(input.ChangedButton, eventArgs);
		source = resolve();
		if (!source) return true;
		source->OnMouseDoubleClick(source, eventArgs);
		source = resolve();
		if (source && source->DefaultInvalidateVisualOnMouseDown(
			input.ChangedButton)) source->InvalidateVisual();
	}
	break;
	case InputReportKind::Cancel:
	case InputReportKind::CaptureLost:
		if (auto* source = resolve())
		{
			source->_defaultLeftButtonPressActive = false;
			source->SetStyleState(ControlStyleState::Pressed, false);
			if (input.Kind == InputReportKind::Cancel
				&& source->IsMouseCaptured())
				(void)source->ReleaseMouseCapture();
		}
		break;
	case InputReportKind::KeyDown:
	{
		auto eventArgs = input.CreateKeyEventArgs();
		if (auto* source = resolve())
			source->OnKeyDown(source, eventArgs);
	}
	break;
	case InputReportKind::KeyUp:
	{
		auto eventArgs = input.CreateKeyEventArgs();
		if (auto* source = resolve())
			source->OnKeyUp(source, eventArgs);
	}
	break;
	}
	return true;
}

// 布局属性实现
GET_CPP(Control, Thickness, Margin)
{
	static const auto& property = MarginProperty();
	return GetDependencyPropertyValue<Thickness>(property);
}
SET_CPP(Control, Thickness, Margin)
{
	static const auto& property = MarginProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, Thickness, Padding)
{
	static const auto& property = PaddingProperty();
	BindingValue value;
	Thickness padding{};
	if (TryGetPropertyValue(property, value))
		(void)value.TryGet(padding);
	return padding;
}
SET_CPP(Control, Thickness, Padding)
{
	static const auto& property = PaddingProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, ::HorizontalAlignment, HorizontalAlignment)
{
	static const auto& property =
		HorizontalAlignmentProperty();
	return GetDependencyPropertyValue<::HorizontalAlignment>(property);
}
SET_CPP(Control, ::HorizontalAlignment, HorizontalAlignment)
{
	static const auto& property =
		HorizontalAlignmentProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, ::VerticalAlignment, VerticalAlignment)
{
	static const auto& property =
		VerticalAlignmentProperty();
	return GetDependencyPropertyValue<::VerticalAlignment>(property);
}
SET_CPP(Control, ::VerticalAlignment, VerticalAlignment)
{
	static const auto& property =
		VerticalAlignmentProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, ::HorizontalAlignment, HorizontalContentAlignment)
{
	static const auto& property =
		HorizontalContentAlignmentProperty();
	return GetDependencyPropertyValue<::HorizontalAlignment>(property);
}
SET_CPP(Control, ::HorizontalAlignment, HorizontalContentAlignment)
{
	static const auto& property =
		HorizontalContentAlignmentProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, ::VerticalAlignment, VerticalContentAlignment)
{
	static const auto& property =
		VerticalContentAlignmentProperty();
	return GetDependencyPropertyValue<::VerticalAlignment>(property);
}
SET_CPP(Control, ::VerticalAlignment, VerticalContentAlignment)
{
	static const auto& property =
		VerticalContentAlignmentProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, int, GridRow)
{
	static const auto& property = GridRowProperty();
	return GetDependencyPropertyValue<int>(property);
}
SET_CPP(Control, int, GridRow)
{
	static const auto& property = GridRowProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, int, GridColumn)
{
	static const auto& property = GridColumnProperty();
	return GetDependencyPropertyValue<int>(property);
}
SET_CPP(Control, int, GridColumn)
{
	static const auto& property = GridColumnProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, int, GridRowSpan)
{
	static const auto& property =
		GridRowSpanProperty();
	return GetDependencyPropertyValue<int>(property);
}
SET_CPP(Control, int, GridRowSpan)
{
	static const auto& property =
		GridRowSpanProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, int, GridColumnSpan)
{
	static const auto& property =
		GridColumnSpanProperty();
	return GetDependencyPropertyValue<int>(property);
}
SET_CPP(Control, int, GridColumnSpan)
{
	static const auto& property =
		GridColumnSpanProperty();
	(void)SetDependencyPropertyValue(property, value);
}

GET_CPP(Control, Dock, DockPosition)
{
	static const auto& property =
		DockPositionProperty();
	return GetDependencyPropertyValue<Dock>(property);
}
SET_CPP(Control, Dock, DockPosition)
{
	static const auto& property =
		DockPositionProperty();
	(void)SetDependencyPropertyValue(property, value);
}

cui::core::Size Control::GetMinSizeDip() const noexcept
{
	static const auto& widthProperty =
		MinWidthProperty();
	static const auto& heightProperty =
		MinHeightProperty();
	return {
		GetDependencyPropertyValue<float>(widthProperty),
		GetDependencyPropertyValue<float>(heightProperty) };
}

void Control::SetMinSizeDip(cui::core::Size value)
{
	MinWidth = value.width;
	MinHeight = value.height;
}

cui::core::Size Control::GetMaxSizeDip() const noexcept
{
	static const auto& widthProperty =
		MaxWidthProperty();
	static const auto& heightProperty =
		MaxHeightProperty();
	return {
		GetDependencyPropertyValue<float>(widthProperty),
		GetDependencyPropertyValue<float>(heightProperty) };
}

void Control::SetMaxSizeDip(cui::core::Size value)
{
	MaxWidth = value.width;
	MaxHeight = value.height;
}

cui::layout::LayoutStyle Control::GetSpecifiedLayout() const
{
	cui::layout::LayoutStyle result;
	static const auto& widthProperty =
		WidthProperty();
	static const auto& heightProperty =
		HeightProperty();
	static const auto& marginProperty =
		MarginProperty();
	static const auto& paddingProperty =
		PaddingProperty();
	static const auto& horizontalAlignmentProperty =
		HorizontalAlignmentProperty();
	static const auto& verticalAlignmentProperty =
		VerticalAlignmentProperty();
	result.width =
		GetDependencyPropertyValue<cui::layout::Length>(widthProperty);
	result.height =
		GetDependencyPropertyValue<cui::layout::Length>(heightProperty);
	result.minimumSize = GetMinSizeDip();
	result.maximumSize = GetMaxSizeDip();

	const auto margin =
		GetDependencyPropertyValue<Thickness>(marginProperty);
	// Padding belongs to WPF Control (and a few explicit AddOwner types), not
	// FrameworkElement. Panel, Border, Popup and other structural elements still
	// share this C++ layout helper, so absence is the semantic zero value rather
	// than a failed dependency-property read.
	Thickness padding{};
	BindingValue paddingValue;
	if (const_cast<Control*>(this)->TryGetPropertyValue(
		paddingProperty, paddingValue))
		(void)paddingValue.TryGet(padding);
	result.margin = cui::core::Insets{
		margin.Left, margin.Top, margin.Right, margin.Bottom };
	result.padding = cui::core::Insets{
		padding.Left, padding.Top, padding.Right, padding.Bottom };
	result.horizontalAlignment = ToLayoutAlignment(
		GetDependencyPropertyValue<::HorizontalAlignment>(
			horizontalAlignmentProperty));
	result.verticalAlignment = ToLayoutAlignment(
		GetDependencyPropertyValue<::VerticalAlignment>(
			verticalAlignmentProperty));
	return result;
}

cui::core::Size Control::MeasureCore(const cui::core::Constraints& available)
{
	(void)available;
	// WPF Control.MeasureOverride returns zero when neither a template nor a
	// derived control contributes content.  The former 120x20 fallback was a
	// WinForms-era preferred size which made empty and text-adjacent elements
	// claim arbitrary layout space.
	return {};
}

cui::core::Size Control::ResolveDesiredSize(
	cui::core::Size intrinsicSize,
	const cui::core::Constraints& available) const
{
	intrinsicSize = intrinsicSize.NonNegative();
	const auto availableConstraints = available.Normalized();
	const auto elementConstraints =
		ElementSizeConstraints(GetSpecifiedLayout());

	intrinsicSize.width = (std::clamp)(
		intrinsicSize.width,
		elementConstraints.minimum.width,
		elementConstraints.maximum.width);
	intrinsicSize.height = (std::clamp)(
		intrinsicSize.height,
		elementConstraints.minimum.height,
		elementConstraints.maximum.height);
	// The parent remains authoritative when its slot is smaller than the
	// element's Min/Width, matching FrameworkElement's clipped DesiredSize.
	return availableConstraints.Constrain(intrinsicSize);
}

cui::core::Size Control::Measure(const cui::core::Constraints& available)
{
	const cui::core::Constraints constraints = available.Normalized();
	if (IsCollapsed())
	{
		if (_layoutState.NeedsMeasure()
			|| _layoutState.lastMeasureConstraints != constraints
			|| _layoutState.desiredSize != cui::core::Size{})
			_layoutState.CommitMeasure({}, constraints);
		return {};
	}
	const ControlWeakReference lifetime(this);
	(void)ApplyTemplate();
	auto* live = lifetime.Get();
	if (!live) return {};
	if (live->_layoutState.NeedsMeasure() ||
		live->_layoutState.lastMeasureConstraints != constraints)
	{
		const auto elementConstraints =
			ElementSizeConstraints(live->GetSpecifiedLayout());

		// WPF applies Width/Height/Min/Max to the available size before
		// MeasureOverride. This is essential for controls whose height depends
		// on the offered width, notably wrapping TextBlock.
		const cui::core::Size measureMaximum{
			(std::max)(
				elementConstraints.minimum.width,
				(std::min)(
					constraints.maximum.width,
					elementConstraints.maximum.width)),
			(std::max)(
				elementConstraints.minimum.height,
				(std::min)(
					constraints.maximum.height,
					elementConstraints.maximum.height)) };
		const cui::core::Constraints measureConstraints{
			cui::core::Size{}, measureMaximum };
		live->PrepareMeasureCore(measureConstraints);
		live = lifetime.Get();
		if (!live) return {};
		auto* templateRoot = live->GetControlTemplateRoot();
		const auto intrinsic = templateRoot
			? templateRoot->Measure(measureConstraints)
			: live->MeasureCore(measureConstraints);
		live = lifetime.Get();
		if (!live) return {};
		const auto finalized = live->FinalizeMeasureCore(
			intrinsic, measureConstraints);
		live = lifetime.Get();
		if (!live) return {};
		live->_layoutState.CommitMeasure(
			live->ResolveDesiredSize(finalized, constraints), constraints);
	}
	live = lifetime.Get();
	return live ? live->_layoutState.desiredSize : cui::core::Size{};
}


cui::core::Point Control::GetActualLocationDip() const
{
	if (_layoutState.hasArranged)
		return _layoutState.arrangedRect.Origin();
	return {};
}

cui::core::Size Control::GetActualSizeDip() const
{
	return _layoutState.hasArranged
		? _layoutState.arrangedRect.Extent().NonNegative()
		: cui::core::Size{};
}

cui::core::Size Control::GetRenderSizeDip()
{
	return GetActualSizeDip();
}

cui::core::Point Control::GetAbsoluteLocationDip() const
{
	const Control* ancestor = this;
	cui::core::Point absoluteLocation = ancestor->GetActualLocationDip();
	InlinePointerSet<const Control> visited;
	// A Popup owns a window-viewport coordinate space even though its logical
	// template ownership remains in the main visual tree. Descendants accumulate
	// through that transient root, but never inherit coordinates above it.
	while (ancestor->_visualParent
		&& !ancestor->BreaksVisualPresentationInheritance())
	{
		if (!visited.Insert(ancestor)) break;
		ancestor = ancestor->_visualParent;
		const auto ancestorLocation = ancestor->GetActualLocationDip();
		absoluteLocation += cui::core::Vector{
			ancestorLocation.x, ancestorLocation.y };
		const auto childOffset = ancestor->GetVisualChildrenRenderOffset();
		absoluteLocation += cui::core::Vector{
			(float)childOffset.x, (float)childOffset.y };
	}
	return absoluteLocation;
}

cui::core::Rect Control::GetAbsoluteRectDip() const
{
	return cui::core::Rect{
		GetAbsoluteLocationDip(), GetActualSizeDip() };
}

D2D1_RECT_F Control::GetAbsoluteBoundsDip() const
{
	const auto rect = GetAbsoluteRectDip();
	return D2D1::RectF(
		rect.Left(), rect.Top(), rect.Right(), rect.Bottom());
}

D2D1_MATRIX_3X2_F Control::GetInheritedRenderTransform() const
{
	auto result = D2D1::Matrix3x2F::Identity();
	if (BreaksVisualPresentationInheritance()) return result;
	InlinePointerSet<const Control> visited;
	(void)visited.Insert(this);
	for (auto* ancestor = this->_visualParent;
		ancestor && visited.Insert(ancestor);
		ancestor = ancestor->BreaksVisualPresentationInheritance()
			? nullptr : ancestor->_visualParent)
		result = result * AsMatrix(
			ancestor->GetEffectiveDescendantRenderTransform());
	return result;
}

D2D1_MATRIX_3X2_F Control::GetEffectiveDescendantRenderTransform() const
{
	auto result = D2D1::Matrix3x2F::Identity();
	if (_renderTransform)
	{
		const auto size = const_cast<Control*>(this)->GetActualSizeDip();
		const auto local = AsMatrix(_renderTransform->ToMatrix(
			D2D1::SizeF(size.width, size.height), _renderTransformOrigin));
		const auto absolute = GetAbsoluteLocationDip();
		result = D2D1::Matrix3x2F::Translation(-absolute.x, -absolute.y)
			* local
			* D2D1::Matrix3x2F::Translation(absolute.x, absolute.y);
	}
	D2D1_MATRIX_3X2_F extra{};
	if (TryGetDescendantRenderTransform(extra))
		result = result * AsMatrix(extra);
	return result;
}

D2D1_MATRIX_3X2_F Control::GetLocalToRenderTransform() const
{
	const auto size = const_cast<Control*>(this)->GetActualSizeDip();
	const auto local = _renderTransform
		? AsMatrix(_renderTransform->ToMatrix(
			D2D1::SizeF(size.width, size.height), _renderTransformOrigin))
		: D2D1::Matrix3x2F::Identity();
	const auto absolute = GetAbsoluteLocationDip();
	return local
		* D2D1::Matrix3x2F::Translation(absolute.x, absolute.y)
		* AsMatrix(GetInheritedRenderTransform());
}

D2D1_MATRIX_3X2_F Control::GetInheritedRenderTransformForRecording() const
{
	auto result = D2D1::Matrix3x2F::Identity();
	if (BreaksVisualPresentationInheritance()) return result;
	InlinePointerSet<const Control> visited;
	(void)visited.Insert(this);
	for (auto* ancestor = this->_visualParent;
		ancestor && visited.Insert(ancestor);
		ancestor = ancestor->BreaksVisualPresentationInheritance()
			? nullptr : ancestor->_visualParent)
		result = result * AsMatrix(
			ancestor->GetEffectiveDescendantRenderTransformForRecording());
	return result;
}

D2D1_MATRIX_3X2_F
Control::GetEffectiveDescendantRenderTransformForRecording() const
{
	auto result = D2D1::Matrix3x2F::Identity();
	if (_renderTransform && !IsRenderTransformSuppressedForRecording(this))
	{
		const auto size = const_cast<Control*>(this)->GetActualSizeDip();
		const auto local = AsMatrix(_renderTransform->ToMatrix(
			D2D1::SizeF(size.width, size.height), _renderTransformOrigin));
		const auto absolute = GetAbsoluteLocationDip();
		result = D2D1::Matrix3x2F::Translation(-absolute.x, -absolute.y)
			* local
			* D2D1::Matrix3x2F::Translation(absolute.x, absolute.y);
	}
	D2D1_MATRIX_3X2_F extra{};
	if (TryGetDescendantRenderTransform(extra))
		result = result * AsMatrix(extra);
	return result;
}

D2D1_MATRIX_3X2_F Control::GetLocalToRenderTransformForRecording() const
{
	const auto size = const_cast<Control*>(this)->GetActualSizeDip();
	const auto local = _renderTransform
		&& !IsRenderTransformSuppressedForRecording(this)
		? AsMatrix(_renderTransform->ToMatrix(
			D2D1::SizeF(size.width, size.height), _renderTransformOrigin))
		: D2D1::Matrix3x2F::Identity();
	const auto absolute = GetAbsoluteLocationDip();
	return local
		* D2D1::Matrix3x2F::Translation(absolute.x, absolute.y)
		* AsMatrix(GetInheritedRenderTransformForRecording());
}

D2D1_RECT_F Control::GetRenderedAbsoluteRectDipForRecording() const
{
	const auto absolute = GetAbsoluteLocationDip();
	const auto size = const_cast<Control*>(this)->GetRenderSizeDip();
	const auto transform = D2D1::Matrix3x2F::Translation(
		-absolute.x, -absolute.y)
		* AsMatrix(GetLocalToRenderTransformForRecording());
	const D2D1_RECT_F rect{
		absolute.x, absolute.y,
		absolute.x + size.width, absolute.y + size.height };
	const D2D1_POINT_2F points[]{
		transform.TransformPoint(D2D1::Point2F(rect.left, rect.top)),
		transform.TransformPoint(D2D1::Point2F(rect.right, rect.top)),
		transform.TransformPoint(D2D1::Point2F(rect.left, rect.bottom)),
		transform.TransformPoint(D2D1::Point2F(rect.right, rect.bottom)) };
	D2D1_RECT_F bounds{
		points[0].x, points[0].y, points[0].x, points[0].y };
	for (size_t index = 1; index < std::size(points); ++index)
	{
		bounds.left = (std::min)(bounds.left, points[index].x);
		bounds.top = (std::min)(bounds.top, points[index].y);
		bounds.right = (std::max)(bounds.right, points[index].x);
		bounds.bottom = (std::max)(bounds.bottom, points[index].y);
	}
	return bounds;
}

std::span<Control* const>
Control::ExchangeRenderTransformSuppressionsForRecording(
	std::span<Control* const> roots) noexcept
{
	auto previous = RenderTransformSuppressionRoots;
	RenderTransformSuppressionRoots = roots;
	return previous;
}

bool Control::TryTransformRenderPointToLocal(
	D2D1_POINT_2F renderPoint,
	D2D1_POINT_2F& localPoint) const
{
	auto inverse = AsMatrix(GetLocalToRenderTransform());
	if (!inverse.Invert()) return false;
	localPoint = inverse.TransformPoint(renderPoint);
	return std::isfinite(localPoint.x) && std::isfinite(localPoint.y);
}

bool Control::IsRenderPointInsideClip(D2D1_POINT_2F renderPoint) const
{
	std::unordered_set<const Control*> visited;
	for (auto* current = this;
		current && visited.insert(current).second;
		current = current->_visualParent)
	{
		D2D1_POINT_2F local{};
		if (current->_clip
			&& (!current->TryTransformRenderPointToLocal(renderPoint, local)
				|| !current->_clip->ContainsPoint(local)))
			return false;

		// ClipsChildren describes the viewport offered by an ancestor to this
		// descendant. The element itself is still tested against ContainsPoint;
		// only its visual parents contribute child-layout clips here.
		if (current != this
			&& const_cast<Control*>(current)->ClipsChildren())
		{
			if (!current->TryTransformRenderPointToLocal(renderPoint, local))
				return false;
			const auto clip =
				const_cast<Control*>(current)->GetVisualChildrenClipRect();
			if (local.x < clip.left || local.y < clip.top
				|| local.x > clip.right || local.y > clip.bottom)
				return false;
		}
		if (current->BreaksVisualPresentationInheritance()) break;
	}
	return true;
}

D2D1_RECT_F Control::TransformAbsoluteRectToRenderSpace(
	const D2D1_RECT_F& rect) const
{
	const auto absolute = GetAbsoluteLocationDip();
	const auto transform = D2D1::Matrix3x2F::Translation(
		-absolute.x, -absolute.y)
		* AsMatrix(GetLocalToRenderTransform());
	const D2D1_POINT_2F points[] = {
		transform.TransformPoint(D2D1::Point2F(rect.left, rect.top)),
		transform.TransformPoint(D2D1::Point2F(rect.right, rect.top)),
		transform.TransformPoint(D2D1::Point2F(rect.left, rect.bottom)),
		transform.TransformPoint(D2D1::Point2F(rect.right, rect.bottom))
	};
	D2D1_RECT_F bounds{
		points[0].x, points[0].y, points[0].x, points[0].y };
	for (size_t index = 1; index < std::size(points); ++index)
	{
		bounds.left = (std::min)(bounds.left, points[index].x);
		bounds.top = (std::min)(bounds.top, points[index].y);
		bounds.right = (std::max)(bounds.right, points[index].x);
		bounds.bottom = (std::max)(bounds.bottom, points[index].y);
	}
	return bounds;
}

D2D1_RECT_F Control::GetRenderedAbsoluteRectDip()
{
	const cui::core::Rect rect{
		GetAbsoluteLocationDip(), GetRenderSizeDip() };
	return TransformAbsoluteRectToRenderSpace(D2D1_RECT_F{
		rect.Left(), rect.Top(), rect.Right(), rect.Bottom() });
}

void Control::Arrange(cui::core::Rect finalRect)
{
	finalRect = finalRect.Normalized();
	const cui::core::Rect previousRect = _layoutState.hasArranged
		? _layoutState.arrangedRect
		: cui::core::Rect{};
	const bool geometryChanged = previousRect != finalRect;
	const bool sizeChanged = previousRect.width != finalRect.width
		|| previousRect.height != finalRect.height;

	if (geometryChanged)
		InvalidateVisualBoundsSubtree();
	_layoutState.CommitArrange(finalRect);
	if (auto* templateRoot = GetControlTemplateRoot())
	{
		templateRoot->Arrange(cui::core::Rect{
			0.0f, 0.0f, finalRect.width, finalRect.height });
	}
	if (geometryChanged)
	{
		InvalidatePresentationGeometrySubtree();
		InvalidateVisualBoundsSubtree();
	}

	if (sizeChanged)
	{
		if (const auto* metadata = GetPropertyMetadata(ActualWidthProperty()))
			ApplyPropertyMetadataChange(*metadata,
				BindingValue(previousRect.width), BindingValue(finalRect.width));
		if (const auto* metadata = GetPropertyMetadata(ActualHeightProperty()))
			ApplyPropertyMetadataChange(*metadata,
				BindingValue(previousRect.height), BindingValue(finalRect.height));
		SizeChangedEventArgs args(
			previousRect.Extent(), finalRect.Extent());
		this->SizeChanged(this, args);
	}
	if (sizeChanged)
		this->OnComputedLayoutSizeChanged();
}
