#define NOMINMAX
#include "ScrollViewer.h"
#include "Window.h"
#include "Layout/OverlayLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
	template<typename TValue>
	DependencyPropertyOptions<ScrollViewer, TValue> ScrollViewerPropertyOptions(
		TValue defaultValue
		CUI_DESIGN_METADATA_ARGUMENTS(
			const wchar_t* category,
			int categoryOrder,
			int order,
			DependencyPropertyEditorKind editor),
		DependencyPropertyFlags flags = DependencyPropertyFlags::AffectsRender)
	{
		DependencyPropertyOptions<ScrollViewer, TValue> options;
		options.DefaultValue = std::move(defaultValue);
		options.Flags = flags;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = category;
		options.Design.CategoryOrder = categoryOrder;
		options.Design.Order = order;
		options.Design.Editor = editor;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return options;
	}

	auto ScrollViewerPropertySubscriber(
		const DependencyProperty& (*propertyAccessor)())
	{
		return [propertyAccessor](
			ScrollViewer& target,
			DependencyPropertyMetadata::ChangeHandler handler,
			DataSourceUpdateMode)
		{
			return target.OnPropertyValueChanged.Subscribe(
				[propertyAccessor, handler = std::move(handler)](
					DependencyObject*, const DependencyPropertyChangedEventArgs& args)
				{
					if (args.Property == &propertyAccessor())
						handler();
				});
		};
	}

	DependencyPropertyOptions<ScrollViewer, int>
		ScrollBarVisibilityOptions(int order)
	{
		const auto layoutFlags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsRender;
		auto options = ScrollViewerPropertyOptions(
			static_cast<int>(ScrollBarVisibility::Auto)
			CUI_DESIGN_METADATA_ARGUMENTS(
				L"Behavior", 110, order,
				DependencyPropertyEditorKind::Choice), layoutFlags);
		options.Validate = [](const int& proposed)
		{
			switch (static_cast<ScrollBarVisibility>(proposed))
			{
			case ScrollBarVisibility::Disabled:
			case ScrollBarVisibility::Auto:
			case ScrollBarVisibility::Hidden:
			case ScrollBarVisibility::Visible:
				return true;
			default:
				return false;
			}
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Choices = {
			{ L"Disabled", BindingValue(static_cast<int>(ScrollBarVisibility::Disabled)) },
			{ L"Auto", BindingValue(static_cast<int>(ScrollBarVisibility::Auto)) },
			{ L"Hidden", BindingValue(static_cast<int>(ScrollBarVisibility::Hidden)) },
			{ L"Visible", BindingValue(static_cast<int>(ScrollBarVisibility::Visible)) },
		};
		)
		return options;
	}

	template<typename TName, typename TGetter, typename TSetter>
	DependencyPropertyKeyRegistration RegisterScrollViewerReadOnlyProperty(
		TName name,
		const DependencyProperty& (*propertyAccessor)(),
		TGetter getter,
		TSetter setter)
	{
		DependencyPropertyOptions<ScrollViewer, double> options;
		options.DefaultValue = 0.0;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			ScrollViewer, double>(
				std::move(name), std::move(getter), std::move(setter),
				ScrollViewerPropertySubscriber(propertyAccessor),
				std::move(options));
	}

	bool CanScroll(ScrollBarVisibility visibility) noexcept
	{
		return visibility != ScrollBarVisibility::Disabled;
	}

	bool MustShowScrollBar(ScrollBarVisibility visibility) noexcept
	{
		return visibility == ScrollBarVisibility::Visible;
	}

	bool ShouldShowScrollBar(
		ScrollBarVisibility visibility, double extent, float viewport) noexcept
	{
		return visibility == ScrollBarVisibility::Visible
			|| (visibility == ScrollBarVisibility::Auto
				&& extent > static_cast<double>(viewport));
	}

	float SaturateRenderDip(double value) noexcept
	{
		if (std::isnan(value)) return 0.0f;
		const double limit = static_cast<double>(
			(std::numeric_limits<float>::max)());
		return static_cast<float>((std::clamp)(value, -limit, limit));
	}

	double FiniteMeasuredExtent(float value) noexcept
	{
		if (std::isnan(value) || value <= 0.0f) return 0.0;
		return std::isfinite(value)
			? static_cast<double>(value)
			: static_cast<double>((std::numeric_limits<float>::max)());
	}

	double ClampScrollOffset(double value, double maximum) noexcept
	{
		if (!std::isfinite(maximum) || maximum <= 0.0) return 0.0;
		if (std::isnan(value) || value <= 0.0) return 0.0;
		if (!std::isfinite(value) || value >= maximum) return maximum;
		return value;
	}

}

UIClass ScrollViewer::Type() { return UIClass::UI_ScrollViewer; }

const DependencyProperty&
	ScrollViewer::HorizontalScrollBarVisibilityProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<ScrollViewer, int>(
			DependencyPropertyRegistrationLiteral(
				L"HorizontalScrollBarVisibility"),
			[](ScrollViewer& target)
			{ return static_cast<int>(target.HorizontalScrollBarVisibility); },
			[](ScrollViewer& target, const int& value)
			{
				target.HorizontalScrollBarVisibility =
					static_cast<ScrollBarVisibility>(value);
			},
			ScrollViewerPropertySubscriber(
				&ScrollViewer::HorizontalScrollBarVisibilityProperty),
			ScrollBarVisibilityOptions(10));
	return *registration;
}

const DependencyProperty&
	ScrollViewer::VerticalScrollBarVisibilityProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<ScrollViewer, int>(
			DependencyPropertyRegistrationLiteral(
				L"VerticalScrollBarVisibility"),
			[](ScrollViewer& target)
			{ return static_cast<int>(target.VerticalScrollBarVisibility); },
			[](ScrollViewer& target, const int& value)
			{
				target.VerticalScrollBarVisibility =
					static_cast<ScrollBarVisibility>(value);
			},
			ScrollViewerPropertySubscriber(
				&ScrollViewer::VerticalScrollBarVisibilityProperty),
			ScrollBarVisibilityOptions(20));
	return *registration;
}

const DependencyProperty& ScrollViewer::ExtentWidthProperty()
{
	return ExtentWidthPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::ExtentWidthPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"ExtentWidth"),
		&ScrollViewer::ExtentWidthProperty,
		[](ScrollViewer& target) { return target._extentWidth; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				ExtentWidthPropertyKey(), target._extentWidth, value);
		});
	return registration.Key();
}

const DependencyProperty& ScrollViewer::ExtentHeightProperty()
{
	return ExtentHeightPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::ExtentHeightPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"ExtentHeight"),
		&ScrollViewer::ExtentHeightProperty,
		[](ScrollViewer& target) { return target._extentHeight; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				ExtentHeightPropertyKey(), target._extentHeight, value);
		});
	return registration.Key();
}

const DependencyProperty& ScrollViewer::ViewportWidthProperty()
{
	return ViewportWidthPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::ViewportWidthPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"ViewportWidth"),
		&ScrollViewer::ViewportWidthProperty,
		[](ScrollViewer& target) { return target._viewportWidth; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				ViewportWidthPropertyKey(), target._viewportWidth, value);
		});
	return registration.Key();
}

const DependencyProperty& ScrollViewer::ViewportHeightProperty()
{
	return ViewportHeightPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::ViewportHeightPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"ViewportHeight"),
		&ScrollViewer::ViewportHeightProperty,
		[](ScrollViewer& target) { return target._viewportHeight; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				ViewportHeightPropertyKey(), target._viewportHeight, value);
		});
	return registration.Key();
}

const DependencyProperty& ScrollViewer::HorizontalOffsetProperty()
{
	return HorizontalOffsetPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::HorizontalOffsetPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"HorizontalOffset"),
		&ScrollViewer::HorizontalOffsetProperty,
		[](ScrollViewer& target) { return target._horizontalOffset; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				HorizontalOffsetPropertyKey(), target._horizontalOffset, value);
		});
	return registration.Key();
}

const DependencyProperty& ScrollViewer::VerticalOffsetProperty()
{
	return VerticalOffsetPropertyKey().Property();
}

const DependencyPropertyKey& ScrollViewer::VerticalOffsetPropertyKey()
{
	static const auto registration = RegisterScrollViewerReadOnlyProperty(
		DependencyPropertyRegistrationLiteral(L"VerticalOffset"),
		&ScrollViewer::VerticalOffsetProperty,
		[](ScrollViewer& target) { return target._verticalOffset; },
		[](ScrollViewer& target, const double& value)
		{
			(void)target.SetReadOnlyPropertyField(
				VerticalOffsetPropertyKey(), target._verticalOffset, value);
		});
	return registration.Key();
}

void ScrollViewer::RegisterDependencyProperties()
{
	ContentControl::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)HorizontalScrollBarVisibilityProperty();
	(void)VerticalScrollBarVisibilityProperty();
	(void)ExtentWidthProperty();
	(void)ExtentHeightProperty();
	(void)ViewportWidthProperty();
	(void)ViewportHeightProperty();
	(void)HorizontalOffsetProperty();
	(void)VerticalOffsetProperty();
#endif
}

void ScrollViewer::PerformPendingLayout()
{
	if (!IsLayoutSuspended() && _contentLayoutPending)
		PerformScrollContentLayout();
}

cui::core::Size ScrollViewer::MeasureCore(
	const cui::core::Constraints& available)
{
	const auto padding = GetSpecifiedLayout().padding;
	return cui::layout::MeasureOverlayChildren(
		GetLayoutChildrenView(),
		cui::core::Constraints::Unbounded(), padding);
}

bool ScrollViewer::HandlesNavigationKey(Key key) const
{
	switch (key)
	{
	case Key::Left:
	case Key::Right:
	case Key::Up:
	case Key::Down:
	case Key::Home:
	case Key::End:
	case Key::PageUp:
	case Key::PageDown:
		return true;
	default:
		return false;
	}
}

bool ScrollViewer::CanHandleMouseWheel(int delta, int localX, int localY)
{
	if (delta == 0) return false;
	PerformPendingLayout();

	auto layout = this->CalcScrollLayout();
	ClampScrollOffsets(layout);
	if (localX < 0 || localY < 0 || localX >= this->ActualWidth || localY >= this->ActualHeight)
		return false;
	if (!layout.HasVerticalScroll || layout.MaxScrollY <= 0.0f)
		return false;
	return delta > 0
		? this->_verticalOffset > 0.0
		: this->_verticalOffset < layout.MaxScrollY;
}

ScrollViewer::ScrollViewer()
	: ContentControl()
{
}

GET_CPP(ScrollViewer, ScrollBarVisibility, HorizontalScrollBarVisibility)
{
	return _horizontalScrollBarVisibility;
}

SET_CPP(ScrollViewer, ScrollBarVisibility, HorizontalScrollBarVisibility)
{
	(void)SetPropertyField(HorizontalScrollBarVisibilityProperty(),
		_horizontalScrollBarVisibility, value);
}

GET_CPP(ScrollViewer, ScrollBarVisibility, VerticalScrollBarVisibility)
{
	return _verticalScrollBarVisibility;
}

SET_CPP(ScrollViewer, ScrollBarVisibility, VerticalScrollBarVisibility)
{
	(void)SetPropertyField(VerticalScrollBarVisibilityProperty(),
		_verticalScrollBarVisibility, value);
}

GET_CPP(ScrollViewer, double, ExtentWidth) { return _extentWidth; }
GET_CPP(ScrollViewer, double, ExtentHeight) { return _extentHeight; }
GET_CPP(ScrollViewer, double, ViewportWidth) { return _viewportWidth; }
GET_CPP(ScrollViewer, double, ViewportHeight) { return _viewportHeight; }
GET_CPP(ScrollViewer, double, HorizontalOffset) { return _horizontalOffset; }
GET_CPP(ScrollViewer, double, VerticalOffset) { return _verticalOffset; }

void ScrollViewer::SetLogicalScrollContent(Control* content)
{
	if (_logicalScrollContent == content) return;
	_logicalScrollContent = content;
	// The provider changes the extent source and the coordinate origin used by
	// every descendant transform. The ordinary layout request republishes the
	// read-only scroll metrics; geometry invalidation prevents replaying commands
	// recorded against the previous origin.
	RequestLayout();
	InvalidateDescendantRenderGeometry();
	InvalidateVisual();
}

const cui::framework::ILogicalScrollContent*
ScrollViewer::ResolveLogicalScrollContent() const noexcept
{
	auto* content = _logicalScrollContent.Get();
	if (!content) return nullptr;
	bool descendant = false;
	for (auto* current = content; current; current = current->GetVisualParent())
		if (current == this)
		{
			descendant = true;
			break;
		}
	return descendant
		? dynamic_cast<const cui::framework::ILogicalScrollContent*>(content)
		: nullptr;
}

double ScrollViewer::ResolveLogicalContentHeight(
	float measuredHeight) const noexcept
{
	const double fallback = FiniteMeasuredExtent(measuredHeight);
	const auto* content = ResolveLogicalScrollContent();
	if (!content) return fallback;
	const double logical = content->LogicalExtentHeightDip();
	return std::isfinite(logical) && logical >= 0.0 ? logical : fallback;
}

double ScrollViewer::ResolveLogicalContentWidth(
	float measuredWidth) const noexcept
{
	const double fallback = FiniteMeasuredExtent(measuredWidth);
	const auto* content = ResolveLogicalScrollContent();
	if (!content) return fallback;
	const double logical = content->LogicalExtentWidthDip();
	return std::isfinite(logical) && logical >= 0.0
		? (std::max)(fallback, logical) : fallback;
}

double ScrollViewer::VerticalContentRenderOffset() const noexcept
{
	const auto* content = ResolveLogicalScrollContent();
	if (!content) return _verticalOffset;
	const double origin = content->VerticalLayoutOriginDip();
	return std::isfinite(origin) ? _verticalOffset - origin : _verticalOffset;
}

cui::core::Point ScrollViewer::GetVisualChildrenRenderOffset() const
{
	return {
		-SaturateRenderDip(_horizontalOffset),
		-SaturateRenderDip(VerticalContentRenderOffset()) };
}

D2D1_RECT_F ScrollViewer::GetVisualChildrenClipRect()
{
	auto layout = this->CalcScrollLayout();
	return D2D1_RECT_F{ 0.0f, 0.0f, layout.ViewportWidth, layout.ViewportHeight };
}

void ScrollViewer::PerformScrollContentLayout()
{
	const float scrollBarThickness = _scrollBarThickness;
	const auto viewportSize = this->GetActualSizeDip();
	auto performLayoutPass = [&](bool reserveVerticalScrollBar, bool reserveHorizontalScrollBar)
		{
			const auto padding = GetSpecifiedLayout().padding;
			const float viewportWidth = (std::max)(0.0f,
				viewportSize.width - padding.Horizontal()
				- (reserveVerticalScrollBar ? scrollBarThickness : 0.0f));
			const float viewportHeight = (std::max)(0.0f,
				viewportSize.height - padding.Vertical()
				- (reserveHorizontalScrollBar ? scrollBarThickness : 0.0f));
			const auto requested = MeasureContentSizeDip();
			cui::layout::ArrangeOverlayChildren(
				GetLayoutChildrenView(),
				cui::core::Rect{
					padding.left,
					padding.top,
					(std::max)(viewportWidth,
						requested.width - padding.Horizontal()),
					(std::max)(viewportHeight,
						requested.height - padding.Vertical()) });
		};

	bool needsVerticalScroll =
		MustShowScrollBar(_verticalScrollBarVisibility);
	bool needsHorizontalScroll =
		MustShowScrollBar(_horizontalScrollBarVisibility);
	for (int iter = 0; iter < 3; ++iter)
	{
		performLayoutPass(needsVerticalScroll, needsHorizontalScroll);

		cui::core::Size content = MeasureContentSizeDip();
		content = content.NonNegative();
		const double logicalContentWidth =
			ResolveLogicalContentWidth(content.width);
		const double logicalContentHeight =
			ResolveLogicalContentHeight(content.height);

		float viewportWidth = std::max(0.0f, viewportSize.width - (needsVerticalScroll ? scrollBarThickness : 0.0f));
		float viewportHeight = std::max(0.0f, viewportSize.height - (needsHorizontalScroll ? scrollBarThickness : 0.0f));
		bool nextNeedsHorizontalScroll = ShouldShowScrollBar(
			_horizontalScrollBarVisibility, logicalContentWidth, viewportWidth);
		bool nextNeedsVerticalScroll = ShouldShowScrollBar(
			_verticalScrollBarVisibility,
			logicalContentHeight, viewportHeight);
		if (nextNeedsHorizontalScroll == needsHorizontalScroll && nextNeedsVerticalScroll == needsVerticalScroll)
		{
			break;
		}

		needsHorizontalScroll = nextNeedsHorizontalScroll;
		needsVerticalScroll = nextNeedsVerticalScroll;
	}

	_contentLayoutPending = false;
	auto layout = CalcScrollLayout();
	ClampScrollOffsets(layout);
}

cui::core::Size ScrollViewer::MeasureContentSizeDip()
{
	return cui::layout::MeasureOverlayChildren(
		GetLayoutChildrenView(),
		cui::core::Constraints::Unbounded(),
		GetSpecifiedLayout().padding);
}

ScrollViewer::ScrollLayout ScrollViewer::CalcScrollLayout()
{
	ScrollLayout layout{};
	layout.ScrollBarThickness = _scrollBarThickness;

	cui::core::Size content = MeasureContentSizeDip();
	content = content.NonNegative();
	const double logicalContentWidth = ResolveLogicalContentWidth(content.width);
	const double logicalContentHeight =
		ResolveLogicalContentHeight(content.height);
	const auto viewportSize = this->GetActualSizeDip();

	bool needsVerticalScroll =
		MustShowScrollBar(_verticalScrollBarVisibility);
	bool needsHorizontalScroll =
		MustShowScrollBar(_horizontalScrollBarVisibility);
	for (int iter = 0; iter < 3; ++iter)
	{
		float viewportWidth = viewportSize.width - (needsVerticalScroll ? layout.ScrollBarThickness : 0.0f);
		float viewportHeight = viewportSize.height - (needsHorizontalScroll ? layout.ScrollBarThickness : 0.0f);
		if (viewportWidth < 0.0f) viewportWidth = 0.0f;
		if (viewportHeight < 0.0f) viewportHeight = 0.0f;

		bool nextNeedsHorizontalScroll = ShouldShowScrollBar(
			_horizontalScrollBarVisibility, logicalContentWidth, viewportWidth);
		bool nextNeedsVerticalScroll = ShouldShowScrollBar(
			_verticalScrollBarVisibility,
			logicalContentHeight, viewportHeight);
		if (nextNeedsHorizontalScroll == needsHorizontalScroll && nextNeedsVerticalScroll == needsVerticalScroll)
		{
			layout.HasHorizontalScroll = needsHorizontalScroll;
			layout.HasVerticalScroll = needsVerticalScroll;
			layout.ViewportWidth = viewportWidth;
			layout.ViewportHeight = viewportHeight;
			layout.ContentWidth = logicalContentWidth;
			layout.ContentHeight = logicalContentHeight;
			layout.MaxScrollX = CanScroll(_horizontalScrollBarVisibility)
				? (std::max)(0.0,
					layout.ContentWidth - static_cast<double>(viewportWidth)) : 0.0;
			layout.MaxScrollY = CanScroll(_verticalScrollBarVisibility)
				? (std::max)(0.0,
					layout.ContentHeight - static_cast<double>(viewportHeight)) : 0.0;
			return layout;
		}
		needsHorizontalScroll = nextNeedsHorizontalScroll;
		needsVerticalScroll = nextNeedsVerticalScroll;
	}

	layout.HasHorizontalScroll = needsHorizontalScroll;
	layout.HasVerticalScroll = needsVerticalScroll;
	layout.ViewportWidth = std::max(0.0f, viewportSize.width - (needsVerticalScroll ? layout.ScrollBarThickness : 0.0f));
	layout.ViewportHeight = std::max(0.0f, viewportSize.height - (needsHorizontalScroll ? layout.ScrollBarThickness : 0.0f));
	layout.ContentWidth = logicalContentWidth;
	layout.ContentHeight = logicalContentHeight;
	layout.MaxScrollX = CanScroll(_horizontalScrollBarVisibility)
		? (std::max)(0.0,
			layout.ContentWidth - static_cast<double>(layout.ViewportWidth)) : 0.0;
	layout.MaxScrollY = CanScroll(_verticalScrollBarVisibility)
		? (std::max)(0.0,
			layout.ContentHeight - static_cast<double>(layout.ViewportHeight)) : 0.0;
	return layout;
}

void ScrollViewer::ClampScrollOffsets(const ScrollLayout& layout)
{
	PublishScrollState(layout, _horizontalOffset, _verticalOffset);
}

void ScrollViewer::PublishScrollState(
	const ScrollLayout& layout,
	double horizontalOffset,
	double verticalOffset)
{
	const ControlWeakReference lifetime(this);
	const double oldHorizontalOffset = _horizontalOffset;
	const double oldVerticalOffset = _verticalOffset;
	const double oldExtentWidth = _extentWidth;
	const double oldExtentHeight = _extentHeight;
	const double oldViewportWidth = _viewportWidth;
	const double oldViewportHeight = _viewportHeight;
	const double nextHorizontalOffset = ClampScrollOffset(
		horizontalOffset, layout.MaxScrollX);
	const double nextVerticalOffset = ClampScrollOffset(
		verticalOffset, layout.MaxScrollY);

	auto update = [&lifetime](
		const DependencyPropertyKey& key,
		double oldValue,
		double value)
	{
		if (std::fabs(oldValue - value) <= 0.000001) return false;
		auto* live = dynamic_cast<ScrollViewer*>(lifetime.Get());
		return live
			? live->TrySetReadOnlyPropertyValue(key, BindingValue(value))
			: false;
	};
	bool changed = false;
	if (update(ExtentWidthPropertyKey(), oldExtentWidth, layout.ContentWidth))
		changed = true;
	if (!lifetime.Get()) return;
	if (update(ExtentHeightPropertyKey(), oldExtentHeight, layout.ContentHeight))
		changed = true;
	if (!lifetime.Get()) return;
	if (update(ViewportWidthPropertyKey(), oldViewportWidth, layout.ViewportWidth))
		changed = true;
	if (!lifetime.Get()) return;
	if (update(ViewportHeightPropertyKey(), oldViewportHeight, layout.ViewportHeight))
		changed = true;
	if (!lifetime.Get()) return;
	if (update(HorizontalOffsetPropertyKey(),
		oldHorizontalOffset, nextHorizontalOffset)) changed = true;
	if (!lifetime.Get()) return;
	if (update(VerticalOffsetPropertyKey(),
		oldVerticalOffset, nextVerticalOffset)) changed = true;
	auto* live = dynamic_cast<ScrollViewer*>(lifetime.Get());
	if (!live || !changed) return;

	ScrollChangedEventArgs args;
	args.HorizontalOffset = live->_horizontalOffset;
	args.HorizontalChange = live->_horizontalOffset - oldHorizontalOffset;
	args.VerticalOffset = live->_verticalOffset;
	args.VerticalChange = live->_verticalOffset - oldVerticalOffset;
	args.ExtentWidth = live->_extentWidth;
	args.ExtentWidthChange = live->_extentWidth - oldExtentWidth;
	args.ExtentHeight = live->_extentHeight;
	args.ExtentHeightChange = live->_extentHeight - oldExtentHeight;
	args.ViewportWidth = live->_viewportWidth;
	args.ViewportWidthChange = live->_viewportWidth - oldViewportWidth;
	args.ViewportHeight = live->_viewportHeight;
	args.ViewportHeightChange = live->_viewportHeight - oldViewportHeight;
	live->OnScrollChanged(live, args);
}

void ScrollViewer::SetScrollOffsetCore(
	double horizontalOffset, double verticalOffset, bool interactive)
{
	_lastScrollChangeWasInteractive = interactive;
	const ControlWeakReference lifetime(this);
	auto layout = CalcScrollLayout();
	auto* live = dynamic_cast<ScrollViewer*>(lifetime.Get());
	if (!live) return;
	const double oldX = live->_horizontalOffset;
	const double oldY = live->_verticalOffset;
	live->PublishScrollState(layout, horizontalOffset, verticalOffset);
	live = dynamic_cast<ScrollViewer*>(lifetime.Get());
	if (!live) return;
	if (std::fabs(oldX - live->_horizontalOffset) > 0.000001
		|| std::fabs(oldY - live->_verticalOffset) > 0.000001)
	{
		// Scroll offset is an ancestor render transform. The viewport damage is
		// local, but every descendant's cached bounds and command transform must
		// be recomputed before replay.
		live->InvalidateDescendantRenderGeometry();
		live->InvalidateVisual();
	}
}

void ScrollViewer::LineLeft() { SetScrollOffsetCore(_horizontalOffset - _mouseWheelStep, _verticalOffset); }
void ScrollViewer::LineRight() { SetScrollOffsetCore(_horizontalOffset + _mouseWheelStep, _verticalOffset); }
void ScrollViewer::LineUp() { SetScrollOffsetCore(_horizontalOffset, _verticalOffset - _mouseWheelStep); }
void ScrollViewer::LineDown() { SetScrollOffsetCore(_horizontalOffset, _verticalOffset + _mouseWheelStep); }
void ScrollViewer::PageLeft() { SetScrollOffsetCore(_horizontalOffset - (std::max)(16.0, _viewportWidth - _mouseWheelStep), _verticalOffset); }
void ScrollViewer::PageRight() { SetScrollOffsetCore(_horizontalOffset + (std::max)(16.0, _viewportWidth - _mouseWheelStep), _verticalOffset); }
void ScrollViewer::PageUp() { SetScrollOffsetCore(_horizontalOffset, _verticalOffset - (std::max)(16.0, _viewportHeight - _mouseWheelStep)); }
void ScrollViewer::PageDown() { SetScrollOffsetCore(_horizontalOffset, _verticalOffset + (std::max)(16.0, _viewportHeight - _mouseWheelStep)); }
void ScrollViewer::ScrollToHorizontalOffset(double offset) { SetScrollOffsetCore(offset, _verticalOffset); }
void ScrollViewer::ScrollToVerticalOffset(double offset) { SetScrollOffsetCore(_horizontalOffset, offset); }
void ScrollViewer::ScrollToHome() { SetScrollOffsetCore(0.0, 0.0); }
void ScrollViewer::ScrollToEnd()
{
	auto layout = CalcScrollLayout();
	SetScrollOffsetCore(layout.MaxScrollX, layout.MaxScrollY);
}

bool ScrollViewer::BringDescendantIntoView(Control* descendant)
{
	if (!descendant || descendant == this) return false;
	bool owned = false;
	for (auto* current = descendant->GetVisualParent(); current;
		current = current->GetVisualParent())
		if (current == this)
		{
			owned = true;
			break;
		}
	if (!owned) return false;
	PerformPendingLayout();
	std::vector<Control*> layoutPath;
	for (auto* current = descendant->GetVisualParent();
		current && current != this; current = current->GetVisualParent())
		layoutPath.push_back(current);
	for (auto current = layoutPath.rbegin(); current != layoutPath.rend(); ++current)
		(*current)->UpdateLayout();

	const auto layout = CalcScrollLayout();
	const auto ownerRaw = GetLocalToRenderTransform();
	auto renderToOwner = D2D1::Matrix3x2F(
		ownerRaw._11, ownerRaw._12, ownerRaw._21, ownerRaw._22,
		ownerRaw._31, ownerRaw._32);
	if (!renderToOwner.Invert()) return false;
	const auto targetRaw = descendant->GetLocalToRenderTransform();
	const auto targetToOwner = D2D1::Matrix3x2F(
		targetRaw._11, targetRaw._12, targetRaw._21, targetRaw._22,
		targetRaw._31, targetRaw._32) * renderToOwner;
	const auto targetSize = descendant->GetActualSizeDip();
	const D2D1_POINT_2F corners[] = {
		targetToOwner.TransformPoint(D2D1::Point2F()),
		targetToOwner.TransformPoint(D2D1::Point2F(targetSize.width, 0.0f)),
		targetToOwner.TransformPoint(D2D1::Point2F(0.0f, targetSize.height)),
		targetToOwner.TransformPoint(D2D1::Point2F(
			targetSize.width, targetSize.height)) };
	float left = corners[0].x;
	float top = corners[0].y;
	float right = corners[0].x;
	float bottom = corners[0].y;
	for (size_t index = 1; index < std::size(corners); ++index)
	{
		left = (std::min)(left, corners[index].x);
		top = (std::min)(top, corners[index].y);
		right = (std::max)(right, corners[index].x);
		bottom = (std::max)(bottom, corners[index].y);
	}
	double x = _horizontalOffset;
	double y = _verticalOffset;
	if (left < 0.0f) x += static_cast<int>(std::floor(left));
	else if (right > layout.ViewportWidth)
		x += static_cast<int>(std::ceil(right - layout.ViewportWidth));
	if (top < 0.0f) y += static_cast<int>(std::floor(top));
	else if (bottom > layout.ViewportHeight)
		y += static_cast<int>(std::ceil(bottom - layout.ViewportHeight));
	const double oldX = _horizontalOffset;
	const double oldY = _verticalOffset;
	SetScrollOffsetCore(x, y);
	return std::fabs(oldX - _horizontalOffset) > 0.000001
		|| std::fabs(oldY - _verticalOffset) > 0.000001;
}

bool ScrollViewer::HitVerticalScrollBar(int localX, int localY, const ScrollLayout& layout) const
{
	if (!layout.HasVerticalScroll) return false;
	const auto size = const_cast<ScrollViewer*>(this)->GetActualSizeDip();
	return (float)localX >= layout.ViewportWidth && (float)localX < size.width
		&& localY >= 0 && (float)localY < layout.ViewportHeight;
}

bool ScrollViewer::HitHorizontalScrollBar(int localX, int localY, const ScrollLayout& layout) const
{
	if (!layout.HasHorizontalScroll) return false;
	const auto size = const_cast<ScrollViewer*>(this)->GetActualSizeDip();
	return (float)localY >= layout.ViewportHeight && (float)localY < size.height
		&& localX >= 0 && (float)localX < layout.ViewportWidth;
}

bool ScrollViewer::ShouldHitTestChildrenAt(int localX, int localY) const
{
	if (!this->HitTestChildren()) return false;
	auto layout = const_cast<ScrollViewer*>(this)->CalcScrollLayout();
	if (localX < 0 || localY < 0) return false;
	if (localX >= (int)layout.ViewportWidth || localY >= (int)layout.ViewportHeight) return false;
	return true;
}

bool ScrollViewer::HitChild(Control* child, int localX, int localY, int& childX, int& childY) const
{
	if (!child || !child->IsVisible || !child->IsEnabled) return false;
	const auto childLocation = child->GetActualLocationDip();
	const auto childSize = child->GetActualSizeDip();
	const float drawX = childLocation.x - SaturateRenderDip(_horizontalOffset);
	const float drawY = childLocation.y
		- SaturateRenderDip(VerticalContentRenderOffset());
	const cui::core::Rect childRect{ drawX, drawY, childSize.width, childSize.height };
	if (!childRect.Contains(cui::core::Point{ (float)localX, (float)localY }))
		return false;
	childX = static_cast<int>(std::floor((float)localX - drawX));
	childY = static_cast<int>(std::floor((float)localY - drawY));
	return true;
}

void ScrollViewer::DrawScrollBars(const ScrollLayout& layout)
{
	auto d2d = this->GetDrawingContext();
	if (layout.HasVerticalScroll && layout.ViewportHeight > 0.0f && layout.ContentHeight > layout.ViewportHeight)
	{
		float thumbH = static_cast<float>(
			(static_cast<double>(layout.ViewportHeight)
				* static_cast<double>(layout.ViewportHeight))
			/ layout.ContentHeight);
		float minThumbH = std::max(16.0f, layout.ViewportHeight * 0.1f);
		thumbH = std::clamp(thumbH, minThumbH, layout.ViewportHeight);
		float moveSpace = std::max(0.0f, layout.ViewportHeight - thumbH);
		float per = layout.MaxScrollY > 0.0
			? static_cast<float>((std::clamp)(
				_verticalOffset / layout.MaxScrollY, 0.0, 1.0)) : 0.0f;
		float thumbTop = per * moveSpace;
		d2d->FillRoundRect(layout.ViewportWidth, 0.0f, layout.ScrollBarThickness, layout.ViewportHeight, _scrollBackColor, 4.0f);
		d2d->FillRoundRect(layout.ViewportWidth, thumbTop, layout.ScrollBarThickness, thumbH, _scrollForeColor, 4.0f);
	}

	if (layout.HasHorizontalScroll && layout.ViewportWidth > 0.0f && layout.ContentWidth > layout.ViewportWidth)
	{
		float thumbW = static_cast<float>(
			(static_cast<double>(layout.ViewportWidth)
				* static_cast<double>(layout.ViewportWidth))
			/ layout.ContentWidth);
		float minThumbW = std::max(16.0f, layout.ViewportWidth * 0.1f);
		thumbW = std::clamp(thumbW, minThumbW, layout.ViewportWidth);
		float moveSpace = std::max(0.0f, layout.ViewportWidth - thumbW);
		float per = layout.MaxScrollX > 0.0
			? static_cast<float>((std::clamp)(
				_horizontalOffset / layout.MaxScrollX, 0.0, 1.0)) : 0.0f;
		float thumbLeft = per * moveSpace;
		d2d->FillRoundRect(0.0f, layout.ViewportHeight, layout.ViewportWidth, layout.ScrollBarThickness, _scrollBackColor, 4.0f);
		d2d->FillRoundRect(thumbLeft, layout.ViewportHeight, thumbW, layout.ScrollBarThickness, _scrollForeColor, 4.0f);
	}

	if (layout.HasHorizontalScroll && layout.HasVerticalScroll)
	{
		d2d->FillRect(layout.ViewportWidth, layout.ViewportHeight, layout.ScrollBarThickness, layout.ScrollBarThickness, _scrollBackColor);
	}
}

void ScrollViewer::UpdateVerticalScrollByThumb(float localY, const ScrollLayout& layout)
{
	if (!layout.HasVerticalScroll || layout.ContentHeight <= layout.ViewportHeight || layout.ViewportHeight <= 0.0f)
		return;
	float thumbH = static_cast<float>(
		(static_cast<double>(layout.ViewportHeight)
			* static_cast<double>(layout.ViewportHeight))
		/ layout.ContentHeight);
	float minThumbH = std::max(16.0f, layout.ViewportHeight * 0.1f);
	thumbH = std::clamp(thumbH, minThumbH, layout.ViewportHeight);
	float moveSpace = std::max(0.0f, layout.ViewportHeight - thumbH);
	if (moveSpace <= 0.0f) return;
	float grab = std::clamp(this->_verticalScrollThumbGrabOffset, 0.0f, thumbH);
	if (grab <= 0.0f) grab = thumbH * 0.5f;
	float target = std::clamp(localY - grab, 0.0f, moveSpace);
	float per = target / moveSpace;
	SetScrollOffsetCore(_horizontalOffset, per * layout.MaxScrollY, true);
}

void ScrollViewer::UpdateHorizontalScrollByThumb(float localX, const ScrollLayout& layout)
{
	if (!layout.HasHorizontalScroll || layout.ContentWidth <= layout.ViewportWidth || layout.ViewportWidth <= 0.0f)
		return;
	float thumbW = static_cast<float>(
		(static_cast<double>(layout.ViewportWidth)
			* static_cast<double>(layout.ViewportWidth))
		/ layout.ContentWidth);
	float minThumbW = std::max(16.0f, layout.ViewportWidth * 0.1f);
	thumbW = std::clamp(thumbW, minThumbW, layout.ViewportWidth);
	float moveSpace = std::max(0.0f, layout.ViewportWidth - thumbW);
	if (moveSpace <= 0.0f) return;
	float grab = std::clamp(this->_horizontalScrollThumbGrabOffset, 0.0f, thumbW);
	if (grab <= 0.0f) grab = thumbW * 0.5f;
	float target = std::clamp(localX - grab, 0.0f, moveSpace);
	float per = target / moveSpace;
	SetScrollOffsetCore(per * layout.MaxScrollX, _verticalOffset, true);
}

void ScrollViewer::OnRender()
{
	if (this->IsVisible == false) return;
	auto d2d = this->GetDrawingContext();
	const auto size = this->GetActualSizeDip();
	const float actualWidth = size.width;
	const float actualHeight = size.height;
	const float border = this->BorderThickness.MaxEdge();
	auto layout = this->CalcScrollLayout();
	ClampScrollOffsets(layout);

	this->BeginRender();
	{
		d2d->FillRect(0, 0, actualWidth, actualHeight, this->RendererBackgroundColor);

		DrawScrollBars(layout);
		if (border > 0.0f && this->RendererBorderColor.a > 0.0f)
		{
			const float drawW = (std::max)(0.0f, actualWidth - border);
			const float drawH = (std::max)(0.0f, actualHeight - border);
			d2d->DrawRect(border * 0.5f, border * 0.5f,
				drawW, drawH, this->RendererBorderColor, border);
		}
	}
	if (!this->IsEnabled)
		d2d->FillRect(0, 0, actualWidth, actualHeight,
			cui::theme::palette::DisabledOverlay);
	this->EndRender();
}

bool ScrollViewer::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost)
	{
		const ControlWeakReference scrollLifetime(this);
		const bool restoreVerticalCache = _draggingVerticalScrollBar;
		_draggingVerticalScrollBar = false;
		_draggingHorizontalScrollBar = false;
		if (input.Kind == InputReportKind::Cancel && IsMouseCaptured())
			(void)ReleaseMouseCapture();
		auto* live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
		if (!live) return true;
		if (restoreVerticalCache)
			if (auto* logical = const_cast<cui::framework::ILogicalScrollContent*>(
				live->ResolveLogicalScrollContent()))
				logical->OnVerticalThumbDragCompleted();
		if (!scrollLifetime.Get()) return true;
		return Control::ProcessInput(input);
	}
	if (!this->IsEnabled || !this->IsVisible) return true;
	const ControlWeakReference processLifetime(this);
	PerformPendingLayout();
	auto* processLive = dynamic_cast<ScrollViewer*>(processLifetime.Get());
	if (!processLive) return true;
	auto layout = processLive->CalcScrollLayout();
	processLive = dynamic_cast<ScrollViewer*>(processLifetime.Get());
	if (!processLive) return true;
	processLive->ClampScrollOffsets(layout);
	if (!processLifetime.Get()) return true;

	if (input.Kind == InputReportKind::PointerDown
		&& input.ChangedButton == MouseButton::Left && this->GetPresentationWindow())
	{
		const ControlWeakReference scrollLifetime(this);
		this->GetPresentationWindow()->SetKeyboardFocus(this, false);
		auto* live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
		if (!live) return true;
		layout = live->CalcScrollLayout();
		live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
		if (!live) return true;
		live->ClampScrollOffsets(layout);
		if (!scrollLifetime.Get()) return true;
	}

	if (_draggingVerticalScrollBar
		&& input.Kind == InputReportKind::PointerMove)
	{
		UpdateVerticalScrollByThumb((float)input.Y, layout);
		return true;
	}
	if (_draggingHorizontalScrollBar
		&& input.Kind == InputReportKind::PointerMove)
	{
		UpdateHorizontalScrollByThumb((float)input.X, layout);
		return true;
	}
	if ((_draggingVerticalScrollBar || _draggingHorizontalScrollBar)
		&& input.Kind == InputReportKind::PointerUp)
	{
		const ControlWeakReference scrollLifetime(this);
		const bool restoreVerticalCache = _draggingVerticalScrollBar;
		_draggingVerticalScrollBar = false;
		_draggingHorizontalScrollBar = false;
		if (IsMouseCaptured()) (void)ReleaseMouseCapture();
		auto* live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
		if (!live) return true;
		if (restoreVerticalCache)
			if (auto* logical = const_cast<cui::framework::ILogicalScrollContent*>(
				live->ResolveLogicalScrollContent()))
				logical->OnVerticalThumbDragCompleted();
		if (!scrollLifetime.Get()) return true;
	}

	switch (input.Kind)
	{
	case InputReportKind::MouseWheel:
	{
		const int delta = input.WheelDelta;

		if (!this->CanHandleMouseWheel(delta, input.X, input.Y))
			return false;

		int steps = delta / InputReport::WheelDeltaUnit;
		if (steps != 0)
		{
			SetScrollOffsetCore(
				_horizontalOffset,
				_verticalOffset - (steps * _mouseWheelStep), true);
		}
		auto eventArgs = input.CreateMouseEventArgs();
		this->OnMouseWheel(this, eventArgs);
		return true;
	}
	case InputReportKind::PointerDown:
	{
		if (input.ChangedButton != MouseButton::Left) break;
		if (HitVerticalScrollBar(input.X, input.Y, layout)
			&& layout.ContentHeight > layout.ViewportHeight)
		{
			float thumbH = static_cast<float>(
				(static_cast<double>(layout.ViewportHeight)
					* static_cast<double>(layout.ViewportHeight))
				/ layout.ContentHeight);
			float minThumbH = std::max(16.0f, layout.ViewportHeight * 0.1f);
			thumbH = std::clamp(thumbH, minThumbH, layout.ViewportHeight);
			float moveSpace = std::max(0.0f, layout.ViewportHeight - thumbH);
			float scrollRatio = layout.MaxScrollY > 0.0
				? static_cast<float>((std::clamp)(
					_verticalOffset / layout.MaxScrollY, 0.0, 1.0)) : 0.0f;
			float thumbTop = scrollRatio * moveSpace;
			float pointerY = (float)input.Y;
			bool hitThumb = pointerY >= thumbTop && pointerY <= (thumbTop + thumbH);
			const float grabOffset = hitThumb
				? (pointerY - thumbTop) : (thumbH * 0.5f);
			const ControlWeakReference scrollLifetime(this);
			const bool requiresCapture = GetPresentationWindow() != nullptr;
			if (requiresCapture && !CaptureMouse()) return true;
			auto* live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || (requiresCapture && !live->IsMouseCaptured())) return true;
			live->_verticalScrollThumbGrabOffset = grabOffset;
			live->_draggingVerticalScrollBar = true;
			auto currentLayout = live->CalcScrollLayout();
			live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || !live->_draggingVerticalScrollBar
				|| (requiresCapture && !live->IsMouseCaptured())) return true;
			live->ClampScrollOffsets(currentLayout);
			live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || !live->_draggingVerticalScrollBar
				|| (requiresCapture && !live->IsMouseCaptured())) return true;
			live->UpdateVerticalScrollByThumb(pointerY, currentLayout);
			return true;
		}
		if (HitHorizontalScrollBar(input.X, input.Y, layout)
			&& layout.ContentWidth > layout.ViewportWidth)
		{
			float thumbW = static_cast<float>(
				(static_cast<double>(layout.ViewportWidth)
					* static_cast<double>(layout.ViewportWidth))
				/ layout.ContentWidth);
			float minThumbW = std::max(16.0f, layout.ViewportWidth * 0.1f);
			thumbW = std::clamp(thumbW, minThumbW, layout.ViewportWidth);
			float moveSpace = std::max(0.0f, layout.ViewportWidth - thumbW);
			float scrollRatio = layout.MaxScrollX > 0.0
				? static_cast<float>((std::clamp)(
					_horizontalOffset / layout.MaxScrollX, 0.0, 1.0)) : 0.0f;
			float thumbLeft = scrollRatio * moveSpace;
			float pointerX = (float)input.X;
			bool hitThumb = pointerX >= thumbLeft && pointerX <= (thumbLeft + thumbW);
			const float grabOffset = hitThumb
				? (pointerX - thumbLeft) : (thumbW * 0.5f);
			const ControlWeakReference scrollLifetime(this);
			const bool requiresCapture = GetPresentationWindow() != nullptr;
			if (requiresCapture && !CaptureMouse()) return true;
			auto* live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || (requiresCapture && !live->IsMouseCaptured())) return true;
			live->_horizontalScrollThumbGrabOffset = grabOffset;
			live->_draggingHorizontalScrollBar = true;
			auto currentLayout = live->CalcScrollLayout();
			live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || !live->_draggingHorizontalScrollBar
				|| (requiresCapture && !live->IsMouseCaptured())) return true;
			live->ClampScrollOffsets(currentLayout);
			live = dynamic_cast<ScrollViewer*>(scrollLifetime.Get());
			if (!live || !live->_draggingHorizontalScrollBar
				|| (requiresCapture && !live->IsMouseCaptured())) return true;
			live->UpdateHorizontalScrollByThumb(pointerX, currentLayout);
			return true;
		}
	}
	break;
	case InputReportKind::KeyDown:
	{
		const bool ctrlDown = input.HasModifier(ModifierKeys::Control);
		const int lineStepY = std::max(16, _mouseWheelStep);
		const int lineStepX = std::max(16, _mouseWheelStep);
		const int pageStepY = std::max(16, (int)layout.ViewportHeight - lineStepY);
		const int pageStepX = std::max(16, (int)layout.ViewportWidth - lineStepX);
		bool handledScroll = false;

		switch (input.Key)
		{
		case Key::Up:
			if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, _verticalOffset - lineStepY, true);
				handledScroll = true;
			}
			break;
		case Key::Down:
			if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, _verticalOffset + lineStepY, true);
				handledScroll = true;
			}
			break;
		case Key::Left:
			if (layout.MaxScrollX > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset - lineStepX, _verticalOffset, true);
				handledScroll = true;
			}
			break;
		case Key::Right:
			if (layout.MaxScrollX > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset + lineStepX, _verticalOffset, true);
				handledScroll = true;
			}
			break;
		case Key::PageUp:
			if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, _verticalOffset - pageStepY, true);
				handledScroll = true;
			}
			break;
		case Key::PageDown:
			if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, _verticalOffset + pageStepY, true);
				handledScroll = true;
			}
			break;
		case Key::Home:
			if (ctrlDown)
			{
				if (layout.MaxScrollX > 0.0f || layout.MaxScrollY > 0.0f)
				{
					SetScrollOffsetCore(0.0, 0.0, true);
					handledScroll = true;
				}
			}
			else if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, 0.0, true);
				handledScroll = true;
			}
			else if (layout.MaxScrollX > 0.0f)
			{
				SetScrollOffsetCore(0.0, _verticalOffset, true);
				handledScroll = true;
			}
			break;
		case Key::End:
			if (ctrlDown)
			{
				if (layout.MaxScrollX > 0.0f || layout.MaxScrollY > 0.0f)
				{
					SetScrollOffsetCore(layout.MaxScrollX, layout.MaxScrollY, true);
					handledScroll = true;
				}
			}
			else if (layout.MaxScrollY > 0.0f)
			{
				SetScrollOffsetCore(_horizontalOffset, layout.MaxScrollY, true);
				handledScroll = true;
			}
			else if (layout.MaxScrollX > 0.0f)
			{
				SetScrollOffsetCore(layout.MaxScrollX, _verticalOffset, true);
				handledScroll = true;
			}
			break;
		}

		if (handledScroll)
		{
			auto eventArgs = input.CreateKeyEventArgs();
			this->OnKeyDown(this, eventArgs);
			return true;
		}
	}
	break;
	}

	return Control::ProcessInput(input);
}
