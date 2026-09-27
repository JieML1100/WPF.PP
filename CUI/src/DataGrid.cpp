#define NOMINMAX
#include "DataGrid.h"

#include "AutomationPeer.h"
#include "Border.h"
#include "ContentPresenter.h"
#include "DependencyPropertyInfrastructure.h"
#include "EventInfrastructure.h"
#include "Label.h"
#include "RichTextClipboard.h"
#include "ScrollViewer.h"
#include "StyleInfrastructure.h"
#include "TemplateInfrastructure.h"
#include "TreeInfrastructure.h"
#include "Window.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	template<typename TCallback>
	class ScopeExit final
	{
	public:
		explicit ScopeExit(TCallback callback)
			: _callback(std::move(callback)) {}
		ScopeExit(const ScopeExit&) = delete;
		ScopeExit& operator=(const ScopeExit&) = delete;
		~ScopeExit() { _callback(); }

	private:
		TCallback _callback;
	};

	template<typename TCallback>
	auto MakeScopeExit(TCallback&& callback)
	{
		return ScopeExit<std::decay_t<TCallback>>(
			std::forward<TCallback>(callback));
	}

	class DataGridDisplayCheckBox final : public CheckBox
	{
	public:
		bool ParticipatesInInputHitTesting() const noexcept override
		{
			return false;
		}
	};

	class DataGridDisplayComboBox final : public ComboBox
	{
	public:
		bool ParticipatesInInputHitTesting() const noexcept override
		{
			return false;
		}
	};

	class DataGridColumnReorderIndicator final : public Panel
	{
	public:
		bool ParticipatesInInputHitTesting() const noexcept override
		{
			return false;
		}
	};

	class DefaultDataGridRowValidationErrorTemplate final
		: public IControlTemplate
	{
		std::unique_ptr<Control> BuildVisual(Control* owner) const
		{
			auto root = std::make_unique<Border>();
			root->SetWidth(cui::layout::Length::Fixed(16.0f));
			root->SetHeight(cui::layout::Length::Fixed(16.0f));
			root->SetBackground(cui::drawing::MakeSolidColorBrush(
				D2D1_COLOR_F{ 0.78f, 0.08f, 0.10f, 1.0f }));
			root->SetCornerRadius(CornerRadius(8.0f));

			auto label = std::make_unique<Label>();
			label->SetText(L"!");
			label->SetFontSize(11.0);
			label->SetForeground(cui::drawing::MakeSolidColorBrush(
				D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 1.0f }));
			label->SetTextAlignment(TextAlignment::Center);
			label->SetHorizontalAlignment(HorizontalAlignment::Stretch);
			label->SetVerticalAlignment(VerticalAlignment::Center);
			if (owner)
			{
				cui::framework::TreeAccess::SetTemplatedParent(*root, owner);
				cui::framework::TreeAccess::SetTemplatedParent(*label, owner);
			}
			root->SetChild(std::move(label));
			return root;
		}

	public:
		UIClass TargetType() const noexcept override
		{
			return UIClass::UI_Control;
		}

		bool Apply(Control& owner, std::wstring* outError) const override
		{
			cui::framework::TemplateAccess::SetTemplateRoot(
				owner, BuildVisual(&owner));
			if (outError) outError->clear();
			return true;
		}

		std::unique_ptr<Control> Build(
			std::wstring* outError = nullptr) const override
		{
			if (outError) outError->clear();
			return BuildVisual(nullptr);
		}
	};

	const ControlTemplateReference&
		DefaultDataGridRowValidationErrorTemplateReference()
	{
		static const ControlTemplateReference value(
			std::make_shared<DefaultDataGridRowValidationErrorTemplate>());
		return value;
	}

	constexpr double DataGridColumnHeaderGripperWidth = 8.0;
	constexpr double DataGridRowHeaderGripperHeight = 8.0;
	constexpr double DataGridColumnHeaderDragThreshold = 4.0;
	constexpr double DataGridColumnDropIndicatorWidth = 2.0;

	GridLength SparseTrackLength(double value) noexcept
	{
		if (!std::isfinite(value) || value <= 0.0)
			return GridLength::Pixels(0.0f);
		return GridLength::Pixels(static_cast<float>((std::min)(value,
			static_cast<double>((std::numeric_limits<float>::max)()))));
	}

	double RenderSpaceX(const Control& control, int localX) noexcept
	{
		const auto raw = control.GetLocalToRenderTransform();
		return D2D1::Matrix3x2F(
			raw._11, raw._12, raw._21, raw._22, raw._31, raw._32)
			.TransformPoint(D2D1::Point2F(static_cast<float>(localX), 0.0f)).x;
	}

	double RenderSpaceY(const Control& control, int localY) noexcept
	{
		const auto raw = control.GetLocalToRenderTransform();
		return D2D1::Matrix3x2F(
			raw._11, raw._12, raw._21, raw._22, raw._31, raw._32)
			.TransformPoint(D2D1::Point2F(0.0f, static_cast<float>(localY))).y;
	}

	D2D1_POINT_2F ToRenderSpace(
		const Control& control, float localX, float localY) noexcept
	{
		const auto raw = control.GetLocalToRenderTransform();
		return D2D1::Matrix3x2F(
			raw._11, raw._12, raw._21, raw._22, raw._31, raw._32)
			.TransformPoint(D2D1::Point2F(localX, localY));
	}

	bool IsPointInside(
		const D2D1_RECT_F& rect, D2D1_POINT_2F point) noexcept
	{
		return point.x >= rect.left && point.x < rect.right
			&& point.y >= rect.top && point.y < rect.bottom;
	}

	bool PublishAccessibilityBounds(
		Control* control, AccessibilityVirtualNode& node)
	{
		if (!control)
		{
			node.BoundsDip = {};
			node.BoundsAreRenderSpace = true;
			node.Visible = false;
			return false;
		}
		node.BoundsDip = control->GetRenderedAbsoluteRectDip();
		node.BoundsAreRenderSpace = true;
		const float width = node.BoundsDip.right - node.BoundsDip.left;
		const float height = node.BoundsDip.bottom - node.BoundsDip.top;
		if (!control->GetIsVisible() || width <= 0.0f || height <= 0.0f)
		{
			node.Visible = false;
			return false;
		}
		const auto center = D2D1::Point2F(
			(node.BoundsDip.left + node.BoundsDip.right) * 0.5f,
			(node.BoundsDip.top + node.BoundsDip.bottom) * 0.5f);
		const auto topLeft = D2D1::Point2F(
			node.BoundsDip.left + 0.5f, node.BoundsDip.top + 0.5f);
		const auto bottomRight = D2D1::Point2F(
			node.BoundsDip.right - 0.5f, node.BoundsDip.bottom - 0.5f);
		node.Visible = control->IsRenderPointInsideClip(center)
			|| control->IsRenderPointInsideClip(topLeft)
			|| control->IsRenderPointInsideClip(bottomRight);
		return node.Visible;
	}

	const ItemsPanelTemplateReference& DefaultDataGridRowsPanel()
	{
		static const auto definition = []
		{
			auto value = std::make_shared<ItemsPanelTemplate>();
			value->Kind = ItemsPanelKind::VirtualizingStack;
			value->Orientation = Orientation::Vertical;
			value->ItemHeight = 38.0f;
			value->CacheLength = 1.0f;
			return ItemsPanelTemplateReference(std::move(value));
		}();
		return definition;
	}

	std::wstring_view Trim(std::wstring_view value) noexcept
	{
		while (!value.empty() && std::iswspace(value.front()))
			value.remove_prefix(1);
		while (!value.empty() && std::iswspace(value.back()))
			value.remove_suffix(1);
		return value;
	}

	bool EqualsIgnoreCase(
		std::wstring_view left, std::wstring_view right) noexcept
	{
		if (left.size() != right.size()) return false;
		for (size_t index = 0; index < left.size(); ++index)
		{
			if (std::towlower(left[index]) != std::towlower(right[index]))
				return false;
		}
		return true;
	}

	std::optional<cui::drawing::Brush> ConvertBrush(
		const BindingValue& value)
	{
		cui::drawing::Brush brush;
		if (value.TryGet(brush)) return brush;
		D2D1_COLOR_F color{};
		if (value.TryGet(color))
			return cui::drawing::MakeSolidColorBrush(color);
		return std::nullopt;
	}

	template<typename TValue>
	DependencyPropertyOptions<DataGrid, TValue> DataGridOptions(
		TValue defaultValue,
		DependencyPropertyFlags flags = DependencyPropertyFlags::None)
	{
		DependencyPropertyOptions<DataGrid, TValue> options;
		options.DefaultValue = std::move(defaultValue);
		options.Flags = flags;
		return options;
	}

	bool HasColumnHeaders(DataGridHeadersVisibility value) noexcept
	{
		return (static_cast<int>(value)
			& static_cast<int>(DataGridHeadersVisibility::Column)) != 0;
	}

	bool HasRowHeaders(DataGridHeadersVisibility value) noexcept
	{
		return (static_cast<int>(value)
			& static_cast<int>(DataGridHeadersVisibility::Row)) != 0;
	}

	const cui::drawing::Transform& HorizontalTranslation(double offset)
	{
		thread_local cui::drawing::Transform transform = []
		{
			cui::drawing::Transform value;
			cui::drawing::TransformOperation operation;
			operation.Kind = cui::drawing::TransformKind::Translate;
			value.Operations.push_back(operation);
			return value;
		}();
		transform.Operations.front().X = static_cast<float>(offset);
		return transform;
	}

	void ApplyFrozenColumnVisual(
		Control& visual, bool frozen, double offset,
		double frozenWidth, double logicalLeft, double logicalRight)
	{
		if (frozen)
		{
			visual.SetZIndex(1);
			visual.ClearClip();
			visual.SetRenderTransform(HorizontalTranslation(offset));
			return;
		}

		visual.SetZIndex(0);
		visual.ClearRenderTransform();
		const double width = (std::max)(0.0, logicalRight - logicalLeft);
		const double hiddenWidth = frozenWidth + offset - logicalLeft;
		if (frozenWidth <= 0.0001 || hiddenWidth <= 0.0001)
		{
			visual.ClearClip();
			return;
		}
		cui::drawing::Geometry clip;
		clip.Kind = cui::drawing::GeometryKind::Rectangle;
		if (hiddenWidth >= width - 0.0001)
			clip.Rect = D2D1::RectF();
		else
		{
			constexpr float verticalLimit = 1000000.0f;
			clip.Rect = D2D1::RectF(
				static_cast<float>((std::clamp)(hiddenWidth, 0.0, width)),
				-verticalLimit,
				static_cast<float>(width), verticalLimit);
		}
		visual.SetClip(clip);
	}

	bool HasHorizontalGridLines(DataGridGridLinesVisibility value) noexcept
	{
		return value == DataGridGridLinesVisibility::All
			|| value == DataGridGridLinesVisibility::Horizontal;
	}

	bool HasVerticalGridLines(DataGridGridLinesVisibility value) noexcept
	{
		return value == DataGridGridLinesVisibility::All
			|| value == DataGridGridLinesVisibility::Vertical;
	}

	bool HasSortPath(const CollectionSortDescription& description) noexcept
	{
#if CUI_ENABLE_DYNAMIC_XAML
		if (!description.PropertyName.empty()) return true;
#endif
		return !description.CompiledPath.Empty();
	}

	bool SameSortPath(
		const CollectionSortDescription& left,
		const CollectionSortDescription& right) noexcept
	{
#if CUI_ENABLE_DYNAMIC_XAML
		if (!left.PropertyName.empty() || !right.PropertyName.empty())
			return left.PropertyName == right.PropertyName;
#endif
		return SameCompiledCollectionPath(
			left.CompiledPath, right.CompiledPath);
	}

	bool SameCellIdentity(
		const DataGridCellInfo& left,
		const DataGridCellInfo& right) noexcept
	{
		return left == right;
	}

	bool SameCellItemAndColumn(
		const DataGridCellInfo& left,
		const DataGridCellInfo& right) noexcept
	{
		return SameCellIdentity(left, right);
	}

	bool SameCellCollection(
		const DataGridSelectedCellCollection& left,
		const DataGridSelectedCellCollection& right,
		bool ignoreLocators)
	{
		(void)ignoreLocators;
		return left.SameLogicalCells(right);
	}

	bool ContainsCellIdentity(
		const std::vector<DataGridCellInfo>& cells,
		const DataGridCellInfo& candidate) noexcept
	{
		return std::any_of(cells.begin(), cells.end(),
			[&candidate](const DataGridCellInfo& value)
			{ return SameCellIdentity(value, candidate); });
	}

	bool ContainsCellIdentity(
		const DataGridSelectedCellCollection& cells,
		const DataGridCellInfo& candidate) noexcept
	{
		return cells.Contains(candidate);
	}

	bool IsRoutedDescendantOrSelf(
		Control* element, Control* ancestor) noexcept
	{
		if (!element || !ancestor) return false;
		for (auto* current = element; current;
			current = current->GetRoutedParent())
		{
			if (current == ancestor) return true;
		}
		return false;
	}

	bool EditingElementOwnsKey(
		Key key, ModifierKeys modifiers) noexcept
	{
		if (key == Key::A
			&& HasModifier(modifiers, ModifierKeys::Control)) return true;
		return key == Key::Left || key == Key::Right
			|| key == Key::Up || key == Key::Down
			|| key == Key::Home || key == Key::End
			|| key == Key::Prior || key == Key::Next;
	}

	size_t RawOffsetForSelectedOrdinal(
		size_t selectedIndex,
		size_t regionSize,
		const std::vector<size_t>& excluded) noexcept
	{
		if (excluded.empty()) return selectedIndex;
		// The raw offset can move right by at most the number of exclusions.
		// Find the first prefix containing selectedIndex + 1 live cells so a
		// dense sparse-exclusion set remains O(log E) per indexed cell.
		size_t low = selectedIndex;
		const size_t maximumShift = (std::min)(
			excluded.size(), regionSize - 1 - selectedIndex);
		size_t high = selectedIndex + maximumShift;
		while (low < high)
		{
			const size_t middle = low + (high - low) / 2;
			const size_t excludedThrough = static_cast<size_t>(
				std::upper_bound(excluded.begin(), excluded.end(), middle)
					- excluded.begin());
			const size_t selectedThrough = middle + 1 - excludedThrough;
			if (selectedThrough > selectedIndex) high = middle;
			else low = middle + 1;
		}
		return low;
	}

	template<typename TInterval>
	size_t IntervalEndExclusive(const TInterval& interval) noexcept
	{
		return interval.Count > (std::numeric_limits<size_t>::max)()
			- interval.Start
			? (std::numeric_limits<size_t>::max)()
			: interval.Start + interval.Count;
	}

	template<typename TInterval>
	bool IsRowInIntervals(
		const std::vector<TInterval>& intervals, size_t row) noexcept
	{
		const auto after = std::upper_bound(
			intervals.begin(), intervals.end(), row,
			[](size_t value, const TInterval& interval)
			{ return value < interval.Start; });
		if (after == intervals.begin()) return false;
		const auto& interval = *std::prev(after);
		return row >= interval.Start
			&& row - interval.Start < interval.Count;
	}

	size_t CountIntervalRowsBefore(
		const std::vector<DataGridSelectedCellCollection::
			RegionExcludedOffsets::RowInterval>& intervals,
		size_t rawOffset) noexcept
	{
		if (rawOffset == 0 || intervals.empty()) return 0;
		const auto after = std::lower_bound(
			intervals.begin(), intervals.end(), rawOffset,
			[](const auto& interval, size_t value)
			{ return interval.Start < value; });
		if (after == intervals.begin()) return 0;
		const auto& interval = *std::prev(after);
		const size_t coveredInInterval = rawOffset <= interval.Start
			? 0 : (std::min)(interval.Count, rawOffset - interval.Start);
		return interval.ExcludedBefore + coveredInInterval;
	}

	size_t CountExcludedRowsBefore(
		const DataGridSelectedCellCollection::RegionExcludedOffsets& excluded,
		size_t rawOffset) noexcept
	{
		return CountIntervalRowsBefore(excluded.RowIntervals, rawOffset)
			+ static_cast<size_t>(std::lower_bound(
				excluded.Rows.begin(), excluded.Rows.end(), rawOffset)
				- excluded.Rows.begin());
	}

	bool IsProjectedRowExcluded(
		const DataGridSelectedCellCollection::RegionExcludedOffsets& excluded,
		size_t rawOffset) noexcept
	{
		return IsRowInIntervals(excluded.RowIntervals, rawOffset)
			|| std::binary_search(
				excluded.Rows.begin(), excluded.Rows.end(), rawOffset);
	}

	size_t RawRowOffsetForSelectedOrdinal(
		size_t selectedIndex, size_t projectedRows,
		const DataGridSelectedCellCollection::RegionExcludedOffsets& excluded)
		noexcept
	{
		const size_t intervalRows = excluded.RowIntervals.empty()
			? 0
			: excluded.RowIntervals.back().ExcludedBefore
				+ excluded.RowIntervals.back().Count;
		const size_t totalExcluded = intervalRows + excluded.Rows.size();
		if (totalExcluded == 0) return selectedIndex;
		size_t low = selectedIndex;
		const size_t maximumShift = (std::min)(
			totalExcluded, projectedRows - 1 - selectedIndex);
		size_t high = selectedIndex + maximumShift;
		while (low < high)
		{
			const size_t middle = low + (high - low) / 2;
			const size_t selectedThrough = middle + 1
				- CountExcludedRowsBefore(excluded, middle + 1);
			if (selectedThrough > selectedIndex) high = middle;
			else low = middle + 1;
		}
		return low;
	}

	const DependencyPropertyMetadataRegistration&
		DataGridCellFocusableMetadataRelation()
	{
		static const DependencyPropertyMetadataRegistration relation = []
		{
			const auto& property = Control::FocusableProperty();
			DependencyPropertyOptions<DataGridCell, bool> options;
			options.DefaultValue = true;
			CUI_DESIGN_METADATA_ONLY(
			const std::type_index ownerTypes[] = {
				std::type_index(typeid(Control))
			};
			const auto* base = DependencyPropertyRegistry::FindRegistered(
				ownerTypes, L"Focusable");
			if (!base)
				throw std::logic_error(
					"Control.Focusable must be registered before DataGridCell");
			options.Design = base->Design();
			)
			return DependencyPropertyRegistry::OverrideMetadataStatic<
				DataGridCell, ContentControl, bool>(
					property, std::move(options));
		}();
		return relation;
	}

	const DependencyPropertyMetadataRegistration&
		DataGridRowHeaderFocusableMetadataRelation()
	{
		static const DependencyPropertyMetadataRegistration relation = []
		{
			const auto& property = Control::FocusableProperty();
			DependencyPropertyOptions<DataGridRowHeader, bool> options;
			options.DefaultValue = false;
			CUI_DESIGN_METADATA_ONLY(
			const std::type_index ownerTypes[] = {
				std::type_index(typeid(Control))
			};
			const auto* base = DependencyPropertyRegistry::FindRegistered(
				ownerTypes, L"Focusable");
			if (!base)
				throw std::logic_error(
					"Control.Focusable must be registered before DataGridRowHeader");
			options.Design = base->Design();
			)
			return DependencyPropertyRegistry::OverrideMetadataStatic<
				DataGridRowHeader, Button, bool>(
					property, std::move(options));
		}();
		return relation;
	}

	const DependencyPropertyMetadataRegistration&
		DataGridRowHeaderClickModeMetadataRelation()
	{
		static const DependencyPropertyMetadataRegistration relation = []
		{
			const auto& property = ButtonBase::ClickModeProperty();
			DependencyPropertyOptions<DataGridRowHeader, ::ClickMode> options;
			options.DefaultValue = ::ClickMode::Press;
			CUI_DESIGN_METADATA_ONLY(
			const std::type_index ownerTypes[] = {
				std::type_index(typeid(ButtonBase))
			};
			const auto* base = DependencyPropertyRegistry::FindRegistered(
				ownerTypes, L"ClickMode");
			if (!base)
				throw std::logic_error(
					"ButtonBase.ClickMode must be registered before DataGridRowHeader");
			options.Design = base->Design();
			)
			return DependencyPropertyRegistry::OverrideMetadataStatic<
				DataGridRowHeader, Button, ::ClickMode>(
					property, std::move(options));
		}();
		return relation;
	}

}

/**
 * DataGrid-only display projection which appends one synthetic new-item row.
 * The placeholder is never inserted into the application list, and the adapter
 * delegates every data occurrence token so selection/UIA identity remains the
 * same as the underlying CollectionViewSource. Its immutable snapshot adds only
 * one strong record reference, independent of the source row count.
 */
struct DataGrid::DataGridItemsView final
	: public IBindingList,
	  public IBindingListOccurrenceIdentity,
	  public IBindingListOccurrenceLookup,
	  public IBindingListSnapshotProvider,
	  public IBindingListGroupView,
	  public IBindingListCurrentView,
	  public std::enable_shared_from_this<DataGridItemsView>
{
	struct CallbackLifetime final
	{
		DataGridItemsView* Owner = nullptr;
	};

	class StableSnapshot final
		: public IBindingList,
		  public IBindingListOccurrenceIdentity,
		  public IBindingListOccurrenceLookup,
		  public IBindingListStableSnapshot
	{
	public:
		StableSnapshot(
			BindingListReference inner,
			BindingSourceReference placeholder,
			bool placeholderVisible,
			size_t placeholderOccurrence) noexcept
			: _inner(std::move(inner)),
			  _placeholder(std::move(placeholder)),
			  _placeholderVisible(placeholderVisible),
			  _placeholderOccurrence(placeholderOccurrence) {}

		size_t Count() const noexcept override
		{
			return (_inner ? _inner.Get()->Count() : 0)
				+ (_placeholderVisible ? 1 : 0);
		}
		bool TryGetItem(
			size_t index, BindingSourceReference& out) const override
		{
			out = {};
			const size_t innerCount = _inner ? _inner.Get()->Count() : 0;
			if (index < innerCount)
				return _inner.Get()->TryGetItem(index, out);
			if (!_placeholderVisible || index != innerCount) return false;
			out = _placeholder;
			return static_cast<bool>(out);
		}
		EventConnection SubscribeChanged(ChangedHandler) override { return {}; }
		DataTypeToken GetItemTypeToken() const noexcept override
		{
			return _inner
				? _inner.Get()->GetItemTypeToken() : DataTypeToken{};
		}
#if CUI_ENABLE_DYNAMIC_XAML
		const std::wstring& ItemTypeName() const noexcept override
		{
			static const std::wstring empty;
			return _inner ? _inner.Get()->ItemTypeName() : empty;
		}
#endif
		bool TryGetItemOccurrenceIdentity(
			size_t index, size_t& result) const noexcept override
		{
			result = 0;
			const size_t innerCount = _inner ? _inner.Get()->Count() : 0;
			if (_placeholderVisible && index == innerCount)
			{
				result = _placeholderOccurrence;
				return result != 0
					&& result != DataGridCellInfo::InvalidIndex;
			}
			if (index >= innerCount) return false;
			const auto* identities = dynamic_cast<
				const IBindingListOccurrenceIdentity*>(_inner.Get());
			return identities
				&& identities->TryGetItemOccurrenceIdentity(index, result);
		}
		bool TryGetItemIndexByOccurrenceIdentity(
			size_t identity, size_t& index) const noexcept override
		{
			index = 0;
			const size_t innerCount = _inner ? _inner.Get()->Count() : 0;
			if (_placeholderVisible && identity == _placeholderOccurrence)
			{
				index = innerCount;
				return true;
			}
			const auto* lookup = _inner ? dynamic_cast<
				const IBindingListOccurrenceLookup*>(_inner.Get()) : nullptr;
			return lookup
				&& lookup->TryGetItemIndexByOccurrenceIdentity(identity, index)
				&& index < innerCount;
		}
		bool IsItemIndexByOccurrenceIdentityLookupBounded()
			const noexcept override
		{
			const auto* lookup = _inner ? dynamic_cast<
				const IBindingListOccurrenceLookup*>(_inner.Get()) : nullptr;
			return lookup
				&& lookup->IsItemIndexByOccurrenceIdentityLookupBounded();
		}

	private:
		const BindingListReference _inner;
		const BindingSourceReference _placeholder;
		const bool _placeholderVisible = false;
		const size_t _placeholderOccurrence = 0;
	};

	static std::shared_ptr<DataGridItemsView> Create(
		std::shared_ptr<CollectionViewSource> inner,
		BindingListReference groupSource = {})
	{
		auto result = std::shared_ptr<DataGridItemsView>(
			new DataGridItemsView(
				std::move(inner), std::move(groupSource)));
		result->Attach();
		return result;
	}

	~DataGridItemsView() override { Detach(); }

	size_t Count() const noexcept override
	{
		return _publishedInnerCount + (_placeholderVisible ? 1 : 0);
	}
	bool TryGetItem(size_t index, BindingSourceReference& out) const override
	{
		out = {};
		const size_t innerCount = _publishedInnerCount;
		if (index < innerCount) return _inner->TryGetItem(index, out);
		if (!_placeholderVisible || index != innerCount) return false;
		out = _placeholder;
		return true;
	}
	EventConnection SubscribeChanged(ChangedHandler handler) override
	{
		if (!handler) return {};
		return _changed.Subscribe(
			[handler = std::move(handler)](
				DataGridItemsView*, const CollectionChangedEventArgs& change)
			{ handler(change); });
	}
	const std::vector<BindingListGroup>& Groups() const noexcept override
	{
		static const std::vector<BindingListGroup> empty;
		const auto* grouped = _groupSource ? dynamic_cast<
			const IBindingListGroupView*>(_groupSource.Get()) : nullptr;
		return grouped ? grouped->Groups()
			: _inner ? _inner->Groups() : empty;
	}
	EventConnection SubscribeGroupsChanged(
		GroupsChangedHandler handler) override
	{
		if (!handler) return {};
		if (auto* grouped = _groupSource ? dynamic_cast<
			IBindingListGroupView*>(_groupSource.Get()) : nullptr)
			return grouped->SubscribeGroupsChanged(std::move(handler));
		return _inner
			? _inner->SubscribeGroupsChanged(std::move(handler))
			: EventConnection{};
	}
	DataTypeToken GetItemTypeToken() const noexcept override
	{
		return _inner ? _inner->GetItemTypeToken() : DataTypeToken{};
	}
#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring& ItemTypeName() const noexcept override
	{
		static const std::wstring empty;
		return _inner ? _inner->ItemTypeName() : empty;
	}
#endif
	bool TryGetStableSnapshot(BindingListReference& result) const override
	{
		result = {};
		const auto* snapshots = _inner ? dynamic_cast<
			const IBindingListSnapshotProvider*>(_inner.get()) : nullptr;
		BindingListReference innerSnapshot;
		if (!snapshots || !snapshots->TryGetStableSnapshot(innerSnapshot)
			|| !innerSnapshot
			|| !dynamic_cast<const IBindingListStableSnapshot*>(
				innerSnapshot.Get())) return false;
		result = BindingListReference(std::make_shared<StableSnapshot>(
			std::move(innerSnapshot), _placeholder, _placeholderVisible,
			_placeholderOccurrence));
		return true;
	}
	bool TryGetItemOccurrenceIdentity(
		size_t index, size_t& result) const noexcept override
	{
		result = 0;
		if (_placeholderVisible && index == _publishedInnerCount)
		{
			result = _placeholderOccurrence;
			return result != 0
				&& result != DataGridCellInfo::InvalidIndex;
		}
		if (!_inner || index >= _publishedInnerCount) return false;
		return _inner->TryGetItemOccurrenceIdentity(index, result);
	}
	bool TryGetItemIndexByOccurrenceIdentity(
		size_t identity, size_t& index) const noexcept override
	{
		index = 0;
		if (_placeholderVisible && identity == _placeholderOccurrence)
		{
			index = _publishedInnerCount;
			return true;
		}
		return _inner
			&& _inner->TryGetItemIndexByOccurrenceIdentity(identity, index)
			&& index < _publishedInnerCount;
	}
	bool IsItemIndexByOccurrenceIdentityLookupBounded()
		const noexcept override
	{
		return _inner
			&& _inner->IsItemIndexByOccurrenceIdentityLookupBounded();
	}
	int CurrentPosition() const noexcept override
	{
		return _inner ? _inner->CurrentPosition() : -1;
	}
	BindingSourceReference CurrentItem() const noexcept override
	{
		return _inner ? _inner->CurrentItem() : BindingSourceReference{};
	}
	bool MoveCurrentToPosition(int position) override
	{
		return _inner && position >= -1
			&& (position < 0
				|| static_cast<size_t>(position) < _publishedInnerCount)
			&& _inner->MoveCurrentToPosition(position);
	}
	EventConnection SubscribeCurrentChanged(
		CurrentChangedHandler handler) override
	{
		return _inner && handler
			? _inner->SubscribeCurrentChanged(std::move(handler))
			: EventConnection{};
	}

	const BindingSourceReference& Placeholder() const noexcept
	{
		return _placeholder;
	}
	bool PlaceholderVisible() const noexcept { return _placeholderVisible; }
	void SetPlaceholderVisible(bool value)
	{
		_placeholderRequested = value;
		const auto* editable = _inner ? dynamic_cast<
			const IEditableBindingList*>(_inner.get()) : nullptr;
		value = value && (!editable || !editable->IsAddingNew());
		if (_placeholderVisible == value || !_inner) return;
		if (value) EnsurePlaceholderOccurrenceIdentity();
		const size_t innerCount = _publishedInnerCount;
		const size_t oldSize = innerCount + (_placeholderVisible ? 1 : 0);
		_placeholderVisible = value;
		const CollectionChangedEventArgs change{
			value ? CollectionChangeAction::Add : CollectionChangeAction::Remove,
			value ? CollectionChangedEventArgs::Npos : innerCount,
			value ? innerCount : CollectionChangedEventArgs::Npos,
			value ? 0u : 1u,
			value ? 1u : 0u,
			oldSize,
			innerCount + (value ? 1u : 0u) };
		cui::framework::EventAccess::Raise(_changed, this, change);
	}
	void Detach() noexcept
	{
		if (_callbackLifetime) _callbackLifetime->Owner = nullptr;
		_innerChanged.Disconnect();
		_callbackLifetime.reset();
	}

private:
	explicit DataGridItemsView(
		std::shared_ptr<CollectionViewSource> inner,
		BindingListReference groupSource)
		: _inner(std::move(inner)),
		  _groupSource(std::move(groupSource)),
		  _placeholder(BindingSourceReference(
			  std::make_shared<ObservableObject>())),
		  _publishedInnerCount(_inner ? _inner->Count() : 0) {}

	void Attach()
	{
		_callbackLifetime = std::make_shared<CallbackLifetime>();
		_callbackLifetime->Owner = this;
		const std::weak_ptr<CallbackLifetime> weakLifetime = _callbackLifetime;
		if (_inner)
			_innerChanged = _inner->SubscribeChanged(
				[weakLifetime](const CollectionChangedEventArgs& change)
				{
					const auto lifetime = weakLifetime.lock();
					if (lifetime && lifetime->Owner)
						lifetime->Owner->OnInnerChanged(change);
				});
	}
	void OnInnerChanged(const CollectionChangedEventArgs& change)
	{
		const bool oldPlaceholderVisible = _placeholderVisible;
		const size_t oldPlaceholderOccurrence = _placeholderOccurrence;
		// An externally initiated AddNew can arrive without DataGrid first hiding
		// the placeholder. Remember the requested policy so an external CommitNew
		// or CancelNew refresh can restore it without a DataGrid-owned transaction.
		if (const auto* editable = _inner ? dynamic_cast<
			const IEditableBindingList*>(_inner.get()) : nullptr;
			 editable)
			_placeholderVisible = _placeholderRequested
				&& !editable->IsAddingNew();
		else _placeholderVisible = _placeholderRequested;
		if (_placeholderVisible) EnsurePlaceholderOccurrenceIdentity();
		const bool placeholderChanged =
			oldPlaceholderVisible != _placeholderVisible;
		const bool placeholderIdentityChanged = _placeholderVisible
			&& oldPlaceholderOccurrence != _placeholderOccurrence;
		const size_t oldInnerCount = change.OldSize == _publishedInnerCount
			? change.OldSize : _publishedInnerCount;
		const size_t newInnerCount = change.NewSize;
		_publishedInnerCount = newInnerCount;
		if (placeholderChanged || placeholderIdentityChanged)
		{
			const CollectionChangedEventArgs reset{
				CollectionChangeAction::Reset,
				CollectionChangedEventArgs::Npos,
				CollectionChangedEventArgs::Npos,
				oldInnerCount + (oldPlaceholderVisible ? 1u : 0u),
				newInnerCount + (_placeholderVisible ? 1u : 0u),
				oldInnerCount + (oldPlaceholderVisible ? 1u : 0u),
				newInnerCount + (_placeholderVisible ? 1u : 0u) };
			cui::framework::EventAccess::Raise(_changed, this, reset);
			return;
		}
		auto forwarded = change;
		forwarded.OldSize = oldInnerCount
			+ (_placeholderVisible ? 1u : 0u);
		forwarded.NewSize = newInnerCount
			+ (_placeholderVisible ? 1u : 0u);
		cui::framework::EventAccess::Raise(_changed, this, forwarded);
	}

	void EnsurePlaceholderOccurrenceIdentity() noexcept
	{
		if (_placeholderOccurrence == 0
			|| _placeholderOccurrence == DataGridCellInfo::InvalidIndex)
			_placeholderOccurrence = DataGridCellInfo::InvalidIndex - 1;
		const auto* lookup = _inner ? dynamic_cast<
			const IBindingListOccurrenceLookup*>(_inner.get()) : nullptr;
		if (!lookup) return;
		size_t ignored = 0;
		while (lookup->TryGetItemIndexByOccurrenceIdentity(
			_placeholderOccurrence, ignored))
		{
			if (_placeholderOccurrence <= 1)
			{
				_placeholderOccurrence = 0;
				return;
			}
			--_placeholderOccurrence;
		}
	}

	std::shared_ptr<CollectionViewSource> _inner;
	BindingListReference _groupSource;
	BindingSourceReference _placeholder;
	EventConnection _innerChanged;
	std::shared_ptr<CallbackLifetime> _callbackLifetime;
	Event<void(DataGridItemsView*, const CollectionChangedEventArgs&)> _changed;
	size_t _publishedInnerCount = 0;
	bool _placeholderRequested = false;
	bool _placeholderVisible = false;
	size_t _placeholderOccurrence = DataGridCellInfo::InvalidIndex - 1;
};

/**
 * DataGrid rows already own one authoritative resolved-width prefix.  Feeding
 * those fixed widths back through a general Grid made every live row repeat
 * Auto/Star/span discovery and allocate a ColumnDefinition vector on each
 * resize frame.  This internal presenter keeps the real DataGridCell visual
 * children, but projects their known column rectangles in one linear pass.
 */
class DataGridCellsPresenter final : public Panel
{
private:
	struct CellSnapshot final
	{
		ControlWeakReference Lifetime;
		DataGridCell* Identity = nullptr;
		float Left = 0.0f;
		float Width = 0.0f;
	};

	class CellsLayoutEngine final : public LayoutEngine
	{
	public:
		explicit CellsLayoutEngine(DataGridCellsPresenter& owner) noexcept
			: _owner(owner) {}

		cui::core::Size Measure(
			LayoutContext& context,
			const cui::core::Constraints& available) override
		{
			auto* const presenter = &_owner;
			auto* const row = presenter->_row;
			auto* const owner = row ? row->GetDataGridOwner() : nullptr;
			if (!row || !owner || row->_cellsGrid != presenter)
			{
				_needsLayout = false;
				return {};
			}
			const size_t projectionRevision =
				owner->_columnWidthProjectionRevision;
			const ControlWeakReference presenterLifetime(presenter);
			const ControlWeakReference rowLifetime(row);
			const ControlWeakReference ownerLifetime(owner);
			const auto currentPresenter = [&]()
				-> DataGridCellsPresenter*
			{
				auto* livePresenter = dynamic_cast<DataGridCellsPresenter*>(
					presenterLifetime.Get());
				auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
				auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				return livePresenter && liveRow && liveOwner
					&& livePresenter == presenter
					&& livePresenter->_row == liveRow
					&& liveRow == row
					&& liveRow->_cellsGrid == livePresenter
					&& liveRow->GetDataGridOwner() == liveOwner
					&& liveOwner == owner
					&& liveOwner->_columnWidthProjectionRevision
						== projectionRevision
					? livePresenter : nullptr;
			};
			const auto maximum = available.Normalized().maximum;
			// A fixed cells-presenter height is the authoritative vertical measure.
			// It may come from DataGrid.RowHeight or one sparse user row override.
			// Measuring every retained cell here is both unnecessary and expensive:
			// the outer row Grid probes its Auto column once unbounded and once with
			// the row slot, which otherwise alternates every cell template between
			// infinite and finite height on each column-resize frame. Arrange below
			// still measures cells against their final slots, so the resized column
			// updates content while unchanged columns retain their measure caches.
			const auto specifiedHeight = presenter->GetHeight();
			if (!specifiedHeight.IsAuto())
			{
				const double fixedRowHeight = specifiedHeight.value;
				auto* livePresenter = currentPresenter();
				if (!livePresenter) return {};
				const float totalWidth = livePresenter->TotalColumnWidth();
				if (!currentPresenter()) return {};
				const double maximumFloat = static_cast<double>(
					(std::numeric_limits<float>::max)());
				float desiredHeight = static_cast<float>((std::clamp)(
					fixedRowHeight, 0.0, maximumFloat));
				if (std::isfinite(maximum.height))
					desiredHeight = (std::min)(desiredHeight, maximum.height);
				_needsLayout = false;
				return { totalWidth, desiredHeight };
			}
			std::vector<CellSnapshot> localCells;
			const bool sharedScratch = !presenter->_layoutScratchInUse;
			if (sharedScratch)
			{
				presenter->_layoutScratchInUse = true;
				presenter->_layoutScratch.clear();
			}
			auto scratchGuard = MakeScopeExit(
				[presenterLifetime, sharedScratch]
				{
					if (!sharedScratch) return;
					if (auto* live = dynamic_cast<DataGridCellsPresenter*>(
						presenterLifetime.Get()))
					{
						live->_layoutScratch.clear();
						live->_layoutScratchInUse = false;
					}
				});
			auto& cells = sharedScratch
				? presenter->_layoutScratch : localCells;
			const int childCount = context.ChildCount();
			cells.reserve(static_cast<size_t>((std::max)(0, childCount)));
			for (int childIndex = 0; childIndex < childCount; ++childIndex)
			{
				auto* livePresenter = currentPresenter();
				if (!livePresenter) return {};
				auto* cell = dynamic_cast<DataGridCell*>(
					context.ChildAt(childIndex));
				if (!cell || cell->IsCollapsed()) continue;
				CellSnapshot snapshot;
				snapshot.Lifetime = cell;
				snapshot.Identity = cell;
				const bool resolved = livePresenter->TryGetCellRect(
					cell, snapshot.Left, snapshot.Width);
				livePresenter = currentPresenter();
				auto* liveCell = dynamic_cast<DataGridCell*>(
					snapshot.Lifetime.Get());
				if (!livePresenter || liveCell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(liveCell)) return {};
				if (resolved) cells.push_back(std::move(snapshot));
			}
			// An Auto DataGrid row must discover the wrapped cell's natural height.
			// The outer row Grid may offer the previous arranged height while a
			// column is being resized; feeding that finite value back here makes the
			// old height self-lock and prevents a narrower column from growing it.
			const float availableHeight = cui::core::Infinity;
			float desiredHeight = 0.0f;
			for (const auto& snapshot : cells)
			{
				auto* livePresenter = currentPresenter();
				auto* cell = dynamic_cast<DataGridCell*>(
					snapshot.Lifetime.Get());
				if (!livePresenter || cell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(cell)) return {};
				if (cell->IsCollapsed()) continue;
				const Thickness margin = cell->Margin;
				const float childWidth = (std::max)(
					0.0f, snapshot.Width - margin.Left - margin.Right);
				const float childHeight = std::isfinite(availableHeight)
					? (std::max)(0.0f,
						availableHeight - margin.Top - margin.Bottom)
					: cui::core::Infinity;
				const bool widthChanged =
					owner->_columnWidthMeasureDirty.size()
						!= owner->ColumnCount()
					|| cell->_columnIndex
						>= owner->_columnWidthMeasureDirty.size()
					|| owner->_columnWidthMeasureDirty[cell->_columnIndex] != 0;
				const auto desired = widthChanged
					|| cell->GetComputedLayout().NeedsMeasure()
					? cell->Measure(cui::core::Constraints{
						cui::core::Size{ childWidth, childHeight } })
					: cell->GetDesiredSizeDip();
				livePresenter = currentPresenter();
				cell = dynamic_cast<DataGridCell*>(snapshot.Lifetime.Get());
				if (!livePresenter || cell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(cell)) return {};
				desiredHeight = (std::max)(desiredHeight,
					desired.height + margin.Top + margin.Bottom);
			}
			auto* livePresenter = currentPresenter();
			if (!livePresenter) return {};
			const float totalWidth = livePresenter->TotalColumnWidth();
			if (!currentPresenter()) return {};
			_needsLayout = false;
			return { totalWidth, desiredHeight };
		}

		void Arrange(LayoutContext& context, cui::core::Rect finalRect) override
		{
			auto* const presenter = &_owner;
			auto* const row = presenter->_row;
			auto* const owner = row ? row->GetDataGridOwner() : nullptr;
			if (!row || !owner || row->_cellsGrid != presenter)
			{
				_needsLayout = false;
				return;
			}
			const size_t projectionRevision =
				owner->_columnWidthProjectionRevision;
			const ControlWeakReference presenterLifetime(presenter);
			const ControlWeakReference rowLifetime(row);
			const ControlWeakReference ownerLifetime(owner);
			const auto currentPresenter = [&]()
				-> DataGridCellsPresenter*
			{
				auto* livePresenter = dynamic_cast<DataGridCellsPresenter*>(
					presenterLifetime.Get());
				auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
				auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				return livePresenter && liveRow && liveOwner
					&& livePresenter == presenter
					&& livePresenter->_row == liveRow
					&& liveRow == row
					&& liveRow->_cellsGrid == livePresenter
					&& liveRow->GetDataGridOwner() == liveOwner
					&& liveOwner == owner
					&& liveOwner->_columnWidthProjectionRevision
						== projectionRevision
					? livePresenter : nullptr;
			};
			finalRect = finalRect.Normalized();
			const bool partialResizeArrange =
				owner->_columnWidthDirtyBegin
					!= DataGridCellInfo::InvalidIndex
				&& owner->_columnWidthDirtyEnd
					!= DataGridCellInfo::InvalidIndex
				&& owner->_columnWidthDirtyBegin
					< owner->_columnWidthDirtyEnd
				&& std::isfinite(presenter->_lastArrangedHeight)
				&& std::abs(presenter->_lastArrangedHeight - finalRect.height)
					<= 0.0001f;
			const size_t dirtyBegin = owner->_columnWidthDirtyBegin;
			const size_t dirtyEnd = owner->_columnWidthDirtyEnd;
			std::vector<CellSnapshot> localCells;
			const bool sharedScratch = !presenter->_layoutScratchInUse;
			if (sharedScratch)
			{
				presenter->_layoutScratchInUse = true;
				presenter->_layoutScratch.clear();
			}
			auto scratchGuard = MakeScopeExit(
				[presenterLifetime, sharedScratch]
				{
					if (!sharedScratch) return;
					if (auto* live = dynamic_cast<DataGridCellsPresenter*>(
						presenterLifetime.Get()))
					{
						live->_layoutScratch.clear();
						live->_layoutScratchInUse = false;
					}
				});
			auto& cells = sharedScratch
				? presenter->_layoutScratch : localCells;
			const int childCount = context.ChildCount();
			cells.reserve(static_cast<size_t>((std::max)(0, childCount)));
			for (int childIndex = 0; childIndex < childCount; ++childIndex)
			{
				auto* livePresenter = currentPresenter();
				if (!livePresenter) return;
				auto* cell = dynamic_cast<DataGridCell*>(
					context.ChildAt(childIndex));
				if (!cell || cell->IsCollapsed()) continue;
				if (partialResizeArrange
					&& (cell->_columnIndex < dirtyBegin
						|| cell->_columnIndex >= dirtyEnd)) continue;
				CellSnapshot snapshot;
				snapshot.Lifetime = cell;
				snapshot.Identity = cell;
				const bool resolved = livePresenter->TryGetCellRect(
					cell, snapshot.Left, snapshot.Width);
				livePresenter = currentPresenter();
				auto* liveCell = dynamic_cast<DataGridCell*>(
					snapshot.Lifetime.Get());
				if (!livePresenter || liveCell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(liveCell)) return;
				if (resolved) cells.push_back(std::move(snapshot));
			}
			for (const auto& snapshot : cells)
			{
				auto* livePresenter = currentPresenter();
				auto* cell = dynamic_cast<DataGridCell*>(
					snapshot.Lifetime.Get());
				if (!livePresenter || cell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(cell)) return;
				if (cell->IsCollapsed()) continue;
				const bool fixedRowHeight = !livePresenter->IsHeightAuto();
				if (fixedRowHeight)
				{
					// Fixed-height rows intentionally skip cell Measure above.  A
					// Stretch/Stretch cell also takes the bounded arrange fast path
					// below, but Control normally applies its template from Measure.
					// Materialize the template before reading layout properties so an
					// authored cell template and its initial visual states participate
					// in the very first arranged frame.
					(void)cell->ApplyTemplate();
					livePresenter = currentPresenter();
					cell = dynamic_cast<DataGridCell*>(
						snapshot.Lifetime.Get());
					if (!livePresenter || cell != snapshot.Identity
						|| !livePresenter->IsCurrentCell(cell)) return;
					if (cell->IsCollapsed()) continue;
				}

				const Thickness margin = cell->Margin;
				const float contentWidth = (std::max)(0.0f,
					snapshot.Width - margin.Left - margin.Right);
				const float contentHeight = (std::max)(0.0f,
					finalRect.height - margin.Top - margin.Bottom);
				float x = finalRect.x + snapshot.Left + margin.Left;
				float y = finalRect.y + margin.Top;
				const auto horizontal =
					cui::layout::ResolveHorizontalArrangeAlignment(*cell);
				const auto vertical =
					cui::layout::ResolveVerticalArrangeAlignment(*cell);
				// Auto rows already measured width-dirty cells with unbounded height
				// above. Measuring them again here with the arranged row height would
				// alternate the constraint on every ancestor probe and defeat the cache.
				// Arrange can consume that natural DesiredSize directly. Fixed rows skip
				// cell work in Measure. A default Stretch/Stretch cell needs no natural
				// size at this level: assigning its new slot makes its template root
				// arrange against the new width directly. Non-stretch alignment still
				// takes the bounded measure path to obtain its alignment offset.
				const bool stretchSlot =
					horizontal == HorizontalAlignment::Stretch
					&& vertical == VerticalAlignment::Stretch;
				const auto desired = fixedRowHeight && stretchSlot
					? cui::core::Size{ contentWidth, contentHeight }
					: fixedRowHeight
					? cell->Measure(cui::core::Constraints{
						cui::core::Size{ contentWidth, contentHeight } })
					: (cell->GetComputedLayout().NeedsMeasure()
						? cell->Measure(cui::core::Constraints{
							cui::core::Size{
								contentWidth, cui::core::Infinity } })
						: cell->GetDesiredSizeDip());
				livePresenter = currentPresenter();
				cell = dynamic_cast<DataGridCell*>(snapshot.Lifetime.Get());
				if (!livePresenter || cell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(cell)) return;
				float width = desired.width;
				float height = desired.height;
				if (horizontal == HorizontalAlignment::Stretch)
					width = contentWidth;
				else if (horizontal == HorizontalAlignment::Center)
					x += (contentWidth - width) * 0.5f;
				else if (horizontal == HorizontalAlignment::Right)
					x += contentWidth - width;
				if (vertical == VerticalAlignment::Stretch)
					height = contentHeight;
				else if (vertical == VerticalAlignment::Center)
					y += (contentHeight - height) * 0.5f;
				else if (vertical == VerticalAlignment::Bottom)
					y += contentHeight - height;
				cell->Arrange(cui::core::Rect{ x, y, width, height });
				livePresenter = currentPresenter();
				cell = dynamic_cast<DataGridCell*>(snapshot.Lifetime.Get());
				if (!livePresenter || cell != snapshot.Identity
					|| !livePresenter->IsCurrentCell(cell)) return;
			}
			if (auto* livePresenter = currentPresenter())
				livePresenter->_lastArrangedHeight = finalRect.height;
			_needsLayout = false;
		}

	private:
		DataGridCellsPresenter& _owner;
	};

public:
	explicit DataGridCellsPresenter(DataGridRow& row)
		: _row(&row)
	{
		SetLayoutEngine(new CellsLayoutEngine(*this));
	}

	bool TryCommitResizeLayoutLocally(bool heightIsFixed)
	{
		if (!_row || _row->_cellsGrid != this
			|| !_row->GetDataGridOwner()
			|| !GetComputedLayout().hasArranged) return false;
		if (heightIsFixed)
		{
			// Fixed rows cannot affect the vertical item stack. The custom cells
			// engine can therefore update changed slots directly in the retained
			// panel without walking through Row/Grid/ItemsPresenter/ScrollViewer.
			UpdateLayout();
			return true;
		}

		const float previousDesiredHeight = GetDesiredSizeDip().height;
		const auto desired = Measure(cui::core::Constraints{
			cui::core::Size{
				cui::core::Infinity, cui::core::Infinity } });
		if (std::abs(desired.height - previousDesiredHeight) > 0.0001f)
			return false;
		const auto location = GetActualLocationDip();
		const auto size = GetActualSizeDip();
		Arrange(cui::core::Rect{
			location.x, location.y, size.width, size.height });
		return true;
	}

	void InvalidateColumnLayout(bool propagate)
	{
		if (propagate)
		{
			InvalidateLayout();
			return;
		}
		_needsMeasure = true;
		_needsArrange = true;
		if (_layoutEngine) _layoutEngine->Invalidate();
		// A column-width frame changes this presenter's layout policy, not every
		// retained cell.  The layout engine offers each cell its resolved slot;
		// Control::Measure naturally remeasures only cells whose constraint changed
		// while later cells can keep their desired-size/template caches and merely
		// move during Arrange.  The DataGrid owns the ancestor transaction.
		_layoutState.InvalidateMeasure();
	}

private:
	bool IsCurrentCell(DataGridCell* cell) const noexcept
	{
		if (!_row || _row->_cellsGrid != this || !cell
			|| cell->_row != _row) return false;
		auto* owner = _row->GetDataGridOwner();
		return owner && cell->_columnIndex < owner->ColumnCount()
			&& owner->GetColumnFromDisplayIndex(cell->_columnIndex) == cell->_column;
	}

	bool TryGetCellRect(
		DataGridCell* cell,
		float& left, float& width) const
	{
		left = 0.0f;
		width = 0.0f;
		auto* const presenter =
			const_cast<DataGridCellsPresenter*>(this);
		auto* const row = _row;
		if (!row || row->_cellsGrid != presenter
			|| !cell || cell->_row != row) return false;
		auto* const owner = row->GetDataGridOwner();
		if (!owner) return false;
		const ControlWeakReference presenterLifetime(presenter);
		const ControlWeakReference rowLifetime(row);
		const ControlWeakReference ownerLifetime(owner);
		const ControlWeakReference cellLifetime(cell);
		const size_t columnIndex = cell->_columnIndex;
		auto* const column = cell->_column;
		const size_t projectionRevision =
			owner->_columnWidthProjectionRevision;
		if (columnIndex >= owner->ColumnCount()
			|| column != owner->GetColumnFromDisplayIndex(columnIndex)) return false;
		double logicalLeft = 0.0;
		double logicalRight = 0.0;
		if (!owner->TryResolveColumnBounds(
			columnIndex, logicalLeft, logicalRight)) return false;
		auto* livePresenter = dynamic_cast<DataGridCellsPresenter*>(
			presenterLifetime.Get());
		auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* liveCell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (livePresenter != presenter || liveRow != row
			|| liveOwner != owner || liveCell != cell
			|| livePresenter->_row != liveRow
			|| liveRow->_cellsGrid != livePresenter
			|| liveRow->GetDataGridOwner() != liveOwner
			|| liveOwner->_columnWidthProjectionRevision != projectionRevision
			|| liveCell->_row != liveRow
			|| liveCell->_columnIndex != columnIndex
			|| liveCell->_column != column
			|| columnIndex >= liveOwner->ColumnCount()
			|| liveOwner->GetColumnFromDisplayIndex(columnIndex) != column) return false;
		const double maximum = static_cast<double>(
			(std::numeric_limits<float>::max)());
		left = static_cast<float>((std::clamp)(
			logicalLeft, 0.0, maximum));
		width = static_cast<float>((std::clamp)(
			logicalRight - logicalLeft, 0.0, maximum));
		return true;
	}

	float TotalColumnWidth() const
	{
		auto* const presenter =
			const_cast<DataGridCellsPresenter*>(this);
		auto* const row = _row;
		if (!row || row->_cellsGrid != presenter) return 0.0f;
		auto* const owner = row->GetDataGridOwner();
		if (!owner || owner->ColumnCount() == 0) return 0.0f;
		const size_t columnCount = owner->ColumnCount();
		const size_t projectionRevision =
			owner->_columnWidthProjectionRevision;
		const ControlWeakReference presenterLifetime(presenter);
		const ControlWeakReference rowLifetime(row);
		const ControlWeakReference ownerLifetime(owner);
		double ignored = 0.0;
		double total = 0.0;
		if (!owner->TryResolveColumnBounds(
			columnCount - 1, ignored, total)) return 0.0f;
		auto* livePresenter = dynamic_cast<DataGridCellsPresenter*>(
			presenterLifetime.Get());
		auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (livePresenter != presenter || liveRow != row
			|| liveOwner != owner || livePresenter->_row != liveRow
			|| liveRow->_cellsGrid != livePresenter
			|| liveRow->GetDataGridOwner() != liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision
				!= projectionRevision) return 0.0f;
		return static_cast<float>((std::clamp)(total, 0.0,
			static_cast<double>((std::numeric_limits<float>::max)())));
	}

	DataGridRow* _row = nullptr;
	std::vector<CellSnapshot> _layoutScratch;
	bool _layoutScratchInUse = false;
	float _lastArrangedHeight =
		(std::numeric_limits<float>::quiet_NaN)();
};

DataGridCellInfo DataGridSelectedCellCollection::const_iterator::operator*()
	const
{
	if (!_owner) throw std::out_of_range(
		"DataGrid selected-cell iterator is not dereferenceable");
	if (_excludedOffsets)
		return _owner->AtWithExcludedOffsets(_index, *_excludedOffsets);
	return _owner->at(_index);
}

DataGridSelectedCellCollection::DataGridSelectedCellCollection(
	DataGrid* owner) noexcept
	: _ownerLifetime(owner), _ownerBound(owner != nullptr)
{
}

DataGridSelectedCellCollection::const_iterator
DataGridSelectedCellCollection::begin() const
{
	const bool hasRowProjection = std::any_of(
		_regions.begin(), _regions.end(), [](const CellRegion& region)
		{
			return !region.IncludedRowOffsets.empty()
				|| !region.ExcludedSnapshotRowIntervals.empty()
				|| !region.ExcludedRows.empty();
		});
	if (_excludedCells.empty() && !hasRowProjection)
		return const_iterator(this, 0);
	return const_iterator(this, 0, ExcludedOffsets());
}

size_t DataGridSelectedCellCollection::size() const noexcept
{
	size_t result = _cells.size();
	const auto* lookup = _source
		? dynamic_cast<const IBindingListOccurrenceLookup*>(_source.Get())
		: nullptr;
	for (const auto& region : _regions)
	{
		const size_t columns = region.Columns.size();
		const size_t projectedRows = region.IncludedRowOffsets.empty()
			? region.Height : region.IncludedRowOffsets.size();
		size_t intervalRows = 0;
		for (const auto& interval : region.ExcludedSnapshotRowIntervals)
		{
			const size_t regionEnd = region.Height
				> (std::numeric_limits<size_t>::max)() - region.Top
				? (std::numeric_limits<size_t>::max)()
				: region.Top + region.Height;
			const size_t start = (std::max)(region.Top, interval.Start);
			const size_t end = (std::min)(
				regionEnd, IntervalEndExclusive(interval));
			if (start >= end) continue;
			if (region.IncludedRowOffsets.empty())
				intervalRows += end - start;
			else
			{
				const auto first = std::lower_bound(
					region.IncludedRowOffsets.begin(),
					region.IncludedRowOffsets.end(), start - region.Top);
				const auto last = std::lower_bound(
					first, region.IncludedRowOffsets.end(), end - region.Top);
				intervalRows += static_cast<size_t>(std::distance(first, last));
			}
		}
		const size_t excludedPointRows = static_cast<size_t>(std::count_if(
			region.ExcludedRows.begin(), region.ExcludedRows.end(),
			[&](const CellRegion::ExcludedRow& row)
			{
				if (row.SnapshotRow < region.Top
					|| row.SnapshotRow - region.Top >= region.Height) return false;
				if (IsRowInIntervals(
					region.ExcludedSnapshotRowIntervals, row.SnapshotRow))
					return false;
				return region.IncludedRowOffsets.empty()
					|| std::binary_search(region.IncludedRowOffsets.begin(),
						region.IncludedRowOffsets.end(),
						row.SnapshotRow - region.Top);
			}));
		const size_t excludedRows = intervalRows > projectedRows
			? projectedRows
			: (std::min)(projectedRows, intervalRows + excludedPointRows);
		const size_t rows = excludedRows < projectedRows
			? projectedRows - excludedRows : 0;
		if (columns != 0 && rows
			> (std::numeric_limits<size_t>::max)() / columns)
			return (std::numeric_limits<size_t>::max)();
		const size_t regionSize = rows * columns;
		size_t excludedCells = 0;
		if (lookup && columns != 0)
			for (const auto& excluded : _excludedCells)
			{
				if (std::find(region.Columns.begin(), region.Columns.end(),
					excluded.Column) == region.Columns.end()) continue;
				size_t snapshotRow = 0;
				if (!lookup->TryGetItemIndexByOccurrenceIdentity(
						excluded._itemOccurrence, snapshotRow)
					|| snapshotRow < region.Top
					|| snapshotRow - region.Top >= region.Height) continue;
				const size_t rowOffset = snapshotRow - region.Top;
				if (!region.IncludedRowOffsets.empty()
					&& !std::binary_search(region.IncludedRowOffsets.begin(),
						region.IncludedRowOffsets.end(), rowOffset)) continue;
				if (IsRowInIntervals(
					region.ExcludedSnapshotRowIntervals, snapshotRow)) continue;
				if (std::any_of(region.ExcludedRows.begin(),
					region.ExcludedRows.end(), [&](const auto& row)
					{ return row.Occurrence == excluded._itemOccurrence; }))
					continue;
				++excludedCells;
			}
		const size_t selectedRegionSize = excludedCells < regionSize
			? regionSize - excludedCells : 0;
		if (result > (std::numeric_limits<size_t>::max)()
			- selectedRegionSize)
			return (std::numeric_limits<size_t>::max)();
		result += selectedRegionSize;
	}
	return result;
}

bool DataGridSelectedCellCollection::TryCreateRegionCell(
	const CellRegion& region,
	size_t rowOffset,
	size_t columnOffset,
	DataGridCellInfo& result) const
{
	result = {};
	if (rowOffset >= region.Height
		|| columnOffset >= region.Columns.size()) return false;
	if (region.Top > (std::numeric_limits<size_t>::max)() - rowOffset)
		return false;
	const size_t snapshotRow = region.Top + rowOffset;
	if (IsRowInIntervals(
		region.ExcludedSnapshotRowIntervals, snapshotRow)) return false;
	auto* column = region.Columns[columnOffset];
	if (!column) return false;
	if (!_source || snapshotRow >= _source.Get()->Count()) return false;
	BindingSourceReference item;
	if (!_source.Get()->TryGetItem(snapshotRow, item) || !item) return false;
	const auto* identities =
		dynamic_cast<const IBindingListOccurrenceIdentity*>(_source.Get());
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!identities || !identities->TryGetItemOccurrenceIdentity(
			snapshotRow, occurrence)
		|| occurrence == DataGridCellInfo::InvalidIndex) return false;
	if (std::any_of(region.ExcludedRows.begin(), region.ExcludedRows.end(),
		[occurrence](const CellRegion::ExcludedRow& row)
		{ return row.Occurrence == occurrence; })) return false;
	auto* owner = dynamic_cast<DataGrid*>(_ownerLifetime.Get());
	if (_ownerBound && !owner) return false;
	std::optional<size_t> liveColumnIndex;
	if (owner)
	{
		const auto resolveLiveOwnerAndColumn = [&]() -> DataGrid*
		{
			auto* live = dynamic_cast<DataGrid*>(_ownerLifetime.Get());
			if (!live || live->_source.Shared()
				!= _ownerSourceIdentity.Shared()) return nullptr;
			const auto liveColumn = std::find_if(
				live->_columns.begin(), live->_columns.end(),
				[column](const auto& candidate)
				{ return candidate.get() == column; });
			if (liveColumn == live->_columns.end()) return nullptr;
			liveColumnIndex = static_cast<size_t>(std::distance(
				live->_columns.begin(), liveColumn));
			return live;
		};
		owner = resolveLiveOwnerAndColumn();
		if (!owner) return false;
		if (!region.UseSnapshotLocators
			&& owner->_source.Shared() == _ownerSourceIdentity.Shared())
		{
			// The common immutable/pass-through case has not reordered this
			// occurrence since the region snapshot was captured.  Validate the
			// snapshot index first so enumerating a large selected region stays
			// sequential and never performs one reverse lookup per cell.  A sort
			// or Move simply misses this fast path and falls back to the stable
			// occurrence lookup below.
			const auto liveItems = owner->GetItemsView();
			if (!liveItems) return false;
			const auto materializeCurrent = [&](size_t liveRow,
				BindingSourceReference liveItem) -> bool
			{
				result.Item = std::move(liveItem);
				result.Column = column;
				result.RowIndex = liveRow;
				result.ColumnIndex = *liveColumnIndex;
				result._itemOccurrence = occurrence;
				return true;
			};
			// When the selected region retained the current immutable view itself,
			// the item and occurrence read above already are the live validation.
			// Re-reading them would double source work for every enumerated cell.
			if (liveItems.Shared() == _source.Shared())
				return materializeCurrent(snapshotRow, std::move(item));
			size_t candidateOccurrence = DataGridCellInfo::InvalidIndex;
			const bool occurrenceRead = owner->TryGetItemOccurrenceAt(
				snapshotRow, candidateOccurrence);
			owner = resolveLiveOwnerAndColumn();
			if (!owner || owner->GetItemsView().Shared()
				!= liveItems.Shared()) return false;
			BindingSourceReference candidateItem;
			const bool itemRead = snapshotRow < liveItems.Get()->Count()
				&& liveItems.Get()->TryGetItem(snapshotRow, candidateItem);
			owner = resolveLiveOwnerAndColumn();
			if (!owner || owner->GetItemsView().Shared()
				!= liveItems.Shared()) return false;
			if (occurrenceRead && itemRead && candidateItem
				&& candidateOccurrence == occurrence
				&& candidateItem.Shared() == item.Shared())
				return materializeCurrent(
					snapshotRow, std::move(candidateItem));

			size_t liveRow = DataGridCellInfo::InvalidIndex;
			if (owner->TryResolveItemOccurrence(item, occurrence, liveRow))
			{
				owner = resolveLiveOwnerAndColumn();
				return owner && owner->TryCreateCellInfo(
					liveRow, *liveColumnIndex, result);
			}
			return false;
		}
	}
	result.Item = std::move(item);
	result.Column = column;
	result.RowIndex = snapshotRow;
	result.ColumnIndex = liveColumnIndex.value_or(
		columnOffset < region.ColumnIndices.size()
			? region.ColumnIndices[columnOffset] : columnOffset);
	result._itemOccurrence = occurrence;
	return true;
}

DataGridSelectedCellCollection::RegionExcludedOffsets
DataGridSelectedCellCollection::ExcludedOffsetsForRegion(
	const CellRegion& region) const
{
	RegionExcludedOffsets result;
	const auto* lookup = _source
		? dynamic_cast<const IBindingListOccurrenceLookup*>(_source.Get())
		: nullptr;
	if (!lookup) return result;
	const auto logicalRow = [&region](size_t snapshotRow)
		-> std::optional<size_t>
	{
		if (snapshotRow < region.Top
			|| snapshotRow - region.Top >= region.Height) return std::nullopt;
		const size_t offset = snapshotRow - region.Top;
		if (region.IncludedRowOffsets.empty()) return offset;
		const auto found = std::lower_bound(region.IncludedRowOffsets.begin(),
			region.IncludedRowOffsets.end(), offset);
		if (found == region.IncludedRowOffsets.end() || *found != offset)
			return std::nullopt;
		return static_cast<size_t>(std::distance(
			region.IncludedRowOffsets.begin(), found));
	};
	const size_t regionEnd = region.Height
		> (std::numeric_limits<size_t>::max)() - region.Top
		? (std::numeric_limits<size_t>::max)()
		: region.Top + region.Height;
	for (const auto& interval : region.ExcludedSnapshotRowIntervals)
	{
		const size_t start = (std::max)(region.Top, interval.Start);
		const size_t end = (std::min)(
			regionEnd, IntervalEndExclusive(interval));
		if (start >= end) continue;
		size_t projectedStart = 0;
		size_t projectedCount = 0;
		if (region.IncludedRowOffsets.empty())
		{
			projectedStart = start - region.Top;
			projectedCount = end - start;
		}
		else
		{
			const auto first = std::lower_bound(
				region.IncludedRowOffsets.begin(),
				region.IncludedRowOffsets.end(), start - region.Top);
			const auto last = std::lower_bound(
				first, region.IncludedRowOffsets.end(), end - region.Top);
			projectedStart = static_cast<size_t>(std::distance(
				region.IncludedRowOffsets.begin(), first));
			projectedCount = static_cast<size_t>(std::distance(first, last));
		}
		if (projectedCount == 0) continue;
		if (!result.RowIntervals.empty()
			&& IntervalEndExclusive(result.RowIntervals.back())
				>= projectedStart)
		{
			auto& previous = result.RowIntervals.back();
			const size_t mergedEnd = (std::max)(
				IntervalEndExclusive(previous),
				projectedStart + projectedCount);
			previous.Count = mergedEnd - previous.Start;
		}
		else result.RowIntervals.push_back({
			projectedStart, projectedCount, 0 });
	}
	size_t intervalPrefix = 0;
	for (auto& interval : result.RowIntervals)
	{
		interval.ExcludedBefore = intervalPrefix;
		intervalPrefix += interval.Count;
	}
	result.Rows.reserve(region.ExcludedRows.size());
	for (const auto& excluded : region.ExcludedRows)
		if (const auto row = logicalRow(excluded.SnapshotRow))
			if (!IsRowInIntervals(result.RowIntervals, *row))
				result.Rows.push_back(*row);
	std::sort(result.Rows.begin(), result.Rows.end());
	result.Rows.erase(std::unique(result.Rows.begin(), result.Rows.end()),
		result.Rows.end());
	result.Cells.reserve(_excludedCells.size());
	for (const auto& excluded : _excludedCells)
	{
		const auto column = std::find(
			region.Columns.begin(), region.Columns.end(), excluded.Column);
		if (column == region.Columns.end()) continue;
		size_t rowIndex = 0;
		if (!lookup->TryGetItemIndexByOccurrenceIdentity(
			excluded._itemOccurrence, rowIndex)) continue;
		const auto projectedRow = logicalRow(rowIndex);
		if (!projectedRow || IsProjectedRowExcluded(
			result, *projectedRow)) continue;
		const size_t selectedRow = *projectedRow
			- CountExcludedRowsBefore(result, *projectedRow);
		const size_t columnOffset = static_cast<size_t>(
			std::distance(region.Columns.begin(), column));
		result.Cells.push_back(
			selectedRow * region.Columns.size() + columnOffset);
	}
	std::sort(result.Cells.begin(), result.Cells.end());
	result.Cells.erase(std::unique(result.Cells.begin(), result.Cells.end()),
		result.Cells.end());
	return result;
}

std::shared_ptr<const std::vector<
	DataGridSelectedCellCollection::RegionExcludedOffsets>>
DataGridSelectedCellCollection::ExcludedOffsets() const
{
	if (_excludedOffsetsCache) return _excludedOffsetsCache;
	auto offsets = std::make_shared<std::vector<RegionExcludedOffsets>>();
	offsets->reserve(_regions.size());
	for (const auto& region : _regions)
		offsets->push_back(ExcludedOffsetsForRegion(region));
	_excludedOffsetsCache = offsets;
	return offsets;
}

DataGridCellInfo DataGridSelectedCellCollection::AtWithExcludedOffsets(
	size_t index,
	const std::vector<RegionExcludedOffsets>& excludedOffsets) const
{
	// The caller already paid to translate occurrence-based holes into these
	// compact offsets. Do not call size() here: size() must resolve raw sparse
	// holes, so doing that once per iterator dereference would turn a single
	// exclusion into one reverse occurrence lookup per selected cell.
	size_t selectedCount = _cells.size();
	for (size_t regionIndex = 0;
		regionIndex < _regions.size(); ++regionIndex)
	{
		const auto& region = _regions[regionIndex];
		if (region.Columns.empty() || region.Height == 0) continue;
		const size_t projectedRows = region.IncludedRowOffsets.empty()
			? region.Height : region.IncludedRowOffsets.size();
		static const RegionExcludedOffsets noExclusions;
		const auto& excluded = regionIndex < excludedOffsets.size()
			? excludedOffsets[regionIndex] : noExclusions;
		const size_t excludedRows = CountExcludedRowsBefore(
			excluded, projectedRows);
		const size_t selectedRows = excludedRows < projectedRows
			? projectedRows - excludedRows : 0;
		const size_t regionSize = region.Columns.size() != 0
			&& selectedRows > (std::numeric_limits<size_t>::max)()
				/ region.Columns.size()
			? (std::numeric_limits<size_t>::max)()
			: selectedRows * region.Columns.size();
		const size_t selectedSize = excluded.Cells.size() < regionSize
			? regionSize - excluded.Cells.size() : 0;
		if (selectedCount > (std::numeric_limits<size_t>::max)()
			- selectedSize)
		{
			selectedCount = (std::numeric_limits<size_t>::max)();
			break;
		}
		selectedCount += selectedSize;
	}
	if (index >= selectedCount) throw std::out_of_range(
		"DataGrid selected-cell index is out of range");
	auto* owner = dynamic_cast<DataGrid*>(_ownerLifetime.Get());
	if (_ownerBound && !owner) return {};
	if (index < _cells.size())
	{
		const auto& cell = _cells[index];
		if (owner && std::none_of(owner->_columns.begin(), owner->_columns.end(),
			[&cell](const auto& column)
			{ return column.get() == cell.Column; })) return {};
		return cell;
	}
	index -= _cells.size();
	for (size_t regionIndex = 0;
		regionIndex < _regions.size(); ++regionIndex)
	{
		const auto& region = _regions[regionIndex];
		if (region.Columns.empty() || region.Height == 0) continue;
		const size_t projectedRows = region.IncludedRowOffsets.empty()
			? region.Height : region.IncludedRowOffsets.size();
		static const RegionExcludedOffsets noExclusions;
		const auto& excluded = regionIndex < excludedOffsets.size()
			? excludedOffsets[regionIndex] : noExclusions;
		const size_t excludedRows = CountExcludedRowsBefore(
			excluded, projectedRows);
		const size_t selectedRows = excludedRows < projectedRows
			? projectedRows - excludedRows : 0;
		const size_t regionSize = selectedRows * region.Columns.size();
		const size_t selectedSize = excluded.Cells.size() < regionSize
			? regionSize - excluded.Cells.size() : 0;
		if (index >= selectedSize)
		{
			index -= selectedSize;
			continue;
		}
		const size_t flat = RawOffsetForSelectedOrdinal(
			index, regionSize, excluded.Cells);
		const size_t selectedRow = flat / region.Columns.size();
		const size_t logicalRow = RawRowOffsetForSelectedOrdinal(
			selectedRow, projectedRows, excluded);
		const size_t snapshotOffset = region.IncludedRowOffsets.empty()
			? logicalRow : region.IncludedRowOffsets[logicalRow];
		DataGridCellInfo result;
		if (flat < regionSize && TryCreateRegionCell(region,
			snapshotOffset,
			flat % region.Columns.size(), result)) return result;
		return {};
	}
	return {};
}

DataGridCellInfo DataGridSelectedCellCollection::at(size_t index) const
{
	if (index >= size()) throw std::out_of_range(
		"DataGrid selected-cell index is out of range");
	// Public snapshots keep column identity as a non-owning pointer. Once their
	// owner has died no cell, including a sparse one, may expose that pointer.
	auto* owner = dynamic_cast<DataGrid*>(_ownerLifetime.Get());
	if (_ownerBound && !owner) return {};
	if (!_regions.empty())
		return AtWithExcludedOffsets(index, *ExcludedOffsets());
	if (index < _cells.size())
	{
		const auto& cell = _cells[index];
		if (owner && std::none_of(owner->_columns.begin(), owner->_columns.end(),
			[&cell](const auto& column)
			{ return column.get() == cell.Column; })) return {};
		return cell;
	}
	index -= _cells.size();
	for (const auto& region : _regions)
	{
		if (region.Columns.empty() || region.Height == 0) continue;
		const size_t regionSize = region.Height * region.Columns.size();
		if (_excludedCells.empty())
		{
			if (index >= regionSize)
			{
				index -= regionSize;
				continue;
			}
			DataGridCellInfo result;
			if (TryCreateRegionCell(region,
				index / region.Columns.size(),
				index % region.Columns.size(), result)) return result;
			return {};
		}
	}
	return {};
}

bool DataGridSelectedCellCollection::Contains(
	const DataGridCellInfo& candidate) const noexcept
{
	try
	{
		if (ContainsCellIdentity(_cells, candidate)) return true;
		const auto* lookup = _source
			? dynamic_cast<const IBindingListOccurrenceLookup*>(_source.Get())
			: nullptr;
		if (!lookup || candidate._itemOccurrence
			== DataGridCellInfo::InvalidIndex) return false;
		size_t snapshotRow = 0;
		if (!lookup->TryGetItemIndexByOccurrenceIdentity(
			candidate._itemOccurrence, snapshotRow)) return false;
		BindingSourceReference snapshotItem;
		if (!_source.Get()->TryGetItem(snapshotRow, snapshotItem)
			|| !snapshotItem
			|| snapshotItem.Shared() != candidate.Item.Shared()) return false;
		for (const auto& region : _regions)
		{
			if (snapshotRow < region.Top
				|| snapshotRow - region.Top >= region.Height
				|| std::find(region.Columns.begin(), region.Columns.end(),
					candidate.Column) == region.Columns.end()) continue;
			const size_t offset = snapshotRow - region.Top;
			if (!region.IncludedRowOffsets.empty()
				&& !std::binary_search(region.IncludedRowOffsets.begin(),
					region.IncludedRowOffsets.end(), offset)) continue;
			if (IsRowInIntervals(
				region.ExcludedSnapshotRowIntervals, snapshotRow)) continue;
			if (std::any_of(region.ExcludedRows.begin(),
				region.ExcludedRows.end(), [&](const CellRegion::ExcludedRow& row)
				{ return row.Occurrence == candidate._itemOccurrence; }))
				continue;
			return !ContainsCellIdentity(_excludedCells, candidate);
		}
	}
	catch (...) {}
	return false;
}

void DataGridSelectedCellCollection::SetSparse(
	DataGrid* owner,
	BindingListReference source,
	std::vector<DataGridCellInfo> cells) noexcept
{
	_ownerLifetime = ControlWeakReference(owner);
	_ownerBound = owner != nullptr;
	_ownerSourceIdentity = owner ? owner->_source : source;
	_source = std::move(source);
	_cells = std::move(cells);
	_regions.clear();
	_excludedCells.clear();
	InvalidateExcludedOffsets();
}

void DataGridSelectedCellCollection::SetFullRegion(
	DataGrid* owner,
	BindingListReference source,
	size_t rowCount,
	std::vector<DataGridColumn*> columns) noexcept
{
	_ownerLifetime = ControlWeakReference(owner);
	_ownerBound = owner != nullptr;
	_ownerSourceIdentity = owner ? owner->_source : source;
	_source = std::move(source);
	_cells.clear();
	_regions.clear();
	_excludedCells.clear();
	InvalidateExcludedOffsets();
	if (rowCount == 0 || columns.empty()) return;
	CellRegion region;
	region.Height = rowCount;
	region.Columns = std::move(columns);
	region.ColumnIndices.resize(region.Columns.size());
	for (size_t index = 0; index < region.ColumnIndices.size(); ++index)
		region.ColumnIndices[index] = index;
	_regions.push_back(std::move(region));
}

bool DataGridSelectedCellCollection::SameLogicalCells(
	const DataGridSelectedCellCollection& other) const noexcept
{
	if (size() != other.size()) return false;
	const auto sameSet = [](const std::vector<DataGridCellInfo>& left,
		const std::vector<DataGridCellInfo>& right)
	{
		return left.size() == right.size()
			&& std::all_of(left.begin(), left.end(),
				[&right](const DataGridCellInfo& cell)
				{ return ContainsCellIdentity(right, cell); });
	};
	if (_ownerSourceIdentity.Shared() == other._ownerSourceIdentity.Shared()
		&& _source.Shared() == other._source.Shared()
		&& _regions.size() == other._regions.size())
	{
		bool regionsEqual = true;
		for (size_t index = 0; index < _regions.size(); ++index)
		{
			const auto& left = _regions[index];
			const auto& right = other._regions[index];
			if (left.Top != right.Top || left.Height != right.Height
				|| left.Columns != right.Columns
				|| left.IncludedRowOffsets != right.IncludedRowOffsets
				|| left.ExcludedSnapshotRowIntervals.size()
					!= right.ExcludedSnapshotRowIntervals.size()
				|| !std::equal(
					left.ExcludedSnapshotRowIntervals.begin(),
					left.ExcludedSnapshotRowIntervals.end(),
					right.ExcludedSnapshotRowIntervals.begin(),
					[](const auto& a, const auto& b)
					{
						return a.Start == b.Start && a.Count == b.Count;
					})
				|| left.UseSnapshotLocators != right.UseSnapshotLocators
				|| left.ExcludedRows.size() != right.ExcludedRows.size()
				|| !std::equal(left.ExcludedRows.begin(),
					left.ExcludedRows.end(), right.ExcludedRows.begin(),
					right.ExcludedRows.end(),
					[](const CellRegion::ExcludedRow& a,
						const CellRegion::ExcludedRow& b)
					{
						return a.Occurrence == b.Occurrence
							&& a.SnapshotRow == b.SnapshotRow;
					}))
			{
				regionsEqual = false;
				break;
			}
		}
		if (regionsEqual && sameSet(_cells, other._cells)
			&& sameSet(_excludedCells, other._excludedCells)) return true;
	}
	// Sparse selections preserve their historical identity-set comparison.
	if (_regions.empty() && other._regions.empty())
		return sameSet(_cells, other._cells);
	return false;
}

DataGridSelectedCellCollection DataGridSelectedCellCollection::Difference(
	const DataGridSelectedCellCollection& other) const
{
	auto* owner = dynamic_cast<DataGrid*>(_ownerLifetime.Get());
	DataGridSelectedCellCollection result(owner);
	if (_ownerBound && !owner) result._ownerBound = true;
	result._ownerSourceIdentity = _ownerSourceIdentity;
	result._source = _source;
	for (const auto& cell : _cells)
		if (!other.Contains(cell)) result._cells.push_back(cell);
	if (_regions.empty()) return result;
	if (other.empty())
	{
		result._regions = _regions;
		result._excludedCells = _excludedCells;
		return result;
	}

	const bool sameRows =
		_ownerSourceIdentity.Shared() == other._ownerSourceIdentity.Shared()
		&& _source.Shared() == other._source.Shared()
		&& _regions.size() == 1 && other._regions.size() == 1
		&& _regions.front().Top == other._regions.front().Top
		&& _regions.front().Height == other._regions.front().Height;
	if (sameRows)
	{
		const auto& left = _regions.front();
		const auto& right = other._regions.front();
		const auto rowExcluded = [](const CellRegion& region, size_t row)
		{
			return IsRowInIntervals(
				region.ExcludedSnapshotRowIntervals, row)
				|| std::any_of(region.ExcludedRows.begin(),
				region.ExcludedRows.end(), [row](const auto& excluded)
				{ return excluded.SnapshotRow == row; });
		};
		struct RowRun final { size_t Start = 0; size_t Count = 0; };
		std::vector<RowRun> removedRuns;
		const size_t leftEnd = left.Height
			> (std::numeric_limits<size_t>::max)() - left.Top
			? (std::numeric_limits<size_t>::max)()
			: left.Top + left.Height;
		const auto appendSelectedRuns = [&](size_t requestedStart,
			size_t requestedEnd)
		{
			const size_t start = (std::max)(left.Top, requestedStart);
			const size_t end = (std::min)(leftEnd, requestedEnd);
			if (start >= end) return;
			if (!left.IncludedRowOffsets.empty())
			{
				auto at = std::lower_bound(left.IncludedRowOffsets.begin(),
					left.IncludedRowOffsets.end(), start - left.Top);
				RowRun run;
				for (; at != left.IncludedRowOffsets.end(); ++at)
				{
					const size_t row = left.Top + *at;
					if (row >= end) break;
					if (rowExcluded(left, row)) continue;
					if (run.Count != 0
						&& run.Start + run.Count == row)
					{
						++run.Count;
						continue;
					}
					if (run.Count != 0) removedRuns.push_back(run);
					run = { row, 1 };
				}
				if (run.Count != 0) removedRuns.push_back(run);
				return;
			}

			// Subtract existing row holes using interval algebra.  Work is
			// proportional to existing holes/runs, never to requestedEnd-start.
			std::vector<RowRun> cuts;
			cuts.reserve(left.ExcludedSnapshotRowIntervals.size()
				+ left.ExcludedRows.size());
			for (const auto& interval : left.ExcludedSnapshotRowIntervals)
			{
				const size_t cutStart = (std::max)(start, interval.Start);
				const size_t cutEnd = (std::min)(
					end, IntervalEndExclusive(interval));
				if (cutStart < cutEnd)
					cuts.push_back({ cutStart, cutEnd - cutStart });
			}
			for (const auto& excluded : left.ExcludedRows)
				if (excluded.SnapshotRow >= start
					&& excluded.SnapshotRow < end)
					cuts.push_back({ excluded.SnapshotRow, 1 });
			std::sort(cuts.begin(), cuts.end(), [](const RowRun& a,
				const RowRun& b) { return a.Start < b.Start; });
			size_t cursor = start;
			for (const auto& cut : cuts)
			{
				if (cut.Start > cursor)
					removedRuns.push_back({ cursor, cut.Start - cursor });
				cursor = (std::max)(cursor, IntervalEndExclusive(cut));
				if (cursor >= end) break;
			}
			if (cursor < end) removedRuns.push_back({ cursor, end - cursor });
		};

		// A retired continuous old-snapshot range becomes one lazy region (or
		// one per pre-existing hole), instead of K IncludedRowOffsets.
		for (const auto& interval : right.ExcludedSnapshotRowIntervals)
			appendSelectedRuns(interval.Start, IntervalEndExclusive(interval));
		for (const auto& excluded : right.ExcludedRows)
		{
			if (rowExcluded(left, excluded.SnapshotRow)
				|| IsRowInIntervals(right.ExcludedSnapshotRowIntervals,
					excluded.SnapshotRow)) continue;
			appendSelectedRuns(excluded.SnapshotRow,
				excluded.SnapshotRow == (std::numeric_limits<size_t>::max)()
					? excluded.SnapshotRow : excluded.SnapshotRow + 1);
		}
		std::sort(removedRuns.begin(), removedRuns.end(),
			[](const RowRun& a, const RowRun& b) { return a.Start < b.Start; });
		std::vector<RowRun> mergedRuns;
		for (const auto& run : removedRuns)
		{
			if (run.Count == 0) continue;
			if (!mergedRuns.empty()
				&& IntervalEndExclusive(mergedRuns.back()) >= run.Start)
			{
				auto& previous = mergedRuns.back();
				const size_t end = (std::max)(
					IntervalEndExclusive(previous),
					IntervalEndExclusive(run));
				previous.Count = end - previous.Start;
			}
			else mergedRuns.push_back(run);
		}
		for (const auto& run : mergedRuns)
		{
			CellRegion removedRows;
			removedRows.Top = run.Start;
			removedRows.Height = run.Count;
			removedRows.Columns = left.Columns;
			removedRows.ColumnIndices = left.ColumnIndices;
			removedRows.UseSnapshotLocators = true;
			result._regions.push_back(std::move(removedRows));
		}
		if (!mergedRuns.empty())
		{
			const auto* lookup = _source
				? dynamic_cast<const IBindingListOccurrenceLookup*>(
					_source.Get()) : nullptr;
			if (lookup)
				for (const auto& excluded : _excludedCells)
				{
					if (std::find(left.Columns.begin(), left.Columns.end(),
						excluded.Column) == left.Columns.end()) continue;
					size_t snapshotRow = 0;
					if (!lookup->TryGetItemIndexByOccurrenceIdentity(
							excluded._itemOccurrence, snapshotRow)) continue;
					const auto after = std::upper_bound(
						mergedRuns.begin(), mergedRuns.end(), snapshotRow,
						[](size_t row, const RowRun& run)
						{ return row < run.Start; });
					if (after != mergedRuns.begin()
						&& snapshotRow - std::prev(after)->Start
							< std::prev(after)->Count)
						result._excludedCells.push_back(excluded);
				}
		}
		CellRegion removedColumns;
		removedColumns.Top = left.Top;
		removedColumns.Height = left.Height;
		removedColumns.IncludedRowOffsets = left.IncludedRowOffsets;
		removedColumns.ExcludedSnapshotRowIntervals =
			left.ExcludedSnapshotRowIntervals;
		removedColumns.ExcludedRows = left.ExcludedRows;
		for (size_t index = 0; index < left.Columns.size(); ++index)
			if (std::find(right.Columns.begin(), right.Columns.end(),
				left.Columns[index]) == right.Columns.end())
			{
				removedColumns.Columns.push_back(left.Columns[index]);
				removedColumns.ColumnIndices.push_back(
					index < left.ColumnIndices.size()
						? left.ColumnIndices[index] : index);
			}
		if (!removedColumns.Columns.empty())
		{
			result._regions.push_back(std::move(removedColumns));
			for (const auto& excluded : _excludedCells)
				if (std::find(result._regions.back().Columns.begin(),
					result._regions.back().Columns.end(), excluded.Column)
					!= result._regions.back().Columns.end()
					&& !ContainsCellIdentity(result._excludedCells, excluded))
					result._excludedCells.push_back(excluded);
		}
		// On columns shared by both domains, a newly excluded identity is the
		// exact removed-cell delta; no rectangle needs to be expanded.
		for (const auto& excluded : other._excludedCells)
			if (Contains(excluded) && !ContainsCellIdentity(
				result._cells, excluded)) result._cells.push_back(excluded);
		return result;
	}

	if (other._regions.empty())
	{
		result._regions = _regions;
		result._excludedCells = _excludedCells;
		for (const auto& retained : other._cells)
			if (Contains(retained)) result.Exclude(retained);
		return result;
	}

	// A raw Reset may replace the immutable checkpoint while ListBox restores
	// the WPF-selected object multiplicities.  Expanding two otherwise dense
	// row regions here would turn one O(rows) Reset into O(rows * columns)
	// DataGridCellInfo objects.  Compare selected row multiplicities once and
	// retain only missing old-row offsets in the old snapshot instead.
	if (_regions.size() == 1 && other._regions.size() == 1
		&& _excludedCells.empty() && other._excludedCells.empty()
		&& _regions.front().Columns == other._regions.front().Columns
		&& _source && other._source)
	{
		const auto& left = _regions.front();
		const auto& right = other._regions.front();
		const auto rowSelected = [](const CellRegion& region, size_t offset)
		{
			if (offset >= region.Height) return false;
			if (!region.IncludedRowOffsets.empty()
				&& !std::binary_search(region.IncludedRowOffsets.begin(),
					region.IncludedRowOffsets.end(), offset)) return false;
			const size_t row = region.Top + offset;
			return !IsRowInIntervals(
				region.ExcludedSnapshotRowIntervals, row)
				&& std::none_of(region.ExcludedRows.begin(),
				region.ExcludedRows.end(), [row](const auto& excluded)
				{ return excluded.SnapshotRow == row; });
		};
		std::unordered_map<IBindingSource*, size_t> retainedCounts;
		retainedCounts.reserve((std::min)(right.Height, size_t{ 4096 }));
		const size_t leftCount = _source.Get()->Count();
		const size_t rightCount = other._source.Get()->Count();
		bool readable = true;
		for (size_t offset = 0; offset < right.Height; ++offset)
		{
			if (!rowSelected(right, offset)) continue;
			BindingSourceReference item;
			if (right.Top > rightCount
				|| offset >= rightCount - right.Top
				|| !other._source.Get()->TryGetItem(
					right.Top + offset, item) || !item)
			{
				readable = false;
				break;
			}
			++retainedCounts[item.Get()];
		}
		CellRegion removedRows;
		removedRows.Top = left.Top;
		removedRows.Height = left.Height;
		removedRows.Columns = left.Columns;
		removedRows.ColumnIndices = left.ColumnIndices;
		removedRows.UseSnapshotLocators = true;
		if (readable)
			for (size_t offset = 0; offset < left.Height; ++offset)
			{
				if (!rowSelected(left, offset)) continue;
				BindingSourceReference item;
				if (left.Top > leftCount
					|| offset >= leftCount - left.Top
					|| !_source.Get()->TryGetItem(
						left.Top + offset, item) || !item)
				{
					readable = false;
					break;
				}
				auto retained = retainedCounts.find(item.Get());
				if (retained != retainedCounts.end()
					&& retained->second != 0)
				{
					--retained->second;
					continue;
				}
				removedRows.IncludedRowOffsets.push_back(offset);
			}
		if (readable)
		{
			if (!removedRows.IncludedRowOffsets.empty())
				result._regions.push_back(std::move(removedRows));
			return result;
		}
	}

	// Different dense row domains are not produced by the safe SelectAll slice.
	// Preserve correctness for a future caller by materializing only at this
	// explicit delta boundary instead of silently truncating the change.
	for (const auto& cell : *this)
		if (!other.Contains(cell)) result._cells.push_back(cell);
	return result;
}

void DataGridSelectedCellCollection::PruneColumns(
	const std::vector<std::unique_ptr<DataGridColumn>>& columns)
{
	InvalidateExcludedOffsets();
	for (auto& region : _regions)
	{
		std::vector<DataGridColumn*> retained;
		std::vector<size_t> indices;
		retained.reserve(region.Columns.size());
		indices.reserve(region.Columns.size());
		for (auto* column : region.Columns)
		{
			const auto found = std::find_if(columns.begin(), columns.end(),
				[column](const auto& candidate)
				{ return candidate.get() == column; });
			if (found == columns.end()) continue;
			retained.push_back(column);
			indices.push_back(static_cast<size_t>(
				std::distance(columns.begin(), found)));
		}
		region.Columns = std::move(retained);
		region.ColumnIndices = std::move(indices);
	}
	_regions.erase(std::remove_if(_regions.begin(), _regions.end(),
		[](const CellRegion& region)
		{ return region.Height == 0 || region.Columns.empty(); }), _regions.end());
	_excludedCells.erase(std::remove_if(
		_excludedCells.begin(), _excludedCells.end(),
		[this](const DataGridCellInfo& cell)
		{
			return std::none_of(_regions.begin(), _regions.end(),
				[&cell](const CellRegion& region)
				{
					return std::find(region.Columns.begin(),
						region.Columns.end(), cell.Column)
						!= region.Columns.end();
				});
		}), _excludedCells.end());
}

bool DataGridSelectedCellCollection::Exclude(
	const DataGridCellInfo& cell)
{
	if (!Contains(cell) || ContainsCellIdentity(_excludedCells, cell))
		return false;
	_excludedCells.push_back(cell);
	InvalidateExcludedOffsets();
	return true;
}

bool DataGridSelectedCellCollection::Include(
	const DataGridCellInfo& cell)
{
	const auto found = std::find_if(
		_excludedCells.begin(), _excludedCells.end(),
		[&cell](const DataGridCellInfo& candidate)
		{ return SameCellIdentity(candidate, cell); });
	if (found == _excludedCells.end()) return false;
	_excludedCells.erase(found);
	InvalidateExcludedOffsets();
	return true;
}

const DependencyProperty& DataGridHyperlink::NavigateUriProperty()
{
	static const auto registration = []
	{
		auto options = DependencyPropertyOptions<
			DataGridHyperlink, std::wstring>{
				std::wstring{}, DependencyPropertyFlags::AffectsRender };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGridHyperlink, std::wstring>(
				DependencyPropertyRegistrationLiteral(L"NavigateUri"),
				[](DataGridHyperlink& target)
				{ return target.GetNavigateUri(); },
				[](DataGridHyperlink& target, const std::wstring& value)
				{ target.SetNavigateUri(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGridHyperlink::TargetNameProperty()
{
	static const auto registration = []
	{
		auto options = DependencyPropertyOptions<
			DataGridHyperlink, std::wstring>{
				std::wstring{}, DependencyPropertyFlags::None };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGridHyperlink, std::wstring>(
				DependencyPropertyRegistrationLiteral(L"TargetName"),
				[](DataGridHyperlink& target)
				{ return target.GetTargetName(); },
				[](DataGridHyperlink& target, const std::wstring& value)
				{ target.SetTargetName(value); }, {}, std::move(options));
	}();
	return *registration;
}

void DataGridHyperlink::RegisterDependencyProperties()
{
	Button::RegisterDependencyProperties();
	(void)NavigateUriProperty();
	(void)TargetNameProperty();
}

DataGridHyperlink::DataGridHyperlink()
	: Button()
{
	RegisterDependencyProperties();
}

void DataGridHyperlink::SetNavigateUri(std::wstring value)
{
	(void)SetPropertyField(
		NavigateUriProperty(), _navigateUri, std::move(value));
}

void DataGridHyperlink::SetTargetName(std::wstring value)
{
	(void)SetPropertyField(
		TargetNameProperty(), _targetName, std::move(value));
}

std::unique_ptr<AutomationPeer>
DataGridHyperlink::OnCreateAutomationPeer()
{
	return std::make_unique<InvokeAutomationPeer>(
		*this, AutomationControlType::Hyperlink, L"Hyperlink");
}

bool DataGridHyperlink::OnClick()
{
	const ControlWeakReference lifetime(this);
	bool navigationRaised = false;
	if (!_navigateUri.empty())
	{
		DataGridHyperlinkNavigateEventArgs args;
		args.NavigateUri = _navigateUri;
		args.TargetName = _targetName;
		cui::framework::EventAccess::Raise(NavigateRequested, this, args);
		navigationRaised = true;
	}
	auto* current = dynamic_cast<DataGridHyperlink*>(lifetime.Get());
	return current
		? current->Button::OnClick() || navigationRaised
		: navigationRaised;
}

bool DataGridLength::IsValid() const noexcept
{
	if (!std::isfinite(Value) || Value < 0.0) return false;
	switch (UnitType)
	{
	case DataGridLengthUnitType::Auto:
	case DataGridLengthUnitType::Pixel:
	case DataGridLengthUnitType::SizeToCells:
	case DataGridLengthUnitType::SizeToHeader:
	case DataGridLengthUnitType::Star:
		return true;
	}
	return false;
}

bool DataGridLength::TryParse(
	std::wstring_view text, DataGridLength& out) noexcept
{
	try
	{
		text = Trim(text);
		if (EqualsIgnoreCase(text, L"Auto"))
		{
			out = Auto();
			return true;
		}
		if (EqualsIgnoreCase(text, L"SizeToHeader"))
		{
			out = SizeToHeader();
			return true;
		}
		if (EqualsIgnoreCase(text, L"SizeToCells"))
		{
			out = SizeToCells();
			return true;
		}

		bool star = !text.empty() && text.back() == L'*';
		if (star) text.remove_suffix(1);
		text = Trim(text);
		if (star && text.empty())
		{
			out = Star();
			return true;
		}
		if (text.empty()) return false;
		std::wstring buffer(text);
		wchar_t* end = nullptr;
		const double value = std::wcstod(buffer.c_str(), &end);
		if (!end || end != buffer.c_str() + buffer.size()) return false;
		DataGridLength parsed(value, star
			? DataGridLengthUnitType::Star
			: DataGridLengthUnitType::Pixel);
		if (!parsed.IsValid()) return false;
		out = parsed;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

std::wstring DataGridLength::ToString() const
{
	switch (UnitType)
	{
	case DataGridLengthUnitType::Auto:
		return L"Auto";
	case DataGridLengthUnitType::SizeToCells:
		return L"SizeToCells";
	case DataGridLengthUnitType::SizeToHeader:
		return L"SizeToHeader";
	case DataGridLengthUnitType::Pixel:
	case DataGridLengthUnitType::Star:
	{
		std::wostringstream stream;
		stream << std::setprecision(15) << Value;
		if (UnitType == DataGridLengthUnitType::Star) stream << L'*';
		return stream.str();
	}
	default:
		return {};
	}
}

void DataGridColumn::SetHeader(BindingValue value)
{
	if (BindingValuesEqual(_header, value)) return;
	_header = std::move(value);
	NotifyOwnerChanged();
}

void DataGridColumn::SetHeaderStyle(std::wstring value)
{
	if (_headerStyle == value) return;
	_headerStyle = std::move(value);
	if (_owner) _owner->RefreshRealizedColumnHeaderPresentation(this);
}

void DataGridColumn::SetHeaderTemplate(ItemTemplateReference value)
{
	if (_headerTemplate == value) return;
	_headerTemplate = std::move(value);
	if (_owner) _owner->RefreshRealizedColumnHeaderPresentation(this);
}

void DataGridColumn::SetCellStyle(std::wstring value)
{
	if (_cellStyle == value) return;
	_cellStyle = std::move(value);
	if (_owner) _owner->RefreshRealizedCellStyles(this);
}

const DataGridLength& DataGridColumn::GetWidth() const noexcept
{
	return _hasWidth || !_owner ? _width : _owner->GetColumnWidth();
}

double DataGridColumn::GetMinWidth() const noexcept
{
	return _hasMinWidth || !_owner
		? _minWidth : _owner->GetMinColumnWidth();
}

double DataGridColumn::GetMaxWidth() const noexcept
{
	return _hasMaxWidth || !_owner
		? _maxWidth : _owner->GetMaxColumnWidth();
}

double DataGridColumn::GetLayoutMinWidth() const noexcept
{
	// WPF gives maximum constraints precedence when a caller deliberately
	// supplies contradictory Min/Max values. Keep every layout clamp ordered.
	return (std::min)(GetMinWidth(), GetMaxWidth());
}

double DataGridColumn::GetLayoutMaxWidth() const noexcept
{
	return GetMaxWidth();
}

void DataGridColumn::SetWidth(DataGridLength value)
{
	if (!value.IsValid())
		throw std::invalid_argument("DataGridColumn.Width is invalid");
	if (_hasWidth && _width == value) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	const auto previousWidth = GetWidth();
	const bool removedLastStar = _owner
		&& previousWidth.UnitType == DataGridLengthUnitType::Star
		&& value.UnitType != DataGridLengthUnitType::Star
		&& std::none_of(_owner->_columns.begin(), _owner->_columns.end(),
			[this](const auto& column)
			{
				return column.get() != this
					&& column->GetWidth().UnitType
						== DataGridLengthUnitType::Star;
			});
	_width = value;
	_hasWidth = true;
	_runtimeWidth = {};
	if (removedLastStar)
	{
		// WPF releases compensation Display values once no Star column remains;
		// otherwise the former donor stays artificially narrow and leaves a gap.
		for (const auto& column : _owner->_columns)
		{
			if (column.get() == this
				|| !column->_runtimeWidth.HasDisplayOverride
				|| !std::isfinite(column->_runtimeWidth.Desired)) continue;
			column->_runtimeWidth = {};
		}
	}
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::ClearWidth()
{
	if (!_hasWidth) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_hasWidth = false;
	_width = DataGridLength::Auto();
	_runtimeWidth = {};
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::SetMinWidth(double value)
{
	if (!std::isfinite(value) || value < 0.0)
		throw std::invalid_argument("DataGridColumn.MinWidth is invalid");
	if (_hasMinWidth && _minWidth == value) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_minWidth = value;
	_hasMinWidth = true;
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::ClearMinWidth()
{
	if (!_hasMinWidth) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_hasMinWidth = false;
	_minWidth = 20.0;
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::SetMaxWidth(double value)
{
	if (std::isnan(value) || value < 0.0)
		throw std::invalid_argument("DataGridColumn.MaxWidth is invalid");
	if (_hasMaxWidth && _maxWidth == value) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_maxWidth = value;
	_hasMaxWidth = true;
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::ClearMaxWidth()
{
	if (!_hasMaxWidth) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_hasMaxWidth = false;
	_maxWidth = (std::numeric_limits<double>::infinity)();
	if (_owner) _owner->RefreshColumnWidths();
}

void DataGridColumn::SetIsReadOnly(bool value)
{
	if (_isReadOnly == value) return;
	_isReadOnly = value;
	NotifyOwnerChanged();
}

void DataGridColumn::SetCanUserSort(bool value)
{
	if (_canUserSort == value) return;
	_canUserSort = value;
	NotifyOwnerChanged();
}

void DataGridColumn::SetCanUserResize(bool value)
{
	if (_canUserResize == value) return;
	if (_owner && !_owner->_columnResizeSnapshot.empty())
		_owner->EndColumnResizeTransaction(true);
	_canUserResize = value;
}

void DataGridColumn::SetCanUserReorder(bool value)
{
	if (_canUserReorder == value) return;
	_canUserReorder = value;
	if (_owner && !value)
		_owner->CancelColumnReorderForEligibilityChange(this);
}

void DataGridColumn::SetVisibility(Visibility value)
{
	if (value != Visibility::Visible && value != Visibility::Hidden
		&& value != Visibility::Collapsed)
		throw std::invalid_argument("DataGridColumn.Visibility is invalid");
	if (_visibility == value) return;
	if (!_owner)
	{
		_visibility = value;
		return;
	}
	(void)_owner->SetColumnVisibility(*this, value);
}

bool DataGridColumn::GetIsFrozen() const noexcept
{
	return _owner && _displayIndex != UnsetDisplayIndex
		&& _displayIndex < static_cast<size_t>(
			(std::max)(0, _owner->GetFrozenColumnCount()));
}

void DataGridColumn::SetDisplayIndex(size_t value)
{
	if (!_owner)
	{
		_displayIndex = value;
		return;
	}
	if (value >= _owner->ColumnCount())
		throw std::invalid_argument(
			"DataGridColumn.DisplayIndex is outside the current schema");
	(void)_owner->SetColumnDisplayIndex(*this, value);
}

void DataGridColumn::SetSortDirection(
	std::optional<CollectionSortDirection> value)
{
	if (_sortDirection == value) return;
	_sortDirection = value;
	if (_owner) _owner->RefreshRealizedColumnHeaderPresentation(this);
}

void DataGridColumn::NotifyOwnerChanged()
{
	if (_owner) _owner->RefreshColumns();
}

#if CUI_ENABLE_DYNAMIC_XAML
void DataGridColumn::SetSortMemberPath(std::wstring value)
{
	if (_sortMemberPath == value && _compiledSortMemberPath.Empty()) return;
	_sortMemberPath = std::move(value);
	_compiledSortMemberPath = {};
	NotifyOwnerChanged();
}
#endif

void DataGridColumn::SetCompiledSortMemberPath(
	CompiledBindingPathView value)
{
	if (SameCompiledCollectionPath(_compiledSortMemberPath, value)) return;
	_compiledSortMemberPath = value;
#if CUI_ENABLE_DYNAMIC_XAML
	_sortMemberPath.clear();
#endif
	NotifyOwnerChanged();
}

#if CUI_ENABLE_DYNAMIC_XAML
void DataGridBoundColumn::SetBindingPath(std::wstring value)
{
	if (_bindingPath == value && _compiledBindingPath.Empty()
		&& !_multiBindingPlan) return;
	_bindingPath = std::move(value);
	_compiledBindingPath = {};
	_multiBindingPlan.reset();
	NotifyOwnerChanged();
}
#endif

void DataGridBoundColumn::SetElementStyle(std::wstring value)
{
	if (_elementStyle == value) return;
	_elementStyle = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetEditingElementStyle(std::wstring value)
{
	if (_editingElementStyle == value) return;
	_editingElementStyle = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetCompiledBindingPath(
	CompiledBindingPathView value)
{
	if (SameCompiledCollectionPath(_compiledBindingPath, value)
		&& !_multiBindingPlan) return;
	_compiledBindingPath = value;
	_multiBindingPlan.reset();
#if CUI_ENABLE_DYNAMIC_XAML
	_bindingPath.clear();
#endif
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetBindingMode(BindingMode value)
{
	if (_bindingMode == value) return;
	_bindingMode = value;
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetDataSourceUpdateMode(
	DataSourceUpdateMode value)
{
	if (_dataSourceUpdateMode == value) return;
	_dataSourceUpdateMode = value;
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetBindingConverter(
	std::shared_ptr<const IBindingValueConverter> value)
{
	if (_converter == value) return;
	_converter = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetFallbackValue(
	std::optional<BindingValue> value)
{
	_fallbackValue = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetTargetNullValue(
	std::optional<BindingValue> value)
{
	_targetNullValue = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetConverterParameter(
	std::optional<BindingValue> value)
{
	_converterParameter = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetStringFormat(
	std::optional<std::wstring> value)
{
	_stringFormat = std::move(value);
	NotifyOwnerChanged();
}

Binding* DataGridBoundColumn::ApplyBindingPlan(
	DataGridCell& cell,
	Control& target,
	const DependencyProperty& targetProperty,
	const BindingSourceReference& item,
	DataGridBindingSourceKind sourceKind,
	Control* elementSource,
	std::wstring_view dynamicPath,
	CompiledBindingPathView compiledPath,
	BindingMode configuredMode,
	DataSourceUpdateMode updateMode,
	const std::shared_ptr<const IBindingValueConverter>& converter,
	const std::optional<BindingValue>& fallbackValue,
	const std::optional<BindingValue>& targetNullValue,
	const std::optional<BindingValue>& converterParameter,
	const std::optional<std::wstring>& stringFormat,
	BindingMode defaultMode,
	bool forceMode)
{
	const auto mode = forceMode || configuredMode == BindingMode::Default
		? defaultMode : configuredMode;
	IBindingSource* explicitSource = nullptr;
	switch (sourceKind)
	{
	case DataGridBindingSourceKind::RowDataContext:
		break;
	case DataGridBindingSourceKind::DataGridCell:
		explicitSource = &cell;
		break;
	case DataGridBindingSourceKind::DataGridRow:
		explicitSource = cell.GetRowOwner();
		break;
	case DataGridBindingSourceKind::DataGrid:
		explicitSource = cell.GetRowOwner()
			? cell.GetRowOwner()->GetDataGridOwner() : nullptr;
		break;
	case DataGridBindingSourceKind::ElementName:
		explicitSource = elementSource;
		break;
	default:
		return nullptr;
	}
	if (sourceKind != DataGridBindingSourceKind::RowDataContext)
	{
		if (!explicitSource) return nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
		if (!dynamicPath.empty())
			return target.DataBindings.Add(
				targetProperty, explicitSource, std::wstring(dynamicPath), mode,
				updateMode, converter, fallbackValue, targetNullValue,
				converterParameter, stringFormat);
#endif
		if (!compiledPath.Empty())
			return target.DataBindings.Add(
				targetProperty, explicitSource, compiledPath, mode, updateMode,
				converter, fallbackValue, targetNullValue,
				converterParameter, stringFormat);
		return nullptr;
	}
	if (!item) return nullptr;
	// Observable source-to-target modes use the generated element's stable
	// DataContext proxy so recycled content can retarget without rebuilding.
	// OneTime and OneWayToSource intentionally do not observe proxy retargets;
	// bind those modes directly to the current item and rebuild on row reuse.
	const bool bindCurrentItem = mode == BindingMode::OneTime
		|| mode == BindingMode::OneWayToSource;
#if CUI_ENABLE_DYNAMIC_XAML
	if (!dynamicPath.empty())
	{
		if (bindCurrentItem)
			return target.DataBindings.Add(
				targetProperty, item, std::wstring(dynamicPath), mode,
				updateMode, converter, fallbackValue,
				targetNullValue, converterParameter, stringFormat);
		return target.DataBindings.Add(
			targetProperty, &target.DataContextSource(), std::wstring(dynamicPath),
			mode, updateMode, converter, fallbackValue,
			targetNullValue, converterParameter, stringFormat);
	}
#endif
	if (!compiledPath.Empty())
	{
		if (bindCurrentItem)
			return target.DataBindings.Add(
				targetProperty, item, compiledPath, mode,
				updateMode, converter, fallbackValue,
				targetNullValue, converterParameter, stringFormat);
		return target.DataBindings.Add(
			targetProperty, &target.DataContextSource(), compiledPath, mode,
			updateMode, converter, fallbackValue,
			targetNullValue, converterParameter, stringFormat);
	}
	return nullptr;
}

Binding* DataGridBoundColumn::ApplyBinding(
	DataGridCell& cell,
	Control& target,
	const DependencyProperty& targetProperty,
	const BindingSourceReference& item,
	BindingMode defaultMode,
	bool forceMode) const
{
	if (_multiBindingPlan)
	{
		(void)ApplyMultiBindingPlan(
			cell, target, targetProperty, item, *_multiBindingPlan,
			defaultMode, forceMode);
		return nullptr;
	}
	std::wstring_view dynamicPath;
#if CUI_ENABLE_DYNAMIC_XAML
	dynamicPath = _bindingPath;
#endif
	return ApplyBindingPlan(
		cell, target, targetProperty, item, _bindingSourceKind,
		_bindingElementSource.Get(), dynamicPath, _compiledBindingPath,
		_bindingMode, _dataSourceUpdateMode, _converter, _fallbackValue,
		_targetNullValue, _converterParameter, _stringFormat,
		defaultMode, forceMode);
}

MultiBinding* DataGridBoundColumn::ApplyMultiBindingPlan(
	DataGridCell& cell,
	Control& target,
	const DependencyProperty& targetProperty,
	const BindingSourceReference& item,
	const DataGridMultiBindingPlan& plan,
	BindingMode defaultMode,
	bool forceMode)
{
	if (plan.Sources.size() < 2) return nullptr;
	const auto mode = forceMode || plan.Mode == BindingMode::Default
		? defaultMode : plan.Mode;
	std::vector<MultiBindingSource> sources;
	sources.reserve(plan.Sources.size());
	for (const auto& child : plan.Sources)
	{
		IBindingSource* explicitSource = nullptr;
		switch (child.SourceKind)
		{
		case DataGridBindingSourceKind::RowDataContext:
			break;
		case DataGridBindingSourceKind::DataGridCell:
			explicitSource = &cell;
			break;
		case DataGridBindingSourceKind::DataGridRow:
			explicitSource = cell.GetRowOwner();
			break;
		case DataGridBindingSourceKind::DataGrid:
			explicitSource = cell.GetRowOwner()
				? cell.GetRowOwner()->GetDataGridOwner() : nullptr;
			break;
		case DataGridBindingSourceKind::ElementName:
			explicitSource = child.ElementSource.Get();
			break;
		default:
			return nullptr;
		}

		const auto requestedChildMode = child.Mode == BindingMode::Default
			? mode : child.Mode;
		const bool bindCurrentItem = requestedChildMode == BindingMode::OneTime
			|| requestedChildMode == BindingMode::OneWayToSource;
		MultiBindingSource source;
		if (child.SourceKind == DataGridBindingSourceKind::RowDataContext)
		{
			if (!item) return nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
			if (!child.SourcePath.empty())
			{
				source = bindCurrentItem
					? MultiBindingSource(item, child.SourcePath, child.Converter,
						child.FallbackValue, child.TargetNullValue,
						child.ConverterParameter, child.StringFormat)
					: MultiBindingSource(&target.DataContextSource(),
						child.SourcePath, child.Converter, child.FallbackValue,
						child.TargetNullValue, child.ConverterParameter,
						child.StringFormat);
			}
			else
#endif
			if (!child.CompiledPath.Empty())
			{
				source = bindCurrentItem
					? MultiBindingSource(item, child.CompiledPath,
						child.Converter, child.FallbackValue,
						child.TargetNullValue, child.ConverterParameter,
						child.StringFormat)
					: MultiBindingSource(&target.DataContextSource(),
						child.CompiledPath, child.Converter, child.FallbackValue,
						child.TargetNullValue, child.ConverterParameter,
						child.StringFormat);
			}
			else return nullptr;
		}
		else
		{
			if (!explicitSource) return nullptr;
#if CUI_ENABLE_DYNAMIC_XAML
			if (!child.SourcePath.empty())
				source = MultiBindingSource(explicitSource, child.SourcePath,
					child.Converter, child.FallbackValue,
					child.TargetNullValue, child.ConverterParameter,
					child.StringFormat);
			else
#endif
			if (!child.CompiledPath.Empty())
				source = MultiBindingSource(explicitSource, child.CompiledPath,
					child.Converter, child.FallbackValue,
					child.TargetNullValue, child.ConverterParameter,
					child.StringFormat);
			else return nullptr;
		}
		source.Mode = child.Mode;
		source.UpdateMode = child.UpdateMode;
		sources.push_back(std::move(source));
	}
	return target.DataBindings.AddMulti(
		targetProperty, std::move(sources), mode, plan.UpdateMode,
		plan.Converter, plan.FallbackValue, plan.TargetNullValue,
		plan.ConverterParameter, plan.StringFormat);
}

void DataGridBoundColumn::ApplyElementStyle(
	Control& target,
	bool editing,
	std::wstring_view defaultResourceKey) const
{
	const auto& authored = editing
		? _editingElementStyle : _elementStyle;
	if (!authored.empty())
	{
		cui::framework::StyleAccess::SetResourceKey(target, authored);
		return;
	}
	// The built-in keys are the native equivalent of each WPF derived
	// column's DefaultElementStyle/DefaultEditingElementStyle metadata.
	cui::framework::StyleAccess::SetResourceKey(
		target, std::wstring(defaultResourceKey), false, true);
}

std::unique_ptr<Control> DataGridTextColumn::GenerateElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto label = std::make_unique<Label>();
	ApplyElementStyle(
		*label, false, L"CuiDataGridTextElementStyle");
	(void)ApplyBinding(
		cell, *label, Label::TextProperty(), item, BindingMode::OneWay);
	return label;
}

std::unique_ptr<Control> DataGridTextColumn::GenerateEditingElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto editor = std::make_unique<TextBox>();
	// A DataGrid editor is hosted by the cell chrome.  Reusing the standalone
	// TextBox template would add a second rounded focus border, padding and a
	// clear button inside that chrome, unlike WPF's editing-element contract.
	ApplyElementStyle(
		*editor, true, L"CuiDataGridTextEditingElementStyle");
	(void)ApplyBinding(
		cell, *editor, TextBox::TextProperty(), item, BindingMode::TwoWay);
	return editor;
}

void DataGridCheckBoxColumn::SetIsThreeState(bool value)
{
	if (_isThreeState == value) return;
	_isThreeState = value;
	NotifyOwnerChanged();
}

std::unique_ptr<Control> DataGridCheckBoxColumn::GenerateElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto checkBox = std::make_unique<DataGridDisplayCheckBox>();
	ApplyElementStyle(
		*checkBox, false, L"CuiDataGridCheckBoxElementStyle");
	checkBox->SetFocusable(false);
	checkBox->SetIsThreeState(_isThreeState);
	(void)ApplyBinding(
		cell, *checkBox, ToggleButton::IsCheckedProperty(), item,
		BindingMode::OneWay, true);
	return checkBox;
}

std::unique_ptr<Control> DataGridCheckBoxColumn::GenerateEditingElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto checkBox = std::make_unique<CheckBox>();
	ApplyElementStyle(
		*checkBox, true, L"CuiDataGridCheckBoxElementStyle");
	checkBox->SetIsThreeState(_isThreeState);
	(void)ApplyBinding(
		cell, *checkBox, ToggleButton::IsCheckedProperty(), item,
		BindingMode::TwoWay);
	return checkBox;
}

void DataGridComboBoxColumn::SetItemsSource(BindingListReference value)
{
	if (_itemsSource == value) return;
	_itemsSource = std::move(value);
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetBindingSourceKind(
	DataGridBindingSourceKind value)
{
	if (value != DataGridBindingSourceKind::RowDataContext
		&& value != DataGridBindingSourceKind::DataGridCell
		&& value != DataGridBindingSourceKind::DataGridRow
		&& value != DataGridBindingSourceKind::DataGrid
		&& value != DataGridBindingSourceKind::ElementName)
		throw std::invalid_argument(
			"DataGridBoundColumn.BindingSourceKind is invalid");
	if (_bindingSourceKind == value) return;
	_bindingSourceKind = value;
	if (value != DataGridBindingSourceKind::ElementName)
		_bindingElementSource.Reset();
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetBindingElementSource(Control* value)
{
	if (_bindingElementSource.Get() == value
		&& (_bindingElementSource.HasValue() == (value != nullptr))) return;
	_bindingElementSource = value;
	NotifyOwnerChanged();
}

void DataGridBoundColumn::SetMultiBindingPlan(
	std::shared_ptr<const DataGridMultiBindingPlan> value)
{
	if (value && value->Sources.size() < 2)
		throw std::invalid_argument(
			"DataGridBoundColumn.MultiBindingPlan needs at least two sources");
	if (_multiBindingPlan == value) return;
	_multiBindingPlan = std::move(value);
	NotifyOwnerChanged();
}

void DataGridComboBoxColumn::SetSelectionBinding(
	DataGridComboBoxSelectionBinding value)
{
	if (value != DataGridComboBoxSelectionBinding::SelectedItem
		&& value != DataGridComboBoxSelectionBinding::SelectedValue)
		throw std::invalid_argument(
			"DataGridComboBoxColumn.SelectionBinding is invalid");
	if (_selectionBinding == value) return;
	_selectionBinding = value;
	NotifyOwnerChanged();
}

#if CUI_ENABLE_DYNAMIC_XAML
void DataGridComboBoxColumn::SetDisplayMemberPath(std::wstring value)
{
	if (_displayMemberPath == value && _compiledDisplayMemberPath.Empty())
		return;
	_displayMemberPath = std::move(value);
	_compiledDisplayMemberPath = {};
	NotifyOwnerChanged();
}

void DataGridComboBoxColumn::SetSelectedValuePath(std::wstring value)
{
	if (_selectedValuePath == value && _compiledSelectedValuePath.Empty())
		return;
	_selectedValuePath = std::move(value);
	_compiledSelectedValuePath = {};
	NotifyOwnerChanged();
}
#endif

void DataGridComboBoxColumn::SetCompiledDisplayMemberPath(
	CompiledBindingPathView value)
{
	if (SameCompiledCollectionPath(_compiledDisplayMemberPath, value)) return;
	_compiledDisplayMemberPath = value;
#if CUI_ENABLE_DYNAMIC_XAML
	_displayMemberPath.clear();
#endif
	NotifyOwnerChanged();
}

void DataGridComboBoxColumn::SetCompiledSelectedValuePath(
	CompiledBindingPathView value)
{
	if (SameCompiledCollectionPath(_compiledSelectedValuePath, value)) return;
	_compiledSelectedValuePath = value;
#if CUI_ENABLE_DYNAMIC_XAML
	_selectedValuePath.clear();
#endif
	NotifyOwnerChanged();
}

std::unique_ptr<ComboBox> DataGridComboBoxColumn::GenerateComboBox(
	DataGridCell& cell,
	const BindingSourceReference& item,
	bool editing) const
{
	std::unique_ptr<ComboBox> combo = editing
		? std::make_unique<ComboBox>()
		: std::make_unique<DataGridDisplayComboBox>();
	ApplyElementStyle(
		*combo, editing, editing
			? L"CuiDataGridComboBoxEditingElementStyle"
			: L"CuiDataGridComboBoxElementStyle");
	combo->SetItemsSource(_itemsSource);
	if (!_compiledDisplayMemberPath.Empty())
		combo->SetCompiledDisplayMemberPath(_compiledDisplayMemberPath);
#if CUI_ENABLE_DYNAMIC_XAML
	else if (!_displayMemberPath.empty())
		combo->SetDisplayMemberPath(_displayMemberPath);
#endif
	if (!_compiledSelectedValuePath.Empty())
		combo->SetCompiledSelectedValuePath(_compiledSelectedValuePath);
#if CUI_ENABLE_DYNAMIC_XAML
	else if (!_selectedValuePath.empty())
		combo->SetSelectedValuePath(_selectedValuePath);
#endif
	if (!editing)
	{
		combo->SetFocusable(false);
		combo->SetIsTabStop(false);
	}
	const auto& selectionProperty = _selectionBinding
		== DataGridComboBoxSelectionBinding::SelectedValue
		? Selector::SelectedValueProperty()
		: Selector::SelectedItemProperty();
	(void)ApplyBinding(
		cell, *combo, selectionProperty, item,
		editing ? BindingMode::TwoWay : BindingMode::OneWay,
		!editing);
	return combo;
}

std::unique_ptr<Control> DataGridComboBoxColumn::GenerateElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	return GenerateComboBox(cell, item, false);
}

std::unique_ptr<Control> DataGridComboBoxColumn::GenerateEditingElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	return GenerateComboBox(cell, item, true);
}

void DataGridHyperlinkColumn::SetTargetName(std::wstring value)
{
	if (_targetName == value) return;
	_targetName = std::move(value);
	NotifyOwnerChanged();
}

#if CUI_ENABLE_DYNAMIC_XAML
void DataGridHyperlinkColumn::SetContentBindingPath(std::wstring value)
{
	if (_contentBindingPath == value
		&& _compiledContentBindingPath.Empty()
		&& !_contentMultiBindingPlan) return;
	_contentBindingPath = std::move(value);
	_compiledContentBindingPath = {};
	_contentMultiBindingPlan.reset();
	NotifyOwnerChanged();
}
#endif

void DataGridHyperlinkColumn::SetCompiledContentBindingPath(
	CompiledBindingPathView value)
{
	if (SameCompiledCollectionPath(_compiledContentBindingPath, value)
		&& !_contentMultiBindingPlan) return;
	_compiledContentBindingPath = value;
	_contentMultiBindingPlan.reset();
#if CUI_ENABLE_DYNAMIC_XAML
	_contentBindingPath.clear();
#endif
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentBindingSourceKind(
	DataGridBindingSourceKind value)
{
	if (value != DataGridBindingSourceKind::RowDataContext
		&& value != DataGridBindingSourceKind::DataGridCell
		&& value != DataGridBindingSourceKind::DataGridRow
		&& value != DataGridBindingSourceKind::DataGrid
		&& value != DataGridBindingSourceKind::ElementName)
		throw std::invalid_argument(
			"DataGridHyperlinkColumn.ContentBindingSourceKind is invalid");
	if (_contentBindingSourceKind == value) return;
	_contentBindingSourceKind = value;
	if (value != DataGridBindingSourceKind::ElementName)
		_contentBindingElementSource.Reset();
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentBindingElementSource(Control* value)
{
	if (_contentBindingElementSource.Get() == value
		&& (_contentBindingElementSource.HasValue() == (value != nullptr))) return;
	_contentBindingElementSource = value;
	NotifyOwnerChanged();
}

bool DataGridHyperlinkColumn::HasContentBinding() const noexcept
{
	if (_contentMultiBindingPlan) return true;
#if CUI_ENABLE_DYNAMIC_XAML
	if (!_contentBindingPath.empty()) return true;
#endif
	return !_compiledContentBindingPath.Empty();
}

void DataGridHyperlinkColumn::ClearContentBinding()
{
	if (!HasContentBinding()
		&& _contentBindingMode == BindingMode::Default
		&& _contentDataSourceUpdateMode == DataSourceUpdateMode::Default
		&& _contentBindingSourceKind
			== DataGridBindingSourceKind::RowDataContext
		&& !_contentBindingElementSource.HasValue()
		&& !_contentConverter && !_contentFallbackValue
		&& !_contentTargetNullValue && !_contentConverterParameter
		&& !_contentStringFormat) return;
#if CUI_ENABLE_DYNAMIC_XAML
	_contentBindingPath.clear();
#endif
	_compiledContentBindingPath = {};
	_contentMultiBindingPlan.reset();
	_contentBindingSourceKind = DataGridBindingSourceKind::RowDataContext;
	_contentBindingElementSource.Reset();
	_contentBindingMode = BindingMode::Default;
	_contentDataSourceUpdateMode = DataSourceUpdateMode::Default;
	_contentConverter.reset();
	_contentFallbackValue.reset();
	_contentTargetNullValue.reset();
	_contentConverterParameter.reset();
	_contentStringFormat.reset();
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentMultiBindingPlan(
	std::shared_ptr<const DataGridMultiBindingPlan> value)
{
	if (value && value->Sources.size() < 2)
		throw std::invalid_argument(
			"DataGridHyperlinkColumn.ContentMultiBindingPlan needs at least two sources");
	if (_contentMultiBindingPlan == value) return;
	_contentMultiBindingPlan = std::move(value);
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentBindingMode(BindingMode value)
{
	if (_contentBindingMode == value) return;
	_contentBindingMode = value;
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentDataSourceUpdateMode(
	DataSourceUpdateMode value)
{
	if (_contentDataSourceUpdateMode == value) return;
	_contentDataSourceUpdateMode = value;
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentBindingConverter(
	std::shared_ptr<const IBindingValueConverter> value)
{
	if (_contentConverter == value) return;
	_contentConverter = std::move(value);
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentFallbackValue(
	std::optional<BindingValue> value)
{
	_contentFallbackValue = std::move(value);
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentTargetNullValue(
	std::optional<BindingValue> value)
{
	_contentTargetNullValue = std::move(value);
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentConverterParameter(
	std::optional<BindingValue> value)
{
	_contentConverterParameter = std::move(value);
	NotifyOwnerChanged();
}

void DataGridHyperlinkColumn::SetContentStringFormat(
	std::optional<std::wstring> value)
{
	_contentStringFormat = std::move(value);
	NotifyOwnerChanged();
}

Binding* DataGridHyperlinkColumn::ApplyContentBinding(
	DataGridCell& cell,
	Control& target,
	const DependencyProperty& targetProperty,
	const BindingSourceReference& item) const
{
	if (_contentMultiBindingPlan)
	{
		(void)ApplyMultiBindingPlan(
			cell, target, targetProperty, item, *_contentMultiBindingPlan,
			BindingMode::OneWay);
		return nullptr;
	}
	std::wstring_view dynamicPath;
#if CUI_ENABLE_DYNAMIC_XAML
	dynamicPath = _contentBindingPath;
#endif
	return ApplyBindingPlan(
		cell, target, targetProperty, item, _contentBindingSourceKind,
		_contentBindingElementSource.Get(), dynamicPath,
		_compiledContentBindingPath, _contentBindingMode,
		_contentDataSourceUpdateMode, _contentConverter,
		_contentFallbackValue, _contentTargetNullValue,
		_contentConverterParameter, _contentStringFormat,
		BindingMode::OneWay);
}

std::unique_ptr<Control> DataGridHyperlinkColumn::GenerateElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto hyperlink = std::make_unique<DataGridHyperlink>();
	ApplyElementStyle(
		*hyperlink, false, L"CuiDataGridHyperlinkElementStyle");
	hyperlink->SetTargetName(_targetName);
	(void)ApplyBinding(
		cell, *hyperlink, DataGridHyperlink::NavigateUriProperty(), item,
		BindingMode::OneWay);
	if (HasContentBinding())
		(void)ApplyContentBinding(
			cell, *hyperlink, ContentControl::ContentProperty(), item);
	else
		(void)ApplyBinding(
			cell, *hyperlink, ContentControl::ContentProperty(), item,
			BindingMode::OneWay);
	return hyperlink;
}

std::unique_ptr<Control> DataGridHyperlinkColumn::GenerateEditingElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	auto editor = std::make_unique<TextBox>();
	ApplyElementStyle(
		*editor, true, L"CuiDataGridTextEditingElementStyle");
	(void)ApplyBinding(
		cell, *editor, TextBox::TextProperty(), item, BindingMode::TwoWay);
	return editor;
}

void DataGridTemplateColumn::SetCellTemplate(ItemTemplateReference value)
{
	if (_cellTemplate == value) return;
	_cellTemplate = std::move(value);
	NotifyOwnerChanged();
}

void DataGridTemplateColumn::SetCellEditingTemplate(
	ItemTemplateReference value)
{
	if (_cellEditingTemplate == value) return;
	_cellEditingTemplate = std::move(value);
	NotifyOwnerChanged();
}

void DataGridTemplateColumn::SetCellTemplateSelector(
	ItemTemplateSelectorReference value)
{
	if (_cellTemplateSelector == value) return;
	_cellTemplateSelector = std::move(value);
	NotifyOwnerChanged();
}

void DataGridTemplateColumn::SetCellEditingTemplateSelector(
	ItemTemplateSelectorReference value)
{
	if (_cellEditingTemplateSelector == value) return;
	_cellEditingTemplateSelector = std::move(value);
	NotifyOwnerChanged();
}

std::unique_ptr<Control> DataGridTemplateColumn::BuildTemplateContent(
	DataGridCell& cell,
	const BindingSourceReference& item,
	bool editing) const
{
	// Match WPF precedence: an editing pair is used when either editing member
	// is present; otherwise the display pair is the editing fallback. Within a
	// pair, an explicit template wins over its selector.
	ItemTemplateReference contentTemplate;
	ItemTemplateSelectorReference contentSelector;
	if (editing)
	{
		contentTemplate = _cellEditingTemplate;
		contentSelector = _cellEditingTemplateSelector;
	}
	if (!contentTemplate && !contentSelector)
	{
		contentTemplate = _cellTemplate;
		contentSelector = _cellTemplateSelector;
	}
	const size_t rowIndex = cell.GetRowOwner()
		? cell.GetRowOwner()->ItemIndex() : 0;
	if (!contentTemplate && contentSelector)
	{
		// Pin the shared selector before entering application code. The callback
		// may remove this column or destroy the owning DataGrid/cell.
		const auto* expectedColumn = this;
		const ControlWeakReference cellLifetime(&cell);
		contentTemplate = contentSelector.Get()->SelectTemplate(item, cell);
		auto* liveCell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		auto* liveRow = liveCell ? liveCell->GetRowOwner() : nullptr;
		if (!liveCell || liveCell->GetColumn() != expectedColumn
			|| !liveRow || !liveRow->GetDataGridOwner()) return {};
	}
	if (!contentTemplate) return std::make_unique<Label>();
	// The selected template is likewise pinned across its user callback. Do not
	// touch the column or cell after entering Build.
	return contentTemplate.Get()->Build(item, rowIndex);
}

std::unique_ptr<Control> DataGridTemplateColumn::GenerateElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	return BuildTemplateContent(cell, item, false);
}

std::unique_ptr<Control> DataGridTemplateColumn::GenerateEditingElement(
	DataGridCell& cell, const BindingSourceReference& item) const
{
	return BuildTemplateContent(cell, item, true);
}

const DependencyProperty& DataGridCell::IsSelectedProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridCell, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGridCell, bool>(
			DependencyPropertyRegistrationLiteral(L"IsSelected"),
			[](DataGridCell& target) { return target._isSelected; },
			[](DataGridCell& target, const bool& value)
			{ target.ApplyIsSelectedValue(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGridCell::IsNewItemPlaceholderProperty()
{
	return IsNewItemPlaceholderPropertyKey().Property();
}

const DependencyPropertyKey& DataGridCell::IsNewItemPlaceholderPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridCell, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 20;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGridCell, bool>(
				DependencyPropertyRegistrationLiteral(
					L"IsNewItemPlaceholder"),
				[](DataGridCell& target)
					{ return target._isNewItemPlaceholder; },
				[](DataGridCell& target, const bool& value)
				{
					(void)target.SetReadOnlyPropertyField(
						IsNewItemPlaceholderPropertyKey(),
						target._isNewItemPlaceholder, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

void DataGridCell::RegisterDependencyProperties()
{
	ContentControl::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)IsSelectedProperty();
	(void)IsNewItemPlaceholderProperty();
#endif
	CUI_DESIGN_METADATA_ONLY(
	(void)DataGridCellFocusableMetadataRelation();
	)
}

DataGridCell::DataGridCell()
{
	RegisterDependencyProperties();
	auto activateCell =
		[this](Control*, MouseEventArgs& args)
		{
			if (args.ChangedButton != MouseButton::Left || !_row || !_column)
				return;
			if (_isEditing && _editingElement)
			{
				for (auto* current = args.OriginalSource; current;
					current = current->GetRoutedParent())
				{
					if (current != _editingElement) continue;
					// The editor already processed this pointer report.  Keep its
					// caret/capture/focus instead of treating the bubble as another
					// cell activation or a ListBox row click.
					args.Handled = true;
					return;
				}
			}
			auto* owner = _row->GetDataGridOwner();
			if (!owner) return;
			ControlWeakReference cellLifetime(this);
			ControlWeakReference ownerLifetime(owner);
			const size_t rowIndex = _row->ItemIndex();
			const size_t columnIndex = _columnIndex;
			const bool current = IsKeyboardFocusWithin
				&& owner->GetCurrentCell().IsValid()
				&& owner->GetCurrentCell().Item == _item
				&& owner->GetCurrentCell().Column == _column;
			const bool checkBoxActivation =
				[&]()
				{
					if (!dynamic_cast<DataGridCheckBoxColumn*>(_column))
						return false;
					// WPF's display CheckBox is not hit-testable. The cell still
					// enters edit on a repeated press anywhere, but preparation
					// toggles only when that press was over the checkbox glyph.
					auto* display = dynamic_cast<CheckBox*>(GetVisualContent());
					if (!display) return false;
					const auto renderPoint = args.HasRootPosition
						? D2D1::Point2F(args.RootX, args.RootY)
						: ToRenderSpace(*this,
							static_cast<float>(args.X),
							static_cast<float>(args.Y));
					D2D1_POINT_2F local{};
					if (!display->TryTransformRenderPointToLocal(
						renderPoint, local)
						|| !display->IsRenderPointInsideClip(renderPoint))
						return false;
					const auto size = display->GetActualSizeDip();
					return local.x >= 0.0f && local.y >= 0.0f
						&& local.x < size.width && local.y < size.height;
				}();
			args.Handled = true;
			// SetCurrentCell commits the old editor.  Do that before moving
			// focus, otherwise TextBox LostFocus can update the source before
			// CellEditEnding has a chance to cancel the transition.
			if (!owner->SetCurrentCell(rowIndex, columnIndex)) return;
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* liveCell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			if (!owner || !liveCell) return;
			if (auto* window = owner->GetPresentationWindow())
				window->SetKeyboardFocus(liveCell, true);
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!owner || !cellLifetime.Get()) return;
			const bool selected = owner->ApplySelectionForCellInput(
				rowIndex, columnIndex, args.Modifiers, true, true);
			if (!current || !selected
				|| HasModifier(args.Modifiers, ModifierKeys::Control)) return;
			if (!cellLifetime.Get()) return;
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (owner) (void)owner->BeginEditCore(
				&args, checkBoxActivation);
		};
	RetainEventConnection(OnMouseDown.Subscribe(activateCell));
	// WPF raises the second physical press of a native double-click through the
	// same MouseLeftButtonDown class handler. CUI exposes WM_*DBLCLK as its own
	// routed event, so explicitly reuse the cell-activation transaction here.
	RetainEventConnection(OnMouseDoubleClick.Subscribe(std::move(activateCell)));
}

const DependencyPropertyMetadata*
DataGridCell::ResolveExactDependencyPropertyMetadata(
	const DependencyProperty& property) const
{
	if (&property == &Control::FocusableProperty())
		return &DataGridCellFocusableMetadataRelation().Metadata();
	return ContentControl::ResolveExactDependencyPropertyMetadata(property);
}

void DataGridCell::SetIsSelected(bool value)
{
	(void)TrySetPropertyValue(IsSelectedProperty(), BindingValue(value));
}

void DataGridCell::ApplyIsSelectedValue(bool value)
{
	if (_isSelected == value) return;
	if (!_synchronizingIsSelected && _row)
	{
		if (auto* owner = _row->GetDataGridOwner())
		{
			ControlWeakReference lifetime(this);
			owner->OnCellIsSelectedChanged(*this, value);
			auto* live = dynamic_cast<DataGridCell*>(lifetime.Get());
			if (!live || live->_isSelected == value) return;
		}
	}
	if (!SetPropertyField(IsSelectedProperty(), _isSelected, value)) return;
	SetStyleState(ControlStyleState::Selected, value);
	if (_suppressIsSelectedRoutedEvents) return;
	RoutedEventArgs args;
	if (value) Selected(this, args);
	else Unselected(this, args);
}

void DataGridCell::SetCurrentIsSelected(
	bool value, bool suppressRoutedEvents)
{
	if (_isSelected == value) return;
	ControlWeakReference lifetime(this);
	const bool previous = _synchronizingIsSelected;
	const bool previousSuppression = _suppressIsSelectedRoutedEvents;
	_synchronizingIsSelected = true;
	_suppressIsSelectedRoutedEvents = suppressRoutedEvents;
	try
	{
		(void)SetCurrentPropertyField(IsSelectedProperty(), _isSelected, value);
		if (auto* live = dynamic_cast<DataGridCell*>(lifetime.Get()))
		{
			live->_synchronizingIsSelected = previous;
			live->_suppressIsSelectedRoutedEvents = previousSuppression;
		}
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGridCell*>(lifetime.Get()))
		{
			live->_synchronizingIsSelected = previous;
			live->_suppressIsSelectedRoutedEvents = previousSuppression;
		}
		throw;
	}
}

void DataGridCell::SetCurrentIsNewItemPlaceholder(bool value)
{
	(void)SetReadOnlyPropertyField(
		IsNewItemPlaceholderPropertyKey(), _isNewItemPlaceholder, value);
}

bool DataGridCell::GetIsReadOnly() const noexcept
{
	if (!_row || !_column || !_row->GetDataGridOwner()) return true;
	if (_row->GetDataGridOwner()->GetIsReadOnly()
		|| _column->GetIsReadOnly()) return true;
	if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(_column))
	{
		return bound->GetBindingMode() == BindingMode::OneWay
			|| bound->GetBindingMode() == BindingMode::OneTime;
	}
	return false;
}

bool DataGridCell::Initialize(
	DataGridRow& row,
	DataGridColumn& column,
	const BindingSourceReference& item,
	size_t columnIndex,
	std::wstring* outError)
{
	_row = &row;
	_column = &column;
	_item = item;
	_columnIndex = columnIndex;
	(void)SetDataContext(item);
	auto* owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column
		|| column.GetVisibility() != Visibility::Visible) return false;
	SetCurrentIsNewItemPlaceholder(owner->IsNewItemPlaceholder(item));
	owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column
		|| column.GetVisibility() != Visibility::Visible) return false;
	cui::framework::StyleAccess::SetResourceKey(
		*this, owner->EffectiveCellStyle(column));
	owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column
		|| column.GetVisibility() != Visibility::Visible) return false;
	const bool vertical = HasVerticalGridLines(
		owner->GetGridLinesVisibility());
	SetBorderThickness(Thickness(
		0.0f, 0.0f, vertical ? 1.0f : 0.0f, 0.0f));
	owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column) return false;
	SetBorderBrush(owner->GetVerticalGridLinesBrush());
	owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column) return false;
	if (!ReplaceContent(false, outError)) return false;
	owner = row.GetDataGridOwner();
	if (!owner || columnIndex >= owner->ColumnCount()
		|| owner->GetColumnFromDisplayIndex(columnIndex) != &column) return false;
	SetCurrentIsSelected(owner->IsCellSelected(row.ItemIndex(), columnIndex));
	return true;
}

bool DataGridCell::ReplaceContent(bool editing, std::wstring* outError)
{
	if (outError) outError->clear();
	if (!_column)
	{
		if (outError) *outError = L"DataGridCell 缺少 Column。";
		return false;
	}
	const ControlWeakReference cellLifetime(this);
	auto* column = _column;
	const auto item = _item;
	try
	{
		auto content = editing
			? column->GenerateEditingElement(*this, item)
			: column->GenerateElement(*this, item);
		auto* live = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!live) return false;
		if (!content) content = std::make_unique<Label>();
		auto* raw = content.get();
		(void)live->SetVisualContent(std::move(content));
		live = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!live) return false;
		if (live->_row) live->_row->InvalidateValidationVisualTracking();
		live->_editingElement = editing ? raw : nullptr;
		live->_isEditing = editing;
		return true;
	}
	catch (const std::exception& error)
	{
		if (outError)
		{
			const auto* first = reinterpret_cast<const unsigned char*>(
				error.what());
			while (*first) outError->push_back(
				static_cast<wchar_t>(*first++));
		}
		return false;
	}
	catch (...)
	{
		if (outError) *outError = L"DataGridCell 内容生成失败。";
		return false;
	}
}

bool DataGridCell::BeginEdit()
{
	if (!_row || !_row->GetDataGridOwner()) return false;
	auto* owner = _row->GetDataGridOwner();
	ControlWeakReference ownerLifetime(owner);
	if (!owner->SetCurrentCell(_row->ItemIndex(), _columnIndex)) return false;
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	return owner ? owner->BeginEdit() : false;
}

bool DataGridCell::CommitEdit()
{
	return _row && _row->GetDataGridOwner()
		? _row->GetDataGridOwner()->CommitEdit() : false;
}

bool DataGridCell::CancelEdit()
{
	return _row && _row->GetDataGridOwner()
		? _row->GetDataGridOwner()->CancelEdit() : false;
}

bool DataGridCell::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::KeyDown
		&& _row && _row->GetDataGridOwner())
	{
		auto* owner = _row->GetDataGridOwner();
		if (input.Key == Key::F2)
		{
			KeyEventArgs args(input.Key, input.Modifiers);
			return owner->BeginEdit(&args);
		}
		if (input.Key == Key::Space && !_isEditing
			&& dynamic_cast<DataGridCheckBoxColumn*>(_column))
		{
			KeyEventArgs args(input.Key, input.Modifiers);
			return owner->BeginEditCore(&args, true);
		}
		if (input.Key == Key::Return && _isEditing)
			return owner->CommitEdit();
		if (input.Key == Key::Escape && _isEditing)
			return owner->CancelEdit();
	}
	return ContentControl::ProcessInput(input);
}

void DataGridCell::OnRender()
{
	// Grid-line and background chrome belongs to the declarative Cell template.
	ContentControl::OnRender();
}

const DependencyProperty& DataGridRow::IsEditingProperty()
{
	return IsEditingPropertyKey().Property();
}

const DependencyPropertyKey& DataGridRow::IsEditingPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 20;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGridRow, bool>(
				DependencyPropertyRegistrationLiteral(L"IsEditing"),
				[](DataGridRow& target) { return target._isEditing; },
				[](DataGridRow& target, const bool& value)
				{
					(void)target.SetReadOnlyPropertyField(
						IsEditingPropertyKey(), target._isEditing, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

const DependencyProperty& DataGridRow::IsNewItemProperty()
{
	return IsNewItemPropertyKey().Property();
}

const DependencyPropertyKey& DataGridRow::IsNewItemPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 30;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGridRow, bool>(
				DependencyPropertyRegistrationLiteral(L"IsNewItem"),
				[](DataGridRow& target) { return target._isNewItem; },
				[](DataGridRow& target, const bool& value)
				{
					(void)target.SetReadOnlyPropertyField(
						IsNewItemPropertyKey(), target._isNewItem, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

const DependencyProperty& DataGridRow::HasValidationErrorProperty()
{
	return HasValidationErrorPropertyKey().Property();
}

const DependencyPropertyKey& DataGridRow::HasValidationErrorPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Validation";
		options.Design.CategoryOrder = 400;
		options.Design.Order = 10;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGridRow, bool>(
				DependencyPropertyRegistrationLiteral(L"HasValidationError"),
				[](DataGridRow& target) { return target._hasValidationError; },
				[](DataGridRow& target, const bool& value)
				{
					(void)target.SetReadOnlyPropertyField(
						HasValidationErrorPropertyKey(),
						target._hasValidationError, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

const DependencyProperty& DataGridRow::ValidationErrorsProperty()
{
	return ValidationErrorsPropertyKey().Property();
}

const DependencyPropertyKey& DataGridRow::ValidationErrorsPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow,
			std::vector<BindingValidationResult>> options;
		options.DefaultValue = std::vector<BindingValidationResult>{};
		options.Equals = [](
			const std::vector<BindingValidationResult>& left,
			const std::vector<BindingValidationResult>& right)
				{ return left == right; };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Validation";
		options.Design.CategoryOrder = 400;
		options.Design.Order = 20;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Editor = DependencyPropertyEditorKind::Text;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<DataGridRow,
			std::vector<BindingValidationResult>>(
				DependencyPropertyRegistrationLiteral(L"ValidationErrors"),
				[](DataGridRow& target) { return target._validationErrors; },
				[](DataGridRow& target,
					const std::vector<BindingValidationResult>& value)
				{
					(void)target.SetReadOnlyPropertyField(
						ValidationErrorsPropertyKey(),
						target._validationErrors, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

const DependencyProperty& DataGridRow::ValidationErrorTemplateProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, ControlTemplateReference> options;
		options.DefaultValue = ControlTemplateReference{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure;
		options.Equals = [](
			const ControlTemplateReference& left,
			const ControlTemplateReference& right) { return left == right; };
		options.Coerce = [](DataGridRow& target,
			const ControlTemplateReference& value)
			-> std::optional<ControlTemplateReference>
		{
			if (value) return value;
			auto* owner = target.GetDataGridOwner();
			return owner
				? owner->GetRowValidationErrorTemplate()
				: ControlTemplateReference{};
		};
		options.Changed = [](DataGridRow& target,
			const ControlTemplateReference&,
			const ControlTemplateReference&)
			{ target.UpdateValidationVisual(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Validation";
		options.Design.CategoryOrder = 400;
		options.Design.Order = 30;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGridRow,
			ControlTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"ValidationErrorTemplate"),
				[](DataGridRow& target)
					{ return target._validationErrorTemplate; },
				[](DataGridRow& target,
					const ControlTemplateReference& value)
					{ target._validationErrorTemplate = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGridRow::DetailsTemplateProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, ItemTemplateReference> options;
		options.DefaultValue = ItemTemplateReference{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure;
		options.Equals = [](const ItemTemplateReference& left,
			const ItemTemplateReference& right) { return left == right; };
		options.Coerce = [](DataGridRow& target,
			const ItemTemplateReference& value)
			-> std::optional<ItemTemplateReference>
		{
			if (target.HasPropertyValue(DetailsTemplateProperty(),
				DependencyPropertyValueSource::Local)) return value;
			auto* owner = target.GetDataGridOwner();
			return owner ? owner->GetRowDetailsTemplate()
				: ItemTemplateReference{};
		};
		options.Changed = [](DataGridRow& target,
			const ItemTemplateReference&, const ItemTemplateReference&)
		{
			if (target._restoringDetailsOverrides) return;
			const ControlWeakReference lifetime(&target);
			auto* owner = target.GetDataGridOwner();
			if (owner && !owner->PersistRowPresentationOverride(target)) return;
			auto* row = dynamic_cast<DataGridRow*>(lifetime.Get());
			if (!row) return;
			const size_t revision = row->_detailsPresentationRevision;
			(void)row->CoerceValue(DetailsVisibilityProperty());
			row = dynamic_cast<DataGridRow*>(lifetime.Get());
			if (row && row->_detailsPresentationRevision == revision)
				(void)row->UpdateDetailsPresentation();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 58;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGridRow,
			ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"DetailsTemplate"),
				[](DataGridRow& target) { return target._detailsTemplate; },
				[](DataGridRow& target, const ItemTemplateReference& value)
					{ target._detailsTemplate = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGridRow::DetailsTemplateSelectorProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow,
			ItemTemplateSelectorReference> options;
		options.DefaultValue = ItemTemplateSelectorReference{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure;
		options.Equals = [](const ItemTemplateSelectorReference& left,
			const ItemTemplateSelectorReference& right) { return left == right; };
		options.Coerce = [](DataGridRow& target,
			const ItemTemplateSelectorReference& value)
			-> std::optional<ItemTemplateSelectorReference>
		{
			if (target.HasPropertyValue(DetailsTemplateSelectorProperty(),
				DependencyPropertyValueSource::Local)) return value;
			auto* owner = target.GetDataGridOwner();
			return owner ? owner->GetRowDetailsTemplateSelector()
				: ItemTemplateSelectorReference{};
		};
		options.Changed = [](DataGridRow& target,
			const ItemTemplateSelectorReference&,
			const ItemTemplateSelectorReference&)
		{
			if (target._restoringDetailsOverrides) return;
			const ControlWeakReference lifetime(&target);
			auto* owner = target.GetDataGridOwner();
			if (owner && !owner->PersistRowPresentationOverride(target)) return;
			auto* row = dynamic_cast<DataGridRow*>(lifetime.Get());
			if (!row) return;
			const size_t revision = row->_detailsPresentationRevision;
			(void)row->CoerceValue(DetailsVisibilityProperty());
			row = dynamic_cast<DataGridRow*>(lifetime.Get());
			if (row && row->_detailsPresentationRevision == revision)
				(void)row->UpdateDetailsPresentation();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 59;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGridRow,
			ItemTemplateSelectorReference>(
				DependencyPropertyRegistrationLiteral(
					L"DetailsTemplateSelector"),
				[](DataGridRow& target)
					{ return target._detailsTemplateSelector; },
				[](DataGridRow& target,
					const ItemTemplateSelectorReference& value)
					{ target._detailsTemplateSelector = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGridRow::DetailsVisibilityProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRow, int> options;
		options.DefaultValue = static_cast<int>(::Visibility::Collapsed);
		options.Flags = DependencyPropertyFlags::AffectsMeasure;
		options.Validate = [](const int& value)
		{
			return value == static_cast<int>(::Visibility::Visible)
				|| value == static_cast<int>(::Visibility::Hidden)
				|| value == static_cast<int>(::Visibility::Collapsed);
		};
		options.Coerce = [](DataGridRow& target,
			const int& value) -> std::optional<int>
		{
			if (target.HasPropertyValue(DetailsVisibilityProperty(),
				DependencyPropertyValueSource::Local)) return value;
			auto* owner = target.GetDataGridOwner();
			if (!owner || owner->IsNewItemPlaceholder(target._item))
				return static_cast<int>(::Visibility::Collapsed);
			const bool hasDetails = target.GetDetailsTemplate()
				|| target.GetDetailsTemplateSelector();
			if (!hasDetails)
				return static_cast<int>(::Visibility::Collapsed);
			const auto mode = owner->GetRowDetailsVisibilityMode();
			const bool visible = mode == DataGridRowDetailsVisibilityMode::Visible
				|| (mode
					== DataGridRowDetailsVisibilityMode::VisibleWhenSelected
					&& target.GetIsSelected());
			return static_cast<int>(visible
				? ::Visibility::Visible : ::Visibility::Collapsed);
		};
		options.Changed = [](DataGridRow& target, const int&, const int&)
		{
			if (target._restoringDetailsOverrides) return;
			auto* owner = target.GetDataGridOwner();
			if (owner && !owner->PersistRowPresentationOverride(target)) return;
			(void)target.UpdateDetailsPresentation();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 60;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Browsable = false;
		options.Design.Choices = {
			{ L"Visible", BindingValue(static_cast<int>(::Visibility::Visible)) },
			{ L"Hidden", BindingValue(static_cast<int>(::Visibility::Hidden)) },
			{ L"Collapsed", BindingValue(static_cast<int>(::Visibility::Collapsed)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGridRow, int>(
			DependencyPropertyRegistrationLiteral(L"DetailsVisibility"),
			[](DataGridRow& target)
				{ return static_cast<int>(target._detailsVisibility); },
			[](DataGridRow& target, const int& value)
				{ target._detailsVisibility = static_cast<::Visibility>(value); },
			{}, std::move(options));
	}();
	return *registration;
}

void DataGridRow::RegisterDependencyProperties()
{
	ListBoxItem::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)IsEditingProperty();
	(void)IsNewItemProperty();
	(void)HasValidationErrorProperty();
	(void)ValidationErrorsProperty();
	(void)ValidationErrorTemplateProperty();
	(void)DetailsTemplateProperty();
	(void)DetailsTemplateSelectorProperty();
	(void)DetailsVisibilityProperty();
#endif
}

void DataGridRow::SetCurrentIsEditing(bool value)
{
	(void)SetReadOnlyPropertyField(
		IsEditingPropertyKey(), _isEditing, value);
}

void DataGridRow::SetCurrentIsNewItem(bool value)
{
	(void)SetReadOnlyPropertyField(
		IsNewItemPropertyKey(), _isNewItem, value);
}

void DataGridRow::SetValidationErrorTemplate(
	ControlTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		ValidationErrorTemplateProperty(), std::move(value));
}

void DataGridRow::SetDetailsTemplate(ItemTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		DetailsTemplateProperty(), std::move(value));
}

bool DataGridRow::ClearDetailsTemplate()
{
	return ClearPropertyValue(DetailsTemplateProperty());
}

void DataGridRow::SetDetailsTemplateSelector(
	ItemTemplateSelectorReference value)
{
	(void)SetDependencyPropertyValue(
		DetailsTemplateSelectorProperty(), std::move(value));
}

bool DataGridRow::ClearDetailsTemplateSelector()
{
	return ClearPropertyValue(DetailsTemplateSelectorProperty());
}

void DataGridRow::SetDetailsVisibility(::Visibility value)
{
	(void)TrySetPropertyValue(DetailsVisibilityProperty(),
		BindingValue(static_cast<int>(value)));
}

bool DataGridRow::ClearDetailsVisibility()
{
	return ClearPropertyValue(DetailsVisibilityProperty());
}

std::wstring DataGridRow::GetValidationSummary(size_t maxIssues) const
{
	std::vector<BindingValidationIssue> unique;
	unique.reserve(_validationErrors.size());
	for (const auto& result : _validationErrors)
		if (std::find(unique.begin(), unique.end(), result.Issue) == unique.end())
			unique.push_back(result.Issue);
	const size_t visibleCount = maxIssues == 0
		? unique.size() : (std::min)(unique.size(), maxIssues);
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
		summary += L"+" + std::to_wstring(unique.size() - visibleCount);
	}
	return summary;
}

DataGridRow::DataGridRow()
{
	RegisterDependencyProperties();
	RetainEventConnection(OnPropertyValueChanged.Subscribe(
		[this](DependencyObject*, const DependencyPropertyChangedEventArgs& args)
		{
			if ((args.Property != &ItemContainerControl::IsSelectedProperty()
				&& args.Property != &Control::IsMouseOverProperty()))
				return;
			if (args.Property == &ItemContainerControl::IsSelectedProperty())
			{
				UpdateRowHeader();
				const size_t revision = _detailsPresentationRevision;
				(void)CoerceValue(DetailsVisibilityProperty());
				if (_detailsPresentationRevision == revision)
					(void)UpdateDetailsPresentation();
			}
			auto* owner = GetDataGridOwner();
			if (!owner) return;
			if (GetIsSelected() || IsMouseOver)
			{
				// Let the row state triggers own selected and pointer-over chrome.
				(void)ClearPropertyValue(Control::BackgroundProperty());
			}
			else
			{
				SetBackground((ItemIndex() % 2) != 0
					? owner->GetAlternatingRowBackground()
					: owner->GetRowBackground());
			}
		}));
}

DataGrid* DataGridRow::GetDataGridOwner() const noexcept
{
	return dynamic_cast<DataGrid*>(_ownerLifetime.Get());
}

Control* DataGridRow::GetDetailsElement() const noexcept
{
	return _detailsPresenter
		? _detailsPresenter->GetGeneratedContent() : nullptr;
}

void DataGridRow::UpdateHeightPresentation()
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_cellsGrid) return;
	double rowHeight = 0.0;
	const bool hasUserHeight = owner->TryGetProjectedRowHeight(
		ItemIndex(), rowHeight);
	if (!hasUserHeight) rowHeight = owner->EffectiveDefaultRowHeight();
	const bool fixed = std::isfinite(rowHeight) && rowHeight > 0.0;
	const float minimum = static_cast<float>((std::min)(
		owner->GetMinRowHeight(),
		static_cast<double>((std::numeric_limits<float>::max)())));
	_cellsGrid->SetMinHeight(minimum);
	if (fixed)
	{
		const auto height = static_cast<float>((std::min)(rowHeight,
			static_cast<double>((std::numeric_limits<float>::max)())));
		_cellsGrid->SetHeight(cui::layout::Length::Fixed(height));
		SetHeight(_detailsVisible
			? cui::layout::Length::Auto()
			: cui::layout::Length::Fixed(height));
	}
	else
	{
		_cellsGrid->SetHeight(cui::layout::Length::Auto());
		SetHeight(cui::layout::Length::Auto());
	}
}

bool DataGridRow::UpdateDetailsPresentation(std::wstring* outError)
{
	if (outError) outError->clear();
	auto* owner = GetDataGridOwner();
	if (!owner || !_rowLayoutGrid || !_cellsGrid) return true;
	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	if (++_detailsPresentationRevision == 0) ++_detailsPresentationRevision;
	const size_t revision = _detailsPresentationRevision;
	auto detailsTemplate = GetDetailsTemplate();
	const auto detailsSelector = GetDetailsTemplateSelector();
	bool shouldShow = _detailsVisibility == ::Visibility::Visible
		&& !owner->IsNewItemPlaceholder(_item)
		&& (detailsTemplate || detailsSelector);
	const auto requestIsCurrent = [rowLifetime, ownerLifetime, revision]()
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return row && liveOwner && row->GetDataGridOwner() == liveOwner
			&& row->_detailsPresentationRevision == revision;
	};
	std::unique_ptr<ContentPresenter> stagedPresenter;
	if (shouldShow && !detailsTemplate && detailsSelector)
	{
		ContentPresenter* selectorContainer = _detailsPresenter;
		if (!selectorContainer)
		{
			stagedPresenter = std::make_unique<ContentPresenter>();
			selectorContainer = stagedPresenter.get();
		}
		(void)selectorContainer->SetDataContext(_item);
		if (!requestIsCurrent()) return false;
		detailsTemplate = detailsSelector.Get()->SelectTemplate(
			_item, *selectorContainer);
		if (!requestIsCurrent()) return false;
		shouldShow = static_cast<bool>(detailsTemplate);
	}

	const auto applyHeightProjection = [rowLifetime, ownerLifetime](bool visible)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!row || !liveOwner || !row->_cellsGrid) return;
		row->_detailsVisible = visible;
		row->UpdateHeightPresentation();
	};
	const auto raiseDetailsEvent = [rowLifetime, ownerLifetime, &requestIsCurrent](
		Event<void(DataGrid*, DataGridRowDetailsEventArgs&)>& event,
		Control& detailsElement)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!row || !liveOwner || row->GetDataGridOwner() != liveOwner)
			return false;
		return liveOwner->RaiseRowDetailsEvent(event, *row, detailsElement)
			&& requestIsCurrent();
	};

	if (!shouldShow)
	{
		const bool wasVisible = _detailsVisible;
		_detailsVisible = false;
		if (wasVisible)
		{
			if (auto* detailsElement = GetDetailsElement())
				if (!raiseDetailsEvent(
					owner->RowDetailsVisibilityChanged, *detailsElement))
					return false;
		}
		if (!requestIsCurrent()) return false;
		if (_detailsPresenter)
		{
			if (_detailsLoaded)
			{
				auto* detailsElement = GetDetailsElement();
				_detailsLoaded = false;
				if (detailsElement && !raiseDetailsEvent(
					owner->UnloadingRowDetails, *detailsElement))
					return false;
			}
			if (!requestIsCurrent()) return false;
			auto* presenter = _detailsPresenter;
			_detailsPresenter = nullptr;
			_appliedDetailsTemplate = {};
			if (!_rowLayoutGrid->DeleteVisualChild(presenter))
			{
				if (outError) *outError =
					L"DataGridRow 无法回收折叠的 RowDetails 视觉。";
				return false;
			}
		}
		else
		{
			_detailsLoaded = false;
			_appliedDetailsTemplate = {};
		}
		if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
		applyHeightProjection(false);
		return requestIsCurrent();
	}

	if (_detailsPresenter && _appliedDetailsTemplate == detailsTemplate)
	{
		const ControlWeakReference presenterLifetime(_detailsPresenter);
		BindingSourceReference presentedItem;
		const bool contentIsCurrent = _detailsPresenter->GetContent().TryGet(
			presentedItem) && presentedItem.Shared() == _item.Shared();
		try
		{
			if (!contentIsCurrent)
			{
				(void)_detailsPresenter->SetDataContext(_item);
				if (!requestIsCurrent()) return false;
				auto* presenter = dynamic_cast<ContentPresenter*>(
					presenterLifetime.Get());
				if (!presenter || presenter != _detailsPresenter) return false;
				presenter->SetContent(BindingValue(_item));
			}
		}
		catch (...)
		{
			if (outError)
			{
				auto* presenter = dynamic_cast<ContentPresenter*>(
					presenterLifetime.Get());
				*outError = presenter
					? presenter->LastTemplateError() : std::wstring{};
			}
			throw;
		}
		if (!requestIsCurrent()) return false;
		auto* currentPresenter = dynamic_cast<ContentPresenter*>(
			presenterLifetime.Get());
		if (!currentPresenter) return false;
		if (!currentPresenter->LastTemplateError().empty())
		{
			if (outError) *outError = currentPresenter->LastTemplateError();
			return false;
		}
		const bool wasVisible = _detailsVisible;
		_detailsVisible = true;
		applyHeightProjection(true);
		if (!requestIsCurrent()) return false;
		UpdateDetailsHorizontalScrollOffset(owner->_horizontalScrollOffset);
		if (!requestIsCurrent()) return false;
		auto* detailsElement = GetDetailsElement();
		if (!detailsElement) return false;
		if (!wasVisible && !raiseDetailsEvent(
			owner->RowDetailsVisibilityChanged, *detailsElement))
			return false;
		if (!requestIsCurrent()) return false;
		if (!_detailsLoaded)
		{
			_detailsLoaded = true;
			if (!raiseDetailsEvent(owner->LoadingRowDetails, *detailsElement))
				return false;
		}
		return requestIsCurrent();
	}
	const bool wasVisible = _detailsVisible;
	if (_detailsPresenter)
	{
		if (_detailsLoaded)
		{
			auto* detailsElement = GetDetailsElement();
			_detailsLoaded = false;
			if (detailsElement && !raiseDetailsEvent(
				owner->UnloadingRowDetails, *detailsElement))
				return false;
		}
		if (!requestIsCurrent()) return false;
		auto* presenter = _detailsPresenter;
		_detailsPresenter = nullptr;
		_appliedDetailsTemplate = {};
		if (!_rowLayoutGrid->DeleteVisualChild(presenter))
		{
			if (outError) *outError =
				L"DataGridRow 无法替换 RowDetails 视觉。";
			return false;
		}
	}
	if (!requestIsCurrent()) return false;

	auto presenter = std::move(stagedPresenter);
	if (!presenter) presenter = std::make_unique<ContentPresenter>();
	presenter->VerticalAlignment = VerticalAlignment::Top;
	Grid::SetRow(*presenter, 1);
	Grid::SetColumn(*presenter, 1);
	presenter->SetContentTypeToken(
		detailsTemplate.Get()->GetDataTypeToken());
	try
	{
		presenter->SetContent(BindingValue(_item));
		presenter->SetContentTemplate(detailsTemplate);
	}
	catch (...)
	{
		if (outError) *outError = presenter->LastTemplateError();
		throw;
	}
	if (!presenter->LastTemplateError().empty())
	{
		if (outError) *outError = presenter->LastTemplateError();
		return false;
	}
	if (!requestIsCurrent()) return false;
	auto* raw = _rowLayoutGrid->AddOwned(std::move(presenter));
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row || !ownerLifetime.Get() || !raw
		|| row->_detailsPresentationRevision != revision)
	{
		if (row && raw && row->_rowLayoutGrid)
			(void)row->_rowLayoutGrid->DeleteVisualChild(raw);
		return false;
	}
	row->_detailsPresenter = dynamic_cast<ContentPresenter*>(raw);
	if (!row->_detailsPresenter) return false;
	row->_appliedDetailsTemplate = detailsTemplate;
	row->_detailsVisible = true;
	applyHeightProjection(true);
	if (!requestIsCurrent()) return false;
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!row || !owner) return false;
	row->UpdateDetailsHorizontalScrollOffset(owner->_horizontalScrollOffset);
	if (!requestIsCurrent()) return false;
	auto* detailsElement = row->GetDetailsElement();
	if (!detailsElement) return false;
	if (!wasVisible && !raiseDetailsEvent(
		owner->RowDetailsVisibilityChanged, *detailsElement))
		return false;
	if (!requestIsCurrent()) return false;
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!row || !owner) return false;
	row->_detailsLoaded = true;
	if (!raiseDetailsEvent(owner->LoadingRowDetails, *detailsElement))
		return false;
	return requestIsCurrent();
}

DataGridCell* DataGridRow::GetCell(size_t columnIndex) const noexcept
{
	if (!_columnStorageIsSparse)
		return columnIndex < _cells.size() ? _cells[columnIndex] : nullptr;
	if (columnIndex < _realizedFrozenColumnEnd)
		return columnIndex < _frozenCells.size()
			? _frozenCells[columnIndex] : nullptr;
	return columnIndex >= _realizedColumnBegin
		&& columnIndex < _realizedColumnEnd
		&& columnIndex - _realizedColumnBegin < _cells.size()
		? _cells[columnIndex - _realizedColumnBegin] : nullptr;
}

bool DataGridRow::RefreshValidationState()
{
	if (_refreshingValidation) return true;
	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(GetDataGridOwner());
	_refreshingValidation = true;
	_validationStateDirty = false;
	bool refreshCommitted = false;
	auto reset = MakeScopeExit([rowLifetime]
	{
		if (auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get()))
			row->_refreshingValidation = false;
	});
	auto restoreDirty = MakeScopeExit([rowLifetime, &refreshCommitted]
	{
		if (refreshCommitted) return;
		if (auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get()))
			row->_validationStateDirty = true;
	});

	struct ValidationPath final
	{
#if CUI_ENABLE_DYNAMIC_XAML
		std::wstring Dynamic;
#endif
		CompiledBindingPathView Compiled;
	};
	std::vector<ValidationPath> paths;
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		paths.reserve(owner->_logicalColumns.size());
		for (auto* column : owner->_logicalColumns)
		{
			auto* bound = dynamic_cast<DataGridBoundColumn*>(column);
			if (!bound || bound->GetBindingSourceKind()
				!= DataGridBindingSourceKind::RowDataContext) continue;
			ValidationPath path;
			path.Compiled = bound->GetCompiledBindingPath();
#if CUI_ENABLE_DYNAMIC_XAML
			if (path.Compiled.Empty()) path.Dynamic = bound->GetBindingPath();
			if (path.Compiled.Empty() && path.Dynamic.empty()) continue;
#else
			if (path.Compiled.Empty()) continue;
#endif
			paths.push_back(std::move(path));
		}
	}

	const auto item = _item;
	std::vector<BindingValidationResult> next;
	const auto appendIssue = [&next](std::wstring sourceProperty,
		const BindingValidationIssue& issue)
	{
		if (std::any_of(next.begin(), next.end(),
			[&issue](const BindingValidationResult& existing)
			{ return existing.Issue == issue; })) return;
		next.push_back(BindingValidationResult{
			L"Row", std::move(sourceProperty), issue });
	};
	if (auto* source = item.Get())
	{
#if CUI_ENABLE_DYNAMIC_XAML
		for (const auto& issue : source->GetValidationIssues(L""))
#else
		for (const auto& issue : source->GetValidationIssues(
			BindingSourcePropertyToken{}))
#endif
			appendIssue({}, issue);
		if (!rowLifetime.Get()) return false;
		for (const auto& path : paths)
		{
			std::vector<BindingValidationIssue> issues;
			std::wstring sourceProperty;
			if (!path.Compiled.Empty())
				issues = GetBindingValidationIssuesForPath(
					*source, path.Compiled);
#if CUI_ENABLE_DYNAMIC_XAML
			else
			{
				sourceProperty = path.Dynamic;
				issues = GetBindingValidationIssuesForPath(
					*source, path.Dynamic);
			}
#endif
			if (!rowLifetime.Get()) return false;
			for (const auto& issue : issues)
				appendIssue(sourceProperty, issue);
		}
	}

	const auto appendVisual = [&next](const BindingValidationResult& result)
	{
		if (std::any_of(next.begin(), next.end(),
			[&result](const BindingValidationResult& existing)
			{ return existing.Issue == result.Issue; })) return;
		next.push_back(result);
	};
	std::vector<ControlWeakReference> visuals;
	std::unordered_set<Control*> visited;
	const auto collect = [&](Control* root, const auto& self) -> void
	{
		if (!root || !visited.insert(root).second) return;
		visuals.emplace_back(root);
		for (auto* child : root->GetVisualChildrenView()) self(child, self);
	};
	collect(_cellsGrid, collect);
	for (const auto& visualLifetime : visuals)
	{
		auto* visual = visualLifetime.Get();
		if (!visual) continue;
		for (const auto& result : visual->GetValidationResults())
			appendVisual(result);
		if (!rowLifetime.Get()) return false;
	}

	const bool hasError = std::any_of(next.begin(), next.end(),
		[](const BindingValidationResult& result)
		{
			return result.Issue.Severity
				== BindingValidationSeverity::Error;
		});
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row) return false;
	if (row->_validationErrors != next)
		(void)row->SetReadOnlyPropertyField(
			ValidationErrorsPropertyKey(),
			row->_validationErrors, std::move(next));
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row) return false;
	if (row->_hasValidationError != hasError)
		(void)row->SetReadOnlyPropertyField(
			HasValidationErrorPropertyKey(),
			row->_hasValidationError, hasError);
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row) return false;
	row->UpdateValidationVisual();
	refreshCommitted = true;
	return rowLifetime.Get() != nullptr;
}

void DataGridRow::InvalidateValidationVisualTracking() noexcept
{
	if (++_validationVisualRevision == 0) ++_validationVisualRevision;
	_validationStateDirty = true;
}

bool DataGridRow::AttachValidationTracking()
{
	const ControlWeakReference rowLifetime(this);
	const bool itemChanged = _validationTrackedItem.Shared() != _item.Shared();
	if (itemChanged)
	{
		_itemValidationConnection.Disconnect();
		_validationTrackedItem = {};
		_validationStateDirty = true;
		const auto item = _item;
		EventConnection nextConnection;
		if (auto* source = item.Get())
			if (auto* changed = source->ValidationChanged())
				nextConnection = changed->Subscribe(
				[rowLifetime](const BindingValidationChangedEventArgs&)
				{
					if (auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get()))
					{
						row->_validationStateDirty = true;
						(void)row->RefreshValidationState();
					}
				});
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!row || row->_item.Shared() != item.Shared()) return false;
		row->_itemValidationConnection = std::move(nextConnection);
		row->_validationTrackedItem = item;
	}

	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row) return false;
	const bool visualChanged = row->_appliedValidationVisualRevision
		!= row->_validationVisualRevision;
	if (visualChanged)
	{
		row->_visualValidationConnections.clear();
		row->_validationStateDirty = true;
		const size_t visualRevision = row->_validationVisualRevision;
		std::unordered_set<Control*> visited;
		const auto subscribe = [&](Control* root, const auto& self) -> bool
		{
			auto* current = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!current || current->_validationVisualRevision != visualRevision)
				return false;
			if (!root || !visited.insert(root).second) return true;
			current->_visualValidationConnections.push_back(
				root->OnValidationStateChanged.Subscribe(
					[rowLifetime](const BindingValidationChangedEventArgs&)
					{
						if (auto* live = dynamic_cast<DataGridRow*>(
							rowLifetime.Get()))
						{
							live->_validationStateDirty = true;
							(void)live->RefreshValidationState();
						}
					}));
			if (!rowLifetime.Get()) return false;
			std::vector<ControlWeakReference> children;
			children.reserve(root->GetVisualChildrenView().size());
			for (auto* child : root->GetVisualChildrenView())
				children.emplace_back(child);
			for (const auto& child : children)
				if (!self(child.Get(), self)) return false;
			return true;
		};
		if (!subscribe(row->_cellsGrid, subscribe)) return false;
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!row || row->_validationVisualRevision != visualRevision)
			return false;
		row->_appliedValidationVisualRevision = visualRevision;
	}
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row) return false;
	return row->_validationStateDirty ? row->RefreshValidationState() : true;
}

void DataGridRow::UpdateValidationVisual()
{
	if (!_rowHeaderHost) return;
	const ControlWeakReference rowLifetime(this);
	if (!_validationErrorIndicator)
	{
		auto indicator = std::make_unique<Control>();
		indicator->HorizontalAlignment = HorizontalAlignment::Right;
		indicator->VerticalAlignment = VerticalAlignment::Center;
		indicator->SetZIndex(2);
		_validationErrorIndicator =
			_rowHeaderHost->AddOwned(std::move(indicator));
	}
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row || !row->_validationErrorIndicator) return;
	const auto templateReference = row->_validationErrorTemplate;
	row->_validationErrorIndicator->SetTemplate(templateReference);
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!row || !row->_validationErrorIndicator) return;
	row->_validationErrorIndicator->SetVisibility(
		row->_hasValidationError && templateReference
			? Visibility::Visible : Visibility::Collapsed);
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (row && row->_rowHeader)
		row->_rowHeader->SetAutomationHelpText(
			row->_hasValidationError ? row->GetValidationSummary(3) : L"");
}

bool DataGridRow::Initialize(
	DataGrid& owner,
	const BindingSourceReference& item,
	size_t index,
	std::wstring* outError)
{
	if (outError) outError->clear();
	const ControlWeakReference ownerLifetime(&owner);
	_itemValidationConnection.Disconnect();
	_visualValidationConnections.clear();
	_validationTrackedItem = {};
	_appliedValidationVisualRevision = 0;
	InvalidateValidationVisualTracking();
	_rowLayoutGrid = nullptr;
	_rowHeaderHost = nullptr;
	_cellsGrid = nullptr;
	_rowHeader = nullptr;
	_validationErrorIndicator = nullptr;
	_detailsPresenter = nullptr;
	_appliedDetailsTemplate = {};
	_detailsVisible = false;
	_detailsLoaded = false;
	if (++_detailsPresentationRevision == 0) ++_detailsPresentationRevision;
	_ownerLifetime = &owner;
	_item = item;
	SetCurrentIsNewItem(
		owner.IsNewItemPlaceholder(item)
		|| owner.IsPendingNewItem(item, index));
	if (!ownerLifetime.Get()) return false;
	(void)CoerceValue(ValidationErrorTemplateProperty());
	if (!ownerLifetime.Get()) return false;
	SetItemIndex(index);
	SetCurrentIsEditing(owner.IsRowEditIdentity(item, index));
	(void)SetDataContext(item);
	auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	if (!liveOwner->RestoreRowPresentationOverride(*this))
	{
		if (outError) *outError =
			L"DataGridRow 无法恢复行级详情属性。";
		return false;
	}
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	// The cells presenter is created below and owns the resizable baseline.
	// UpdateDetailsPresentation applies the effective grid/user height once both
	// the cells and optional details presenters are available.
	SetHeight(cui::layout::Length::Auto());
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	SetBackground((index % 2) != 0
		? liveOwner->GetAlternatingRowBackground()
		: liveOwner->GetRowBackground());
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	const bool horizontal = HasHorizontalGridLines(
		liveOwner->GetGridLinesVisibility());
	SetBorderThickness(Thickness(
		0.0f, 0.0f, 0.0f, horizontal ? 1.0f : 0.0f));
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	SetBorderBrush(liveOwner->GetHorizontalGridLinesBrush());
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;

	auto rowLayout = std::make_unique<Grid>();
	rowLayout->AddRow(GridLength::Auto());
	rowLayout->AddRow(GridLength::Auto());
	const double rowHeaderWidth = liveOwner->ResolveRowHeaderWidth();
	const bool rowHeaderVisible = HasRowHeaders(
		liveOwner->GetHeadersVisibility());
	const bool rowHeaderAutoWidth =
		std::isnan(liveOwner->GetRowHeaderWidth());
	rowLayout->AddColumn(!rowHeaderVisible
		? GridLength::Pixels(0.0f)
		: rowHeaderAutoWidth
			? GridLength::Auto()
			: GridLength::Pixels(static_cast<float>(rowHeaderWidth)));
	rowLayout->AddColumn(GridLength::Auto());

	auto rowHeaderHost = std::make_unique<Grid>();
	// Horizontally scrolled cells may render beneath the frozen row-header
	// gutter. Keep that chrome above the cells for both rendering and hit test.
	rowHeaderHost->SetZIndex(1);
	auto rowHeader = std::make_unique<DataGridRowHeader>();
	rowHeader->Initialize(*this);
	Grid::SetColumn(*rowHeaderHost, 0);
	Grid::SetRow(*rowHeaderHost, 0);
	Grid::SetRowSpan(*rowHeaderHost, 2);
	auto* rowHeaderRaw = rowHeaderHost->AddOwned(std::move(rowHeader));
	auto* rowHeaderHostRaw = rowLayout->AddOwned(std::move(rowHeaderHost));

	auto grid = std::make_unique<DataGridCellsPresenter>(*this);
	const auto realizedColumns = liveOwner->ResolveRealizedColumnRange();
	const size_t initialColumnCount = liveOwner->ColumnCount();
	const size_t initialWidthProjectionRevision =
		liveOwner->_columnWidthProjectionRevision;
	const size_t initialCellSelectionRevision =
		liveOwner->_cellSelectionRevision;
	const bool sparseColumns = liveOwner->GetEnableColumnVirtualization()
		&& initialColumnCount > 0;
	_columnStorageIsSparse = sparseColumns;
	_realizedFrozenColumnEnd = sparseColumns
		? (std::min)(initialColumnCount, static_cast<size_t>(
			(std::max)(0, liveOwner->GetFrozenColumnCount()))) : 0;
	_realizedColumnBegin = sparseColumns ? realizedColumns.first : 0;
	_realizedColumnEnd = sparseColumns
		? realizedColumns.second : initialColumnCount;
	if (sparseColumns && _realizedColumnBegin < _realizedFrozenColumnEnd)
		return false;
	_frozenCells.assign(_realizedFrozenColumnEnd, nullptr);
	_cells.assign(sparseColumns
		? _realizedColumnEnd - _realizedColumnBegin
		: initialColumnCount, nullptr);
	const auto addCell = [&](size_t columnIndex,
		std::vector<DataGridCell*>& storage, size_t storageIndex,
		int layoutColumn) -> bool
	{
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!liveOwner || liveOwner->ColumnCount() != initialColumnCount)
			return false;
		auto* column = liveOwner->GetColumnFromDisplayIndex(columnIndex);
		if (!column) return false;
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!liveOwner || liveOwner->ColumnCount() != initialColumnCount
			|| liveOwner->GetColumnFromDisplayIndex(columnIndex) != column) return false;
		if (column->GetVisibility() != Visibility::Visible) return true;
		auto cell = std::make_unique<DataGridCell>();
		if (!cell->Initialize(*this, *column, item, columnIndex, outError))
			return false;
		if (!ownerLifetime.Get()) return false;
		Grid::SetColumn(*cell, layoutColumn);
		storage[storageIndex] = grid->AddOwned(std::move(cell));
		return storage[storageIndex] != nullptr;
	};
	if (sparseColumns)
	{
		for (size_t columnIndex = 0;
			columnIndex < _realizedFrozenColumnEnd; ++columnIndex)
			if (!addCell(columnIndex, _frozenCells, columnIndex,
				static_cast<int>(columnIndex))) return false;
		for (size_t columnIndex = _realizedColumnBegin;
			columnIndex < _realizedColumnEnd; ++columnIndex)
		{
			const size_t slot = columnIndex - _realizedColumnBegin;
			if (!addCell(columnIndex, _cells, slot, static_cast<int>(
				_realizedFrozenColumnEnd + 1 + slot))) return false;
		}
		_realizedCells.reserve(_frozenCells.size() + _cells.size());
		_realizedCells.insert(_realizedCells.end(),
			_frozenCells.begin(), _frozenCells.end());
		_realizedCells.insert(_realizedCells.end(),
			_cells.begin(), _cells.end());
	}
	else
	{
		for (size_t columnIndex = 0;
			columnIndex < initialColumnCount; ++columnIndex)
			if (!addCell(columnIndex, _cells, columnIndex,
				static_cast<int>(columnIndex))) return false;
	}
	if (!ownerLifetime.Get()) return false;
	Grid::SetColumn(*grid, 1);
	Grid::SetRow(*grid, 0);
	auto* cellsGridRaw = rowLayout->AddOwned(std::move(grid));
	const ControlWeakReference rowLifetime(this);
	_rowLayoutGrid = rowLayout.get();
	_rowHeaderHost = rowHeaderHostRaw;
	_rowHeader = rowHeaderRaw;
	_cellsGrid = cellsGridRaw;
	UpdateRowHeader();
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
	if (!UpdateRowHeaderPresentation()) return false;
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
	UpdateValidationVisual();
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
	liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveOwner) return false;
	if (liveOwner->_columnWidthProjectionRevision
		== initialWidthProjectionRevision)
		_appliedColumnWidthProjectionRevision =
			initialWidthProjectionRevision;
	if (liveOwner->_cellSelectionRevision == initialCellSelectionRevision)
		_appliedCellSelectionRevision = initialCellSelectionRevision;
	UpdateHorizontalScrollOffset(liveOwner->_horizontalScrollOffset);
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
	(void)SetVisualContent(std::move(rowLayout));
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!liveRow || !ownerLifetime.Get()) return false;
	if (!liveRow->UpdateDetailsPresentation(outError)) return false;
	liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!liveRow || !ownerLifetime.Get()) return false;
	return liveRow->AttachValidationTracking()
		&& rowLifetime.Get() != nullptr && ownerLifetime.Get() != nullptr;
}

bool DataGridRow::RefreshRealizedColumns(
	size_t frozenEnd, size_t begin, size_t end,
	std::wstring* outError)
{
	if (outError) outError->clear();
	auto* owner = GetDataGridOwner();
	if (!owner || !_cellsGrid || !_columnStorageIsSparse
		|| frozenEnd > begin || begin > end
		|| end > owner->ColumnCount()) return false;
	if (frozenEnd == _realizedFrozenColumnEnd
		&& begin == _realizedColumnBegin && end == _realizedColumnEnd)
	{
		UpdateColumnWidths();
		return true;
	}

	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	const ControlWeakReference gridLifetime(_cellsGrid);
	const size_t columnCount = owner->ColumnCount();
	const size_t oldFrozenEnd = _realizedFrozenColumnEnd;
	const size_t oldBegin = _realizedColumnBegin;
	const size_t oldEnd = _realizedColumnEnd;
	const auto oldFrozenCells = _frozenCells;
	const auto oldCells = _cells;
	std::vector<DataGridCell*> nextFrozen(frozenEnd, nullptr);
	std::vector<DataGridCell*> next(end - begin, nullptr);
	std::vector<std::unique_ptr<DataGridCell>> createdFrozen(frozenEnd);
	std::vector<std::unique_ptr<DataGridCell>> created(end - begin);
	const auto oldAt = [&](size_t index) -> DataGridCell*
	{
		if (index < oldFrozenEnd)
			return index < oldFrozenCells.size() ? oldFrozenCells[index] : nullptr;
		return index >= oldBegin && index < oldEnd
			&& index - oldBegin < oldCells.size()
			? oldCells[index - oldBegin] : nullptr;
	};
	const auto nextAt = [&](size_t index) -> DataGridCell*
	{
		if (index < frozenEnd)
			return index < nextFrozen.size() ? nextFrozen[index] : nullptr;
		return index >= begin && index < end
			&& index - begin < next.size()
			? next[index - begin] : nullptr;
	};
	try
	{
		const auto stageRange = [&](size_t rangeBegin, size_t rangeEnd,
			std::vector<DataGridCell*>& destination,
			std::vector<std::unique_ptr<DataGridCell>>& creations) -> bool
		{
			for (size_t index = rangeBegin; index < rangeEnd; ++index)
			{
				const size_t slot = index - rangeBegin;
				owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (!owner || !gridLifetime.Get()
					|| owner->ColumnCount() != columnCount) return false;
				auto* column = owner->GetColumnFromDisplayIndex(index);
				if (!column) return false;
				if (column->GetVisibility() != Visibility::Visible) continue;
				auto* retained = oldAt(index);
				if (retained)
				{
					if (!_cellsGrid->ContainsControl(retained)
						|| retained->_columnIndex != index
						|| retained->_column != column) return false;
					destination[slot] = retained;
					continue;
				}
				auto cell = std::make_unique<DataGridCell>();
				if (!cell->Initialize(*this, *column, _item, index, outError))
					return false;
				if (!rowLifetime.Get() || !ownerLifetime.Get()
					|| !gridLifetime.Get()) return false;
				destination[slot] = cell.get();
				creations[slot] = std::move(cell);
			}
			return true;
		};
		if (!stageRange(0, frozenEnd, nextFrozen, createdFrozen)
			|| !stageRange(begin, end, next, created)) return false;
		// Do not mutate the established visual tree until every entering cell
		// has been generated successfully.  Column element/binding creation may
		// throw; staging preserves the complete old strip in that case.
		const auto addCreated = [&](std::vector<DataGridCell*>& destination,
			std::vector<std::unique_ptr<DataGridCell>>& creations) -> bool
		{
			for (size_t slot = 0; slot < creations.size(); ++slot)
			{
				auto* grid = dynamic_cast<DataGridCellsPresenter*>(
					gridLifetime.Get());
				if (!grid || !rowLifetime.Get() || !ownerLifetime.Get())
					return false;
				if (creations[slot])
					destination[slot] = grid->AddOwned(
						std::move(creations[slot]));
			}
			return true;
		};
		if (!addCreated(nextFrozen, createdFrozen)
			|| !addCreated(next, created)) return false;
	}
	catch (...)
	{
		if (outError && outError->empty())
			*outError = L"DataGrid 横向虚拟单元格更新失败。";
		return false;
	}
	const auto removeOld = [&](size_t index, DataGridCell* cell) -> bool
	{
		if (nextAt(index) == cell) return true;
		auto* grid = dynamic_cast<DataGridCellsPresenter*>(
			gridLifetime.Get());
		if (!grid || (cell && grid->ContainsControl(cell)
			&& !grid->DeleteVisualChild(cell))) return false;
		if (!rowLifetime.Get() || !ownerLifetime.Get()) return false;
		return true;
	};
	for (size_t slot = 0; slot < oldFrozenCells.size(); ++slot)
		if (!removeOld(slot, oldFrozenCells[slot])) return false;
	for (size_t slot = 0; slot < oldCells.size(); ++slot)
		if (!removeOld(oldBegin + slot, oldCells[slot])) return false;

	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* grid = dynamic_cast<DataGridCellsPresenter*>(gridLifetime.Get());
	if (!owner || !grid || owner->ColumnCount() != columnCount) return false;
	const auto validateRange = [&](size_t rangeBegin,
		const std::vector<DataGridCell*>& values, int layoutBase) -> bool
	{
		for (size_t slot = 0; slot < values.size(); ++slot)
		{
			auto* cell = values[slot];
			const size_t index = rangeBegin + slot;
			auto* column = owner->GetColumnFromDisplayIndex(index);
			if (!column) return false;
			if (column->GetVisibility() != Visibility::Visible)
			{
				if (cell) return false;
				continue;
			}
			if (!cell || !grid->ContainsControl(cell)) return false;
			Grid::SetColumn(*cell, layoutBase + static_cast<int>(slot));
		}
		return true;
	};
	if (!validateRange(0, nextFrozen, 0)
		|| !validateRange(begin, next,
			static_cast<int>(frozenEnd + 1))) return false;
	_frozenCells = std::move(nextFrozen);
	_cells = std::move(next);
	_realizedCells.clear();
	_realizedCells.reserve(_frozenCells.size() + _cells.size());
	_realizedCells.insert(_realizedCells.end(),
		_frozenCells.begin(), _frozenCells.end());
	_realizedCells.insert(_realizedCells.end(),
		_cells.begin(), _cells.end());
	_realizedFrozenColumnEnd = frozenEnd;
	_realizedColumnBegin = begin;
	_realizedColumnEnd = end;
	InvalidateValidationVisualTracking();
	_appliedColumnWidthProjectionRevision = 0;
	_appliedHorizontalScrollOffset =
		(std::numeric_limits<double>::quiet_NaN)();
	UpdateColumnWidths();
	UpdateHorizontalScrollOffset(owner->_horizontalScrollOffset);
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	return liveRow && ownerLifetime.Get()
		&& liveRow->AttachValidationTracking();
}

void DataGridRow::UpdateColumnWidths(bool propagateLayoutInvalidation)
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_cellsGrid) return;
	auto* const grid = _cellsGrid;
	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	const ControlWeakReference gridLifetime(grid);
	const size_t projectionRevision =
		owner->_columnWidthProjectionRevision;
	// A realized row receives this projection during Initialize.  The subsequent
	// realization callback also visits every retained row; treating an identical
	// revision as a fresh invalidation turns one entering/leaving row during a
	// vertical window resize into a complete DataGrid relayout.  New rows already
	// carry their normal construction/template dirtiness, so an applied revision
	// is a true no-op here just as it is for the header presenter.
	if (_appliedColumnWidthProjectionRevision == projectionRevision) return;
	const size_t columnCount = owner->ColumnCount();
	const bool sparseColumns = _columnStorageIsSparse;
	const size_t frozenEnd = sparseColumns
		? _realizedFrozenColumnEnd : 0;
	const size_t begin = sparseColumns
		? _realizedColumnBegin : 0;
	const size_t end = sparseColumns
		? _realizedColumnEnd : columnCount;
	if (frozenEnd > begin || begin > end || end > columnCount) return;
	if (columnCount > 0)
	{
		double ignored = 0.0;
		double total = 0.0;
		if (!owner->TryResolveColumnBounds(
			columnCount - 1, ignored, total)) return;
	}
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveGrid = dynamic_cast<DataGridCellsPresenter*>(gridLifetime.Get());
	if (row != this || owner == nullptr || owner != ownerLifetime.Get()
		|| liveGrid != grid || row->_cellsGrid != liveGrid
		|| row->GetDataGridOwner() != owner
		|| owner->ColumnCount() != columnCount
		|| owner->_columnWidthProjectionRevision != projectionRevision
		|| row->_columnStorageIsSparse != sparseColumns
		|| row->_realizedFrozenColumnEnd != frozenEnd
		|| row->_realizedColumnBegin != begin
		|| row->_realizedColumnEnd != end) return;
	liveGrid->InvalidateColumnLayout(propagateLayoutInvalidation);
	// DataGridRow is a ContentControl. Its child Grid may keep the same outer
	// row slot while the cells presenter changes width, so SizeChanged will not
	// set the content-layout pending bit for us. Mark that local transaction
	// explicitly without starting another ancestor walk.
	row->_contentLayoutPending = true;
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	liveGrid = dynamic_cast<DataGridCellsPresenter*>(gridLifetime.Get());
	if (row == this && owner && liveGrid == grid
		&& row->_cellsGrid == liveGrid
		&& row->GetDataGridOwner() == owner
		&& owner->_columnWidthProjectionRevision == projectionRevision
		&& row->_columnStorageIsSparse == sparseColumns
		&& row->_realizedFrozenColumnEnd == frozenEnd
		&& row->_realizedColumnBegin == begin
		&& row->_realizedColumnEnd == end)
	{
		row->_appliedColumnWidthProjectionRevision = projectionRevision;
		row->UpdateHorizontalScrollOffset(owner->_horizontalScrollOffset);
	}
}

void DataGridRow::UpdateRowHeader()
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_rowLayoutGrid || !_rowHeaderHost || !_rowHeader) return;
	const bool visible = HasRowHeaders(owner->GetHeadersVisibility());
	const bool autoWidth = std::isnan(owner->GetRowHeaderWidth());
	const double width = owner->ResolveRowHeaderWidth();
	const bool selected = GetIsSelected();
	const bool projectionCurrent = _rowHeaderProjectionInitialized
		&& _appliedRowHeaderVisible == visible
		&& _appliedRowHeaderAutoWidth == autoWidth
		&& (autoWidth
			|| std::abs(_appliedRowHeaderWidth - width) <= 0.0001);
	if (projectionCurrent)
	{
		_rowHeader->SetCurrentIsRowSelected(selected);
		return;
	}
	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	const ControlWeakReference layoutLifetime(_rowLayoutGrid);
	const ControlWeakReference hostLifetime(_rowHeaderHost);
	const ControlWeakReference headerLifetime(_rowHeader);
	auto* layout = dynamic_cast<Grid*>(layoutLifetime.Get());
	if (!layout) return;
	std::vector<ColumnDefinition> definitions;
	definitions.reserve(2);
	definitions.emplace_back(!visible
		? GridLength::Pixels(0.0f)
		: autoWidth
			? GridLength::Auto()
			: GridLength::Pixels(static_cast<float>(width)));
	definitions.emplace_back(GridLength::Auto());
	layout->ReplaceColumns(std::move(definitions));
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return;
	auto* host = dynamic_cast<Grid*>(hostLifetime.Get());
	if (!host) return;
	host->SetVisibility(
		visible ? Visibility::Visible : Visibility::Collapsed);
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return;
	auto* header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!header) return;
	header->SetVisibility(
		visible ? Visibility::Visible : Visibility::Collapsed);
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return;
	header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!header) return;
	header->SetWidth(autoWidth
		? cui::layout::Length::Auto()
		: cui::layout::Length::Fixed(static_cast<float>(width)));
	if (!rowLifetime.Get() || !ownerLifetime.Get()) return;
	header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!header) return;
	header->SetCurrentIsRowSelected(selected);
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (row && owner && row->GetDataGridOwner() == owner)
	{
		row->_rowHeaderProjectionInitialized = true;
		row->_appliedRowHeaderVisible = visible;
		row->_appliedRowHeaderAutoWidth = autoWidth;
		row->_appliedRowHeaderWidth = width;
	}
}

bool DataGridRow::UpdateRowHeaderPresentation()
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_rowHeader) return false;
	const ControlWeakReference rowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	const ControlWeakReference headerLifetime(_rowHeader);
	if (++_rowHeaderPresentationRevision == 0)
		++_rowHeaderPresentationRevision;
	const size_t revision = _rowHeaderPresentationRevision;
	(void)_rowHeader->SetDataContext(_item);
	auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!row || !owner || !header || row->GetDataGridOwner() != owner
		|| row->_rowHeaderPresentationRevision != revision) return false;

	ItemTemplateReference selected;
	if (!owner->TryResolveRowHeaderTemplate(*row, *header, selected))
		return false;
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!row || !owner || !header || row->GetDataGridOwner() != owner
		|| row->_rowHeaderPresentationRevision != revision) return false;
	try
	{
		// On a recycled row, never let a newly selected template build once with
		// the previous row's Content. Clear the old projection before changing
		// template identity, then publish the current item exactly once.
		if (header->GetContentTemplate() != selected)
		{
			header->SetContent(BindingValue{});
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
			if (!row || !owner || !header
				|| row->GetDataGridOwner() != owner
				|| row->_rowHeaderPresentationRevision != revision)
				return false;
			header->SetContentTemplate(selected);
		}
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
		if (!row || !owner || !header || row->GetDataGridOwner() != owner
			|| row->_rowHeaderPresentationRevision != revision) return false;
		// Preserve the existing raw row-header surface when no template was
		// chosen: callers may install their own visual resize/hit target.
		header->SetContent(selected
			? BindingValue(row->_item) : BindingValue{});
	}
	catch (...)
	{
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* liveHeader = dynamic_cast<DataGridRowHeader*>(
			headerLifetime.Get());
		if (liveOwner && liveHeader)
			liveOwner->SetLastTemplateError(liveHeader->LastTemplateError());
		throw;
	}
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!row || !owner || !header || row->GetDataGridOwner() != owner
		|| row->_rowHeaderPresentationRevision != revision) return false;
	if (!header->LastTemplateError().empty())
	{
		owner->SetLastTemplateError(header->LastTemplateError());
		return false;
	}
	return true;
}

void DataGridRow::UpdateHorizontalScrollOffset(double offset)
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_rowHeaderHost) return;
	if (!std::isfinite(offset)) offset = 0.0;
	_rowHeaderHost->SetRenderTransform(HorizontalTranslation(offset));
	const size_t frozenEnd = (std::min)(owner->ColumnCount(),
		static_cast<size_t>((std::max)(0, owner->GetFrozenColumnCount())));
	double frozenWidth = 0.0;
	if (frozenEnd > 0)
	{
		double ignored = 0.0;
		if (!owner->TryResolveColumnBounds(
			frozenEnd - 1, ignored, frozenWidth)) return;
	}
	for (auto* cell : GetCells())
	{
		if (!cell) continue;
		double left = 0.0;
		double right = 0.0;
		if (!owner->TryResolveColumnBounds(
			cell->_columnIndex, left, right)) return;
		ApplyFrozenColumnVisual(*cell,
			cell->_columnIndex < frozenEnd, offset,
			frozenWidth, left, right);
	}
	UpdateDetailsHorizontalScrollOffset(offset);
	_appliedHorizontalScrollOffset = offset;
}

void DataGridRow::UpdateDetailsHorizontalScrollOffset(double offset)
{
	auto* owner = GetDataGridOwner();
	if (!owner || !_detailsPresenter) return;
	if (!std::isfinite(offset)) offset = 0.0;
	_detailsPresenter->SetRenderTransform(HorizontalTranslation(
		owner->GetAreRowDetailsFrozen() ? offset : 0.0));
}

const DependencyProperty& DataGridRowHeader::IsRowSelectedProperty()
{
	return IsRowSelectedPropertyKey().Property();
}

const DependencyPropertyKey& DataGridRowHeader::IsRowSelectedPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<DataGridRowHeader, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGridRowHeader, bool>(
				DependencyPropertyRegistrationLiteral(L"IsRowSelected"),
				[](DataGridRowHeader& target)
				{ return target._isRowSelected; },
				[](DataGridRowHeader& target, const bool& value)
				{
					(void)target.SetReadOnlyPropertyField(
						IsRowSelectedPropertyKey(),
						target._isRowSelected, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

void DataGridRowHeader::RegisterDependencyProperties()
{
	Button::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)IsRowSelectedProperty();
#endif
	CUI_DESIGN_METADATA_ONLY(
	(void)DataGridRowHeaderFocusableMetadataRelation();
	(void)DataGridRowHeaderClickModeMetadataRelation();
	)
}

DataGridRowHeader::DataGridRowHeader()
{
	RegisterDependencyProperties();
	(void)TrySetCurrentPropertyValue(
		ButtonBase::ClickModeProperty(),
		BindingValue(::ClickMode::Press));
	RetainEventConnection(OnPreviewMouseDown.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.ChangedButton != MouseButton::Left) return;
			// A ContentTemplate makes its decorative child the native hit source.
			// Acquire the gripper during the tunnel, before ButtonBase's descendant
			// MouseDown class handler can execute ClickMode=Press and select the row.
			if (!_isResizing && BeginRowResize(args.Y))
			{
				_activationModifiers = ModifierKeys::None;
				args.Handled = true;
				return;
			}
			_activationModifiers = args.Modifiers;
		}));
	RetainEventConnection(OnPreviewMouseDoubleClick.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.ChangedButton != MouseButton::Left) return;
			size_t rowIndex = DataGridCellInfo::InvalidIndex;
			bool fromTopEdge = false;
			auto* row = GetRowOwner();
			auto* owner = row ? row->GetDataGridOwner() : nullptr;
			if (owner && TryResolveResizeRow(
				args.Y, rowIndex, fromTopEdge)
				&& owner->AutoSizeRow(rowIndex))
			{
				_activationModifiers = ModifierKeys::None;
				args.Handled = true;
				return;
			}
			_activationModifiers = args.Modifiers;
		}));
	RetainEventConnection(OnMouseDown.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.ChangedButton != MouseButton::Left) return;
			if (!_isResizing && BeginRowResize(args.Y))
			{
				const ControlWeakReference lifetime(this);
				InputReport cancelButtonPress;
				cancelButtonPress.Kind = InputReportKind::Cancel;
				(void)ButtonBase::ProcessInput(cancelButtonPress);
				if (auto* source = dynamic_cast<DataGridRowHeader*>(lifetime.Get()))
					source->_activationModifiers = ModifierKeys::None;
				args.Handled = true;
				return;
			}
			_activationModifiers = args.Modifiers;
			// ButtonBase already owns the press. Stop the ancestor DataGridRow
			// from treating the same press as an ordinary row-container click.
			args.Handled = true;
		}));
	RetainEventConnection(OnMouseDoubleClick.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.ChangedButton != MouseButton::Left) return;
			size_t rowIndex = DataGridCellInfo::InvalidIndex;
			bool fromTopEdge = false;
			auto* row = GetRowOwner();
			auto* owner = row ? row->GetDataGridOwner() : nullptr;
			if (owner && TryResolveResizeRow(
				args.Y, rowIndex, fromTopEdge)
				&& owner->AutoSizeRow(rowIndex))
			{
				InputReport cancelButtonPress;
				cancelButtonPress.Kind = InputReportKind::Cancel;
				(void)ButtonBase::ProcessInput(cancelButtonPress);
				_activationModifiers = ModifierKeys::None;
				args.Handled = true;
				return;
			}
			_activationModifiers = args.Modifiers;
			args.Handled = true;
		}));
}

const DependencyPropertyMetadata*
DataGridRowHeader::ResolveExactDependencyPropertyMetadata(
	const DependencyProperty& property) const
{
	if (&property == &Control::FocusableProperty())
		return &DataGridRowHeaderFocusableMetadataRelation().Metadata();
	if (&property == &ButtonBase::ClickModeProperty())
		return &DataGridRowHeaderClickModeMetadataRelation().Metadata();
	return Button::ResolveExactDependencyPropertyMetadata(property);
}

DataGridRow* DataGridRowHeader::GetRowOwner() const noexcept
{
	return dynamic_cast<DataGridRow*>(_rowLifetime.Get());
}

void DataGridRowHeader::InvalidateSharedWidthMeasure()
{
	RequestLayout();
}

cui::core::Size DataGridRowHeader::FinalizeMeasureCore(
	cui::core::Size intrinsic,
	const cui::core::Constraints& available)
{
	intrinsic = Button::FinalizeMeasureCore(intrinsic, available);
	auto* row = GetRowOwner();
	auto* owner = row ? row->GetDataGridOwner() : nullptr;
	double sharedWidth = intrinsic.width;
	if (owner && owner->TryShareRowHeaderDesiredWidth(
		*this, intrinsic.width, sharedWidth))
		intrinsic.width = static_cast<float>(sharedWidth);
	return intrinsic;
}

void DataGridRowHeader::Initialize(DataGridRow& row)
{
	_rowLifetime = &row;
	if (auto* owner = row.GetDataGridOwner())
	{
		cui::framework::StyleAccess::SetResourceKey(
			*this, owner->GetRowHeaderStyle());
	}
	SetCurrentIsRowSelected(row.GetIsSelected());
}

void DataGridRowHeader::SetCurrentIsRowSelected(bool value)
{
	(void)SetReadOnlyPropertyField(
		IsRowSelectedPropertyKey(), _isRowSelected, value);
}

bool DataGridRowHeader::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::PointerDown
		&& input.ChangedButton == MouseButton::Left
		&& BeginRowResize(input.Y))
	{
		auto args = input.CreateMouseEventArgs();
		OnMouseDown(this, args);
		return true;
	}
	if (input.Kind == InputReportKind::PointerMove && _isResizing)
	{
		(void)ContinueRowResize(input.Y);
		auto args = input.CreateMouseEventArgs();
		OnMouseMove(this, args);
		return true;
	}
	if (input.Kind == InputReportKind::PointerUp && _isResizing
		&& input.ChangedButton == MouseButton::Left)
	{
		EndRowResize(false);
		auto args = input.CreateMouseEventArgs();
		OnMouseUp(this, args);
		return true;
	}
	if ((input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost) && _isResizing)
	{
		EndRowResize(true);
		return true;
	}
	if (input.Kind == InputReportKind::PointerDoubleClick
		&& input.ChangedButton == MouseButton::Left)
	{
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		bool fromTopEdge = false;
		auto* row = GetRowOwner();
		auto* owner = row ? row->GetDataGridOwner() : nullptr;
		if (owner && TryResolveResizeRow(
			input.Y, rowIndex, fromTopEdge)
			&& owner->AutoSizeRow(rowIndex))
		{
			auto args = input.CreateMouseEventArgs();
			OnMouseDoubleClick(this, args);
			return true;
		}
	}
	if ((input.Kind == InputReportKind::PointerDown
		|| input.Kind == InputReportKind::PointerDoubleClick)
		&& input.ChangedButton == MouseButton::Left)
		_activationModifiers = input.Modifiers;
	else if (input.Kind == InputReportKind::KeyDown
		&& input.Key == Key::Space)
		_activationModifiers = input.Modifiers;
	else if (input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost)
		_activationModifiers = ModifierKeys::None;
	return Button::ProcessInput(input);
}

bool DataGridRowHeader::OnClick()
{
	const ControlWeakReference headerLifetime(this);
	const ModifierKeys modifiers = _activationModifiers;
	_activationModifiers = ModifierKeys::None;
	const bool clicked = Button::OnClick();
	auto* header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!header) return clicked;
	if (header->IsMouseCaptured()) (void)header->ReleaseMouseCapture();
	header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!header) return clicked;
	auto* row = header->GetRowOwner();
	auto* owner = row ? row->GetDataGridOwner() : nullptr;
	return owner ? owner->HandleRowHeaderClick(*row, modifiers) || clicked
		: clicked;
}

DataGridColumnHeader::DataGridColumnHeader()
{
	// A templated header is normally hit through its Border/ContentPresenter.
	// Consume the routed descendant press at the header boundary so the native
	// resize behavior is independent of the theme's decorative subtree.
	RetainEventConnection(OnMouseDown.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (_isResizing || args.ChangedButton != MouseButton::Left) return;
			size_t columnIndex = DataGridCellInfo::InvalidIndex;
			bool resizeFromLeftEdge = false;
			if (!TryResolveResizeColumn(
				args.X, columnIndex, resizeFromLeftEdge))
			{
				(void)PrepareColumnReorder(args.X, args.Y);
				return;
			}

			// The templated descendant has already started ButtonBase's click
			// transaction.  End it before the resize transaction acquires capture;
			// otherwise its synchronous CaptureLost would cancel the resize again.
			const ControlWeakReference lifetime(this);
			InputReport cancelButtonPress;
			cancelButtonPress.Kind = InputReportKind::Cancel;
			(void)ButtonBase::ProcessInput(cancelButtonPress);
			auto* source = dynamic_cast<DataGridColumnHeader*>(lifetime.Get());
			if (!source || !source->BeginColumnResize(args.X)) return;
			args.Handled = true;
		}));
	RetainEventConnection(OnMouseDoubleClick.Subscribe(
		[this](Control*, MouseEventArgs& args)
		{
			if (args.OriginalSource == this
				|| args.ChangedButton != MouseButton::Left || !_owner) return;
			size_t columnIndex = DataGridCellInfo::InvalidIndex;
			bool resizeFromLeftEdge = false;
			if (!TryResolveResizeColumn(
				args.X, columnIndex, resizeFromLeftEdge)
				|| !_owner->AutoSizeColumn(columnIndex)) return;
			InputReport cancelButtonPress;
			cancelButtonPress.Kind = InputReportKind::Cancel;
			(void)ButtonBase::ProcessInput(cancelButtonPress);
			args.Handled = true;
		}));
}

std::wstring DataGridColumnHeader::ContentPresentationError() const
{
	return LastContentError();
}

bool DataGridColumnHeader::Initialize(
	DataGrid& owner, DataGridColumn& column, size_t index,
	std::wstring* outError)
{
	if (outError) outError->clear();
	const ControlWeakReference ownerLifetime(&owner);
	auto* const expectedColumn = &column;
	_owner = &owner;
	_ownerLifetime = ownerLifetime;
	_column = expectedColumn;
	_columnIndex = index;
	_isReorderPending = false;
	_isReordering = false;
	_reorderStartLocalX = 0.0;
	_reorderStartRenderX = 0.0;
	_reorderStartRenderY = 0.0;
	const auto resolveOwner = [&]() -> DataGrid*
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live && index < live->ColumnCount()
			&& live->GetColumnFromDisplayIndex(index) == expectedColumn)
			return live;
		_owner = nullptr;
		_ownerLifetime = nullptr;
		_column = nullptr;
		if (outError && outError->empty())
			*outError = L"DataGrid 列标题生成期间列集合或宿主已失效。";
		return nullptr;
	};
	const auto style = owner.EffectiveColumnHeaderStyle(column);
	cui::framework::StyleAccess::SetResourceKey(*this, style);
	auto* liveOwner = resolveOwner();
	if (!liveOwner) return false;
	auto* liveColumn = liveOwner->GetColumnFromDisplayIndex(index);
	const auto content = liveColumn->GetHeader();
	SetContent(content);
	liveOwner = resolveOwner();
	if (!liveOwner) return false;
	if (const auto error = ContentPresentationError(); !error.empty())
	{
		if (outError) *outError = L"DataGrid 第 "
			+ std::to_wstring(index + 1)
			+ L" 列标题内容生成失败：" + error;
		return false;
	}
	liveColumn = liveOwner->GetColumnFromDisplayIndex(index);
	const auto contentTemplate = liveColumn->GetHeaderTemplate();
	SetContentTemplate(contentTemplate);
	liveOwner = resolveOwner();
	if (!liveOwner) return false;
	if (const auto error = ContentPresentationError(); !error.empty())
	{
		if (outError) *outError = L"DataGrid 第 "
			+ std::to_wstring(index + 1)
			+ L" 列标题模板安装失败：" + error;
		return false;
	}
	const auto height = liveOwner->GetColumnHeaderHeight();
	if (std::isfinite(height))
		SetHeight(cui::layout::Length::Fixed(
			static_cast<float>(height)));
	else SetHeight(cui::layout::Length::Auto());
	return resolveOwner() != nullptr;
}

void DataGridColumnHeader::OnApplyTemplate()
{
	const ControlWeakReference headerLifetime(this);
	const auto ownerLifetime = _ownerLifetime;
	Button::OnApplyTemplate();
	auto* header = dynamic_cast<DataGridColumnHeader*>(headerLifetime.Get());
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!header || !owner) return;
	auto* presenter = owner->GetColumnHeadersPresenter();
	if (!presenter
		|| presenter->GetHeader(header->_columnIndex) != header) return;
	auto* column = owner->GetColumnFromDisplayIndex(header->_columnIndex);
	if (!column || column != header->_column) return;
	const auto error = header->ContentPresentationError();
	if (!error.empty())
		owner->PublishColumnHeaderPresentationError(*header,
			L"DataGrid 列标题模板应用失败：" + error);
	else owner->ClearColumnHeaderPresentationError(*header);
}

bool DataGridColumnHeader::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::PointerDown
		&& input.ChangedButton == MouseButton::Left
		&& BeginColumnResize(input.X))
	{
		auto args = input.CreateMouseEventArgs();
		OnMouseDown(this, args);
		return true;
	}
	if (input.Kind == InputReportKind::PointerMove && _isResizing)
	{
		(void)ContinueColumnResize(input.X);
		auto args = input.CreateMouseEventArgs();
		OnMouseMove(this, args);
		return true;
	}
	if (input.Kind == InputReportKind::PointerMove && _isReordering)
	{
		if (!input.IsButtonPressed(MouseButton::Left))
		{
			EndColumnReorder(true);
			return true;
		}
		const ControlWeakReference lifetime(this);
		(void)ContinueColumnReorder(input.X, input.Y);
		if (auto* source = dynamic_cast<DataGridColumnHeader*>(lifetime.Get()))
		{
			auto args = input.CreateMouseEventArgs();
			source->OnMouseMove(source, args);
		}
		return true;
	}
	if (input.Kind == InputReportKind::PointerMove && _isReorderPending)
	{
		if (!input.IsButtonPressed(MouseButton::Left))
			_isReorderPending = false;
		else
		{
			if (std::abs(static_cast<double>(input.X)
				- _reorderStartLocalX) > DataGridColumnHeaderDragThreshold)
			{
				(void)BeginColumnReorder(input.X, input.Y);
				return true;
			}
		}
	}
	if (input.Kind == InputReportKind::PointerUp && _isResizing
		&& input.ChangedButton == MouseButton::Left)
	{
		EndColumnResize(false);
		auto args = input.CreateMouseEventArgs();
		OnMouseUp(this, args);
		return true;
	}
	if (input.Kind == InputReportKind::PointerUp && _isReordering
		&& input.ChangedButton == MouseButton::Left)
	{
		const ControlWeakReference lifetime(this);
		auto args = input.CreateMouseEventArgs();
		OnMouseUp(this, args);
		if (auto* source = dynamic_cast<DataGridColumnHeader*>(lifetime.Get()))
			source->EndColumnReorder(false);
		return true;
	}
	if ((input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost) && _isResizing)
	{
		EndColumnResize(true);
		return true;
	}
	if ((input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost) && _isReordering)
	{
		EndColumnReorder(true);
		return true;
	}
	if ((input.Kind == InputReportKind::Cancel
		|| input.Kind == InputReportKind::CaptureLost
		|| (input.Kind == InputReportKind::PointerUp
			&& input.ChangedButton == MouseButton::Left))
		&& _isReorderPending)
		_isReorderPending = false;
	if (input.Kind == InputReportKind::PointerDoubleClick
		&& input.ChangedButton == MouseButton::Left)
	{
		size_t columnIndex = DataGridCellInfo::InvalidIndex;
		bool resizeFromLeftEdge = false;
		if (TryResolveResizeColumn(
			input.X, columnIndex, resizeFromLeftEdge)
			&& _owner && _owner->AutoSizeColumn(columnIndex))
		{
			auto args = input.CreateMouseEventArgs();
			OnMouseDoubleClick(this, args);
			return true;
		}
	}
	if (input.Kind == InputReportKind::PointerDown
		&& input.ChangedButton == MouseButton::Left)
		(void)PrepareColumnReorder(input.X, input.Y);
	if (input.Kind == InputReportKind::PointerDown
		|| input.Kind == InputReportKind::KeyDown)
	{
		_multiColumnSortRequested =
			input.HasModifier(ModifierKeys::Shift);
	}
	return Button::ProcessInput(input);
}

bool DataGridColumnHeader::PrepareColumnReorder(int localX, int localY)
{
	if (_isResizing || _isReordering || !_owner || !_column
		|| !_owner->IsColumnReorderEligible(*_column)
		|| _columnIndex == DataGridCellInfo::InvalidIndex
		|| _columnIndex != _column->GetDisplayIndex()) return false;
	const auto size = GetActualSizeDip();
	if (localY < 0 || static_cast<double>(localY) > size.height
		|| localX < 0 || static_cast<double>(localX) > size.width)
		return false;
	size_t resizeColumn = DataGridCellInfo::InvalidIndex;
	bool resizeFromLeftEdge = false;
	if (TryResolveResizeColumn(
		localX, resizeColumn, resizeFromLeftEdge)) return false;
	const auto point = ToRenderSpace(
		*this, static_cast<float>(localX), static_cast<float>(localY));
	_reorderStartLocalX = localX;
	_reorderStartRenderX = point.x;
	_reorderStartRenderY = point.y;
	_isReorderPending = true;
	return true;
}

bool DataGridColumnHeader::BeginColumnReorder(int localX, int localY)
{
	if (!_isReorderPending || !_owner || !_column) return false;
	const auto point = ToRenderSpace(
		*this, static_cast<float>(localX), static_cast<float>(localY));
	_isReorderPending = false;
	const ControlWeakReference headerLifetime(this);
	const ControlWeakReference ownerLifetime(_owner);
	InputReport cancelButtonPress;
	cancelButtonPress.Kind = InputReportKind::Cancel;
	(void)ButtonBase::ProcessInput(cancelButtonPress);
	auto* source = dynamic_cast<DataGridColumnHeader*>(headerLifetime.Get());
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!source || !owner || source->_owner != owner || !source->_column
		|| !owner->IsColumnReorderEligible(*source->_column)) return false;
	auto* presenter = owner->GetColumnHeadersPresenter();
	if (!presenter || !presenter->ContainsControl(source)) return false;
	const ControlWeakReference presenterLifetime(presenter);
	const auto result = presenter->BeginColumnHeaderDrag(
		*source, point.x, point.y);
	source = dynamic_cast<DataGridColumnHeader*>(headerLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!source || !owner || result != DataGridColumnHeadersPresenter::
		ColumnReorderStartResult::Started) return false;
	source->_isReordering = true;
	if (!source->GetPresentationWindow()) return true;
	const bool captured = source->CaptureMouse();
	source = dynamic_cast<DataGridColumnHeader*>(headerLifetime.Get());
	if (captured && source && source->_isReordering
		&& source->IsMouseCaptured()) return true;
	if (source) source->_isReordering = false;
	if (auto* livePresenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get()))
		livePresenter->FinishColumnHeaderDrag(true);
	return false;
}

bool DataGridColumnHeader::ContinueColumnReorder(int localX, int localY)
{
	if (!_isReordering || !_owner || !_column) return false;
	auto* presenter = _owner->GetColumnHeadersPresenter();
	if (!presenter || !presenter->GetIsColumnHeaderDragging())
	{
		_isReordering = false;
		return false;
	}
	const auto point = ToRenderSpace(
		*this, static_cast<float>(localX), static_cast<float>(localY));
	if (presenter->UpdateColumnHeaderDrag(point.x, point.y)) return true;
	_isReordering = false;
	return false;
}

void DataGridColumnHeader::EndColumnReorder(bool cancel)
{
	if (!_isReordering && !_isReorderPending) return;
	_isReorderPending = false;
	const bool wasReordering = _isReordering;
	_isReordering = false;
	const ControlWeakReference ownerLifetime(_owner);
	if (IsMouseCaptured()) (void)ReleaseMouseCapture();
	if (!wasReordering) return;
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* presenter = owner ? owner->GetColumnHeadersPresenter() : nullptr;
	if (presenter) presenter->FinishColumnHeaderDrag(cancel);
}

CursorKind DataGridColumnHeader::QueryCursor(int localX, int localY)
{
	if (_isResizing) return CursorKind::SizeWE;
	const auto size = GetActualSizeDip();
	if (localY < 0 || static_cast<double>(localY) > size.height)
		return Button::QueryCursor(localX, localY);
	size_t columnIndex = DataGridCellInfo::InvalidIndex;
	bool resizeFromLeftEdge = false;
	return TryResolveResizeColumn(
		localX, columnIndex, resizeFromLeftEdge)
		? CursorKind::SizeWE : Button::QueryCursor(localX, localY);
}

bool DataGridColumnHeader::TryResolveResizeColumn(
	int localX, size_t& columnIndex,
	bool& resizeFromLeftEdge) const noexcept
{
	columnIndex = DataGridCellInfo::InvalidIndex;
	resizeFromLeftEdge = false;
	if (!_owner || !_owner->GetCanUserResizeColumns()
		|| _columnIndex == DataGridCellInfo::InvalidIndex) return false;
	const auto size = GetActualSizeDip();
	const double width = static_cast<double>(size.width);
	if (!std::isfinite(width) || width <= 0.0) return false;
	if (localX < 0 || static_cast<double>(localX) > width) return false;
	if (static_cast<double>(localX) >= width - DataGridColumnHeaderGripperWidth)
		columnIndex = _columnIndex;
	else if (localX <= DataGridColumnHeaderGripperWidth && _columnIndex > 0)
	{
		// The same divider exposes two distinct resize handles. Its left
		// header adjusts the left column from the right; its right header adjusts
		// the right column from the left. A fixed right column falls back to the
		// former behavior so the divider remains draggable from either hit zone.
		auto* current = _owner->GetColumnFromDisplayIndex(_columnIndex);
		if (current && current->GetCanUserResize())
		{
			columnIndex = _columnIndex;
			resizeFromLeftEdge = true;
		}
		else if (current
			&& current->_visibleIndex != DataGridColumn::UnsetDisplayIndex
			&& current->_visibleIndex > 0)
			columnIndex = _owner->_visibleColumns[
				current->_visibleIndex - 1]->_displayIndex;
		else return false;
	}
	else return false;
	auto* column = _owner->GetColumnFromDisplayIndex(columnIndex);
	return column && column->GetVisibility() == Visibility::Visible
		&& column->GetCanUserResize();
}

bool DataGridColumnHeader::BeginColumnResize(int localX)
{
	size_t columnIndex = DataGridCellInfo::InvalidIndex;
	bool resizeFromLeftEdge = false;
	if (!TryResolveResizeColumn(
		localX, columnIndex, resizeFromLeftEdge) || !_owner) return false;
	const double width = _owner->GetColumnDisplayWidth(columnIndex);
	if (!std::isfinite(width)
		|| !_owner->BeginColumnResizeTransaction(
			columnIndex, resizeFromLeftEdge)) return false;
	const ControlWeakReference ownerLifetime(_owner);
	bool transactionCommitted = false;
	auto rollbackTransaction = MakeScopeExit(
		[ownerLifetime, &transactionCommitted]
		{
			if (transactionCommitted) return;
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				owner->EndColumnResizeTransaction(true);
		});
	_isResizing = true;
	_resizeFromLeftEdge = resizeFromLeftEdge;
	_resizingColumnIndex = columnIndex;
	_resizeStartRenderX = RenderSpaceX(*this, localX);
	_resizeStartWidth = width;
	// Normalized-input unit tests may exercise a detached header.  A presented
	// header, however, must own native mouse capture for the drag to be valid.
	if (!GetPresentationWindow())
	{
		transactionCommitted = true;
		return true;
	}
	const ControlWeakReference lifetime(this);
	const bool captured = CaptureMouse();
	auto* source = dynamic_cast<DataGridColumnHeader*>(lifetime.Get());
	if (!source) return false;
	if (!captured || !source->_isResizing || !source->IsMouseCaptured())
	{
		if (source->_isResizing) source->EndColumnResize(true);
		return false;
	}
	transactionCommitted = true;
	return true;
}

bool DataGridColumnHeader::ContinueColumnResize(int localX)
{
	if (!_isResizing || !_owner
		|| _resizingColumnIndex == DataGridCellInfo::InvalidIndex) return false;
	const double delta = RenderSpaceX(*this, localX) - _resizeStartRenderX;
	if (_owner->ResizeColumnInTransaction(
		_resizingColumnIndex,
		_resizeStartWidth + (_resizeFromLeftEdge ? -delta : delta)))
		return true;
	// An external schema/eligibility mutation can abort the owner transaction
	// while this header still owns capture. End the local gesture immediately so
	// later Move/Up input is not swallowed or accidentally committed.
	EndColumnResize(true);
	return false;
}

void DataGridColumnHeader::EndColumnResize(bool cancel)
{
	if (!_isResizing) return;
	_isResizing = false;
	_resizeFromLeftEdge = false;
	_resizingColumnIndex = DataGridCellInfo::InvalidIndex;
	_resizeStartRenderX = 0.0;
	_resizeStartWidth = 0.0;
	const ControlWeakReference ownerLifetime(_owner);
	if (!cancel)
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			(void)owner->ApplyPendingColumnResizeInput();
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		owner->EndColumnResizeTransaction(cancel);
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		owner->ApplyPendingColumnWidths();
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		owner->CommitColumnWidthLayoutToAncestors();
	if (IsMouseCaptured()) (void)ReleaseMouseCapture();
	// Reconciliation can replace this header, so it must be the final action.
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		owner->InvalidateRealizedColumnRange();
		owner->RefreshRealizedColumns();
	}
}

bool DataGridColumnHeader::OnClick()
{
	const ControlWeakReference lifetime(this);
	const bool sorted = _owner && _column
		? _owner->PerformSort(*_column, _multiColumnSortRequested)
		: false;
	if (lifetime.Get() != this) return sorted;
	_multiColumnSortRequested = false;
	const bool clicked = Button::OnClick();
	return sorted || clicked;
}

DataGridColumnHeadersPresenter::DataGridColumnHeadersPresenter() = default;

DataGridColumnHeadersPresenter::ColumnReorderStartResult
DataGridColumnHeadersPresenter::BeginColumnHeaderDrag(
	DataGridColumnHeader& header, double renderX, double renderY)
{
	if (_isColumnHeaderDragging || !_owner
		|| _owner->GetColumnHeadersPresenter() != this
		|| !ContainsControl(&header) || header._owner != _owner
		|| !header._column
		|| !_owner->IsColumnReorderEligible(*header._column))
		return ColumnReorderStartResult::Aborted;
	const auto sourceRect = header.GetRenderedAbsoluteRectDip();
	const double width = sourceRect.right - sourceRect.left;
	const double height = sourceRect.bottom - sourceRect.top;
	if (!std::isfinite(width) || !std::isfinite(height)
		|| width <= 0.0 || height <= 0.0)
		return ColumnReorderStartResult::Aborted;

	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(_owner);
	_isColumnHeaderDragging = true;
	_reorderingColumn = header._column;
	_reorderStartRenderX = header._reorderStartRenderX;
	_reorderStartRenderY = header._reorderStartRenderY;
	_reorderLastRenderX = renderX;
	_reorderLastRenderY = renderY;
	_reorderCurrentRenderX = renderX;
	_reorderCurrentRenderY = renderY;
	_dragPointerOffsetX = header._reorderStartRenderX - sourceRect.left;
	_dragPointerOffsetY = header._reorderStartRenderY - sourceRect.top;
	_dragIndicatorWidth = width;
	_dragIndicatorHeight = height;
	_dropDisplayIndex = DataGridCellInfo::InvalidIndex;
	_dropIndicatorVisible = false;

	if (!_owner->ColumnHeaderDragStarted.Empty())
	{
		const auto presenterRect = GetRenderedAbsoluteRectDip();
		DataGridColumnHeaderDragStartedEventArgs args;
		args.Column = _reorderingColumn;
		args.HorizontalOffset = renderX - presenterRect.left;
		args.VerticalOffset = renderY - presenterRect.top;
		cui::framework::EventAccess::RaiseWhile(
			_owner->ColumnHeaderDragStarted,
			[&]()
			{
				return presenterLifetime.Get() != nullptr
					&& ownerLifetime.Get() != nullptr;
			}, _owner, args);
	}
	auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (presenter && !owner)
	{
		presenter->AbandonColumnHeaderDragAfterOwnerDestruction();
		return ColumnReorderStartResult::Aborted;
	}
	if (!presenter || !owner || !presenter->_isColumnHeaderDragging
		|| presenter->_owner != owner || !presenter->_reorderingColumn
		|| !owner->IsColumnReorderEligible(
			*presenter->_reorderingColumn))
		return ColumnReorderStartResult::Aborted;

	DataGridColumnReorderingEventArgs reordering;
	reordering.Column = presenter->_reorderingColumn;
	auto dragIndicator = std::make_unique<DataGridColumnReorderIndicator>();
	dragIndicator->SetBackground(cui::drawing::MakeSolidColorBrush(
		D2D1_COLOR_F{ 0.18f, 0.48f, 0.86f, 0.32f }));
	reordering.DragIndicator = std::move(dragIndicator);
	auto dropIndicator = std::make_unique<DataGridColumnReorderIndicator>();
	dropIndicator->SetBackground(cui::drawing::MakeSolidColorBrush(
		D2D1_COLOR_F{ 0.18f, 0.48f, 0.86f, 1.0f }));
	reordering.DropLocationIndicator = std::move(dropIndicator);
	if (!owner->ColumnReordering.Empty())
		cui::framework::EventAccess::RaiseWhile(
			owner->ColumnReordering,
			[&]()
			{
				return presenterLifetime.Get() != nullptr
					&& ownerLifetime.Get() != nullptr;
			}, owner, reordering);
	presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (presenter && !owner)
	{
		presenter->AbandonColumnHeaderDragAfterOwnerDestruction();
		return ColumnReorderStartResult::Aborted;
	}
	if (!presenter || !owner || !presenter->_isColumnHeaderDragging)
		return ColumnReorderStartResult::Aborted;
	if (reordering.Cancel)
	{
		presenter->FinishColumnHeaderDrag(true);
		return ColumnReorderStartResult::Canceled;
	}
	if (presenter->_owner != owner || !presenter->_reorderingColumn
		|| !owner->IsColumnReorderEligible(*presenter->_reorderingColumn))
	{
		presenter->FinishColumnHeaderDrag(true);
		return ColumnReorderStartResult::Aborted;
	}

	try
	{
		if (reordering.DragIndicator)
		{
			auto* indicator = presenter->AddOwned(
				std::move(reordering.DragIndicator));
			if (!presenterLifetime.Get() || !indicator
				|| !presenter->ContainsControl(indicator))
			{
				if (auto* live = dynamic_cast<
					DataGridColumnHeadersPresenter*>(presenterLifetime.Get()))
					live->FinishColumnHeaderDrag(true);
				return ColumnReorderStartResult::Aborted;
			}
			indicator->SetZIndex(3);
			presenter->_dragIndicator = ControlWeakReference(indicator);
		}
		if (reordering.DropLocationIndicator)
		{
			auto* indicator = presenter->AddOwned(
				std::move(reordering.DropLocationIndicator));
			if (!presenterLifetime.Get() || !indicator
				|| !presenter->ContainsControl(indicator))
			{
				if (auto* live = dynamic_cast<
					DataGridColumnHeadersPresenter*>(presenterLifetime.Get()))
					live->FinishColumnHeaderDrag(true);
				return ColumnReorderStartResult::Aborted;
			}
			indicator->SetZIndex(4);
			presenter->_dropIndicator = ControlWeakReference(indicator);
		}
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get()))
			live->FinishColumnHeaderDrag(true);
		return ColumnReorderStartResult::Aborted;
	}
	presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	if (!presenter || !presenter->_isColumnHeaderDragging)
		return ColumnReorderStartResult::Aborted;
	size_t displayIndex = DataGridCellInfo::InvalidIndex;
	double boundary = 0.0;
	bool valid = false;
	(void)presenter->ResolveColumnHeaderDrop(
		renderX, renderY, displayIndex, boundary, valid);
	presenter->_dropDisplayIndex = valid
		? displayIndex : DataGridCellInfo::InvalidIndex;
	presenter->_dropIndicatorBoundary = boundary;
	presenter->_dropIndicatorVisible = valid;
	presenter->ArrangeColumnHeaderDragIndicators();
	return ColumnReorderStartResult::Started;
}

bool DataGridColumnHeadersPresenter::UpdateColumnHeaderDrag(
	double renderX, double renderY)
{
	if (!_isColumnHeaderDragging || !_owner || !_reorderingColumn)
		return false;
	if (!_owner->IsColumnReorderEligible(*_reorderingColumn))
	{
		FinishColumnHeaderDrag(true);
		return false;
	}
	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(_owner);
	const double horizontalChange = renderX - _reorderLastRenderX;
	const double verticalChange = renderY - _reorderLastRenderY;
	_reorderLastRenderX = renderX;
	_reorderLastRenderY = renderY;
	_reorderCurrentRenderX = renderX;
	_reorderCurrentRenderY = renderY;
	size_t displayIndex = DataGridCellInfo::InvalidIndex;
	double boundary = 0.0;
	bool valid = false;
	(void)ResolveColumnHeaderDrop(
		renderX, renderY, displayIndex, boundary, valid);
	_dropDisplayIndex = valid
		? displayIndex : DataGridCellInfo::InvalidIndex;
	_dropIndicatorBoundary = boundary;
	_dropIndicatorVisible = valid;
	ArrangeColumnHeaderDragIndicators();
	if (!_owner->ColumnHeaderDragDelta.Empty())
	{
		DataGridColumnHeaderDragDeltaEventArgs args;
		args.Column = _reorderingColumn;
		args.HorizontalChange = horizontalChange;
		args.VerticalChange = verticalChange;
		cui::framework::EventAccess::RaiseWhile(
			_owner->ColumnHeaderDragDelta,
			[&]()
			{
				return presenterLifetime.Get() != nullptr
					&& ownerLifetime.Get() != nullptr;
			}, _owner, args);
	}
	auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	if (presenter && !ownerLifetime.Get())
	{
		presenter->AbandonColumnHeaderDragAfterOwnerDestruction();
		return false;
	}
	return presenter && presenter->_isColumnHeaderDragging;
}

bool DataGridColumnHeadersPresenter::ResolveColumnHeaderDrop(
	double renderX, double renderY, size_t& displayIndex,
	double& boundary, bool& valid) const
{
	displayIndex = DataGridCellInfo::InvalidIndex;
	boundary = 0.0;
	valid = false;
	if (!_isColumnHeaderDragging || !_owner || !_reorderingColumn
		|| _owner->_visibleColumns.empty()
		|| !_owner->EnsureColumnWidthPrefix()
		|| _owner->_columnWidthPrefix.size() != _owner->ColumnCount() + 1)
		return false;
	const auto presenterRect = const_cast<
		DataGridColumnHeadersPresenter*>(this)->GetRenderedAbsoluteRectDip();
	const double logicalPointerX = renderX - presenterRect.left;
	const double viewportX = logicalPointerX - _owner->_horizontalScrollOffset;
	const size_t frozenEnd = (std::min)(_owner->ColumnCount(),
		static_cast<size_t>((std::max)(0, _owner->GetFrozenColumnCount())));
	const double frozenWidth = _owner->_columnWidthPrefix[frozenEnd];
	const double targetX = viewportX < frozenWidth
		? viewportX : logicalPointerX;
	const auto& visible = _owner->_visibleColumns;
	const auto position = std::lower_bound(
		visible.begin(), visible.end(), targetX,
		[&](const DataGridColumn* column, double x)
		{
			const size_t index = column->GetDisplayIndex();
			return (_owner->_columnWidthPrefix[index]
				+ _owner->_columnWidthPrefix[index + 1]) * 0.5 < x;
		});
	const size_t insertion = position == visible.end()
		? _owner->ColumnCount() : (*position)->GetDisplayIndex();
	boundary = _owner->_columnWidthPrefix[insertion];
	const size_t sourceIndex = _reorderingColumn->GetDisplayIndex();
	if (sourceIndex >= _owner->ColumnCount()) return false;
	displayIndex = insertion;
	if (sourceIndex < displayIndex) --displayIndex;
	if (displayIndex >= _owner->ColumnCount())
		displayIndex = _owner->ColumnCount() - 1;
	const bool sourceFrozen = sourceIndex < frozenEnd;
	const bool targetFrozen = displayIndex < frozenEnd;
	const double localY = renderY - presenterRect.top;
	valid = std::isfinite(localY)
		&& localY >= -2.0 * _dragIndicatorHeight
		&& localY <= 3.0 * _dragIndicatorHeight
		&& sourceFrozen == targetFrozen;
	return true;
}

void DataGridColumnHeadersPresenter::ArrangeColumnHeaderDragIndicators()
{
	if (!_isColumnHeaderDragging || !_owner) return;
	const auto presenterRect = GetRenderedAbsoluteRectDip();
	const auto size = GetActualSizeDip();
	const auto toFloat = [](double value)
	{
		const double limit = static_cast<double>(
			(std::numeric_limits<float>::max)());
		return static_cast<float>((std::clamp)(value, -limit, limit));
	};
	if (auto* indicator = _dragIndicator.Get();
		indicator && ContainsControl(indicator))
	{
		indicator->ClearRenderTransform();
		indicator->Arrange(cui::core::Rect{
			toFloat(_reorderCurrentRenderX - presenterRect.left
				- _dragPointerOffsetX),
			toFloat(_reorderCurrentRenderY - presenterRect.top
				- _dragPointerOffsetY),
			toFloat(_dragIndicatorWidth),
			toFloat(_dragIndicatorHeight) });
	}
	if (auto* indicator = _dropIndicator.Get();
		indicator && ContainsControl(indicator))
	{
		const bool frozen = _reorderingColumn
			&& _reorderingColumn->GetDisplayIndex() < static_cast<size_t>(
				(std::max)(0, _owner->GetFrozenColumnCount()));
		if (frozen) indicator->SetRenderTransform(
			HorizontalTranslation(_owner->_horizontalScrollOffset));
		else indicator->ClearRenderTransform();
		indicator->Arrange(cui::core::Rect{
			toFloat(_dropIndicatorBoundary), 0.0f,
			_dropIndicatorVisible
				? static_cast<float>(DataGridColumnDropIndicatorWidth) : 0.0f,
			(std::max)(size.height, toFloat(_dragIndicatorHeight)) });
	}
}

void DataGridColumnHeadersPresenter::FinishColumnHeaderDrag(bool cancel)
{
	if (!_isColumnHeaderDragging) return;
	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(_owner);
	DataGrid* owner = _owner;
	DataGridColumn* column = _reorderingColumn;
	const size_t target = _dropDisplayIndex;
	const double horizontalChange =
		_reorderCurrentRenderX - _reorderStartRenderX;
	const double verticalChange =
		_reorderCurrentRenderY - _reorderStartRenderY;
	cancel = cancel || target == DataGridCellInfo::InvalidIndex;
	_isColumnHeaderDragging = false;
	_reorderingColumn = nullptr;
	_dropDisplayIndex = DataGridCellInfo::InvalidIndex;
	_dropIndicatorVisible = false;
	if (column && column->GetDataGridOwner() == owner)
	{
		if (auto* header = GetHeader(column->GetDisplayIndex()))
		{
			header->_isReorderPending = false;
			header->_isReordering = false;
			if (header->IsMouseCaptured()) (void)header->ReleaseMouseCapture();
		}
	}
	if (auto* indicator = _dragIndicator.Get();
		indicator && ContainsControl(indicator))
		(void)DeleteVisualChild(indicator);
	_dragIndicator = nullptr;
	if (auto* indicator = _dropIndicator.Get();
		indicator && ContainsControl(indicator))
		(void)DeleteVisualChild(indicator);
	_dropIndicator = nullptr;

	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!owner)
	{
		AbandonColumnHeaderDragAfterOwnerDestruction();
		return;
	}
	if (!owner->ColumnHeaderDragCompleted.Empty())
	{
		DataGridColumnHeaderDragCompletedEventArgs args;
		args.Column = column;
		args.HorizontalChange = horizontalChange;
		args.VerticalChange = verticalChange;
		args.Canceled = cancel;
		cui::framework::EventAccess::RaiseWhile(
			owner->ColumnHeaderDragCompleted,
			[&]() { return ownerLifetime.Get() != nullptr; },
			owner, args);
	}
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!owner)
	{
		if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get()))
			presenter->AbandonColumnHeaderDragAfterOwnerDestruction();
		return;
	}
	if (cancel || !column || target >= owner->ColumnCount()) return;
	auto ownedColumn = std::find_if(
		owner->_columns.begin(), owner->_columns.end(),
		[column](const auto& candidate) { return candidate.get() == column; });
	if (ownedColumn == owner->_columns.end()
		|| !owner->IsColumnReorderEligible(**ownedColumn)
		|| (*ownedColumn)->GetDisplayIndex() == target) return;
	if (!owner->SetColumnDisplayIndex(**ownedColumn, target)) return;
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!owner || owner->ColumnReordered.Empty())
		return;
	ownedColumn = std::find_if(
		owner->_columns.begin(), owner->_columns.end(),
		[column](const auto& candidate) { return candidate.get() == column; });
	if (ownedColumn == owner->_columns.end()) return;
	DataGridColumnEventArgs args;
	args.Column = ownedColumn->get();
	cui::framework::EventAccess::RaiseWhile(
		owner->ColumnReordered,
		[&]()
		{
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return live && std::any_of(
				live->_columns.begin(), live->_columns.end(),
				[column](const auto& candidate)
				{ return candidate.get() == column; });
		}, owner, args);
}

void DataGridColumnHeadersPresenter::
AbandonColumnHeaderDragAfterOwnerDestruction()
{
	_isColumnHeaderDragging = false;
	_reorderingColumn = nullptr;
	_dropDisplayIndex = DataGridCellInfo::InvalidIndex;
	_dropIndicatorVisible = false;
	for (auto* header : _frozenHeaders)
		if (header)
		{
			header->_owner = nullptr;
			header->_ownerLifetime = nullptr;
			header->_column = nullptr;
			header->_isReorderPending = false;
			header->_isReordering = false;
		}
	for (auto* header : _headers)
		if (header)
		{
			header->_owner = nullptr;
			header->_ownerLifetime = nullptr;
			header->_column = nullptr;
			header->_isReorderPending = false;
			header->_isReordering = false;
		}
	if (auto* indicator = _dragIndicator.Get();
		indicator && ContainsControl(indicator))
		(void)DeleteVisualChild(indicator);
	_dragIndicator = nullptr;
	if (auto* indicator = _dropIndicator.Get();
		indicator && ContainsControl(indicator))
		(void)DeleteVisualChild(indicator);
	_dropIndicator = nullptr;
	_owner = nullptr;
}

bool DataGridColumnHeadersPresenter::Initialize(
	DataGrid& owner, std::wstring* outError)
{
	if (outError) outError->clear();
	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(&owner);
	auto generation = ++_initializeGeneration;
	if (generation == 0) generation = ++_initializeGeneration;
	const auto committedAtEntry = _committedInitializeGeneration;
	bool supersededByNestedInitialize = false;
	const auto wasSuperseded = [&]() noexcept
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		return presenter && ownerLifetime.Get()
			&& presenter->_committedInitializeGeneration != committedAtEntry
			&& presenter->_committedInitializeGeneration != generation;
	};
	const auto fail = [&](std::wstring message)
	{
		if (outError && outError->empty()) *outError = std::move(message);
		return false;
	};
	try
	{
		auto& tracked = owner._trackedColumnHeaderPresenters;
		if (std::find_if(tracked.begin(), tracked.end(),
			[this](const ControlWeakReference& lifetime)
				{ return lifetime.Get() == this; }) == tracked.end())
			tracked.emplace_back(this);
	}
	catch (...)
	{
		return fail(L"DataGrid 列标题呈现器生命期登记失败。");
	}
	if (_isColumnHeaderDragging)
	{
		FinishColumnHeaderDrag(true);
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!presenter || !ownerLifetime.Get())
			return fail(L"DataGrid 列标题拖拽终止期间宿主已失效。");
		if (wasSuperseded()) return true;
	}
	const size_t columnCount = owner.ColumnCount();
	const size_t columnRevision = owner._columnWidthProjectionRevision;
	const auto realizedColumns = owner.ResolveRealizedColumnRange();
	auto* resolvedOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (wasSuperseded()) return true;
	if (!resolvedOwner || resolvedOwner->ColumnCount() != columnCount
		|| resolvedOwner->_columnWidthProjectionRevision != columnRevision)
		return fail(L"DataGrid 列标题范围解析期间宿主或列集合已变化。");
	const bool nextStorageIsSparse = resolvedOwner->GetEnableColumnVirtualization()
		&& columnCount > 0;
	const size_t nextFrozenEnd = nextStorageIsSparse
		? (std::min)(columnCount, static_cast<size_t>(
			(std::max)(0, resolvedOwner->GetFrozenColumnCount()))) : 0;
	const size_t nextBegin = nextStorageIsSparse
		? realizedColumns.first : 0;
	const size_t nextEnd = nextStorageIsSparse
		? realizedColumns.second : columnCount;
	if (nextStorageIsSparse && nextBegin < nextFrozenEnd)
		return fail(L"DataGrid 列标题虚拟化范围无效。");

	const auto committedTreeMatchesCurrentRange = [&]() noexcept
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!presenter || !liveOwner) return false;
		if (!presenter->_owner && presenter->_frozenHeaders.empty()
			&& presenter->_headers.empty()
			&& presenter->VisualChildCount() == 0) return true;
		if (presenter->_owner != liveOwner
			|| presenter->_columnStorageIsSparse != nextStorageIsSparse
			|| presenter->_realizedFrozenColumnEnd != nextFrozenEnd
			|| presenter->_realizedColumnBegin != nextBegin
			|| presenter->_realizedColumnEnd != nextEnd
			|| presenter->_frozenHeaders.size() != nextFrozenEnd
			|| presenter->_headers.size() != (nextStorageIsSparse
				? nextEnd - nextBegin : columnCount)) return false;

		std::vector<DataGridColumnHeader*> expectedHeaders;
		expectedHeaders.reserve(presenter->_frozenHeaders.size()
			+ presenter->_headers.size());
		const auto validateSlot = [&](size_t index,
			DataGridColumnHeader* header) noexcept
		{
			auto* column = liveOwner->GetColumnFromDisplayIndex(index);
			if (!column || column->GetVisibility() != Visibility::Visible)
				return header == nullptr;
			if (!header || header->_owner != liveOwner
				|| header->_ownerLifetime.Get() != liveOwner
				|| header->_column != column
				|| header->_columnIndex != index
				|| !presenter->ContainsControl(header)
				|| std::find(expectedHeaders.begin(), expectedHeaders.end(), header)
					!= expectedHeaders.end()) return false;
			expectedHeaders.push_back(header);
			return true;
		};
		for (size_t index = 0; index < presenter->_frozenHeaders.size(); ++index)
			if (!validateSlot(index, presenter->_frozenHeaders[index]))
				return false;
		for (size_t slot = 0; slot < presenter->_headers.size(); ++slot)
		{
			const size_t index = nextStorageIsSparse
				? nextBegin + slot : slot;
			if (!validateSlot(index, presenter->_headers[slot])) return false;
		}
		if (presenter->VisualChildCount()
			!= static_cast<int>(expectedHeaders.size())) return false;
		for (int index = 0; index < presenter->VisualChildCount(); ++index)
		{
			auto* header = dynamic_cast<DataGridColumnHeader*>(
				presenter->GetVisualChild(index));
			if (!header || std::find(expectedHeaders.begin(),
				expectedHeaders.end(), header) == expectedHeaders.end())
				return false;
		}
		return true;
	};
	if (!committedTreeMatchesCurrentRange())
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!presenter || (presenter->_owner && presenter->_owner != resolvedOwner))
			return fail(L"DataGrid 列标题呈现器仍属于其他宿主。");
		resolvedOwner->RetireColumnHeadersPresenterNoCallbacks(
			presenter, generation);
		try
		{
			presenter->ClearVisualChildren();
			presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
				presenterLifetime.Get());
			resolvedOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (wasSuperseded()) return true;
			if (!presenter || !resolvedOwner)
				return fail(L"DataGrid 无效旧列标题树清理期间宿主已失效。");
			presenter->ClearColumns();
		}
		catch (...)
		{
			if (wasSuperseded()) return true;
			return fail(L"DataGrid 无效旧列标题树清理失败。");
		}
		resolvedOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!resolvedOwner || resolvedOwner->ColumnCount() != columnCount
			|| resolvedOwner->_columnWidthProjectionRevision != columnRevision)
			return fail(L"DataGrid 无效旧列标题树清理期间列集合已变化。");
	}

	struct HeaderInputSnapshot final
	{
		size_t Index = DataGridCellInfo::InvalidIndex;
		DataGridColumn* Column = nullptr;
		::Visibility ColumnVisibility = ::Visibility::Visible;
		BindingValue Content;
		ItemTemplateReference ContentTemplate;
		std::wstring Style;
	};
	std::vector<HeaderInputSnapshot> inputSnapshots;
	inputSnapshots.reserve(nextFrozenEnd + (nextStorageIsSparse
		? nextEnd - nextBegin : columnCount));

	std::vector<DataGridColumnHeader*> nextFrozen(nextFrozenEnd, nullptr);
	std::vector<DataGridColumnHeader*> nextHeaders(nextStorageIsSparse
		? nextEnd - nextBegin : columnCount, nullptr);
	std::vector<std::unique_ptr<DataGridColumnHeader>> stagedFrozen(nextFrozenEnd);
	std::vector<std::unique_ptr<DataGridColumnHeader>> stagedHeaders(
		nextStorageIsSparse ? nextEnd - nextBegin : columnCount);
	std::vector<ControlWeakReference> stagedHeaderLifetimes;
	stagedHeaderLifetimes.reserve(nextFrozenEnd + stagedHeaders.size());
	try
	{
		const auto stageHeader = [&](size_t index,
			std::vector<DataGridColumnHeader*>& destination,
			std::vector<std::unique_ptr<DataGridColumnHeader>>& staged,
			size_t slot, int layoutColumn) -> bool
		{
			auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
				presenterLifetime.Get());
			if (wasSuperseded())
			{
				supersededByNestedInitialize = true;
				return false;
			}
			if (!liveOwner || !presenter
				|| liveOwner->ColumnCount() != columnCount
				|| liveOwner->_columnWidthProjectionRevision != columnRevision)
				return false;
			auto* column = liveOwner->GetColumnFromDisplayIndex(index);
			if (!column) return false;
			inputSnapshots.push_back({ index, column,
				column->GetVisibility(), column->GetHeader(),
				column->GetHeaderTemplate(),
				liveOwner->EffectiveColumnHeaderStyle(*column) });
			if (column->GetVisibility() != Visibility::Visible) return true;
			auto header = std::make_unique<DataGridColumnHeader>();
			stagedHeaderLifetimes.emplace_back(header.get());
			if (!header->Initialize(*liveOwner, *column, index, outError))
				return false;
			liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
				presenterLifetime.Get());
			if (wasSuperseded())
			{
				supersededByNestedInitialize = true;
				return false;
			}
			if (!liveOwner || !presenter
				|| liveOwner->ColumnCount() != columnCount
				|| liveOwner->_columnWidthProjectionRevision != columnRevision
				|| liveOwner->GetColumnFromDisplayIndex(index) != column)
				return false;
			Grid::SetColumn(*header, layoutColumn);
			destination[slot] = header.get();
			staged[slot] = std::move(header);
			return true;
		};
		if (nextStorageIsSparse)
		{
			for (size_t index = 0; index < nextFrozenEnd; ++index)
				if (!stageHeader(index, nextFrozen, stagedFrozen, index,
					static_cast<int>(index)))
					return supersededByNestedInitialize
						? true
						: fail(L"DataGrid 冻结列标题生成失败。");
			for (size_t index = nextBegin; index < nextEnd; ++index)
			{
				const size_t slot = index - nextBegin;
				if (!stageHeader(index, nextHeaders, stagedHeaders, slot,
					static_cast<int>(nextFrozenEnd + 1 + slot)))
					return supersededByNestedInitialize
						? true
						: fail(L"DataGrid 可滚动列标题生成失败。");
			}
		}
		else
			for (size_t index = 0; index < columnCount; ++index)
				if (!stageHeader(index, nextHeaders, stagedHeaders, index,
					static_cast<int>(index)))
					return supersededByNestedInitialize
						? true
						: fail(L"DataGrid 列标题生成失败。");
	}
	catch (...)
	{
		if (wasSuperseded()) return true;
		return fail(L"DataGrid 列标题生成失败。");
	}

	auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	if (wasSuperseded()) return true;
	if (!liveOwner || !presenter || liveOwner->ColumnCount() != columnCount
		|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		return fail(L"DataGrid 列标题提交前列集合已变化。");
	const auto headerValuesEquivalent = [](const BindingValue& left,
		const BindingValue& right)
	{
		if (BindingValuesEqual(left, right)) return true;
		BindingSourceReference leftSource;
		BindingSourceReference rightSource;
		if (left.TryGet(leftSource) && right.TryGet(rightSource))
			return leftSource == rightSource;
		// BindingValue has no general equality contract for opaque Object values.
		// Their column identity/template/style are still validated transactionally.
		return left.Kind() == BindingValueKind::Object
			&& right.Kind() == BindingValueKind::Object
			&& left.Type() == right.Type();
	};
	const auto inputsStillCurrent = [&](DataGrid& live)
	{
		if (live.ColumnCount() != columnCount
			|| live._columnWidthProjectionRevision != columnRevision)
			return false;
		for (const auto& input : inputSnapshots)
		{
			auto* column = live.GetColumnFromDisplayIndex(input.Index);
			if (!column || column != input.Column
				|| column->GetVisibility() != input.ColumnVisibility
				|| column->GetHeaderTemplate() != input.ContentTemplate
				|| live.EffectiveColumnHeaderStyle(*column) != input.Style
				|| !headerValuesEquivalent(column->GetHeader(), input.Content))
				return false;
		}
		return true;
	};
	if (!inputsStillCurrent(*liveOwner))
		return fail(L"DataGrid 列标题提交前呈现输入已变化。");
	const auto retireHeader = [](DataGridColumnHeader* header) noexcept
	{
		if (!header) return;
		header->_owner = nullptr;
		header->_ownerLifetime = nullptr;
		header->_column = nullptr;
		header->_columnIndex = DataGridCellInfo::InvalidIndex;
		header->_isResizing = false;
		header->_isReorderPending = false;
		header->_isReordering = false;
	};
	const auto retireVisualHeaders = [&](DataGridColumnHeadersPresenter& target)
	{
		for (int childIndex = 0;
			childIndex < target.VisualChildCount(); ++childIndex)
			retireHeader(dynamic_cast<DataGridColumnHeader*>(
				target.GetVisualChild(childIndex)));
	};
	const auto leaveCommittedTreeEmpty = [&]() noexcept
	{
		auto* live = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!live || wasSuperseded()) return;
		retireVisualHeaders(*live);
		for (const auto& lifetime : stagedHeaderLifetimes)
			retireHeader(dynamic_cast<DataGridColumnHeader*>(lifetime.Get()));
		live->_frozenHeaders.clear();
		live->_headers.clear();
		live->_owner = nullptr;
		live->_columnStorageIsSparse = false;
		live->_realizedFrozenColumnEnd = 0;
		live->_realizedColumnBegin = 0;
		live->_realizedColumnEnd = 0;
		live->_appliedColumnWidthProjectionRevision = 0;
		live->_isColumnHeaderDragging = false;
		live->_reorderingColumn = nullptr;
		live->_dropDisplayIndex = DataGridCellInfo::InvalidIndex;
		live->_dropIndicatorVisible = false;
		live->_dragIndicator = nullptr;
		live->_dropIndicator = nullptr;
		try { live->ClearVisualChildren(); }
		catch (...) {}
		live = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!live || wasSuperseded()) return;
		try { live->ClearColumns(); }
		catch (...) {}
	};
	bool initializeCommitted = false;
	const auto retireStagedUnlessNestedTreeAdopted = [&]() noexcept
	{
		auto* currentOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* transactionPresenter = dynamic_cast<
			DataGridColumnHeadersPresenter*>(presenterLifetime.Get());
		for (const auto& lifetime : stagedHeaderLifetimes)
		{
			auto* header = dynamic_cast<DataGridColumnHeader*>(lifetime.Get());
			if (!header) continue;
			DataGridColumnHeadersPresenter* adoptingPresenter = nullptr;
			if (currentOwner)
				adoptingPresenter = currentOwner->GetColumnHeadersPresenter();
			if (!adoptingPresenter && transactionPresenter
				&& transactionPresenter->_owner == currentOwner)
				adoptingPresenter = transactionPresenter;
			const bool nestedGenerationOwnsTree = adoptingPresenter
				&& (adoptingPresenter != transactionPresenter
					|| (adoptingPresenter->_committedInitializeGeneration
						!= committedAtEntry
						&& adoptingPresenter->_committedInitializeGeneration
						!= generation));
			const size_t index = header->_columnIndex;
			const bool adopted = nestedGenerationOwnsTree && currentOwner
				&& index < currentOwner->ColumnCount()
				&& header->_owner == currentOwner
				&& header->_ownerLifetime.Get() == currentOwner
				&& header->_column
					== currentOwner->GetColumnFromDisplayIndex(index)
				&& adoptingPresenter->GetHeader(index) == header
				&& adoptingPresenter->ContainsControl(header);
			if (!adopted) retireHeader(header);
		}
	};
	auto retireStagedOnExit = MakeScopeExit([&]() noexcept
	{
		if (!initializeCommitted)
			retireStagedUnlessNestedTreeAdopted();
	});
	try
	{
		// All user template factories have succeeded. Only now retire the old
		// realized tree; a staging failure above leaves it completely untouched.
		retireVisualHeaders(*presenter);
		presenter->_frozenHeaders.clear();
		presenter->_headers.clear();
		presenter->ClearVisualChildren();
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题旧视觉树清理期间状态已变化。");
		}
		presenter->ClearColumns();
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题布局列清理期间状态已变化。");
		}
		presenter->_owner = liveOwner;
		presenter->_columnStorageIsSparse = nextStorageIsSparse;
		presenter->_realizedFrozenColumnEnd = nextFrozenEnd;
		presenter->_realizedColumnBegin = nextBegin;
		presenter->_realizedColumnEnd = nextEnd;
		presenter->SetVisibility(HasColumnHeaders(liveOwner->GetHeadersVisibility())
			? Visibility::Visible : Visibility::Collapsed);
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题可见性提交期间状态已变化。");
		}
		if (std::isfinite(liveOwner->GetColumnHeaderHeight()))
			presenter->SetHeight(cui::layout::Length::Fixed(
				static_cast<float>(liveOwner->GetColumnHeaderHeight())));
		else presenter->SetHeight(cui::layout::Length::Auto());
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题高度提交期间状态已变化。");
		}

		const auto attach = [&](auto& staged,
			std::vector<DataGridColumnHeader*>& destination) -> bool
		{
			for (size_t slot = 0; slot < staged.size(); ++slot)
			{
				if (!staged[slot]) continue;
				const ControlWeakReference headerLifetime(staged[slot].get());
				auto* livePresenter = dynamic_cast<
					DataGridColumnHeadersPresenter*>(presenterLifetime.Get());
				auto* attachOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (wasSuperseded())
				{
					supersededByNestedInitialize = true;
					return false;
				}
				if (!livePresenter || !attachOwner
					|| attachOwner->_columnWidthProjectionRevision
						!= columnRevision) return false;
				auto* attached = livePresenter->AddOwned(
					std::move(staged[slot]));
				livePresenter = dynamic_cast<
					DataGridColumnHeadersPresenter*>(presenterLifetime.Get());
				auto* liveHeader = dynamic_cast<DataGridColumnHeader*>(
					headerLifetime.Get());
				attachOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (wasSuperseded())
				{
					supersededByNestedInitialize = true;
					return false;
				}
				if (!livePresenter || !attachOwner || !liveHeader
					|| attachOwner->_columnWidthProjectionRevision
						!= columnRevision
					|| attached != liveHeader
					|| !livePresenter->ContainsControl(liveHeader)) return false;
				destination[slot] = liveHeader;
			}
			return true;
		};
		if (!attach(stagedFrozen, nextFrozen)
			|| !attach(stagedHeaders, nextHeaders))
		{
			if (supersededByNestedInitialize || wasSuperseded()) return true;
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题视觉树挂载失败。");
		}
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题视觉树发布前状态已变化。");
		}
		struct PublishedHeaderSnapshot final
		{
			size_t Index = DataGridCellInfo::InvalidIndex;
			DataGridColumnHeader* Raw = nullptr;
			ControlWeakReference Lifetime;
			int LayoutColumn = 0;
		};
		std::vector<PublishedHeaderSnapshot> publishedHeaders;
		publishedHeaders.reserve(stagedHeaderLifetimes.size());
		const auto capturePublishedRange = [&](size_t rangeBegin,
			const std::vector<DataGridColumnHeader*>& values,
			int layoutBase)
		{
			for (size_t slot = 0; slot < values.size(); ++slot)
			{
				const size_t index = rangeBegin + slot;
				const auto input = std::find_if(inputSnapshots.begin(),
					inputSnapshots.end(), [index](const HeaderInputSnapshot& value)
						{ return value.Index == index; });
				if (input == inputSnapshots.end()) return false;
				auto* header = values[slot];
				if (input->ColumnVisibility != Visibility::Visible)
				{
					if (header) return false;
					continue;
				}
				const int layoutColumn =
					layoutBase + static_cast<int>(slot);
				if (!header || !presenter->ContainsControl(header)
					|| header->_owner != liveOwner
					|| header->_ownerLifetime.Get() != liveOwner
					|| header->_column != input->Column
					|| header->_columnIndex != index
					|| Grid::GetColumn(*header) != layoutColumn
					|| std::find_if(publishedHeaders.begin(),
						publishedHeaders.end(), [header](
							const PublishedHeaderSnapshot& value)
							{ return value.Raw == header; })
						!= publishedHeaders.end()) return false;
				publishedHeaders.push_back({ index, header,
					ControlWeakReference(header), layoutColumn });
			}
			return true;
		};
		if (nextStorageIsSparse)
		{
			if (!capturePublishedRange(0, nextFrozen, 0)
				|| !capturePublishedRange(nextBegin, nextHeaders,
					static_cast<int>(nextFrozenEnd + 1)))
			{
				leaveCommittedTreeEmpty();
				return fail(L"DataGrid 列标题发布快照无效。");
			}
		}
		else if (!capturePublishedRange(0, nextHeaders, 0))
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题发布快照无效。");
		}
		presenter->_frozenHeaders = std::move(nextFrozen);
		presenter->_headers = std::move(nextHeaders);
		presenter->_appliedColumnWidthProjectionRevision = 0;
		const bool widthsUpdated = presenter->UpdateColumnWidths(false);
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!widthsUpdated || !presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题宽度提交期间状态已变化。");
		}
		const bool offsetUpdated = presenter->UpdateHorizontalScrollOffset(
			liveOwner->_horizontalScrollOffset);
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (wasSuperseded()) return true;
		if (!offsetUpdated || !presenter || !liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题滚动位置提交期间状态已变化。");
		}
		if (!inputsStillCurrent(*liveOwner))
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题提交期间呈现输入已变化。");
		}
		const auto publishedTreeIsCurrent = [&]() noexcept
		{
			if (presenter->_owner != liveOwner
				|| presenter->_columnStorageIsSparse != nextStorageIsSparse
				|| presenter->_realizedFrozenColumnEnd != nextFrozenEnd
				|| presenter->_realizedColumnBegin != nextBegin
				|| presenter->_realizedColumnEnd != nextEnd
				|| presenter->_frozenHeaders.size() != nextFrozenEnd
				|| presenter->_headers.size() != (nextStorageIsSparse
					? nextEnd - nextBegin : columnCount)) return false;
			std::vector<DataGridColumnHeader*> expectedVisuals;
			expectedVisuals.reserve(publishedHeaders.size());
			const auto validateSlot = [&](size_t index,
				DataGridColumnHeader* raw, int layoutColumn) noexcept
			{
				const auto input = std::find_if(inputSnapshots.begin(),
					inputSnapshots.end(), [index](const HeaderInputSnapshot& value)
						{ return value.Index == index; });
				if (input == inputSnapshots.end()) return false;
				const auto published = std::find_if(publishedHeaders.begin(),
					publishedHeaders.end(), [=](
						const PublishedHeaderSnapshot& value)
						{ return value.Index == index && value.Raw == raw; });
				if (input->ColumnVisibility != Visibility::Visible)
					return !raw && published == publishedHeaders.end();
				if (!raw || published == publishedHeaders.end()
					|| published->LayoutColumn != layoutColumn) return false;
				auto* header = dynamic_cast<DataGridColumnHeader*>(
					published->Lifetime.Get());
				if (header != raw || !presenter->ContainsControl(header)
					|| presenter->GetHeader(index) != header
					|| header->_owner != liveOwner
					|| header->_ownerLifetime.Get() != liveOwner
					|| header->_column != input->Column
					|| header->_columnIndex != index
					|| Grid::GetColumn(*header) != layoutColumn
					|| header->GetContentTemplate() != input->ContentTemplate
					|| cui::framework::StyleAccess::ResourceKey(*header)
						!= input->Style
					|| !headerValuesEquivalent(
						header->GetContent(), input->Content)
					|| !header->ContentPresentationError().empty()) return false;
				expectedVisuals.push_back(header);
				return true;
			};
			for (size_t index = 0; index < presenter->_frozenHeaders.size();
				++index)
				if (!validateSlot(index, presenter->_frozenHeaders[index],
					static_cast<int>(index))) return false;
			for (size_t slot = 0; slot < presenter->_headers.size(); ++slot)
			{
				const size_t index = nextStorageIsSparse
					? nextBegin + slot : slot;
				const int layoutColumn = nextStorageIsSparse
					? static_cast<int>(nextFrozenEnd + 1 + slot)
					: static_cast<int>(slot);
				if (!validateSlot(index, presenter->_headers[slot],
					layoutColumn)) return false;
			}
			if (expectedVisuals.size() != publishedHeaders.size()
				|| presenter->VisualChildCount()
					!= static_cast<int>(expectedVisuals.size())) return false;
			for (int child = 0; child < presenter->VisualChildCount(); ++child)
			{
				auto* header = dynamic_cast<DataGridColumnHeader*>(
					presenter->GetVisualChild(child));
				if (!header || std::find(expectedVisuals.begin(),
					expectedVisuals.end(), header) == expectedVisuals.end())
					return false;
			}
			return true;
		};
		if (!publishedTreeIsCurrent())
		{
			leaveCommittedTreeEmpty();
			return fail(L"DataGrid 列标题最终树校验失败。");
		}
		presenter->_committedInitializeGeneration = generation;
		initializeCommitted = true;
		return true;
	}
	catch (...)
	{
		if (wasSuperseded()) return true;
		leaveCommittedTreeEmpty();
		return fail(L"DataGrid 列标题提交失败。");
	}
}

bool DataGridColumnHeadersPresenter::RefreshRealizedColumns(
	size_t frozenEnd, size_t begin, size_t end,
	std::wstring* outError)
{
	if (outError) outError->clear();
	auto* owner = _owner;
	if (!owner || !_columnStorageIsSparse || frozenEnd > begin
		|| begin > end || end > owner->ColumnCount())
	{
		if (outError) *outError =
			L"DataGrid 横向虚拟列标题范围无效。";
		return false;
	}
	if (frozenEnd == _realizedFrozenColumnEnd
		&& begin == _realizedColumnBegin && end == _realizedColumnEnd)
	{
		if (UpdateColumnWidths()) return true;
		if (outError) *outError =
			L"DataGrid 横向虚拟列标题宽度更新失败。";
		return false;
	}

	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	auto generation = ++_initializeGeneration;
	if (generation == 0) generation = ++_initializeGeneration;
	const auto committedAtEntry = _committedInitializeGeneration;
	const auto wasSuperseded = [&]() noexcept
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		return presenter && ownerLifetime.Get()
			&& presenter->_committedInitializeGeneration != committedAtEntry
			&& presenter->_committedInitializeGeneration != generation;
	};
	const auto fail = [&](std::wstring message)
	{
		if (outError && outError->empty()) *outError = std::move(message);
		return false;
	};
	const size_t columnCount = owner->ColumnCount();
	const size_t columnRevision = owner->_columnWidthProjectionRevision;
	const size_t oldFrozenEnd = _realizedFrozenColumnEnd;
	const size_t oldBegin = _realizedColumnBegin;
	const size_t oldEnd = _realizedColumnEnd;
	const auto oldFrozenHeaders = _frozenHeaders;
	const auto oldHeaders = _headers;

	struct OldHeader final
	{
		size_t Index = DataGridCellInfo::InvalidIndex;
		ControlWeakReference Lifetime;
	};
	std::vector<OldHeader> oldHeaderLifetimes;
	oldHeaderLifetimes.reserve(oldFrozenHeaders.size() + oldHeaders.size());
	std::vector<DataGridColumnHeader*> uniqueOldHeaders;
	uniqueOldHeaders.reserve(oldHeaderLifetimes.capacity());
	const auto captureOld = [&](size_t index, DataGridColumnHeader* header)
	{
		if (!header) return true;
		if (std::find(uniqueOldHeaders.begin(), uniqueOldHeaders.end(), header)
			!= uniqueOldHeaders.end() || !ContainsControl(header)
			|| header->_owner != owner || header->_ownerLifetime.Get() != owner
			|| header->_columnIndex != index
			|| header->_column != owner->GetColumnFromDisplayIndex(index))
			return false;
		uniqueOldHeaders.push_back(header);
		oldHeaderLifetimes.push_back({ index, ControlWeakReference(header) });
		return true;
	};
	for (size_t index = 0; index < oldFrozenHeaders.size(); ++index)
		if (!captureOld(index, oldFrozenHeaders[index]))
			return fail(L"DataGrid 旧冻结列标题树无效。");
	for (size_t slot = 0; slot < oldHeaders.size(); ++slot)
		if (!captureOld(oldBegin + slot, oldHeaders[slot]))
			return fail(L"DataGrid 旧滚动列标题树无效。");
	if (VisualChildCount() != static_cast<int>(uniqueOldHeaders.size()))
		return fail(L"DataGrid 旧列标题视觉树与索引不一致。");
	for (int index = 0; index < VisualChildCount(); ++index)
		if (auto* header = dynamic_cast<DataGridColumnHeader*>(
			GetVisualChild(index)); !header
			|| std::find(uniqueOldHeaders.begin(), uniqueOldHeaders.end(), header)
				== uniqueOldHeaders.end())
			return fail(L"DataGrid 旧列标题视觉树包含未索引项。");

	const auto resolve = [&]() noexcept -> std::pair<
		DataGridColumnHeadersPresenter*, DataGrid*>
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!presenter || !liveOwner || presenter->_owner != liveOwner
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != columnRevision)
			return {};
		return { presenter, liveOwner };
	};
	const auto retireHeader = [](DataGridColumnHeader* header) noexcept
	{
		if (!header) return;
		header->_owner = nullptr;
		header->_ownerLifetime = nullptr;
		header->_column = nullptr;
		header->_columnIndex = DataGridCellInfo::InvalidIndex;
		header->_multiColumnSortRequested = false;
		header->_isResizing = false;
		header->_resizeFromLeftEdge = false;
		header->_resizingColumnIndex = DataGridCellInfo::InvalidIndex;
		header->_isReorderPending = false;
		header->_isReordering = false;
	};
	std::vector<ControlWeakReference> createdLifetimes;
	createdLifetimes.reserve(frozenEnd + end - begin);
	const auto retireToEmpty = [&]() noexcept
	{
		if (wasSuperseded()) return;
		// A candidate may have been moved to another visual parent by an
		// AddOwned/Grid attached-property observer before it was published in a
		// sidecar. Retire those weakly tracked candidates before clearing the
		// presenter's own visual tree so callbacks cannot observe an active ghost.
		for (const auto& lifetime : createdLifetimes)
			retireHeader(dynamic_cast<DataGridColumnHeader*>(lifetime.Get()));
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!presenter || !liveOwner
			|| (presenter->_owner && presenter->_owner != liveOwner)) return;
		liveOwner->RetireColumnHeadersPresenterNoCallbacks(
			presenter, generation);
		try { presenter->ClearVisualChildren(); }
		catch (...) {}
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!presenter || wasSuperseded()) return;
		try { presenter->ClearColumns(); }
		catch (...) {}
	};

	std::vector<DataGridColumnHeader*> nextFrozen(frozenEnd, nullptr);
	std::vector<DataGridColumnHeader*> nextHeaders(end - begin, nullptr);
	std::vector<std::unique_ptr<DataGridColumnHeader>> createdFrozen(frozenEnd);
	std::vector<std::unique_ptr<DataGridColumnHeader>> createdHeaders(
		end - begin);
	bool transactionCommitted = false;
	const auto retireCreatedUnlessNestedTreeAdopted = [&]() noexcept
	{
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* transactionPresenter = dynamic_cast<
			DataGridColumnHeadersPresenter*>(presenterLifetime.Get());
		for (const auto& lifetime : createdLifetimes)
		{
			auto* header = dynamic_cast<DataGridColumnHeader*>(lifetime.Get());
			if (!header) continue;
			DataGridColumnHeadersPresenter* adoptingPresenter = nullptr;
			if (liveOwner)
				adoptingPresenter = liveOwner->GetColumnHeadersPresenter();
			if (!adoptingPresenter && transactionPresenter
				&& transactionPresenter->_owner == liveOwner)
				adoptingPresenter = transactionPresenter;
			const bool nestedGenerationOwnsTree = adoptingPresenter
				&& (adoptingPresenter != transactionPresenter
					|| (adoptingPresenter->_committedInitializeGeneration
						!= committedAtEntry
						&& adoptingPresenter->_committedInitializeGeneration
						!= generation));
			const size_t index = header->_columnIndex;
			const bool adopted = nestedGenerationOwnsTree && liveOwner
				&& index < liveOwner->ColumnCount()
				&& header->_owner == liveOwner
				&& header->_ownerLifetime.Get() == liveOwner
				&& header->_column
					== liveOwner->GetColumnFromDisplayIndex(index)
				&& adoptingPresenter->GetHeader(index) == header
				&& adoptingPresenter->ContainsControl(header);
			if (!adopted) retireHeader(header);
		}
	};
	auto retireCreatedOnExit = MakeScopeExit([&]() noexcept
	{
		if (!transactionCommitted)
			retireCreatedUnlessNestedTreeAdopted();
	});
	const auto oldAt = [&](size_t index) -> DataGridColumnHeader*
	{
		if (index < oldFrozenEnd)
			return index < oldFrozenHeaders.size()
				? oldFrozenHeaders[index] : nullptr;
		return index >= oldBegin && index < oldEnd
			&& index - oldBegin < oldHeaders.size()
			? oldHeaders[index - oldBegin] : nullptr;
	};
	const auto nextAt = [&](size_t index) -> DataGridColumnHeader*
	{
		if (index < frozenEnd)
			return index < nextFrozen.size() ? nextFrozen[index] : nullptr;
		return index >= begin && index < end
			&& index - begin < nextHeaders.size()
			? nextHeaders[index - begin] : nullptr;
	};
	bool nestedCommit = false;
	const auto stageRange = [&](size_t rangeBegin, size_t rangeEnd,
		std::vector<DataGridColumnHeader*>& destination,
		std::vector<std::unique_ptr<DataGridColumnHeader>>& creations)
	{
		for (size_t index = rangeBegin; index < rangeEnd; ++index)
		{
			if (wasSuperseded())
			{
				nestedCommit = true;
				return false;
			}
			auto [presenter, liveOwner] = resolve();
			if (!presenter || !liveOwner) return false;
			auto* column = liveOwner->GetColumnFromDisplayIndex(index);
			if (!column) return false;
			const size_t slot = index - rangeBegin;
			if (column->GetVisibility() != Visibility::Visible) continue;
			if (auto* retained = oldAt(index))
			{
				auto found = std::find_if(oldHeaderLifetimes.begin(),
					oldHeaderLifetimes.end(), [index](const OldHeader& entry)
						{ return entry.Index == index; });
				auto* liveRetained = found == oldHeaderLifetimes.end()
					? nullptr : dynamic_cast<DataGridColumnHeader*>(
						found->Lifetime.Get());
				if (liveRetained != retained
					|| !presenter->ContainsControl(liveRetained)
					|| liveRetained->_owner != liveOwner
					|| liveRetained->_column != column
					|| liveRetained->_columnIndex != index) return false;
				destination[slot] = liveRetained;
				continue;
			}
			auto header = std::make_unique<DataGridColumnHeader>();
			createdLifetimes.emplace_back(header.get());
			if (!header->Initialize(
				*liveOwner, *column, index, outError)) return false;
			if (wasSuperseded())
			{
				nestedCommit = true;
				return false;
			}
			std::tie(presenter, liveOwner) = resolve();
			if (!presenter || !liveOwner
				|| liveOwner->GetColumnFromDisplayIndex(index) != column)
				return false;
			destination[slot] = header.get();
			creations[slot] = std::move(header);
		}
		return true;
	};
	try
	{
		if (!stageRange(0, frozenEnd, nextFrozen, createdFrozen)
			|| !stageRange(begin, end, nextHeaders, createdHeaders))
		{
			auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!nestedCommit && liveOwner
				&& (liveOwner->ColumnCount() != columnCount
					|| liveOwner->_columnWidthProjectionRevision != columnRevision))
				retireToEmpty();
			return nestedCommit ? true
				: fail(L"DataGrid 横向虚拟列标题生成失败。");
		}

		const auto attachCreated = [&](auto& creations,
			std::vector<DataGridColumnHeader*>& destination)
		{
			for (size_t slot = 0; slot < creations.size(); ++slot)
			{
				if (!creations[slot]) continue;
				if (wasSuperseded())
				{
					nestedCommit = true;
					return false;
				}
				auto [presenter, liveOwner] = resolve();
				if (!presenter || !liveOwner) return false;
				const ControlWeakReference headerLifetime(
					creations[slot].get());
				auto* attached = presenter->AddOwned(
					std::move(creations[slot]));
				if (wasSuperseded())
				{
					nestedCommit = true;
					return false;
				}
				std::tie(presenter, liveOwner) = resolve();
				auto* liveHeader = dynamic_cast<DataGridColumnHeader*>(
					headerLifetime.Get());
				if (!presenter || !liveOwner || !liveHeader
					|| attached != liveHeader
					|| !presenter->ContainsControl(liveHeader)
					|| liveHeader->_owner != liveOwner
					|| liveHeader->_columnIndex >= columnCount
					|| liveOwner->GetColumnFromDisplayIndex(
						liveHeader->_columnIndex) != liveHeader->_column)
					return false;
				destination[slot] = liveHeader;
			}
			return true;
		};
		if (!attachCreated(createdFrozen, nextFrozen)
			|| !attachCreated(createdHeaders, nextHeaders))
		{
			if (nestedCommit || wasSuperseded()) return true;
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题挂载失败。");
		}

		for (const auto& entry : oldHeaderLifetimes)
		{
			auto* header = dynamic_cast<DataGridColumnHeader*>(
				entry.Lifetime.Get());
			if (nextAt(entry.Index) == header) continue;
			if (wasSuperseded()) return true;
			auto [presenter, liveOwner] = resolve();
			if (!presenter || !liveOwner || !header
				|| !presenter->ContainsControl(header))
			{
				retireToEmpty();
				return fail(L"DataGrid 旧虚拟列标题移除前已失效。");
			}
			retireHeader(header);
			if (!presenter->DeleteVisualChild(header))
			{
				if (wasSuperseded()) return true;
				retireToEmpty();
				return fail(L"DataGrid 旧虚拟列标题移除失败。");
			}
			if (wasSuperseded()) return true;
			if (!resolve().first)
			{
				retireToEmpty();
				return fail(L"DataGrid 旧虚拟列标题移除期间状态变化。");
			}
		}

		const auto headerValuesEquivalent = [](const BindingValue& left,
			const BindingValue& right)
		{
			if (BindingValuesEqual(left, right)) return true;
			BindingSourceReference leftSource;
			BindingSourceReference rightSource;
			if (left.TryGet(leftSource) && right.TryGet(rightSource))
				return leftSource == rightSource;
			return left.Kind() == BindingValueKind::Object
				&& right.Kind() == BindingValueKind::Object
				&& left.Type() == right.Type();
		};
		struct PublishedHeader final
		{
			size_t Index = DataGridCellInfo::InvalidIndex;
			DataGridColumnHeader* Raw = nullptr;
			ControlWeakReference Lifetime;
			DataGridColumn* Column = nullptr;
			std::uint32_t ColumnIdentity = 0;
			int LayoutColumn = 0;
		};
		std::vector<PublishedHeader> publishedHeaders;
		publishedHeaders.reserve(frozenEnd + end - begin);
		auto publishRange = [&](size_t rangeBegin,
			const std::vector<DataGridColumnHeader*>& values,
			int layoutBase)
		{
			for (size_t slot = 0; slot < values.size(); ++slot)
			{
				const size_t index = rangeBegin + slot;
				auto [presenter, liveOwner] = resolve();
				if (!presenter || !liveOwner) return false;
				auto* column = liveOwner->GetColumnFromDisplayIndex(index);
				auto* header = values[slot];
				if (!column) return false;
				if (column->GetVisibility() != Visibility::Visible)
				{
					if (header) return false;
					continue;
				}
				if (!header || !presenter->ContainsControl(header)
					|| header->_owner != liveOwner
					|| header->_column != column
					|| header->_columnIndex != index
					|| header->GetContentTemplate()
						!= column->GetHeaderTemplate()
					|| cui::framework::StyleAccess::ResourceKey(*header)
						!= liveOwner->EffectiveColumnHeaderStyle(*column)
					|| !headerValuesEquivalent(
						header->GetContent(), column->GetHeader())) return false;
				if (std::find_if(publishedHeaders.begin(),
					publishedHeaders.end(), [header](const PublishedHeader& value)
						{ return value.Raw == header; }) != publishedHeaders.end())
					return false;
				const ControlWeakReference headerLifetime(header);
				const int layoutColumn =
					layoutBase + static_cast<int>(slot);
				publishedHeaders.push_back({ index, header, headerLifetime,
					column, column->_accessibilityIdentity, layoutColumn });
				Grid::SetColumn(*header,
					layoutColumn);
				if (wasSuperseded())
				{
					nestedCommit = true;
					return false;
				}
				std::tie(presenter, liveOwner) = resolve();
				if (!presenter || !liveOwner
					|| headerLifetime.Get() != header
					|| !presenter->ContainsControl(header)) return false;
			}
			return true;
		};
		if (!publishRange(0, nextFrozen, 0)
			|| !publishRange(begin, nextHeaders,
				static_cast<int>(frozenEnd + 1)))
		{
			if (nestedCommit || wasSuperseded()) return true;
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题布局发布失败。");
		}

		auto [presenter, liveOwner] = resolve();
		if (!presenter || !liveOwner)
		{
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题发布前状态变化。");
		}
		presenter->_frozenHeaders = std::move(nextFrozen);
		presenter->_headers = std::move(nextHeaders);
		presenter->_realizedFrozenColumnEnd = frozenEnd;
		presenter->_realizedColumnBegin = begin;
		presenter->_realizedColumnEnd = end;
		presenter->_appliedColumnWidthProjectionRevision = 0;
		const bool widthsUpdated = presenter->UpdateColumnWidths();
		if (wasSuperseded()) return true;
		std::tie(presenter, liveOwner) = resolve();
		if (!widthsUpdated || !presenter || !liveOwner)
		{
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题宽度提交失败。");
		}
		const bool offsetUpdated = presenter->UpdateHorizontalScrollOffset(
			liveOwner->_horizontalScrollOffset);
		if (wasSuperseded()) return true;
		std::tie(presenter, liveOwner) = resolve();
		if (!offsetUpdated || !presenter || !liveOwner)
		{
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题滚动提交失败。");
		}
		const auto publishedTreeIsCurrent = [&]() noexcept
		{
			if (presenter->_owner != liveOwner
				|| !presenter->_columnStorageIsSparse
				|| presenter->_realizedFrozenColumnEnd != frozenEnd
				|| presenter->_realizedColumnBegin != begin
				|| presenter->_realizedColumnEnd != end
				|| presenter->_frozenHeaders.size() != frozenEnd
				|| presenter->_headers.size() != end - begin)
				return false;
			std::vector<DataGridColumnHeader*> expectedVisuals;
			expectedVisuals.reserve(publishedHeaders.size());
			const auto validateSlot = [&](size_t index,
				DataGridColumnHeader* raw, int layoutColumn) noexcept
			{
				auto* column = liveOwner->GetColumnFromDisplayIndex(index);
				if (!column) return false;
				const auto found = std::find_if(publishedHeaders.begin(),
					publishedHeaders.end(), [=](const PublishedHeader& value)
						{ return value.Index == index && value.Raw == raw; });
				if (column->GetVisibility() != Visibility::Visible)
					return !raw && found == publishedHeaders.end();
				if (!raw || found == publishedHeaders.end()
					|| found->Column != column
					|| column->_accessibilityIdentity != found->ColumnIdentity
					|| found->LayoutColumn != layoutColumn) return false;
				auto* header = dynamic_cast<DataGridColumnHeader*>(
					found->Lifetime.Get());
				if (header != raw || !presenter->ContainsControl(header)
					|| presenter->GetHeader(index) != header
					|| header->_owner != liveOwner
					|| header->_ownerLifetime.Get() != liveOwner
					|| header->_column != column
					|| header->_columnIndex != index
					|| Grid::GetColumn(*header) != layoutColumn
					|| header->GetContentTemplate()
						!= column->GetHeaderTemplate()
					|| cui::framework::StyleAccess::ResourceKey(*header)
						!= liveOwner->EffectiveColumnHeaderStyle(*column)
					|| !headerValuesEquivalent(
						header->GetContent(), column->GetHeader())
					|| !header->ContentPresentationError().empty()) return false;
				expectedVisuals.push_back(header);
				return true;
			};
			for (size_t index = 0; index < frozenEnd; ++index)
				if (!validateSlot(index, presenter->_frozenHeaders[index],
					static_cast<int>(index))) return false;
			for (size_t slot = 0; slot < presenter->_headers.size(); ++slot)
				if (!validateSlot(begin + slot, presenter->_headers[slot],
					static_cast<int>(frozenEnd + 1 + slot))) return false;
			if (expectedVisuals.size() != publishedHeaders.size()
				|| presenter->VisualChildCount()
					!= static_cast<int>(expectedVisuals.size())) return false;
			for (int child = 0; child < presenter->VisualChildCount(); ++child)
			{
				auto* header = dynamic_cast<DataGridColumnHeader*>(
					presenter->GetVisualChild(child));
				if (!header || std::find(expectedVisuals.begin(),
					expectedVisuals.end(), header) == expectedVisuals.end())
					return false;
			}
			return true;
		};
		if (!publishedTreeIsCurrent())
		{
			retireToEmpty();
			return fail(L"DataGrid 横向虚拟列标题最终树校验失败。");
		}
		presenter->_committedInitializeGeneration = generation;
		transactionCommitted = true;
		return true;
	}
	catch (...)
	{
		if (wasSuperseded()) return true;
		retireToEmpty();
		return fail(L"DataGrid 横向虚拟列标题更新失败。");
	}
}

bool DataGridColumnHeadersPresenter::UpdateColumnWidths(
	bool propagateLayoutInvalidation)
{
	if (!_owner) return false;
	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(_owner);
	const size_t projectionRevision =
		_owner->_columnWidthProjectionRevision;
	if (_appliedColumnWidthProjectionRevision == projectionRevision)
		return true;
	const size_t columnCount = _owner->ColumnCount();
	const size_t frozenEnd = _columnStorageIsSparse
		? _realizedFrozenColumnEnd : 0;
	const size_t begin = _columnStorageIsSparse
		? _realizedColumnBegin : 0;
	const size_t end = _columnStorageIsSparse
		? _realizedColumnEnd : columnCount;
	if (frozenEnd > begin || begin > end || end > columnCount) return false;
	const auto initializeGeneration = _initializeGeneration;
	const auto committedInitializeGeneration =
		_committedInitializeGeneration;
	const auto presentationRefreshGeneration =
		_presentationRefreshGeneration;
	const bool storageIsSparse = _columnStorageIsSparse;
	const size_t frozenSlotCount = _frozenHeaders.size();
	const size_t scrollingSlotCount = _headers.size();
	std::vector<std::pair<DataGridColumn*, std::uint32_t>> columns;
	columns.reserve(columnCount);
	for (size_t index = 0; index < columnCount; ++index)
	{
		auto* column = _owner->GetColumnFromDisplayIndex(index);
		if (!column) return false;
		columns.emplace_back(column, column->_accessibilityIdentity);
	}
	const auto resolveOwner = [&]() noexcept -> DataGrid*
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!presenter || !owner || presenter->_owner != owner
			|| owner->ColumnCount() != columnCount
			|| owner->_columnWidthProjectionRevision != projectionRevision
			|| presenter->_initializeGeneration != initializeGeneration
			|| presenter->_committedInitializeGeneration
				!= committedInitializeGeneration
			|| presenter->_presentationRefreshGeneration
				!= presentationRefreshGeneration
			|| presenter->_columnStorageIsSparse != storageIsSparse
			|| presenter->_realizedFrozenColumnEnd
				!= (storageIsSparse ? frozenEnd : 0)
			|| presenter->_realizedColumnBegin != begin
			|| presenter->_realizedColumnEnd != end
			|| presenter->_frozenHeaders.size() != frozenSlotCount
			|| presenter->_headers.size() != scrollingSlotCount)
			return nullptr;
		for (size_t index = 0; index < columnCount; ++index)
		{
			auto* column = owner->GetColumnFromDisplayIndex(index);
			if (!column || column != columns[index].first
				|| column->_accessibilityIdentity != columns[index].second)
				return nullptr;
		}
		return owner;
	};
	std::vector<ColumnDefinition> definitions;
	definitions.reserve((end - begin) + frozenEnd
		+ (_columnStorageIsSparse ? 2 : 0));
	const auto appendColumn = [&](size_t index) -> bool
	{
		auto* owner = resolveOwner();
		if (!owner || index >= columns.size()) return false;
		auto* column = owner->GetColumnFromDisplayIndex(index);
		if (!column || column != columns[index].first) return false;
		if (column->GetVisibility() != Visibility::Visible)
		{
			definitions.emplace_back(GridLength::Pixels(0.0f));
			return true;
		}
		const double minimum = column->GetLayoutMinWidth();
		const double maximum = column->GetLayoutMaxWidth();
		const auto length = owner->ResolveColumnGridLength(index);
		owner = resolveOwner();
		if (!owner || owner->GetColumnFromDisplayIndex(index)
			!= columns[index].first) return false;
		definitions.emplace_back(length, static_cast<float>(minimum),
			static_cast<float>((std::min)(maximum,
				static_cast<double>((std::numeric_limits<float>::max)()))));
		return true;
	};
	if (_columnStorageIsSparse)
	{
		auto* owner = resolveOwner();
		if (!owner || !owner->EnsureColumnWidthPrefix()) return false;
		owner = resolveOwner();
		if (!owner || owner->_columnWidthPrefix.size() != columnCount + 1)
			return false;
		for (size_t index = 0; index < frozenEnd; ++index)
			if (!appendColumn(index)) return false;
		owner = resolveOwner();
		if (!owner) return false;
		definitions.emplace_back(SparseTrackLength((std::max)(0.0,
			owner->_columnWidthPrefix[begin]
				- owner->_columnWidthPrefix[frozenEnd])));
		for (size_t index = begin; index < end; ++index)
			if (!appendColumn(index)) return false;
		owner = resolveOwner();
		if (!owner) return false;
		definitions.emplace_back(SparseTrackLength((std::max)(0.0,
			owner->_columnWidthPrefix[columnCount]
				- owner->_columnWidthPrefix[end])));
	}
	else
		for (size_t index = 0; index < columnCount; ++index)
			if (!appendColumn(index)) return false;
	if (!resolveOwner()) return false;
	ReplaceColumns(std::move(definitions), propagateLayoutInvalidation);
	auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	auto* owner = resolveOwner();
	if (presenter && owner && presenter->_owner == owner
		&& owner->_columnWidthProjectionRevision == projectionRevision)
	{
		if (!presenter->UpdateHorizontalScrollOffset(
			owner->_horizontalScrollOffset)) return false;
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		owner = resolveOwner();
		if (!presenter || !owner) return false;
		presenter->_appliedColumnWidthProjectionRevision = projectionRevision;
		return true;
	}
	return false;
}

bool DataGridColumnHeadersPresenter::UpdateHorizontalScrollOffset(double offset)
{
	auto* owner = _owner;
	if (!owner) return false;
	if (!std::isfinite(offset)) offset = 0.0;
	const ControlWeakReference presenterLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	auto* const registeredPresenter = owner->GetColumnHeadersPresenter();
	// Programmatic presenters may be initialized before becoming the active
	// template part, but an owner must never have a different active presenter
	// while this one is applying visual state.
	if (registeredPresenter && registeredPresenter != this) return false;
	const size_t columnCount = owner->ColumnCount();
	const size_t projectionRevision = owner->_columnWidthProjectionRevision;
	const int frozenColumnCount = owner->GetFrozenColumnCount();
	const double ownerOffset = owner->_horizontalScrollOffset;
	const auto initializeGeneration = _initializeGeneration;
	const auto committedInitializeGeneration =
		_committedInitializeGeneration;
	const auto presentationRefreshGeneration =
		_presentationRefreshGeneration;
	const bool storageIsSparse = _columnStorageIsSparse;
	const size_t realizedFrozenEnd = _realizedFrozenColumnEnd;
	const size_t realizedBegin = _realizedColumnBegin;
	const size_t realizedEnd = _realizedColumnEnd;
	const size_t frozenSlotCount = _frozenHeaders.size();
	const size_t scrollingSlotCount = _headers.size();
	const auto resolveOwner = [&]() noexcept -> DataGrid*
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!presenter || !liveOwner || presenter->_owner != liveOwner
			|| liveOwner->GetColumnHeadersPresenter() != registeredPresenter
			|| liveOwner->ColumnCount() != columnCount
			|| liveOwner->_columnWidthProjectionRevision != projectionRevision
			|| liveOwner->GetFrozenColumnCount() != frozenColumnCount
			|| liveOwner->_horizontalScrollOffset != ownerOffset
			|| presenter->_initializeGeneration != initializeGeneration
			|| presenter->_committedInitializeGeneration
				!= committedInitializeGeneration
			|| presenter->_presentationRefreshGeneration
				!= presentationRefreshGeneration
			|| presenter->_columnStorageIsSparse != storageIsSparse
			|| presenter->_realizedFrozenColumnEnd != realizedFrozenEnd
			|| presenter->_realizedColumnBegin != realizedBegin
			|| presenter->_realizedColumnEnd != realizedEnd
			|| presenter->_frozenHeaders.size() != frozenSlotCount
			|| presenter->_headers.size() != scrollingSlotCount)
			return nullptr;
		return liveOwner;
	};
	if (!resolveOwner()) return false;

	struct HeaderSnapshot final
	{
		ControlWeakReference Lifetime;
		size_t Index = DataGridCellInfo::InvalidIndex;
		DataGridColumn* Column = nullptr;
		std::uint32_t ColumnIdentity = 0;
	};
	std::vector<HeaderSnapshot> headers;
	headers.reserve(frozenSlotCount + scrollingSlotCount);
	const auto snapshotHeader = [&](size_t index,
		DataGridColumnHeader* header) -> bool
	{
		if (!header) return true;
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = resolveOwner();
		if (!presenter || !liveOwner || index >= columnCount
			|| presenter->GetHeader(index) != header
			|| !presenter->ContainsControl(header)
			|| header->_owner != liveOwner
			|| header->_ownerLifetime.Get() != liveOwner
			|| header->_columnIndex != index) return false;
		auto* column = liveOwner->GetColumnFromDisplayIndex(index);
		if (!column || header->_column != column) return false;
		for (const auto& existing : headers)
			if (existing.Lifetime.Get() == header) return false;
		headers.push_back({ ControlWeakReference(header), index, column,
			column->_accessibilityIdentity });
		return true;
	};
	for (size_t slot = 0; slot < frozenSlotCount; ++slot)
		if (!snapshotHeader(slot, _frozenHeaders[slot])) return false;
	for (size_t slot = 0; slot < scrollingSlotCount; ++slot)
	{
		const size_t index = storageIsSparse ? realizedBegin + slot : slot;
		if (!snapshotHeader(index, _headers[slot])) return false;
	}
	const auto resolveHeader = [&](const HeaderSnapshot& snapshot) noexcept
		-> DataGridColumnHeader*
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		auto* liveOwner = resolveOwner();
		auto* header = dynamic_cast<DataGridColumnHeader*>(
			snapshot.Lifetime.Get());
		if (!presenter || !liveOwner || !header
			|| snapshot.Index >= columnCount
			|| presenter->GetHeader(snapshot.Index) != header
			|| !presenter->ContainsControl(header)
			|| header->_owner != liveOwner
			|| header->_ownerLifetime.Get() != liveOwner
			|| header->_columnIndex != snapshot.Index
			|| header->_column != snapshot.Column) return nullptr;
		auto* column = liveOwner->GetColumnFromDisplayIndex(snapshot.Index);
		if (!column || column != snapshot.Column
			|| column->_accessibilityIdentity != snapshot.ColumnIdentity)
			return nullptr;
		return header;
	};

	const size_t frozenEnd = (std::min)(columnCount,
		static_cast<size_t>((std::max)(0, frozenColumnCount)));
	double frozenWidth = 0.0;
	if (frozenEnd > 0)
	{
		double ignored = 0.0;
		auto* liveOwner = resolveOwner();
		if (!liveOwner || !liveOwner->TryResolveColumnBounds(
			frozenEnd - 1, ignored, frozenWidth)
			|| !resolveOwner()) return false;
	}
	const auto apply = [&](const HeaderSnapshot& snapshot)
	{
		if (!resolveHeader(snapshot)) return false;
		double left = 0.0;
		double right = 0.0;
		auto* liveOwner = resolveOwner();
		if (!liveOwner || !liveOwner->TryResolveColumnBounds(
			snapshot.Index, left, right)
			|| !resolveHeader(snapshot)) return false;
		const auto mutate = [&](auto&& callback)
		{
			auto* header = resolveHeader(snapshot);
			if (!header) return false;
			callback(*header);
			return resolveHeader(snapshot) != nullptr;
		};
		if (snapshot.Index < frozenEnd)
		{
			if (!mutate([](DataGridColumnHeader& header)
				{ header.SetZIndex(1); })
				|| !mutate([](DataGridColumnHeader& header)
					{ header.ClearClip(); })
				|| !mutate([&](DataGridColumnHeader& header)
					{ header.SetRenderTransform(
						HorizontalTranslation(offset)); })) return false;
			return true;
		}
		if (!mutate([](DataGridColumnHeader& header)
			{ header.SetZIndex(0); })
			|| !mutate([](DataGridColumnHeader& header)
				{ header.ClearRenderTransform(); })) return false;
		const double width = (std::max)(0.0, right - left);
		const double hiddenWidth = frozenWidth + offset - left;
		if (frozenWidth <= 0.0001 || hiddenWidth <= 0.0001)
			return mutate([](DataGridColumnHeader& header)
				{ header.ClearClip(); });
		cui::drawing::Geometry clip;
		clip.Kind = cui::drawing::GeometryKind::Rectangle;
		if (hiddenWidth >= width - 0.0001)
			clip.Rect = D2D1::RectF();
		else
		{
			constexpr float verticalLimit = 1000000.0f;
			clip.Rect = D2D1::RectF(
				static_cast<float>((std::clamp)(hiddenWidth, 0.0, width)),
				-verticalLimit,
				static_cast<float>(width), verticalLimit);
		}
		if (!mutate([&](DataGridColumnHeader& header)
			{ header.SetClip(clip); })) return false;
		return true;
	};
	for (const auto& header : headers)
		if (!apply(header)) return false;
	return resolveOwner() != nullptr;
}

bool DataGridColumnHeadersPresenter::TryCommitResizeLayoutLocally(
	bool heightIsFixed)
{
	if (!_owner || !GetComputedLayout().hasArranged) return false;
	if (heightIsFixed)
	{
		UpdateLayout();
		return true;
	}
	const float previousDesiredHeight = GetDesiredSizeDip().height;
	const auto size = GetActualSizeDip();
	const auto desired = Measure(cui::core::Constraints{
		cui::core::Size{ size.width, cui::core::Infinity } });
	if (std::abs(desired.height - previousDesiredHeight) > 0.0001f)
		return false;
	const auto location = GetActualLocationDip();
	Arrange(cui::core::Rect{
		location.x, location.y, size.width, size.height });
	return true;
}

cui::core::Size DataGridColumnHeadersPresenter::MeasureCore(
	const cui::core::Constraints& available)
{
	bool hasPublishedScrollViewport = false;
	if (_owner)
		if (auto* scroll = dynamic_cast<ScrollViewer*>(
			_owner->_scrollViewer.Get()))
			hasPublishedScrollViewport = std::isfinite(
				scroll->GetViewportWidth()) && scroll->GetViewportWidth() > 0.0;
	// The bounded header viewport is useful as a first-layout/detached fallback.
	// Once the body ScrollViewer has published its viewport it is authoritative:
	// it excludes the vertical scrollbar, whereas the header host does not.
	// Alternating between those two widths would request layout forever.
	if (_owner && !hasPublishedScrollViewport
		&& std::isfinite(available.maximum.width))
		_owner->UpdateColumnViewportWidth(available.maximum.width);
	return Grid::MeasureCore(available);
}

void DataGridColumnHeadersPresenter::Arrange(cui::core::Rect finalRect)
{
	Grid::Arrange(finalRect);
	ArrangeColumnHeaderDragIndicators();
}

const DependencyProperty& DataGrid::AutoGenerateColumnsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Changed = [](DataGrid& target, const bool&, const bool& value)
		{
			const ControlWeakReference lifetime(&target);
			if (value) target.EnsureAutoGeneratedColumns();
			else target.RemoveAutoGeneratedColumns();
			if (auto* live = dynamic_cast<DataGrid*>(lifetime.Get()))
			{
				live->RefreshColumns();
				live = dynamic_cast<DataGrid*>(lifetime.Get());
				if (live) live->FlushAutoGeneratedColumnsEvent();
			}
		};
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"AutoGenerateColumns"),
			[](DataGrid& target) { return target._autoGenerateColumns; },
			[](DataGrid& target, const bool& value)
			{ target._autoGenerateColumns = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CurrentItemProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(BindingValue{});
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 30;
		options.Design.Editor = DependencyPropertyEditorKind::Auto;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, BindingValue>(
			DependencyPropertyRegistrationLiteral(L"CurrentItem"),
			[](DataGrid& target) { return target.GetCurrentItem(); },
			[](DataGrid& target, const BindingValue& value)
			{ target.SetCurrentItem(value); },
			[](DataGrid& target,
				DependencyPropertyMetadata::ChangeHandler handler,
				DataSourceUpdateMode)
			{
				return target._currentItemChanged.Subscribe(
					[handler = std::move(handler)](DataGrid*) { handler(); });
			}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CurrentColumnProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(static_cast<DataGridColumn*>(nullptr));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 40;
		options.Design.Editor = DependencyPropertyEditorKind::Auto;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, DataGridColumn*>(
				DependencyPropertyRegistrationLiteral(L"CurrentColumn"),
				[](DataGrid& target) { return target.GetCurrentColumn(); },
				[](DataGrid& target, DataGridColumn* const& value)
				{ target.SetCurrentColumn(value); },
				[](DataGrid& target,
					DependencyPropertyMetadata::ChangeHandler handler,
					DataSourceUpdateMode)
				{
					return target._currentColumnChanged.Subscribe(
						[handler = std::move(handler)](DataGrid*) { handler(); });
				}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CurrentCellProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(DataGridCellInfo{});
		options.Equals = [](const DataGridCellInfo& left,
			const DataGridCellInfo& right)
		{
			return left == right
				&& left.RowIndex == right.RowIndex
				&& left.ColumnIndex == right.ColumnIndex;
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 50;
		options.Design.Editor = DependencyPropertyEditorKind::Auto;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, DataGridCellInfo>(
				DependencyPropertyRegistrationLiteral(L"CurrentCell"),
				[](DataGrid& target) { return target.GetCurrentCell(); },
				[](DataGrid& target, const DataGridCellInfo& value)
				{ (void)target.SetCurrentCell(value); },
				[](DataGrid& target,
					DependencyPropertyMetadata::ChangeHandler handler,
					DataSourceUpdateMode)
				{
					return target._currentCellProjectionChanged.Subscribe(
						[handler = std::move(handler)](DataGrid*) { handler(); });
				}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::IsReadOnlyProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(false);
		options.Changed = [](DataGrid& target, const bool&, const bool& value)
		{
			const ControlWeakReference lifetime(&target);
			if (value)
				(void)target.CancelEdit(DataGridEditingUnit::Row);
			if (auto* live = dynamic_cast<DataGrid*>(lifetime.Get()))
			{
				(void)live->CoerceValue(CanUserAddRowsProperty());
				(void)live->CoerceValue(CanUserDeleteRowsProperty());
				live = dynamic_cast<DataGrid*>(lifetime.Get());
				if (!live) return;
				live->InvalidateRows();
				live = dynamic_cast<DataGrid*>(lifetime.Get());
				if (live) live->InvalidateCommandState();
			}
		};
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"IsReadOnly"),
			[](DataGrid& target) { return target._isReadOnly; },
			[](DataGrid& target, const bool& value)
			{ target._isReadOnly = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CanUserAddRowsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Coerce = [](
			DataGrid& target, const bool& value) -> std::optional<bool>
		{
			if (!value || target._isReadOnly || !target.IsEnabled)
				return false;
			const auto* editable = target.EditableItems();
			return editable && editable->CanAddNew();
		};
		options.Changed = [](DataGrid& target, const bool&, const bool&)
		{ target.RefreshNewItemPlaceholder(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 7;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserAddRows"),
			[](DataGrid& target) { return target._canUserAddRows; },
			[](DataGrid& target, const bool& value)
			{ target._canUserAddRows = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CanUserDeleteRowsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Coerce = [](
			DataGrid& target, const bool& value) -> std::optional<bool>
		{
			if (!value || target._isReadOnly || !target.IsEnabled)
				return false;
			const auto* editable = target.EditableItems();
			return editable && editable->CanRemove();
		};
		options.Changed = [](DataGrid& target, const bool&, const bool&)
		{ target.InvalidateCommandState(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 8;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserDeleteRows"),
			[](DataGrid& target) { return target._canUserDeleteRows; },
			[](DataGrid& target, const bool& value)
			{ target._canUserDeleteRows = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CanUserSortColumnsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Changed = [](DataGrid& target, const bool&, const bool&)
		{ target.RefreshHeaderPresenter(); };
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserSortColumns"),
			[](DataGrid& target) { return target._canUserSortColumns; },
			[](DataGrid& target, const bool& value)
			{ target._canUserSortColumns = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CanUserResizeColumnsProperty()
{
	static const auto registration = []
	{
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserResizeColumns"),
			[](DataGrid& target) { return target._canUserResizeColumns; },
			[](DataGrid& target, const bool& value)
			{
				if (target._canUserResizeColumns == value) return;
				if (!target._columnResizeSnapshot.empty())
					target.EndColumnResizeTransaction(true);
				target._canUserResizeColumns = value;
			}, {}, DataGridOptions(true));
	}();
	return *registration;
}

CursorKind DataGridRowHeader::QueryCursor(int localX, int localY)
{
	if (_isResizing) return CursorKind::SizeNS;
	const auto size = GetActualSizeDip();
	if (localX < 0 || static_cast<double>(localX) > size.width)
		return Button::QueryCursor(localX, localY);
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	bool fromTopEdge = false;
	return TryResolveResizeRow(localY, rowIndex, fromTopEdge)
		? CursorKind::SizeNS : Button::QueryCursor(localX, localY);
}

bool DataGridRowHeader::TryResolveResizeRow(
	int localY, size_t& rowIndex, bool& fromTopEdge) const noexcept
{
	rowIndex = DataGridCellInfo::InvalidIndex;
	fromTopEdge = false;
	auto* row = GetRowOwner();
	auto* owner = row ? row->GetDataGridOwner() : nullptr;
	if (!row || !owner || !owner->GetCanUserResizeRows()) return false;
	const auto size = GetActualSizeDip();
	const double height = size.height;
	if (!std::isfinite(height) || height <= 0.0
		|| localY < 0 || static_cast<double>(localY) > height) return false;
	// Keep the middle half available to ButtonBase even when a compact/custom
	// row-header template is shorter than the default WPF header chrome.
	const double gripperHeight = (std::min)(
		DataGridRowHeaderGripperHeight, height * 0.25);
	const size_t currentIndex = row->ItemIndex();
	if (static_cast<double>(localY) >= height - gripperHeight)
		rowIndex = currentIndex;
	else if (localY <= gripperHeight && currentIndex > 0
		&& owner->ResolveRow(currentIndex - 1))
	{
		rowIndex = currentIndex - 1;
		fromTopEdge = true;
	}
	else return false;
	BindingSourceReference item;
	const auto items = owner->GetItemsView();
	return items && items.Get()->TryGetItem(rowIndex, item) && item
		&& !owner->IsNewItemPlaceholder(item);
}

bool DataGridRowHeader::BeginRowResize(int localY)
{
	if (_isResizing) return true;
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	bool fromTopEdge = false;
	if (!TryResolveResizeRow(localY, rowIndex, fromTopEdge)) return false;
	auto* row = GetRowOwner();
	auto* owner = row ? row->GetDataGridOwner() : nullptr;
	double startHeight = 0.0;
	if (!owner || !owner->BeginRowResizeTransaction(rowIndex, startHeight))
		return false;
	const ControlWeakReference ownerLifetime(owner);
	bool transactionCommitted = false;
	auto rollback = MakeScopeExit([ownerLifetime, &transactionCommitted]
	{
		if (transactionCommitted) return;
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->EndRowResizeTransaction(true);
	});
	_isResizing = true;
	_resizeFromTopEdge = fromTopEdge;
	_resizeStartRenderY = RenderSpaceY(*this, localY);
	_resizeStartHeight = startHeight;
	if (!GetPresentationWindow())
	{
		transactionCommitted = true;
		return true;
	}
	const ControlWeakReference lifetime(this);
	const bool captured = CaptureMouse();
	auto* source = dynamic_cast<DataGridRowHeader*>(lifetime.Get());
	if (!source) return false;
	if (!captured || !source->_isResizing || !source->IsMouseCaptured())
	{
		if (source->_isResizing) source->EndRowResize(true);
		return false;
	}
	transactionCommitted = true;
	return true;
}

bool DataGridRowHeader::ContinueRowResize(int localY)
{
	if (!_isResizing) return false;
	auto* row = GetRowOwner();
	auto* owner = row ? row->GetDataGridOwner() : nullptr;
	if (!owner || !owner->ResizeRowInTransaction(
		_resizeStartHeight + RenderSpaceY(*this, localY) - _resizeStartRenderY))
	{
		EndRowResize(true);
		return false;
	}
	return true;
}

void DataGridRowHeader::EndRowResize(bool cancel)
{
	if (!_isResizing) return;
	_isResizing = false;
	_resizeFromTopEdge = false;
	_resizeStartRenderY = 0.0;
	_resizeStartHeight = 0.0;
	auto* row = GetRowOwner();
	const ControlWeakReference ownerLifetime(
		row ? row->GetDataGridOwner() : nullptr);
	if (!cancel)
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			(void)owner->ApplyPendingRowResizeInput();
	if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		owner->EndRowResizeTransaction(cancel);
	if (IsMouseCaptured()) (void)ReleaseMouseCapture();
}

const DependencyProperty& DataGrid::CanUserResizeRowsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Changed = [](DataGrid& target, const bool&, const bool& value)
		{
			if (!value) target.EndRowResizeTransaction(true);
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 13;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserResizeRows"),
			[](DataGrid& target) { return target._canUserResizeRows; },
			[](DataGrid& target, const bool& value)
			{ target._canUserResizeRows = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::EnableColumnVirtualizationProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(false);
		options.Changed = [](DataGrid& target, const bool&, const bool&)
		{
			target.InvalidateRealizedColumnRange();
			target.RefreshColumns();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 20;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"EnableColumnVirtualization"),
			[](DataGrid& target) { return target._enableColumnVirtualization; },
			[](DataGrid& target, const bool& value)
			{ target._enableColumnVirtualization = value; }, {},
			std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CanUserReorderColumnsProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(true);
		options.Changed = [](DataGrid& target, const bool&, const bool& value)
		{
			if (!value) target.CancelColumnReorderForEligibilityChange();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 15;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"CanUserReorderColumns"),
			[](DataGrid& target) { return target._canUserReorderColumns; },
			[](DataGrid& target, const bool& value)
			{ target._canUserReorderColumns = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::FrozenColumnCountProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(0);
		options.Validate = [](const int& value) { return value >= 0; };
		options.Coerce = [](
			DataGrid& target, const int& value) -> std::optional<int>
		{
			const size_t maximum = (std::min)(
				target.ColumnCount(),
				static_cast<size_t>((std::numeric_limits<int>::max)()));
			return (std::min)(value, static_cast<int>(maximum));
		};
		options.Changed = [](DataGrid& target, const int&, const int&)
		{
			target.InvalidateRealizedColumnRange();
			if (!target._coercingFrozenColumnCountForSchema)
				target.RefreshColumns();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 25;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Minimum = 0.0;
		options.Design.Step = 1.0;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(L"FrozenColumnCount"),
			[](DataGrid& target) { return target._frozenColumnCount; },
			[](DataGrid& target, const int& value)
			{ target._frozenColumnCount = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::SelectionUnitProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			static_cast<int>(DataGridSelectionUnit::FullRow));
		options.Validate = [](const int& value)
		{
			return value == static_cast<int>(DataGridSelectionUnit::Cell)
				|| value == static_cast<int>(DataGridSelectionUnit::FullRow)
				|| value == static_cast<int>(
					DataGridSelectionUnit::CellOrRowHeader);
		};
		options.Changed = [](DataGrid& target,
			const int& oldValue, const int& newValue)
		{
			target.OnSelectionUnitChanged(
				static_cast<DataGridSelectionUnit>(oldValue),
				static_cast<DataGridSelectionUnit>(newValue));
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 30;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"Cell", BindingValue(
				static_cast<int>(DataGridSelectionUnit::Cell)) },
			{ L"FullRow", BindingValue(
				static_cast<int>(DataGridSelectionUnit::FullRow)) },
			{ L"CellOrRowHeader", BindingValue(static_cast<int>(
				DataGridSelectionUnit::CellOrRowHeader)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(L"SelectionUnit"),
			[](DataGrid& target)
			{ return static_cast<int>(target._selectionUnit); },
			[](DataGrid& target, const int& value)
			{ target._selectionUnit = static_cast<DataGridSelectionUnit>(value); },
			{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::ColumnWidthProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			DataGridLength::SizeToHeader(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Convert = [](const BindingValue& value)
			-> std::optional<DataGridLength>
		{
			DataGridLength typed;
			if (value.TryGet(typed)) return typed;
			std::wstring text;
			if (!value.TryGetString(text)
				|| !DataGridLength::TryParse(text, typed)) return std::nullopt;
			return typed;
		};
		options.Validate = [](const DataGridLength& value)
		{ return value.IsValid(); };
		options.Changed = [](DataGrid& target,
			const DataGridLength&, const DataGridLength&)
		{ target.RefreshInheritedColumnWidthDefaults(true, false, false); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Text;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, DataGridLength>(
				DependencyPropertyRegistrationLiteral(L"ColumnWidth"),
				[](DataGrid& target) { return target._columnWidth; },
				[](DataGrid& target, const DataGridLength& value)
				{ target._columnWidth = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::MinColumnWidthProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			20.0, DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isfinite(value) && value >= 0.0; };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{ target.RefreshInheritedColumnWidthDefaults(false, true, false); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 11;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Minimum = 0.0;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"MinColumnWidth"),
			[](DataGrid& target) { return target._minColumnWidth; },
			[](DataGrid& target, const double& value)
			{ target._minColumnWidth = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::MaxColumnWidthProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			(std::numeric_limits<double>::infinity)(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return !std::isnan(value) && value >= 0.0; };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{ target.RefreshInheritedColumnWidthDefaults(false, false, true); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 12;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Minimum = 0.0;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"MaxColumnWidth"),
			[](DataGrid& target) { return target._maxColumnWidth; },
			[](DataGrid& target, const double& value)
			{ target._maxColumnWidth = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::ColumnHeaderHeightProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			(std::numeric_limits<double>::quiet_NaN)(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isnan(value) || (std::isfinite(value) && value >= 0.0); };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{ target.RefreshHeaderPresenter(); };
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"ColumnHeaderHeight"),
			[](DataGrid& target) { return target._columnHeaderHeight; },
			[](DataGrid& target, const double& value)
			{ target._columnHeaderHeight = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowHeaderWidthProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			(std::numeric_limits<double>::quiet_NaN)(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isnan(value) || (std::isfinite(value) && value >= 0.0); };
		options.Changed = [](DataGrid& target, const double&, const double& value)
		{
			target.InvalidateRowHeaderWidthBaseline();
			const double actual = std::isnan(value) ? 0.0 : value;
			const ControlWeakReference lifetime(&target);
			(void)target.SetCurrentRowHeaderActualWidth(actual);
			auto* live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live) live->RefreshHeadersVisibility();
			live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live && std::isnan(value))
				(void)live->RefreshRowHeaderActualWidth();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 25;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"RowHeaderWidth"),
			[](DataGrid& target) { return target._rowHeaderWidth; },
			[](DataGrid& target, const double& value)
			{ target._rowHeaderWidth = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowHeaderActualWidthProperty()
{
	return RowHeaderActualWidthPropertyKey().Property();
}

const DependencyPropertyKey& DataGrid::RowHeaderActualWidthPropertyKey()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			0.0, DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isfinite(value) && value >= 0.0; };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{ (void)target.RefreshRowHeaderActualWidthProjection(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 20;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			DataGrid, double>(
				DependencyPropertyRegistrationLiteral(L"RowHeaderActualWidth"),
				[](DataGrid& target)
				{ return target._rowHeaderActualWidth; },
				[](DataGrid& target, const double& value)
				{
					(void)target.SetReadOnlyPropertyField(
						RowHeaderActualWidthPropertyKey(),
						target._rowHeaderActualWidth, value);
				}, {}, std::move(options));
	}();
	return registration.Key();
}

const DependencyProperty& DataGrid::RowHeightProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			(std::numeric_limits<double>::quiet_NaN)(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isnan(value) || (std::isfinite(value) && value > 0.0); };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{
			target.EndRowResizeTransaction(true);
			const ControlWeakReference lifetime(&target);
			(void)target.RefreshRowHeightOverrideProjection();
			auto* live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (!live) return;
			live->RefreshRealizedRowHeights();
			live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live) live->RefreshVirtualScrollMetrics();
		};
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"RowHeight"),
			[](DataGrid& target) { return target._rowHeight; },
			[](DataGrid& target, const double& value)
			{ target._rowHeight = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::MinRowHeightProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			0.0, DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const double& value)
		{ return std::isfinite(value) && value >= 0.0; };
		options.Changed = [](DataGrid& target, const double&, const double&)
		{
			target.EndRowResizeTransaction(true);
			const ControlWeakReference lifetime(&target);
			(void)target.RefreshRowHeightOverrideProjection();
			auto* live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (!live) return;
			live->RefreshRealizedRowHeights();
			live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live) live->RefreshVirtualScrollMetrics();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 29;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Minimum = 0.0;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, double>(
			DependencyPropertyRegistrationLiteral(L"MinRowHeight"),
			[](DataGrid& target) { return target._minRowHeight; },
			[](DataGrid& target, const double& value)
			{ target._minRowHeight = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::HeadersVisibilityProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			static_cast<int>(DataGridHeadersVisibility::All),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const int& value)
		{ return value >= 0 && value <= 3; };
		options.Changed = [](DataGrid& target, const int&, const int& value)
		{
			const ControlWeakReference lifetime(&target);
			target.RefreshHeadersVisibility();
			auto* live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live && HasRowHeaders(
				static_cast<DataGridHeadersVisibility>(value)))
				(void)live->RefreshRowHeaderActualWidth();
			live = dynamic_cast<DataGrid*>(lifetime.Get());
			if (live) live->NotifyAccessibilityStructureChanged();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 30;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"None", BindingValue(
				static_cast<int>(DataGridHeadersVisibility::None)) },
			{ L"Column", BindingValue(
				static_cast<int>(DataGridHeadersVisibility::Column)) },
			{ L"Row", BindingValue(
				static_cast<int>(DataGridHeadersVisibility::Row)) },
			{ L"All", BindingValue(
				static_cast<int>(DataGridHeadersVisibility::All)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(L"HeadersVisibility"),
			[](DataGrid& target)
			{ return static_cast<int>(target._headersVisibility); },
			[](DataGrid& target, const int& value)
			{ target._headersVisibility = static_cast<DataGridHeadersVisibility>(value); },
			{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::GridLinesVisibilityProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			static_cast<int>(DataGridGridLinesVisibility::All),
			DependencyPropertyFlags::AffectsRender);
		options.Validate = [](const int& value)
		{ return value >= 0 && value <= 3; };
		options.Changed = [](DataGrid& target, const int&, const int&)
		{ target.InvalidateRows(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 40;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"All", BindingValue(
				static_cast<int>(DataGridGridLinesVisibility::All)) },
			{ L"Horizontal", BindingValue(
				static_cast<int>(DataGridGridLinesVisibility::Horizontal)) },
			{ L"None", BindingValue(
				static_cast<int>(DataGridGridLinesVisibility::None)) },
			{ L"Vertical", BindingValue(
				static_cast<int>(DataGridGridLinesVisibility::Vertical)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(L"GridLinesVisibility"),
			[](DataGrid& target)
			{ return static_cast<int>(target._gridLinesVisibility); },
			[](DataGrid& target, const int& value)
			{ target._gridLinesVisibility = static_cast<DataGridGridLinesVisibility>(value); },
			{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowBackgroundProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			cui::drawing::NoBrush(), DependencyPropertyFlags::AffectsRender);
		options.Convert = ConvertBrush;
		options.Equals = [](const cui::drawing::Brush& left,
			const cui::drawing::Brush& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const cui::drawing::Brush&, const cui::drawing::Brush&)
		{ target.InvalidateRows(); };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, cui::drawing::Brush>(
				DependencyPropertyRegistrationLiteral(L"RowBackground"),
				[](DataGrid& target) { return target._rowBackground; },
				[](DataGrid& target, const cui::drawing::Brush& value)
				{ target._rowBackground = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::AlternatingRowBackgroundProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			cui::drawing::NoBrush(), DependencyPropertyFlags::AffectsRender);
		options.Convert = ConvertBrush;
		options.Equals = [](const cui::drawing::Brush& left,
			const cui::drawing::Brush& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const cui::drawing::Brush&, const cui::drawing::Brush&)
		{ target.InvalidateRows(); };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, cui::drawing::Brush>(
				DependencyPropertyRegistrationLiteral(L"AlternatingRowBackground"),
				[](DataGrid& target) { return target._alternatingRowBackground; },
				[](DataGrid& target, const cui::drawing::Brush& value)
				{ target._alternatingRowBackground = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::HorizontalGridLinesBrushProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			cui::drawing::MakeSolidColorBrush(
				D2D1_COLOR_F{ 0.85f, 0.85f, 0.85f, 1.0f }),
			DependencyPropertyFlags::AffectsRender);
		options.Convert = ConvertBrush;
		options.Equals = [](const cui::drawing::Brush& left,
			const cui::drawing::Brush& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const cui::drawing::Brush&, const cui::drawing::Brush&)
		{ target.InvalidateRows(); };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, cui::drawing::Brush>(
				DependencyPropertyRegistrationLiteral(L"HorizontalGridLinesBrush"),
				[](DataGrid& target) { return target._horizontalGridLinesBrush; },
				[](DataGrid& target, const cui::drawing::Brush& value)
				{ target._horizontalGridLinesBrush = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::VerticalGridLinesBrushProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			cui::drawing::MakeSolidColorBrush(
				D2D1_COLOR_F{ 0.85f, 0.85f, 0.85f, 1.0f }),
			DependencyPropertyFlags::AffectsRender);
		options.Convert = ConvertBrush;
		options.Equals = [](const cui::drawing::Brush& left,
			const cui::drawing::Brush& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const cui::drawing::Brush&, const cui::drawing::Brush&)
		{ target.InvalidateRows(); };
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, cui::drawing::Brush>(
				DependencyPropertyRegistrationLiteral(L"VerticalGridLinesBrush"),
				[](DataGrid& target) { return target._verticalGridLinesBrush; },
				[](DataGrid& target, const cui::drawing::Brush& value)
				{ target._verticalGridLinesBrush = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowValidationErrorTemplateProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			DefaultDataGridRowValidationErrorTemplateReference(),
			DependencyPropertyFlags::AffectsMeasure);
		options.Equals = [](
			const ControlTemplateReference& left,
			const ControlTemplateReference& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const ControlTemplateReference&,
			const ControlTemplateReference&)
			{ target.RefreshRealizedRowValidationStates(true); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Validation";
		options.Design.CategoryOrder = 400;
		options.Design.Order = 10;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid,
			ControlTemplateReference>(
				DependencyPropertyRegistrationLiteral(
					L"RowValidationErrorTemplate"),
				[](DataGrid& target)
					{ return target._rowValidationErrorTemplate; },
				[](DataGrid& target,
					const ControlTemplateReference& value)
					{ target._rowValidationErrorTemplate = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::ClipboardCopyModeProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			static_cast<int>(DataGridClipboardCopyMode::ExcludeHeader));
		options.Validate = [](const int& value)
		{
			return value == static_cast<int>(DataGridClipboardCopyMode::None)
				|| value == static_cast<int>(
					DataGridClipboardCopyMode::ExcludeHeader)
				|| value == static_cast<int>(
					DataGridClipboardCopyMode::IncludeHeader);
		};
		options.Changed = [](DataGrid& target, const int&, const int&)
		{ target.InvalidateCommandState(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 110;
		options.Design.Order = 35;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"None", BindingValue(static_cast<int>(
				DataGridClipboardCopyMode::None)) },
			{ L"ExcludeHeader", BindingValue(static_cast<int>(
				DataGridClipboardCopyMode::ExcludeHeader)) },
			{ L"IncludeHeader", BindingValue(static_cast<int>(
				DataGridClipboardCopyMode::IncludeHeader)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(L"ClipboardCopyMode"),
			[](DataGrid& target)
			{ return static_cast<int>(target._clipboardCopyMode); },
			[](DataGrid& target, const int& value)
			{
				target._clipboardCopyMode =
					static_cast<DataGridClipboardCopyMode>(value);
			}, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::CellStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(std::wstring{});
		options.Changed = [](DataGrid& target,
			const std::wstring&, const std::wstring&)
		{ target.RefreshRealizedCellStyles(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 50;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"CellStyle"),
			[](DataGrid& target) { return target._cellStyle; },
			[](DataGrid& target, const std::wstring& value)
			{ target._cellStyle = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::ColumnHeaderStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(std::wstring{});
		options.Changed = [](DataGrid& target,
			const std::wstring&, const std::wstring&)
		{ target.RefreshRealizedColumnHeaderPresentation(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 51;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"ColumnHeaderStyle"),
			[](DataGrid& target) { return target._columnHeaderStyle; },
			[](DataGrid& target, const std::wstring& value)
			{ target._columnHeaderStyle = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(std::wstring{});
		options.Coerce = [](DataGrid& target,
			const std::wstring& value) -> std::optional<std::wstring>
		{
			if (!value.empty() && target._rowStyleSelector)
				return std::nullopt;
			return value;
		};
		options.Changed = [](DataGrid& target,
			const std::wstring&, const std::wstring&)
		{
			if (!target.ApplyItemContainerStyle())
				throw std::runtime_error("DataGrid.RowStyle could not be applied");
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 52;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"RowStyle"),
			[](DataGrid& target) { return target._rowStyle; },
			[](DataGrid& target, const std::wstring& value)
			{ target._rowStyle = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowStyleSelectorProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(ItemStyleSelectorReference{});
		options.Equals = [](const ItemStyleSelectorReference& left,
			const ItemStyleSelectorReference& right) { return left == right; };
		options.Coerce = [](DataGrid& target,
			const ItemStyleSelectorReference& value)
			-> std::optional<ItemStyleSelectorReference>
		{
			if (value && !target._rowStyle.empty()) return std::nullopt;
			return value;
		};
		options.Changed = [](DataGrid& target,
			const ItemStyleSelectorReference&,
			const ItemStyleSelectorReference&)
		{
			if (!target.ApplyItemContainerStyle())
				throw std::runtime_error(
					"DataGrid.RowStyleSelector could not be applied");
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 53;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid,
			ItemStyleSelectorReference>(
				DependencyPropertyRegistrationLiteral(L"RowStyleSelector"),
				[](DataGrid& target) { return target._rowStyleSelector; },
				[](DataGrid& target,
					const ItemStyleSelectorReference& value)
					{ target._rowStyleSelector = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowHeaderStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(std::wstring{});
		options.Changed = [](DataGrid& target,
			const std::wstring&, const std::wstring&)
		{ target.RefreshRealizedRowHeaderPresentation(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 53;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"RowHeaderStyle"),
			[](DataGrid& target) { return target._rowHeaderStyle; },
			[](DataGrid& target, const std::wstring& value)
			{ target._rowHeaderStyle = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowHeaderTemplateProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			ItemTemplateReference{}, DependencyPropertyFlags::AffectsMeasure);
		options.Equals = [](const ItemTemplateReference& left,
			const ItemTemplateReference& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const ItemTemplateReference&, const ItemTemplateReference&)
		{ target.RefreshRealizedRowHeaderPresentation(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 54;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"RowHeaderTemplate"),
				[](DataGrid& target) { return target._rowHeaderTemplate; },
				[](DataGrid& target, const ItemTemplateReference& value)
				{ target._rowHeaderTemplate = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowHeaderTemplateSelectorProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(ItemTemplateSelectorReference{},
			DependencyPropertyFlags::AffectsMeasure);
		options.Equals = [](const ItemTemplateSelectorReference& left,
			const ItemTemplateSelectorReference& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const ItemTemplateSelectorReference&,
			const ItemTemplateSelectorReference&)
			{ target.RefreshRealizedRowHeaderPresentation(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 55;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid,
			ItemTemplateSelectorReference>(
				DependencyPropertyRegistrationLiteral(
					L"RowHeaderTemplateSelector"),
				[](DataGrid& target)
					{ return target._rowHeaderTemplateSelector; },
				[](DataGrid& target,
					const ItemTemplateSelectorReference& value)
					{ target._rowHeaderTemplateSelector = value; },
				{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::AreRowDetailsFrozenProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(false,
			DependencyPropertyFlags::AffectsArrange);
		options.Changed = [](DataGrid& target, const bool&, const bool&)
			{ target.RefreshRealizedRowDetailsHorizontalAlignment(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 55;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, bool>(
			DependencyPropertyRegistrationLiteral(L"AreRowDetailsFrozen"),
			[](DataGrid& target) { return target._areRowDetailsFrozen; },
			[](DataGrid& target, const bool& value)
				{ target._areRowDetailsFrozen = value; },
			{}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowDetailsVisibilityModeProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(static_cast<int>(
			DataGridRowDetailsVisibilityMode::VisibleWhenSelected),
			DependencyPropertyFlags::AffectsMeasure);
		options.Validate = [](const int& value)
		{
			return value == static_cast<int>(
				DataGridRowDetailsVisibilityMode::Collapsed)
				|| value == static_cast<int>(
					DataGridRowDetailsVisibilityMode::Visible)
				|| value == static_cast<int>(
					DataGridRowDetailsVisibilityMode::VisibleWhenSelected);
		};
		options.Changed = [](DataGrid& target, const int&, const int&)
			{ target.RefreshRealizedRowDetails(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 56;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"Collapsed", BindingValue(static_cast<int>(
				DataGridRowDetailsVisibilityMode::Collapsed)) },
			{ L"Visible", BindingValue(static_cast<int>(
				DataGridRowDetailsVisibilityMode::Visible)) },
			{ L"VisibleWhenSelected", BindingValue(static_cast<int>(
				DataGridRowDetailsVisibilityMode::VisibleWhenSelected)) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid, int>(
			DependencyPropertyRegistrationLiteral(
				L"RowDetailsVisibilityMode"),
			[](DataGrid& target)
			{ return static_cast<int>(target._rowDetailsVisibilityMode); },
			[](DataGrid& target, const int& value)
			{
				target._rowDetailsVisibilityMode =
					static_cast<DataGridRowDetailsVisibilityMode>(value);
			}, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowDetailsTemplateProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(
			ItemTemplateReference{}, DependencyPropertyFlags::AffectsMeasure);
		options.Equals = [](const ItemTemplateReference& left,
			const ItemTemplateReference& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const ItemTemplateReference&, const ItemTemplateReference&)
			{ target.RefreshRealizedRowDetails(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 57;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			DataGrid, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"RowDetailsTemplate"),
				[](DataGrid& target) { return target._rowDetailsTemplate; },
				[](DataGrid& target, const ItemTemplateReference& value)
				{ target._rowDetailsTemplate = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& DataGrid::RowDetailsTemplateSelectorProperty()
{
	static const auto registration = []
	{
		auto options = DataGridOptions(ItemTemplateSelectorReference{},
			DependencyPropertyFlags::AffectsMeasure);
		options.Equals = [](const ItemTemplateSelectorReference& left,
			const ItemTemplateSelectorReference& right) { return left == right; };
		options.Changed = [](DataGrid& target,
			const ItemTemplateSelectorReference&,
			const ItemTemplateSelectorReference&)
			{ target.RefreshRealizedRowDetails(); };
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 20;
		options.Design.Order = 59;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<DataGrid,
			ItemTemplateSelectorReference>(
				DependencyPropertyRegistrationLiteral(
					L"RowDetailsTemplateSelector"),
				[](DataGrid& target)
					{ return target._rowDetailsTemplateSelector; },
				[](DataGrid& target,
					const ItemTemplateSelectorReference& value)
					{ target._rowDetailsTemplateSelector = value; },
				{}, std::move(options));
	}();
	return *registration;
}

void DataGrid::RegisterDependencyProperties()
{
	ListBox::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)AutoGenerateColumnsProperty();
	(void)CurrentItemProperty();
	(void)CurrentColumnProperty();
	(void)CurrentCellProperty();
	(void)IsReadOnlyProperty();
	(void)CanUserAddRowsProperty();
	(void)CanUserDeleteRowsProperty();
	(void)CanUserSortColumnsProperty();
	(void)CanUserResizeColumnsProperty();
	(void)CanUserResizeRowsProperty();
	(void)CanUserReorderColumnsProperty();
	(void)EnableColumnVirtualizationProperty();
	(void)FrozenColumnCountProperty();
	(void)SelectionUnitProperty();
	(void)ColumnWidthProperty();
	(void)MinColumnWidthProperty();
	(void)MaxColumnWidthProperty();
	(void)ColumnHeaderHeightProperty();
	(void)RowHeaderWidthProperty();
	(void)RowHeaderActualWidthProperty();
	(void)RowHeightProperty();
	(void)MinRowHeightProperty();
	(void)HeadersVisibilityProperty();
	(void)GridLinesVisibilityProperty();
	(void)RowBackgroundProperty();
	(void)AlternatingRowBackgroundProperty();
	(void)HorizontalGridLinesBrushProperty();
	(void)VerticalGridLinesBrushProperty();
	(void)RowValidationErrorTemplateProperty();
	(void)ClipboardCopyModeProperty();
	(void)CellStyleProperty();
	(void)ColumnHeaderStyleProperty();
	(void)RowStyleProperty();
	(void)RowStyleSelectorProperty();
	(void)RowHeaderStyleProperty();
	(void)RowHeaderTemplateProperty();
	(void)RowHeaderTemplateSelectorProperty();
	(void)AreRowDetailsFrozenProperty();
	(void)RowDetailsVisibilityModeProperty();
	(void)RowDetailsTemplateProperty();
	(void)RowDetailsTemplateSelectorProperty();
#endif
}

const RoutedCommand& DataGrid::BeginEditCommand()
{
	static const RoutedCommand command(L"BeginEdit");
	return command;
}

const RoutedCommand& DataGrid::CommitEditCommand()
{
	static const RoutedCommand command(L"CommitEdit");
	return command;
}

const RoutedCommand& DataGrid::CancelEditCommand()
{
	static const RoutedCommand command(L"CancelEdit");
	return command;
}

const RoutedCommand& DataGrid::DeleteCommand()
{
	return ApplicationCommands::Delete();
}

const RoutedCommand& DataGrid::CopyCommand()
{
	return ApplicationCommands::Copy();
}

void DataGrid::EnsureCommandBindingsRegistered()
{
	static const auto registrations = []
	{
		std::vector<EventConnection> result;
		result.reserve(12);
		auto registerCommand = [&result](
			const RoutedCommand& command,
			void (DataGrid::*canExecute)(CanExecuteRoutedEventArgs&),
			void (DataGrid::*executed)(ExecutedRoutedEventArgs&))
		{
			CommandBinding binding;
			binding.Command = command;
			binding.CanExecute = [canExecute](
				Control* target, CanExecuteRoutedEventArgs& args)
			{
				if (auto* grid = dynamic_cast<DataGrid*>(target))
					(grid->*canExecute)(args);
			};
			binding.Executed = [executed](
				Control* target, ExecutedRoutedEventArgs& args)
			{
				if (auto* grid = dynamic_cast<DataGrid*>(target))
					(grid->*executed)(args);
			};
			result.push_back(
				RoutedCommandManager::RegisterClassCommandBinding(
					UIClass::UI_DataGrid, std::move(binding)));
		};
		auto registerGesture = [&result](
			const RoutedCommand& command, Key key,
			ModifierKeys modifiers = ModifierKeys::None)
		{
			KeyBinding binding;
			binding.Command = command;
			binding.Gesture.Key = key;
			binding.Gesture.Modifiers = modifiers;
			result.push_back(
				RoutedCommandManager::RegisterClassInputBinding(
					UIClass::UI_DataGrid,
					InputBinding(std::move(binding))));
		};

		registerCommand(BeginEditCommand(),
			&DataGrid::OnCanExecuteBeginEdit,
			&DataGrid::OnExecutedBeginEdit);
		registerCommand(CommitEditCommand(),
			&DataGrid::OnCanExecuteCommitEdit,
			&DataGrid::OnExecutedCommitEdit);
		registerCommand(CancelEditCommand(),
			&DataGrid::OnCanExecuteCancelEdit,
			&DataGrid::OnExecutedCancelEdit);
		registerCommand(DeleteCommand(),
			&DataGrid::OnCanExecuteDelete,
			&DataGrid::OnExecutedDelete);
		registerCommand(CopyCommand(),
			&DataGrid::OnCanExecuteCopy,
			&DataGrid::OnExecutedCopy);
		registerGesture(BeginEditCommand(), Key::F2);
		registerGesture(CancelEditCommand(), Key::Escape);
		// ApplicationCommands supplies this default gesture in WPF. CUI keeps
		// the same class-local behavior until application command gestures are
		// generalized beyond the controls that consume them.
		registerGesture(DeleteCommand(), Key::Delete);
		registerGesture(CopyCommand(), Key::C, ModifierKeys::Control);
		return result;
	}();
	(void)registrations;
}

DataGrid::DataGrid()
	: _itemsView(std::make_shared<CollectionViewSource>()),
	  _displayItemsView(DataGridItemsView::Create(_itemsView)),
	  _selectedCells(this),
	  _horizontalGridLinesBrush(cui::drawing::MakeSolidColorBrush(
		  D2D1_COLOR_F{ 0.85f, 0.85f, 0.85f, 1.0f })),
	  _verticalGridLinesBrush(cui::drawing::MakeSolidColorBrush(
		  D2D1_COLOR_F{ 0.85f, 0.85f, 0.85f, 1.0f })),
	  _rowValidationErrorTemplate(
		  DefaultDataGridRowValidationErrorTemplateReference())
{
	RegisterDependencyProperties();
	EnsureCommandBindingsRegistered();
	// WPF's DataGrid view publishes one terminal Refresh/Reset for sorting.
	// Keeping the generic CollectionViewSource precise-Move default lets other
	// controls opt into container-preserving changes without making a header
	// click execute one complete DataGrid transaction per moved row.
	_itemsView->SetUseResetNotificationForComplexRefresh(true);
	// WPF enables DataGrid row virtualization by default.  Install the native
	// default before generated XAML assigns ItemsSource: implicit theme styles
	// are applied only after InitializeComponent has built the complete tree.
	(void)TrySetPropertyValue(
		ItemsControl::ItemsPanelProperty(),
		BindingValue(DefaultDataGridRowsPanel()),
		DependencyPropertyValueSource::Theme);
	SetSelectionMode(SelectionMode::Extended);
	ListBox::SetItemsSource(BindingListReference(_displayItemsView));
	(void)CoerceValue(CanUserAddRowsProperty());
	(void)CoerceValue(CanUserDeleteRowsProperty());
	RetainEventConnection(OnKeyDown.Subscribe(
		[this](Control*, KeyEventArgs& args)
		{
			// Direct DataGrid input already ran HandleCellKey in ProcessInput.
			// This routed hook exists for an editing descendant such as TextBox.
			if (args.OriginalSource == this) return;
			if (auto* cell = ResolveCurrentCellContainer();
				cell && cell->GetIsEditing())
			{
				auto* editor = cell->GetEditingElement();
				if (dynamic_cast<TextBox*>(editor)
					&& EditingElementOwnsKey(args.Key, args.Modifiers)
					&& IsRoutedDescendantOrSelf(
						args.OriginalSource, editor)) return;
			}
			if (!args.Handled && HandleCellKey(args.Key, args.Modifiers))
				args.Handled = true;
		}));
	RetainEventConnection(OnPropertyValueChanged.Subscribe(
		[this](DependencyObject*, const DependencyPropertyChangedEventArgs& args)
		{
			if (args.Property == &Control::IsEnabledProperty())
			{
				const ControlWeakReference lifetime(this);
				(void)CoerceValue(CanUserAddRowsProperty());
				if (auto* live = dynamic_cast<DataGrid*>(lifetime.Get()))
				{
					(void)live->CoerceValue(CanUserDeleteRowsProperty());
					live = dynamic_cast<DataGrid*>(lifetime.Get());
					if (live) live->InvalidateCommandState();
				}
				return;
			}
			if (args.Property != &ListBox::SelectionModeProperty()) return;
			const ControlWeakReference lifetime(this);
			RefreshHeadersVisibility();
			if (!lifetime.Get()
				|| GetSelectionMode() != SelectionMode::Single) return;
			if (_selectionUnit == DataGridSelectionUnit::FullRow)
			{
				SynchronizeSelectedCellsFromRows();
				return;
			}
			if (!GetSelectedIndices().empty())
			{
				const size_t row = static_cast<size_t>(
					GetSelectedIndices().back());
				std::vector<DataGridCellInfo> retainedRow;
				retainedRow.reserve(_columns.size());
				for (size_t column = 0; column < _columns.size(); ++column)
				{
					DataGridCellInfo info;
					if (TryCreateCellInfo(row, column, info))
						retainedRow.push_back(std::move(info));
				}
				_selectionAnchor = retainedRow.empty()
					? std::optional<DataGridCellInfo>{}
					: std::optional<DataGridCellInfo>{ retainedRow.front() };
				ResetSelectionRange();
				ApplySelectedCells(std::move(retainedRow));
				return;
			}
			if (_selectedCells.size() <= 1) return;
			DataGridCellInfo retained = _selectedCells.front();
			_selectionAnchor = _selectedCells.empty()
				? std::optional<DataGridCellInfo>{}
				: std::optional<DataGridCellInfo>{ retained };
			ResetSelectionRange();
			(void)ApplySelectedCells({ std::move(retained) });
		}));
}

void DataGrid::RetireColumnHeadersPresenterNoCallbacks(
	DataGridColumnHeadersPresenter* presenter,
	std::uint64_t committedGeneration) noexcept
{
	if (!presenter) return;
	const auto retireHeader = [this](DataGridColumnHeader* header) noexcept
	{
		if (!header || (header->_owner != this
			&& header->_ownerLifetime.Get() != this)) return;
		ClearColumnHeaderPresentationError(*header);
		header->_owner = nullptr;
		header->_ownerLifetime = nullptr;
		header->_column = nullptr;
		header->_columnIndex = DataGridCellInfo::InvalidIndex;
		header->_multiColumnSortRequested = false;
		header->_isResizing = false;
		header->_resizeFromLeftEdge = false;
		header->_resizingColumnIndex = DataGridCellInfo::InvalidIndex;
		header->_resizeStartRenderX = 0.0;
		header->_resizeStartWidth = 0.0;
		header->_isReorderPending = false;
		header->_isReordering = false;
		header->_reorderStartLocalX = 0.0;
		header->_reorderStartRenderX = 0.0;
		header->_reorderStartRenderY = 0.0;
	};
	for (auto* header : presenter->_frozenHeaders) retireHeader(header);
	for (auto* header : presenter->_headers) retireHeader(header);
	for (int index = 0; index < presenter->VisualChildCount(); ++index)
		retireHeader(dynamic_cast<DataGridColumnHeader*>(
			presenter->GetVisualChild(index)));
	// A failed/staged transaction can clear the presenter owner before its
	// sidecars are reset.  That null-owner state still belongs to this retiring
	// transaction; only a presenter positively owned by another grid is foreign.
	if (presenter->_owner && presenter->_owner != this) return;

	if (committedGeneration == 0)
	{
		committedGeneration = ++presenter->_initializeGeneration;
		if (committedGeneration == 0)
			committedGeneration = ++presenter->_initializeGeneration;
	}
	presenter->_committedInitializeGeneration = committedGeneration;
	if (++presenter->_presentationRefreshGeneration == 0)
		++presenter->_presentationRefreshGeneration;
	presenter->_frozenHeaders.clear();
	presenter->_headers.clear();
	presenter->_owner = nullptr;
	presenter->_columnStorageIsSparse = false;
	presenter->_realizedFrozenColumnEnd = 0;
	presenter->_realizedColumnBegin = 0;
	presenter->_realizedColumnEnd = 0;
	presenter->_appliedColumnWidthProjectionRevision = 0;
	presenter->_isColumnHeaderDragging = false;
	presenter->_reorderingColumn = nullptr;
	presenter->_dropDisplayIndex = DataGridCellInfo::InvalidIndex;
	presenter->_dropIndicatorVisible = false;
	presenter->_dragIndicator = nullptr;
	presenter->_dropIndicator = nullptr;
}

DataGrid::~DataGrid()
{
	_destroyingDataGrid = true;
	auto* currentHeaders = dynamic_cast<DataGridColumnHeadersPresenter*>(
		_headersPresenter.Get());
	bool retiredCurrentHeaders = false;
	for (const auto& lifetime : _trackedColumnHeaderPresenters)
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			lifetime.Get());
		if (!presenter) continue;
		retiredCurrentHeaders = retiredCurrentHeaders
			|| presenter == currentHeaders;
		RetireColumnHeadersPresenterNoCallbacks(presenter);
	}
	if (!retiredCurrentHeaders)
		RetireColumnHeadersPresenterNoCallbacks(currentHeaders);
	_trackedColumnHeaderPresenters.clear();
	_headersPresenter = nullptr;
	if (_displayItemsView) _displayItemsView->Detach();
	// Do not publish row-container state while the visual tree is being torn
	// down. The strong item reference keeps a custom source alive through the
	// best-effort rollback of its one pending transaction.
	const auto item = std::move(_editingRowItem);
	const bool sourceTransaction = _editingRowHasSourceTransaction;
	_editingRowHasSourceTransaction = false;
	_editingRowOccurrence = DataGridCellInfo::InvalidIndex;
	_editingRowIndex = DataGridCellInfo::InvalidIndex;
	if (sourceTransaction && item)
	{
		try
		{
			if (auto* editable = dynamic_cast<IEditableBindingSource*>(item.Get()))
				(void)editable->CancelEdit();
		}
		catch (...) {}
	}
	try
	{
		if (auto* editable = EditableItems(); editable
			&& editable->IsAddingNew()) (void)editable->CancelNew();
	}
	catch (...) {}
}

std::unique_ptr<AutomationPeer> DataGrid::OnCreateAutomationPeer()
{
	return std::make_unique<DataGridAutomationPeer>(*this);
}

void DataGrid::PrepareMeasureCore(
	const cui::core::Constraints& available)
{
	(void)ApplyPendingRowResizeInput();
	ListBox::PrepareMeasureCore(available);
	// A native resize burst may publish dozens of Width values before the next
	// layout pass. Project only the newest value into the realized header/rows,
	// before their template subtree measures against it.
	(void)ApplyPendingColumnResizeInput();
	ApplyPendingColumnWidths();
}

void DataGrid::PreparePresentation()
{
	const ControlWeakReference ownerLifetime(this);
	const bool coalescedColumnResizeFrame =
		(std::isfinite(_columnResizePendingRawWidth)
			|| _columnWidthRefreshPending)
		&& !_columnResizeSnapshot.empty();
	// The input event already queued damage for this frame. Width solving and
	// projection can invalidate changed descendants again while the retained
	// scene is being prepared, so merge those requests into the frame in flight
	// instead of posting an otherwise redundant follow-up paint.
	ScopedVisualInvalidation localFrame(
		*this, !coalescedColumnResizeFrame);
	(void)ApplyPendingRowResizeInput();
	(void)ApplyPendingColumnResizeInput();
	if (!ownerLifetime.Get()) return;
	if (_horizontalScrollAlignmentPending)
	{
		_horizontalScrollAlignmentPending = false;
		RefreshHorizontalScrollAlignment();
		if (!ownerLifetime.Get()) return;
	}
	// Native column dragging schedules a DataGrid-local damage frame instead of
	// invalidating the Window measure root. Project the newest coalesced widths
	// immediately before the retained scene prepares this node, then let the
	// ordinary ItemsControl presentation hook commit only the invalid local paths.
	if (_columnWidthRefreshPending && !_columnResizeSnapshot.empty())
	{
		// Arrange invalidates every changed cell's old and new bounds. The damage
		// that brought us into this frame already covers that complete suffix, so
		// retain the geometry revisions but discard the duplicate future damage.
		// This prevents a drag from perpetually carrying one redundant paint turn.
		ApplyPendingColumnWidths(false);
		ListBox::PreparePresentation();
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		{
			live->_columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
			live->_columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
			live->_columnWidthDirtyVisualSpan =
				(std::numeric_limits<double>::quiet_NaN)();
			live->_columnWidthMeasureDirty.clear();
		}
		return;
	}
	if (_columnWidthRefreshPending) ApplyPendingColumnWidths();
	ListBox::PreparePresentation();
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		live->_columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
		live->_columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
		live->_columnWidthDirtyVisualSpan =
			(std::numeric_limits<double>::quiet_NaN)();
		live->_columnWidthMeasureDirty.clear();
	}
}

void DataGrid::Arrange(cui::core::Rect finalRect)
{
	// Column headers and rows live in separate Grids so the header can remain
	// vertically frozen. Resolve every DataGridLength, including Star, against
	// one shared data viewport before either Grid is arranged; otherwise an
	// unbounded ScrollViewer row measures Star as content while the bounded
	// header distributes the same Stars across its viewport.
	double columnViewport = ResolveColumnViewportWidth(finalRect.width);
	// ScrollViewer publishes its exact viewport after arranging the body. During
	// continuous window resizing that value therefore describes the previous
	// DataGrid slot. Carry the owner's width delta forward so Star columns are
	// projected before the header/body walk, avoiding an old-width arrange plus a
	// corrective second pass on every WM_SIZE. A scrollbar state transition is
	// corrected locally by the subsequent ScrollChanged notification.
	if (auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get()))
	{
		const double published = scroll->GetViewportWidth();
		const auto previousSize = GetActualSizeDip();
		if (std::isfinite(published) && published > 0.0
			&& previousSize.width > 0.0f
			&& std::isfinite(finalRect.width))
			columnViewport = (std::max)(0.0,
				columnViewport + static_cast<double>(finalRect.width)
				- static_cast<double>(previousSize.width));
	}
	UpdateColumnViewportWidth(columnViewport, false);
	RefreshRealizedColumns();
	ListBox::Arrange(finalRect);
}

void DataGrid::SetItemsSource(BindingListReference value)
{
	const ControlWeakReference ownerLifetime(this);
	if (_source == value && _itemsView
		&& _displayItemsView
		&& ListBox::GetItemsSource().Shared() == _displayItemsView) return;
	if (_settingItemsSource || IsItemsSourceUpdateInProgress())
		throw std::logic_error(
			"DataGrid does not support reentrant ItemsSource changes");
	if (_editingRowItem)
	{
		if (_startingRowEdit)
			AbandonRowEditTransaction(true);
		else if (_endingRowEdit)
		{
			if (_callingRowEditSource) ClearRowEditTransaction();
			else AbandonRowEditTransaction(true);
		}
		else if (!CancelEdit(DataGridEditingUnit::Row)) return;
		auto* liveAfterEdit = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!liveAfterEdit) return;
		if (liveAfterEdit->_source == value && liveAfterEdit->_itemsView
			&& liveAfterEdit->_displayItemsView
			&& liveAfterEdit->ListBox::GetItemsSource().Shared()
				== liveAfterEdit->_displayItemsView) return;
	}
	else if (IsAddingNewItem())
	{
		if (!CancelPendingNewItem()) return;
		auto* liveAfterAdd = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!liveAfterAdd) return;
		if (liveAfterAdd->_source == value && liveAfterAdd->_itemsView
			&& liveAfterAdd->_displayItemsView
			&& liveAfterAdd->ListBox::GetItemsSource().Shared()
				== liveAfterAdd->_displayItemsView) return;
	}
	// WPF does not carry a view's SortDescriptions across an ItemsSource
	// identity boundary. Doing so would also force an otherwise lazy million-row
	// source to materialize every item merely because the previous source was
	// sorted. Keep same-source recovery semantics, and clear column indicators
	// only after the replacement transaction commits.
	const bool clearSortState = _source
		&& _source.Shared() != value.Shared();
	auto previousAccessibilityState = CaptureAccessibilityIdentityState();
	_settingItemsSource = true;
	InvalidateItemOccurrenceCache();
	auto resetSettingItemsSource = MakeScopeExit([ownerLifetime]
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		{
			live->_settingItemsSource = false;
			// Candidate construction itself precedes the main transaction try and
			// may throw after an old-view callback committed a real structure
			// change.  Flush that restored/old pending event here without ever
			// replacing the exception already leaving SetItemsSource.
			try { live->FlushAccessibilityStructureChange(); }
			catch (...) {}
		}
	});

	// Populate the ICollectionView before ItemsControl subscribes to it.  A
	// first projection publishes precise Add notifications; attaching an empty
	// view first would turn N source records into N snapshot/generator
	// transactions even though no prior containers can be preserved.
	auto candidate = value ? std::dynamic_pointer_cast<CollectionViewSource>(
		value.Shared()) : std::shared_ptr<CollectionViewSource>{};
	const bool usesProvidedView = static_cast<bool>(candidate);
	if (!candidate) candidate = std::make_shared<CollectionViewSource>();
	candidate->SetUseResetNotificationForComplexRefresh(true);
	if (!usesProvidedView && !clearSortState && _itemsView)
		candidate->SetSortDescriptions(_itemsView->SortDescriptions());
	if (!usesProvidedView) candidate->SetSource(value);
	BindingListReference groupingSource;
	if (value && dynamic_cast<IBindingListGroupView*>(value.Get()))
		groupingSource = value;
	auto candidateDisplay = DataGridItemsView::Create(
		candidate, std::move(groupingSource));
	EventConnection candidateRowHeaderSourceChanged;
	if (value)
	{
		auto* observedSource = value.Get();
		candidateRowHeaderSourceChanged = observedSource->SubscribeChanged(
			[ownerLifetime, observedSource](const CollectionChangedEventArgs& change)
			{
				if (change.Action != CollectionChangeAction::Reset) return;
				auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (!owner || owner->_source.Get() != observedSource
					|| owner->_settingItemsSource
					|| owner->IsItemsSourceUpdateInProgress()
					|| !std::isnan(owner->_rowHeaderWidth)) return;
				owner->InvalidateRowHeaderWidthBaseline();
				(void)owner->SetCurrentRowHeaderActualWidth(0.0);
				owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (owner) (void)owner->RefreshRowHeaderActualWidth();
			});
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;

	const auto previousSource = live->_source;
	const auto previousView = live->_itemsView;
	const auto previousDisplayView = live->_displayItemsView;
	const auto previousCurrentCell = live->_currentCell;
	const bool previousRuntimeAutoGenerationComplete =
		live->_runtimeAutoGenerationComplete;
	const bool previousAutoGeneratedColumnsEventPending =
		live->_autoGeneratedColumnsEventPending;
	struct RuntimeAutoColumnSnapshot final
	{
		size_t Index = 0;
		std::unique_ptr<DataGridColumn> Column;
	};
	std::vector<RuntimeAutoColumnSnapshot> previousRuntimeAutoColumns;
#if CUI_ENABLE_DYNAMIC_XAML
	const auto previousLogicalColumns = live->_logicalColumns;
	const size_t runtimeAutoColumnCount = static_cast<size_t>(std::count_if(
		live->_columns.begin(), live->_columns.end(), [](const auto& column)
		{ return column->_isRuntimeAutoGenerated; }));
	if (runtimeAutoColumnCount != 0)
	{
		// Allocate both transaction buffers before moving a single definition.
		// AOT-generated columns remain authoritative and are never regenerated
		// from runtime reflection.
		previousRuntimeAutoColumns.reserve(runtimeAutoColumnCount);
		std::vector<std::unique_ptr<DataGridColumn>> retainedColumns;
		retainedColumns.reserve(live->_columns.size());
		for (size_t index = 0; index < live->_columns.size(); ++index)
		{
			if (live->_columns[index]->_isRuntimeAutoGenerated)
			{
				live->_columns[index]->_visibleIndex =
					DataGridColumn::UnsetDisplayIndex;
				previousRuntimeAutoColumns.push_back(
					{ index, std::move(live->_columns[index]) });
			}
			else retainedColumns.push_back(std::move(live->_columns[index]));
		}
		live->_columns = std::move(retainedColumns);
		live->_logicalColumns.erase(std::remove_if(
			live->_logicalColumns.begin(), live->_logicalColumns.end(),
			[](const DataGridColumn* column)
			{ return column->_isRuntimeAutoGenerated; }),
			live->_logicalColumns.end());
		live->ReindexDisplayColumns();
		live->ReevaluateFrozenColumnCountForSchemaChange();
	}
	live->_runtimeAutoGenerationComplete = false;
	live->_autoGeneratedColumnsEventPending = false;
#endif
	auto previousWidths = std::move(live->_resolvedColumnWidths);
	auto previousColumnWidthPrefix = std::move(live->_columnWidthPrefix);
	auto previousSampledContentWidths =
		std::move(live->_sampledColumnContentWidths);
	const size_t previousColumnContentWidthCacheEpoch =
		live->_columnContentWidthCacheEpoch;
	auto rollbackDerivedState = [&](DataGrid& target)
	{
		// A UIA query may have materialized identities for the candidate view
		// from a reentrant ItemsSource callback.  Mark that projection stale
		// before any candidate column is destroyed.
		target.InvalidateAccessibilityVirtualIdentities();
		target._source = previousSource;
		target._runtimeAutoGenerationComplete =
			previousRuntimeAutoGenerationComplete;
		target._autoGeneratedColumnsEventPending =
			previousAutoGeneratedColumnsEventPending;
		if (++target._cellSelectionRevision == 0)
			target._cellSelectionRevision = 1;
		target._itemsView = previousView;
		target._displayItemsView = previousDisplayView;
		target._resolvedColumnWidths = std::move(previousWidths);
		target._columnWidthPrefix = std::move(previousColumnWidthPrefix);
		target._sampledColumnContentWidths =
			std::move(previousSampledContentWidths);
		target._columnContentWidthCacheEpoch =
			previousColumnContentWidthCacheEpoch;
		target._currentCell = previousCurrentCell;
		// Runtime auto-generation appends logical identities, but a customization
		// may request a display position. Remove the partial candidate from either
		// visual position, then restore the exact old logical/display projections.
		for (size_t index = target._columns.size(); index > 0;)
		{
			--index;
			if (!target._columns[index]->_isRuntimeAutoGenerated) continue;
			target._columns[index]->_owner = nullptr;
			target._columns[index]->_displayIndex =
				DataGridColumn::UnsetDisplayIndex;
			target._columns[index]->_visibleIndex =
				DataGridColumn::UnsetDisplayIndex;
			target._columns.erase(target._columns.begin() + index);
		}
		for (auto& snapshot : previousRuntimeAutoColumns)
		{
			const size_t index = (std::min)(
				snapshot.Index, target._columns.size());
			target._columns.insert(
				target._columns.begin() + index, std::move(snapshot.Column));
		}
#if CUI_ENABLE_DYNAMIC_XAML
		// A reentrant candidate callback may permanently delete an authored
		// definition. Restore the old logical order only for definitions whose
		// ownership survived in the display projection; never revive a dangling
		// pointer from the transaction snapshot.
		target._logicalColumns.clear();
		target._logicalColumns.reserve(previousLogicalColumns.size());
		for (auto* previous : previousLogicalColumns)
		{
			const bool survived = std::any_of(
				target._columns.begin(), target._columns.end(),
				[previous](const auto& candidate)
				{ return candidate.get() == previous; });
			if (survived) target._logicalColumns.push_back(previous);
		}
#endif
		target.ReindexDisplayColumns();
		target.ReevaluateFrozenColumnCountForSchemaChange();
		target.PruneAccessibilityColumnIdentities();
		target.RestoreAccessibilityIdentityState(
			std::move(previousAccessibilityState));
	};
	try
	{
		// Candidate preparation above can re-enter and commit a real mutation to
		// the still-live old projection.  Preserve that pending notification in
		// the rollback seed before the identity boundary moves to the candidate;
		// notifications raised after this point belong to the tentative view and
		// must disappear if it fails.
		previousAccessibilityState.StructureChangePending =
			previousAccessibilityState.StructureChangePending
			|| live->_accessibilityStructureChangePending;
		live->_source = value;
		if (++live->_cellSelectionRevision == 0)
			live->_cellSelectionRevision = 1;
		live->_itemsView = candidate;
		live->_displayItemsView = candidateDisplay;
		// Candidate population is extensible and may have rebuilt the old view's
		// UIA table after the transaction's initial invalidation.  Switching the
		// live view is therefore an independent identity boundary: invalidate at
		// the assignment point before reflection, snapshotting, or UIA can observe
		// candidate rows through locators belonging to the old projection.
		live->InvalidateItemOccurrenceCache();
		live->InvalidateColumnContentWidthCache();
		// Row provider identities belong to the retired occurrence domain. Keep
		// column identities stable, but never ask the replacement view to remap old
		// row tokens (an absent selected/current record would otherwise scan every
		// lazy row on the next UIA query). The transaction snapshot above restores
		// the complete table if candidate preparation rolls back.
		live->_accessibilityRows.clear();
		live->_accessibilityRowIndexLookup.clear();
		live->_accessibilityNodeLookup.clear();
		// Auto-generated definitions must exist before a visible viewport asks
		// the base generator to compose its first row.  Keep this inside the
		// transaction because reflection or allocation may throw midway.
		live->EnsureAutoGeneratedColumns();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		live->ListBox::SetItemsSource(BindingListReference(candidateDisplay));
	}
	catch (...)
	{
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live)
		{
			if (live->ListBox::GetItemsSource().Shared() != candidateDisplay)
				rollbackDerivedState(*live);
			live->RefreshSelectedCellContainersAfterRollback();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (live)
			{
				live->_settingItemsSource = false;
				// Preserve the original transaction failure if an accessibility
				// client itself fails while observing the restored old projection.
				try { live->FlushAccessibilityStructureChange(); }
				catch (...) {}
			}
		}
		throw;
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (live->ListBox::GetItemsSource().Shared() != candidateDisplay)
	{
		rollbackDerivedState(*live);
		live->RefreshSelectedCellContainersAfterRollback();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live)
		{
			live->_settingItemsSource = false;
			live->FlushAccessibilityStructureChange();
		}
		return;
	}
	if (clearSortState)
		for (auto& column : live->_columns)
			column->_sortDirection.reset();
	// A replacement source may deliberately reuse the same item at the same
	// viewport locator (for example while replacing a reflected auto schema).
	// Preserve the WPF item + authored-column CurrentCell identity with one
	// bounded probe; never scan a lazy replacement merely to find an old item.
	if (!live->_currentCell.IsValid() && previousCurrentCell.IsValid())
	{
		const auto column = std::find_if(live->_columns.begin(), live->_columns.end(),
			[&](const auto& candidate)
			{ return candidate.get() == previousCurrentCell.Column; });
		const auto items = live->GetItemsView();
		if (column != live->_columns.end() && items)
		{
			const size_t columnIndex = static_cast<size_t>(
				std::distance(live->_columns.begin(), column));
			const size_t count = items.Get()->Count();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return;
			if (live->GetItemsView().Shared() == items.Shared()
				&& columnIndex < live->_columns.size()
				&& live->_columns[columnIndex].get() == previousCurrentCell.Column
				&& previousCurrentCell.RowIndex < count)
			{
				BindingSourceReference itemAtLocator;
				const bool read = items.Get()->TryGetItem(
					previousCurrentCell.RowIndex, itemAtLocator);
				live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (!live) return;
				if (read && itemAtLocator
					&& live->GetItemsView().Shared() == items.Shared()
					&& columnIndex < live->_columns.size()
					&& live->_columns[columnIndex].get()
						== previousCurrentCell.Column
					&& itemAtLocator.Shared()
						== previousCurrentCell.Item.Shared())
				{
					DataGridCellInfo restored;
					const bool created = live->TryCreateCellInfo(
						previousCurrentCell.RowIndex, columnIndex, restored);
					live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
					if (!live) return;
					if (created && !live->_currentCell.IsValid()
						&& live->GetItemsView().Shared() == items.Shared()
						&& columnIndex < live->_columns.size()
						&& live->_columns[columnIndex].get()
							== previousCurrentCell.Column)
						live->_currentCell = std::move(restored);
				}
			}
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
	}
	// ItemsSource replacement is an identity boundary.  CurrentCell belongs to
	// the old view and otherwise stays invalid instead of scanning the replacement
	// for the same object reference. This keeps a selected row from materializing
	// a lazy million-row source merely because VisibleWhenSelected showed details.
	if (!live->ReconcileCurrentCellColumn()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->ReconcileSelectedCells()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->SynchronizeSelectedCellsFromRows()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	// WPF's CurrentCell equality is item + column. Replacing ItemsSource may
	// assign a new occurrence token to the same object, but that internal token
	// refresh is not a logical CurrentCell change and must not raise the event.
	if (live->_currentCellChangeDeferred
		&& live->_deferredCurrentCellOld.IsValid()
		&& live->_currentCell.IsValid()
		&& live->_deferredCurrentCellOld.Item.Shared()
			== live->_currentCell.Item.Shared()
		&& live->_deferredCurrentCellOld.Column == live->_currentCell.Column)
	{
		// A replacement view can assign a fresh source occurrence token to the
		// same WPF item/column identity. Align only that private token so the
		// deferred projection can still publish locator changes without raising a
		// false logical CurrentCellChanged event.
		live->_deferredCurrentCellOld._itemOccurrence =
			live->_currentCell._itemOccurrence;
	}
	if (std::isnan(live->_rowHeaderWidth))
	{
		live->InvalidateRowHeaderWidthBaseline();
		(void)live->SetCurrentRowHeaderActualWidth(0.0);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->RefreshRowHeaderActualWidth()) return;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
	}
	live->_rowHeaderSourceChanged =
		std::move(candidateRowHeaderSourceChanged);
	for (auto& snapshot : previousRuntimeAutoColumns)
		if (snapshot.Column)
		{
			snapshot.Column->_owner = nullptr;
			snapshot.Column->_visibleIndex =
				DataGridColumn::UnsetDisplayIndex;
		}
	live->_autoColumnsChangedDuringPreparation = true;
	live->_settingItemsSource = false;
	(void)live->CoerceValue(CanUserAddRowsProperty());
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	(void)live->CoerceValue(CanUserDeleteRowsProperty());
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->RefreshNewItemPlaceholder();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->FlushCommittedItemsSourceState();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->FlushAccessibilityStructureChange();
}

void DataGrid::SetAutoGenerateColumns(bool value)
{
	(void)TrySetPropertyValue(
		AutoGenerateColumnsProperty(), BindingValue(value));
}

void DataGrid::SetIsReadOnly(bool value)
{
	(void)TrySetPropertyValue(IsReadOnlyProperty(), BindingValue(value));
}

void DataGrid::SetCanUserAddRows(bool value)
{
	(void)TrySetPropertyValue(
		CanUserAddRowsProperty(), BindingValue(value));
}

void DataGrid::SetCanUserDeleteRows(bool value)
{
	(void)TrySetPropertyValue(
		CanUserDeleteRowsProperty(), BindingValue(value));
}

void DataGrid::SetCanUserSortColumns(bool value)
{
	(void)TrySetPropertyValue(
		CanUserSortColumnsProperty(), BindingValue(value));
}

void DataGrid::SetCanUserResizeColumns(bool value)
{
	(void)TrySetPropertyValue(
		CanUserResizeColumnsProperty(), BindingValue(value));
}

void DataGrid::SetCanUserResizeRows(bool value)
{
	(void)TrySetPropertyValue(
		CanUserResizeRowsProperty(), BindingValue(value));
}

void DataGrid::SetEnableColumnVirtualization(bool value)
{
	(void)TrySetPropertyValue(
		EnableColumnVirtualizationProperty(), BindingValue(value));
}

void DataGrid::SetCanUserReorderColumns(bool value)
{
	(void)TrySetPropertyValue(
		CanUserReorderColumnsProperty(), BindingValue(value));
}

void DataGrid::SetFrozenColumnCount(int value)
{
	if (value < 0)
		throw std::invalid_argument(
			"DataGrid.FrozenColumnCount cannot be negative");
	(void)TrySetPropertyValue(
		FrozenColumnCountProperty(), BindingValue(value));
}

void DataGrid::SetSelectionUnit(DataGridSelectionUnit value)
{
	(void)TrySetPropertyValue(
		SelectionUnitProperty(), BindingValue(static_cast<int>(value)));
}

void DataGrid::SetColumnWidth(DataGridLength value)
{
	if (!value.IsValid())
		throw std::invalid_argument("DataGrid.ColumnWidth is invalid");
	(void)SetDependencyPropertyValue(ColumnWidthProperty(), value);
}

void DataGrid::SetMinColumnWidth(double value)
{
	if (!std::isfinite(value) || value < 0.0)
		throw std::invalid_argument("DataGrid.MinColumnWidth is invalid");
	(void)SetDependencyPropertyValue(MinColumnWidthProperty(), value);
}

void DataGrid::SetMaxColumnWidth(double value)
{
	if (std::isnan(value) || value < 0.0)
		throw std::invalid_argument("DataGrid.MaxColumnWidth is invalid");
	(void)SetDependencyPropertyValue(MaxColumnWidthProperty(), value);
}

void DataGrid::SetClipboardCopyMode(DataGridClipboardCopyMode value)
{
	(void)TrySetPropertyValue(
		ClipboardCopyModeProperty(), BindingValue(static_cast<int>(value)));
}

void DataGrid::SetCellStyle(std::wstring value)
{
	(void)SetDependencyPropertyValue(CellStyleProperty(), std::move(value));
}

void DataGrid::SetColumnHeaderStyle(std::wstring value)
{
	(void)SetDependencyPropertyValue(
		ColumnHeaderStyleProperty(), std::move(value));
}

void DataGrid::SetRowStyle(std::wstring value)
{
	if (!value.empty() && _rowStyleSelector)
		throw std::invalid_argument(
			"DataGrid.RowStyle and RowStyleSelector are mutually exclusive");
	(void)SetDependencyPropertyValue(RowStyleProperty(), std::move(value));
}

void DataGrid::SetRowStyleSelector(ItemStyleSelectorReference value)
{
	if (value && !_rowStyle.empty())
		throw std::invalid_argument(
			"DataGrid.RowStyleSelector and RowStyle are mutually exclusive");
	(void)SetDependencyPropertyValue(
		RowStyleSelectorProperty(), std::move(value));
}

void DataGrid::SetRowHeaderStyle(std::wstring value)
{
	(void)SetDependencyPropertyValue(
		RowHeaderStyleProperty(), std::move(value));
}

void DataGrid::SetRowHeaderTemplate(ItemTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		RowHeaderTemplateProperty(), std::move(value));
}

void DataGrid::SetRowHeaderTemplateSelector(
	ItemTemplateSelectorReference value)
{
	(void)SetDependencyPropertyValue(
		RowHeaderTemplateSelectorProperty(), std::move(value));
}

void DataGrid::SetAreRowDetailsFrozen(bool value)
{
	(void)TrySetPropertyValue(
		AreRowDetailsFrozenProperty(), BindingValue(value));
}

void DataGrid::SetRowDetailsVisibilityMode(
	DataGridRowDetailsVisibilityMode value)
{
	(void)TrySetPropertyValue(RowDetailsVisibilityModeProperty(),
		BindingValue(static_cast<int>(value)));
}

void DataGrid::SetRowDetailsTemplate(ItemTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		RowDetailsTemplateProperty(), std::move(value));
}

void DataGrid::SetRowDetailsTemplateSelector(
	ItemTemplateSelectorReference value)
{
	(void)SetDependencyPropertyValue(
		RowDetailsTemplateSelectorProperty(), std::move(value));
}

void DataGrid::SetColumnHeaderHeight(double value)
{
	(void)TrySetPropertyValue(
		ColumnHeaderHeightProperty(), BindingValue(value));
}

void DataGrid::SetRowHeaderWidth(double value)
{
	(void)TrySetPropertyValue(
		RowHeaderWidthProperty(), BindingValue(value));
}

bool DataGrid::SetCurrentRowHeaderActualWidth(double value)
{
	if (!std::isfinite(value) || value < 0.0) return false;
	return SetReadOnlyPropertyField(
		RowHeaderActualWidthPropertyKey(), _rowHeaderActualWidth, value);
}

void DataGrid::InvalidateRowHeaderWidthBaseline() noexcept
{
	if (++_rowHeaderWidthEpoch == 0) ++_rowHeaderWidthEpoch;
}

bool DataGrid::TryShareRowHeaderDesiredWidth(
	DataGridRowHeader& header,
	double desiredWidth,
	double& sharedWidth)
{
	sharedWidth = desiredWidth;
	if (!std::isfinite(desiredWidth) || desiredWidth < 0.0
		|| !std::isnan(_rowHeaderWidth)
		|| !HasRowHeaders(_headersVisibility)
		|| _settingItemsSource || IsItemsSourceUpdateInProgress()) return false;
	auto* row = header.GetRowOwner();
	if (!row || row->GetDataGridOwner() != this) return false;
	bool attached = false;
	for (auto* current = static_cast<Control*>(&header); current;
		current = current->GetVisualParent())
	{
		if (current->GetVisibility() == Visibility::Collapsed) return false;
		if (current == this)
		{
			attached = true;
			break;
		}
	}
	if (!attached) return false;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference headerLifetime(&header);
	if (desiredWidth > _rowHeaderActualWidth + 0.0001)
		(void)SetCurrentRowHeaderActualWidth(desiredWidth);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveHeader = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	if (!live || !liveHeader || !std::isnan(live->_rowHeaderWidth)
		|| !HasRowHeaders(live->_headersVisibility)
		|| live->_settingItemsSource || live->IsItemsSourceUpdateInProgress())
		return false;
	row = liveHeader->GetRowOwner();
	if (!row || row->GetDataGridOwner() != live) return false;
	sharedWidth = live->_rowHeaderActualWidth;
	return true;
}

void DataGrid::SetRowHeight(double value)
{
	(void)TrySetPropertyValue(RowHeightProperty(), BindingValue(value));
}

void DataGrid::SetMinRowHeight(double value)
{
	(void)TrySetPropertyValue(MinRowHeightProperty(), BindingValue(value));
}

void DataGrid::SetHeadersVisibility(DataGridHeadersVisibility value)
{
	(void)TrySetPropertyValue(
		HeadersVisibilityProperty(), BindingValue(static_cast<int>(value)));
}

void DataGrid::SetGridLinesVisibility(DataGridGridLinesVisibility value)
{
	(void)TrySetPropertyValue(
		GridLinesVisibilityProperty(), BindingValue(static_cast<int>(value)));
}

void DataGrid::SetRowBackground(cui::drawing::Brush value)
{
	(void)TrySetPropertyValue(
		RowBackgroundProperty(), BindingValue(std::move(value)));
}

void DataGrid::SetAlternatingRowBackground(cui::drawing::Brush value)
{
	(void)TrySetPropertyValue(
		AlternatingRowBackgroundProperty(), BindingValue(std::move(value)));
}

void DataGrid::SetHorizontalGridLinesBrush(cui::drawing::Brush value)
{
	(void)TrySetPropertyValue(
		HorizontalGridLinesBrushProperty(), BindingValue(std::move(value)));
}

void DataGrid::SetVerticalGridLinesBrush(cui::drawing::Brush value)
{
	(void)TrySetPropertyValue(
		VerticalGridLinesBrushProperty(), BindingValue(std::move(value)));
}

DataGridColumn* DataGrid::AddColumn(
	std::unique_ptr<DataGridColumn> column)
{
	return AddColumnCore(std::move(column), false);
}

void DataGrid::SetRowValidationErrorTemplate(
	ControlTemplateReference value)
{
	(void)SetDependencyPropertyValue(
		RowValidationErrorTemplateProperty(), std::move(value));
}

DataGridColumn* DataGrid::InsertColumn(
	size_t logicalIndex, std::unique_ptr<DataGridColumn> column)
{
	return AddColumnCore(std::move(column), false, logicalIndex);
}

DataGridColumn* DataGrid::AddAutoGeneratedColumn(
	std::unique_ptr<DataGridColumn> column)
{
	return AddColumnCore(std::move(column), true);
}

DataGridColumn* DataGrid::AdoptColumn(DataGridColumn* column)
{
	return column ? AddColumn(std::unique_ptr<DataGridColumn>(column)) : nullptr;
}

DataGridColumn* DataGrid::AddColumnCore(
	std::unique_ptr<DataGridColumn> column, bool autoGenerated,
	size_t logicalIndex)
{
	if (!column) return nullptr;
	if (logicalIndex == DataGridCellInfo::InvalidIndex)
		logicalIndex = _logicalColumns.size();
	if (logicalIndex > _logicalColumns.size())
		throw std::out_of_range(
			"DataGrid column insertion index is outside the collection");
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	const ControlWeakReference ownerLifetime(this);
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing() && !CancelEdit()) return nullptr;
	if (!ownerLifetime.Get()) return nullptr;
	if (column->_owner)
		throw std::logic_error("DataGridColumn already has an owner");
	const size_t displayIndex = column->_displayIndex ==
		DataGridColumn::UnsetDisplayIndex
		? logicalIndex
		: (std::min)(column->_displayIndex, _columns.size());
	// Complete every potentially-throwing capacity change before publishing the
	// owner. All projections are then committed without another allocation.
	const size_t oldColumnCount = _columns.size();
	const bool preserveContentSamples =
		_sampledColumnContentWidths.size() == oldColumnCount;
	_columns.reserve(_columns.size() + 1);
	_logicalColumns.reserve(_logicalColumns.size() + 1);
	_visibleColumns.reserve(_columns.size() + 1);
	if (preserveContentSamples)
		_sampledColumnContentWidths.reserve(oldColumnCount + 1);
	if (column->_accessibilityIdentity == 0)
		column->_accessibilityIdentity = AllocateAccessibilityVirtualId();
	column->_owner = this;
	column->_isAutoGenerated = autoGenerated;
	column->_isRuntimeAutoGenerated = false;
	auto* raw = column.get();
	_columns.push_back(std::move(column));
	_logicalColumns.insert(
		_logicalColumns.begin() + logicalIndex, raw);
	if (displayIndex + 1 < _columns.size())
		std::rotate(
			_columns.begin() + displayIndex,
			_columns.end() - 1,
			_columns.end());
	if (preserveContentSamples)
		_sampledColumnContentWidths.insert(
			_sampledColumnContentWidths.begin() + displayIndex,
			std::nullopt);
	ReindexDisplayColumns(displayIndex);
	RefreshColumnDisplayOrder();
	return ownerLifetime.Get() ? raw : nullptr;
}

std::unique_ptr<DataGridColumn> DataGrid::RemoveColumn(
	size_t logicalIndex)
{
	if (logicalIndex >= _logicalColumns.size()) return {};
	auto* const target = _logicalColumns[logicalIndex];
	if (!target || target->_owner != this
		|| target->_displayIndex >= _columns.size()
		|| _columns[target->_displayIndex].get() != target) return {};

	const ControlWeakReference ownerLifetime(this);
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing() && !CancelEdit()) return {};
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || logicalIndex >= live->_logicalColumns.size()
		|| live->_logicalColumns[logicalIndex] != target
		|| target->_owner != live
		|| target->_displayIndex >= live->_columns.size()
		|| live->_columns[target->_displayIndex].get() != target) return {};

	const size_t displayIndex = target->_displayIndex;
	const bool preserveContentSamples =
		live->_sampledColumnContentWidths.size() == live->_columns.size();
	auto removed = std::move(live->_columns[displayIndex]);
	live->_columns.erase(live->_columns.begin() + displayIndex);
	live->_logicalColumns.erase(
		live->_logicalColumns.begin() + logicalIndex);
	if (preserveContentSamples)
		live->_sampledColumnContentWidths.erase(
			live->_sampledColumnContentWidths.begin() + displayIndex);
	removed->_owner = nullptr;
	removed->_displayIndex = DataGridColumn::UnsetDisplayIndex;
	removed->_visibleIndex = DataGridColumn::UnsetDisplayIndex;
	removed->_sortDirection.reset();
	live->ReindexDisplayColumns(displayIndex);
	live->PruneAccessibilityColumnIdentities();
	// Preserve the removed definition until CurrentCellChanged and
	// SelectedCellsChanged observers have inspected the old identities.
	if (!live->ReconcileCurrentCellColumn()) return removed;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return removed;
	if (!live->ReconcileSelectedCells()) return removed;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return removed;
	live->RefreshColumnDisplayOrder();
	return removed;
}

std::unique_ptr<DataGridColumn> DataGrid::RemoveColumn(
	DataGridColumn& column)
{
	const auto found = std::find(
		_logicalColumns.begin(), _logicalColumns.end(), &column);
	return found == _logicalColumns.end()
		? std::unique_ptr<DataGridColumn>{}
		: RemoveColumn(static_cast<size_t>(
			std::distance(_logicalColumns.begin(), found)));
}

void DataGrid::ClearColumns()
{
	_runtimeAutoGenerationComplete = false;
	_autoGeneratedColumnsEventPending = false;
	if (_columns.empty()) return;
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	const ControlWeakReference ownerLifetime(this);
	(void)CancelEdit();
	if (!ownerLifetime.Get()) return;
	_selectionAnchor.reset();
	ResetSelectionRange();
	if (!ApplySelectedCells({})) return;
	auto removed = std::move(_columns);
	_logicalColumns.clear();
	_visibleColumns.clear();
	PruneAccessibilityColumnIdentities();
	for (auto& column : removed)
	{
		column->_owner = nullptr;
		column->_displayIndex = DataGridColumn::UnsetDisplayIndex;
		column->_visibleIndex = DataGridColumn::UnsetDisplayIndex;
		column->_sortDirection.reset();
	}
	const auto previous = _currentCell;
	_currentCell.Column = nullptr;
	_currentCell.ColumnIndex = DataGridCellInfo::InvalidIndex;
	// Keep removed definitions alive while observers inspect OldCell.Column.
	if (!RaiseCurrentCellChanged(previous)) return;
	RefreshColumns();
}

DataGridColumn* DataGrid::GetColumn(size_t index) const noexcept
{
	return index < _logicalColumns.size() ? _logicalColumns[index] : nullptr;
}

DataGridColumn* DataGrid::GetColumnFromDisplayIndex(size_t index) const noexcept
{
	return index < _columns.size() ? _columns[index].get() : nullptr;
}

bool DataGrid::IsColumnReorderEligible(
	const DataGridColumn& column) const noexcept
{
	return _canUserReorderColumns && column._canUserReorder
		&& column._owner == this
		&& column._visibility == Visibility::Visible
		&& column._displayIndex < _columns.size()
		&& _columns[column._displayIndex].get() == &column;
}

void DataGrid::CancelColumnReorderForEligibilityChange(
	const DataGridColumn* column)
{
	auto* presenter = GetColumnHeadersPresenter();
	if (!presenter || !presenter->_isColumnHeaderDragging
		|| (column && presenter->_reorderingColumn != column)) return;
	presenter->FinishColumnHeaderDrag(true);
}

void DataGrid::ReindexDisplayColumns(size_t begin) noexcept
{
	begin = (std::min)(begin, _columns.size());
	for (size_t index = begin; index < _columns.size(); ++index)
		_columns[index]->_displayIndex = index;
	RebuildVisibleColumnProjection();
}

void DataGrid::RebuildVisibleColumnProjection() noexcept
{
	_visibleColumns.clear();
	for (const auto& column : _columns)
	{
		column->_visibleIndex = DataGridColumn::UnsetDisplayIndex;
		if (column->_visibility != Visibility::Visible) continue;
		column->_visibleIndex = _visibleColumns.size();
		_visibleColumns.push_back(column.get());
	}
}

DataGridColumn* DataGrid::GetColumnFromVisibleIndex(
	size_t index) const noexcept
{
	return index < _visibleColumns.size() ? _visibleColumns[index] : nullptr;
}

size_t DataGrid::VisibleIndexFromDisplayIndex(
	size_t displayIndex) const noexcept
{
	return displayIndex < _columns.size()
		? _columns[displayIndex]->_visibleIndex
		: DataGridColumn::UnsetDisplayIndex;
}

size_t DataGrid::FirstVisibleDisplayIndex() const noexcept
{
	return _visibleColumns.empty()
		? DataGridColumn::UnsetDisplayIndex
		: _visibleColumns.front()->_displayIndex;
}

size_t DataGrid::LastVisibleDisplayIndex() const noexcept
{
	return _visibleColumns.empty()
		? DataGridColumn::UnsetDisplayIndex
		: _visibleColumns.back()->_displayIndex;
}

bool DataGrid::SetColumnVisibility(
	DataGridColumn& column, ::Visibility value)
{
	if (value != Visibility::Visible && value != Visibility::Hidden
		&& value != Visibility::Collapsed) return false;
	if (column._owner != this) return false;
	if (column._visibility == value) return true;
	const bool oldVisible = column._visibility == Visibility::Visible;
	const bool newVisible = value == Visibility::Visible;
	if (oldVisible == newVisible)
	{
		column._visibility = value;
		return true;
	}
	if (_changingColumnVisibility) return false;

	const ControlWeakReference ownerLifetime(this);
	_changingColumnVisibility = true;
	auto resetChanging = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_changingColumnVisibility = false;
	});
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing() && !CancelEdit()) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || column._owner != live
		|| column._displayIndex >= live->_columns.size()
		|| live->_columns[column._displayIndex].get() != &column
		|| column._visibility == value) return false;
	column._visibility = value;
	live->RebuildVisibleColumnProjection();
	live->RefreshColumnDisplayOrder();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->RequestAccessibilityStructureChanged();
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::MoveColumn(
	size_t oldDisplayIndex, size_t newDisplayIndex)
{
	if (oldDisplayIndex >= _columns.size()) return false;
	return SetColumnDisplayIndex(
		*_columns[oldDisplayIndex], newDisplayIndex);
}

bool DataGrid::SetColumnDisplayIndex(
	DataGridColumn& column, size_t displayIndex)
{
	if (_changingColumnDisplayIndex || column._owner != this
		|| displayIndex >= _columns.size()
		|| column._displayIndex >= _columns.size()
		|| _columns[column._displayIndex].get() != &column) return false;
	const size_t oldDisplayIndex = column._displayIndex;
	if (oldDisplayIndex == displayIndex) return true;

	const ControlWeakReference ownerLifetime(this);
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	(void)CancelEdit();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_changingColumnDisplayIndex
		|| column._owner != live || displayIndex >= live->_columns.size()
		|| column._displayIndex >= live->_columns.size()
		|| live->_columns[column._displayIndex].get() != &column) return false;

	const size_t committedOldIndex = column._displayIndex;
	live->_changingColumnDisplayIndex = true;
	const auto resetChanging = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_changingColumnDisplayIndex = false;
	});
	const auto rotateProjection = [committedOldIndex, displayIndex](auto& values)
	{
		if (values.size() == 0) return;
		if (committedOldIndex < displayIndex)
			std::rotate(
				values.begin() + committedOldIndex,
				values.begin() + committedOldIndex + 1,
				values.begin() + displayIndex + 1);
		else std::rotate(
			values.begin() + displayIndex,
			values.begin() + committedOldIndex,
			values.begin() + committedOldIndex + 1);
	};
	rotateProjection(live->_columns);
	if (live->_sampledColumnContentWidths.size() == live->_columns.size())
		rotateProjection(live->_sampledColumnContentWidths);
	live->ReindexDisplayColumns((std::min)(
		committedOldIndex, displayIndex));
	live->RefreshColumnDisplayOrder();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || displayIndex >= live->_columns.size()
		|| live->_columns[displayIndex].get() != &column) return false;
	live->_changingColumnDisplayIndex = false;
	if (live->ColumnDisplayIndexChanged.Empty()) return true;
	DataGridColumnDisplayIndexChangedEventArgs args;
	args.Column = &column;
	args.OldDisplayIndex = committedOldIndex;
	args.NewDisplayIndex = displayIndex;
	cui::framework::EventAccess::RaiseWhile(
		live->ColumnDisplayIndexChanged,
		[&]() { return ownerLifetime.Get() != nullptr; },
		live, args);
	return ownerLifetime.Get() != nullptr;
}

void DataGrid::RemoveAutoGeneratedColumns()
{
	_runtimeAutoGenerationComplete = false;
	_autoGeneratedColumnsEventPending = false;
	const size_t autoCount = static_cast<size_t>(std::count_if(
		_columns.begin(), _columns.end(),
		[](const auto& column) { return column->_isAutoGenerated; }));
	if (autoCount == 0) return;
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	std::vector<std::unique_ptr<DataGridColumn>> retained;
	std::vector<std::unique_ptr<DataGridColumn>> removed;
	retained.reserve(_columns.size() - autoCount);
	removed.reserve(autoCount);
	for (auto& column : _columns)
	{
		if (column->_isAutoGenerated)
		{
			column->_owner = nullptr;
			column->_displayIndex = DataGridColumn::UnsetDisplayIndex;
			column->_visibleIndex = DataGridColumn::UnsetDisplayIndex;
			removed.push_back(std::move(column));
		}
		else retained.push_back(std::move(column));
	}
	_columns = std::move(retained);
	_logicalColumns.erase(std::remove_if(
		_logicalColumns.begin(), _logicalColumns.end(),
		[](const DataGridColumn* column)
		{ return column->_isAutoGenerated; }), _logicalColumns.end());
	ReindexDisplayColumns();
	ReevaluateFrozenColumnCountForSchemaChange();
	PruneAccessibilityColumnIdentities();
	// Keep removed definitions alive while CurrentCellChanged observers inspect
	// the old cell identity.
	if (!ReconcileCurrentCellColumn()) return;
	(void)ReconcileSelectedCells();
}

void DataGrid::EnsureAutoGeneratedColumns()
{
#if CUI_ENABLE_DYNAMIC_XAML
	const ControlWeakReference ownerLifetime(this);
	const auto source = _source;
	if (!_autoGenerateColumns || !source || _runtimeAutoGenerationComplete
		|| _autoGenerationInProgress) return;
	if (std::any_of(_columns.begin(), _columns.end(),
		[](const auto& column) { return column->_isAutoGenerated; }))
	{
		_runtimeAutoGenerationComplete = true;
		return;
	}
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	std::vector<std::pair<DataGridColumn*, uint32_t>> columnSnapshot;
	columnSnapshot.reserve(_columns.size());
	for (const auto& column : _columns)
		columnSnapshot.emplace_back(
			column.get(), column->_accessibilityIdentity);
	const auto resolveCurrent = [&]() -> DataGrid*
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->_autoGenerateColumns
			|| live->_source.Shared() != source.Shared()
			|| live->_columns.size() != columnSnapshot.size()) return nullptr;
		for (size_t index = 0; index < columnSnapshot.size(); ++index)
			if (live->_columns[index].get() != columnSnapshot[index].first
				|| live->_columns[index]->_accessibilityIdentity
					!= columnSnapshot[index].second) return nullptr;
		return live;
	};
	const size_t count = source.Get()->Count();
	auto* live = resolveCurrent();
	if (!live || count == 0) return;
	BindingSourceReference first;
	const bool read = source.Get()->TryGetItem(0, first);
	live = resolveCurrent();
	if (!live || !read || !first) return;
	const auto properties = first.Get()->GetProperties();
	live = resolveCurrent();
	if (!live) return;
	live->_autoGenerationInProgress = true;
	auto resetGeneration = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_autoGenerationInProgress = false;
	});
	const bool customizeColumns = !live->AutoGeneratingColumn.Empty();
	std::vector<std::unique_ptr<DataGridColumn>> generated;
	generated.reserve(properties.size());
	for (const auto& property : properties)
	{
		if (!property.CanRead || property.Name.empty()) continue;
		std::unique_ptr<DataGridColumn> column;
		if (property.ValueKind == BindingValueKind::Bool
			|| property.ValueKind == BindingValueKind::NullableBool)
		{
			auto check = std::make_unique<DataGridCheckBoxColumn>();
			check->SetIsThreeState(
				property.ValueKind == BindingValueKind::NullableBool);
			column = std::move(check);
		}
		else column = std::make_unique<DataGridTextColumn>();
		column->SetHeader(BindingValue(property.Name));
		column->SetSortMemberPath(property.Name);
		column->SetIsReadOnly(!property.CanWrite);
		auto* bound = dynamic_cast<DataGridBoundColumn*>(column.get());
		if (bound) bound->SetBindingPath(property.Name);
		if (customizeColumns)
		{
			DataGridAutoGeneratingColumnEventArgs args;
			args.PropertyName = property.Name;
			args.PropertyKind = property.ValueKind;
			args.PropertyType = property.ValueType;
			args.Column = std::move(column);
			cui::framework::EventAccess::RaiseWhile(
				live->AutoGeneratingColumn,
				[&]() { return resolveCurrent() != nullptr; },
				live, args);
			live = resolveCurrent();
			if (!live) return;
			if (args.Cancel || !args.Column) continue;
			column = std::move(args.Column);
			if (column->_owner)
				throw std::logic_error(
					"AutoGeneratingColumn replacement already has an owner");
		}
		generated.push_back(std::move(column));
	}
	live = resolveCurrent();
	if (!live) return;
	live->_columns.reserve(live->_columns.size() + generated.size());
	live->_logicalColumns.reserve(
		live->_logicalColumns.size() + generated.size());
	live->_visibleColumns.reserve(
		live->_columns.size() + generated.size());
	size_t firstChangedDisplayIndex = live->_columns.size();
	for (auto& column : generated)
	{
		const size_t displayIndex = column->_displayIndex ==
			DataGridColumn::UnsetDisplayIndex
			? live->_columns.size()
			: (std::min)(column->_displayIndex, live->_columns.size());
		column->_accessibilityIdentity = AllocateAccessibilityVirtualId();
		column->_owner = live;
		column->_isAutoGenerated = true;
		column->_isRuntimeAutoGenerated = true;
		auto* raw = column.get();
		live->_columns.push_back(std::move(column));
		live->_logicalColumns.push_back(raw);
		if (displayIndex + 1 < live->_columns.size())
			std::rotate(
				live->_columns.begin() + displayIndex,
				live->_columns.end() - 1,
				live->_columns.end());
		firstChangedDisplayIndex = (std::min)(
			firstChangedDisplayIndex, displayIndex);
	}
	live->ReindexDisplayColumns(firstChangedDisplayIndex);
	live->_runtimeAutoGenerationComplete = true;
	if (!live->AutoGeneratedColumns.Empty())
		live->_autoGeneratedColumnsEventPending = true;
	live->ReevaluateFrozenColumnCountForSchemaChange();
#endif
}

void DataGrid::FlushAutoGeneratedColumnsEvent()
{
	if (!_autoGeneratedColumnsEventPending || _settingItemsSource
		|| IsItemsSourceUpdateInProgress()) return;
	_autoGeneratedColumnsEventPending = false;
	if (AutoGeneratedColumns.Empty()) return;
	const ControlWeakReference ownerLifetime(this);
	cui::framework::EventAccess::RaiseWhile(
		AutoGeneratedColumns,
		[&]() { return ownerLifetime.Get() != nullptr; },
		this);
}

void DataGrid::ReevaluateFrozenColumnCountForSchemaChange()
{
	if (_coercingFrozenColumnCountForSchema) return;
	_coercingFrozenColumnCountForSchema = true;
	const ControlWeakReference ownerLifetime(this);
	const auto reset = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_coercingFrozenColumnCountForSchema = false;
	});
	(void)CoerceValue(FrozenColumnCountProperty());
}

const std::wstring& DataGrid::EffectiveCellStyle(
	const DataGridColumn& column) const noexcept
{
	return column._cellStyle.empty() ? _cellStyle : column._cellStyle;
}

const std::wstring& DataGrid::EffectiveColumnHeaderStyle(
	const DataGridColumn& column) const noexcept
{
	return column._headerStyle.empty()
		? _columnHeaderStyle : column._headerStyle;
}

const std::wstring& DataGrid::EffectiveRowStyle() const noexcept
{
	return _rowStyle.empty() ? GetItemContainerStyle() : _rowStyle;
}

bool DataGrid::TryResolveRowStyle(
	DataGridRow& row, std::wstring& result) const
{
	result.clear();
	if (row.GetDataGridOwner() != this) return false;
	if (!_rowStyle.empty())
	{
		result = _rowStyle;
		return true;
	}
	const auto selector = _rowStyleSelector;
	if (!selector)
	{
		result = GetItemContainerStyle();
		return true;
	}
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const ControlWeakReference rowLifetime(&row);
	result = selector.Get()->SelectStyle(row.GetItem(), row);
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!owner || !liveRow || liveRow->GetDataGridOwner() != owner
		|| owner->_rowStyleSelector != selector
		|| !owner->_rowStyle.empty()) return false;
	if (result.empty()) result = owner->GetItemContainerStyle();
	return true;
}

bool DataGrid::TryResolveRowHeaderTemplate(
	DataGridRow& row, DataGridRowHeader& header,
	ItemTemplateReference& result) const
{
	result = _rowHeaderTemplate;
	if (result || !_rowHeaderTemplateSelector) return true;
	const auto selector = _rowHeaderTemplateSelector;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const ControlWeakReference rowLifetime(&row);
	const ControlWeakReference headerLifetime(&header);
	result = selector.Get()->SelectTemplate(row.GetItem(), header);
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	auto* liveHeader = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
	return owner && liveRow && liveHeader
		&& liveRow->GetDataGridOwner() == owner
		&& liveRow->GetRowHeader() == liveHeader
		&& owner->_rowHeaderTemplateSelector == selector
		&& !owner->_rowHeaderTemplate;
}

void DataGrid::RefreshRealizedCellStyles(const DataGridColumn* column)
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> cells;
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
		if (!row || row->GetDataGridOwner() != this) continue;
		for (auto* cell : row->GetCells())
			if (cell && (!column || cell->GetColumn() == column))
				cells.emplace_back(cell);
	}
	for (const auto& lifetime : cells)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* cell = dynamic_cast<DataGridCell*>(lifetime.Get());
		if (!owner || !cell) return;
		auto* row = cell->GetRowOwner();
		auto* liveColumn = cell->GetColumn();
		if (!row || row->GetDataGridOwner() != owner || !liveColumn
			|| liveColumn->GetDataGridOwner() != owner
			|| (column && liveColumn != column)) continue;
		cui::framework::StyleAccess::SetResourceKey(
			*cell, owner->EffectiveCellStyle(*liveColumn));
	}
}

void DataGrid::PublishColumnHeaderPresentationError(
	DataGridColumnHeader& source, std::wstring error)
{
	if (source._owner != this || source._ownerLifetime.Get() != this) return;
	_columnHeaderPresentationErrorSource = &source;
	_columnHeaderPresentationErrorText = std::move(error);
	SetLastTemplateError(_columnHeaderPresentationErrorText);
}

void DataGrid::ClearColumnHeaderPresentationError(
	DataGridColumnHeader& source) noexcept
{
	if (_columnHeaderPresentationErrorSource.Get() != &source) return;
	if (!_columnHeaderPresentationErrorText.empty()
		&& LastTemplateError() == _columnHeaderPresentationErrorText)
		SetLastTemplateError({});
	_columnHeaderPresentationErrorSource = nullptr;
	_columnHeaderPresentationErrorText.clear();
}

void DataGrid::RefreshRealizedColumnHeaderPresentation(
	const DataGridColumn* column)
{
	struct RealizedHeader final
	{
		size_t Index = DataGridCellInfo::InvalidIndex;
		ControlWeakReference Lifetime;
	};
	const ControlWeakReference ownerLifetime(this);
	std::vector<RealizedHeader> headers;
	auto* presenter = GetColumnHeadersPresenter();
	if (!presenter) return;
	if (presenter->GetDataGridOwner() != this)
	{
		// The template PART remains registered even when its first transactional
		// Initialize failed. A later column template/style change gets one full
		// retry before attempting an in-place realized-header refresh.
		RefreshHeaderPresenter();
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!owner) return;
		presenter = owner->GetColumnHeadersPresenter();
		if (!presenter || presenter->GetDataGridOwner() != owner) return;
	}
	const ControlWeakReference presenterLifetime(presenter);
	auto refreshGeneration = ++presenter->_presentationRefreshGeneration;
	if (refreshGeneration == 0)
		refreshGeneration = ++presenter->_presentationRefreshGeneration;
	for (size_t index = 0; index < ColumnCount(); ++index)
	{
		auto* liveColumn = GetColumnFromDisplayIndex(index);
		if (!liveColumn || (column && liveColumn != column)) continue;
		if (auto* header = presenter->GetHeader(index))
			headers.push_back({ index, ControlWeakReference(header) });
	}
	for (const auto& realized : headers)
	{
		auto resolveCurrent = [&]() -> std::tuple<
			DataGrid*, DataGridColumnHeadersPresenter*,
			DataGridColumnHeader*, DataGridColumn*>
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* header = dynamic_cast<DataGridColumnHeader*>(
				realized.Lifetime.Get());
			if (!owner || !header) return {};
			auto* currentPresenter = owner->GetColumnHeadersPresenter();
			auto* transactionPresenter = dynamic_cast<
				DataGridColumnHeadersPresenter*>(presenterLifetime.Get());
			if (!currentPresenter
				|| currentPresenter != transactionPresenter
				|| currentPresenter->_presentationRefreshGeneration
					!= refreshGeneration
				|| currentPresenter->GetHeader(realized.Index) != header)
				return {};
			auto* ownedColumn = owner->GetColumnFromDisplayIndex(realized.Index);
			if (!ownedColumn || header->_column != ownedColumn
				|| header->_columnIndex != realized.Index
				|| (column && ownedColumn != column)) return {};
			return { owner, currentPresenter, header, ownedColumn };
		};
		auto [owner, currentPresenter, header, liveColumn] = resolveCurrent();
		(void)currentPresenter;
		if (!owner || !header || !liveColumn) return;
		cui::framework::StyleAccess::SetResourceKey(
			*header, owner->EffectiveColumnHeaderStyle(*liveColumn));
		std::tie(owner, currentPresenter, header, liveColumn) = resolveCurrent();
		if (!owner || !header || !liveColumn) return;
		const auto previousHeaderError = header->ContentPresentationError();
		const auto contentTemplate = liveColumn->GetHeaderTemplate();
		if (header->GetContentTemplate() == contentTemplate)
		{
			// A failed build can still leave the DP at the requested template while
			// the previous visual remains active. Calling the same-value setter would
			// clear its diagnostic and then no-op, falsely reporting recovery.
			if (!previousHeaderError.empty())
			{
				if (owner->_columnHeaderPresentationErrorSource.Get() == header)
				{
					if (owner->LastTemplateError().empty()
						&& !owner->_columnHeaderPresentationErrorText.empty())
						owner->SetLastTemplateError(
							owner->_columnHeaderPresentationErrorText);
				}
				else if (owner->LastTemplateError().empty())
					owner->PublishColumnHeaderPresentationError(*header,
						L"DataGrid 列标题模板更新失败："
							+ previousHeaderError);
				return;
			}
			continue;
		}
		header->SetContentTemplate(contentTemplate);
		std::tie(owner, currentPresenter, header, liveColumn) = resolveCurrent();
		if (!owner || !header || !liveColumn) return;
		// A nested property change superseded this refresh. Its own refresh owns
		// the result and any error publication.
		if (liveColumn->GetHeaderTemplate() != contentTemplate) return;
		const auto error = header->ContentPresentationError();
		if (!error.empty())
		{
			owner->PublishColumnHeaderPresentationError(*header,
				L"DataGrid 列标题模板更新失败：" + error);
			return;
		}
		owner->ClearColumnHeaderPresentationError(*header);
	}
}

void DataGrid::RefreshRealizedRowHeaderPresentation()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> headers;
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
		if (row && row->GetDataGridOwner() == this && row->GetRowHeader())
			headers.emplace_back(row->GetRowHeader());
	}
	for (const auto& lifetime : headers)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* header = dynamic_cast<DataGridRowHeader*>(lifetime.Get());
		if (!owner || !header) return;
		auto* row = header->GetRowOwner();
		if (!row || row->GetDataGridOwner() != owner) continue;
		cui::framework::StyleAccess::SetResourceKey(
			*header, owner->_rowHeaderStyle);
		header = dynamic_cast<DataGridRowHeader*>(lifetime.Get());
		owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!owner || !header) return;
		row = header->GetRowOwner();
		if (!row || row->GetDataGridOwner() != owner) continue;
		if (!row->UpdateRowHeaderPresentation()) return;
	}
}

void DataGrid::RefreshRealizedRowDetails()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
			row && row->GetDataGridOwner() == this)
			rows.emplace_back(row);
	}
	for (const auto& lifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(lifetime.Get());
		if (!owner || !row) return;
		if (row->GetDataGridOwner() != owner) continue;
		{
			row->_restoringDetailsOverrides = true;
			auto restore = MakeScopeExit([lifetime]
			{
				if (auto* current = dynamic_cast<DataGridRow*>(lifetime.Get()))
					current->_restoringDetailsOverrides = false;
			});
			if (!row->CoerceValue(DataGridRow::DetailsTemplateProperty())
				|| !row->CoerceValue(
					DataGridRow::DetailsTemplateSelectorProperty())
				|| !row->CoerceValue(
					DataGridRow::DetailsVisibilityProperty())) return;
		}
		row = dynamic_cast<DataGridRow*>(lifetime.Get());
		owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!row || !owner || row->GetDataGridOwner() != owner) return;
		std::wstring error;
		if (!row->UpdateDetailsPresentation(&error))
		{
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (owner && !error.empty()) owner->SetLastTemplateError(error);
			return;
		}
	}
}

void DataGrid::RefreshRealizedRowDetailsHorizontalAlignment()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
			row && row->GetDataGridOwner() == this)
			rows.emplace_back(row);
	}
	const double offset = _horizontalScrollOffset;
	for (const auto& lifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(lifetime.Get());
		if (!owner || !row) return;
		if (row->GetDataGridOwner() == owner)
			row->UpdateDetailsHorizontalScrollOffset(offset);
	}
}

bool DataGrid::RaiseRowLifecycleEvent(
	Event<void(DataGrid*, DataGridRowEventArgs&)>& event,
	DataGridRow& row)
{
	if (event.Empty()) return true;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(&row);
	DataGridRowEventArgs args;
	args.Row = &row;
	cui::framework::EventAccess::RaiseWhile(
		event,
		[&]
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			return owner && liveRow
				&& liveRow->GetDataGridOwner() == owner;
		}, this, args);
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	return owner && liveRow && liveRow->GetDataGridOwner() == owner;
}

bool DataGrid::RaiseRowDetailsEvent(
	Event<void(DataGrid*, DataGridRowDetailsEventArgs&)>& event,
	DataGridRow& row, Control& detailsElement)
{
	if (event.Empty()) return true;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(&row);
	const ControlWeakReference detailsLifetime(&detailsElement);
	DataGridRowDetailsEventArgs args;
	args.Row = &row;
	args.DetailsElement = &detailsElement;
	cui::framework::EventAccess::RaiseWhile(
		event,
		[&]
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			auto* details = detailsLifetime.Get();
			return owner && liveRow && details
				&& liveRow->GetDataGridOwner() == owner
				&& liveRow->GetDetailsElement() == details;
		}, this, args);
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	auto* details = detailsLifetime.Get();
	return owner && liveRow && details
		&& liveRow->GetDataGridOwner() == owner
		&& liveRow->GetDetailsElement() == details;
}

bool DataGrid::SynchronizeRealizedRowLifecycle()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
			row && row->GetDataGridOwner() == this)
			rows.emplace_back(row);
	}
	const auto previouslyLoaded = _lifecycleLoadedRows;
	_lifecycleLoadedRows = rows;
	std::unordered_set<Control*> currentRows;
	currentRows.reserve(rows.size());
	for (const auto& row : rows)
		if (auto* value = row.Get()) currentRows.insert(value);
	std::unordered_set<Control*> previousRows;
	previousRows.reserve(previouslyLoaded.size());
	for (const auto& row : previouslyLoaded)
		if (auto* value = row.Get()) previousRows.insert(value);
	for (const auto& previous : previouslyLoaded)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!owner || owner->_lifecycleLoadedRows != rows) return false;
		if (currentRows.contains(previous.Get())) continue;
		if (auto* retired = dynamic_cast<DataGridRow*>(previous.Get()))
			if (!owner->RaiseRowLifecycleEvent(owner->UnloadingRow, *retired))
				return false;
	}
	for (const auto& current : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!owner || owner->_lifecycleLoadedRows != rows) return false;
		if (previousRows.contains(current.Get())) continue;
		if (auto* loaded = dynamic_cast<DataGridRow*>(current.Get()))
			if (!owner->RaiseRowLifecycleEvent(owner->LoadingRow, *loaded))
				return false;
	}
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::ApplyItemContainerStyle()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
			row && row->GetDataGridOwner() == this)
			rows.emplace_back(row);
	}
	for (const auto& lifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(lifetime.Get());
		if (!owner || !row) return false;
		if (row->GetDataGridOwner() != owner) continue;
		std::wstring style;
		if (!owner->TryResolveRowStyle(*row, style)) return false;
		owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(lifetime.Get());
		if (!owner || !row || row->GetDataGridOwner() != owner) return false;
		cui::framework::StyleAccess::SetResourceKey(
			*row, style);
		if (!ownerLifetime.Get()) return false;
	}
	return ownerLifetime.Get() != nullptr;
}

void DataGrid::RefreshColumns()
{
	const ControlWeakReference ownerLifetime(this);
	(void)CancelEdit();
	if (!ownerLifetime.Get()) return;
	ReevaluateFrozenColumnCountForSchemaChange();
	if (!ownerLifetime.Get()) return;
	InvalidateRealizedColumnRange();
	InvalidateAccessibilityVirtualIdentities();
	InvalidateColumnContentWidthCache();
	(void)RebuildGeneratedItems();
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		live->RefreshHeaderPresenter();
}

void DataGrid::RefreshColumnDisplayOrder()
{
	// Display reordering does not change a column's binding or content. Preserve
	// the per-column sampled widths (already rotated with the projection) and
	// invalidate only order-dependent width/prefix and realized visual state.
	const ControlWeakReference ownerLifetime(this);
	ReevaluateFrozenColumnCountForSchemaChange();
	if (!ownerLifetime.Get()) return;
	InvalidateRealizedColumnRange();
	InvalidateAccessibilityVirtualIdentities();
	InvalidateColumnWidthCache();
	_columnWidthRefreshPending = false;
	_columnWidthMeasureDirty.clear();
	_columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
	_columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
	++_columnContentWidthCacheEpoch;
	if (_columnContentWidthCacheEpoch == 0)
		_columnContentWidthCacheEpoch = 1;

	const bool previous = _preserveColumnContentWidthsDuringRowRebuild;
	_preserveColumnContentWidthsDuringRowRebuild = true;
	const auto restore = MakeScopeExit([ownerLifetime, previous]
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_preserveColumnContentWidthsDuringRowRebuild = previous;
	});
	(void)RebuildGeneratedItems();
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		live->RefreshHeaderPresenter();
}

void DataGrid::InvalidateRows()
{
	// Appearance/row-height changes rebuild realized containers, but neither the
	// source projection nor a column binding changed.  Preserve the expensive
	// Auto/SizeToCells sample while keeping the existing regeneration contract.
	// A source transaction that re-enters preparation remains authoritative and
	// ignores this guard in OnBeforeGeneratedItemsPrepared.
	const ControlWeakReference ownerLifetime(this);
	const bool previous = _preserveColumnContentWidthsDuringRowRebuild;
	_preserveColumnContentWidthsDuringRowRebuild = true;
	auto restore = MakeScopeExit([ownerLifetime, previous]
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_preserveColumnContentWidthsDuringRowRebuild = previous;
	});
	(void)RebuildGeneratedItems();
}

double DataGrid::EffectiveDefaultRowHeight() const noexcept
{
	return std::isfinite(_rowHeight)
		? (std::max)(_rowHeight, _minRowHeight)
		: (std::numeric_limits<double>::quiet_NaN)();
}

bool DataGrid::TryGetProjectedRowHeight(
	size_t rowIndex, double& height) const noexcept
{
	height = 0.0;
	const auto found = std::lower_bound(
		_virtualRowHeightOverrides.begin(),
		_virtualRowHeightOverrides.end(), rowIndex,
		[](const VirtualizedItemExtentOverride& item, size_t index)
		{ return item.ItemIndex < index; });
	if (found == _virtualRowHeightOverrides.end()
		|| found->ItemIndex != rowIndex) return false;
	height = found->Extent;
	return std::isfinite(height) && height > 0.0;
}

bool DataGrid::RefreshRowHeightOverrideProjection()
{
	const ControlWeakReference ownerLifetime(this);
	const auto items = GetItemsView();
	std::vector<VirtualizedItemExtentOverride> projection;
	projection.reserve(_rowHeightOverrides.size() + (_rowResizeActive ? 1 : 0));
	for (const auto& item : _rowHeightOverrides)
	{
		if (_rowResizeActive
			&& item.Item.Shared() == _rowResizeItem.Shared()
			&& item.Occurrence == _rowResizeOccurrence) continue;
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		if (!TryResolveItemOccurrence(item.Item, item.Occurrence, rowIndex))
			continue;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
		projection.push_back({ rowIndex,
			(std::max)(item.Height, live->_minRowHeight) });
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
	if (live->_rowResizeActive
		&& live->_rowResizeIndex != DataGridCellInfo::InvalidIndex)
		projection.push_back({ live->_rowResizeIndex,
			(std::max)(live->_rowResizeHeight, live->_minRowHeight) });
	std::sort(projection.begin(), projection.end(),
		[](const auto& left, const auto& right)
		{ return left.ItemIndex < right.ItemIndex; });
	projection.erase(std::unique(projection.begin(), projection.end(),
		[](const auto& left, const auto& right)
		{ return left.ItemIndex == right.ItemIndex; }), projection.end());
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
	live->_virtualRowHeightOverrides = std::move(projection);
	if (++live->_virtualRowHeightOverrideRevision == 0)
		++live->_virtualRowHeightOverrideRevision;
	live->RefreshVirtualScrollMetrics();
	return ownerLifetime.Get() != nullptr;
}

void DataGrid::PruneRowHeightOverrides()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<RowHeightOverride> retained;
	retained.reserve(_rowHeightOverrides.size());
	for (const auto& item : _rowHeightOverrides)
	{
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		if (TryResolveItemOccurrence(item.Item, item.Occurrence, rowIndex))
			retained.push_back(item);
		if (!ownerLifetime.Get()) return;
	}
	_rowHeightOverrides = std::move(retained);
	(void)RefreshRowHeightOverrideProjection();
}

bool DataGrid::PersistRowPresentationOverride(DataGridRow& row)
{
	if (row.GetDataGridOwner() != this) return false;
	// The synthetic new-item row has no source occurrence to persist against.
	// Its local values remain valid for that live container, but must never
	// create an entry which could later leak into a recycled data row.
	if (!row.GetItem()
		|| row.ItemIndex() == DataGridCellInfo::InvalidIndex
		|| IsNewItemPlaceholder(row.GetItem())) return true;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(&row);
	const auto item = row.GetItem();
	const size_t rowIndex = row.ItemIndex();
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!owner || !liveRow || liveRow->GetDataGridOwner() != owner
		|| liveRow->GetItem().Shared() != item.Shared()
		|| liveRow->ItemIndex() != rowIndex) return false;

	RowPresentationOverride next;
	next.Item = item;
	next.Occurrence = occurrence;
	BindingValue raw;
	next.HasDetailsTemplate = liveRow->HasPropertyValue(
		DataGridRow::DetailsTemplateProperty(),
		DependencyPropertyValueSource::Local);
	if (next.HasDetailsTemplate
		&& (!liveRow->TryGetPropertyValue(
			DataGridRow::DetailsTemplateProperty(),
			DependencyPropertyValueSource::Local, raw)
			|| !raw.TryGet(next.DetailsTemplate))) return false;
	raw = {};
	next.HasDetailsTemplateSelector = liveRow->HasPropertyValue(
		DataGridRow::DetailsTemplateSelectorProperty(),
		DependencyPropertyValueSource::Local);
	if (next.HasDetailsTemplateSelector
		&& (!liveRow->TryGetPropertyValue(
			DataGridRow::DetailsTemplateSelectorProperty(),
			DependencyPropertyValueSource::Local, raw)
			|| !raw.TryGet(next.DetailsTemplateSelector))) return false;
	raw = {};
	next.HasDetailsVisibility = liveRow->HasPropertyValue(
		DataGridRow::DetailsVisibilityProperty(),
		DependencyPropertyValueSource::Local);
	if (next.HasDetailsVisibility)
	{
		int value = static_cast<int>(::Visibility::Collapsed);
		if (!liveRow->TryGetPropertyValue(
			DataGridRow::DetailsVisibilityProperty(),
			DependencyPropertyValueSource::Local, raw)
			|| !raw.TryGet(value)) return false;
		next.DetailsVisibility = static_cast<::Visibility>(value);
	}
	owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!owner || !liveRow || liveRow->GetDataGridOwner() != owner
		|| liveRow->GetItem().Shared() != item.Shared()
		|| liveRow->ItemIndex() != rowIndex) return false;
	auto found = std::find_if(owner->_rowPresentationOverrides.begin(),
		owner->_rowPresentationOverrides.end(),
		[&](const RowPresentationOverride& candidate)
		{
			return candidate.Item.Shared() == item.Shared()
				&& candidate.Occurrence == occurrence;
		});
	const bool hasAny = next.HasDetailsTemplate
		|| next.HasDetailsTemplateSelector || next.HasDetailsVisibility;
	if (!hasAny)
	{
		if (found != owner->_rowPresentationOverrides.end())
			owner->_rowPresentationOverrides.erase(found);
		return true;
	}
	if (found == owner->_rowPresentationOverrides.end())
		owner->_rowPresentationOverrides.push_back(std::move(next));
	else *found = std::move(next);
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::RestoreRowPresentationOverride(DataGridRow& row)
{
	if (row.GetDataGridOwner() != this || !row.GetItem()
		|| row.ItemIndex() == DataGridCellInfo::InvalidIndex) return false;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(&row);
	const auto item = row.GetItem();
	const size_t rowIndex = row.ItemIndex();
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	const bool canPersist = !IsNewItemPlaceholder(item);
	if (canPersist && !TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!owner || !liveRow || liveRow->GetDataGridOwner() != owner
		|| liveRow->GetItem().Shared() != item.Shared()
		|| liveRow->ItemIndex() != rowIndex) return false;
	RowPresentationOverride selected;
	const auto found = canPersist
		? std::find_if(owner->_rowPresentationOverrides.begin(),
			owner->_rowPresentationOverrides.end(),
			[&](const RowPresentationOverride& candidate)
			{
				return candidate.Item.Shared() == item.Shared()
					&& candidate.Occurrence == occurrence;
			})
		: owner->_rowPresentationOverrides.end();
	if (found != owner->_rowPresentationOverrides.end()) selected = *found;

	liveRow->_restoringDetailsOverrides = true;
	auto restore = MakeScopeExit([rowLifetime]
	{
		if (auto* current = dynamic_cast<DataGridRow*>(rowLifetime.Get()))
			current->_restoringDetailsOverrides = false;
	});
	if (liveRow->HasPropertyValue(DataGridRow::DetailsTemplateProperty(),
		DependencyPropertyValueSource::Local))
		(void)liveRow->ClearPropertyValue(
			DataGridRow::DetailsTemplateProperty());
	if (liveRow->HasPropertyValue(
		DataGridRow::DetailsTemplateSelectorProperty(),
		DependencyPropertyValueSource::Local))
		(void)liveRow->ClearPropertyValue(
			DataGridRow::DetailsTemplateSelectorProperty());
	if (liveRow->HasPropertyValue(DataGridRow::DetailsVisibilityProperty(),
		DependencyPropertyValueSource::Local))
		(void)liveRow->ClearPropertyValue(
			DataGridRow::DetailsVisibilityProperty());
	if (selected.HasDetailsTemplate)
		liveRow->SetDetailsTemplate(selected.DetailsTemplate);
	if (selected.HasDetailsTemplateSelector)
		liveRow->SetDetailsTemplateSelector(
			selected.DetailsTemplateSelector);
	if (selected.HasDetailsVisibility)
		liveRow->SetDetailsVisibility(selected.DetailsVisibility);
	if (!liveRow->CoerceValue(DataGridRow::DetailsTemplateProperty())
		|| !liveRow->CoerceValue(
			DataGridRow::DetailsTemplateSelectorProperty())
		|| !liveRow->CoerceValue(DataGridRow::DetailsVisibilityProperty()))
		return false;
	return ownerLifetime.Get() != nullptr && rowLifetime.Get() != nullptr;
}

void DataGrid::PruneRowPresentationOverrides()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<RowPresentationOverride> retained;
	retained.reserve(_rowPresentationOverrides.size());
	for (const auto& item : _rowPresentationOverrides)
	{
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		if (TryResolveItemOccurrence(item.Item, item.Occurrence, rowIndex))
			retained.push_back(item);
		if (!ownerLifetime.Get()) return;
	}
	_rowPresentationOverrides = std::move(retained);
}

void DataGrid::RefreshRealizedRowHeights()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner) return;
		if (!row || row->GetDataGridOwner() != owner) continue;
		row->UpdateHeightPresentation();
	}
}

void DataGrid::ApplyRowHeightToRealizedItem(size_t rowIndex)
{
	if (auto* row = ResolveRow(rowIndex))
	{
		row->UpdateHeightPresentation();
		row->RequestLayout();
	}
	RequestLayout();
	InvalidateVisual();
}

bool DataGrid::SetRowHeightOverride(size_t rowIndex, double height)
{
	if (!_canUserResizeRows || _rowResizeActive
		|| !std::isfinite(height) || height <= 0.0) return false;
	const auto items = GetItemsView();
	BindingSourceReference item;
	if (!items || rowIndex >= items.Get()->Count()
		|| !items.Get()->TryGetItem(rowIndex, item) || !item
		|| IsNewItemPlaceholder(item)) return false;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	if (auto* row = ResolveRow(rowIndex))
	{
		height = (std::max)(height, static_cast<double>(_minRowHeight));
		if (auto* header = row->GetRowHeader())
			height = (std::max)(height,
				static_cast<double>(header->GetDesiredSizeDip().height));
		if (std::isfinite(row->GetMaxHeight()))
			height = (std::min)(height,
				static_cast<double>(row->GetMaxHeight()));
	}
	else height = (std::max)(height, _minRowHeight);
	auto found = std::find_if(_rowHeightOverrides.begin(),
		_rowHeightOverrides.end(), [&](const RowHeightOverride& candidate)
		{
			return candidate.Item.Shared() == item.Shared()
				&& candidate.Occurrence == occurrence;
		});
	if (found == _rowHeightOverrides.end())
		_rowHeightOverrides.push_back({ item, occurrence, height });
	else found->Height = height;
	const ControlWeakReference ownerLifetime(this);
	if (!RefreshRowHeightOverrideProjection()) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->ApplyRowHeightToRealizedItem(rowIndex);
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::ClearRowHeightOverride(size_t rowIndex)
{
	if (!_canUserResizeRows || _rowResizeActive) return false;
	const auto items = GetItemsView();
	BindingSourceReference item;
	if (!items || rowIndex >= items.Get()->Count()
		|| !items.Get()->TryGetItem(rowIndex, item) || !item
		|| IsNewItemPlaceholder(item)) return false;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	_rowHeightOverrides.erase(std::remove_if(
		_rowHeightOverrides.begin(), _rowHeightOverrides.end(),
		[&](const RowHeightOverride& candidate)
		{
			return candidate.Item.Shared() == item.Shared()
				&& candidate.Occurrence == occurrence;
		}), _rowHeightOverrides.end());
	const ControlWeakReference ownerLifetime(this);
	if (!RefreshRowHeightOverrideProjection()) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->ApplyRowHeightToRealizedItem(rowIndex);
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::BeginRowResizeTransaction(
	size_t rowIndex, double& startHeight)
{
	startHeight = 0.0;
	if (!_canUserResizeRows || _rowResizeActive) return false;
	auto* row = ResolveRow(rowIndex);
	const auto items = GetItemsView();
	BindingSourceReference item;
	if (!row || row->GetDataGridOwner() != this || !row->_cellsGrid
		|| !items || rowIndex >= items.Get()->Count()
		|| !items.Get()->TryGetItem(rowIndex, item) || !item
		|| IsNewItemPlaceholder(item)) return false;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	startHeight = row->_cellsGrid->GetActualSizeDip().height;
	if (!std::isfinite(startHeight) || startHeight <= 0.0)
	{
		if (!TryGetProjectedRowHeight(rowIndex, startHeight))
		{
			startHeight = EffectiveDefaultRowHeight();
			if (!std::isfinite(startHeight) || startHeight <= 0.0)
				startHeight = (std::max)(
					static_cast<double>(GetVirtualizedItemHeight()), _minRowHeight);
		}
	}
	startHeight = (std::max)(startHeight, _minRowHeight);
	if (auto* header = row->GetRowHeader())
		startHeight = (std::max)(startHeight,
			static_cast<double>(header->GetDesiredSizeDip().height));
	_rowHeightOverrides.reserve(_rowHeightOverrides.size() + 1);
	_virtualRowHeightOverrides.reserve(_rowHeightOverrides.size() + 1);
	_rowResizeItem = item;
	_rowResizeOccurrence = occurrence;
	_rowResizeIndex = rowIndex;
	_rowResizeHeight = startHeight;
	_rowResizeActive = true;
	if (RefreshRowHeightOverrideProjection()) return true;
	_rowResizeActive = false;
	_rowResizeItem = {};
	_rowResizeOccurrence = DataGridCellInfo::InvalidIndex;
	_rowResizeIndex = DataGridCellInfo::InvalidIndex;
	_rowResizeHeight = 0.0;
	return false;
}

bool DataGrid::ResizeRowInTransaction(double pixelHeight)
{
	if (!_rowResizeActive || !_canUserResizeRows
		|| !std::isfinite(pixelHeight)) return false;
	if (GetPresentationWindow() && !_applyingPendingRowResizeInput)
	{
		_rowResizePendingHeight = pixelHeight;
		// Layout requests are coalesced by the Window. This guarantees the latest
		// height is consumed by PrepareMeasureCore before the next presented frame
		// without rebuilding sparse extent metadata for every pointer message.
		RequestLayout();
		return true;
	}
	_rowResizePendingHeight =
		(std::numeric_limits<double>::quiet_NaN)();
	const auto items = GetItemsView();
	BindingSourceReference item;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!items || _rowResizeIndex >= items.Get()->Count()
		|| !items.Get()->TryGetItem(_rowResizeIndex, item) || !item
		|| item.Shared() != _rowResizeItem.Shared()
		|| !TryGetItemOccurrenceAt(_rowResizeIndex, occurrence)
		|| occurrence != _rowResizeOccurrence) return false;
	auto* row = ResolveRow(_rowResizeIndex);
	if (!row || row->GetDataGridOwner() != this) return false;
	pixelHeight = (std::max)(pixelHeight, _minRowHeight);
	if (auto* header = row->GetRowHeader())
		pixelHeight = (std::max)(pixelHeight,
			static_cast<double>(header->GetDesiredSizeDip().height));
	if (std::isfinite(row->GetMaxHeight()))
		pixelHeight = (std::min)(pixelHeight,
			static_cast<double>(row->GetMaxHeight()));
	_rowResizeHeight = pixelHeight;
	const auto projected = std::lower_bound(
		_virtualRowHeightOverrides.begin(),
		_virtualRowHeightOverrides.end(), _rowResizeIndex,
		[](const VirtualizedItemExtentOverride& candidate, size_t index)
		{ return candidate.ItemIndex < index; });
	if (projected == _virtualRowHeightOverrides.end()
		|| projected->ItemIndex != _rowResizeIndex) return false;
	projected->Extent = pixelHeight;
	if (++_virtualRowHeightOverrideRevision == 0)
		++_virtualRowHeightOverrideRevision;
	if (!TryUpdateVirtualizedItemExtentOverride(
		_rowResizeIndex, pixelHeight))
		RefreshVirtualScrollMetrics();
	ApplyRowHeightToRealizedItem(_rowResizeIndex);
	return true;
}

bool DataGrid::ApplyPendingRowResizeInput()
{
	if (!std::isfinite(_rowResizePendingHeight)) return true;
	if (!_rowResizeActive) return false;
	const double height = std::exchange(
		_rowResizePendingHeight,
		(std::numeric_limits<double>::quiet_NaN)());
	const bool previous = _applyingPendingRowResizeInput;
	_applyingPendingRowResizeInput = true;
	const ControlWeakReference ownerLifetime(this);
	auto restore = MakeScopeExit([ownerLifetime, previous]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_applyingPendingRowResizeInput = previous;
	});
	const bool applied = ResizeRowInTransaction(height);
	return applied && ownerLifetime.Get() != nullptr;
}

void DataGrid::EndRowResizeTransaction(bool cancel)
{
	if (!_rowResizeActive) return;
	if (cancel) _rowResizePendingHeight =
		(std::numeric_limits<double>::quiet_NaN)();
	const auto item = _rowResizeItem;
	const size_t occurrence = _rowResizeOccurrence;
	const size_t rowIndex = _rowResizeIndex;
	const double height = _rowResizeHeight;
	if (!cancel)
	{
		auto found = std::find_if(_rowHeightOverrides.begin(),
			_rowHeightOverrides.end(), [&](const RowHeightOverride& candidate)
			{
				return candidate.Item.Shared() == item.Shared()
					&& candidate.Occurrence == occurrence;
			});
		if (found == _rowHeightOverrides.end())
			_rowHeightOverrides.push_back({ item, occurrence, height });
		else found->Height = height;
	}
	_rowResizeActive = false;
	_rowResizeItem = {};
	_rowResizeOccurrence = DataGridCellInfo::InvalidIndex;
	_rowResizeIndex = DataGridCellInfo::InvalidIndex;
	_rowResizeHeight = 0.0;
	_rowResizePendingHeight =
		(std::numeric_limits<double>::quiet_NaN)();
	const ControlWeakReference ownerLifetime(this);
	(void)RefreshRowHeightOverrideProjection();
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		live->ApplyRowHeightToRealizedItem(rowIndex);
}

void DataGrid::RefreshRealizedRowValidationStates(bool coerceTemplate)
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner) return;
		if (!row || row->GetDataGridOwner() != owner) continue;
		if (coerceTemplate)
		{
			(void)row->CoerceValue(
				DataGridRow::ValidationErrorTemplateProperty());
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!owner) return;
			if (!row || row->GetDataGridOwner() != owner) continue;
		}
		(void)row->RefreshValidationState();
	}
}

void DataGrid::RefreshHeaderPresenter()
{
	const ControlWeakReference ownerLifetime(this);
	auto* presenter = GetColumnHeadersPresenter();
	if (!presenter) return;
	std::wstring error;
	const bool initialized = presenter->Initialize(*this, &error);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (!initialized)
	{
		live->_columnHeaderPresenterInitializationError = error.empty()
			? L"DataGrid 列标题刷新失败。" : error;
		live->SetLastTemplateError(
			live->_columnHeaderPresenterInitializationError);
	}
	else
	{
		if (!live->_columnHeaderPresenterInitializationError.empty()
			&& live->LastTemplateError()
				== live->_columnHeaderPresenterInitializationError)
			live->SetLastTemplateError({});
		live->_columnHeaderPresenterInitializationError.clear();
	}
	live->RefreshHeadersVisibility();
}

bool DataGrid::RefreshRowHeaderActualWidthProjection()
{
	const ControlWeakReference ownerLifetime(this);
	const double width = _rowHeaderActualWidth;
	if (auto* selectAll = GetSelectAllButton())
	{
		selectAll->SetWidth(cui::layout::Length::Fixed(
			static_cast<float>(width)));
		if (!ownerLifetime.Get()) return false;
	}

	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	std::vector<ControlWeakReference> headers;
	headers.reserve(live->GetRealizedItems().size());
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		auto* row = dynamic_cast<DataGridRow*>(live->GetGeneratedItem(index));
		auto* header = row ? row->GetRowHeader() : nullptr;
		if (header) headers.emplace_back(header);
	}
	for (const auto& headerLifetime : headers)
	{
		auto* header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		auto* row = header ? header->GetRowOwner() : nullptr;
		if (!row || row->GetDataGridOwner() != live) continue;
		header->InvalidateSharedWidthMeasure();
		if (!ownerLifetime.Get()) return false;
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->RefreshVirtualScrollMetrics();
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::RefreshRowHeaderActualWidth()
{
	if (!std::isnan(_rowHeaderWidth)
		|| !HasRowHeaders(_headersVisibility)
		|| GetVisibility() == Visibility::Collapsed) return true;
	const ControlWeakReference ownerLifetime(this);
	const size_t baselineEpoch = _rowHeaderWidthEpoch;
	const auto participatesInMeasure = [](Control* header,
		DataGrid* owner) noexcept
	{
		for (auto* current = header; current;
			current = current->GetVisualParent())
		{
			if (current->GetVisibility() == Visibility::Collapsed) return false;
			if (current == owner) return true;
		}
		return false;
	};
	std::vector<ControlWeakReference> headers;
	headers.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
		auto* header = row ? row->GetRowHeader() : nullptr;
		if (header && participatesInMeasure(header, this))
			headers.emplace_back(header);
	}
	double widest = _rowHeaderActualWidth;
	for (const auto& headerLifetime : headers)
	{
		auto* header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
		if (!header || header->GetVisibility() == Visibility::Collapsed) continue;
		const auto desired = header->Measure(cui::core::Constraints::Unbounded());
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		if (!std::isnan(live->_rowHeaderWidth)
			|| !HasRowHeaders(live->_headersVisibility)
			|| live->_rowHeaderWidthEpoch != baselineEpoch)
			return true;
		header = dynamic_cast<DataGridRowHeader*>(headerLifetime.Get());
		if (!header || !participatesInMeasure(header, live)) continue;
		widest = (std::max)(widest, static_cast<double>(desired.width));
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!std::isnan(live->_rowHeaderWidth)
		|| !HasRowHeaders(live->_headersVisibility)
		|| live->_rowHeaderWidthEpoch != baselineEpoch) return true;
	if (widest > live->_rowHeaderActualWidth + 0.0001)
	{
		(void)live->SetCurrentRowHeaderActualWidth(widest);
		return ownerLifetime.Get() != nullptr;
	}
	return true;
}

double DataGrid::ResolveRowHeaderWidth() const noexcept
{
	return _rowHeaderActualWidth;
}

void DataGrid::RefreshHeadersVisibility()
{
	const ControlWeakReference ownerLifetime(this);
	const auto headersVisibility = _headersVisibility;
	const double columnHeaderHeight = _columnHeaderHeight;
	if (auto* presenter = GetColumnHeadersPresenter())
	{
		const ControlWeakReference presenterLifetime(presenter);
		presenter->SetVisibility(HasColumnHeaders(headersVisibility)
			? Visibility::Visible : Visibility::Collapsed);
		if (!ownerLifetime.Get()) return;
		presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			presenterLifetime.Get());
		if (!presenter) return;
		if (std::isfinite(columnHeaderHeight))
			presenter->SetHeight(cui::layout::Length::Fixed(
				static_cast<float>(columnHeaderHeight)));
		else presenter->SetHeight(cui::layout::Length::Auto());
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (auto* selectAll = live->GetSelectAllButton())
	{
		const ControlWeakReference selectAllLifetime(selectAll);
		const bool cornerVisible =
			live->_headersVisibility == DataGridHeadersVisibility::All;
		const double rowHeaderWidth = live->ResolveRowHeaderWidth();
		const double liveColumnHeaderHeight = live->_columnHeaderHeight;
		const bool selectAllEnabled =
			live->GetSelectionMode() == SelectionMode::Extended;
		selectAll->SetVisibility(
			cornerVisible ? Visibility::Visible : Visibility::Collapsed);
		if (!ownerLifetime.Get()) return;
		selectAll = dynamic_cast<Button*>(selectAllLifetime.Get());
		if (!selectAll) return;
		selectAll->SetWidth(cui::layout::Length::Fixed(
			static_cast<float>(rowHeaderWidth)));
		if (!ownerLifetime.Get()) return;
		selectAll = dynamic_cast<Button*>(selectAllLifetime.Get());
		if (!selectAll) return;
		if (std::isfinite(liveColumnHeaderHeight))
			selectAll->SetHeight(cui::layout::Length::Fixed(
				static_cast<float>(liveColumnHeaderHeight)));
		else selectAll->SetHeight(cui::layout::Length::Auto());
		if (!ownerLifetime.Get()) return;
		selectAll = dynamic_cast<Button*>(selectAllLifetime.Get());
		if (!selectAll) return;
		selectAll->SetIsEnabled(selectAllEnabled);
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::vector<ControlWeakReference> rows;
	rows.reserve(live->GetRealizedItems().size());
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(
			live->GetGeneratedItem(index))) rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (row && row->GetDataGridOwner() == live) row->UpdateRowHeader();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
	}
	live->RefreshHorizontalScrollAlignment();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->RefreshVirtualScrollMetrics();
}

void DataGrid::RefreshHorizontalScrollAlignment()
{
	const ControlWeakReference ownerLifetime(this);
	if (auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get()))
		_horizontalScrollOffset = std::isfinite(scroll->GetHorizontalOffset())
			? scroll->GetHorizontalOffset() : 0.0;
	else _horizontalScrollOffset = 0.0;
	RefreshRealizedColumns();
	auto* liveAfterVirtualization = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!liveAfterVirtualization) return;
	const double horizontalOffset = _horizontalScrollOffset;
	if (auto* presenter = GetColumnHeadersPresenter())
	{
		presenter->SetRenderTransform(
			HorizontalTranslation(-horizontalOffset));
		presenter->UpdateHorizontalScrollOffset(horizontalOffset);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::vector<ControlWeakReference> rows;
	rows.reserve(live->GetRealizedItems().size());
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(live->GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (row && row->GetDataGridOwner() == live)
			row->UpdateHorizontalScrollOffset(horizontalOffset);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
	}
}

bool DataGrid::HandleRowHeaderClick(
	DataGridRow& row, ModifierKeys modifiers)
{
	if (row.GetDataGridOwner() != this) return false;
	size_t rowIndex = row.ItemIndex();
	if (rowIndex >= ItemCount()) return false;
	const auto targetView = GetItemsView();
	const BindingSourceReference targetItem = row.GetItem();
	size_t targetOccurrence = DataGridCellInfo::InvalidIndex;
	if (!TryGetItemOccurrenceAt(rowIndex, targetOccurrence)) return false;
	const ControlWeakReference ownerLifetime(this);
	size_t focusColumn = 0;
	auto resolveTarget = [&]() -> DataGridRow*
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != targetView.Shared()
			|| !live->TryResolveItemOccurrence(
				targetItem, targetOccurrence, rowIndex)) return nullptr;
		return live->ResolveRow(rowIndex);
	};
	if (!_visibleColumns.empty())
	{
		focusColumn = FirstVisibleDisplayIndex();
		const bool currentRow = _currentCell.IsValid()
			&& _currentCell.Item.Shared() == targetItem.Shared()
			&& _currentCell._itemOccurrence == targetOccurrence;
		if (currentRow)
		{
			if (_currentCell.Column
				&& _currentCell.Column->_visibleIndex
					!= DataGridColumn::UnsetDisplayIndex)
				focusColumn = _currentCell.Column->_displayIndex;
			if (auto* current = ResolveCurrentCellContainer();
				current && current->GetIsEditing()) (void)CommitEdit();
			if (!resolveTarget()) return false;
		}
		else if (!SetCurrentCell(rowIndex, focusColumn)) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* liveRow = resolveTarget();
		if (!live || !liveRow) return false;
		if (auto* cell = liveRow->GetCell(focusColumn))
			if (auto* window = live->GetPresentationWindow())
				window->SetKeyboardFocus(cell, true);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (live->_selectionUnit == DataGridSelectionUnit::Cell) return true;
	if (!resolveTarget()) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	const bool replaceCellSelection = live->_selectionUnit
		== DataGridSelectionUnit::CellOrRowHeader
		&& !HasModifier(modifiers, ModifierKeys::Control)
		&& !HasModifier(modifiers, ModifierKeys::Shift);
	const auto previousRows = live->GetSelectedIndices();
	const bool previousReplacement = live->_replaceCellSelectionFromRows;
	if (replaceCellSelection) live->_replaceCellSelectionFromRows = true;
	auto restoreReplacement = MakeScopeExit(
		[ownerLifetime, previousReplacement]
		{
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				owner->_replaceCellSelectionFromRows = previousReplacement;
		});
	live->NotifyItemClicked(rowIndex, MouseButton::Left, modifiers);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	// SelectOnly is a no-op when the row was already the sole selected row.
	// WPF still clears independent CellOrRowHeader cells on a plain row-header
	// click, so perform the same synchronization when no row event ran.
	if (replaceCellSelection && live->GetSelectedIndices() == previousRows
		&& !live->SynchronizeSelectedCellsFromRows()) return false;
	return true;
}

void DataGrid::HandleSelectAll()
{
	if (GetSelectionMode() != SelectionMode::Extended
		|| ItemCount() == 0 || _visibleColumns.empty()) return;
	if (_selectionUnit == DataGridSelectionUnit::Cell) SelectAllCells();
	else ListBox::SelectAll();
}

void DataGrid::OnControlTemplatePresentationChanged()
{
	_selectAllClick.Disconnect();
	_dataGridScrollChanged.Disconnect();
	RetireColumnHeadersPresenterNoCallbacks(
		dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get()));
	_headersPresenter.Reset();
	_selectAllButton.Reset();
	_scrollViewer.Reset();
	_horizontalScrollOffset = 0.0;
	_horizontalScrollAlignmentPending = false;
	_columnViewportWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	InvalidateColumnWidthCache();
	const ControlWeakReference ownerLifetime(this);
	ListBox::OnControlTemplatePresentationChanged();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (!live->GetControlTemplateRoot()) return;
	auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		live->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_ColumnHeadersPresenter")));
	if (!presenter)
	{
		live->SetLastTemplateError(
			L"DataGrid ControlTemplate 必须包含 PART_ColumnHeadersPresenter。");
		return;
	}
	const ControlWeakReference presenterLifetime(presenter);
	live->_headersPresenter = presenter;
	std::wstring error;
	const bool headersInitialized = presenter->Initialize(*live, &error);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		presenterLifetime.Get());
	if (!live || !presenter) return;
	if (!headersInitialized)
	{
		live->_columnHeaderPresenterInitializationError = error.empty()
			? L"DataGrid 列标题初始化失败。" : std::move(error);
		live->SetLastTemplateError(
			live->_columnHeaderPresenterInitializationError);
	}
	else
	{
		if (!live->_columnHeaderPresenterInitializationError.empty()
			&& live->LastTemplateError()
				== live->_columnHeaderPresenterInitializationError)
			live->SetLastTemplateError({});
		live->_columnHeaderPresenterInitializationError.clear();
	}
	if (auto* selectAll = dynamic_cast<Button*>(
		live->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_SelectAllButton"))))
	{
		live->_selectAllButton = selectAll;
		live->_selectAllClick = selectAll->Click.Subscribe(
			[ownerLifetime](Control*, RoutedEventArgs& args)
			{
				args.Handled = true;
				if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
					owner->HandleSelectAll();
			});
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (auto* scroll = dynamic_cast<ScrollViewer*>(
		live->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_ScrollViewer"))))
	{
		live->_scrollViewer = scroll;
		live->_dataGridScrollChanged = scroll->OnScrollChanged.Subscribe(
			[ownerLifetime](Control* sender, ScrollChangedEventArgs& args)
			{
				auto* activeScroll = dynamic_cast<ScrollViewer*>(sender);
				if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				{
					constexpr double epsilon = 0.000001;
					const bool horizontalChanged =
						!std::isfinite(owner->_horizontalScrollOffset)
						|| std::abs(args.HorizontalOffset
							- owner->_horizontalScrollOffset) > epsilon;
					const double resolvedViewportWidth =
						owner->ResolveColumnViewportWidth(args.ViewportWidth);
					const bool viewportWidthChanged =
						!std::isfinite(owner->_columnViewportWidth)
						|| std::abs(resolvedViewportWidth
							- owner->_columnViewportWidth) > epsilon;
					if (horizontalChanged)
						owner->_horizontalScrollOffset = args.HorizontalOffset;
					if (viewportWidthChanged)
						owner->UpdateColumnViewportWidth(resolvedViewportWidth);
					owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
					if (!owner) return;
					if (viewportWidthChanged
						&& !owner->TryCommitViewportColumnLayoutLocally())
						owner->RequestLayout();
					owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
					if (!owner) return;
					if (horizontalChanged)
					{
						if (auto* window = owner->GetPresentationWindow();
							window && activeScroll
							&& activeScroll->_lastScrollChangeWasInteractive)
						{
							owner->_horizontalScrollAlignmentPending = true;
							// Column realization changes topology; commit it before
							// the frame's scene snapshot instead of mid-traversal.
							window->RequestPresentationPreparation(*owner);
						}
						else owner->RefreshHorizontalScrollAlignment();
					}
				}
			});
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->RefreshHeadersVisibility();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) (void)live->RefreshRowHeaderActualWidth();
}

std::unique_ptr<DataGridColumnHeadersPresenter>
DataGrid::CreateColumnHeadersPresenter()
{
	const ControlWeakReference ownerLifetime(this);
	auto presenter = std::make_unique<DataGridColumnHeadersPresenter>();
	std::wstring error;
	const bool initialized = presenter->Initialize(*this, &error);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return {};
	if (!initialized)
	{
		live->SetLastTemplateError(error.empty()
			? L"DataGrid 列标题创建失败。" : error);
		return {};
	}
	live->_headersPresenter = ControlWeakReference(presenter.get());
	live->RefreshHeadersVisibility();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_headersPresenter.Get() != presenter.get()) return {};
	return presenter;
}

std::unique_ptr<Control> DataGrid::BuildGeneratedItem(
	const BindingSourceReference& item,
	size_t index,
	BindingPathObservation& observation)
{
	observation = {};
	const ControlWeakReference ownerLifetime(this);
	auto row = std::make_unique<DataGridRow>();
	// WPF style selectors receive a prepared container identity before the row
	// visual tree is built. Expose only the stable owner/item/index tuple here;
	// Initialize below remains the atomic visual commit.
	row->_ownerLifetime = this;
	row->_item = item;
	row->SetItemIndex(index);
	std::wstring rowStyle;
	if (!TryResolveRowStyle(*row, rowStyle)) return {};
	cui::framework::StyleAccess::SetResourceKey(*row, rowStyle);
	std::wstring error;
	if (!row->Initialize(*this, item, index, &error))
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->SetLastTemplateError(std::move(error));
		return {};
	}
	if (!ownerLifetime.Get()) return {};
	return row;
}

namespace
{
	/** Set while an attached row is being moved by TryRebindRealizedItemInPlace. */
	thread_local const DataGrid* DataGridAttachedRebindOwner = nullptr;
}

bool DataGrid::CanRebindRealizedItemInPlace(
	const Control& visual, size_t oldIndex) const noexcept
{
	const auto* row = dynamic_cast<const DataGridRow*>(&visual);
	// A moved row that was under the pointer is corrected by the Window's
	// post-preparation mouse-over resynchronization, exactly like a row that a
	// scroll carries away from a stationary pointer.
	if (!row || !row->GetVisualParent()) return false;
	const auto* previous = std::exchange(DataGridAttachedRebindOwner, this);
	const bool compatible = CanRecycleGeneratedItemAcrossIndices(
		visual, oldIndex);
	DataGridAttachedRebindOwner = previous;
	return compatible;
}

bool DataGrid::TryRebindRealizedItemInPlace(
	Control& visual,
	size_t oldIndex,
	size_t newIndex,
	const BindingSourceReference& item,
	BindingPathObservation& observation,
	std::wstring* outError)
{
	// Reuse the complete cross-index rebind contract (and its virtual hook);
	// only the detached-container precondition is relaxed for this call.
	const auto* previous = std::exchange(DataGridAttachedRebindOwner, this);
	const auto restore = MakeScopeExit([previous]
	{
		DataGridAttachedRebindOwner = previous;
	});
	return TryRebindGeneratedItemAcrossIndices(
		visual, oldIndex, newIndex, item, observation, outError);
}

bool DataGrid::CanRecycleGeneratedItemAcrossIndices(
	const Control& visual, size_t oldIndex) const noexcept
{
	const auto* row = dynamic_cast<const DataGridRow*>(&visual);
	auto* mutableRow = const_cast<DataGridRow*>(row);
	const bool attachedRebind = DataGridAttachedRebindOwner == this;
	if (!row || row->GetDataGridOwner() != this
		|| row->ItemIndex() != oldIndex
		|| row->GetIsEditing()
		|| (!attachedRebind
			&& (row->GetVisualParent() || row->GetLogicalParent()
				|| row->GetPresentationWindow()))
		|| mutableRow->IsKeyboardFocusWithin
		|| mutableRow->IsMouseCaptureWithin)
		return false;
	if (_currentCell.IsValid() && _currentCell.RowIndex == oldIndex)
		return false;
	for (auto* cell : row->GetCells())
	{
		if (!cell || cell->GetRowOwner() != row || cell->GetIsEditing())
			return false;
		const size_t columnIndex = cell->_columnIndex;
		if (columnIndex >= _columns.size()
			|| _columns[columnIndex].get() != cell->_column)
			return false;
	}
	return true;
}

bool DataGrid::TryRebindGeneratedItemAcrossIndices(
	Control& visual,
	size_t oldIndex,
	size_t newIndex,
	const BindingSourceReference& item,
	BindingPathObservation& observation,
	std::wstring* outError)
{
	if (outError) outError->clear();
	auto* row = dynamic_cast<DataGridRow*>(&visual);
	if (!item || newIndex >= ItemCount() || oldIndex == newIndex
		|| !row || !CanRecycleGeneratedItemAcrossIndices(*row, oldIndex))
	{
		if (outError) *outError = L"DataGrid 回收行与目标索引不兼容。";
		return false;
	}

	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(row);
	const auto items = GetItemsView();
	const size_t columnCount = _columns.size();
	const size_t columnRevision = _columnWidthProjectionRevision;
	const size_t cellSelectionRevision = _cellSelectionRevision;
	std::vector<DataGridColumn*> columns;
	columns.reserve(columnCount);
	for (const auto& column : _columns) columns.push_back(column.get());
	auto resolveCurrent = [&]() -> std::pair<DataGrid*, DataGridRow*>
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* liveRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner || !liveRow || liveRow->GetDataGridOwner() != owner
			|| owner->GetItemsView().Shared() != items.Shared()
			|| owner->_columns.size() != columnCount
			|| owner->_columnWidthProjectionRevision != columnRevision)
			return {};
		for (size_t index = 0; index < columnCount; ++index)
			if (owner->_columns[index].get() != columns[index]) return {};
		return { owner, liveRow };
	};

	BindingSourceReference currentItem;
	if (!items || !items.Get()->TryGetItem(newIndex, currentItem)
		|| currentItem.Shared() != item.Shared())
	{
		if (outError) *outError = L"DataGrid 回收行的目标项已变化。";
		return false;
	}
	auto [owner, liveRow] = resolveCurrent();
	if (!owner || !liveRow)
	{
		if (outError) *outError = L"DataGrid 回收期间列或数据源已变化。";
		return false;
	}

	// DataGrid does not install an outer item observation, but clear a future
	// derived observation before the visual starts representing a new occurrence.
	observation = {};
	liveRow->_item = item;
	liveRow->SetItemIndex(newIndex);
	(void)liveRow->SetDataContext(item);
	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow || liveRow->GetItem().Shared() != item.Shared()
		|| liveRow->ItemIndex() != newIndex)
	{
		if (outError) *outError = L"DataGrid 回收行 DataContext 更新失败。";
		return false;
	}
	const bool isNewItemPlaceholder = owner->IsNewItemPlaceholder(item);
	liveRow->SetCurrentIsNewItem(
		isNewItemPlaceholder || owner->IsPendingNewItem(item, newIndex));
	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow || liveRow->GetItem().Shared() != item.Shared()
		|| liveRow->ItemIndex() != newIndex)
	{
		if (outError) *outError = L"DataGrid 回收行新项状态更新失败。";
		return false;
	}
	if (!owner->RestoreRowPresentationOverride(*liveRow))
	{
		if (outError) *outError = L"DataGrid 回收行详情覆盖恢复失败。";
		return false;
	}
	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow)
	{
		if (outError) *outError = L"DataGrid 回收行详情覆盖已失效。";
		return false;
	}
	std::wstring rowStyle;
	if (!owner->TryResolveRowStyle(*liveRow, rowStyle))
	{
		if (outError) *outError = L"DataGrid 回收行样式选择失败。";
		return false;
	}
	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow)
	{
		if (outError) *outError = L"DataGrid 回收行样式已失效。";
		return false;
	}
	cui::framework::StyleAccess::SetResourceKey(*liveRow, rowStyle);
	if (!liveRow->GetIsSelected() && !liveRow->IsMouseOver)
		liveRow->SetBackground((newIndex % 2) != 0
			? owner->GetAlternatingRowBackground()
			: owner->GetRowBackground());
	else (void)liveRow->ClearPropertyValue(Control::BackgroundProperty());
	liveRow->UpdateHeightPresentation();
	const bool horizontalLines = HasHorizontalGridLines(
		owner->GetGridLinesVisibility());
	liveRow->SetBorderThickness(Thickness(
		0.0f, 0.0f, 0.0f, horizontalLines ? 1.0f : 0.0f));
	liveRow->SetBorderBrush(owner->GetHorizontalGridLinesBrush());

	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow)
	{
		if (outError) *outError = L"DataGrid 回收行外观更新失败。";
		return false;
	}
	if (liveRow->_columnStorageIsSparse)
	{
		const auto range = owner->ResolveRealizedColumnRange();
		const size_t frozenEnd = (std::min)(owner->ColumnCount(),
			static_cast<size_t>((std::max)(
				0, owner->GetFrozenColumnCount())));
		if ((frozenEnd != liveRow->_realizedFrozenColumnEnd
			|| range.first != liveRow->_realizedColumnBegin
			|| range.second != liveRow->_realizedColumnEnd)
			&& !liveRow->RefreshRealizedColumns(
				frozenEnd, range.first, range.second, outError)) return false;
	}

	std::vector<ControlWeakReference> cells;
	const auto realizedCells = liveRow->GetCells();
	cells.reserve(realizedCells.size());
	for (auto* cell : realizedCells)
		if (cell) cells.emplace_back(cell);
	for (const auto& cellLifetime : cells)
	{
		std::tie(owner, liveRow) = resolveCurrent();
		auto* cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!owner || !liveRow || !cell || cell->GetRowOwner() != liveRow
			|| cell->_columnIndex >= columnCount
			|| columns[cell->_columnIndex] != cell->_column)
		{
			if (outError) *outError = L"DataGrid 回收行单元格结构已变化。";
			return false;
		}
		cell->_item = item;
		(void)cell->SetDataContext(item);
		std::tie(owner, liveRow) = resolveCurrent();
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!owner || !liveRow || !cell || cell->GetRowOwner() != liveRow
			|| liveRow->GetItem().Shared() != item.Shared()
			|| liveRow->ItemIndex() != newIndex
			|| cell->_item.Shared() != item.Shared()
			|| cell->_columnIndex >= columnCount
			|| columns[cell->_columnIndex] != cell->_column)
		{
			if (outError) *outError = L"DataGrid 回收行 DataContext 更新失败。";
			return false;
		}
		cell->SetCurrentIsNewItemPlaceholder(
			owner->IsNewItemPlaceholder(item));
		std::tie(owner, liveRow) = resolveCurrent();
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!owner || !liveRow || !cell || cell->GetRowOwner() != liveRow
			|| liveRow->GetItem().Shared() != item.Shared()
			|| liveRow->ItemIndex() != newIndex
			|| cell->_item.Shared() != item.Shared()
			|| cell->_columnIndex >= columnCount
			|| columns[cell->_columnIndex] != cell->_column)
		{
			if (outError) *outError = L"DataGrid 回收行单元格新项状态更新失败。";
			return false;
		}
		// Built-in bound columns bind through the element DataContext proxy, so
		// modes that observe that proxy can retain their content and Binding.
		// OneTime and OneWayToSource deliberately do not observe source retargets;
		// rebuilding preserves the same initialization semantics as a fresh row.
		// CheckBox display bindings are always forced OneWay by GenerateElement.
		const auto* textColumn =
			dynamic_cast<DataGridTextColumn*>(cell->_column);
		const auto* comboBoxColumn =
			dynamic_cast<DataGridComboBoxColumn*>(cell->_column);
		const auto* hyperlinkColumn =
			dynamic_cast<DataGridHyperlinkColumn*>(cell->_column);
		const auto* boundColumn = textColumn
			? static_cast<const DataGridBoundColumn*>(textColumn)
			: comboBoxColumn
				? static_cast<const DataGridBoundColumn*>(comboBoxColumn)
				: hyperlinkColumn
					? static_cast<const DataGridBoundColumn*>(hyperlinkColumn)
					: nullptr;
		const BindingMode boundMode = boundColumn
			? boundColumn->GetBindingMode() : BindingMode::Default;
		const bool retainsBoundContent =
			dynamic_cast<DataGridCheckBoxColumn*>(cell->_column)
			|| (boundColumn && boundMode != BindingMode::OneTime
				&& boundMode != BindingMode::OneWayToSource
				&& (!hyperlinkColumn
					|| !hyperlinkColumn->HasContentBinding()
					|| (hyperlinkColumn->GetContentBindingMode()
						!= BindingMode::OneTime
						&& hyperlinkColumn->GetContentBindingMode()
							!= BindingMode::OneWayToSource)));
		if (!retainsBoundContent
			&& !cell->ReplaceContent(false, outError)) return false;
		std::tie(owner, liveRow) = resolveCurrent();
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!owner || !liveRow || !cell || cell->GetRowOwner() != liveRow)
		{
			if (outError) *outError = L"DataGrid 回收行绑定更新失败。";
			return false;
		}
		cell->SetCurrentIsSelected(
			!owner->_selectedCells.empty()
			&& owner->IsCellSelected(newIndex, cell->_columnIndex), true);
	}

	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow)
	{
		if (outError) *outError = L"DataGrid 回收行最终投影失败。";
		return false;
	}
	liveRow->UpdateColumnWidths();
	liveRow->UpdateRowHeader();
	if (!liveRow->UpdateRowHeaderPresentation()) return false;
	std::tie(owner, liveRow) = resolveCurrent();
	if (!owner || !liveRow) return false;
	liveRow->UpdateHorizontalScrollOffset(owner->_horizontalScrollOffset);
	if (!liveRow->UpdateDetailsPresentation(outError)) return false;
	if (owner->_cellSelectionRevision == cellSelectionRevision)
		liveRow->_appliedCellSelectionRevision = cellSelectionRevision;
	else liveRow->_appliedCellSelectionRevision = 0;
	return ownerLifetime.Get() != nullptr && rowLifetime.Get() != nullptr;
}

void DataGrid::OnBeforeGeneratedItemsPrepared()
{
	const bool preserveContentWidths =
		_preserveColumnContentWidthsDuringRowRebuild
		&& !IsItemsSourceUpdateInProgress();
	if (!preserveContentWidths)
		InvalidateColumnContentWidthCache();
	const size_t previousCount = _columns.size();
	EnsureAutoGeneratedColumns();
	if (_columns.size() != previousCount)
	{
		_autoColumnsChangedDuringPreparation = true;
		// Delayed runtime auto-generation is a real schema change even when it
		// happened to be discovered by an otherwise row-only rebuild.
		if (preserveContentWidths)
			InvalidateColumnContentWidthCache();
	}
}

float DataGrid::GetVirtualizedItemHeight() const noexcept
{
	const double baseline = std::isfinite(_rowHeight) && _rowHeight > 0.0
		? _rowHeight
		: static_cast<double>(ItemsControl::GetVirtualizedItemHeight());
	return static_cast<float>((std::min)(
		(std::max)(baseline, _minRowHeight),
		static_cast<double>((std::numeric_limits<float>::max)())));
}

bool DataGrid::UseMeasuredVirtualizedItemHeight(
	const Control& item) const noexcept
{
	const auto* row = dynamic_cast<const DataGridRow*>(&item);
	return row && row->GetDataGridOwner() == this
		&& row->GetDetailsVisibility() == Visibility::Visible;
}

struct DataGrid::DataGridItemsSourceTransactionState final
	: ItemsSourceTransactionState
{
	std::unique_ptr<ItemsSourceTransactionState> SelectorState;
	size_t ColumnCount = 0;
	std::vector<std::optional<GridLength>> ResolvedColumnWidths;
	std::vector<double> ColumnWidthPrefix;
	std::vector<double> RuntimeColumnDesiredWidths;
	std::vector<double> RuntimeColumnDisplayWidths;
	std::vector<bool> RuntimeColumnDisplayOverrides;
	std::vector<std::optional<double>> SampledColumnContentWidths;
	size_t ColumnContentWidthCacheEpoch = 1;
	DataGridCellInfo CurrentCell;
	DataGridSelectedCellCollection SelectedCells;
	std::optional<DataGridCellInfo> SelectionAnchor;
	std::vector<DataGridCellInfo> SelectionRangeBase;
	std::vector<std::pair<BindingSourceReference, size_t>> SelectedRowSnapshot;
	SelectedIndexCollection SelectedRowIndexSnapshot;
	size_t RowSelectionRevision = 1;
	bool AutoColumnsChangedDuringPreparation = false;
	bool RuntimeAutoGenerationComplete = false;
	bool AutoGeneratedColumnsEventPending = false;
	int FrozenColumnCount = 0;
	bool CurrentCellChangeDeferred = false;
	DataGridCellInfo DeferredCurrentCellOld;
	bool SelectedCellsChangeDeferred = false;
	DataGridSelectedCellCollection DeferredSelectedCellsOld;
	bool DeferredSelectedCellsIgnoreLocators = false;
	bool SelectionRangeActive = false;
	AccessibilityIdentityState AccessibilityIdentities;
	std::vector<RowHeightOverride> RowHeightOverrides;
	std::vector<VirtualizedItemExtentOverride> VirtualRowHeightOverrides;
	std::vector<RowPresentationOverride> RowPresentationOverrides;
	size_t VirtualRowHeightOverrideRevision = 1;
};

std::unique_ptr<ItemsControl::ItemsSourceTransactionState>
DataGrid::CaptureItemsSourceTransactionState()
{
	auto state = std::make_unique<DataGridItemsSourceTransactionState>();
	state->ColumnCount = _columns.size();
	state->ResolvedColumnWidths = _resolvedColumnWidths;
	state->ColumnWidthPrefix = _columnWidthPrefix;
	state->RuntimeColumnDesiredWidths.reserve(_columns.size());
	state->RuntimeColumnDisplayWidths.reserve(_columns.size());
	state->RuntimeColumnDisplayOverrides.reserve(_columns.size());
	for (const auto& column : _columns)
	{
		state->RuntimeColumnDesiredWidths.push_back(
			column->_runtimeWidth.Desired);
		state->RuntimeColumnDisplayWidths.push_back(
			column->_runtimeWidth.Display);
		state->RuntimeColumnDisplayOverrides.push_back(
			column->_runtimeWidth.HasDisplayOverride);
	}
	state->SampledColumnContentWidths = _sampledColumnContentWidths;
	state->ColumnContentWidthCacheEpoch = _columnContentWidthCacheEpoch;
	state->CurrentCell = _currentCell;
	state->SelectedCells = _selectedCells;
	state->SelectionAnchor = _selectionAnchor;
	state->SelectionRangeBase = _selectionRangeBase;
	state->SelectedRowSnapshot = _selectedRowSnapshot;
	state->SelectedRowIndexSnapshot = _selectedRowIndexSnapshot;
	state->RowSelectionRevision = _rowSelectionRevision;
	state->AutoColumnsChangedDuringPreparation =
		_autoColumnsChangedDuringPreparation;
	state->RuntimeAutoGenerationComplete = _runtimeAutoGenerationComplete;
	state->AutoGeneratedColumnsEventPending =
		_autoGeneratedColumnsEventPending;
	state->FrozenColumnCount = _frozenColumnCount;
	state->CurrentCellChangeDeferred = _currentCellChangeDeferred;
	state->DeferredCurrentCellOld = _deferredCurrentCellOld;
	state->SelectedCellsChangeDeferred = _selectedCellsChangeDeferred;
	state->DeferredSelectedCellsOld = _deferredSelectedCellsOld;
	state->DeferredSelectedCellsIgnoreLocators =
		_deferredSelectedCellsIgnoreLocators;
	state->SelectionRangeActive = _selectionRangeActive;
	state->AccessibilityIdentities = CaptureAccessibilityIdentityState();
	state->RowHeightOverrides = _rowHeightOverrides;
	state->VirtualRowHeightOverrides = _virtualRowHeightOverrides;
	state->RowPresentationOverrides = _rowPresentationOverrides;
	state->VirtualRowHeightOverrideRevision = _virtualRowHeightOverrideRevision;
	state->SelectorState = ListBox::CaptureItemsSourceTransactionState();
	return state;
}

void DataGrid::RestoreItemsSourceTransactionState(
	ItemsSourceTransactionState& state) noexcept
{
	auto* dataGrid = dynamic_cast<DataGridItemsSourceTransactionState*>(&state);
	if (!dataGrid) return;
	// Restore may destroy auto-generated candidate columns after a reentrant
	// UIA query published their raw pointers.  Invalidate before erasing them.
	InvalidateAccessibilityVirtualIdentities();
	if (dataGrid->SelectorState)
		ListBox::RestoreItemsSourceTransactionState(
			*dataGrid->SelectorState);
	const size_t originalLogicalCount = (std::min)(
		dataGrid->ColumnCount, _logicalColumns.size());
	const auto candidateLogicalBegin =
		_logicalColumns.begin() + originalLogicalCount;
	for (auto candidate = candidateLogicalBegin;
		candidate != _logicalColumns.end(); ++candidate)
	{
		if (!(*candidate)->_isRuntimeAutoGenerated) continue;
		(*candidate)->_owner = nullptr;
		(*candidate)->_displayIndex = DataGridColumn::UnsetDisplayIndex;
		(*candidate)->_visibleIndex = DataGridColumn::UnsetDisplayIndex;
	}
	_logicalColumns.erase(std::remove_if(
		candidateLogicalBegin, _logicalColumns.end(),
		[](const DataGridColumn* column)
		{ return column->_owner == nullptr; }),
		_logicalColumns.end());
	for (size_t index = _columns.size(); index > 0;)
	{
		--index;
		auto* const candidate = _columns[index].get();
		if (candidate->_owner != nullptr
			|| !candidate->_isRuntimeAutoGenerated) continue;
		_columns.erase(_columns.begin() + index);
	}
	ReindexDisplayColumns();
	// Coercion performed while preparing a failed candidate is transactional too.
	// Restore the prior effective value directly: the DP base request never moved.
	_frozenColumnCount = dataGrid->FrozenColumnCount;
	PruneAccessibilityColumnIdentities();
	_resolvedColumnWidths = std::move(dataGrid->ResolvedColumnWidths);
	_columnWidthPrefix = std::move(dataGrid->ColumnWidthPrefix);
	const size_t runtimeWidthCount = (std::min)({
		_columns.size(), dataGrid->RuntimeColumnDesiredWidths.size(),
		dataGrid->RuntimeColumnDisplayWidths.size(),
		dataGrid->RuntimeColumnDisplayOverrides.size() });
	for (size_t index = 0; index < runtimeWidthCount; ++index)
	{
		_columns[index]->_runtimeWidth.Desired =
			dataGrid->RuntimeColumnDesiredWidths[index];
		_columns[index]->_runtimeWidth.Display =
			dataGrid->RuntimeColumnDisplayWidths[index];
		_columns[index]->_runtimeWidth.HasDisplayOverride =
			dataGrid->RuntimeColumnDisplayOverrides[index];
	}
	_sampledColumnContentWidths =
		std::move(dataGrid->SampledColumnContentWidths);
	_columnContentWidthCacheEpoch =
		dataGrid->ColumnContentWidthCacheEpoch;
	_currentCell = dataGrid->CurrentCell;
	_selectedCells = std::move(dataGrid->SelectedCells);
	_selectedCells._ownerLifetime = ControlWeakReference(this);
	_selectedCells._ownerBound = true;
	_selectedCells._ownerSourceIdentity = _source;
	_selectedCells.InvalidateExcludedOffsets();
	_deferredSelectedCellsOld = std::move(
		dataGrid->DeferredSelectedCellsOld);
	_deferredSelectedCellsOld._ownerLifetime = ControlWeakReference(this);
	_deferredSelectedCellsOld._ownerBound = true;
	if (_deferredSelectedCellsOld._source)
		_deferredSelectedCellsOld._ownerSourceIdentity = _source;
	_deferredSelectedCellsOld.InvalidateExcludedOffsets();
	_selectionAnchor = std::move(dataGrid->SelectionAnchor);
	_selectionRangeBase = std::move(dataGrid->SelectionRangeBase);
	_selectedRowSnapshot = std::move(dataGrid->SelectedRowSnapshot);
	_selectedRowIndexSnapshot = std::move(
		dataGrid->SelectedRowIndexSnapshot);
	_rowSelectionRevision = dataGrid->RowSelectionRevision;
	// A rollback restores the logical cell selection, but readers which observed
	// the failed candidate must still see an identity boundary. This revision is
	// a clock, not transaction payload, so never move it backwards.
	if (++_cellSelectionRevision == 0) _cellSelectionRevision = 1;
	_selectedCellsVisualRefreshPending = true;
	_autoColumnsChangedDuringPreparation =
		dataGrid->AutoColumnsChangedDuringPreparation;
	_runtimeAutoGenerationComplete =
		dataGrid->RuntimeAutoGenerationComplete;
	_autoGeneratedColumnsEventPending =
		dataGrid->AutoGeneratedColumnsEventPending;
	_currentCellChangeDeferred = dataGrid->CurrentCellChangeDeferred;
	_deferredCurrentCellOld = dataGrid->DeferredCurrentCellOld;
	_selectedCellsChangeDeferred = dataGrid->SelectedCellsChangeDeferred;
	_deferredSelectedCellsIgnoreLocators =
		dataGrid->DeferredSelectedCellsIgnoreLocators;
	_selectionRangeActive = dataGrid->SelectionRangeActive;
	_rowHeightOverrides = std::move(dataGrid->RowHeightOverrides);
	_virtualRowHeightOverrides = std::move(
		dataGrid->VirtualRowHeightOverrides);
	_rowPresentationOverrides = std::move(
		dataGrid->RowPresentationOverrides);
	_virtualRowHeightOverrideRevision =
		dataGrid->VirtualRowHeightOverrideRevision;
	_rowResizeActive = false;
	_rowResizeItem = {};
	_rowResizeOccurrence = DataGridCellInfo::InvalidIndex;
	_rowResizeIndex = DataGridCellInfo::InvalidIndex;
	_rowResizeHeight = 0.0;
	RestoreAccessibilityIdentityState(
		std::move(dataGrid->AccessibilityIdentities));
	if (!_settingItemsSource)
		RefreshSelectedCellContainersAfterRollback();
}

void DataGrid::OnItemsSourceTransactionCommitted()
{
	const ControlWeakReference ownerLifetime(this);
	ListBox::OnItemsSourceTransactionCommitted();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (live->_settingItemsSource) return;
	live->FlushCommittedItemsSourceState();
	if (auto* current = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		current->FlushAccessibilityStructureChange();
}

void DataGrid::OnItemsSourceReplacementPreparing(
	const BindingListReference&,
	const BindingListReference&)
{
	EndRowResizeTransaction(true);
	_rowHeightOverrides.clear();
	_virtualRowHeightOverrides.clear();
	_rowPresentationOverrides.clear();
	if (++_virtualRowHeightOverrideRevision == 0)
		++_virtualRowHeightOverrideRevision;
	// A different ItemsSource is a new occurrence domain. Clear cell locators
	// before the candidate generator rebuilds so old CurrentCell/SelectedCells
	// are never resolved by scanning the replacement. The surrounding
	// ItemsControl transaction has already captured these fields for rollback,
	// while the normal deferred notification path publishes the removals only
	// after a successful commit.
	const auto previousCurrent = _currentCell;
	_currentCell = {};
	if (!RaiseCurrentCellChanged(previousCurrent))
		throw std::runtime_error(
			"DataGrid failed to prepare source replacement current cell");
	_selectionAnchor.reset();
	ResetSelectionRange();
	_selectedRowSnapshot.clear();
	_selectedRowIndexSnapshot.Clear();
	if (!ApplySelectedCells({}))
		throw std::runtime_error(
			"DataGrid failed to prepare source replacement cell selection");
}

void DataGrid::OnItemsSourceCollectionChangePreparing(
	const CollectionChangedEventArgs& change,
	const BindingListReference& previousSnapshot)
{
	EndRowResizeTransaction(true);
	const auto previousRowShape = GetSelectedIndices();
	ListBox::OnItemsSourceCollectionChangePreparing(change, previousSnapshot);
	if (!(previousRowShape == GetSelectedIndices()))
		if (++_rowSelectionRevision == 0) _rowSelectionRevision = 1;
	const bool cellRegion = _selectionUnit == DataGridSelectionUnit::Cell;
	const bool fullRowRegion =
		_selectionUnit == DataGridSelectionUnit::FullRow
		&& (GetSelectedIndices().IsRangeBacked()
			|| _selectedRowIndexSnapshot.IsRangeBacked());
	if ((!cellRegion && !fullRowRegion)
		|| !_selectedCells.IsRegionBacked()
		|| _selectedCells._ownerSourceIdentity.Shared() != _source.Shared())
		return;
	const auto captureRowShape = [this, fullRowRegion]()
	{
		if (!fullRowRegion) return;
		_selectedRowIndexSnapshot = GetSelectedIndices();
		_selectedRowSnapshot.clear();
	};
	// Add allocates identities that do not exist in the immutable SelectAll
	// checkpoint, so the new rows are naturally unselected. Move/Swap preserve
	// occurrence identities and need only locator reconciliation after commit.
	if (change.Action == CollectionChangeAction::Add
		|| change.Action == CollectionChangeAction::Move
		|| change.Action == CollectionChangeAction::Swap)
	{
		captureRowShape();
		return;
	}
	if (change.Action == CollectionChangeAction::Reset)
	{
		std::vector<size_t> oldToNew;
		if (TryResolveSelectionOccurrencePermutation(
			previousSnapshot, oldToNew))
		{
			captureRowShape();
			return;
		}
		// WPF rebuilds cell selection from surviving full-row selector ranges on
		// a non-permutation Reset. Cell-only mode has no such ranges, so clear the
		// current domain. A sort/deferred Move retains physical occurrence tokens
		// and follows the fast return above; ReconcileSelectedCells remaps locators.
		// RaiseSelectedCellsChangedCore binds the resulting RemovedCells regions
		// to their old immutable snapshot, without N x C materialization.
		const ControlWeakReference ownerLifetime(this);
		if (!ApplySelectedCells({}) && ownerLifetime.Get())
			throw std::runtime_error(
				"DataGrid failed to prepare Reset cell selection");
		captureRowShape();
		return;
	}
	if (change.Action != CollectionChangeAction::Remove
		&& change.Action != CollectionChangeAction::Replace) return;
	const auto* oldIdentities = previousSnapshot
		? dynamic_cast<const IBindingListOccurrenceIdentity*>(
			previousSnapshot.Get()) : nullptr;
	if (!oldIdentities || change.OldIndex == CollectionChangedEventArgs::Npos
		|| change.OldCount == 0
		|| change.OldIndex > previousSnapshot.Get()->Count()
		|| change.OldCount > previousSnapshot.Get()->Count() - change.OldIndex)
		return;

	auto selected = _selectedCells;
	bool changed = false;
	// CollectionView pass-through snapshots preserve source occurrence tokens,
	// so the old live index range maps to a continuous immutable checkpoint
	// range in the common mutable-list case.  Confirm only the two boundaries;
	// Replace also keeps physical occurrence tokens by contract.  A future
	// third-party provider with non-contiguous tokens falls back to the exact
	// occurrence path below.
	const auto* snapshotLookup = selected._source
		? dynamic_cast<const IBindingListOccurrenceLookup*>(
			selected._source.Get()) : nullptr;
	bool intervalApplied = false;
	if (snapshotLookup && change.OldCount != 0)
	{
		size_t firstOccurrence = DataGridCellInfo::InvalidIndex;
		size_t lastOccurrence = DataGridCellInfo::InvalidIndex;
		size_t firstSnapshotRow = DataGridCellInfo::InvalidIndex;
		size_t lastSnapshotRow = DataGridCellInfo::InvalidIndex;
		const size_t lastOldRow = change.OldIndex + change.OldCount - 1;
		if (oldIdentities->TryGetItemOccurrenceIdentity(
				change.OldIndex, firstOccurrence)
			&& oldIdentities->TryGetItemOccurrenceIdentity(
				lastOldRow, lastOccurrence)
			&& snapshotLookup->TryGetItemIndexByOccurrenceIdentity(
				firstOccurrence, firstSnapshotRow)
			&& snapshotLookup->TryGetItemIndexByOccurrenceIdentity(
				lastOccurrence, lastSnapshotRow)
			&& firstSnapshotRow <= lastSnapshotRow
			&& lastSnapshotRow - firstSnapshotRow + 1 == change.OldCount)
		{
			for (auto& region : selected._regions)
			{
				const size_t regionEnd = region.Height
					> (std::numeric_limits<size_t>::max)() - region.Top
					? (std::numeric_limits<size_t>::max)()
					: region.Top + region.Height;
				const size_t start = (std::max)(
					region.Top, firstSnapshotRow);
				const size_t end = (std::min)(regionEnd,
					lastSnapshotRow == (std::numeric_limits<size_t>::max)()
						? lastSnapshotRow : lastSnapshotRow + 1);
				if (start >= end) continue;
				if (!region.IncludedRowOffsets.empty())
				{
					const auto included = std::lower_bound(
						region.IncludedRowOffsets.begin(),
						region.IncludedRowOffsets.end(), start - region.Top);
					if (included == region.IncludedRowOffsets.end()
						|| *included >= end - region.Top) continue;
				}
				DataGridSelectedCellCollection::CellRegion::
					ExcludedSnapshotRowInterval added{
					start, end - start };
				auto& intervals = region.ExcludedSnapshotRowIntervals;
				intervals.push_back(added);
				std::sort(intervals.begin(), intervals.end(),
					[](const auto& a, const auto& b)
					{ return a.Start < b.Start; });
				std::vector<DataGridSelectedCellCollection::CellRegion::
					ExcludedSnapshotRowInterval> merged;
				merged.reserve(intervals.size());
				for (const auto& interval : intervals)
				{
					if (interval.Count == 0) continue;
					if (!merged.empty()
						&& IntervalEndExclusive(merged.back())
							>= interval.Start)
					{
						auto& previous = merged.back();
						const size_t mergedEnd = (std::max)(
							IntervalEndExclusive(previous),
							IntervalEndExclusive(interval));
						previous.Count = mergedEnd - previous.Start;
					}
					else merged.push_back(interval);
				}
				intervals = std::move(merged);
				region.ExcludedRows.erase(std::remove_if(
					region.ExcludedRows.begin(), region.ExcludedRows.end(),
					[&](const auto& row)
					{
						return IsRowInIntervals(
							intervals, row.SnapshotRow);
					}), region.ExcludedRows.end());
				changed = true;
			}
			if (changed)
			{
				selected._cells.erase(std::remove_if(
					selected._cells.begin(), selected._cells.end(),
					[&](const DataGridCellInfo& cell)
					{
						size_t snapshotRow = 0;
						return snapshotLookup->
							TryGetItemIndexByOccurrenceIdentity(
								cell._itemOccurrence, snapshotRow)
							&& snapshotRow >= firstSnapshotRow
							&& snapshotRow <= lastSnapshotRow;
					}), selected._cells.end());
				selected._excludedCells.erase(std::remove_if(
					selected._excludedCells.begin(),
					selected._excludedCells.end(),
					[&](const DataGridCellInfo& cell)
					{
						size_t snapshotRow = 0;
						return snapshotLookup->
							TryGetItemIndexByOccurrenceIdentity(
								cell._itemOccurrence, snapshotRow)
							&& snapshotRow >= firstSnapshotRow
							&& snapshotRow <= lastSnapshotRow;
					}), selected._excludedCells.end());
				intervalApplied = true;
			}
		}
	}
	if (!intervalApplied)
	for (size_t offset = 0; offset < change.OldCount; ++offset)
	{
		bool changedOccurrence = false;
		const size_t oldRow = change.OldIndex + offset;
		BindingSourceReference oldItem;
		size_t occurrence = DataGridCellInfo::InvalidIndex;
		if (!previousSnapshot.Get()->TryGetItem(oldRow, oldItem) || !oldItem
			|| !oldIdentities->TryGetItemOccurrenceIdentity(
				oldRow, occurrence)
			|| occurrence == DataGridCellInfo::InvalidIndex) continue;
		for (auto& region : selected._regions)
		{
			const auto* lookup = selected._source
				? dynamic_cast<const IBindingListOccurrenceLookup*>(
					selected._source.Get()) : nullptr;
			if (!lookup) continue;
			size_t snapshotRow = 0;
			if (!lookup->TryGetItemIndexByOccurrenceIdentity(
					occurrence, snapshotRow)
				|| snapshotRow < region.Top
				|| snapshotRow - region.Top >= region.Height) continue;
			BindingSourceReference snapshotItem;
			if (!selected._source.Get()->TryGetItem(
					snapshotRow, snapshotItem)
				|| !snapshotItem
				|| snapshotItem.Shared() != oldItem.Shared()) continue;
			const size_t rowOffset = snapshotRow - region.Top;
			if (!region.IncludedRowOffsets.empty()
				&& !std::binary_search(region.IncludedRowOffsets.begin(),
					region.IncludedRowOffsets.end(), rowOffset)) continue;
			if (std::any_of(region.ExcludedRows.begin(),
				region.ExcludedRows.end(), [occurrence](const auto& row)
				{ return row.Occurrence == occurrence; })) continue;
			region.ExcludedRows.push_back({
				oldItem, occurrence, snapshotRow });
			changedOccurrence = true;
		}
		const auto cellsEnd = std::remove_if(
			selected._cells.begin(), selected._cells.end(),
			[occurrence](const DataGridCellInfo& cell)
			{ return cell._itemOccurrence == occurrence; });
		if (cellsEnd != selected._cells.end())
		{
			selected._cells.erase(cellsEnd, selected._cells.end());
			changedOccurrence = true;
		}
		if (changedOccurrence)
		{
			selected._excludedCells.erase(std::remove_if(
				selected._excludedCells.begin(), selected._excludedCells.end(),
				[occurrence](const DataGridCellInfo& cell)
				{ return cell._itemOccurrence == occurrence; }),
				selected._excludedCells.end());
		}
		changed = changed || changedOccurrence;
	}
	if (!changed)
	{
		captureRowShape();
		return;
	}
	for (auto& region : selected._regions)
		std::sort(region.ExcludedRows.begin(), region.ExcludedRows.end(),
			[](const auto& left, const auto& right)
			{ return left.SnapshotRow < right.SnapshotRow; });
	selected.InvalidateExcludedOffsets();
	const ControlWeakReference ownerLifetime(this);
	if (!ApplySelectedCellCollection(std::move(selected))
		&& ownerLifetime.Get())
		throw std::runtime_error(
			"DataGrid failed to prepare mutable cell selection");
	if (ownerLifetime.Get()) captureRowShape();
}

void DataGrid::OnItemsSourceCollectionChangeCommitted(
	const CollectionChangedEventArgs& change)
{
	const ControlWeakReference ownerLifetime(this);
	if (!_callingEditableList)
		ReconcileRowEditTransaction();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->PruneRowHeightOverrides();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->PruneRowPresentationOverrides();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->RefreshRealizedRowHeights();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !std::isnan(live->_rowHeaderWidth)) return;
	if (change.Action == CollectionChangeAction::Reset)
	{
		live->InvalidateRowHeaderWidthBaseline();
		(void)live->SetCurrentRowHeaderActualWidth(0.0);
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) (void)live->RefreshRowHeaderActualWidth();
}

void DataGrid::OnGeneratedItemClearing(Control& visual)
{
	auto* row = dynamic_cast<DataGridRow*>(&visual);
	if (!row || row->GetDataGridOwner() != this) return;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference rowLifetime(row);
	const auto clearingItem = row->GetItem();
	const size_t clearingIndex = row->ItemIndex();
	row->_itemValidationConnection.Disconnect();
	row->_validationTrackedItem = {};
	row->_validationStateDirty = true;
	row->SetCurrentIsNewItem(false);
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!ownerLifetime.Get() || !row || row->GetDataGridOwner() != this
		|| row->GetItem().Shared() != clearingItem.Shared()
		|| row->ItemIndex() != clearingIndex) return;
	std::vector<ControlWeakReference> cells;
	cells.reserve(row->GetCells().size());
	for (auto* cell : row->GetCells())
		if (cell) cells.emplace_back(cell);
	for (const auto& cellLifetime : cells)
	{
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		auto* cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!ownerLifetime.Get() || !row || row->GetDataGridOwner() != this
			|| row->GetItem().Shared() != clearingItem.Shared()
			|| row->ItemIndex() != clearingIndex)
			return;
		if (!cell || cell->GetRowOwner() != row) continue;
		cell->SetCurrentIsNewItemPlaceholder(false);
	}
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!ownerLifetime.Get() || !row || row->GetDataGridOwner() != this
		|| row->GetItem().Shared() != clearingItem.Shared()
		|| row->ItemIndex() != clearingIndex) return;
	// CUI releases details visuals with their realized row.  Publish the WPF
	// details-reuse notification while both the row and generated element are
	// still valid, before the broader UnloadingRow notification.
	for (size_t attempt = 0; attempt < 16; ++attempt)
	{
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!ownerLifetime.Get() || !row || row->GetDataGridOwner() != this
			|| row->GetItem().Shared() != clearingItem.Shared()
			|| row->ItemIndex() != clearingIndex) return;
		auto* detailsElement = row->GetDetailsElement();
		if (!row->_detailsLoaded || !detailsElement)
		{
			row->_detailsLoaded = false;
			break;
		}
		row->_detailsLoaded = false;
		(void)RaiseRowDetailsEvent(
			UnloadingRowDetails, *row, *detailsElement);
	}
	row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
	if (!ownerLifetime.Get() || !row || row->GetDataGridOwner() != this
		|| row->GetItem().Shared() != clearingItem.Shared()
		|| row->ItemIndex() != clearingIndex) return;
	row->_detailsLoaded = false;
	const auto found = std::find(
		_lifecycleLoadedRows.begin(), _lifecycleLoadedRows.end(), rowLifetime);
	if (found == _lifecycleLoadedRows.end()) return;
	_lifecycleLoadedRows.erase(found);
	(void)RaiseRowLifecycleEvent(UnloadingRow, *row);
}

void DataGrid::OnItemsSourceChanged(
	const BindingListReference& oldValue,
	const BindingListReference& newValue)
{
	Selector::OnItemsSourceChanged(oldValue, newValue);
	if (_settingItemsSource || !_displayItemsView
		|| oldValue.Shared() != _displayItemsView
		|| newValue.Shared() == _displayItemsView) return;
	// ItemsControl pins a failed live mutation to its last materialized
	// snapshot. Publish that same source here and release the detached live view
	// so rows, CurrentCell and width measurement consume one projection.
	_source = newValue;
	if (++_cellSelectionRevision == 0) _cellSelectionRevision = 1;
	_itemsView.reset();
	_displayItemsView.reset();
	_rowHeaderSourceChanged.Disconnect();
	InvalidateColumnContentWidthCache();
	(void)CoerceValue(CanUserAddRowsProperty());
	(void)CoerceValue(CanUserDeleteRowsProperty());
}

DataGridRow* DataGrid::ResolveRow(size_t rowIndex) const noexcept
{
	return dynamic_cast<DataGridRow*>(GetGeneratedItem(rowIndex));
}

DataGridCell* DataGrid::ResolveCurrentCellContainer() const noexcept
{
	if (!_currentCell.IsValid()) return nullptr;
	auto* row = ResolveRow(_currentCell.RowIndex);
	return row ? row->GetCell(_currentCell.ColumnIndex) : nullptr;
}

bool DataGrid::TryCreateCellInfo(
	size_t rowIndex, size_t columnIndex, DataGridCellInfo& result) const
{
	result = {};
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const auto items = GetItemsView();
	if (!items) return false;
	const size_t count = items.Get()->Count();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| rowIndex >= count || columnIndex >= live->_columns.size())
		return false;
	auto* const column = live->_columns[columnIndex].get();
	const uint32_t columnIdentity = column->_accessibilityIdentity;
	const auto resolveCurrent = [&]() -> DataGrid*
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return owner && owner->GetItemsView().Shared() == items.Shared()
			&& columnIndex < owner->_columns.size()
			&& owner->_columns[columnIndex].get() == column
			&& owner->_columns[columnIndex]->_accessibilityIdentity
				== columnIdentity
			? owner : nullptr;
	};
	BindingSourceReference item;
	const bool read = items.Get()->TryGetItem(rowIndex, item);
	live = resolveCurrent();
	if (!live || !read || !item) return false;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	// The synthetic WPF new-item placeholder is a display-only sentinel and
	// deliberately has no source occurrence identity. It may still become the
	// current cell; BeginEdit replaces this transient identity with the real
	// occurrence created by AddNewItem before publishing editing events.
	if (!live->IsNewItemPlaceholder(item)
		&& !live->TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
	live = resolveCurrent();
	if (!live) return false;
	result.Item = std::move(item);
	result.Column = column;
	result.RowIndex = rowIndex;
	result.ColumnIndex = columnIndex;
	result._itemOccurrence = occurrence;
	return true;
}

bool DataGrid::TryGetItemOccurrenceAt(
	size_t rowIndex, size_t& occurrence) const
{
	occurrence = DataGridCellInfo::InvalidIndex;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const auto items = GetItemsView();
	if (!items) return false;
	const size_t count = items.Get()->Count();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| rowIndex >= count) return false;
	if (const auto* identities =
		dynamic_cast<const IBindingListOccurrenceIdentity*>(items.Get()))
	{
		const bool read = identities->TryGetItemOccurrenceIdentity(
			rowIndex, occurrence);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return read && occurrence != DataGridCellInfo::InvalidIndex
			&& live && live->GetItemsView().Shared() == items.Shared();
	}

	// Third-party lists that do not expose physical occurrence identities keep
	// the historical same-object ordinal fallback.  This is intentionally local
	// to the requested row: the normal CollectionViewSource path is O(1), while
	// selecting one row in a million-row view must never allocate an N-entry
	// occurrence cache merely to identify that row.
	BindingSourceReference target;
	if (!items.Get()->TryGetItem(rowIndex, target) || !target) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
	size_t ordinal = 0;
	for (size_t index = 0; index < rowIndex; ++index)
	{
		BindingSourceReference candidate;
		const bool read = items.Get()->TryGetItem(index, candidate);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared())
			return false;
		if (read && candidate
			&& candidate.Shared() == target.Shared()) ++ordinal;
	}
	occurrence = ordinal;
	return true;
}

bool DataGrid::TryGetStableSelectedCellRegionSnapshot(
	BindingListReference& snapshot) const
{
	snapshot = {};
	const auto items = GetItemsView();
	if (!items) return false;
	if (dynamic_cast<const IBindingListStableSnapshot*>(items.Get()))
		snapshot = items;
	else if (const auto* provider =
		dynamic_cast<const IBindingListSnapshotProvider*>(items.Get()))
	{
		if (!provider->TryGetStableSnapshot(snapshot)) return false;
	}
	if (!snapshot
		|| !dynamic_cast<const IBindingListStableSnapshot*>(snapshot.Get())
		|| !dynamic_cast<const IBindingListOccurrenceIdentity*>(snapshot.Get())
		|| !dynamic_cast<const IBindingListOccurrenceLookup*>(snapshot.Get())
		|| !dynamic_cast<const IBindingListOccurrenceIdentity*>(items.Get())
		|| !dynamic_cast<const IBindingListOccurrenceLookup*>(items.Get())
		|| snapshot.Get()->Count() != items.Get()->Count())
	{
		snapshot = {};
		return false;
	}
	return true;
}

bool DataGrid::TryResolveItemOccurrence(
	const BindingSourceReference& item,
	size_t occurrence,
	size_t& rowIndex) const
{
	rowIndex = DataGridCellInfo::InvalidIndex;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const auto items = GetItemsView();
	if (!item || !items) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
	const size_t count = items.Get()->Count();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return false;
	if (const auto* lookup =
		dynamic_cast<const IBindingListOccurrenceLookup*>(items.Get()))
	{
		size_t candidateIndex = DataGridCellInfo::InvalidIndex;
		const bool resolved = lookup->TryGetItemIndexByOccurrenceIdentity(
			occurrence, candidateIndex);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared())
			return false;
		if (resolved && candidateIndex < count)
		{
			BindingSourceReference candidate;
			const bool read = items.Get()->TryGetItem(
				candidateIndex, candidate);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared())
				return false;
			size_t candidateOccurrence = DataGridCellInfo::InvalidIndex;
			if (read && candidate
				&& candidate.Shared() == item.Shared()
				&& live->TryGetItemOccurrenceAt(
					candidateIndex, candidateOccurrence)
				&& candidateOccurrence == occurrence)
			{
				rowIndex = candidateIndex;
				return true;
			}
		}
	}
	for (size_t index = 0; index < count; ++index)
	{
		BindingSourceReference candidate;
		const bool read = items.Get()->TryGetItem(index, candidate);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared())
			return false;
		if (!read || !candidate || candidate.Shared() != item.Shared())
			continue;
		size_t candidateOccurrence = DataGridCellInfo::InvalidIndex;
		if (!live->TryGetItemOccurrenceAt(index, candidateOccurrence))
			return false;
		if (candidateOccurrence != occurrence) continue;
		rowIndex = index;
		return true;
	}
	return false;
}

void DataGrid::InvalidateItemOccurrenceCache() const noexcept
{
	InvalidateAccessibilityVirtualIdentities();
}

DataGrid::AccessibilityIdentityState
DataGrid::CaptureAccessibilityIdentityState() const
{
	AccessibilityIdentityState state;
	state.Columns = _accessibilityColumns;
	state.Rows = _accessibilityRows;
	// A callback in the candidate transaction may legally remove authored
	// columns.  Preserve only stable identity seeds; never retain column pointers
	// or a locator table that could refer to objects destroyed by that callback.
	for (auto& column : state.Columns) column.Column = nullptr;
	for (auto& row : state.Rows)
		for (auto& cell : row.Cells) cell.Column = nullptr;
	state.StructureChangePending = _accessibilityStructureChangePending;
	return state;
}

void DataGrid::RestoreAccessibilityIdentityState(
	AccessibilityIdentityState&& state) noexcept
{
	std::vector<AccessibilityColumnIdentity> retiredColumns;
	std::vector<AccessibilityRowIdentity> retiredRows;
	std::unordered_map<uint32_t, AccessibilityNodeLocator> retiredLookup;
	retiredColumns.swap(_accessibilityColumns);
	retiredRows.swap(_accessibilityRows);
	retiredLookup.swap(_accessibilityNodeLookup);
	_accessibilityRowIndexLookup.clear();
	_accessibilityColumns.swap(state.Columns);
	_accessibilityRows.swap(state.Rows);
	_accessibilityIdentitiesDirty = true;
	++_accessibilityIdentityRevision;
	if (_accessibilityIdentityRevision == 0)
		++_accessibilityIdentityRevision;
	_accessibilityStructureChangePending = state.StructureChangePending;
	// Retired candidate item references are released only after the live object
	// has a complete, dirty seed projection.  A destructor that re-enters UIA
	// therefore observes a rebuildable state rather than a half-restored table.
}

void DataGrid::RequestAccessibilityStructureChanged()
{
	if (_settingItemsSource || IsItemsSourceUpdateInProgress())
	{
		_accessibilityStructureChangePending = true;
		return;
	}
	NotifyAccessibilityStructureChanged();
}

void DataGrid::FlushAccessibilityStructureChange()
{
	if (!_accessibilityStructureChangePending
		|| _settingItemsSource || IsItemsSourceUpdateInProgress()) return;
	_accessibilityStructureChangePending = false;
	NotifyAccessibilityStructureChanged();
}

void DataGrid::InvalidateAccessibilityVirtualIdentities() const noexcept
{
	_accessibilityIdentitiesDirty = true;
	++_accessibilityIdentityRevision;
	if (_accessibilityIdentityRevision == 0)
		++_accessibilityIdentityRevision;
}

void DataGrid::PruneAccessibilityColumnIdentities() noexcept
{
	const auto isLive = [this](uint32_t identity)
	{
		return identity != 0 && std::any_of(
			_columns.begin(), _columns.end(),
			[identity](const auto& column)
			{ return column->_accessibilityIdentity == identity; });
	};
	std::erase_if(_accessibilityColumns,
		[&isLive](const AccessibilityColumnIdentity& column)
		{ return !isLive(column.ColumnIdentity); });
	for (auto& row : _accessibilityRows)
		std::erase_if(row.Cells,
			[&isLive](const AccessibilityCellIdentity& cell)
			{ return !isLive(cell.ColumnIdentity); });
	InvalidateAccessibilityVirtualIdentities();
}

bool DataGrid::EnsureAccessibilityVirtualIdentities() const
{
	if (!_accessibilityIdentitiesDirty) return true;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_buildingAccessibilityIdentities) return false;
	live->_buildingAccessibilityIdentities = true;
	auto resetBuilding = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_buildingAccessibilityIdentities = false;
	});
	const auto items = GetItemsView();
	if (!items)
	{
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		live->_accessibilityColumns.clear();
		live->_accessibilityRows.clear();
		live->_accessibilityRowIndexLookup.clear();
		live->_accessibilityNodeLookup.clear();
		live->_accessibilityIdentitiesDirty = false;
		return true;
	}
	const size_t revision = live->_accessibilityIdentityRevision;

	const auto oldColumns = live->_accessibilityColumns;
	const auto oldRows = live->_accessibilityRows;
	std::unordered_map<uint32_t, uint32_t> oldHeaderIds;
	oldHeaderIds.reserve(oldColumns.size());
	for (const auto& entry : oldColumns)
		oldHeaderIds.emplace(entry.ColumnIdentity, entry.HeaderId);

	std::vector<AccessibilityColumnIdentity> columns;
	columns.reserve(live->_columns.size());
	for (const auto& column : live->_columns)
	{
		if (column->_accessibilityIdentity == 0)
			column->_accessibilityIdentity = AllocateAccessibilityVirtualId();
		const auto found = oldHeaderIds.find(column->_accessibilityIdentity);
		columns.push_back({ column.get(), column->_accessibilityIdentity,
			found != oldHeaderIds.end()
				? found->second : AllocateAccessibilityVirtualId() });
	}

	std::vector<AccessibilityRowIdentity> rows;
	rows.reserve(oldRows.size());
	for (auto next : oldRows)
	{
		if (!next.Item
			|| next.Occurrence == DataGridCellInfo::InvalidIndex) continue;
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		if (!live->TryResolveItemOccurrence(
			next.Item, next.Occurrence, rowIndex))
		{
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared())
				return false;
			continue;
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared())
			return false;
		next.ViewIndex = rowIndex;
		if (next.RowId == 0) next.RowId = AllocateAccessibilityVirtualId();
		if (next.HeaderId == 0)
			next.HeaderId = AllocateAccessibilityVirtualId();
		const auto oldCells = std::move(next.Cells);
		next.Cells.clear();
		next.Cells.reserve(oldCells.size());
		for (auto cell : oldCells)
		{
			const auto column = std::find_if(
				columns.begin(), columns.end(),
				[&cell](const AccessibilityColumnIdentity& value)
				{ return value.ColumnIdentity == cell.ColumnIdentity; });
			if (column == columns.end() || cell.Id == 0
				|| !column->Column) continue;
			cell.Column = column->Column;
			cell.ColumnIndex = static_cast<size_t>(
				std::distance(columns.begin(), column));
			next.Cells.push_back(std::move(cell));
		}
		rows.push_back(std::move(next));
	}

	std::unordered_map<uint32_t, AccessibilityNodeLocator> lookup;
	size_t sparseCellCount = 0;
	for (const auto& row : rows) sparseCellCount += row.Cells.size();
	lookup.reserve(columns.size() + rows.size() * 2 + sparseCellCount);
	std::unordered_map<size_t, size_t> rowLookup;
	rowLookup.reserve(rows.size());
	for (size_t column = 0; column < columns.size(); ++column)
		if (columns[column].Column
			&& columns[column].Column->GetVisibility() == Visibility::Visible)
			lookup.emplace(columns[column].HeaderId,
				AccessibilityNodeLocator{
					AccessibilityNodeKind::ColumnHeader,
					DataGridCellInfo::InvalidIndex, column });
	for (size_t row = 0; row < rows.size(); ++row)
	{
		if (!rowLookup.emplace(rows[row].ViewIndex, row).second)
			return false;
		lookup.emplace(rows[row].RowId,
			AccessibilityNodeLocator{
				AccessibilityNodeKind::Row, rows[row].ViewIndex,
				DataGridCellInfo::InvalidIndex });
		lookup.emplace(rows[row].HeaderId,
			AccessibilityNodeLocator{
				AccessibilityNodeKind::RowHeader, rows[row].ViewIndex,
				DataGridCellInfo::InvalidIndex });
		for (const auto& cell : rows[row].Cells)
			if (cell.Column
				&& cell.Column->GetVisibility() == Visibility::Visible)
				lookup.emplace(cell.Id,
					AccessibilityNodeLocator{
						AccessibilityNodeKind::Cell,
						rows[row].ViewIndex, cell.ColumnIndex });
	}

	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| live->_accessibilityIdentityRevision != revision
		|| live->_columns.size() != columns.size()) return false;
	for (size_t index = 0; index < columns.size(); ++index)
	{
		if (live->_columns[index].get() != columns[index].Column
			|| live->_columns[index]->_accessibilityIdentity
				!= columns[index].ColumnIdentity) return false;
	}
	live->_accessibilityColumns = std::move(columns);
	live->_accessibilityRows = std::move(rows);
	live->_accessibilityRowIndexLookup = std::move(rowLookup);
	live->_accessibilityNodeLookup = std::move(lookup);
	live->_accessibilityIdentitiesDirty = false;
	return true;
}

DataGrid::AccessibilityRowIdentity*
DataGrid::FindAccessibilityRowIdentity(size_t rowIndex) const noexcept
{
	const auto found = _accessibilityRowIndexLookup.find(rowIndex);
	if (found == _accessibilityRowIndexLookup.end()
		|| found->second >= _accessibilityRows.size()) return nullptr;
	auto& row = _accessibilityRows[found->second];
	return row.ViewIndex == rowIndex ? &row : nullptr;
}

DataGrid::AccessibilityRowIdentity*
DataGrid::EnsureAccessibilityRowIdentity(
	size_t rowIndex, bool ensureCells) const
{
	if (!EnsureAccessibilityVirtualIdentities()) return nullptr;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	// A clean sparse identity table already proves this row belongs to the same
	// projection revision. Return it before invoking Count on a potentially lazy
	// million-row source; callers commonly ask for the row immediately after the
	// matching cell identity was created.
	if (live)
		if (auto* existing = live->FindAccessibilityRowIdentity(rowIndex))
		{
			(void)ensureCells;
			live->_accessibilityNodeLookup[existing->RowId] = {
				AccessibilityNodeKind::Row, rowIndex,
				DataGridCellInfo::InvalidIndex };
			live->_accessibilityNodeLookup[existing->HeaderId] = {
				AccessibilityNodeKind::RowHeader, rowIndex,
				DataGridCellInfo::InvalidIndex };
			for (const auto& cell : existing->Cells)
				if (cell.Column
					&& cell.Column->GetVisibility() == Visibility::Visible)
					live->_accessibilityNodeLookup[cell.Id] = {
						AccessibilityNodeKind::Cell, rowIndex, cell.ColumnIndex };
			return existing;
		}
	const auto items = live ? live->GetItemsView() : BindingListReference{};
	if (!live || !items) return nullptr;
	const size_t count = items.Get()->Count();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| rowIndex >= count) return nullptr;

	BindingSourceReference item;
	const bool read = items.Get()->TryGetItem(rowIndex, item);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| !read || !item) return nullptr;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (!live->TryGetItemOccurrenceAt(rowIndex, occurrence)) return nullptr;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()) return nullptr;

	AccessibilityRowIdentity next;
	next.Item = std::move(item);
	next.Occurrence = occurrence;
	next.ViewIndex = rowIndex;
	next.RowId = AllocateAccessibilityVirtualId();
	next.HeaderId = AllocateAccessibilityVirtualId();
	(void)ensureCells;
	const size_t sparseIndex = live->_accessibilityRows.size();
	live->_accessibilityRows.push_back(std::move(next));
	live->_accessibilityRowIndexLookup[rowIndex] = sparseIndex;
	auto* inserted = &live->_accessibilityRows.back();
	live->_accessibilityNodeLookup[inserted->RowId] = {
		AccessibilityNodeKind::Row, rowIndex,
		DataGridCellInfo::InvalidIndex };
	live->_accessibilityNodeLookup[inserted->HeaderId] = {
		AccessibilityNodeKind::RowHeader, rowIndex,
		DataGridCellInfo::InvalidIndex };
	for (const auto& cell : inserted->Cells)
		live->_accessibilityNodeLookup[cell.Id] = {
			AccessibilityNodeKind::Cell, rowIndex, cell.ColumnIndex };
	return inserted;
}

DataGrid::AccessibilityCellIdentity*
DataGrid::EnsureAccessibilityCellIdentity(
	size_t rowIndex, size_t columnIndex) const
{
	if (!EnsureAccessibilityVirtualIdentities()
		|| columnIndex >= _accessibilityColumns.size()
		|| !_accessibilityColumns[columnIndex].Column
		|| _accessibilityColumns[columnIndex].Column->GetVisibility()
			!= Visibility::Visible) return nullptr;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	auto* row = live
		? live->EnsureAccessibilityRowIdentity(rowIndex, false) : nullptr;
	if (!live || !row || columnIndex >= live->_accessibilityColumns.size())
		return nullptr;
	const auto& column = live->_accessibilityColumns[columnIndex];
	auto existing = std::find_if(
		row->Cells.begin(), row->Cells.end(),
		[&column](const AccessibilityCellIdentity& candidate)
		{ return candidate.ColumnIdentity == column.ColumnIdentity; });
	if (existing != row->Cells.end())
	{
		existing->Column = column.Column;
		existing->ColumnIndex = columnIndex;
		live->_accessibilityNodeLookup[existing->Id] = {
			AccessibilityNodeKind::Cell, rowIndex, columnIndex };
		return &*existing;
	}
	row->Cells.push_back({ column.Column, column.ColumnIdentity,
		columnIndex, AllocateAccessibilityVirtualId() });
	auto* inserted = &row->Cells.back();
	live->_accessibilityNodeLookup[inserted->Id] = {
		AccessibilityNodeKind::Cell, rowIndex, columnIndex };
	return inserted;
}

bool DataGrid::TryResolveAccessibilityVirtualNode(
	uint32_t id, AccessibilityNodeLocator& result) const
{
	result = {};
	if (id == 0 || !EnsureAccessibilityVirtualIdentities()) return false;
	const auto found = _accessibilityNodeLookup.find(id);
	if (found == _accessibilityNodeLookup.end()) return false;
	result = found->second;
	return true;
}

bool DataGrid::TryReadAccessibilityCellValue(
	size_t rowIndex, size_t columnIndex, std::wstring& result) const
{
	result.clear();
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	const auto items = GetItemsView();
	if (!items) return false;
	const size_t count = items.Get()->Count();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != items.Shared()
		|| rowIndex >= count || columnIndex >= live->_columns.size())
		return false;
	const uint32_t columnIdentity =
		live->_columns[columnIndex]->_accessibilityIdentity;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring bindingPath;
#endif
	CompiledBindingPathView compiledPath;
	bool useRowItemPath = false;
	if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(
		live->_columns[columnIndex].get()))
	{
		useRowItemPath = bound->GetBindingSourceKind()
			== DataGridBindingSourceKind::RowDataContext;
#if CUI_ENABLE_DYNAMIC_XAML
		if (useRowItemPath) bindingPath = bound->GetBindingPath();
#endif
		if (useRowItemPath) compiledPath = bound->GetCompiledBindingPath();
	}
	const auto isCurrent = [&]()
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return owner && owner->GetItemsView().Shared() == items.Shared()
			&& columnIndex < owner->_columns.size()
			&& owner->_columns[columnIndex]->_accessibilityIdentity
				== columnIdentity;
	};
	if (useRowItemPath)
	{
		BindingSourceReference item;
		const bool gotItem = items.Get()->TryGetItem(rowIndex, item);
		if (!isCurrent() || !gotItem || !item) return false;
		BindingValue value;
		bool read = false;
#if CUI_ENABLE_DYNAMIC_XAML
		if (!bindingPath.empty())
			read = TryGetBindingPathValue(
				*item.Get(), bindingPath, value);
#endif
		if (!isCurrent()) return false;
		if (!read && !compiledPath.Empty())
			read = TryGetBindingPathValue(
				*item.Get(), compiledPath, value);
		if (!isCurrent()) return false;
		if (read)
		{
			result = value.ToString();
			return true;
		}
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live && columnIndex < live->_columns.size()
		&& dynamic_cast<DataGridTemplateColumn*>(
			live->_columns[columnIndex].get()))
		return false;
	auto* row = live ? live->ResolveRow(rowIndex) : nullptr;
	auto* cell = row ? row->GetCell(columnIndex) : nullptr;
	if (!cell) return false;
	const ControlWeakReference cellLifetime(cell);
	const auto snapshot = cell->GetAccessibilitySnapshot();
	if (!isCurrent() || !cellLifetime.Get()) return false;
	result = snapshot.Value.empty() ? snapshot.Name : snapshot.Value;
	return !result.empty();
}

bool DataGrid::TryGetAccessibilityVirtualNode(
	uint32_t id, AccessibilityVirtualNode& result)
{
	result = {};
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)) return false;
	if (locator.RowIndex != DataGridCellInfo::InvalidIndex
		&& locator.RowIndex >= ItemCount()) return false;
	if (locator.ColumnIndex != DataGridCellInfo::InvalidIndex
		&& locator.ColumnIndex >= _accessibilityColumns.size()) return false;

	const ControlWeakReference ownerLifetime(this);
	const auto gridSnapshot = GetAccessibilitySnapshot();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	AccessibilityNodeLocator currentLocator;
	if (!live || !live->TryResolveAccessibilityVirtualNode(id, currentLocator))
		return false;
	locator = currentLocator;
	if (locator.RowIndex != DataGridCellInfo::InvalidIndex
		&& locator.RowIndex >= live->ItemCount()) return false;
	if (locator.ColumnIndex != DataGridCellInfo::InvalidIndex
		&& locator.ColumnIndex >= live->_accessibilityColumns.size()) return false;
	const std::wstring prefix = gridSnapshot.AutomationId.empty()
		? L"dataGrid" : gridSnapshot.AutomationId;
	result.Id = id;
	result.Enabled = live->IsEffectivelyEnabled();
	result.Visible = live->GetIsVisible();
	result.ReadOnly = true;
	result.RowSpan = 1;
	result.ColumnSpan = 1;

	switch (locator.Kind)
	{
	case AccessibilityNodeKind::ColumnHeader:
	{
		auto* column = _accessibilityColumns[locator.ColumnIndex].Column;
		if (!column) return false;
		result.ControlType = AutomationControlType::HeaderItem;
		result.ClassName = L"DataGridColumnHeader";
		result.KeyboardFocusable = false;
		result.HasKeyboardFocus = false;
		result.IsControlElement = true;
		result.IsContentElement = false;
		result.Patterns = AutomationPattern::ScrollItem
			| AutomationPattern::VirtualizedItem;
		if (_canUserSortColumns && column->GetCanUserSort())
			result.Patterns |= AutomationPattern::Invoke;
		result.Name = column->GetHeader().ToString();
		result.AutomationId = prefix + L".column-"
			+ std::to_wstring(id);
		result.Column = static_cast<int>(column->_visibleIndex);
		auto* presenter = GetColumnHeadersPresenter();
		auto* header = presenter
			? presenter->GetHeader(locator.ColumnIndex) : nullptr;
		(void)PublishAccessibilityBounds(header, result);
		result.Visible = result.Visible
			&& HasColumnHeaders(_headersVisibility);
		return true;
	}
	case AccessibilityNodeKind::Row:
	{
		auto* rowIdentity = live->EnsureAccessibilityRowIdentity(
			locator.RowIndex, false);
		if (!rowIdentity) return false;
		const uint32_t rowId = rowIdentity->RowId;
		result.ControlType = AutomationControlType::DataItem;
		result.ClassName = L"DataGridRow";
		result.KeyboardFocusable = false;
		result.HasKeyboardFocus = false;
		result.IsControlElement = true;
		result.IsContentElement = true;
		result.SelectionReturnsNullWhenEmpty = true;
		result.Patterns = AutomationPattern::ScrollItem
			| AutomationPattern::Selection;
		if (!_isReadOnly && !_visibleColumns.empty())
			result.Patterns |= AutomationPattern::Invoke;
		if (_selectionUnit != DataGridSelectionUnit::Cell)
			result.Patterns |= AutomationPattern::SelectionItem;
		result.AutomationId = prefix + L".row-" + std::to_wstring(id);
		result.Row = static_cast<int>(locator.RowIndex);
		result.Selected = IsIndexSelected(locator.RowIndex);
		std::wstring value;
		if (!_visibleColumns.empty())
			(void)TryReadAccessibilityCellValue(
				locator.RowIndex, FirstVisibleDisplayIndex(), value);
		AccessibilityNodeLocator currentLocator;
		if (!ownerLifetime.Get()
			|| !TryResolveAccessibilityVirtualNode(id, currentLocator)
			|| currentLocator.Kind != locator.Kind
			|| currentLocator.RowIndex != locator.RowIndex) return false;
		result.Name = value.empty()
			? L"Row " + std::to_wstring(locator.RowIndex + 1) : value;
		result.Value = result.Name;
		auto* row = ResolveRow(locator.RowIndex);
		(void)PublishAccessibilityBounds(row, result);
		if (!row)
			result.Patterns |= AutomationPattern::VirtualizedItem;
		return rowId == id;
	}
	case AccessibilityNodeKind::RowHeader:
	{
		if (!HasRowHeaders(_headersVisibility)) return false;
		auto* rowIdentity = live->EnsureAccessibilityRowIdentity(
			locator.RowIndex, false);
		if (!rowIdentity) return false;
		auto* row = ResolveRow(locator.RowIndex);
		auto* header = row ? row->GetRowHeader() : nullptr;
		if (!header || !header->GetIsVisible()) return false;
		result.ParentId = rowIdentity->RowId;
		result.ControlType = AutomationControlType::HeaderItem;
		result.ClassName = L"DataGridRowHeader";
		result.KeyboardFocusable = false;
		result.HasKeyboardFocus = false;
		result.IsControlElement = true;
		result.IsContentElement = false;
		result.Patterns = AutomationPattern::Invoke;
		result.Name = std::to_wstring(locator.RowIndex + 1);
		result.Value = result.Name;
		result.AutomationId = prefix + L".row-header-"
			+ std::to_wstring(id);
		result.Row = static_cast<int>(locator.RowIndex);
		result.Selected = IsIndexSelected(locator.RowIndex);
		(void)PublishAccessibilityBounds(header, result);
		return true;
	}
	case AccessibilityNodeKind::Cell:
	{
		auto* rowIdentity = live->EnsureAccessibilityRowIdentity(
			locator.RowIndex, false);
		auto* cellIdentity = live->EnsureAccessibilityCellIdentity(
			locator.RowIndex, locator.ColumnIndex);
		if (!rowIdentity || !cellIdentity) return false;
		const uint32_t rowId = rowIdentity->RowId;
		const uint32_t cellId = cellIdentity->Id;
		const uint32_t rowHeaderId = rowIdentity->HeaderId;
		const uint32_t columnIdentity = _accessibilityColumns[
			locator.ColumnIndex].ColumnIdentity;
		auto* column = _accessibilityColumns[locator.ColumnIndex].Column;
		if (!column || cellId != id) return false;
		result.ParentId = rowId;
		result.ControlType = AutomationControlType::Custom;
		result.ClassName = L"DataGridCell";
		result.IsControlElement = true;
		result.IsContentElement = true;
		result.Patterns = AutomationPattern::GridItem
			| AutomationPattern::TableItem
			| AutomationPattern::ScrollItem
			| AutomationPattern::Value;
		if (_selectionUnit != DataGridSelectionUnit::FullRow)
			result.Patterns |= AutomationPattern::SelectionItem;
		const auto* bound = dynamic_cast<const DataGridBoundColumn*>(column);
		const bool bindingReadOnly = bound
			&& (bound->GetBindingMode() == BindingMode::OneWay
				|| bound->GetBindingMode() == BindingMode::OneTime);
		result.ReadOnly = _isReadOnly || column->GetIsReadOnly()
			|| bindingReadOnly;
		if (!result.ReadOnly) result.Patterns |= AutomationPattern::Invoke;
		result.AutomationId = prefix + L".cell-" + std::to_wstring(id);
		result.Row = static_cast<int>(locator.RowIndex);
		result.Column = static_cast<int>(column->_visibleIndex);
		result.Selected = _selectionUnit == DataGridSelectionUnit::FullRow
			? IsIndexSelected(locator.RowIndex)
			: IsCellSelected(locator.RowIndex, locator.ColumnIndex);
		(void)TryReadAccessibilityCellValue(
			locator.RowIndex, locator.ColumnIndex, result.Value);
		AccessibilityNodeLocator currentLocator;
		if (!ownerLifetime.Get()
			|| !TryResolveAccessibilityVirtualNode(id, currentLocator)
			|| currentLocator.Kind != locator.Kind
			|| currentLocator.RowIndex != locator.RowIndex
			|| currentLocator.ColumnIndex != locator.ColumnIndex
			|| locator.ColumnIndex >= _accessibilityColumns.size()
			|| _accessibilityColumns[locator.ColumnIndex].ColumnIdentity
				!= columnIdentity) return false;
		column = _accessibilityColumns[locator.ColumnIndex].Column;
		if (!column) return false;
		auto* row = ResolveRow(locator.RowIndex);
		auto* cell = row ? row->GetCell(locator.ColumnIndex) : nullptr;
		std::wstring cellName;
		if (cell && dynamic_cast<DataGridTemplateColumn*>(column))
		{
			// Template columns have no automation Value without a clipboard
			// binding, but WPF still obtains the cell Name from the realized
			// cell peer. Keep these two accessibility contracts independent.
			const ControlWeakReference cellLifetime(cell);
			const auto cellSnapshot = cell->GetAccessibilitySnapshot();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			AccessibilityNodeLocator refreshedLocator;
			if (!live || !cell
				|| !live->TryResolveAccessibilityVirtualNode(id, refreshedLocator)
				|| refreshedLocator.Kind != AccessibilityNodeKind::Cell
				|| refreshedLocator.RowIndex != locator.RowIndex
				|| refreshedLocator.ColumnIndex != locator.ColumnIndex
				|| locator.ColumnIndex >= live->_accessibilityColumns.size()
				|| live->_accessibilityColumns[
					locator.ColumnIndex].ColumnIdentity != columnIdentity)
				return false;
			row = live->ResolveRow(locator.RowIndex);
			if (!row || row->GetCell(locator.ColumnIndex) != cell) return false;
			column = live->_accessibilityColumns[locator.ColumnIndex].Column;
			if (!column) return false;
			cellName = cellSnapshot.Name;
		}
		result.Name = !cellName.empty() ? std::move(cellName)
			: (result.Value.empty()
				? column->GetHeader().ToString() : result.Value);
		result.KeyboardFocusable = cell && cell->CanReceiveKeyboardFocus();
		result.HasKeyboardFocus = cell && cell->GetIsKeyboardFocused();
		auto* rowHeader = row ? row->GetRowHeader() : nullptr;
		if (HasRowHeaders(live->_headersVisibility)
			&& rowHeader && rowHeader->GetIsVisible())
			result.RowHeaderId = rowHeaderId;
		(void)PublishAccessibilityBounds(cell, result);
		if (!cell)
			result.Patterns |= AutomationPattern::VirtualizedItem;
		return true;
	}
	}
	return false;
}

size_t DataGrid::GetAccessibilityVirtualChildCount(
	uint32_t parentId) const
{
	if (!EnsureAccessibilityVirtualIdentities()) return 0;
	if (parentId == 0)
		return (HasColumnHeaders(_headersVisibility)
			? _visibleColumns.size() : 0)
			+ ItemCount();
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(parentId, locator)
		|| locator.Kind != AccessibilityNodeKind::Row
		|| locator.RowIndex >= ItemCount()) return 0;
	const auto* row = ResolveRow(locator.RowIndex);
	const auto* header = row ? row->GetRowHeader() : nullptr;
	const bool hasRowHeader = HasRowHeaders(_headersVisibility)
		&& header && header->GetIsVisible();
	return _visibleColumns.size() + (hasRowHeader ? 1 : 0);
}

bool DataGrid::TryGetAccessibilityVirtualChildAt(
	uint32_t parentId, size_t index, uint32_t& result) const
{
	result = 0;
	if (!EnsureAccessibilityVirtualIdentities()) return false;
	if (parentId == 0)
	{
		const size_t headerCount = HasColumnHeaders(_headersVisibility)
			? _visibleColumns.size() : 0;
		if (index < headerCount)
		{
			auto* column = GetColumnFromVisibleIndex(index);
			if (!column || column->_displayIndex >= _accessibilityColumns.size())
				return false;
			result = _accessibilityColumns[column->_displayIndex].HeaderId;
		}
		else
		{
			index -= headerCount;
			if (index >= ItemCount()) return false;
			auto* row = EnsureAccessibilityRowIdentity(index, false);
			if (!row) return false;
			result = row->RowId;
		}
		return result != 0;
	}
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(parentId, locator)
		|| locator.Kind != AccessibilityNodeKind::Row
		|| locator.RowIndex >= ItemCount()) return false;
	auto* rowIdentity = EnsureAccessibilityRowIdentity(
		locator.RowIndex, false);
	if (!rowIdentity) return false;
	const auto* row = ResolveRow(locator.RowIndex);
	const auto* header = row ? row->GetRowHeader() : nullptr;
	const bool hasRowHeader = HasRowHeaders(_headersVisibility)
		&& header && header->GetIsVisible();
	if (hasRowHeader)
	{
		if (index == 0)
		{
			result = rowIdentity->HeaderId;
			return result != 0;
		}
		--index;
	}
	if (index >= _visibleColumns.size()) return false;
	auto* column = GetColumnFromVisibleIndex(index);
	if (!column) return false;
	auto* cell = EnsureAccessibilityCellIdentity(
		locator.RowIndex, column->_displayIndex);
	if (!cell) return false;
	result = cell->Id;
	return result != 0;
}

bool DataGrid::TryGetAccessibilityVirtualSibling(
	uint32_t parentId, uint32_t id, bool next, uint32_t& result) const
{
	result = 0;
	if (!EnsureAccessibilityVirtualIdentities()) return false;
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)) return false;
	size_t index = DataGridCellInfo::InvalidIndex;
	if (parentId == 0)
	{
		const size_t headerCount = HasColumnHeaders(_headersVisibility)
			? _visibleColumns.size() : 0;
		if (locator.Kind == AccessibilityNodeKind::ColumnHeader)
		{
			auto* column = locator.ColumnIndex < _columns.size()
				? _columns[locator.ColumnIndex].get() : nullptr;
			if (!column || column->_visibleIndex
				== DataGridColumn::UnsetDisplayIndex) return false;
			index = column->_visibleIndex;
		}
		else if (locator.Kind == AccessibilityNodeKind::Row)
			index = headerCount + locator.RowIndex;
		else return false;
	}
	else
	{
		AccessibilityNodeLocator parent;
		if (!TryResolveAccessibilityVirtualNode(parentId, parent)
			|| parent.Kind != AccessibilityNodeKind::Row
			|| locator.RowIndex != parent.RowIndex) return false;
		const auto* realized = ResolveRow(parent.RowIndex);
		const auto* header = realized ? realized->GetRowHeader() : nullptr;
		const bool hasRowHeader = HasRowHeaders(_headersVisibility)
			&& header && header->GetIsVisible();
		if (locator.Kind == AccessibilityNodeKind::RowHeader)
		{
			if (!hasRowHeader) return false;
			index = 0;
		}
		else if (locator.Kind == AccessibilityNodeKind::Cell)
		{
			auto* column = locator.ColumnIndex < _columns.size()
				? _columns[locator.ColumnIndex].get() : nullptr;
			if (!column || column->_visibleIndex
				== DataGridColumn::UnsetDisplayIndex) return false;
			index = column->_visibleIndex + (hasRowHeader ? 1 : 0);
		}
		else return false;
	}
	const size_t count = GetAccessibilityVirtualChildCount(parentId);
	if (index == DataGridCellInfo::InvalidIndex
		|| (!next && index == 0) || (next && index + 1 >= count))
		return false;
	return TryGetAccessibilityVirtualChildAt(
		parentId, next ? index + 1 : index - 1, result);
}

bool DataGrid::TryHitTestAccessibilityVirtualNode(
	float localX, float localY, uint32_t& result) const
{
	result = 0;
	if (!EnsureAccessibilityVirtualIdentities()) return false;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	std::vector<uint32_t> ids;
	ids.reserve(_visibleColumns.size()
		+ GetRealizedItems().size() * 16);
	if (HasColumnHeaders(_headersVisibility))
		for (const auto* column : _visibleColumns)
			if (column && column->_displayIndex < _accessibilityColumns.size())
				ids.push_back(
					_accessibilityColumns[column->_displayIndex].HeaderId);
	for (const auto& [rowIndex, visual] : GetRealizedItems())
	{
		(void)visual;
		auto* row = EnsureAccessibilityRowIdentity(rowIndex, false);
		if (!row) continue;
		if (HasRowHeaders(_headersVisibility)) ids.push_back(row->HeaderId);
		auto* realizedRow = ResolveRow(rowIndex);
		if (realizedRow)
			for (auto* realizedCell : realizedRow->GetCells())
				if (realizedCell)
					if (auto* identity = EnsureAccessibilityCellIdentity(
						rowIndex, realizedCell->_columnIndex))
						ids.push_back(identity->Id);
		ids.push_back(row->RowId);
	}
	const auto renderPoint = ToRenderSpace(*this, localX, localY);
	auto hit = [ownerLifetime, renderPoint](uint32_t id)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!owner) return false;
		AccessibilityVirtualNode node;
		return owner->TryGetAccessibilityVirtualNode(id, node) && node.Visible
			&& IsPointInside(node.BoundsDip, renderPoint);
	};
	for (const uint32_t id : ids)
	{
		if (!ownerLifetime.Get()) return false;
		if (hit(id))
		{
			result = id;
			return true;
		}
	}
	return false;
}

AccessibilityVirtualContainerInfo
DataGrid::GetAccessibilityVirtualContainerInfo() const noexcept
{
	AccessibilityVirtualContainerInfo result;
	result.Patterns = AutomationPattern::Grid
		| AutomationPattern::Table
		| AutomationPattern::Selection;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	AccessibilityScrollInfo scrollInfo;
	const bool hasScroll = GetAccessibilityScrollInfo(scrollInfo);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return result;
	if (hasScroll)
		result.Patterns |= AutomationPattern::Scroll;
	result.CanSelectMultiple =
		live->GetSelectionMode() != SelectionMode::Single;
	result.IsSelectionRequired = false;
	try
	{
		const auto items = live->GetItemsView();
		const size_t rows = items ? items.Get()->Count() : 0;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared())
			return result;
		result.RowCount = static_cast<int>((std::min)(rows,
			static_cast<size_t>((std::numeric_limits<int>::max)())));
		result.ColumnCount = static_cast<int>((std::min)(
			live->_visibleColumns.size(),
			static_cast<size_t>((std::numeric_limits<int>::max)())));
	}
	catch (...)
	{
		result.RowCount = 0;
		result.ColumnCount = 0;
	}
	return result;
}

void DataGrid::GetAccessibilityVirtualSelection(
	std::vector<uint32_t>& result) const
{
	result.clear();
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	// One retry lets an observer which performs a single synchronous mutation
	// receive the new complete selection.  A continuously mutating observer
	// gets an empty answer, never a prefix assembled from different revisions.
	for (size_t attempt = 0; attempt != 2; ++attempt)
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		if (!live->EnsureAccessibilityVirtualIdentities())
		{
			if (attempt == 0 && ownerLifetime.Get()) continue;
			return;
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;

		const auto source = live->_source;
		const auto view = live->GetItemsView();
		const auto unit = live->_selectionUnit;
		const size_t rowRevision = live->_rowSelectionRevision;
		const size_t cellRevision = live->_cellSelectionRevision;
		const size_t accessibilityRevision =
			live->_accessibilityIdentityRevision;
		const SelectedIndexCollection selectedRows =
			live->GetSelectedIndices();
		const DataGridSelectedCellCollection selectedCells =
			live->_selectedCells;

		const auto current = [&]() -> DataGrid*
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && owner->_source.Shared() == source.Shared()
				&& owner->GetItemsView().Shared() == view.Shared()
				&& owner->_selectionUnit == unit
				&& owner->_rowSelectionRevision == rowRevision
				&& owner->_cellSelectionRevision == cellRevision
				&& owner->_accessibilityIdentityRevision
					== accessibilityRevision
				? owner : nullptr;
		};

		std::vector<uint32_t> candidate;
		bool invalidated = false;
		try
		{
			if (unit != DataGridSelectionUnit::Cell)
			{
				candidate.reserve(selectedRows.size());
				for (const int index : selectedRows)
				{
					if (index < 0) continue;
					live = current();
					if (!live)
					{
						invalidated = true;
						break;
					}
					auto* row = live->EnsureAccessibilityRowIdentity(
						static_cast<size_t>(index), false);
					live = current();
					if (!live)
					{
						invalidated = true;
						break;
					}
					if (row) candidate.push_back(row->RowId);
				}
			}
			if (!invalidated && unit != DataGridSelectionUnit::FullRow)
			{
				candidate.reserve(candidate.size() + selectedCells.size());
				for (const auto& selected : selectedCells)
				{
					live = current();
					if (!live)
					{
						invalidated = true;
						break;
					}
					auto* row = live->EnsureAccessibilityRowIdentity(
						selected.RowIndex, false);
					live = current();
					if (!live)
					{
						invalidated = true;
						break;
					}
					if (!row || row->Item.Shared() != selected.Item.Shared()
						|| row->Occurrence != selected._itemOccurrence) continue;
					auto* cell = live->EnsureAccessibilityCellIdentity(
						selected.RowIndex, selected.ColumnIndex);
					live = current();
					if (!live)
					{
						invalidated = true;
						break;
					}
					if (cell) candidate.push_back(cell->Id);
				}
			}
		}
		catch (...)
		{
			if (!current()) invalidated = true;
			else throw;
		}
		if (!invalidated && current())
		{
			result.swap(candidate);
			return;
		}
	}
}

bool DataGrid::GetAccessibilityVirtualItemAt(
	int row, int column, uint32_t& result) const
{
	result = 0;
	if (row < 0 || column < 0
		|| !EnsureAccessibilityVirtualIdentities()
		|| static_cast<size_t>(row) >= ItemCount()
		|| static_cast<size_t>(column) >= _visibleColumns.size())
		return false;
	auto* visibleColumn = GetColumnFromVisibleIndex(
		static_cast<size_t>(column));
	if (!visibleColumn) return false;
	auto* cellIdentity = EnsureAccessibilityCellIdentity(
		static_cast<size_t>(row), visibleColumn->_displayIndex);
	if (!cellIdentity) return false;
	result = cellIdentity->Id;
	(void)const_cast<DataGrid*>(this)
		->ScrollAccessibilityVirtualNodeIntoView(result);
	return result != 0;
}

void DataGrid::GetAccessibilityVirtualColumnHeaders(
	std::vector<uint32_t>& result) const
{
	result.clear();
	if (!HasColumnHeaders(_headersVisibility)
		|| !EnsureAccessibilityVirtualIdentities()) return;
	result.reserve(_visibleColumns.size());
	for (const auto* column : _visibleColumns)
		if (column && column->_displayIndex < _accessibilityColumns.size())
			result.push_back(
				_accessibilityColumns[column->_displayIndex].HeaderId);
}

void DataGrid::GetAccessibilityVirtualRowHeaders(
	std::vector<uint32_t>& result) const
{
	result.clear();
	if (!HasRowHeaders(_headersVisibility)
		|| !EnsureAccessibilityVirtualIdentities()) return;
	result.reserve(GetRealizedItems().size());
	for (const auto& [index, visual] : GetRealizedItems())
	{
		(void)visual;
		auto* row = ResolveRow(index);
		auto* header = row ? row->GetRowHeader() : nullptr;
		if (header && header->GetIsVisible())
			if (auto* identity = EnsureAccessibilityRowIdentity(index, false))
				result.push_back(identity->HeaderId);
	}
}

AutomationOperationResult DataGrid::FocusAccessibilityVirtualNode(uint32_t id)
{
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator))
		return AutomationOperationResult::ElementNotAvailable;
	if (locator.Kind != AccessibilityNodeKind::Cell)
		return AutomationOperationResult::InvalidOperation;
	if (!IsEffectivelyEnabled())
		return AutomationOperationResult::ElementNotEnabled;

	const ControlWeakReference ownerLifetime(this);
	auto resolveCell = [id](DataGrid* owner,
		AccessibilityNodeLocator& resolved) -> DataGridCell*
	{
		if (!owner
			|| !owner->TryResolveAccessibilityVirtualNode(id, resolved)
			|| resolved.Kind != AccessibilityNodeKind::Cell)
			return nullptr;
		auto* row = owner->ResolveRow(resolved.RowIndex);
		auto* cell = row ? row->GetCell(resolved.ColumnIndex) : nullptr;
		return row && row->GetDataGridOwner() == owner
			&& cell && cell->GetRowOwner() == row ? cell : nullptr;
	};

	auto* cell = resolveCell(this, locator);
	// Match WPF's item-peer contract: a virtualized cell must be realized before
	// its cell peer can accept keyboard focus.
	if (!cell) return AutomationOperationResult::ElementNotAvailable;
	if (!cell->IsEffectivelyEnabled())
		return AutomationOperationResult::ElementNotEnabled;
	if (!cell->CanReceiveKeyboardFocus())
		return AutomationOperationResult::InvalidOperation;

	const size_t rowIndex = locator.RowIndex;
	const size_t columnIndex = locator.ColumnIndex;
	if (!SetCurrentCell(rowIndex, columnIndex))
		return ownerLifetime.Get()
			? AutomationOperationResult::InvalidOperation
			: AutomationOperationResult::ElementNotAvailable;

	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = resolveCell(live, locator);
	if (!live || !cell || locator.RowIndex != rowIndex
		|| locator.ColumnIndex != columnIndex)
		return AutomationOperationResult::ElementNotAvailable;
	if (!live->_currentCell.IsValid()
		|| live->_currentCell.RowIndex != rowIndex
		|| live->_currentCell.ColumnIndex != columnIndex)
		return AutomationOperationResult::InvalidOperation;
	if (!cell->IsEffectivelyEnabled())
		return AutomationOperationResult::ElementNotEnabled;
	if (!cell->CanReceiveKeyboardFocus())
		return AutomationOperationResult::InvalidOperation;

	const ControlWeakReference cellLifetime(cell);
	if (!cell->Focus())
		return !ownerLifetime.Get() || !cellLifetime.Get()
			? AutomationOperationResult::ElementNotAvailable
			: AutomationOperationResult::InvalidOperation;

	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	AccessibilityNodeLocator focusedLocator;
	if (!live || !cell
		|| resolveCell(live, focusedLocator) != cell
		|| focusedLocator.RowIndex != rowIndex
		|| focusedLocator.ColumnIndex != columnIndex)
		return AutomationOperationResult::ElementNotAvailable;
	return cell->GetIsKeyboardFocused()
		? AutomationOperationResult::Succeeded
		: AutomationOperationResult::InvalidOperation;
}

bool DataGrid::TryGetAccessibilityVirtualFocusedNode(uint32_t& result) const
{
	result = 0;
	if (!EnsureAccessibilityVirtualIdentities()) return false;
	auto* window = GetPresentationWindow();
	auto* cell = window
		? dynamic_cast<DataGridCell*>(window->GetKeyboardFocusedElement())
		: nullptr;
	auto* row = cell ? cell->GetRowOwner() : nullptr;
	if (!row || row->GetDataGridOwner() != this) return false;
	const size_t rowIndex = row->ItemIndex();
	const size_t columnIndex = cell->_columnIndex;
	auto* identity = EnsureAccessibilityCellIdentity(rowIndex, columnIndex);
	if (!identity || columnIndex >= _accessibilityColumns.size()
		|| ResolveRow(rowIndex) != row
		|| row->GetCell(columnIndex) != cell)
		return false;
	result = identity->Id;
	return result != 0;
}

bool DataGrid::InvokeAccessibilityVirtualNode(uint32_t id)
{
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)
		|| !IsEffectivelyEnabled()) return false;
	const ControlWeakReference ownerLifetime(this);
	if (locator.Kind == AccessibilityNodeKind::ColumnHeader)
	{
		auto* column = locator.ColumnIndex < _accessibilityColumns.size()
			? _accessibilityColumns[locator.ColumnIndex].Column : nullptr;
		const bool invoked = column && _canUserSortColumns
			&& column->GetCanUserSort() && PerformSort(*column, false);
		if (invoked)
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->NotifyAccessibilityVirtualChanged(
					id, AccessibilityChange::Invoke);
		return invoked;
	}
	if (locator.Kind == AccessibilityNodeKind::RowHeader)
	{
		if (!ScrollAccessibilityVirtualNodeIntoView(id)) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
			|| locator.Kind != AccessibilityNodeKind::RowHeader) return false;
		auto* row = live->ResolveRow(locator.RowIndex);
		if (!row || !live->HandleRowHeaderClick(
			*row, ModifierKeys::None)) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live) live->NotifyAccessibilityVirtualChanged(
			id, AccessibilityChange::Invoke);
		return live != nullptr;
	}
	if (locator.Kind != AccessibilityNodeKind::Row
		&& locator.Kind != AccessibilityNodeKind::Cell) return false;
	if (_isReadOnly || _visibleColumns.empty()) return false;
	if (!ScrollAccessibilityVirtualNodeIntoView(id)) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
		|| (locator.Kind != AccessibilityNodeKind::Row
			&& locator.Kind != AccessibilityNodeKind::Cell)) return false;
	size_t rowIndex = locator.RowIndex;
	size_t columnIndex = locator.Kind == AccessibilityNodeKind::Cell
		? locator.ColumnIndex : live->FirstVisibleDisplayIndex();
	if (columnIndex >= live->_columns.size()
		|| live->_columns[columnIndex]->GetIsReadOnly()) return false;
	if (locator.Kind == AccessibilityNodeKind::Row
		&& live->_currentCell.IsValid()
		&& live->_currentCell.RowIndex == rowIndex)
	{
		if (auto* current = live->ResolveCurrentCellContainer();
			current && current->GetIsEditing())
		{
			const bool committed = live->CommitEdit();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (committed && live)
				live->NotifyAccessibilityVirtualChanged(
					id, AccessibilityChange::Invoke);
			return committed && live;
		}
	}
	if (locator.Kind == AccessibilityNodeKind::Row)
	{
		live->ListBox::UnselectAll();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
			|| locator.Kind != AccessibilityNodeKind::Row) return false;
		if (!live->ApplySelectedCells({})) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
			|| locator.Kind != AccessibilityNodeKind::Row) return false;
		rowIndex = locator.RowIndex;
		columnIndex = live->FirstVisibleDisplayIndex();
	}
	if (!live->SetCurrentCell(rowIndex, columnIndex)) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (locator.Kind == AccessibilityNodeKind::Cell)
	{
		(void)live->ApplySelectionForCellInput(
			rowIndex, columnIndex, ModifierKeys::None, false, false);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
	}
	if (auto* cell = live->ResolveCurrentCellContainer();
		cell && cell->GetIsEditing())
	{
		live->NotifyAccessibilityVirtualChanged(
			id, AccessibilityChange::Invoke);
		return true;
	}
	const bool invoked = live->BeginEdit();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (invoked && live) live->NotifyAccessibilityVirtualChanged(
		id, AccessibilityChange::Invoke);
	return invoked && live;
}

bool DataGrid::SetAccessibilityVirtualNodeValue(
	uint32_t id, const std::wstring& value)
{
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)
		|| locator.Kind != AccessibilityNodeKind::Cell
		|| locator.ColumnIndex >= _columns.size()
		|| _isReadOnly || _columns[locator.ColumnIndex]->GetIsReadOnly()
		|| !IsEffectivelyEnabled()) return false;
	if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(
		_columns[locator.ColumnIndex].get());
		bound && (bound->GetBindingMode() == BindingMode::OneWay
			|| bound->GetBindingMode() == BindingMode::OneTime)) return false;
	const ControlWeakReference ownerLifetime(this);
	if (!ScrollAccessibilityVirtualNodeIntoView(id)) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
		|| locator.Kind != AccessibilityNodeKind::Cell
		|| locator.ColumnIndex >= live->_columns.size()
		|| live->_isReadOnly
		|| live->_columns[locator.ColumnIndex]->GetIsReadOnly()) return false;
	if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(
		live->_columns[locator.ColumnIndex].get());
		bound && (bound->GetBindingMode() == BindingMode::OneWay
			|| bound->GetBindingMode() == BindingMode::OneTime)) return false;
	if (!live->SetCurrentCell(locator.RowIndex, locator.ColumnIndex))
		return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
		|| locator.Kind != AccessibilityNodeKind::Cell
		|| !(live->_currentCell.IsValid()
			&& live->_currentCell.RowIndex == locator.RowIndex
			&& live->_currentCell.ColumnIndex == locator.ColumnIndex)) return false;
	auto* cell = live->ResolveCurrentCellContainer();
	if (!cell) return false;
	if (!cell->GetIsEditing() && !live->BeginEdit()) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = live ? live->ResolveCurrentCellContainer() : nullptr;
	auto* editor = cell ? cell->GetEditingElement() : nullptr;
	if (!live || !editor) return false;
	const ControlWeakReference editorLifetime(editor);
	// WPF's DataGridTemplateColumn uses ClipboardContentBinding as the
	// automation value contract.  With the default null binding SetValue
	// enters edit mode, but is otherwise a successful no-op regardless of
	// which automation patterns the editing template happens to expose.
	if (locator.ColumnIndex < live->_columns.size()
		&& dynamic_cast<DataGridTemplateColumn*>(
			live->_columns[locator.ColumnIndex].get()))
	{
		return ownerLifetime.Get() != nullptr;
	}
	// Value providers are extensible and may synchronously detach/delete the
	// editing element. Keep the peer alive until the call unwinds; the weak
	// owner/editor checks below decide whether the edit can continue.
	auto editorPeer = editor->AcquireAutomationPeer();
	if (!editorPeer) return false;
	bool wroteValue = editorPeer->SetValue(value)
		== AutomationOperationResult::Succeeded;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	editor = dynamic_cast<Control*>(editorLifetime.Get());
	if (!live || !editor
		|| !live->TryResolveAccessibilityVirtualNode(id, locator)
		|| locator.Kind != AccessibilityNodeKind::Cell) return false;
	if (!wroteValue)
	{
		if (auto* checkBox = dynamic_cast<CheckBox*>(editor))
		{
			const auto text = Trim(value);
			NullableBool checked;
			if (EqualsIgnoreCase(text, L"true")) checked = NullableBool(true);
			else if (EqualsIgnoreCase(text, L"false"))
				checked = NullableBool(false);
			else if (text.empty() && checkBox->GetIsThreeState())
				checked = NullableBool{};
			else return false;
			wroteValue = checkBox->TrySetCurrentPropertyValue(
				ToggleButton::IsCheckedProperty(), BindingValue(checked));
		}
	}
	if (!wroteValue) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!editorLifetime.Get())
	{
		live->NotifyAccessibilityVirtualChanged(
			id, AccessibilityChange::Value);
		return true;
	}
	const bool committed = live->CommitEdit();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (committed && live) live->NotifyAccessibilityVirtualChanged(
		id, AccessibilityChange::Value);
	return committed && live;
}

bool DataGrid::SelectAccessibilityVirtualNode(
	uint32_t id, AccessibilitySelectionAction action)
{
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)
		|| !IsEffectivelyEnabled()) return false;
	if (locator.Kind == AccessibilityNodeKind::Row)
	{
		if (_selectionUnit == DataGridSelectionUnit::Cell
			|| locator.RowIndex >= ItemCount()) return false;
		const bool selected = IsIndexSelected(locator.RowIndex);
		if (action == AccessibilitySelectionAction::Add)
		{
			if (selected) return true;
			if (GetSelectionMode() == SelectionMode::Single
				&& !GetSelectedIndices().empty()) return false;
			const ControlWeakReference ownerLifetime(this);
			RequestItemSelection(locator.RowIndex, true);
			return ownerLifetime.Get() != nullptr;
		}
		if (action == AccessibilitySelectionAction::Remove)
		{
			if (!selected) return true;
			const ControlWeakReference ownerLifetime(this);
			RequestItemSelection(locator.RowIndex, false);
			return ownerLifetime.Get() != nullptr;
		}
		const ControlWeakReference ownerLifetime(this);
		NotifyItemClicked(
			locator.RowIndex, MouseButton::Left, ModifierKeys::None);
		return ownerLifetime.Get() != nullptr;
	}
	if (locator.Kind != AccessibilityNodeKind::Cell
		|| _selectionUnit == DataGridSelectionUnit::FullRow
		|| locator.RowIndex >= ItemCount()
		|| locator.ColumnIndex >= _columns.size()) return false;
	DataGridCellInfo target;
	if (!TryCreateCellInfo(
		locator.RowIndex, locator.ColumnIndex, target)) return false;
	const bool selected = _selectedCells.Contains(target);
	if (action == AccessibilitySelectionAction::Remove)
	{
		if (!selected) return true;
		return UnselectCell(locator.RowIndex, locator.ColumnIndex);
	}
	if (action == AccessibilitySelectionAction::Add)
	{
		if (selected) return true;
		if (GetSelectionMode() == SelectionMode::Single
			&& !_selectedCells.empty()) return false;
		if (_selectedCells.IsRegionBacked())
		{
			auto next = _selectedCells;
			if (!next.Include(target))
				next._cells.push_back(std::move(target));
			return ApplySelectedCellCollection(std::move(next));
		}
		auto next = _selectedCells._cells;
		next.push_back(std::move(target));
		return ApplySelectedCells(std::move(next));
	}
	if (_selectionUnit == DataGridSelectionUnit::CellOrRowHeader
		&& !GetSelectedIndices().empty())
	{
		const ControlWeakReference ownerLifetime(this);
		if (!DeselectAllRowsWithoutCellSync()) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->TryResolveAccessibilityVirtualNode(id, locator)
			|| locator.Kind != AccessibilityNodeKind::Cell) return false;
		DataGridCellInfo refreshed;
		if (!live->TryCreateCellInfo(
			locator.RowIndex, locator.ColumnIndex, refreshed)) return false;
		return live->ApplySelectedCells({ std::move(refreshed) });
	}
	return ApplySelectedCells({ std::move(target) });
}

bool DataGrid::ScrollAccessibilityVirtualNodeIntoView(uint32_t id)
{
	AccessibilityNodeLocator locator;
	if (!TryResolveAccessibilityVirtualNode(id, locator)) return false;
	if (locator.Kind == AccessibilityNodeKind::RowHeader
		&& !HasRowHeaders(_headersVisibility)) return false;
	const ControlWeakReference ownerLifetime(this);
	if (locator.RowIndex != DataGridCellInfo::InvalidIndex)
	{
		if (locator.RowIndex >= ItemCount()
			|| !BringItemIntoView(locator.RowIndex)) return false;
		if (!ownerLifetime.Get()) return false;
		if (!TryResolveAccessibilityVirtualNode(id, locator)) return false;
	}
	if (locator.ColumnIndex != DataGridCellInfo::InvalidIndex)
	{
		if (!BringColumnIntoView(locator.ColumnIndex)) return false;
		if (!ownerLifetime.Get()) return false;
	}
	return true;
}

bool DataGrid::GetAccessibilityScrollInfo(
	AccessibilityScrollInfo& result) const noexcept
{
	result = {};
	auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get());
	if (!scroll) return false;
	try
	{
		return scroll->GetAutomationPeer()
			.GetAccessibilityScrollInfo(result);
	}
	catch (...)
	{
		result = {};
		return false;
	}
}

bool DataGrid::ScrollAccessibility(
	AccessibilityScrollAmount horizontal,
	AccessibilityScrollAmount vertical)
{
	auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get());
	return scroll && scroll->GetAutomationPeer()
		.ScrollAccessibility(horizontal, vertical);
}

bool DataGrid::SetAccessibilityScrollPercent(
	double horizontalPercent, double verticalPercent)
{
	auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get());
	return scroll && scroll->GetAutomationPeer()
		.SetAccessibilityScrollPercent(
			horizontalPercent, verticalPercent);
}

bool DataGrid::IsCellSelected(
	size_t rowIndex, size_t columnIndex) const
{
	DataGridCellInfo info;
	return TryCreateCellInfo(rowIndex, columnIndex, info)
		&& ContainsCellIdentity(_selectedCells, info);
}

bool DataGrid::RefreshSelectedCellContainers(
	bool suppressRoutedEvents, bool onlyStaleRows)
{
	ControlWeakReference ownerLifetime(this);
	const size_t projectionRevision = _cellSelectionRevision;
	std::vector<ControlWeakReference> rows;
	std::vector<ControlWeakReference> cells;
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index));
		if (!row || (onlyStaleRows
			&& row->_appliedCellSelectionRevision == projectionRevision))
			continue;
		rows.emplace_back(row);
		for (auto* cell : row->GetCells())
			if (cell) cells.emplace_back(cell);
	}
	if (!cells.empty())
	{
		_updatingCellSelectionVisuals = true;
		try
		{
			for (auto& reference : cells)
			{
				auto* cell = dynamic_cast<DataGridCell*>(reference.Get());
				if (!cell || !cell->GetRowOwner()) continue;
				// The normal first viewport has no selected cells. Avoid asking
				// IsCellSelected to materialize the full occurrence-identity cache just
				// to prove that each of the few realized cells is false. Re-check the
				// collection for every cell because a routed deselection handler may
				// change selection reentrantly.
				const bool selected = !_selectedCells.empty()
					&& IsCellSelected(
						cell->GetRowOwner()->ItemIndex(), cell->_columnIndex);
				cell->SetCurrentIsSelected(selected,
					suppressRoutedEvents);
				if (!ownerLifetime.Get()) return false;
			}
		}
		catch (...)
		{
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->_updatingCellSelectionVisuals = false;
			throw;
		}
		_updatingCellSelectionVisuals = false;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live && live->_cellSelectionRevision == projectionRevision)
		for (const auto& reference : rows)
			if (auto* row = dynamic_cast<DataGridRow*>(reference.Get());
				row && row->GetDataGridOwner() == live)
				row->_appliedCellSelectionRevision = projectionRevision;
	return true;
}

void DataGrid::RefreshSelectedCellContainersAfterRollback() noexcept
{
	if (!_selectedCellsVisualRefreshPending) return;
	ControlWeakReference ownerLifetime(this);
	try
	{
		if (!RefreshSelectedCellContainers(true)) return;
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_selectedCellsVisualRefreshPending = false;
	}
	catch (...)
	{
		// Rollback must preserve the original transaction failure.  A later
		// realization pass retries the visual projection if an observer throws.
	}
}

bool DataGrid::RaiseSelectedCellsChanged(
	const DataGridSelectedCellCollection& previous,
	bool ignoreLocators)
{
	return RaiseSelectedCellsChangedCore(
		previous, _selectedCells, ignoreLocators);
}

bool DataGrid::RaiseSelectedCellsChangedCore(
	const DataGridSelectedCellCollection& previous,
	const DataGridSelectedCellCollection& current,
	bool ignoreLocators)
{
	ControlWeakReference ownerLifetime(this);
	DataGridSelectedCellsChangedEventArgs args;
	(void)ignoreLocators;
	args.RemovedCells = previous.Difference(current);
	// RemovedCells describes the old logical domain. Bind every lazy region to
	// its retained immutable snapshot: Reset/source reentrancy may already have
	// retired the live occurrence tokens before a handler enumerates the delta.
	for (auto& region : args.RemovedCells._regions)
		region.UseSnapshotLocators = true;
	args.AddedCells = current.Difference(previous);
	if (args.AddedCells.empty() && args.RemovedCells.empty()) return true;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->InvalidateCommandState();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	cui::framework::EventAccess::RaiseWhile(
		live->SelectedCellsChanged,
		[&]()
		{
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			const auto columnIsLive = [live](DataGridColumn* column)
			{
				return !column || std::any_of(
					live->_columns.begin(), live->_columns.end(),
					[column](const auto& candidate)
					{ return candidate.get() == column; });
			};
			const auto collectionColumnsAreLive = [&](const auto& cells)
			{
				return std::all_of(cells._cells.begin(), cells._cells.end(),
					[&](const DataGridCellInfo& cell)
					{ return columnIsLive(cell.Column); })
					&& std::all_of(cells._regions.begin(), cells._regions.end(),
						[&](const auto& region)
						{
							return std::all_of(region.Columns.begin(),
								region.Columns.end(), columnIsLive);
						});
			};
			return collectionColumnsAreLive(args.AddedCells)
				&& collectionColumnsAreLive(args.RemovedCells);
		},
		live, args);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	// WPF only enumerates a virtual delta when an automation listener requests
	// per-cell events. CUI does not yet expose UiaClientsAreListening, so publish
	// one authoritative owner-selection invalidation for a region delta. The
	// Selection.GetSelection path below remains exact and enumerates lazily when
	// a client actually asks for the selected providers.
	if (args.AddedCells.IsRegionBacked()
		|| args.RemovedCells.IsRegionBacked())
	{
		live->NotifyAccessibilityStateChanged();
		return ownerLifetime.Get() != nullptr;
	}
	// A source replacement retires the complete old occurrence domain. UIA row
	// identities for that domain were invalidated at the boundary, so attempting
	// to resolve a sparse RemovedCells payload in the new source would both invent
	// unrelated providers and linearly scan a lazy source for each old cell.
	if (args.RemovedCells._ownerSourceIdentity
		&& args.RemovedCells._ownerSourceIdentity.Shared()
			!= live->_source.Shared())
	{
		live->NotifyAccessibilityStateChanged();
		return ownerLifetime.Get() != nullptr;
	}
	// WPF raises the public event first.  A handler can legally update the
	// selection again, so automation classifies the original delta against the
	// final live selection domain after that callback completes.
	if (!live->EnsureAccessibilityVirtualIdentities())
		return ownerLifetime.Get() != nullptr;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	const auto findId = [ownerLifetime](const DataGridCellInfo& info)
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return uint32_t{ 0 };
		auto row = std::find_if(
			live->_accessibilityRows.begin(), live->_accessibilityRows.end(),
			[&info](const AccessibilityRowIdentity& candidate)
			{
				return candidate.Item.Shared() == info.Item.Shared()
					&& candidate.Occurrence == info._itemOccurrence;
			});
		if (row == live->_accessibilityRows.end())
		{
			size_t rowIndex = DataGridCellInfo::InvalidIndex;
			if (!live->TryResolveItemOccurrence(
				info.Item, info._itemOccurrence, rowIndex)) return uint32_t{ 0 };
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return uint32_t{ 0 };
			if (!live->EnsureAccessibilityRowIdentity(rowIndex, false))
				return uint32_t{ 0 };
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return uint32_t{ 0 };
			row = std::find_if(
				live->_accessibilityRows.begin(),
				live->_accessibilityRows.end(),
				[&info](const AccessibilityRowIdentity& candidate)
				{
					return candidate.Item.Shared() == info.Item.Shared()
						&& candidate.Occurrence == info._itemOccurrence;
				});
			if (row == live->_accessibilityRows.end()) return uint32_t{ 0 };
		}
		const size_t rowIndex = row->ViewIndex;
		auto* cell = live->EnsureAccessibilityCellIdentity(
			rowIndex, info.ColumnIndex);
		return cell ? cell->Id : uint32_t{ 0 };
	};
	std::vector<uint32_t> removedIds;
	std::vector<uint32_t> addedIds;
	removedIds.reserve(args.RemovedCells.size());
	addedIds.reserve(args.AddedCells.size());
	for (const auto& cell : args.RemovedCells)
		if (const uint32_t id = findId(cell); id != 0)
			removedIds.push_back(id);
	for (const auto& cell : args.AddedCells)
		if (const uint32_t id = findId(cell); id != 0)
			addedIds.push_back(id);
	if (live->_selectedCells.size() == 1 && args.AddedCells.size() == 1)
	{
		if (!addedIds.empty())
		{
			live->NotifyAccessibilityVirtualChanged(
				addedIds.front(), AccessibilityChange::SelectionSelected);
			if (!ownerLifetime.Get()) return false;
		}
	}
	else
	{
		for (const uint32_t id : addedIds)
		{
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->NotifyAccessibilityVirtualChanged(
				id, AccessibilityChange::SelectionAdded);
		}
		for (const uint32_t id : removedIds)
		{
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->NotifyAccessibilityVirtualChanged(
				id, AccessibilityChange::SelectionRemoved);
		}
	}
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::ApplySelectedCells(std::vector<DataGridCellInfo> cells)
{
	const bool outermost = _selectedCellsApplyDepth == 0;
	if (outermost) _selectedCellsApplyOld = _selectedCells;
	++_selectedCellsApplyDepth;
	ControlWeakReference ownerLifetime(this);
	try
	{
	const auto items = GetItemsView();
	std::vector<DataGridCellInfo> normalized;
	normalized.reserve(cells.size());
	for (auto& candidate : cells)
	{
		if (!candidate.Item || !candidate.Column || !items) continue;
		const auto column = std::find_if(
			_columns.begin(), _columns.end(),
			[&candidate](const auto& value)
			{ return value.get() == candidate.Column; });
		if (column == _columns.end()) continue;
		const size_t columnIndex = static_cast<size_t>(
			std::distance(_columns.begin(), column));
		size_t rowIndex = DataGridCellInfo::InvalidIndex;
		if (candidate.RowIndex < items.Get()->Count())
		{
			BindingSourceReference atIndex;
			if (items.Get()->TryGetItem(candidate.RowIndex, atIndex)
				&& atIndex.Shared() == candidate.Item.Shared())
			{
				size_t occurrence = DataGridCellInfo::InvalidIndex;
				if (!TryGetItemOccurrenceAt(
					candidate.RowIndex, occurrence)) continue;
				if (candidate._itemOccurrence == DataGridCellInfo::InvalidIndex
					|| candidate._itemOccurrence == occurrence)
				{
					candidate._itemOccurrence = occurrence;
					rowIndex = candidate.RowIndex;
				}
			}
		}
		if (rowIndex == DataGridCellInfo::InvalidIndex)
		{
			if (candidate._itemOccurrence != DataGridCellInfo::InvalidIndex)
			{
				size_t resolvedIndex = DataGridCellInfo::InvalidIndex;
				if (TryResolveItemOccurrence(
					candidate.Item, candidate._itemOccurrence, resolvedIndex))
					rowIndex = resolvedIndex;
			}
		}
		if (rowIndex == DataGridCellInfo::InvalidIndex)
		{
			for (size_t index = 0; index < items.Get()->Count(); ++index)
			{
				BindingSourceReference item;
				if (items.Get()->TryGetItem(index, item)
					&& item.Shared() == candidate.Item.Shared())
				{
					size_t occurrence = DataGridCellInfo::InvalidIndex;
					if (!TryGetItemOccurrenceAt(index, occurrence)) continue;
					if (candidate._itemOccurrence == DataGridCellInfo::InvalidIndex
						|| candidate._itemOccurrence == occurrence)
					{
						candidate._itemOccurrence = occurrence;
						rowIndex = index;
						break;
					}
				}
			}
		}
		if (rowIndex == DataGridCellInfo::InvalidIndex) continue;
		candidate.RowIndex = rowIndex;
		candidate.ColumnIndex = columnIndex;
		candidate.Column = column->get();
		if (!ContainsCellIdentity(normalized, candidate))
			normalized.push_back(std::move(candidate));
	}
	std::sort(normalized.begin(), normalized.end(),
		[](const DataGridCellInfo& left, const DataGridCellInfo& right)
		{
			return left.RowIndex != right.RowIndex
				? left.RowIndex < right.RowIndex
				: left.ColumnIndex < right.ColumnIndex;
		});
	_selectedCells.SetSparse(this, _source, std::move(normalized));
	if (!RefreshSelectedCellContainers()) return false;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		{
			if (live->_selectedCellsApplyDepth != 0)
				--live->_selectedCellsApplyDepth;
			if (live->_selectedCellsApplyDepth == 0)
				live->_selectedCellsApplyOld = {};
		}
		throw;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (live->_selectedCellsApplyDepth != 0)
		--live->_selectedCellsApplyDepth;
	if (live->_selectedCellsApplyDepth != 0) return true;
	auto previous = std::move(live->_selectedCellsApplyOld);
	live->_selectedCellsApplyOld = {};
	const bool ignoreLocators = live->_reconcilingSelectedCellLocators;
	const bool changed = !SameCellCollection(
		previous, live->_selectedCells, ignoreLocators);
	if (!changed) return true;
	if (++live->_cellSelectionRevision == 0)
		live->_cellSelectionRevision = 1;
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(
			live->GetGeneratedItem(index)))
			row->_appliedCellSelectionRevision = live->_cellSelectionRevision;
	}
	if (live->_settingItemsSource || live->IsItemsSourceUpdateInProgress())
	{
		if (!live->_selectedCellsChangeDeferred)
		{
			live->_selectedCellsChangeDeferred = true;
			live->_deferredSelectedCellsOld = previous;
			live->_deferredSelectedCellsIgnoreLocators = ignoreLocators;
		}
		else live->_deferredSelectedCellsIgnoreLocators =
			live->_deferredSelectedCellsIgnoreLocators && ignoreLocators;
		return true;
	}
	return live->RaiseSelectedCellsChanged(previous, ignoreLocators);
}

bool DataGrid::ApplySelectedCellCollection(
	DataGridSelectedCellCollection cells)
{
	const bool outermost = _selectedCellsApplyDepth == 0;
	if (outermost) _selectedCellsApplyOld = _selectedCells;
	++_selectedCellsApplyDepth;
	const ControlWeakReference ownerLifetime(this);
	try
	{
		cells._ownerLifetime = ownerLifetime;
		cells._ownerBound = true;
		if (!cells._ownerSourceIdentity)
			cells._ownerSourceIdentity = _source;
		if (!cells._source) cells._source = GetItemsView();
		_selectedCells = std::move(cells);
		if (!RefreshSelectedCellContainers()) return false;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		{
			if (live->_selectedCellsApplyDepth != 0)
				--live->_selectedCellsApplyDepth;
			if (live->_selectedCellsApplyDepth == 0)
				live->_selectedCellsApplyOld = {};
		}
		throw;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (live->_selectedCellsApplyDepth != 0)
		--live->_selectedCellsApplyDepth;
	if (live->_selectedCellsApplyDepth != 0) return true;
	auto previous = std::move(live->_selectedCellsApplyOld);
	live->_selectedCellsApplyOld = {};
	const bool ignoreLocators = live->_reconcilingSelectedCellLocators;
	if (SameCellCollection(previous, live->_selectedCells, ignoreLocators))
		return true;
	if (++live->_cellSelectionRevision == 0)
		live->_cellSelectionRevision = 1;
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(
			live->GetGeneratedItem(index)))
			row->_appliedCellSelectionRevision = live->_cellSelectionRevision;
	}
	if (live->_settingItemsSource || live->IsItemsSourceUpdateInProgress())
	{
		if (!live->_selectedCellsChangeDeferred)
		{
			live->_selectedCellsChangeDeferred = true;
			live->_deferredSelectedCellsOld = previous;
			live->_deferredSelectedCellsIgnoreLocators = ignoreLocators;
		}
		else live->_deferredSelectedCellsIgnoreLocators =
			live->_deferredSelectedCellsIgnoreLocators && ignoreLocators;
		return true;
	}
	return live->RaiseSelectedCellsChanged(previous, ignoreLocators);
}

bool DataGrid::SelectCell(size_t rowIndex, size_t columnIndex)
{
	if (_selectionUnit == DataGridSelectionUnit::FullRow) return false;
	DataGridCellInfo info;
	if (!TryCreateCellInfo(rowIndex, columnIndex, info)) return false;
	const bool alreadySelected = ContainsCellIdentity(_selectedCells, info);
	_selectionAnchor = info;
	ResetSelectionRange();
	const bool changed = !alreadySelected
		|| GetSelectionMode() == SelectionMode::Single;
	if (GetSelectionMode() == SelectionMode::Single)
		ApplySelectedCells({ std::move(info) });
	else if (_selectedCells.IsRegionBacked())
	{
		auto selected = _selectedCells;
		if (!selected.Include(info) && !alreadySelected)
			selected._cells.push_back(std::move(info));
		ApplySelectedCellCollection(std::move(selected));
	}
	else
	{
		auto selected = _selectedCells._cells;
		if (!alreadySelected) selected.push_back(std::move(info));
		ApplySelectedCells(std::move(selected));
	}
	return changed;
}

bool DataGrid::UnselectCell(size_t rowIndex, size_t columnIndex)
{
	if (_selectionUnit == DataGridSelectionUnit::FullRow) return false;
	DataGridCellInfo info;
	if (!TryCreateCellInfo(rowIndex, columnIndex, info)
		|| !ContainsCellIdentity(_selectedCells, info)) return false;
	auto selected = _selectedCells;
	if (selected.IsRegionBacked())
	{
		selected._cells.erase(std::remove_if(
			selected._cells.begin(), selected._cells.end(),
			[&info](const DataGridCellInfo& value)
			{ return SameCellIdentity(value, info); }), selected._cells.end());
		(void)selected.Exclude(info);
	}
	else selected._cells.erase(std::remove_if(
		selected._cells.begin(), selected._cells.end(),
		[&info](const DataGridCellInfo& value)
		{ return SameCellIdentity(value, info); }), selected._cells.end());
	if (_selectionUnit == DataGridSelectionUnit::CellOrRowHeader)
	{
		ControlWeakReference ownerLifetime(this);
		const bool previous = _suppressRowSelectionCellSync;
		_suppressRowSelectionCellSync = true;
		try
		{
			if (IsIndexSelected(rowIndex))
				RequestItemSelection(rowIndex, false);
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->_suppressRowSelectionCellSync = previous;
		}
		catch (...)
		{
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->_suppressRowSelectionCellSync = previous;
			throw;
		}
	}
	ResetSelectionRange();
	if (selected.IsRegionBacked())
		ApplySelectedCellCollection(std::move(selected));
	else ApplySelectedCells(std::move(selected._cells));
	return true;
}

void DataGrid::SelectAllCells()
{
	if (_selectionUnit == DataGridSelectionUnit::FullRow)
	{
		ListBox::SelectAll();
		return;
	}
	// A stable checkpoint gives the region a durable occurrence domain. Sorting
	// only permutes the view, so the one full-grid region keeps selecting the
	// exact same duplicate occurrences without N x columns cells.
	BindingListReference regionSnapshot;
	if (TryGetStableSelectedCellRegionSnapshot(regionSnapshot))
	{
		std::vector<DataGridColumn*> columns;
		columns.reserve(_columns.size());
		for (const auto& column : _columns)
			if (column) columns.push_back(column.get());
		DataGridSelectedCellCollection selected(this);
		selected.SetFullRegion(
			this, std::move(regionSnapshot),
			ItemCount(), std::move(columns));
		ResetSelectionRange();
		(void)ApplySelectedCellCollection(std::move(selected));
		return;
	}
	std::vector<DataGridCellInfo> selected;
	selected.reserve(ItemCount() * _columns.size());
	for (size_t row = 0; row < ItemCount(); ++row)
		for (size_t column = 0; column < _columns.size(); ++column)
		{
			DataGridCellInfo info;
			if (TryCreateCellInfo(row, column, info))
				selected.push_back(std::move(info));
		}
	ResetSelectionRange();
	ApplySelectedCells(std::move(selected));
}

void DataGrid::UnselectAllCells()
{
	if (_selectionUnit != DataGridSelectionUnit::Cell)
		if (!DeselectAllRowsWithoutCellSync()) return;
	_selectionAnchor.reset();
	ResetSelectionRange();
	ApplySelectedCells({});
}

void DataGrid::ResetSelectionRange() noexcept
{
	_selectionRangeActive = false;
	_selectionRangeBase.clear();
}

bool DataGrid::DeselectAllRowsWithoutCellSync()
{
	ControlWeakReference ownerLifetime(this);
	const bool previous = _suppressRowSelectionCellSync;
	_suppressRowSelectionCellSync = true;
	try
	{
		ListBox::UnselectAll();
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		live->_suppressRowSelectionCellSync = previous;
		live->_selectedRowSnapshot.clear();
		live->_selectedRowIndexSnapshot.Clear();
		return true;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_suppressRowSelectionCellSync = previous;
		throw;
	}
}

bool DataGrid::ApplySelectionForCellInput(
	size_t rowIndex, size_t columnIndex, ModifierKeys modifiers,
	bool allowsRange, bool allowsToggle)
{
	DataGridCellInfo target;
	if (!TryCreateCellInfo(rowIndex, columnIndex, target)) return false;
	const bool control = HasModifier(modifiers, ModifierKeys::Control);
	const bool shift = HasModifier(modifiers, ModifierKeys::Shift);
	const auto mode = GetSelectionMode();

	if (_selectionUnit == DataGridSelectionUnit::FullRow)
	{
		ControlWeakReference ownerLifetime(this);
		_selectionAnchor = target;
		ResetSelectionRange();
		if (mode == SelectionMode::Single && control && allowsToggle
			&& IsIndexSelected(rowIndex))
			ListBox::UnselectAll();
		else
			NotifyItemClicked(rowIndex, MouseButton::Left, modifiers);
		if (!ownerLifetime.Get()) return false;
		return IsCellSelected(rowIndex, columnIndex);
	}

	std::vector<DataGridCellInfo> selected;
	const bool extended = mode == SelectionMode::Extended;
	const bool multiple = mode == SelectionMode::Multiple;
	const bool canExtend = extended && allowsRange && shift
		&& _selectionAnchor && _selectionAnchor->IsValid();
	if (canExtend && control && _selectedCells.IsRegionBacked())
	{
		// The safe dense representation currently originates from SelectAll.
		// Ctrl+Shift unions a rectangle into that already-full region and is
		// therefore an O(1) no-op while retaining the range transaction state.
		_selectionRangeActive = true;
		return true;
	}
	if (canExtend)
	{
		if (_selectionRangeActive && !control)
			_selectionRangeBase.clear();
		if (!_selectionRangeActive)
		{
			_selectionRangeBase = control
				? _selectedCells._cells : std::vector<DataGridCellInfo>{};
			_selectionRangeActive = true;
		}
		selected = _selectionRangeBase;
		const size_t firstRow = (std::min)(
			_selectionAnchor->RowIndex, rowIndex);
		const size_t lastRow = (std::max)(
			_selectionAnchor->RowIndex, rowIndex);
		const size_t firstColumn = (std::min)(
			_selectionAnchor->ColumnIndex, columnIndex);
		const size_t lastColumn = (std::max)(
			_selectionAnchor->ColumnIndex, columnIndex);
		for (size_t row = firstRow; row <= lastRow; ++row)
			for (size_t column = firstColumn; column <= lastColumn; ++column)
			{
				DataGridCellInfo info;
				if (TryCreateCellInfo(row, column, info)
					&& !ContainsCellIdentity(selected, info))
					selected.push_back(std::move(info));
			}
	}
	else
	{
		ResetSelectionRange();
		const bool toggle = allowsToggle
			&& ((extended && control) || multiple);
		if (toggle)
		{
			if (_selectedCells.IsRegionBacked())
			{
				auto next = _selectedCells;
				const bool wasSelected = next.Contains(target);
				if (wasSelected)
				{
					next._cells.erase(std::remove_if(
						next._cells.begin(), next._cells.end(),
						[&target](const DataGridCellInfo& value)
						{ return SameCellIdentity(value, target); }),
						next._cells.end());
					(void)next.Exclude(target);
				}
				else if (!next.Include(target))
					next._cells.push_back(target);
				_selectionAnchor = target;
				if (!DeselectAllRowsWithoutCellSync()) return false;
				const bool targetSelected = next.Contains(target);
				ApplySelectedCellCollection(std::move(next));
				return targetSelected;
			}
			selected = _selectedCells._cells;
			const auto found = std::find_if(selected.begin(), selected.end(),
				[&target](const DataGridCellInfo& value)
				{ return SameCellIdentity(value, target); });
			if (found == selected.end()) selected.push_back(target);
			else selected.erase(found);
		}
		else if (mode == SelectionMode::Single && control && allowsToggle
			&& ContainsCellIdentity(_selectedCells, target))
			selected.clear();
		else selected = { target };
		_selectionAnchor = target;
	}

	if (_selectionUnit == DataGridSelectionUnit::Cell
		|| !control || !extended)
	{
		if (!DeselectAllRowsWithoutCellSync()) return false;
	}
	else if (!ContainsCellIdentity(selected, target)
		&& IsIndexSelected(rowIndex))
	{
		ControlWeakReference ownerLifetime(this);
		const bool previous = _suppressRowSelectionCellSync;
		_suppressRowSelectionCellSync = true;
		try
		{
			RequestItemSelection(rowIndex, false);
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->_suppressRowSelectionCellSync = previous;
		}
		catch (...)
		{
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->_suppressRowSelectionCellSync = previous;
			throw;
		}
	}
	const bool targetSelected = ContainsCellIdentity(selected, target);
	ApplySelectedCells(std::move(selected));
	return targetSelected;
}

void DataGrid::HandleRowSelectionChanged(SelectionChangedEventArgs&)
{
	const ControlWeakReference ownerLifetime(this);
	const auto currentRowShape = GetSelectedIndices();
	std::vector<uint32_t> previousIds;
	std::vector<uint32_t> currentIds;
	const bool coalesceFullRowAutomation =
		_selectionUnit != DataGridSelectionUnit::Cell
		&& (currentRowShape.IsRangeBacked()
			|| _selectedRowIndexSnapshot.IsRangeBacked());
	if (_selectionUnit != DataGridSelectionUnit::Cell
		&& !coalesceFullRowAutomation
		&& EnsureAccessibilityVirtualIdentities())
	{
		for (const auto& [item, index] : _selectedRowSnapshot)
			if (auto* row = EnsureAccessibilityRowIdentity(index, false);
				row && row->Item.Shared() == item.Shared())
				previousIds.push_back(row->RowId);
		for (const int index : currentRowShape)
			if (index >= 0
				&& static_cast<size_t>(index) < ItemCount())
				if (auto* row = EnsureAccessibilityRowIdentity(
					static_cast<size_t>(index), false))
					currentIds.push_back(row->RowId);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::vector<uint32_t> removedIds;
	std::vector<uint32_t> addedIds;
	removedIds.reserve(previousIds.size());
	addedIds.reserve(currentIds.size());
	for (const uint32_t id : previousIds)
		if (std::find(currentIds.begin(), currentIds.end(), id)
			== currentIds.end())
			removedIds.push_back(id);
	for (const uint32_t id : currentIds)
		if (std::find(previousIds.begin(), previousIds.end(), id)
			== previousIds.end())
			addedIds.push_back(id);
	const auto publishChanges = [&]()
	{
		// Match DataGridAutomationPeer: a transaction that leaves exactly one
		// selected row and adds that row raises only ElementSelected.
		auto* currentOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!currentOwner) return false;
		if (coalesceFullRowAutomation)
		{
			currentOwner->NotifyAccessibilityStateChanged();
			return ownerLifetime.Get() != nullptr;
		}
		const auto finalSelectedIndices =
			currentOwner->GetSelectedIndices();
		if (finalSelectedIndices.size() == 1
			&& addedIds.size() == 1)
		{
			if (!currentOwner->EnsureAccessibilityVirtualIdentities())
				return ownerLifetime.Get() != nullptr;
			currentOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!currentOwner
				|| currentOwner->GetSelectedIndices() != finalSelectedIndices)
				return false;
			const int selectedIndex = finalSelectedIndices.front();
			if (selectedIndex < 0) return true;
			auto* selectedRow =
				currentOwner->EnsureAccessibilityRowIdentity(
					static_cast<size_t>(selectedIndex), false);
			currentOwner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!currentOwner || !selectedRow) return currentOwner != nullptr;
			currentOwner->NotifyAccessibilityVirtualChanged(
				selectedRow->RowId,
				AccessibilityChange::SelectionSelected);
			return ownerLifetime.Get() != nullptr;
		}
		for (const uint32_t id : addedIds)
		{
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->NotifyAccessibilityVirtualChanged(
				id, AccessibilityChange::SelectionAdded);
		}
		for (const uint32_t id : removedIds)
		{
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			live->NotifyAccessibilityVirtualChanged(
				id, AccessibilityChange::SelectionRemoved);
		}
		return true;
	};
	if (live->_suppressRowSelectionCellSync)
	{
		if (live->_selectionUnit == DataGridSelectionUnit::FullRow
			&& currentRowShape.IsRangeBacked())
		{
			live->_selectedRowSnapshot.clear();
			live->_selectedRowIndexSnapshot = currentRowShape;
			(void)publishChanges();
			return;
		}
		const auto items = live->GetItemsView();
		const auto selectedIndices = currentRowShape;
		const size_t selectionRevision = live->_rowSelectionRevision;
		std::vector<std::pair<BindingSourceReference, size_t>> snapshot;
		snapshot.reserve(selectedIndices.size());
		for (const int index : selectedIndices)
		{
			BindingSourceReference item;
			const bool read = items && index >= 0
				&& items.Get()->TryGetItem(
					static_cast<size_t>(index), item);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared())
				return;
			if (live->_rowSelectionRevision != selectionRevision) return;
			if (read && item)
				snapshot.emplace_back(
					std::move(item), static_cast<size_t>(index));
		}
		live->_selectedRowSnapshot = std::move(snapshot);
		live->_selectedRowIndexSnapshot = selectedIndices;
		(void)publishChanges();
		return;
	}
	if (!live->SynchronizeSelectedCellsFromRows()) return;
	(void)publishChanges();
}

bool DataGrid::SynchronizeRangeSelectedCellsFromRows(
	SelectedIndexCollection selectedIndices)
{
	const ControlWeakReference ownerLifetime(this);
	const auto items = GetItemsView();
	const size_t selectionRevision = _rowSelectionRevision;
	if (!items || !selectedIndices.IsRangeBacked()
		|| selectedIndices.RangeCount() != items.Get()->Count()) return false;
	const auto resolveCurrent = [&]() -> DataGrid*
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return owner
			&& owner->_selectionUnit == DataGridSelectionUnit::FullRow
			&& owner->_rowSelectionRevision == selectionRevision
			&& owner->GetItemsView().Shared() == items.Shared()
			? owner : nullptr;
	};
	auto* live = resolveCurrent();
	if (!live) return false;
	if (live->_columns.empty())
	{
		if (!live->ApplySelectedCells({})) return false;
		live = resolveCurrent();
		if (!live) return ownerLifetime.Get() != nullptr;
		live->_selectedRowSnapshot.clear();
		live->_selectedRowIndexSnapshot = std::move(selectedIndices);
		return true;
	}

	DataGridSelectedCellCollection selected = live->_selectedCells;
	const bool reusableRegion = selected.IsRegionBacked()
		&& selected._regions.size() == 1
		&& selected._regions.front().Top == 0
		&& selected._regions.front().IncludedRowOffsets.empty()
		&& selected._ownerSourceIdentity.Shared() == live->_source.Shared()
		&& selected._source
		&& dynamic_cast<const IBindingListStableSnapshot*>(
			selected._source.Get())
		&& dynamic_cast<const IBindingListOccurrenceIdentity*>(
			selected._source.Get())
		&& dynamic_cast<const IBindingListOccurrenceLookup*>(
			selected._source.Get());
	if (!reusableRegion)
	{
		BindingListReference regionSnapshot;
		if (!live->TryGetStableSelectedCellRegionSnapshot(regionSnapshot))
			return false;
		live = resolveCurrent();
		if (!live) return false;
		std::vector<DataGridColumn*> columns;
		columns.reserve(live->_columns.size());
		for (const auto& column : live->_columns)
			if (column) columns.push_back(column.get());
		selected.SetFullRegion(live, std::move(regionSnapshot),
			selectedIndices.RangeCount(), std::move(columns));
	}
	live = resolveCurrent();
	if (!live || selected._regions.size() != 1) return false;
	auto& region = selected._regions.front();
	const auto* liveOccurrences = dynamic_cast<
		const IBindingListOccurrenceIdentity*>(items.Get());
	const auto* liveLookup = dynamic_cast<
		const IBindingListOccurrenceLookup*>(items.Get());
	const auto* snapshotLookup = dynamic_cast<
		const IBindingListOccurrenceLookup*>(selected._source.Get());
	if (!liveOccurrences || !liveLookup || !snapshotLookup) return false;

	std::vector<DataGridColumn*> columns;
	columns.reserve(live->_columns.size());
	for (const auto& column : live->_columns)
		if (column) columns.push_back(column.get());
	region.Columns = columns;
	region.ColumnIndices.resize(columns.size());
	for (size_t index = 0; index < columns.size(); ++index)
		region.ColumnIndices[index] = index;
	region.UseSnapshotLocators = false;
	selected._excludedCells.clear();

	const auto tryGetLiveOccurrence = [&](size_t index,
		BindingSourceReference& item, size_t& occurrence)
	{
		item = {};
		occurrence = DataGridCellInfo::InvalidIndex;
		if (index >= items.Get()->Count()) return false;
		const bool occurrenceRead =
			liveOccurrences->TryGetItemOccurrenceIdentity(index, occurrence);
		const bool itemRead = items.Get()->TryGetItem(index, item);
		return resolveCurrent() && occurrenceRead && itemRead && item
			&& occurrence != DataGridCellInfo::InvalidIndex;
	};
	const auto tryResolveLiveOccurrence = [&](
		const DataGridSelectedCellCollection::CellRegion::ExcludedRow& row,
		size_t& index)
	{
		index = DataGridCellInfo::InvalidIndex;
		if (!liveLookup->TryGetItemIndexByOccurrenceIdentity(
				row.Occurrence, index)) return resolveCurrent() != nullptr ? 0 : -1;
		BindingSourceReference item;
		size_t occurrence = DataGridCellInfo::InvalidIndex;
		if (!tryGetLiveOccurrence(index, item, occurrence))
			return resolveCurrent() ? 0 : -1;
		return item.Shared() == row.Item.Shared()
			&& occurrence == row.Occurrence ? 1 : 0;
	};

	std::vector<DataGridSelectedCellCollection::CellRegion::ExcludedRow>
		excludedRows;
	excludedRows.reserve(region.ExcludedRows.size()
		+ selectedIndices.ExcludedIndices().size());
	std::unordered_set<size_t> excludedOccurrences;
	excludedOccurrences.reserve(region.ExcludedRows.size()
		+ selectedIndices.ExcludedIndices().size());
	for (const auto& excluded : region.ExcludedRows)
	{
		if (IsRowInIntervals(
			region.ExcludedSnapshotRowIntervals,
			excluded.SnapshotRow)) continue;
		size_t currentIndex = DataGridCellInfo::InvalidIndex;
		const int resolved = tryResolveLiveOccurrence(excluded, currentIndex);
		if (resolved < 0) return false;
		if (resolved == 0 || currentIndex
			> static_cast<size_t>((std::numeric_limits<int>::max)())
			|| !selectedIndices.Contains(static_cast<int>(currentIndex)))
			if (excludedOccurrences.insert(excluded.Occurrence).second)
				excludedRows.push_back(excluded);
	}
	for (const int excludedIndex : selectedIndices.ExcludedIndices())
	{
		if (excludedIndex < 0) continue;
		BindingSourceReference item;
		size_t occurrence = DataGridCellInfo::InvalidIndex;
		if (!tryGetLiveOccurrence(
				static_cast<size_t>(excludedIndex), item, occurrence))
		{
			if (!resolveCurrent()) return false;
			continue;
		}
		size_t snapshotRow = DataGridCellInfo::InvalidIndex;
		if (!snapshotLookup->TryGetItemIndexByOccurrenceIdentity(
				occurrence, snapshotRow))
		{
			if (!resolveCurrent()) return false;
			continue;
		}
		BindingSourceReference snapshotItem;
		const bool snapshotRead = snapshotRow < selected._source.Get()->Count()
			&& selected._source.Get()->TryGetItem(snapshotRow, snapshotItem);
		if (!resolveCurrent()) return false;
		if (!snapshotRead || !snapshotItem
			|| snapshotItem.Shared() != item.Shared()
			|| snapshotRow >= region.Height) continue;
		if (excludedOccurrences.insert(occurrence).second)
			excludedRows.push_back({
				std::move(snapshotItem), occurrence, snapshotRow });
	}
	std::sort(excludedRows.begin(), excludedRows.end(),
		[](const auto& left, const auto& right)
		{ return left.SnapshotRow < right.SnapshotRow; });
	region.ExcludedRows = std::move(excludedRows);
	// Retired snapshot intervals deliberately have no live occurrence to remap.
	// They stay anchored to the immutable region source across sorting and row
	// reconciliation; per-occurrence user holes above still follow the live view.
	selected.InvalidateExcludedOffsets();

	const auto regionSelects = [&](const DataGridCellInfo& cell)
	{
		if (!cell.Item || cell._itemOccurrence
			== DataGridCellInfo::InvalidIndex
			|| std::find(region.Columns.begin(), region.Columns.end(),
				cell.Column) == region.Columns.end()) return false;
		size_t snapshotRow = DataGridCellInfo::InvalidIndex;
		if (!snapshotLookup->TryGetItemIndexByOccurrenceIdentity(
				cell._itemOccurrence, snapshotRow)
			|| snapshotRow >= region.Height) return false;
		return !excludedOccurrences.contains(cell._itemOccurrence);
	};
	selected._cells.erase(std::remove_if(selected._cells.begin(),
		selected._cells.end(), [&](const DataGridCellInfo& cell)
		{
			DataGridSelectedCellCollection::CellRegion::ExcludedRow identity{
				cell.Item, cell._itemOccurrence,
				DataGridCellInfo::InvalidIndex };
			size_t currentIndex = DataGridCellInfo::InvalidIndex;
			const int resolved = tryResolveLiveOccurrence(identity, currentIndex);
			if (resolved < 0) return true;
			return resolved == 0 || currentIndex
				> static_cast<size_t>((std::numeric_limits<int>::max)())
				|| !selectedIndices.Contains(static_cast<int>(currentIndex))
				|| std::find(columns.begin(), columns.end(), cell.Column)
					== columns.end()
				|| regionSelects(cell);
		}), selected._cells.end());
	if (!resolveCurrent()) return false;

	std::vector<int> newlySelected;
	if (live->_selectedRowIndexSnapshot.IsRangeBacked()
		&& live->_selectedRowIndexSnapshot.RangeCount()
			== selectedIndices.RangeCount())
		std::set_difference(
			live->_selectedRowIndexSnapshot.ExcludedIndices().begin(),
			live->_selectedRowIndexSnapshot.ExcludedIndices().end(),
			selectedIndices.ExcludedIndices().begin(),
			selectedIndices.ExcludedIndices().end(),
			std::back_inserter(newlySelected));
	for (const int index : newlySelected)
	{
		if (index < 0 || !selectedIndices.Contains(index)) continue;
		for (size_t column = 0; column < columns.size(); ++column)
		{
			live = resolveCurrent();
			if (!live) return false;
			DataGridCellInfo info;
			if (!live->TryCreateCellInfo(
					static_cast<size_t>(index), column, info))
			{
				if (!resolveCurrent()) return false;
				continue;
			}
			if (!regionSelects(info)
				&& !ContainsCellIdentity(selected._cells, info))
				selected._cells.push_back(std::move(info));
		}
	}

	// Explicit rows are those selected after they were inserted outside the
	// immutable region checkpoint. Keep their projection complete when columns
	// are added, without touching any row represented by the range itself.
	std::unordered_map<size_t, BindingSourceReference> sparseRows;
	sparseRows.reserve(selected._cells.size());
	for (const auto& cell : selected._cells)
		sparseRows.try_emplace(cell._itemOccurrence, cell.Item);
	for (const auto& [occurrence, item] : sparseRows)
	{
		DataGridSelectedCellCollection::CellRegion::ExcludedRow identity{
			item, occurrence, DataGridCellInfo::InvalidIndex };
		size_t currentIndex = DataGridCellInfo::InvalidIndex;
		const int resolved = tryResolveLiveOccurrence(identity, currentIndex);
		if (resolved < 0) return false;
		if (resolved == 0) continue;
		for (size_t column = 0; column < columns.size(); ++column)
		{
			live = resolveCurrent();
			if (!live) return false;
			DataGridCellInfo info;
			if (!live->TryCreateCellInfo(currentIndex, column, info))
			{
				if (!resolveCurrent()) return false;
				continue;
			}
			if (!regionSelects(info)
				&& !ContainsCellIdentity(selected._cells, info))
				selected._cells.push_back(std::move(info));
		}
	}

	live = resolveCurrent();
	if (!live || !live->ApplySelectedCellCollection(std::move(selected)))
		return false;
	live = resolveCurrent();
	if (!live) return ownerLifetime.Get() != nullptr;
	live->_selectedRowSnapshot.clear();
	live->_selectedRowIndexSnapshot = std::move(selectedIndices);
	return true;
}

bool DataGrid::SynchronizeSelectedCellsFromRows()
{
	const ControlWeakReference ownerLifetime(this);
	auto* live = this;
	// Ordinary cell input has no row-to-cell synchronization contract. A
	// generated-row rebuild (for example after adding a column) must retain an
	// existing lazy SelectAll region. Programmatic Selector row selection in Cell
	// mode remains row-shaped for WPF compatibility and therefore still projects
	// that selected row into cells.
	if (live->_selectionUnit == DataGridSelectionUnit::Cell)
	{
		if (!live->GetSelectedIndices().empty()
			&& !live->_selectedCells.IsRegionBacked())
		{
			// Continue into the regular row projection below.
		}
		else
		{
			live->_selectedRowSnapshot.clear();
			live->_selectedRowIndexSnapshot.Clear();
			return true;
		}
	}
	if (live->_selectionUnit == DataGridSelectionUnit::FullRow
		&& live->GetSelectedIndices().IsRangeBacked())
		return live->SynchronizeRangeSelectedCellsFromRows(
			live->GetSelectedIndices());
	const auto items = live->GetItemsView();
	const auto selectedIndices = live->GetSelectedIndices();
	const size_t selectionRevision = live->_rowSelectionRevision;
	std::vector<std::pair<BindingSourceReference, size_t>> currentRows;
	currentRows.reserve(selectedIndices.size());
	for (const int index : selectedIndices)
	{
		BindingSourceReference item;
		const bool read = items && index >= 0
			&& items.Get()->TryGetItem(static_cast<size_t>(index), item);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != items.Shared()
			|| live->_rowSelectionRevision != selectionRevision) return false;
		if (read && item)
			currentRows.emplace_back(std::move(item), static_cast<size_t>(index));
	}
	BindingListReference regionSnapshot;
	if (live->_selectionUnit == DataGridSelectionUnit::FullRow
		&& selectedIndices.size() == live->ItemCount()
		&& live->TryGetStableSelectedCellRegionSnapshot(regionSnapshot))
	{
		std::vector<DataGridColumn*> columns;
		columns.reserve(live->_columns.size());
		for (const auto& column : live->_columns)
			if (column) columns.push_back(column.get());
		DataGridSelectedCellCollection selected(live);
		selected.SetFullRegion(live, std::move(regionSnapshot),
			live->ItemCount(), std::move(columns));
		live->_selectedRowSnapshot = std::move(currentRows);
		live->_selectedRowIndexSnapshot = selectedIndices;
		return live->ApplySelectedCellCollection(std::move(selected));
	}

	std::vector<DataGridCellInfo> selected =
		(live->_selectionUnit == DataGridSelectionUnit::FullRow
			|| live->_replaceCellSelectionFromRows)
		? std::vector<DataGridCellInfo>{} : live->_selectedCells._cells;
	if (live->_selectionUnit != DataGridSelectionUnit::FullRow)
	{
		for (const auto& oldRow : live->_selectedRowSnapshot)
		{
			const bool retained = std::any_of(
				currentRows.begin(), currentRows.end(),
				[&oldRow](const auto& value)
				{
					return value.first.Shared() == oldRow.first.Shared()
						&& value.second == oldRow.second;
				});
			if (retained) continue;
			selected.erase(std::remove_if(selected.begin(), selected.end(),
				[&oldRow](const DataGridCellInfo& value)
				{
					return value.Item.Shared() == oldRow.first.Shared()
						&& value.RowIndex == oldRow.second;
				}),
				selected.end());
		}
	}
	for (const auto& row : currentRows)
		for (size_t column = 0; ; ++column)
		{
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared()
				|| live->_rowSelectionRevision != selectionRevision) return false;
			if (column >= live->_columns.size()) break;
			DataGridCellInfo info;
			const bool created = live->TryCreateCellInfo(
				row.second, column, info);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared()
				|| live->_rowSelectionRevision != selectionRevision || !created)
				return false;
			if (!ContainsCellIdentity(selected, info))
				selected.push_back(std::move(info));
		}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->_selectedRowSnapshot = std::move(currentRows);
	live->_selectedRowIndexSnapshot = selectedIndices;
	return live->ApplySelectedCells(std::move(selected));
}

bool DataGrid::ReconcileSelectedCells()
{
	ResetSelectionRange();
	if (_selectionAnchor)
	{
		const auto previousAnchor = *_selectionAnchor;
		const auto column = std::find_if(_columns.begin(), _columns.end(),
			[&previousAnchor](const auto& value)
			{ return value.get() == previousAnchor.Column; });
		bool found = false;
		const auto items = GetItemsView();
		if (column != _columns.end() && items)
		{
			const size_t columnIndex = static_cast<size_t>(
				std::distance(_columns.begin(), column));
			size_t row = DataGridCellInfo::InvalidIndex;
			if (previousAnchor._itemOccurrence != DataGridCellInfo::InvalidIndex
				&& TryResolveItemOccurrence(
					previousAnchor.Item,
					previousAnchor._itemOccurrence,
					row))
			{
				DataGridCellInfo info;
				if (TryCreateCellInfo(row, columnIndex, info)
					&& SameCellIdentity(info, previousAnchor))
				{
					_selectionAnchor = std::move(info);
					found = true;
				}
			}
			if (!found && previousAnchor._itemOccurrence
				== DataGridCellInfo::InvalidIndex)
				for (row = 0; row < items.Get()->Count(); ++row)
				{
					DataGridCellInfo info;
					if (TryCreateCellInfo(row, columnIndex, info)
						&& SameCellIdentity(info, previousAnchor))
					{
						_selectionAnchor = std::move(info);
						found = true;
						break;
					}
				}
		}
		if (!found) _selectionAnchor.reset();
	}
	ControlWeakReference ownerLifetime(this);
	const bool previousReconcile = _reconcilingSelectedCellLocators;
	_reconcilingSelectedCellLocators = true;
	try
	{
		bool result = false;
		if (_selectedCells.IsRegionBacked())
		{
			if (_selectedCells._ownerSourceIdentity.Shared()
				!= _source.Shared())
				result = ApplySelectedCells({});
			else
			{
				auto selected = _selectedCells;
				selected.PruneColumns(_columns);
				selected._cells.erase(std::remove_if(
					selected._cells.begin(), selected._cells.end(),
					[this](DataGridCellInfo& cell)
					{
						const auto column = std::find_if(
							_columns.begin(), _columns.end(),
							[&cell](const auto& candidate)
							{ return candidate.get() == cell.Column; });
						size_t rowIndex = DataGridCellInfo::InvalidIndex;
						if (column == _columns.end()
							|| !TryResolveItemOccurrence(cell.Item,
								cell._itemOccurrence, rowIndex)) return true;
						cell.RowIndex = rowIndex;
						cell.ColumnIndex = static_cast<size_t>(
							std::distance(_columns.begin(), column));
						cell.Column = column->get();
						return false;
					}), selected._cells.end());
				for (auto& excluded : selected._excludedCells)
				{
					size_t rowIndex = DataGridCellInfo::InvalidIndex;
					if (TryResolveItemOccurrence(excluded.Item,
						excluded._itemOccurrence, rowIndex))
						excluded.RowIndex = rowIndex;
				}
				selected.InvalidateExcludedOffsets();
				result = ApplySelectedCellCollection(std::move(selected));
			}
		}
		else result = ApplySelectedCells(_selectedCells._cells);
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_reconcilingSelectedCellLocators = previousReconcile;
		return result;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			live->_reconcilingSelectedCellLocators = previousReconcile;
		throw;
	}
}

void DataGrid::OnSelectionUnitChanged(
	DataGridSelectionUnit oldValue, DataGridSelectionUnit newValue)
{
	if (oldValue == newValue) return;
	_selectionAnchor.reset();
	ResetSelectionRange();
	if (!DeselectAllRowsWithoutCellSync()) return;
	_selectedRowSnapshot.clear();
	_selectedRowIndexSnapshot.Clear();
	ApplySelectedCells({});
}

void DataGrid::OnCellIsSelectedChanged(
	DataGridCell& cell, bool selected)
{
	if (_updatingCellSelectionVisuals || !cell.GetRowOwner()) return;
	const size_t rowIndex = cell.GetRowOwner()->ItemIndex();
	const size_t columnIndex = cell._columnIndex;
	if (_selectionUnit == DataGridSelectionUnit::FullRow)
		throw std::logic_error(
			"DataGridCell.IsSelected cannot be changed in FullRow mode");
	DataGridCellInfo info;
	if (!TryCreateCellInfo(rowIndex, columnIndex, info)) return;
	auto cells = _selectedCells;
	if (selected && GetSelectionMode() == SelectionMode::Single)
		cells.SetSparse(this, _source, { info });
	else if (cells.IsRegionBacked())
	{
		if (selected)
		{
			if (!cells.Include(info) && !cells.Contains(info))
				cells._cells.push_back(info);
		}
		else
		{
			cells._cells.erase(std::remove_if(
				cells._cells.begin(), cells._cells.end(),
				[&info](const DataGridCellInfo& value)
				{ return SameCellIdentity(value, info); }), cells._cells.end());
			(void)cells.Exclude(info);
		}
	}
	else if (selected)
	{
		if (!ContainsCellIdentity(cells._cells, info))
			cells._cells.push_back(info);
	}
	else cells._cells.erase(std::remove_if(
		cells._cells.begin(), cells._cells.end(),
		[&info](const DataGridCellInfo& value)
		{ return SameCellIdentity(value, info); }), cells._cells.end());
	if (!selected
		&& _selectionUnit == DataGridSelectionUnit::CellOrRowHeader
		&& IsIndexSelected(rowIndex))
	{
		ControlWeakReference ownerLifetime(this);
		const bool previous = _suppressRowSelectionCellSync;
		_suppressRowSelectionCellSync = true;
		try
		{
			RequestItemSelection(rowIndex, false);
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->_suppressRowSelectionCellSync = previous;
			else return;
		}
		catch (...)
		{
			if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				live->_suppressRowSelectionCellSync = previous;
			throw;
		}
	}
	if (cells.IsRegionBacked())
		(void)ApplySelectedCellCollection(std::move(cells));
	else (void)ApplySelectedCells(std::move(cells._cells));
}

bool DataGrid::TryNormalizeCurrentCellInfo(
	const DataGridCellInfo& value,
	DataGridCellInfo& result) const
{
	result = {};
	ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	DataGridColumn* column = nullptr;
	size_t columnIndex = DataGridCellInfo::InvalidIndex;
	if (value.Column)
	{
		const auto found = std::find_if(
			_columns.begin(), _columns.end(), [&value](const auto& candidate)
			{ return candidate.get() == value.Column; });
		if (found == _columns.end()) return false;
		columnIndex = static_cast<size_t>(
			std::distance(_columns.begin(), found));
		column = found->get();
	}

	BindingSourceReference item;
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	if (value.Item)
	{
		const auto items = GetItemsView();
		if (!items) return false;
		if (value._itemOccurrence != DataGridCellInfo::InvalidIndex)
		{
			if (!TryResolveItemOccurrence(
				value.Item, value._itemOccurrence, rowIndex)) return false;
		}
		else
		{
			const size_t count = items.Get()->Count();
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live || live->GetItemsView().Shared() != items.Shared())
				return false;
			if (value.RowIndex < count)
			{
				BindingSourceReference atLocator;
				const bool read = items.Get()->TryGetItem(
					value.RowIndex, atLocator);
				live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (!live || live->GetItemsView().Shared() != items.Shared())
					return false;
				if (read && atLocator
					&& atLocator.Shared() == value.Item.Shared())
					rowIndex = value.RowIndex;
			}
			if (rowIndex == DataGridCellInfo::InvalidIndex)
			{
				for (size_t index = 0; index < count; ++index)
				{
					BindingSourceReference candidate;
					const bool read = items.Get()->TryGetItem(index, candidate);
					live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
					if (!live
						|| live->GetItemsView().Shared() != items.Shared())
						return false;
					if (!read || !candidate
						|| candidate.Shared() != value.Item.Shared()) continue;
					rowIndex = index;
					break;
				}
			}
			if (rowIndex == DataGridCellInfo::InvalidIndex) return false;
		}
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		if (!live->IsNewItemPlaceholder(value.Item)
			&& !live->TryGetItemOccurrenceAt(rowIndex, occurrence)) return false;
		const auto liveItems = live->GetItemsView();
		if (!liveItems
			|| !liveItems.Get()->TryGetItem(rowIndex, item) || !item)
			return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->GetItemsView().Shared() != liveItems.Shared()
			|| item.Shared() != value.Item.Shared()) return false;
	}

	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (item && column)
		return live->TryCreateCellInfo(rowIndex, columnIndex, result)
			&& result.Item.Shared() == item.Shared()
			&& result._itemOccurrence == occurrence;
	result.Item = std::move(item);
	result.Column = column;
	result.RowIndex = rowIndex;
	result.ColumnIndex = columnIndex;
	result._itemOccurrence = occurrence;
	return true;
}

void DataGrid::SetCurrentItem(const BindingValue& value)
{
	DataGridCellInfo requested = _currentCell;
	BindingSourceReference item;
	if (!value.Empty() && (!value.TryGet(item) || !item)) item = {};
	if (requested.Item.Shared() != item.Shared())
	{
		requested.Item = std::move(item);
		requested.RowIndex = DataGridCellInfo::InvalidIndex;
		requested._itemOccurrence = DataGridCellInfo::InvalidIndex;
	}
	(void)SetCurrentCell(requested);
}

void DataGrid::SetCurrentColumn(DataGridColumn* value)
{
	DataGridCellInfo requested = _currentCell;
	if (requested.Column != value)
	{
		requested.Column = value;
		requested.ColumnIndex = DataGridCellInfo::InvalidIndex;
	}
	(void)SetCurrentCell(requested);
}

bool DataGrid::SetCurrentCell(const DataGridCellInfo& value)
{
	ControlWeakReference ownerLifetime(this);
	DataGridCellInfo next;
	if (!TryNormalizeCurrentCellInfo(value, next)) return false;
	const auto sameProjection = [](const DataGridCellInfo& left,
		const DataGridCellInfo& right)
	{
		return left == right
			&& left.RowIndex == right.RowIndex
			&& left.ColumnIndex == right.ColumnIndex;
	};
	if (sameProjection(_currentCell, next)) return true;
	const bool leavesEditingRow = _editingRowItem
		&& (!next.Item || !IsRowEditIdentity(next.Item, next.RowIndex));
	const bool hasEditingCell = [&]
	{
		auto* current = ResolveCurrentCellContainer();
		return current && current->GetIsEditing();
	}();
	if (leavesEditingRow || hasEditingCell)
	{
		const bool ended = leavesEditingRow
			? CommitEdit(DataGridEditingUnit::Row, true)
			: CommitEdit(DataGridEditingUnit::Cell, true);
		if (!ended) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !live->TryNormalizeCurrentCellInfo(value, next))
			return false;
		if (sameProjection(live->_currentCell, next)) return true;
		const auto previous = live->_currentCell;
		live->_currentCell = std::move(next);
		return live->RaiseCurrentCellChanged(previous);
	}
	const auto previous = _currentCell;
	_currentCell = std::move(next);
	return RaiseCurrentCellChanged(previous);
}

bool DataGrid::SetCurrentCell(size_t rowIndex, size_t columnIndex)
{
	DataGridCellInfo next;
	return TryCreateCellInfo(rowIndex, columnIndex, next)
		&& SetCurrentCell(next);
}

bool DataGrid::ScrollIntoView(const BindingValue& item)
{
	if (item.Empty()) return false;
	return ScrollIntoView(item, nullptr);
}

bool DataGrid::ScrollIntoView(
	const BindingValue& item,
	DataGridColumn* column)
{
	if (item.Empty() && !column) return false;
	ControlWeakReference ownerLifetime(this);
	bool vertical = item.Empty();
	if (!item.Empty())
	{
		BindingSourceReference requested;
		if (!item.TryGet(requested) || !requested) return false;
		DataGridCellInfo locator;
		locator.Item = std::move(requested);
		DataGridCellInfo normalized;
		if (!TryNormalizeCurrentCellInfo(locator, normalized)
			|| !normalized.Item) return false;
		vertical = BringItemIntoView(normalized.RowIndex);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!column) return vertical;
	const auto found = std::find_if(
		live->_columns.begin(), live->_columns.end(),
		[column](const auto& candidate) { return candidate.get() == column; });
	if (found == live->_columns.end()) return false;
	const bool horizontal = live->BringColumnIntoView(static_cast<size_t>(
		std::distance(live->_columns.begin(), found)));
	return item.Empty() ? horizontal : vertical && horizontal;
}

bool DataGrid::RaiseCurrentCellChanged(DataGridCellInfo previous)
{
	const bool projectionChanged = !(previous == _currentCell)
		|| previous.RowIndex != _currentCell.RowIndex
		|| previous.ColumnIndex != _currentCell.ColumnIndex;
	if (!projectionChanged) return true;
	if (_settingItemsSource || IsItemsSourceUpdateInProgress())
	{
		if (!_currentCellChangeDeferred)
		{
			_currentCellChangeDeferred = true;
			_deferredCurrentCellOld = std::move(previous);
		}
		return true;
	}
	ControlWeakReference ownerLifetime(this);
	if (!NotifyCurrentCellProjectionsChanged(previous)) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	live->InvalidateCommandState();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (previous == live->_currentCell) return true;
	DataGridCurrentCellChangedEventArgs args;
	args.OldCell = std::move(previous);
	args.NewCell = live->_currentCell;
	bool firstHandler = true;
	cui::framework::EventAccess::RaiseWhile(
		live->CurrentCellChanged,
		[&]()
		{
			auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
			if (std::exchange(firstHandler, false)) return true;
			const auto columnIsLive = [live](const DataGridCellInfo& cell)
			{
				return !cell.Column || std::any_of(
					live->_columns.begin(), live->_columns.end(),
					[&cell](const auto& column)
					{ return column.get() == cell.Column; });
			};
			return columnIsLive(args.OldCell)
				&& columnIsLive(args.NewCell);
		},
		this, args);
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::NotifyCurrentCellProjectionsChanged(
	const DataGridCellInfo& previous)
{
	const bool itemChanged = previous.Item.Shared()
		!= _currentCell.Item.Shared();
	const bool columnChanged = previous.Column != _currentCell.Column;
	const bool cellChanged = !(previous == _currentCell)
		|| previous.RowIndex != _currentCell.RowIndex
		|| previous.ColumnIndex != _currentCell.ColumnIndex;
	if (!itemChanged && !columnChanged && !cellChanged) return true;

	ControlWeakReference ownerLifetime(this);
	// These properties are computed projections over _currentCell. Their
	// TwoWay subscribers read the live accessor when notified, so retain the
	// Binding expression slot exactly as WPF's SetCurrentValue contract does;
	// writing the projection back into that slot would invoke the setter a
	// second time and can mask a locator-only remap behind stale expression
	// state.
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (itemChanged)
		cui::framework::EventAccess::RaiseWhile(
			live->_currentItemChanged,
			[&ownerLifetime] { return ownerLifetime.Get() != nullptr; }, live);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (columnChanged)
		cui::framework::EventAccess::RaiseWhile(
			live->_currentColumnChanged,
			[&ownerLifetime] { return ownerLifetime.Get() != nullptr; }, live);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (cellChanged)
		cui::framework::EventAccess::RaiseWhile(
			live->_currentCellProjectionChanged,
			[&ownerLifetime] { return ownerLifetime.Get() != nullptr; }, live);
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::ReconcileCurrentCellColumn()
{
	if (!_currentCell.Column) return true;
	const auto found = std::find_if(
		_columns.begin(), _columns.end(),
		[this](const auto& column)
		{ return column.get() == _currentCell.Column; });
	const auto previous = _currentCell;
	if (found != _columns.end())
	{
		_currentCell.ColumnIndex = static_cast<size_t>(
			std::distance(_columns.begin(), found));
		_currentCell.Column = found->get();
	}
	else if (_columns.empty())
	{
		_currentCell.Column = nullptr;
		_currentCell.ColumnIndex = DataGridCellInfo::InvalidIndex;
	}
	else
	{
		_currentCell.ColumnIndex = (std::min)(
			_currentCell.ColumnIndex, _columns.size() - 1);
		_currentCell.Column = _columns[_currentCell.ColumnIndex].get();
	}
	return RaiseCurrentCellChanged(previous);
}

void DataGrid::FlushCommittedItemsSourceState()
{
	ControlWeakReference ownerLifetime(this);
	if (_autoColumnsChangedDuringPreparation)
	{
		_autoColumnsChangedDuringPreparation = false;
		RefreshHeaderPresenter();
		if (!ownerLifetime.Get()) return;
	}
	const bool raiseCurrentCell = _currentCellChangeDeferred;
	auto previousCurrentCell = std::move(_deferredCurrentCellOld);
	_currentCellChangeDeferred = false;
	_deferredCurrentCellOld = {};
	const bool raiseSelectedCells = _selectedCellsChangeDeferred;
	auto previousSelectedCells = std::move(_deferredSelectedCellsOld);
	const auto committedSelectedCells = _selectedCells;
	const bool ignoreLocators = _deferredSelectedCellsIgnoreLocators;
	_selectedCellsChangeDeferred = false;
	_deferredSelectedCellsOld = {};
	_deferredSelectedCellsIgnoreLocators = false;
	if (raiseCurrentCell)
	{
		if (!RaiseCurrentCellChanged(std::move(previousCurrentCell))) return;
	}
	if (raiseSelectedCells)
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !SameCellCollection(
			live->_selectedCells, committedSelectedCells, ignoreLocators)) return;
		(void)live->RaiseSelectedCellsChangedCore(
			previousSelectedCells, committedSelectedCells, ignoreLocators);
	}
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
		live->FlushAutoGeneratedColumnsEvent();
}

bool DataGrid::BeginEdit()
{
	return BeginEdit(nullptr);
}

bool DataGrid::BeginEdit(const RoutedEventArgs* editingEventArgs)
{
	const ControlWeakReference ownerLifetime(this);
	const bool result = BeginEditCore(editingEventArgs, false);
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		live->InvalidateCommandState();
		return result;
	}
	return false;
}

DataGridEditingUnit DataGrid::ResolveCommandEditingUnit(
	const std::any& parameter) const noexcept
{
	if (const auto* unit = std::any_cast<DataGridEditingUnit>(&parameter))
		return *unit;
	const auto* cell = ResolveCurrentCellContainer();
	return cell && cell->GetIsEditing()
		? DataGridEditingUnit::Cell : DataGridEditingUnit::Row;
}

bool DataGrid::HasCurrentCellValidationError() const noexcept
{
	const auto* cell = ResolveCurrentCellContainer();
	if (!cell) return false;
	const auto* editor = cell->GetEditingElement();
	return (editor && editor->HasValidationErrors())
		|| cell->HasValidationErrors();
}

bool DataGrid::HasSelectedDataRows() const noexcept
{
	if (!_itemsView) return false;
	const size_t dataCount = _itemsView->Count();
	if (dataCount == 0) return false;
	const auto selected = GetSelectedIndices();
	if (selected.IsRangeBacked())
	{
		const size_t domain = (std::min)(selected.RangeCount(), dataCount);
		return domain > selected.ExcludedIndices().CountInRange(0, domain);
	}
	return std::any_of(selected.DenseValues().begin(),
		selected.DenseValues().end(), [dataCount](int index)
		{
			return index >= 0 && static_cast<size_t>(index) < dataCount;
		});
}

void DataGrid::InvalidateCommandState()
{
	(void)RoutedCommandManager::InvalidateRequerySuggested(*this);
}

void DataGrid::OnCanExecuteBeginEdit(CanExecuteRoutedEventArgs& args)
{
	auto* cell = ResolveCurrentCellContainer();
	bool canExecute = IsEffectivelyEnabled() && !_isReadOnly
		&& cell && !cell->GetIsEditing() && !cell->GetIsReadOnly()
		&& !HasCurrentCellValidationError();
	if (canExecute)
	{
		if (auto* row = cell->GetRowOwner();
			row && row->GetHasValidationError())
		{
			canExecute = _editingRowItem
				&& IsRowEditIdentity(row->GetItem(), row->ItemIndex());
		}
	}
	args.CanExecute = canExecute;
	if (!canExecute) args.ContinueRouting = true;
}

void DataGrid::OnExecutedBeginEdit(ExecutedRoutedEventArgs&)
{
	(void)BeginEdit();
}

void DataGrid::OnCanExecuteCommitEdit(CanExecuteRoutedEventArgs& args)
{
	const auto unit = ResolveCommandEditingUnit(args.Parameter);
	auto* cell = ResolveCurrentCellContainer();
	auto* row = cell ? cell->GetRowOwner() : nullptr;
	const bool rowPending = unit == DataGridEditingUnit::Row
		&& row && _editingRowItem
		&& IsRowEditIdentity(row->GetItem(), row->ItemIndex());
	const bool canExecute = IsEffectivelyEnabled() && cell
		&& (cell->GetIsEditing()
			|| (!HasCurrentCellValidationError() && rowPending));
	args.CanExecute = canExecute;
	if (!canExecute) args.ContinueRouting = true;
}

void DataGrid::OnExecutedCommitEdit(ExecutedRoutedEventArgs& args)
{
	(void)CommitEdit(ResolveCommandEditingUnit(args.Parameter), true);
}

void DataGrid::OnCanExecuteCancelEdit(CanExecuteRoutedEventArgs& args)
{
	const auto unit = ResolveCommandEditingUnit(args.Parameter);
	auto* cell = ResolveCurrentCellContainer();
	auto* row = cell ? cell->GetRowOwner() : nullptr;
	const bool rowPending = unit == DataGridEditingUnit::Row
		&& row && _editingRowItem
		&& IsRowEditIdentity(row->GetItem(), row->ItemIndex());
	const bool canExecute = IsEffectivelyEnabled() && cell
		&& (cell->GetIsEditing()
			|| (!HasCurrentCellValidationError() && rowPending));
	args.CanExecute = canExecute;
	if (!canExecute) args.ContinueRouting = true;
}

void DataGrid::OnExecutedCancelEdit(ExecutedRoutedEventArgs& args)
{
	(void)CancelEdit(ResolveCommandEditingUnit(args.Parameter));
}

void DataGrid::OnCanExecuteDelete(CanExecuteRoutedEventArgs& args)
{
	auto* cell = ResolveCurrentCellContainer();
	args.CanExecute = IsEffectivelyEnabled() && _canUserDeleteRows
		&& HasSelectedDataRows() && (!cell || !cell->GetIsEditing());
}

void DataGrid::OnExecutedDelete(ExecutedRoutedEventArgs&)
{
	(void)DeleteSelectedRows();
}

void DataGrid::OnCanExecuteCopy(CanExecuteRoutedEventArgs& args)
{
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing())
	{
		auto* editor = cell->GetEditingElement();
		if (editor && IsRoutedDescendantOrSelf(args.OriginalSource, editor))
		{
			// A TextBox editor owns Ctrl+C. WPF reaches its closer Copy binding
			// before DataGrid; CUI expresses that route priority by allowing the
			// direct editor input path to continue.
			args.ContinueRouting = true;
			return;
		}
	}
	args.CanExecute = IsEffectivelyEnabled()
		&& _clipboardCopyMode != DataGridClipboardCopyMode::None
		&& !_selectedCells.empty()
		&& _selectedCells.size() <= ClipboardCopyCellLimit;
}

void DataGrid::OnExecutedCopy(ExecutedRoutedEventArgs&)
{
	(void)Copy();
}

void DataGrid::OnCopyingRowClipboardContent(
	DataGridRowClipboardEventArgs& args)
{
	ControlWeakReference ownerLifetime(this);
	const size_t selectionRevision = _cellSelectionRevision;
	const auto items = GetItemsView();
	std::vector<DataGridColumn*> schema;
	schema.reserve(_columns.size());
	for (const auto& column : _columns) schema.push_back(column.get());
	const auto stateIsCurrent = [&]()
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_cellSelectionRevision != selectionRevision
			|| live->GetItemsView().Shared() != items.Shared()
			|| live->_columns.size() != schema.size()) return false;
		for (size_t index = 0; index < schema.size(); ++index)
			if (live->_columns[index].get() != schema[index]) return false;
		return true;
	};
	if (args.StartColumnDisplayIndex <= args.EndColumnDisplayIndex)
	{
		for (size_t displayIndex = args.StartColumnDisplayIndex;; ++displayIndex)
		{
			auto* column = GetColumnFromDisplayIndex(displayIndex);
			if (!column) break;
			if (column->GetVisibility() == Visibility::Visible)
			{
				BindingValue content;
				if (args.IsColumnHeadersRow)
					content = column->GetHeader();
				else if (args.RowIndexHint != DataGridCellInfo::InvalidIndex)
				{
					DataGridCellInfo cell;
					if (TryCreateCellInfo(
						args.RowIndexHint, displayIndex, cell)
						&& _selectedCells.Contains(cell))
					{
						std::wstring value;
						if (TryReadAccessibilityCellValue(
							args.RowIndexHint, displayIndex, value))
							content = BindingValue(std::move(value));
					}
				}
				if (!stateIsCurrent())
				{
					args.ClipboardRowContent.clear();
					return;
				}
				args.ClipboardRowContent.push_back({
					args.Item, column, std::move(content) });
			}
			if (!ownerLifetime.Get()
				|| displayIndex == args.EndColumnDisplayIndex) break;
		}
	}
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		live && stateIsCurrent())
		cui::framework::EventAccess::RaiseWhile(
			live->CopyingRowClipboardContent,
			[&ownerLifetime]
			{ return ownerLifetime.Get() != nullptr; }, live, args);
}

bool DataGrid::TryBuildClipboardText(std::wstring& result)
{
	result.clear();
	if (_clipboardCopyMode == DataGridClipboardCopyMode::None) return false;
	const size_t selectedCount = _selectedCells.size();
	if (selectedCount == 0 || selectedCount > ClipboardCopyCellLimit)
		return false;

	const ControlWeakReference ownerLifetime(this);
	const size_t selectionRevision = _cellSelectionRevision;
	const auto items = GetItemsView();
	if (!items) return false;
	std::vector<DataGridColumn*> schema;
	schema.reserve(_columns.size());
	for (const auto& column : _columns) schema.push_back(column.get());
	const auto schemaIsCurrent = [&]()
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_cellSelectionRevision != selectionRevision
			|| live->GetItemsView().Shared() != items.Shared()
			|| live->_columns.size() != schema.size()) return false;
		for (size_t index = 0; index < schema.size(); ++index)
			if (live->_columns[index].get() != schema[index]) return false;
		return true;
	};

	const auto selected = _selectedCells;
	size_t minRow = DataGridCellInfo::InvalidIndex;
	size_t maxRow = 0;
	size_t minDisplay = DataGridCellInfo::InvalidIndex;
	size_t maxDisplay = 0;
	for (size_t index = 0; index < selectedCount; ++index)
	{
		const auto cell = selected.at(index);
		if (!schemaIsCurrent() || !cell.IsValid() || !cell.Column)
			return false;
		const size_t displayIndex = cell.Column->GetDisplayIndex();
		if (displayIndex == DataGridColumn::UnsetDisplayIndex
			|| displayIndex >= schema.size()
			|| schema[displayIndex] != cell.Column) return false;
		minRow = (std::min)(minRow, cell.RowIndex);
		maxRow = (std::max)(maxRow, cell.RowIndex);
		minDisplay = (std::min)(minDisplay, displayIndex);
		maxDisplay = (std::max)(maxDisplay, displayIndex);
	}
	if (minRow == DataGridCellInfo::InvalidIndex
		|| minDisplay == DataGridCellInfo::InvalidIndex) return false;
	const size_t rowSpan = maxRow - minRow + 1;
	const size_t columnSpan = maxDisplay - minDisplay + 1;
	if (columnSpan != 0
		&& rowSpan > ClipboardCopyCellLimit / columnSpan) return false;

	size_t serializedCells = 0;
	const auto appendText = [&](std::wstring_view text) -> bool
	{
		if (text.size() > ClipboardCopyCharacterLimit - result.size())
			return false;
		result.append(text);
		return true;
	};
	const auto appendField = [&](std::wstring_view value) -> bool
	{
		const bool quote = value.find_first_of(L"\t\r\n\"")
			!= std::wstring_view::npos;
		if (!quote) return appendText(value);
		if (!appendText(L"\"")) return false;
		for (const wchar_t character : value)
		{
			if (character == L'\"' && !appendText(L"\"")) return false;
			if (result.size() == ClipboardCopyCharacterLimit) return false;
			result.push_back(character);
		}
		return appendText(L"\"");
	};
	const auto appendRow = [&](const DataGridRowClipboardEventArgs& args)
		-> bool
	{
		if (args.ClipboardRowContent.size()
			> ClipboardCopyCellLimit - serializedCells) return false;
		serializedCells += args.ClipboardRowContent.size();
		for (size_t index = 0;
			index < args.ClipboardRowContent.size(); ++index)
		{
			if (index != 0 && !appendText(L"\t")) return false;
			if (!appendField(
				args.ClipboardRowContent[index].Content.ToString()))
				return false;
		}
		return appendText(L"\r\n");
	};
	const auto raiseRow = [&](DataGridRowClipboardEventArgs& args) -> bool
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !schemaIsCurrent()) return false;
		live->OnCopyingRowClipboardContent(args);
		return schemaIsCurrent() && appendRow(args);
	};

	if (_clipboardCopyMode == DataGridClipboardCopyMode::IncludeHeader)
	{
		DataGridRowClipboardEventArgs header;
		header.StartColumnDisplayIndex = minDisplay;
		header.EndColumnDisplayIndex = maxDisplay;
		header.IsColumnHeadersRow = true;
		if (!raiseRow(header)) return false;
	}
	for (size_t rowIndex = minRow;; ++rowIndex)
	{
		if (!schemaIsCurrent() || rowIndex >= items.Get()->Count()) return false;
		bool intersects = false;
		for (size_t displayIndex = minDisplay;; ++displayIndex)
		{
			DataGridCellInfo cell;
			if (!TryCreateCellInfo(rowIndex, displayIndex, cell)
				|| !schemaIsCurrent()) return false;
			if (selected.Contains(cell)) intersects = true;
			if (displayIndex == maxDisplay) break;
		}
		if (intersects)
		{
			BindingSourceReference item;
			if (!items.Get()->TryGetItem(rowIndex, item)
				|| !item || !schemaIsCurrent()) return false;
			DataGridRowClipboardEventArgs row;
			row.Item = std::move(item);
			row.StartColumnDisplayIndex = minDisplay;
			row.EndColumnDisplayIndex = maxDisplay;
			row.RowIndexHint = rowIndex;
			if (!raiseRow(row)) return false;
		}
		if (rowIndex == maxRow) break;
	}
	return !result.empty() && schemaIsCurrent();
}

bool DataGrid::Copy()
{
	std::wstring text;
	if (!TryBuildClipboardText(text)) return false;
	cui::richtext::clipboard::DataObject data;
	data.PlainText = std::move(text);
	return cui::richtext::clipboard::Publish(
		GetPresentationWindow() ? GetPresentationWindow()->Handle : nullptr,
		data);
}

IEditableBindingList* DataGrid::EditableItems() const noexcept
{
	return _itemsView
		? dynamic_cast<IEditableBindingList*>(_itemsView.get()) : nullptr;
}

bool DataGrid::IsNewItemPlaceholder(
	const BindingSourceReference& item) const noexcept
{
	return item && _displayItemsView
		&& item.Shared() == _displayItemsView->Placeholder().Shared();
}

bool DataGrid::IsAddingNewItem() const noexcept
{
	const auto* editable = EditableItems();
	return editable && editable->IsAddingNew();
}

bool DataGrid::TryGetPendingNewItemOccurrence(
	const BindingSourceReference& item,
	size_t& occurrence) const noexcept
{
	occurrence = DataGridCellInfo::InvalidIndex;
	const auto* editable = EditableItems();
	size_t currentAddIndex = DataGridCellInfo::InvalidIndex;
	return item && editable && _itemsView && editable->IsAddingNew()
		&& editable->CurrentAddItem().Shared() == item.Shared()
		&& editable->TryGetCurrentAddIndex(currentAddIndex)
		&& _itemsView->TryGetItemOccurrenceIdentity(
			currentAddIndex, occurrence)
		&& occurrence != 0
		&& occurrence != DataGridCellInfo::InvalidIndex;
}

bool DataGrid::IsPendingNewItem(
	const BindingSourceReference& item,
	size_t rowIndex) const noexcept
{
	if (rowIndex == DataGridCellInfo::InvalidIndex) return false;
	size_t currentAddOccurrence = DataGridCellInfo::InvalidIndex;
	size_t rowOccurrence = DataGridCellInfo::InvalidIndex;
	return _displayItemsView
		&& TryGetPendingNewItemOccurrence(item, currentAddOccurrence)
		&& _displayItemsView->TryGetItemOccurrenceIdentity(
			rowIndex, rowOccurrence)
		&& currentAddOccurrence == rowOccurrence;
}

void DataGrid::RefreshNewItemContainerStates()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, visual] : GetRealizedItems())
	{
		(void)visual;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner || !row || row->GetDataGridOwner() != owner) continue;
		const auto item = row->GetItem();
		const size_t rowIndex = row->ItemIndex();
		const bool isPlaceholder = owner->IsNewItemPlaceholder(item);
		row->SetCurrentIsNewItem(
			isPlaceholder
			|| owner->IsPendingNewItem(item, rowIndex));
		owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner || !row || row->GetDataGridOwner() != owner
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != rowIndex) continue;
		std::vector<ControlWeakReference> cells;
		cells.reserve(row->GetCells().size());
		for (auto* cell : row->GetCells())
			if (cell) cells.emplace_back(cell);
		for (const auto& cellLifetime : cells)
		{
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			auto* cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			if (!owner || !row || row->GetDataGridOwner() != owner
				|| row->GetItem().Shared() != item.Shared()
				|| row->ItemIndex() != rowIndex) return;
			if (!cell || cell->GetRowOwner() != row) continue;
			cell->SetCurrentIsNewItemPlaceholder(isPlaceholder);
		}
	}
}

void DataGrid::RefreshNewItemPlaceholder()
{
	if (_settingItemsSource || _destroyingDataGrid || !_displayItemsView)
		return;
	const bool visible = _canUserAddRows && !_startingNewItem
		&& !IsAddingNewItem();
	_displayItemsView->SetPlaceholderVisible(visible);
}

bool DataGrid::BeginNewItemFromPlaceholder(
	DataGridColumn& column,
	DataGridCellInfo& identity)
{
	identity = {};
	if (!_canUserAddRows || _isReadOnly || !IsEnabled
		|| _startingNewItem || _callingEditableList) return false;
	const auto view = _itemsView;
	auto* editable = EditableItems();
	if (!view || !editable || !editable->CanAddNew()) return false;
	const ControlWeakReference ownerLifetime(this);
	_startingNewItem = true;
	RefreshNewItemPlaceholder();
	bool added = false;
	bool completed = false;
	BindingSourceReference newItem;
	size_t newItemOccurrence = DataGridCellInfo::InvalidIndex;
	auto finishStarting = MakeScopeExit(
		[ownerLifetime, view, &column, &added, &completed,
			&newItem, &newItemOccurrence]
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!owner) return;
			owner->_startingNewItem = false;
			if (added && !completed && owner->_itemsView == view)
			{
				size_t currentOccurrence = DataGridCellInfo::InvalidIndex;
				try
				{
					if (owner->TryGetPendingNewItemOccurrence(
							newItem, currentOccurrence)
						&& currentOccurrence == newItemOccurrence)
						(void)owner->CancelPendingNewItem(&column);
				}
				catch (...) {}
			}
			owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (owner) owner->RefreshNewItemPlaceholder();
		});

	BindingSourceReference candidate;
	DataGridAddingNewItemEventArgs args;
	cui::framework::EventAccess::RaiseWhile(
		AddingNewItem,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && owner->_itemsView == view
				&& std::any_of(owner->_columns.begin(),
					owner->_columns.end(), [&column](const auto& value)
					{ return value.get() == &column; });
		},
		this, args);
	candidate = std::move(args.NewItem);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != view
		|| !std::any_of(live->_columns.begin(), live->_columns.end(),
			[&column](const auto& value) { return value.get() == &column; }))
		return false;
	editable = live->EditableItems();
	if (!editable || !editable->CanAddNew()
		|| (candidate && !editable->CanAddNewItem())) return false;

	live->_callingEditableList = true;
	auto resetListCall = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_callingEditableList = false;
	});
	if (!editable->AddNewItem(std::move(candidate), newItem)
		|| !newItem) return false;
	added = true;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != view) return false;
	if (!live->TryGetPendingNewItemOccurrence(
			newItem, newItemOccurrence))
		return false;

	DataGridInitializingNewItemEventArgs initializing;
	initializing.NewItem = newItem;
	cui::framework::EventAccess::RaiseWhile(
		live->InitializingNewItem,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			size_t currentOccurrence = DataGridCellInfo::InvalidIndex;
			return owner && owner->_itemsView == view
				&& owner->TryGetPendingNewItemOccurrence(
					newItem, currentOccurrence)
				&& currentOccurrence == newItemOccurrence;
		},
		live, initializing);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	size_t currentOccurrence = DataGridCellInfo::InvalidIndex;
	if (!live || live->_itemsView != view
		|| !live->TryGetPendingNewItemOccurrence(
			newItem, currentOccurrence)
		|| currentOccurrence != newItemOccurrence) return false;
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	editable = live->EditableItems();
	if (!editable || !editable->TryGetCurrentAddIndex(rowIndex)
		|| rowIndex >= live->ItemCount()) return false;
	const auto columnFound = std::find_if(
		live->_columns.begin(), live->_columns.end(),
		[&column](const auto& value) { return value.get() == &column; });
	if (columnFound == live->_columns.end()) return false;
	const size_t columnIndex = static_cast<size_t>(
		std::distance(live->_columns.begin(), columnFound));
	if (!live->SetCurrentCell(rowIndex, columnIndex)) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->TryCreateCellInfo(
			rowIndex, columnIndex, identity)
		|| identity._itemOccurrence != newItemOccurrence
		|| !live->IsPendingNewItem(identity.Item, identity.RowIndex)) return false;
	completed = true;
	return true;
}

bool DataGrid::CancelPendingNewItem(DataGridColumn* restoreColumn)
{
	const auto view = _itemsView;
	auto* editable = EditableItems();
	if (!editable || !editable->IsAddingNew())
	{
		RefreshNewItemPlaceholder();
		return true;
	}
	if (_callingEditableList) return false;
	const ControlWeakReference ownerLifetime(this);
	_callingEditableList = true;
	auto resetListCall = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_callingEditableList = false;
	});
	const bool canceled = editable->CancelNew();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != view || !canceled) return false;
	live->_startingNewItem = false;
	live->RefreshNewItemPlaceholder();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!live->_displayItemsView
		|| !live->_displayItemsView->PlaceholderVisible()
		|| live->_columns.empty()) return true;
	size_t columnIndex = 0;
	if (restoreColumn)
	{
		const auto found = std::find_if(
			live->_columns.begin(), live->_columns.end(),
			[restoreColumn](const auto& column)
			{ return column.get() == restoreColumn; });
		if (found != live->_columns.end())
			columnIndex = static_cast<size_t>(
				std::distance(live->_columns.begin(), found));
	}
	const size_t rowIndex = live->_displayItemsView->Count() - 1;
	return live->SetCurrentCell(rowIndex, columnIndex);
}

bool DataGrid::IsRowEditIdentity(
	const BindingSourceReference& item,
	size_t rowIndex) const
{
	if (!_editingRowItem
		|| item.Shared() != _editingRowItem.Shared()) return false;
	if (_editingRowOccurrence == DataGridCellInfo::InvalidIndex)
		return rowIndex == _editingRowIndex;
	size_t occurrence = DataGridCellInfo::InvalidIndex;
	return TryGetItemOccurrenceAt(rowIndex, occurrence)
		&& occurrence == _editingRowOccurrence;
}

bool DataGrid::TryResolveEditingRowIndex(size_t& rowIndex) const
{
	rowIndex = DataGridCellInfo::InvalidIndex;
	if (!_editingRowItem) return false;
	const auto items = GetItemsView();
	if (!items) return false;
	if (_editingRowIndex < items.Get()->Count())
	{
		BindingSourceReference candidate;
		if (items.Get()->TryGetItem(_editingRowIndex, candidate)
			&& candidate.Shared() == _editingRowItem.Shared())
		{
			if (_editingRowOccurrence == DataGridCellInfo::InvalidIndex)
			{
				rowIndex = _editingRowIndex;
				return true;
			}
			size_t occurrence = DataGridCellInfo::InvalidIndex;
			if (TryGetItemOccurrenceAt(_editingRowIndex, occurrence)
				&& occurrence == _editingRowOccurrence)
			{
				rowIndex = _editingRowIndex;
				return true;
			}
		}
	}
	if (_editingRowOccurrence == DataGridCellInfo::InvalidIndex)
		return false;
	return TryResolveItemOccurrence(
		_editingRowItem, _editingRowOccurrence, rowIndex);
}

void DataGrid::RefreshEditingRowContainers()
{
	const ControlWeakReference ownerLifetime(this);
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, visual] : GetRealizedItems())
	{
		(void)visual;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	for (const auto& rowLifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner || !row || row->GetDataGridOwner() != owner) continue;
		row->SetCurrentIsEditing(owner->IsRowEditIdentity(
			row->GetItem(), row->ItemIndex()));
		if (!ownerLifetime.Get()) return;
	}
}

void DataGrid::ClearRowEditTransaction()
{
	_editingRowItem = {};
	_editingRowOccurrence = DataGridCellInfo::InvalidIndex;
	_editingRowIndex = DataGridCellInfo::InvalidIndex;
	_editingRowHasSourceTransaction = false;
	if (++_rowEditRevision == 0) _rowEditRevision = 1;
	RefreshEditingRowContainers();
}

void DataGrid::AbandonRowEditTransaction(bool cancelSource) noexcept
{
	const auto item = _editingRowItem;
	const bool sourceTransaction = _editingRowHasSourceTransaction;
	try { ClearRowEditTransaction(); }
	catch (...) {}
	if (!cancelSource || !sourceTransaction || !item) return;
	try
	{
		if (auto* editable = dynamic_cast<IEditableBindingSource*>(item.Get()))
			(void)editable->CancelEdit();
	}
	catch (...) {}
}

void DataGrid::ReconcileRowEditTransaction()
{
	if (!_editingRowItem) return;
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	if (!TryResolveEditingRowIndex(rowIndex))
	{
		AbandonRowEditTransaction(true);
		return;
	}
	_editingRowIndex = rowIndex;
	RefreshEditingRowContainers();
}

bool DataGrid::BeginRowEditTransaction(const DataGridCellInfo& identity)
{
	if (!identity.IsValid() || _startingRowEdit || _endingRowEdit)
		return false;
	if (_editingRowItem)
		return IsRowEditIdentity(identity.Item, identity.RowIndex);
	const ControlWeakReference ownerLifetime(this);
	const auto view = GetItemsView();
	const auto item = identity.Item;
	const size_t occurrence = identity._itemOccurrence;
	_startingRowEdit = true;
	auto resetStarting = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_startingRowEdit = false;
	});
	auto* editable = dynamic_cast<IEditableBindingSource*>(item.Get());
	bool sourceTransaction = false;
	if (editable)
	{
		sourceTransaction = editable->BeginEdit();
		if (!sourceTransaction) return false;
	}
	auto cancelDetachedSource = [&]() noexcept
	{
		if (!sourceTransaction || !editable) return;
		try { (void)editable->CancelEdit(); }
		catch (...) {}
	};
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->GetItemsView().Shared() != view.Shared()
		|| !(live->_currentCell == identity))
	{
		cancelDetachedSource();
		return false;
	}
	DataGridCellInfo current;
	if (!live->TryCreateCellInfo(
		identity.RowIndex, identity.ColumnIndex, current)
		|| !(current == identity))
	{
		cancelDetachedSource();
		return false;
	}
	live->_editingRowItem = item;
	live->_editingRowOccurrence = occurrence;
	live->_editingRowIndex = identity.RowIndex;
	live->_editingRowHasSourceTransaction = sourceTransaction;
	if (++live->_rowEditRevision == 0) live->_rowEditRevision = 1;
	try
	{
		live->RefreshEditingRowContainers();
	}
	catch (...)
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->AbandonRowEditTransaction(true);
		throw;
	}
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::BeginEditCore(
	const RoutedEventArgs* editingEventArgs,
	bool toggleCheckBox)
{
	ControlWeakReference ownerLifetime(this);
	if (_raisingBeginningEdit || _isReadOnly
		|| !_currentCell.IsValid()) return false;
	DataGridCellInfo editingIdentity = _currentCell;
	DataGridColumn* editingColumn = editingIdentity.Column;
	bool addedNewItem = false;
	bool cellEnteredEdit = false;
	auto rollbackNewItem = MakeScopeExit(
		[ownerLifetime, &addedNewItem, &cellEnteredEdit, &editingColumn]
		{
			if (!addedNewItem || cellEnteredEdit) return;
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			{
				try { (void)owner->CancelPendingNewItem(editingColumn); }
				catch (...) {}
			}
		});
	(void)BringItemIntoView(_currentCell.RowIndex);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !(live->_currentCell == editingIdentity)) return false;
	if (live->_enableColumnVirtualization
		&& !live->ResolveCurrentCellContainer())
	{
		if (!live->BringColumnIntoView(editingIdentity.ColumnIndex)) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !(live->_currentCell == editingIdentity)) return false;
	}
	auto* cell = live->ResolveCurrentCellContainer();
	auto* row = cell ? cell->GetRowOwner() : nullptr;
	if (!cell || !row || cell->GetIsReadOnly()) return false;
	if (live->IsNewItemPlaceholder(editingIdentity.Item))
	{
		editingColumn = cell->GetColumn();
		if (!editingColumn
			|| !live->BeginNewItemFromPlaceholder(
				*editingColumn, editingIdentity)) return false;
		addedNewItem = true;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !(live->_currentCell == editingIdentity)) return false;
		(void)live->BringItemIntoView(editingIdentity.RowIndex);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || !(live->_currentCell == editingIdentity)) return false;
		cell = live->ResolveCurrentCellContainer();
		row = cell ? cell->GetRowOwner() : nullptr;
		if (!cell || !row || cell->GetIsReadOnly()) return false;
	}
	if (cell->GetIsEditing()) return true;
	ControlWeakReference cellLifetime(cell);
	ControlWeakReference rowLifetime(row);
	DataGridBeginningEditEventArgs beginning;
	beginning.Column = cell->GetColumn();
	beginning.Row = row;
	beginning.Cell = cell;
	beginning.EditingEventArgs = editingEventArgs;
	{
		live->_raisingBeginningEdit = true;
		auto resetBeginning = MakeScopeExit([ownerLifetime]
		{
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				owner->_raisingBeginningEdit = false;
		});
		cui::framework::EventAccess::RaiseWhile(
			live->BeginningEdit,
			[&]()
			{
				auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				return owner && cellLifetime.Get() && rowLifetime.Get()
					&& std::any_of(owner->_columns.begin(),
						owner->_columns.end(), [&](const auto& candidate)
						{ return candidate.get() == beginning.Column; });
			},
			live, beginning);
	}
	if (beginning.Cancel) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)) return false;
	row = cell->GetRowOwner();
	if (!row) return false;
	const bool rowTransactionAlreadyActive = !!live->_editingRowItem;
	if (!live->BeginRowEditTransaction(editingIdentity)) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)) return false;
	const size_t startedRowRevision = live->_rowEditRevision;
	auto rollbackNewRowTransaction = MakeScopeExit(
		[ownerLifetime, rowTransactionAlreadyActive,
			startedRowRevision, &cellEnteredEdit]
		{
			if (rowTransactionAlreadyActive || cellEnteredEdit) return;
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				owner && owner->_rowEditRevision == startedRowRevision)
			{
				if (auto* current = owner->ResolveCurrentCellContainer();
					current && current->GetIsEditing())
				{
					std::wstring ignored;
					(void)current->ReplaceContent(false, &ignored);
				}
				owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (owner && owner->_rowEditRevision == startedRowRevision)
					owner->AbandonRowEditTransaction(true);
			}
		});
	std::wstring error;
	const bool replaced = cell->ReplaceContent(true, &error);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)) return false;
	if (!replaced)
	{
		live->SetLastTemplateError(std::move(error));
		return false;
	}
	row = cell->GetRowOwner();
	if (!row || !cell->GetIsEditing()) return false;
	auto* editingElement = cell->GetEditingElement();
	if (auto* text = dynamic_cast<TextBox*>(editingElement))
	{
		const ControlWeakReference editorLifetime(text);
		if (auto* window = live->GetPresentationWindow())
			window->SetKeyboardFocus(text, false);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		text = dynamic_cast<TextBox*>(editorLifetime.Get());
		if (!live || !cell || !text
			|| live->ResolveCurrentCellContainer() != cell
			|| cell->GetEditingElement() != text
			|| !(live->_currentCell == editingIdentity)
			|| !cell->GetIsEditing()) return false;
		if (const auto* composition =
			dynamic_cast<const TextCompositionEventArgs*>(editingEventArgs))
		{
			text->SelectAll();
			text = dynamic_cast<TextBox*>(editorLifetime.Get());
			if (!text) return false;
			text->InsertText(composition->Text);
		}
		else if (const auto* mouse =
			dynamic_cast<const MouseEventArgs*>(editingEventArgs))
		{
			D2D1_POINT_2F renderPoint{};
			bool hasPoint = mouse->HasRootPosition;
			if (hasPoint)
				renderPoint = D2D1::Point2F(mouse->RootX, mouse->RootY);
			else if (cell)
			{
				renderPoint = ToRenderSpace(
					*cell,
					static_cast<float>(mouse->X),
					static_cast<float>(mouse->Y));
				hasPoint = true;
			}
			D2D1_POINT_2F local{};
			if (hasPoint && text->TryTransformRenderPointToLocal(
				renderPoint, local))
				text->SetCaretIndex(text->GetCharacterIndexFromPoint(
					local.x, local.y));
			else text->SelectAll();
		}
		else text->SelectAll();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!live || !cell || live->ResolveCurrentCellContainer() != cell
			|| !(live->_currentCell == editingIdentity)
			|| !cell->GetIsEditing()) return false;
		row = cell->GetRowOwner();
		if (!row) return false;
	}
	if (toggleCheckBox)
	{
		auto* checkBox = dynamic_cast<CheckBox*>(cell->GetEditingElement());
		if (checkBox)
		{
			ControlWeakReference editorLifetime(checkBox);
			checkBox->Toggle();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			if (!live || !cell || !editorLifetime.Get()
				|| live->ResolveCurrentCellContainer() != cell
				|| !(live->_currentCell == editingIdentity)
				|| cell->GetEditingElement() != editorLifetime.Get()
				|| !cell->GetIsEditing()) return false;
			row = cell->GetRowOwner();
			if (!row) return false;
		}
	}
	// WPF opens a DataGridComboBoxColumn's dropdown when the second click
	// enters edit mode, because that press reaches the live ComboBox.  Here
	// the editing ComboBox is created after the press was consumed by the
	// display element, so open the dropdown manually to keep parity.
	if (editingEventArgs
		&& dynamic_cast<const MouseEventArgs*>(editingEventArgs))
	{
		if (auto* combo = dynamic_cast<ComboBox*>(cell->GetEditingElement()))
		{
			ControlWeakReference editorLifetime(combo);
			// The mouse press creates this editor and requests its dropdown in the
			// same routed-input turn. Materialize the themed face before opening so
			// ComboBox resolves its authored Popup instead of a popup-only fallback.
			(void)combo->ApplyTemplate();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			combo = dynamic_cast<ComboBox*>(editorLifetime.Get());
			if (!live || !cell || !combo
				|| live->ResolveCurrentCellContainer() != cell
				|| !(live->_currentCell == editingIdentity)
				|| cell->GetEditingElement() != combo
				|| !cell->GetIsEditing()) return false;
			combo->SetIsDropDownOpen(true);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			if (!live || !cell || !editorLifetime.Get()
				|| live->ResolveCurrentCellContainer() != cell
				|| !(live->_currentCell == editingIdentity)
				|| cell->GetEditingElement() != editorLifetime.Get()
				|| !cell->GetIsEditing()) return false;
			row = cell->GetRowOwner();
			if (!row) return false;
		}
	}
	DataGridPreparingCellForEditEventArgs preparing;
	preparing.Column = cell->GetColumn();
	preparing.Row = row;
	preparing.Cell = cell;
	preparing.EditingElement = cell->GetEditingElement();
	preparing.EditingEventArgs = editingEventArgs;
	ControlWeakReference preparingRowLifetime(row);
	ControlWeakReference editorLifetime(preparing.EditingElement);
	cui::framework::EventAccess::RaiseWhile(
		live->PreparingCellForEdit,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && cellLifetime.Get() && preparingRowLifetime.Get()
				&& (!preparing.EditingElement || editorLifetime.Get())
				&& std::any_of(owner->_columns.begin(),
					owner->_columns.end(), [&](const auto& candidate)
					{ return candidate.get() == preparing.Column; });
		},
		live, preparing);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)
		|| !cell->GetIsEditing()) return false;
	if (auto* editor = cell->GetEditingElement())
	{
		const ControlWeakReference finalEditorLifetime(editor);
		if (auto* window = live->GetPresentationWindow();
			window && window->GetKeyboardFocusedElement() != editor)
			window->SetKeyboardFocus(editor, false);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		editor = finalEditorLifetime.Get();
		if (!live || !cell || !editor
			|| live->ResolveCurrentCellContainer() != cell
			|| !(live->_currentCell == editingIdentity)
			|| cell->GetEditingElement() != editor
			|| !cell->GetIsEditing()) return false;
		// Binding transfer, activation-specific caret/selection preparation and
		// PreparingCellForEdit may all happen after the editor's first retained
		// command list was recorded.  Publish one final content revision so the
		// first visible edit frame already contains the bound text and selection,
		// instead of waiting for the user's next pointer/key report.
		editor->InvalidateVisual();
	}
	cellEnteredEdit = ownerLifetime.Get() != nullptr;
	return cellEnteredEdit;
}

bool DataGrid::CommitEdit()
{
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing())
		return CommitEdit(DataGridEditingUnit::Cell, true);
	if (_editingRowItem)
		return CommitEdit(DataGridEditingUnit::Row, true);
	return true;
}

bool DataGrid::CommitEdit(
	DataGridEditingUnit editingUnit,
	bool exitEditingMode)
{
	const ControlWeakReference ownerLifetime(this);
	bool result = false;
	if (editingUnit == DataGridEditingUnit::Row)
		result = EndRowEdit(DataGridEditAction::Commit, exitEditingMode);
	else if (editingUnit == DataGridEditingUnit::Cell)
	{
		if (auto* cell = ResolveCurrentCellContainer();
			cell && cell->GetIsEditing())
			result = CommitCellEdit(exitEditingMode);
		else result = exitEditingMode ? true : BeginEdit();
	}
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		live->InvalidateCommandState();
		return result;
	}
	return false;
}

bool DataGrid::CommitCellEdit(bool exitEditingMode)
{
	ControlWeakReference ownerLifetime(this);
	if (_endingCellEdit || _endingRowEdit) return false;
	auto* cell = ResolveCurrentCellContainer();
	if (!cell || !cell->GetIsEditing()) return false;
	_endingCellEdit = true;
	auto resetEnding = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_endingCellEdit = false;
	});
	const DataGridCellInfo editingIdentity = _currentCell;
	ControlWeakReference cellLifetime(cell);
	ControlWeakReference editorLifetime(cell->GetEditingElement());
	DataGridCellEditEndingEventArgs ending;
	ending.Column = cell->GetColumn();
	ending.Row = cell->GetRowOwner();
	ending.Cell = cell;
	ending.EditingElement = cell->GetEditingElement();
	ending.EditAction = DataGridEditAction::Commit;
	ControlWeakReference rowLifetime(ending.Row);
	cui::framework::EventAccess::RaiseWhile(
		CellEditEnding,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && cellLifetime.Get() && rowLifetime.Get()
				&& (!ending.EditingElement || editorLifetime.Get())
				&& std::any_of(owner->_columns.begin(),
					owner->_columns.end(), [&](const auto& candidate)
					{ return candidate.get() == ending.Column; });
		},
		this, ending);
	if (ending.Cancel) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)
		|| !cell->GetIsEditing()) return false;
	if (auto* editor = dynamic_cast<Control*>(editorLifetime.Get()))
	{
		if (cell->GetEditingElement() != editor) return false;
		bool updated = true;
		if (dynamic_cast<TextBox*>(editor))
			updated = editor->DataBindings.UpdateSource(TextBox::TextProperty());
		else if (dynamic_cast<CheckBox*>(editor))
			updated = editor->DataBindings.UpdateSource(
				ToggleButton::IsCheckedProperty());
		if (!updated) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
		if (!live || !cell || live->ResolveCurrentCellContainer() != cell
			|| !(live->_currentCell == editingIdentity)
			|| !cell->GetIsEditing()) return false;
		editor = dynamic_cast<Control*>(editorLifetime.Get());
		if (!editor || cell->GetEditingElement() != editor) return false;
		if (editor->HasValidationErrors())
		{
			if (auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get()))
				(void)row->RefreshValidationState();
			return false;
		}
	}
	std::wstring error;
	const bool replaced = cell->ReplaceContent(false, &error);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!replaced)
	{
		live->SetLastTemplateError(std::move(error));
		return false;
	}
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (cell)
		if (auto* row = cell->GetRowOwner())
			if (!row->AttachValidationTracking()) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)) return false;
	if (exitEditingMode)
	{
		if (auto* window = live->GetPresentationWindow())
		{
			auto* focusTarget = cell && live->ResolveCurrentCellContainer() == cell
				? static_cast<Control*>(cell) : static_cast<Control*>(live);
			window->SetKeyboardFocus(focusTarget, true);
		}
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	// The binding source has just accepted an edited value.  It need not publish
	// a collection Replace notification, so explicitly retire the sampled cell
	// widths and resolve Auto/SizeToCells columns against the committed value.
	live->InvalidateColumnContentWidthCache();
	live->RefreshColumnWidths();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	return exitEditingMode ? true : live->BeginEdit();
}

bool DataGrid::CancelEdit()
{
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing())
		return CancelEdit(DataGridEditingUnit::Cell);
	if (_editingRowItem)
		return CancelEdit(DataGridEditingUnit::Row);
	return true;
}

bool DataGrid::CancelEdit(DataGridEditingUnit editingUnit)
{
	const ControlWeakReference ownerLifetime(this);
	bool result = false;
	if (editingUnit == DataGridEditingUnit::Row)
		result = EndRowEdit(DataGridEditAction::Cancel, true);
	else if (editingUnit == DataGridEditingUnit::Cell)
	{
		if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing()) result = CancelCellEdit();
		else result = true;
	}
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		live->InvalidateCommandState();
		return result;
	}
	return false;
}

bool DataGrid::DeleteSelectedRows()
{
	if (!_canUserDeleteRows || _isReadOnly || !IsEnabled
		|| _deletingRows || _callingEditableList) return false;
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing()) return false;
	const ControlWeakReference ownerLifetime(this);
	if (_editingRowItem)
	{
		size_t editingRowIndex = DataGridCellInfo::InvalidIndex;
		const bool wasAdding = IsPendingNewItem(
			_editingRowItem,
			TryResolveEditingRowIndex(editingRowIndex)
				? editingRowIndex : DataGridCellInfo::InvalidIndex);
		if (!CancelEdit(DataGridEditingUnit::Row)) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		if (wasAdding) return true;
	}
	else if (IsAddingNewItem())
	{
		return CancelPendingNewItem(_currentCell.Column);
	}

	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->_itemsView) return false;
	const auto itemsView = live->_itemsView;
	auto* editable = live->EditableItems();
	if (!editable || !editable->CanRemove()) return false;
	const size_t dataCount = itemsView->Count();
	const SelectedIndexCollection selected = live->GetSelectedIndices();
	if (selected.empty() || dataCount == 0) return false;

	std::vector<std::pair<size_t, size_t>> ranges;
	if (selected.IsRangeBacked())
	{
		const size_t selectedDomain = (std::min)(
			selected.RangeCount(), dataCount);
		size_t cursor = 0;
		for (const auto& excluded : selected.ExcludedIntervals())
		{
			if (cursor >= selectedDomain) break;
			const size_t excludedStart = (std::min)(
				excluded.Start, selectedDomain);
			if (cursor < excludedStart)
				ranges.emplace_back(cursor, excludedStart - cursor);
			const size_t excludedEnd = excluded.Count
				> (std::numeric_limits<size_t>::max)() - excluded.Start
				? (std::numeric_limits<size_t>::max)()
				: excluded.Start + excluded.Count;
			cursor = (std::min)(selectedDomain,
				(std::max)(cursor, excludedEnd));
		}
		if (cursor < selectedDomain)
			ranges.emplace_back(cursor, selectedDomain - cursor);
	}
	else
	{
		for (const int raw : selected.DenseValues())
		{
			if (raw < 0 || static_cast<size_t>(raw) >= dataCount) continue;
			const size_t index = static_cast<size_t>(raw);
			if (!ranges.empty()
				&& ranges.back().first + ranges.back().second == index)
				++ranges.back().second;
			else ranges.emplace_back(index, 1);
		}
	}
	if (ranges.empty()) return false;
	const size_t firstRemoved = ranges.front().first;
	DataGridColumn* const restoreColumn = live->_currentCell.Column;

	live->_deletingRows = true;
	auto resetDeleting = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_deletingRows = false;
	});
	if (!live->DeselectAllRowsWithoutCellSync()) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != itemsView) return false;
	if (!live->ApplySelectedCells({})) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != itemsView) return false;

	bool removedAll = true;
	for (size_t range = ranges.size(); range > 0; --range)
	{
		const auto [index, count] = ranges[range - 1];
		editable = live->EditableItems();
		if (!editable || !editable->CanRemove()
			|| !editable->RemoveRange(index, count))
		{
			removedAll = false;
			break;
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_itemsView != itemsView) return false;
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != itemsView) return false;
	const size_t remainingData = itemsView->Count();
	if (live->_columns.empty()) return removedAll;
	size_t columnIndex = 0;
	const auto found = std::find_if(
		live->_columns.begin(), live->_columns.end(),
		[restoreColumn](const auto& column)
		{ return column.get() == restoreColumn; });
	if (found != live->_columns.end())
		columnIndex = static_cast<size_t>(
			std::distance(live->_columns.begin(), found));
	if (remainingData != 0)
	{
		const size_t targetRow = (std::min)(firstRemoved, remainingData - 1);
		if (!live->SetCurrentCell(targetRow, columnIndex)) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		if (live->_selectionUnit != DataGridSelectionUnit::Cell)
			(void)live->SelectIndex(static_cast<int>(targetRow));
	}
	else if (live->_displayItemsView
		&& live->_displayItemsView->PlaceholderVisible())
	{
		(void)live->SetCurrentCell(0, columnIndex);
	}
	return removedAll && ownerLifetime.Get() != nullptr;
}

bool DataGrid::CancelCellEdit()
{
	ControlWeakReference ownerLifetime(this);
	if (_endingCellEdit || _endingRowEdit) return false;
	auto* cell = ResolveCurrentCellContainer();
	if (!cell || !cell->GetIsEditing()) return false;
	_endingCellEdit = true;
	auto resetEnding = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_endingCellEdit = false;
	});
	const DataGridCellInfo editingIdentity = _currentCell;
	ControlWeakReference cellLifetime(cell);
	ControlWeakReference editorLifetime(cell->GetEditingElement());
	DataGridCellEditEndingEventArgs ending;
	ending.Column = cell->GetColumn();
	ending.Row = cell->GetRowOwner();
	ending.Cell = cell;
	ending.EditingElement = cell->GetEditingElement();
	ending.EditAction = DataGridEditAction::Cancel;
	ControlWeakReference rowLifetime(ending.Row);
	cui::framework::EventAccess::RaiseWhile(
		CellEditEnding,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && cellLifetime.Get() && rowLifetime.Get()
				&& (!ending.EditingElement || editorLifetime.Get())
				&& std::any_of(owner->_columns.begin(),
					owner->_columns.end(), [&](const auto& candidate)
					{ return candidate.get() == ending.Column; });
		},
		this, ending);
	if (ending.Cancel) return false;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (!live || !cell || live->ResolveCurrentCellContainer() != cell
		|| !(live->_currentCell == editingIdentity)
		|| !cell->GetIsEditing()) return false;
	std::wstring error;
	const bool replaced = cell->ReplaceContent(false, &error);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (!replaced)
	{
		live->SetLastTemplateError(std::move(error));
		return false;
	}
	cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
	if (auto* window = live->GetPresentationWindow())
	{
		auto* focusTarget = cell && live->ResolveCurrentCellContainer() == cell
			? static_cast<Control*>(cell) : static_cast<Control*>(live);
		window->SetKeyboardFocus(focusTarget, true);
	}
	return ownerLifetime.Get() != nullptr;
}

CollectionSortDescription DataGrid::MakeSortDescription(
	const DataGridColumn& column,
	CollectionSortDirection direction) const
{
	CollectionSortDescription result;
	result.Direction = direction;
	result.IgnoreCase = true;
#if CUI_ENABLE_DYNAMIC_XAML
	result.PropertyName = column.GetSortMemberPath();
	if (result.PropertyName.empty())
	{
		if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(&column))
			if (bound->GetBindingSourceKind()
				== DataGridBindingSourceKind::RowDataContext)
				result.PropertyName = bound->GetBindingPath();
	}
#endif
	result.CompiledPath = column.GetCompiledSortMemberPath();
	if (result.CompiledPath.Empty())
	{
		if (const auto* bound = dynamic_cast<const DataGridBoundColumn*>(&column))
			if (bound->GetBindingSourceKind()
				== DataGridBindingSourceKind::RowDataContext)
				result.CompiledPath = bound->GetCompiledBindingPath();
	}
	return result;
}

bool DataGrid::PerformSort(DataGridColumn& column, bool multiColumn)
{
	const ControlWeakReference ownerLifetime(this);
	auto* const requestedColumn = &column;
	if (!_canUserSortColumns || !column.GetCanUserSort()
		|| column.GetDataGridOwner() != this) return false;
	const auto items = _itemsView;
	if (!items) return false;
	// IBindingListGroupView describes an already-shaped external view but does
	// not promise a mutable sort surface. CollectionViewSource is that surface in
	// CUI, matching WPF's ICollectionView.CanSort distinction. Never apply the
	// owned outer sort globally to an opaque grouped view: doing so would detach
	// rows from the source's published group ranges.
	if (_source
		&& dynamic_cast<IBindingListGroupView*>(_source.Get())
		&& !dynamic_cast<CollectionViewSource*>(_source.Get())) return false;
	const auto direction = column._sortDirection
		== CollectionSortDirection::Ascending
		? CollectionSortDirection::Descending
		: CollectionSortDirection::Ascending;
	if (_editingRowItem)
	{
		if (!CommitEdit(DataGridEditingUnit::Row, true)) return false;
	}
	else if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing()
		&& !CommitEdit(DataGridEditingUnit::Cell, true)) return false;
	else if (IsAddingNewItem())
	{
		auto* editable = EditableItems();
		if (!editable || !editable->CommitNew()) return false;
		RefreshNewItemPlaceholder();
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != items) return false;
	auto columnIt = std::find_if(live->_columns.begin(), live->_columns.end(),
		[requestedColumn](const auto& candidate)
		{ return candidate.get() == requestedColumn; });
	if (columnIt == live->_columns.end()) return false;
	auto* liveColumn = columnIt->get();
	DataGridSortingEventArgs args;
	args.Column = liveColumn;
	args.Direction = direction;
	args.MultiColumn = multiColumn;
	cui::framework::EventAccess::RaiseWhile(
		live->Sorting,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			return owner && std::any_of(owner->_columns.begin(),
				owner->_columns.end(), [requestedColumn](const auto& candidate)
				{ return candidate.get() == requestedColumn; });
		},
		live, args);
	if (args.Handled) return true;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_itemsView != items) return false;
	columnIt = std::find_if(live->_columns.begin(), live->_columns.end(),
		[requestedColumn](const auto& candidate)
		{ return candidate.get() == requestedColumn; });
	if (columnIt == live->_columns.end()) return false;
	liveColumn = columnIt->get();
	auto* itemsView = live->_itemsView.get();
	if (!itemsView) return false;
	auto description = live->MakeSortDescription(*liveColumn, args.Direction);
	if (!HasSortPath(description)) return false;
	multiColumn = args.MultiColumn;
	auto descriptions = multiColumn
		? itemsView->SortDescriptions()
		: std::vector<CollectionSortDescription>{};
	auto found = std::find_if(descriptions.begin(), descriptions.end(),
		[&](const auto& candidate)
		{ return SameSortPath(candidate, description); });
	if (found == descriptions.end()) descriptions.push_back(description);
	else *found = description;
	const auto previousDescriptions = itemsView->SortDescriptions();
	std::vector<std::pair<DataGridColumn*,
		std::optional<CollectionSortDirection>>> previousDirections;
	previousDirections.reserve(live->_columns.size());
	for (const auto& candidate : live->_columns)
		previousDirections.emplace_back(
			candidate.get(), candidate->_sortDirection);
	if (!multiColumn)
	{
		for (auto& candidate : live->_columns)
		{
			if (candidate.get() != liveColumn)
				candidate->_sortDirection.reset();
		}
	}
	liveColumn->_sortDirection = args.Direction;
	try
	{
		itemsView->SetSortDescriptions(std::move(descriptions));
	}
	catch (...)
	{
		const auto error = std::current_exception();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live && live->_itemsView == items)
		{
			try
			{
				itemsView->SetSortDescriptions(previousDescriptions);
			}
			catch (...) {}
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live)
		{
			for (const auto& [candidate, oldDirection]
				: previousDirections)
			{
				auto foundColumn = std::find_if(
					live->_columns.begin(), live->_columns.end(),
					[candidate](const auto& current)
					{ return current.get() == candidate; });
				if (foundColumn != live->_columns.end())
				{
					(*foundColumn)->_sortDirection = oldDirection;
				}
			}
		}
		std::rethrow_exception(error);
	}
	// Do not replace the headers while DataGridColumnHeader::OnClick is still
	// executing on one of them.  SortDirection is column state (the first theme
	// has no sort glyph); a future glyph can update that header in place.
	return true;
}

bool DataGrid::ResizeColumn(size_t columnIndex, double pixelWidth)
{
	return ResizeColumnCore(columnIndex, pixelWidth, false);
}

bool DataGrid::ResizeRow(size_t rowIndex, double pixelHeight)
{
	return SetRowHeightOverride(rowIndex, pixelHeight);
}

bool DataGrid::AutoSizeRow(size_t rowIndex)
{
	return ClearRowHeightOverride(rowIndex);
}

bool DataGrid::ResizeColumnCore(
	size_t columnIndex, double pixelWidth,
	bool preserveRealizedColumnRange)
{
	if (!_canUserResizeColumns || columnIndex >= _columns.size()
		|| !std::isfinite(pixelWidth)) return false;
	auto& column = *_columns[columnIndex];
	if (column.GetVisibility() != Visibility::Visible
		|| !column.GetCanUserResize()) return false;
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	const double width = (std::clamp)(
		pixelWidth, column.GetLayoutMinWidth(), column.GetLayoutMaxWidth());
	const DataGridLength resized(width);
	if (column._hasWidth && column._width == resized) return true;
	column._width = resized;
	column._hasWidth = true;
	column._runtimeWidth = {};
	// Native header dragging preserves the existing overscanned horizontal
	// container strip. Recomputing it on every WM_MOUSEMOVE can recreate
	// template content and makes the gripper lag behind the pointer; release
	// performs the one final range reconciliation. Programmatic resize keeps the
	// immediate behavior.
	RefreshColumnWidths(preserveRealizedColumnRange);
	return true;
}

bool DataGrid::AutoSizeColumn(size_t columnIndex)
{
	if (!_canUserResizeColumns || columnIndex >= _columns.size()) return false;
	auto& column = *_columns[columnIndex];
	if (column.GetVisibility() != Visibility::Visible
		|| !column.GetCanUserResize()) return false;
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	column._width = DataGridLength::Auto();
	column._hasWidth = true;
	column._runtimeWidth = {};
	RefreshColumnWidths();
	return true;
}

GridLength DataGrid::ResolveColumnGridLength(size_t columnIndex) const
{
	if (columnIndex >= _columns.size()) return GridLength::Auto();
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	if (_resolvedColumnWidths.size() != _columns.size()
		|| !_resolvedColumnWidths[columnIndex])
		RebuildResolvedColumnWidths();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	return live && columnIndex < live->_resolvedColumnWidths.size()
		&& live->_resolvedColumnWidths[columnIndex]
		? *live->_resolvedColumnWidths[columnIndex]
		: GridLength::Pixels(0.0f);
}

void DataGrid::InvalidateColumnWidthCache() noexcept
{
	_resolvedColumnWidths.clear();
	_columnWidthPrefix.clear();
	if (++_columnWidthProjectionRevision == 0)
		_columnWidthProjectionRevision = 1;
}

bool DataGrid::BeginColumnResizeTransaction(
	size_t columnIndex, bool compensateLeft)
{
	if (!_canUserResizeColumns || columnIndex >= _columns.size()
		|| !_columns[columnIndex]->GetCanUserResize()
		|| _columns[columnIndex]->GetVisibility() != Visibility::Visible
		|| !_columnResizeSnapshot.empty()) return false;

	std::vector<ColumnResizeSnapshot> snapshot;
	snapshot.reserve(_columns.size());
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		auto& column = *_columns[index];
		const auto resolved = ResolveColumnGridLength(index);
		const double display = resolved.IsPixel()
			? static_cast<double>(resolved.Value)
			: EstimateColumnWidth(index, true, true);
		if (!std::isfinite(display)) return false;
		const auto effectiveWidth = column.GetWidth();
		double desired = display;
		if (std::isfinite(column._runtimeWidth.Desired))
			desired = column._runtimeWidth.Desired;
		else if (effectiveWidth.UnitType == DataGridLengthUnitType::Pixel)
			desired = effectiveWidth.Value;
		else if (effectiveWidth.UnitType == DataGridLengthUnitType::Star
			&& effectiveWidth.Value > 0.000001)
		{
			// The global water-fill solver does not expose WPF's DesiredValue.
			// Recover its common unconstrained water level from any Star which is
			// not currently clamped, then apply it to every Star below.
			desired = (std::numeric_limits<double>::quiet_NaN)();
		}
		snapshot.push_back(ColumnResizeSnapshot{
			&column, effectiveWidth, column._width, column._hasWidth,
			column._runtimeWidth,
			desired, display });
	}
	double starWaterLevel = (std::numeric_limits<double>::quiet_NaN)();
	for (const auto& state : snapshot)
	{
		if (state.Width.UnitType != DataGridLengthUnitType::Star
			|| state.Width.Value <= 0.000001 || !state.Column
			|| state.Column->GetVisibility() != Visibility::Visible) continue;
		const auto* column = state.Column;
		if (!column
			|| state.Display <= column->GetLayoutMinWidth() + 0.000001
			|| state.Display >= column->GetLayoutMaxWidth() - 0.000001) continue;
		starWaterLevel = state.Display / state.Width.Value;
		break;
	}
	if (!std::isfinite(starWaterLevel) || starWaterLevel < 0.0)
	{
		double starDisplay = 0.0;
		double starWeight = 0.0;
		for (const auto& state : snapshot)
		{
			if (state.Width.UnitType != DataGridLengthUnitType::Star
				|| state.Width.Value <= 0.000001 || !state.Column
				|| state.Column->GetVisibility() != Visibility::Visible) continue;
			starDisplay += state.Display;
			starWeight += state.Width.Value;
		}
		starWaterLevel = starWeight > 0.000001
			? starDisplay / starWeight : 0.0;
	}
	for (auto& state : snapshot)
	{
		if (state.Width.UnitType == DataGridLengthUnitType::Star
			&& state.Width.Value > 0.000001
			&& !std::isfinite(state.Desired))
			state.Desired = starWaterLevel * state.Width.Value;
	}
	auto workingSnapshot = snapshot;
	_columnResizeSnapshot = std::move(snapshot);
	_columnResizeWorkingSnapshot = std::move(workingSnapshot);
	_columnResizeDisplayScratch.resize(_columns.size());
	_columnResizeDesiredScratch.resize(_columns.size());
	_columnResizeFactorScratch.resize(_columns.size());
	_columnResizePreviousDisplayScratch.resize(_columns.size());
	_columnResizeTransactionIndex = columnIndex;
	_columnResizeCompensatesLeft = compensateLeft;
	_columnResizeLastRawWidth = _columnResizeWorkingSnapshot[columnIndex].Display;
	_columnResizeInputBias = 0.0;
	_columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
	_columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
	_columnWidthDirtyVisualSpan =
		(std::numeric_limits<double>::quiet_NaN)();
	_columnWidthMeasureDirty.clear();
	return true;
}

bool DataGrid::RebaseColumnResizeTransaction()
{
	if (_columnResizeSnapshot.empty()
		|| _columnResizeTransactionIndex >= _columns.size()) return false;
	std::vector<ColumnResizeSnapshot> snapshot;
	snapshot.reserve(_columns.size());
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		auto& column = *_columns[index];
		const auto resolved = ResolveColumnGridLength(index);
		const double display = resolved.IsPixel()
			? static_cast<double>(resolved.Value)
			: EstimateColumnWidth(index, true, true);
		if (!std::isfinite(display)) return false;
		const auto effectiveWidth = column.GetWidth();
		const double desired = std::isfinite(column._runtimeWidth.Desired)
			? column._runtimeWidth.Desired
			: (effectiveWidth.UnitType == DataGridLengthUnitType::Pixel
				? effectiveWidth.Value : display);
		snapshot.push_back(ColumnResizeSnapshot{
			&column, effectiveWidth, column._width, column._hasWidth,
			column._runtimeWidth,
			desired, display });
	}
	_columnResizeWorkingSnapshot = std::move(snapshot);
	return true;
}

bool DataGrid::ResizeColumnInTransaction(
	size_t columnIndex, double pixelWidth)
{
	if (!std::isfinite(pixelWidth)
		|| columnIndex != _columnResizeTransactionIndex
		|| _columnResizeSnapshot.size() != _columns.size()
		|| _columnResizeWorkingSnapshot.size() != _columns.size()) return false;
	if (!_canUserResizeColumns || !_columns[columnIndex]->GetCanUserResize())
	{
		EndColumnResizeTransaction(true);
		return false;
	}
	if (GetPresentationWindow() && !_applyingPendingColumnResizeInput)
	{
		_columnResizePendingRawWidth = pixelWidth;
		InvalidateVisual();
		return true;
	}
	_columnResizePendingRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	_columnResizeLastRawWidth = pixelWidth;
	pixelWidth += _columnResizeInputBias;
	bool previousProjectionDiffers = false;
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		if (_columns[index].get() != _columnResizeSnapshot[index].Column
			|| _columns[index].get()
				!= _columnResizeWorkingSnapshot[index].Column)
		{
			_columnResizeSnapshot.clear();
			_columnResizeWorkingSnapshot.clear();
			_columnResizeTransactionIndex = DataGridCellInfo::InvalidIndex;
			_columnResizeCompensatesLeft = false;
			return false;
		}
		const auto& runtime = _columns[index]->_runtimeWidth;
		const auto& startRuntime =
			_columnResizeWorkingSnapshot[index].RuntimeWidth;
		previousProjectionDiffers = previousProjectionDiffers
			|| _columns[index]->GetWidth()
				!= _columnResizeWorkingSnapshot[index].Width
			|| _columns[index]->_hasWidth
				!= _columnResizeWorkingSnapshot[index].HasLocalWidth
			|| runtime.HasDisplayOverride
				!= startRuntime.HasDisplayOverride
			|| (runtime.HasDisplayOverride
				&& (std::abs(runtime.Display - startRuntime.Display) > 0.000001
					|| std::abs(runtime.Desired - startRuntime.Desired)
						> 0.000001));
	}
	auto& previousDisplay = _columnResizePreviousDisplayScratch;
	previousDisplay.resize(_columns.size());
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		const double current = _columns[index]->_runtimeWidth.Display;
		previousDisplay[index] = std::isfinite(current)
			? current : _columnResizeWorkingSnapshot[index].Display;
	}

	// Pointer moves carry an absolute displacement from drag start. Restore the
	// complete semantic snapshot first so min/max saturation is reversible and
	// repeated input never accumulates rounding error.
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		_columns[index]->_width =
			_columnResizeWorkingSnapshot[index].LocalWidth;
		_columns[index]->_hasWidth =
			_columnResizeWorkingSnapshot[index].HasLocalWidth;
		_columns[index]->_runtimeWidth =
			_columnResizeWorkingSnapshot[index].RuntimeWidth;
	}

	auto& display = _columnResizeDisplayScratch;
	auto& desired = _columnResizeDesiredScratch;
	display.resize(_columnResizeWorkingSnapshot.size());
	desired.resize(_columnResizeWorkingSnapshot.size());
	for (size_t index = 0;
		index < _columnResizeWorkingSnapshot.size(); ++index)
	{
		display[index] = _columnResizeWorkingSnapshot[index].Display;
		desired[index] = _columnResizeWorkingSnapshot[index].Desired;
	}
	const double startTargetWidth = display[columnIndex];
	const auto& target = *_columns[columnIndex];
	const double requestedTargetWidth = (std::clamp)(
		pixelWidth, target.GetLayoutMinWidth(), target.GetLayoutMaxWidth());
	double delta = requestedTargetWidth - startTargetWidth;
	if (std::abs(delta) <= 0.000001 && !previousProjectionDiffers)
		return true;

	{
		const auto resizeStars = [&](
			double amount, bool shrink, size_t begin, size_t end)
		{
			constexpr double epsilon = 0.000001;
			double perStarWidth = (std::numeric_limits<double>::quiet_NaN)();
			for (size_t index = 0; index < _columns.size(); ++index)
			{
				const auto& state = _columnResizeWorkingSnapshot[index];
				if (state.Width.UnitType != DataGridLengthUnitType::Star
					|| _columns[index]->GetVisibility()
						!= Visibility::Visible
					|| state.Width.Value <= 0.0
					|| !std::isfinite(desired[index])) continue;
				const double candidate = desired[index] / state.Width.Value;
				if (std::isfinite(candidate) && candidate > epsilon)
				{
					perStarWidth = candidate;
					break;
				}
			}
			if (!std::isfinite(perStarWidth) || perStarWidth <= epsilon)
				perStarWidth = 1.0;
			auto& workingFactor = _columnResizeFactorScratch;
			workingFactor.assign(_columns.size(), 0.0);
			for (size_t index = begin; index < end; ++index)
			{
				if (_columnResizeWorkingSnapshot[index].Width.UnitType
					== DataGridLengthUnitType::Star)
					workingFactor[index] = (std::max)(
						0.0,
						_columnResizeWorkingSnapshot[index].Width.Value);
			}
			while (amount > epsilon)
			{
				double totalWeight = 0.0;
				double ratio = (std::numeric_limits<double>::infinity)();
				for (size_t index = begin; index < end; ++index)
				{
					const auto& state =
						_columnResizeWorkingSnapshot[index];
					const auto& column = *_columns[index];
					if (state.Width.UnitType
							!= DataGridLengthUnitType::Star
						|| column.GetVisibility() != Visibility::Visible
						|| !column.GetCanUserResize()
						|| workingFactor[index] <= 0.0) continue;
					const double weight = workingFactor[index];
					const double capacity = shrink
						? display[index] - column.GetLayoutMinWidth()
						: column.GetLayoutMaxWidth() - display[index];
					if (capacity <= epsilon) continue;
					totalWeight += weight;
					ratio = (std::min)(
						ratio, capacity / weight);
				}
				if (totalWeight <= epsilon) break;
				ratio = (std::min)(ratio, amount / totalWeight);
				if (!std::isfinite(ratio)) break;
				if (ratio <= epsilon) break;
				double consumed = 0.0;
				for (size_t index = begin; index < end; ++index)
				{
					const auto& state =
						_columnResizeWorkingSnapshot[index];
					const auto& column = *_columns[index];
					if (state.Width.UnitType
							!= DataGridLengthUnitType::Star
						|| !column.GetCanUserResize()
						|| workingFactor[index] <= 0.0) continue;
					const double weight = workingFactor[index];
					const double capacity = shrink
						? display[index] - column.GetLayoutMinWidth()
						: column.GetLayoutMaxWidth() - display[index];
					if (capacity <= epsilon) continue;
					const double change = (std::min)(
						capacity, ratio * weight);
					display[index] += shrink ? -change : change;
					desired[index] = display[index];
					workingFactor[index] = display[index] / perStarWidth;
					consumed += change;
				}
				if (consumed <= epsilon) break;
				amount -= consumed;
			}
			return (std::max)(0.0, amount);
		};
		const auto resizeNonStars = [&](
			double amount, bool shrink, bool towardDesired,
			size_t begin, size_t end, bool reverse)
		{
			const auto resizeOne = [&](size_t index)
			{
				const auto& state = _columnResizeWorkingSnapshot[index];
				auto& column = *_columns[index];
				if (state.Width.UnitType == DataGridLengthUnitType::Star
					|| column.GetVisibility() != Visibility::Visible
					|| !column.GetCanUserResize()) return;
				const double threshold = towardDesired
					? (std::clamp)(desired[index],
						column.GetLayoutMinWidth(), column.GetLayoutMaxWidth())
					: (shrink
						? column.GetLayoutMinWidth()
						: column.GetLayoutMaxWidth());
				const double capacity = shrink
					? display[index] - threshold
					: threshold - display[index];
				const double change = (std::min)(
					amount, (std::max)(0.0, capacity));
				display[index] += shrink ? -change : change;
				amount -= change;
			};
			if (reverse)
			{
				for (size_t index = end;
					index-- > begin && amount > 0.000001;)
					resizeOne(index);
			}
			else
			{
				for (size_t index = begin;
					index < end && amount > 0.000001; ++index)
					resizeOne(index);
			}
			return (std::max)(0.0, amount);
		};
		const size_t compensationBegin = _columnResizeCompensatesLeft
			? 0 : columnIndex + 1;
		const size_t compensationEnd = _columnResizeCompensatesLeft
			? columnIndex : _columns.size();

		if (delta > 0.0)
		{
			double remaining = delta;
			// Only a right-edge gesture can consume unused space on the right
			// without moving the opposite edge away from the pointer.
			if (!_columnResizeCompensatesLeft)
			{
				double totalDisplay = 0.0;
				for (const double width : display) totalDisplay += width;
				const double unused = std::isfinite(_columnViewportWidth)
					? (std::max)(0.0, _columnViewportWidth - totalDisplay)
					: 0.0;
				const double fromUnused = (std::min)(remaining, unused);
				display[columnIndex] += fromUnused;
				remaining -= fromUnused;
			}

			double before = remaining;
			remaining = resizeNonStars(
				remaining, true, true,
				compensationBegin, compensationEnd, true);
			display[columnIndex] += before - remaining;

			const double beforeStars = remaining;
			remaining = resizeStars(
				remaining, true, compensationBegin, compensationEnd);
			display[columnIndex] += beforeStars - remaining;

			before = remaining;
			remaining = resizeNonStars(
				remaining, true, false,
				compensationBegin, compensationEnd, true);
			display[columnIndex] += before - remaining;

			// A right edge can grow the scroll extent after its right-hand donors
			// saturate. A left edge is anchored by the grid origin, so it clamps
			// when its left-hand donors cannot move any farther.
			if (!_columnResizeCompensatesLeft)
				display[columnIndex] += remaining;
		}
		else
		{
			double remaining = -delta;
			const bool reverse = _columnResizeCompensatesLeft;
			double before = remaining;
			remaining = resizeNonStars(
				remaining, false, true,
				compensationBegin, compensationEnd, reverse);
			display[columnIndex] -= before - remaining;

			const double beforeStars = remaining;
			remaining = resizeStars(
				remaining, false, compensationBegin, compensationEnd);
			display[columnIndex] -= beforeStars - remaining;

			before = remaining;
			remaining = resizeNonStars(
				remaining, false, false,
				compensationBegin, compensationEnd, reverse);
			display[columnIndex] -= before - remaining;

			if (!_columnResizeCompensatesLeft)
				display[columnIndex] -= remaining;
		}
	}

	bool changed = false;
	for (size_t index = 0; index < display.size(); ++index)
	{
		if (std::abs(display[index]
			- _columnResizeWorkingSnapshot[index].Display) > 0.000001)
		{
			changed = true;
			break;
		}
	}

	// Record exactly which cell slots change geometry. When a target and donor
	// exchange the same amount, columns after the donor keep both their width and
	// left edge and do not need to participate in the drag-frame Arrange pass.
	double oldLeft = 0.0;
	double newLeft = 0.0;
	size_t dirtyBegin = DataGridCellInfo::InvalidIndex;
	size_t dirtyEnd = DataGridCellInfo::InvalidIndex;
	for (size_t index = 0; index < display.size(); ++index)
	{
		const double oldWidth = previousDisplay[index];
		const double newWidth = display[index];
		if (std::abs(oldLeft - newLeft) > 0.000001
			|| std::abs(oldWidth - newWidth) > 0.000001)
		{
			if (dirtyBegin == DataGridCellInfo::InvalidIndex)
				dirtyBegin = index;
			dirtyEnd = index + 1;
		}
		oldLeft += oldWidth;
		newLeft += newWidth;
	}
	_columnWidthDirtyBegin = dirtyBegin;
	_columnWidthDirtyEnd = dirtyEnd;
	_columnWidthDirtyVisualSpan =
		(std::numeric_limits<double>::quiet_NaN)();
	if (dirtyBegin != DataGridCellInfo::InvalidIndex
		&& dirtyEnd != DataGridCellInfo::InvalidIndex)
	{
		double oldSpan = 0.0;
		double newSpan = 0.0;
		for (size_t index = dirtyBegin; index < dirtyEnd; ++index)
		{
			oldSpan += previousDisplay[index];
			newSpan += display[index];
		}
		_columnWidthDirtyVisualSpan = (std::max)(oldSpan, newSpan);
	}
	_columnWidthMeasureDirty.assign(display.size(), 0);
	for (size_t index = 0; index < display.size(); ++index)
	{
		if (std::abs(display[index]
			- previousDisplay[index]) > 0.000001)
			_columnWidthMeasureDirty[index] = 1;
	}
	if (!changed)
	{
		if (previousProjectionDiffers) RefreshColumnWidths(true);
		return true;
	}

	double perStarWidth = (std::numeric_limits<double>::quiet_NaN)();
	for (size_t index = 0; index < _columns.size(); ++index)
	{
		const auto& state = _columnResizeWorkingSnapshot[index];
		if (state.Width.UnitType != DataGridLengthUnitType::Star
			|| _columns[index]->GetVisibility() != Visibility::Visible
			|| state.Width.Value <= 0.000001) continue;
		const double candidate = state.Desired / state.Width.Value;
		if (std::isfinite(candidate) && candidate > 0.000001)
		{
			perStarWidth = candidate;
			break;
		}
	}
	if (!std::isfinite(perStarWidth) || perStarWidth <= 0.000001)
		perStarWidth = 1.0;

	for (size_t index = 0; index < _columns.size(); ++index)
	{
		auto& column = *_columns[index];
		const auto& start = _columnResizeWorkingSnapshot[index];
		if (column.GetVisibility() != Visibility::Visible)
		{
			column._runtimeWidth = start.RuntimeWidth;
			continue;
		}
		const bool displayChanged = std::abs(
			display[index] - start.Display) > 0.000001;
		if (start.Width.UnitType == DataGridLengthUnitType::Star)
		{
			if (displayChanged) column._width = start.Width;
			if (index == columnIndex && displayChanged)
			{
				desired[index] = display[index];
				const double targetPerStarWidth = start.Width.Value > 0.0
					? start.Desired / start.Width.Value : perStarWidth;
				if (std::isfinite(targetPerStarWidth)
					&& targetPerStarWidth > 0.000001)
					column._width = DataGridLength::Star(
						(std::max)(0.0,
							desired[index] / targetPerStarWidth));
			}
			else if (displayChanged)
				column._width = DataGridLength::Star(
					(std::max)(0.0, desired[index] / perStarWidth));
		}
		else if (index == columnIndex && displayChanged)
		{
			column._width = DataGridLength(display[index]);
			desired[index] = display[index];
		}
		if (displayChanged
			&& (start.Width.UnitType == DataGridLengthUnitType::Star
				|| index == columnIndex))
			column._hasWidth = true;
		column._runtimeWidth.Desired = desired[index];
		column._runtimeWidth.Display = display[index];
		// A user resize is local to its target and the donors selected by the
		// dragged edge. Preserve every current Star display so the viewport solver
		// cannot redistribute the accepted delta into unrelated columns afterward.
		column._runtimeWidth.HasDisplayOverride =
			start.Width.UnitType == DataGridLengthUnitType::Star
			|| displayChanged || start.RuntimeWidth.HasDisplayOverride;
	}
	RefreshColumnWidths(true);
	return true;
}

bool DataGrid::ApplyPendingColumnResizeInput()
{
	if (!std::isfinite(_columnResizePendingRawWidth)) return true;
	if (_columnResizeTransactionIndex == DataGridCellInfo::InvalidIndex
		|| _columnResizeSnapshot.empty()) return false;
	const double width = std::exchange(
		_columnResizePendingRawWidth,
		(std::numeric_limits<double>::quiet_NaN)());
	const bool previous = _applyingPendingColumnResizeInput;
	_applyingPendingColumnResizeInput = true;
	const ControlWeakReference ownerLifetime(this);
	auto restore = MakeScopeExit([ownerLifetime, previous]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_applyingPendingColumnResizeInput = previous;
	});
	const bool applied = ResizeColumnInTransaction(
		_columnResizeTransactionIndex, width);
	return applied && ownerLifetime.Get() != nullptr;
}

void DataGrid::EndColumnResizeTransaction(bool cancel)
{
	if (cancel) _columnResizePendingRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	if (_columnResizeSnapshot.empty())
	{
		_columnResizeWorkingSnapshot.clear();
		_columnResizeTransactionIndex = DataGridCellInfo::InvalidIndex;
		_columnResizeCompensatesLeft = false;
		_columnResizeLastRawWidth =
			(std::numeric_limits<double>::quiet_NaN)();
		_columnResizeInputBias = 0.0;
		_columnResizePendingRawWidth =
			(std::numeric_limits<double>::quiet_NaN)();
		return;
	}
	const bool canRestore = cancel;
	if (canRestore)
	{
		for (const auto& state : _columnResizeSnapshot)
		{
			auto found = std::find_if(
				_columns.begin(), _columns.end(),
				[identity = state.Column](const auto& column)
				{ return column.get() == identity; });
			if (found == _columns.end()) continue;
			(*found)->_width = state.LocalWidth;
			(*found)->_hasWidth = state.HasLocalWidth;
			(*found)->_runtimeWidth = state.RuntimeWidth;
		}
	}
	_columnResizeSnapshot.clear();
	_columnResizeWorkingSnapshot.clear();
	_columnResizeTransactionIndex = DataGridCellInfo::InvalidIndex;
	_columnResizeCompensatesLeft = false;
	_columnResizeLastRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	_columnResizeInputBias = 0.0;
	_columnResizePendingRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	if (canRestore) RefreshColumnWidths(true);
}

void DataGrid::InvalidateColumnContentWidthCache() noexcept
{
	_sampledColumnContentWidths.clear();
	++_columnContentWidthCacheEpoch;
	if (_columnContentWidthCacheEpoch == 0)
		_columnContentWidthCacheEpoch = 1;
	InvalidateColumnWidthCache();
}

void DataGrid::ApplyPendingColumnWidths(bool refreshVirtualMetrics)
{
	if (!_columnWidthRefreshPending) return;
	_columnWidthRefreshPending = false;
	const bool activeResizeFrame = !refreshVirtualMetrics
		&& !_columnResizeSnapshot.empty();
	const bool fixedRowHeight = std::isfinite(_rowHeight);
	const bool fixedHeaderHeight = std::isfinite(_columnHeaderHeight)
		|| !HasColumnHeaders(_headersVisibility);
	bool localLayoutCommitted = activeResizeFrame;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference headersLifetime(
		dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get()));
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		headersLifetime.Get()))
	{
		presenter->UpdateColumnWidths(false);
		auto* current = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* currentPresenter = dynamic_cast<
			DataGridColumnHeadersPresenter*>(headersLifetime.Get());
		if (!current || !currentPresenter) return;
		if (activeResizeFrame)
			localLayoutCommitted = currentPresenter
				->TryCommitResizeLayoutLocally(fixedHeaderHeight)
				&& localLayoutCommitted;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		if (activeResizeFrame && row
			&& !live->IsVirtualizedItemInViewport(row->ItemIndex()))
			continue;
		if (row && row->GetDataGridOwner() == live)
		{
			row->UpdateColumnWidths(false);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!live) return;
			if (row && row->GetDataGridOwner() == live
				&& activeResizeFrame)
			{
				auto* cells = row->_cellsGrid;
				localLayoutCommitted = cells
					&& cells->TryCommitResizeLayoutLocally(fixedRowHeight)
					&& localLayoutCommitted;
			}
		}
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (activeResizeFrame && localLayoutCommitted) return;

	// Auto-height content is allowed to take the fast path while its desired
	// height remains stable. If any row/header changed height, promote the whole
	// coalesced frame to the ordinary ancestor layout transaction so following
	// rows and the ScrollViewer viewport are repositioned correctly.
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		headersLifetime.Get()))
		live->InvalidateMeasurePathFromDescendant(presenter);
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		if (row && row->GetDataGridOwner() == live)
			live->InvalidateMeasurePathFromDescendant(row);
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	// PrepareMeasureCore or PreparePresentation owns the surrounding layout turn.
	// The leaf presenters dirty only their own measure state; these local paths
	// make the immediately following template layout reach them without root
	// requests or a second frame.
	if (refreshVirtualMetrics) live->RefreshVirtualScrollMetrics();
}

void DataGrid::CommitColumnWidthLayoutToAncestors()
{
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference headersLifetime(
		dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get()));
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		headersLifetime.Get()))
	{
		presenter->UpdateColumnWidths(false);
		auto* current = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* currentPresenter = dynamic_cast<
			DataGridColumnHeadersPresenter*>(headersLifetime.Get());
		if (current && currentPresenter)
			current->InvalidateMeasurePathFromDescendant(currentPresenter);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		if (row && row->GetDataGridOwner() == live)
		{
			row->UpdateColumnWidths(false);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!live) return;
			if (row && row->GetDataGridOwner() == live)
				live->InvalidateMeasurePathFromDescendant(row);
		}
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->RefreshVirtualScrollMetrics();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	// Mouse release needs one public ancestor layout request, not one request per
	// realized row. The local paths preserve final Auto-height and hit-test
	// geometry even when the width revision was already applied locally.
	live->RequestLayout();
}

void DataGrid::InvalidatePendingColumnResizeVisual()
{
	// The resize solver records the exact contiguous geometry interval changed
	// since the previously presented frame. Damage only that interval when its
	// first header is realized; a balanced target/donor exchange then avoids
	// repainting the stable columns after the donor.
	const size_t damageBegin = _columnWidthDirtyBegin;
	if (damageBegin < _columns.size()
		&& std::isfinite(_columnWidthDirtyVisualSpan))
	{
		auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get());
		auto* header = presenter
			? presenter->GetHeader(damageBegin) : nullptr;
		if (header && header->GetPresentationWindow() == GetPresentationWindow())
		{
			const auto gridRect = GetAbsoluteRectDip();
			const auto headerRect = header->GetAbsoluteRectDip();
			const float left = (std::clamp)(
				headerRect.Left() - 2.0f,
				gridRect.Left(), gridRect.Right());
			const double maximum = static_cast<double>(
				(std::numeric_limits<float>::max)());
			const float span = static_cast<float>((std::clamp)(
				_columnWidthDirtyVisualSpan + 4.0, 0.0, maximum));
			const float right = (std::clamp)(
				headerRect.Left() + span,
				gridRect.Left(), gridRect.Right());
			if (left < right)
			{
				InvalidateVisualRect(D2D1::RectF(
					left, gridRect.Top(),
					right, gridRect.Bottom()));
				return;
			}
		}
	}
	// Detached/unrealized headers and semantic-only projection changes use the
	// conservative DataGrid damage path.
	InvalidateVisual();
}

void DataGrid::RefreshColumnWidths(bool preserveRealizedColumnRange)
{
	InvalidateColumnWidthCache();
	if (preserveRealizedColumnRange)
	{
		if (_columnWidthRefreshPending) return;
		_columnWidthRefreshPending = true;
		// The presentation hook consumes only the newest drag delta and commits the
		// header/realized-row paths locally. Scheduling damage for this DataGrid keeps
		// the Window measure root and unrelated retained content out of every frame.
		InvalidatePendingColumnResizeVisual();
		return;
	}
	_columnWidthRefreshPending = false;
	if (!preserveRealizedColumnRange) InvalidateRealizedColumnRange();
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		_headersPresenter.Get())) presenter->UpdateColumnWidths(false);
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			row->UpdateColumnWidths(false);
	}
	RefreshVirtualScrollMetrics();
	// Every realized leaf above is already dirty. One owner request is enough to
	// reach all of them; propagating once per row turns a viewport update into
	// O(realized rows * visual depth) ancestor work before layout even begins.
	RequestLayout();
	InvalidateVisual();
}

void DataGrid::RefreshInheritedColumnWidthDefaults(
	bool widthChanged, bool minimumChanged, bool maximumChanged)
{
	bool affectsProjection = false;
	for (const auto& column : _columns)
	{
		if (!column) continue;
		const bool inheritsWidth = widthChanged && !column->_hasWidth;
		const bool inheritsMinimum = minimumChanged && !column->_hasMinWidth;
		const bool inheritsMaximum = maximumChanged && !column->_hasMaxWidth;
		if (!inheritsWidth && !inheritsMinimum && !inheritsMaximum) continue;
		affectsProjection = true;
		if (inheritsWidth) column->_runtimeWidth = {};
	}
	if (!affectsProjection) return;
	if (!_columnResizeSnapshot.empty()) EndColumnResizeTransaction(true);
	// This intentionally retires only the resolved definition/prefix and the
	// realized projection. Auto/SizeToCells samples remain valid and reusable.
	RefreshColumnWidths();
}

void DataGrid::ProjectColumnWidthsForViewportLayout()
{
	// Viewport width is discovered from Header.Measure, DataGrid.Arrange, or the
	// ScrollViewer layout notification. In all three cases an ancestor layout
	// transaction is already walking this template. Project the new shared column
	// revision locally and let that transaction consume it synchronously instead
	// of scheduling a redundant root measure for every realized row.
	InvalidateColumnWidthCache();
	_columnWidthRefreshPending = false;
	InvalidateRealizedColumnRange();
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		_headersPresenter.Get())) presenter->UpdateColumnWidths(false);
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			row->UpdateColumnWidths(false);
	}
	// Dense rows publish their new measured extent during the current arrange.
	// Sparse rows have no visuals for offscreen columns, so their logical extent
	// still has to be refreshed explicitly.
	if (_enableColumnVirtualization) RefreshVirtualScrollMetrics();
	InvalidateVisual();
}

bool DataGrid::TryCommitViewportColumnLayoutLocally()
{
	// A late ScrollViewer viewport correction arrives after it has arranged its
	// content. Fixed heights commit directly; Auto presenters first remeasure the
	// width-dirty cells and commit locally while their desired height is unchanged.
	// If wrapping changes any height, the caller promotes the correction to one
	// ordinary owner request so the following vertical items are repositioned.
	const bool fixedRowHeight = std::isfinite(_rowHeight);
	const bool fixedHeaderHeight = std::isfinite(_columnHeaderHeight)
		|| !HasColumnHeaders(_headersVisibility);
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference headersLifetime(
		dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get()));
	std::vector<ControlWeakReference> rows;
	rows.reserve(GetRealizedItems().size());
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		headersLifetime.Get()))
	{
		if (!presenter->TryCommitResizeLayoutLocally(fixedHeaderHeight))
			return false;
		if (!ownerLifetime.Get()) return false;
	}
	for (const auto& rowLifetime : rows)
	{
		auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!owner) return false;
		if (!row || row->GetDataGridOwner() != owner) continue;
		auto* cells = row->_cellsGrid;
		if (!cells
			|| !cells->TryCommitResizeLayoutLocally(fixedRowHeight)) return false;
	}
	return ownerLifetime.Get() != nullptr;
}

bool DataGrid::EndRowEdit(
	DataGridEditAction action,
	bool exitEditingMode)
{
	if (!_editingRowItem)
	{
		if (action == DataGridEditAction::Commit && !exitEditingMode
			&& _currentCell.IsValid())
			return BeginRowEditTransaction(_currentCell);
		return true;
	}
	if (_endingRowEdit || _startingRowEdit || _endingCellEdit) return false;
	const ControlWeakReference ownerLifetime(this);
	const size_t revision = _rowEditRevision;
	const auto item = _editingRowItem;
	const bool sourceTransaction = _editingRowHasSourceTransaction;
	const auto editingItemsView = _itemsView;
	size_t initialRowIndex = DataGridCellInfo::InvalidIndex;
	const bool addingNew = TryResolveEditingRowIndex(initialRowIndex)
		&& IsPendingNewItem(item, initialRowIndex);
	DataGridColumn* const restoreColumn = _currentCell.Column;
	if (auto* cell = ResolveCurrentCellContainer();
		cell && cell->GetIsEditing())
	{
		auto* row = cell->GetRowOwner();
		if (!row || !IsRowEditIdentity(row->GetItem(), row->ItemIndex()))
			return false;
		const bool cellEnded = action == DataGridEditAction::Commit
			? CommitCellEdit(true) : CancelCellEdit();
		if (!cellEnded) return false;
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_rowEditRevision != revision
			|| live->_editingRowItem.Shared() != item.Shared()) return false;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	size_t rowIndex = DataGridCellInfo::InvalidIndex;
	if (!live->TryResolveEditingRowIndex(rowIndex))
	{
		live->AbandonRowEditTransaction(true);
		return false;
	}
	live->_editingRowIndex = rowIndex;
	(void)live->BringItemIntoView(rowIndex);
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_rowEditRevision != revision
		|| live->_editingRowItem.Shared() != item.Shared()) return false;
	auto* row = live->ResolveRow(rowIndex);
	if (!row || !live->IsRowEditIdentity(row->GetItem(), rowIndex))
		return false;
	const ControlWeakReference rowLifetime(row);
	live->_endingRowEdit = true;
	auto resetEnding = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_endingRowEdit = false;
	});
	DataGridRowEditEndingEventArgs ending;
	ending.Row = row;
	ending.EditAction = action;
	cui::framework::EventAccess::RaiseWhile(
		live->RowEditEnding,
		[&]()
		{
			auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			auto* currentRow = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			return owner && currentRow
				&& owner->_rowEditRevision == revision
				&& owner->_editingRowItem.Shared() == item.Shared()
				&& currentRow->GetDataGridOwner() == owner
				&& owner->IsRowEditIdentity(
					currentRow->GetItem(), currentRow->ItemIndex());
		},
		live, ending);
	if (ending.Cancel) return false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !rowLifetime.Get() || live->_rowEditRevision != revision
		|| live->_editingRowItem.Shared() != item.Shared()) return false;
	if (action == DataGridEditAction::Commit)
	{
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!row || row->GetDataGridOwner() != live
			|| !row->RefreshValidationState()) return false;
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!row || !live || row->GetDataGridOwner() != live
			|| live->_rowEditRevision != revision
			|| live->_editingRowItem.Shared() != item.Shared()) return false;
		if (row->GetHasValidationError()) return false;
	}
	if (sourceTransaction)
	{
		auto* editable = dynamic_cast<IEditableBindingSource*>(item.Get());
		if (!editable) return false;
		live->_callingRowEditSource = true;
		auto resetSourceCall = MakeScopeExit([ownerLifetime]
		{
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				owner->_callingRowEditSource = false;
		});
		const bool ended = action == DataGridEditAction::Commit
			? editable->EndEdit() : editable->CancelEdit();
		if (!ended) return false;
	}
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (live->_rowEditRevision != revision
		|| live->_editingRowItem.Shared() != item.Shared()) return false;
	if (addingNew)
	{
		if (live->_itemsView != editingItemsView
			|| !live->IsPendingNewItem(item, rowIndex)) return false;
		auto* editable = live->EditableItems();
		if (!editable || live->_callingEditableList) return false;
		live->_callingEditableList = true;
		auto resetListCall = MakeScopeExit([ownerLifetime]
		{
			if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
				owner->_callingEditableList = false;
		});
		const bool ended = action == DataGridEditAction::Commit
			? editable->CommitNew() : editable->CancelNew();
		if (!ended) return false;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_itemsView != editingItemsView
			|| live->_rowEditRevision != revision
			|| live->_editingRowItem.Shared() != item.Shared()) return false;
	}
	live->ClearRowEditTransaction();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return false;
	if (addingNew)
	{
		live->RefreshNewItemPlaceholder();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
		if (action == DataGridEditAction::Cancel
			&& live->_displayItemsView
			&& live->_displayItemsView->PlaceholderVisible()
			&& !live->_columns.empty())
		{
			size_t columnIndex = 0;
			const auto found = std::find_if(
				live->_columns.begin(), live->_columns.end(),
				[restoreColumn](const auto& column)
				{ return column.get() == restoreColumn; });
			if (found != live->_columns.end())
				columnIndex = static_cast<size_t>(
					std::distance(live->_columns.begin(), found));
			const size_t placeholderRow =
				live->_displayItemsView->Count() - 1;
			(void)live->SetCurrentCell(placeholderRow, columnIndex);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (!live) return false;
		}
	}
	if (action == DataGridEditAction::Cancel)
	{
		live->InvalidateColumnContentWidthCache();
		live->RefreshColumnWidths();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return false;
	}
	if (action == DataGridEditAction::Commit && !exitEditingMode)
	{
		DataGridCellInfo identity;
		if (!live->_currentCell.IsValid()
			|| !live->TryCreateCellInfo(
				live->_currentCell.RowIndex,
				live->_currentCell.ColumnIndex, identity)) return false;
		// WPF re-enters row edit mode only after the ending command has
		// completed. Release the reentrancy gate for that deliberate restart;
		// the scope guard still restores it on every earlier return/exception.
		live->_endingRowEdit = false;
		return live->BeginRowEditTransaction(identity);
	}
	return true;
}

bool DataGrid::EnsureColumnWidthPrefix() const
{
	const size_t count = _columns.size();
	if (_columnWidthPrefix.size() == count + 1) return true;
	const ControlWeakReference ownerLifetime(const_cast<DataGrid*>(this));
	std::vector<std::pair<DataGridColumn*, uint32_t>> columns;
	columns.reserve(count);
	for (const auto& column : _columns)
		columns.emplace_back(column.get(), column->_accessibilityIdentity);
	std::vector<double> prefix;
	prefix.reserve(count + 1);
	prefix.push_back(0.0);
	for (size_t index = 0; index < count; ++index)
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_columns.size() != count
			|| live->_columns[index].get() != columns[index].first
			|| live->_columns[index]->_accessibilityIdentity
				!= columns[index].second) return false;
		const auto length = live->ResolveColumnGridLength(index);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_columns.size() != count
			|| live->_columns[index].get() != columns[index].first
			|| live->_columns[index]->_accessibilityIdentity
				!= columns[index].second) return false;
		double width = length.IsPixel() ? length.Value : 0.0;
		if (!std::isfinite(width) || width < 0.0) width = 0.0;
		const double previous = prefix.back();
		prefix.push_back(previous >
			(std::numeric_limits<double>::max)() - width
			? (std::numeric_limits<double>::max)() : previous + width);
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_columns.size() != count) return false;
	for (size_t index = 0; index < count; ++index)
		if (live->_columns[index].get() != columns[index].first
			|| live->_columns[index]->_accessibilityIdentity
				!= columns[index].second) return false;
	live->_columnWidthPrefix = std::move(prefix);
	return true;
}

bool DataGrid::TryResolveColumnBounds(
	size_t columnIndex, double& left, double& right) const
{
	left = 0.0;
	right = 0.0;
	if (columnIndex >= _columns.size() || !EnsureColumnWidthPrefix()
		|| _columnWidthPrefix.size() != _columns.size() + 1) return false;
	left = _columnWidthPrefix[columnIndex];
	right = _columnWidthPrefix[columnIndex + 1];
	return true;
}

std::pair<size_t, size_t> DataGrid::ResolveRealizedColumnRange() const
{
	const size_t count = _columns.size();
	const size_t frozenEnd = (std::min)(count, static_cast<size_t>(
		(std::max)(0, _frozenColumnCount)));
	if (_visibleColumns.empty()) return { frozenEnd, frozenEnd };
	if (!_enableColumnVirtualization || count == 0)
		return { 0, count };
	if (frozenEnd >= count) return { count, count };
	if (!EnsureColumnWidthPrefix()
		|| _columnWidthPrefix.size() != count + 1)
		return { frozenEnd, count };

	const double viewport = std::isfinite(_columnViewportWidth)
		&& _columnViewportWidth > 0.0 ? _columnViewportWidth : 480.0;
	const double offset = std::isfinite(_horizontalScrollOffset)
		? (std::max)(0.0, _horizontalScrollOffset) : 0.0;
	const double frozenWidth = _columnWidthPrefix[frozenEnd];
	if (viewport <= frozenWidth + 0.000001)
		return { frozenEnd, frozenEnd };
	const double visibleStart = offset >
		(std::numeric_limits<double>::max)() - frozenWidth
		? (std::numeric_limits<double>::max)() : offset + frozenWidth;
	const double visibleEnd = offset >
		(std::numeric_limits<double>::max)() - viewport
		? (std::numeric_limits<double>::max)() : offset + viewport;
	const auto firstBoundary = std::upper_bound(
		_columnWidthPrefix.begin() + frozenEnd + 1,
		_columnWidthPrefix.end(), visibleStart);
	size_t first = firstBoundary == _columnWidthPrefix.end()
		? count - 1
		: static_cast<size_t>(std::distance(
			_columnWidthPrefix.begin(), firstBoundary) - 1);
	first = (std::max)(frozenEnd, first);
	size_t last = static_cast<size_t>(std::distance(
		_columnWidthPrefix.begin(), std::lower_bound(
			_columnWidthPrefix.begin() + frozenEnd,
			_columnWidthPrefix.end(), visibleEnd)));
	last = (std::min)(count, (std::max)(first + 1, last));

	// Keep two cached columns in total so small wheel/touchpad deltas do not
	// rebuild every realized row for each ScrollChanged notification. At an
	// extent edge put both on the reachable side; in the interior keep one on
	// either side. "Two per side" would silently double the wide-grid control
	// budget.
	constexpr size_t overscan = 2;
	if (first == frozenEnd)
		last = (std::min)(count, last + overscan);
	else if (last == count)
		first = (std::max)(frozenEnd,
			first > overscan ? first - overscan : frozenEnd);
	else
	{
		if (first > frozenEnd) --first;
		last = (std::min)(count, last + 1);
	}
	return { first, last };
}

void DataGrid::InvalidateRealizedColumnRange() noexcept
{
	_realizedFrozenColumnEnd = DataGridCellInfo::InvalidIndex;
	_realizedColumnBegin = DataGridCellInfo::InvalidIndex;
	_realizedColumnEnd = DataGridCellInfo::InvalidIndex;
}

void DataGrid::RefreshRealizedColumns()
{
	if (!_enableColumnVirtualization || _refreshingRealizedColumns
		|| !_columnResizeSnapshot.empty()) return;
	const ControlWeakReference ownerLifetime(this);
	_refreshingRealizedColumns = true;
	auto reset = MakeScopeExit([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			owner->_refreshingRealizedColumns = false;
	});
	const size_t previousBegin = _realizedColumnBegin;
	const size_t previousEnd = _realizedColumnEnd;
	const size_t previousFrozenEnd = _realizedFrozenColumnEnd;
	const size_t frozenEnd = (std::min)(_columns.size(), static_cast<size_t>(
		(std::max)(0, _frozenColumnCount)));
	auto range = ResolveRealizedColumnRange();
	bool sampledEnteringColumns = false;
	const auto needsContentSample = [this, frozenEnd](
		const std::pair<size_t, size_t>& candidate)
	{
		if (candidate.first > candidate.second
			|| candidate.second > _columns.size()) return false;
		for (size_t index = 0; index < _columns.size(); ++index)
		{
			if (index >= frozenEnd
				&& (index < candidate.first || index >= candidate.second))
				continue;
			if (_columns[index]->GetVisibility() != Visibility::Visible)
				continue;
			const auto unit = _columns[index]->GetWidth().UnitType;
			if (unit == DataGridLengthUnitType::Pixel
				|| unit == DataGridLengthUnitType::SizeToHeader
				|| unit == DataGridLengthUnitType::Star) continue;
			if (_sampledColumnContentWidths.size() != _columns.size()
				|| !_sampledColumnContentWidths[index]) return true;
		}
		return false;
	};
	// Unseen Auto/SizeToCells columns have only a cheap header/minimum estimate
	// in the prefix.  Sample just the entering viewport strip, then let the
	// prefix settle before changing containers.  Cached measurements make this
	// monotonic in normal use; the small bound is a reentrancy firewall.
	for (size_t pass = 0; pass < 4 && needsContentSample(range); ++pass)
	{
		_realizedFrozenColumnEnd = frozenEnd;
		_realizedColumnBegin = range.first;
		_realizedColumnEnd = range.second;
		InvalidateColumnWidthCache();
		sampledEnteringColumns = true;
		const auto refined = ResolveRealizedColumnRange();
		if (refined == range) break;
		range = refined;
	}
	if (previousFrozenEnd == frozenEnd
		&& previousBegin == range.first && previousEnd == range.second
		&& !sampledEnteringColumns) return;
	_realizedFrozenColumnEnd = frozenEnd;
	_realizedColumnBegin = range.first;
	_realizedColumnEnd = range.second;

	bool incrementallyUpdated = true;
	std::wstring error;
	if (auto* presenter = GetColumnHeadersPresenter())
		incrementallyUpdated = presenter->RefreshRealizedColumns(
			frozenEnd, range.first, range.second, &error);
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::vector<ControlWeakReference> rows;
	if (incrementallyUpdated)
	{
		rows.reserve(live->GetRealizedItems().size());
		for (const auto& [index, visual] : live->GetRealizedItems())
		{
			(void)visual;
			if (auto* row = dynamic_cast<DataGridRow*>(
				live->GetGeneratedItem(index))) rows.emplace_back(row);
		}
	}
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		if (!row || row->GetDataGridOwner() != live
			|| !row->RefreshRealizedColumns(
				frozenEnd, range.first, range.second, &error))
		{
			incrementallyUpdated = false;
			break;
		}
	}

	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (!incrementallyUpdated)
	{
		// Reentrant tree observers can invalidate an incremental transfer.  Keep
		// the established all-or-nothing regeneration path as the correctness
		// fallback; ordinary scrolling stays on the overlap-preserving path.
		live->InvalidateRows();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		live->RefreshHeaderPresenter();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
		return;
	}
	live->RefreshVirtualScrollMetrics();
	live->RequestLayout();
	live->InvalidateVisual();
}

bool DataGrid::BringColumnIntoView(size_t columnIndex)
{
	if (columnIndex >= _columns.size()
		|| _columns[columnIndex]->GetVisibility()
			!= Visibility::Visible) return false;
	const size_t frozenEnd = (std::min)(_columns.size(),
		static_cast<size_t>((std::max)(0, _frozenColumnCount)));
	if (columnIndex < frozenEnd)
	{
		RefreshHorizontalScrollAlignment();
		return true;
	}
	auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get());
	if (!scroll) return !_enableColumnVirtualization;
	double gutter = HasRowHeaders(_headersVisibility)
		? ResolveRowHeaderWidth() : 0.0;
	if (!std::isfinite(gutter)) gutter = 0.0;
	double dataLeft = 0.0;
	double dataRight = 0.0;
	if (!TryResolveColumnBounds(columnIndex, dataLeft, dataRight)) return false;
	double frozenWidth = 0.0;
	if (frozenEnd > 0)
	{
		double ignored = 0.0;
		if (!TryResolveColumnBounds(
			frozenEnd - 1, ignored, frozenWidth)) return false;
	}
	const double left = gutter + dataLeft;
	const double right = gutter + dataRight;
	const double viewport = scroll->ViewportWidth;
	if (viewport <= gutter + frozenWidth + 0.0001) return false;
	if (left < scroll->HorizontalOffset + gutter + frozenWidth)
		scroll->ScrollToHorizontalOffset((std::max)(
			0.0, dataLeft - frozenWidth));
	else if (viewport > 0.0
		&& right > scroll->HorizontalOffset + viewport)
		scroll->ScrollToHorizontalOffset(right - viewport);
	RefreshHorizontalScrollAlignment();
	return true;
}

double DataGrid::ResolveColumnViewportWidth(double fallbackWidth) const noexcept
{
	double viewport = fallbackWidth;
	if (auto* scroll = dynamic_cast<ScrollViewer*>(_scrollViewer.Get()))
	{
		const double published = scroll->GetViewportWidth();
		if (std::isfinite(published) && published > 0.0)
			viewport = published;
	}
	if (!std::isfinite(viewport) || viewport < 0.0)
		viewport = 0.0;
	const double rowHeader = HasRowHeaders(_headersVisibility)
		? ResolveRowHeaderWidth() : 0.0;
	return (std::max)(0.0, viewport - rowHeader);
}

double DataGrid::GetVirtualizedHorizontalExtent() const
{
	// Dense rows already publish their complete measured width. Avoid forcing
	// Auto/SizeToCells sampling merely because the vertical virtual host asks for
	// optional logical metrics.
	if (!_enableColumnVirtualization) return 0.0;
	double result = HasRowHeaders(_headersVisibility)
		? ResolveRowHeaderWidth() : 0.0;
	// A source transaction may invalidate sampled widths more than once while it
	// is still rollback-capable. Do not populate a transient prefix from the
	// candidate projection; the committed row/header pass refreshes this metric.
	if (_settingItemsSource || IsItemsSourceUpdateInProgress())
	{
		if (_columnWidthPrefix.size() == _columns.size() + 1)
			result += _columnWidthPrefix.back();
		return result;
	}
	if (!EnsureColumnWidthPrefix() || _columnWidthPrefix.empty()) return result;
	const double columns = _columnWidthPrefix.back();
	if (result > (std::numeric_limits<double>::max)() - columns)
		return (std::numeric_limits<double>::max)();
	result += columns;
	return result;
}

void DataGrid::RedistributeRuntimeWidthsForViewportChange(
	double oldViewportWidth, double newViewportWidth) noexcept
{
	if (!std::isfinite(oldViewportWidth)
		|| !std::isfinite(newViewportWidth)) return;
	double change = newViewportWidth - oldViewportWidth;
	if (std::abs(change) <= 0.0001) return;
	const auto hasOverride = [](const auto& column)
	{
		return column->_runtimeWidth.HasDisplayOverride
			&& std::isfinite(column->_runtimeWidth.Display)
			&& std::isfinite(column->_runtimeWidth.Desired);
	};
	const bool hasStarDisplayOverrides = std::any_of(
		_columns.begin(), _columns.end(), [&](const auto& column)
		{
			return column->GetWidth().UnitType == DataGridLengthUnitType::Star
				&& column->GetVisibility() == Visibility::Visible
				&& hasOverride(column);
		});
	const auto redistributeStarDisplays =
		[&](double amount, bool grow) noexcept
	{
		constexpr double epsilon = 0.000001;
		while (amount > epsilon)
		{
			double totalWeight = 0.0;
			double limitingRatio =
				(std::numeric_limits<double>::infinity)();
			for (const auto& column : _columns)
			{
				const auto& width = column->GetWidth();
				if (width.UnitType != DataGridLengthUnitType::Star
					|| column->GetVisibility() != Visibility::Visible
					|| !hasOverride(column)
					|| width.Value <= epsilon) continue;
				const double capacity = grow
					? column->GetLayoutMaxWidth() - column->_runtimeWidth.Display
					: column->_runtimeWidth.Display - column->GetLayoutMinWidth();
				if (capacity <= epsilon) continue;
				totalWeight += width.Value;
				limitingRatio = (std::min)(
					limitingRatio, capacity / width.Value);
			}
			if (totalWeight <= epsilon
				|| std::isnan(limitingRatio)) break;
			const double ratio = (std::min)(
				limitingRatio, amount / totalWeight);
			if (ratio <= epsilon) break;
			double consumed = 0.0;
			for (const auto& column : _columns)
			{
				const auto& width = column->GetWidth();
				if (width.UnitType != DataGridLengthUnitType::Star
					|| column->GetVisibility() != Visibility::Visible
					|| !hasOverride(column)
					|| width.Value <= epsilon) continue;
				const double capacity = grow
					? column->GetLayoutMaxWidth() - column->_runtimeWidth.Display
					: column->_runtimeWidth.Display - column->GetLayoutMinWidth();
				if (capacity <= epsilon) continue;
				const double delta = (std::min)(
					capacity, ratio * width.Value);
				column->_runtimeWidth.Display += grow ? delta : -delta;
				column->_runtimeWidth.Desired += grow ? delta : -delta;
				consumed += delta;
			}
			if (consumed <= epsilon) break;
			amount -= consumed;
		}
		return (std::max)(0.0, amount);
	};
	if (change > 0.0)
	{
		while (change > 0.000001)
		{
			size_t participantCount = 0;
			double minimumLag = (std::numeric_limits<double>::infinity)();
			for (const auto& column : _columns)
			{
				if (column->GetVisibility() != Visibility::Visible
					|| !hasOverride(column)
					|| column->GetWidth().UnitType == DataGridLengthUnitType::Star)
					continue;
				const double limit = (std::clamp)(
					column->_runtimeWidth.Desired,
					column->GetLayoutMinWidth(), column->GetLayoutMaxWidth());
				const double lag = limit - column->_runtimeWidth.Display;
				if (lag <= 0.000001) continue;
				++participantCount;
				minimumLag = (std::min)(minimumLag, lag);
			}
			if (participantCount == 0) break;
			const double perColumn = (std::min)(
				minimumLag, change / static_cast<double>(participantCount));
			if (!(perColumn > 0.0) || !std::isfinite(perColumn)) break;
			for (const auto& column : _columns)
			{
				if (column->GetVisibility() != Visibility::Visible
					|| !hasOverride(column)
					|| column->GetWidth().UnitType == DataGridLengthUnitType::Star)
					continue;
				const double limit = (std::clamp)(
					column->_runtimeWidth.Desired,
					column->GetLayoutMinWidth(), column->GetLayoutMaxWidth());
				if (column->_runtimeWidth.Display + 0.000001 >= limit) continue;
				const double amount = (std::min)(
					perColumn, limit - column->_runtimeWidth.Display);
				column->_runtimeWidth.Display += amount;
				change -= amount;
			}
		}
		if (hasStarDisplayOverrides)
			(void)redistributeStarDisplays(change, true);
		return;
	}

	double deficit = -change;
	// Star columns consume the available-space reduction first. Only the part
	// beyond their aggregate room down to MinWidth reaches non-Star displays.
	if (hasStarDisplayOverrides)
		deficit = redistributeStarDisplays(deficit, false);
	else
	{
		double starCapacity = 0.0;
		for (const auto& column : _columns)
		{
			if (column->GetVisibility() != Visibility::Visible
				|| column->GetWidth().UnitType != DataGridLengthUnitType::Star)
				continue;
			const double display = std::isfinite(column->_runtimeWidth.Display)
				? column->_runtimeWidth.Display : column->GetLayoutMinWidth();
			starCapacity += (std::max)(
				0.0, display - column->GetLayoutMinWidth());
		}
		deficit = (std::max)(0.0, deficit - starCapacity);
	}
	while (deficit > 0.000001)
	{
		size_t participantCount = 0;
		double minimumExcess = (std::numeric_limits<double>::infinity)();
		for (const auto& column : _columns)
		{
			const auto& width = column->GetWidth();
			if (column->GetVisibility() != Visibility::Visible
				|| width.UnitType == DataGridLengthUnitType::Star)
				continue;
			const double display = std::isfinite(column->_runtimeWidth.Display)
				? column->_runtimeWidth.Display
				: (std::clamp)(width.Value,
					column->GetLayoutMinWidth(), column->GetLayoutMaxWidth());
			const double excess = display - column->GetLayoutMinWidth();
			if (excess <= 0.000001) continue;
			++participantCount;
			minimumExcess = (std::min)(minimumExcess, excess);
		}
		if (participantCount == 0) break;
		const double perColumn = (std::min)(
			minimumExcess, deficit / static_cast<double>(participantCount));
		if (!(perColumn > 0.0) || !std::isfinite(perColumn)) break;
		for (const auto& column : _columns)
		{
			const auto& width = column->GetWidth();
			if (column->GetVisibility() != Visibility::Visible
				|| width.UnitType == DataGridLengthUnitType::Star)
				continue;
			if (!hasOverride(column))
			{
				const double display = std::isfinite(column->_runtimeWidth.Display)
					? column->_runtimeWidth.Display
					: (std::clamp)(width.Value,
						column->GetLayoutMinWidth(), column->GetLayoutMaxWidth());
				column->_runtimeWidth.Desired =
					width.UnitType == DataGridLengthUnitType::Pixel
					? width.Value : display;
				column->_runtimeWidth.Display = display;
				column->_runtimeWidth.HasDisplayOverride = true;
			}
			if (column->_runtimeWidth.Display
				<= column->GetLayoutMinWidth() + 0.000001) continue;
			const double amount = (std::min)(perColumn,
				column->_runtimeWidth.Display - column->GetLayoutMinWidth());
			column->_runtimeWidth.Display -= amount;
			deficit -= amount;
		}
	}
}

void DataGrid::UpdateColumnViewportWidth(
	double availableWidth, bool refreshRealizedColumns)
{
	if (!std::isfinite(availableWidth) || availableWidth < 0.0)
		availableWidth = 0.0;
	if (std::isfinite(_columnViewportWidth)
		&& std::abs(_columnViewportWidth - availableWidth) <= 0.0001)
		return;
	const double oldViewportWidth = _columnViewportWidth;
	const bool hasStarColumns = std::any_of(
		_columns.begin(), _columns.end(), [](const auto& column)
		{
			return column->GetVisibility() == Visibility::Visible
				&& column->GetWidth().UnitType
				== DataGridLengthUnitType::Star;
		});
	if (hasStarColumns)
	{
		// Preserve the displayed geometry before changing the Star water-fill.
		// Viewport projection used to leave the resize dirty mask empty, whose
		// conservative meaning is "measure every cell".  In the common mixed
		// Pixel/Star DataGrid that needlessly remeasured all fixed-width templates
		// on every WM_SIZE even though only Star constraints changed.
		std::vector<double> previousWidths;
		if (std::isfinite(oldViewportWidth))
		{
			previousWidths.reserve(_columns.size());
			for (size_t index = 0; index < _columns.size(); ++index)
			{
				const auto resolved = ResolveColumnGridLength(index);
				previousWidths.push_back(resolved.IsPixel()
					? static_cast<double>(resolved.Value) : 0.0);
			}
		}
		_columnViewportWidth = availableWidth;
		RedistributeRuntimeWidthsForViewportChange(
			oldViewportWidth, availableWidth);
		ProjectColumnWidthsForViewportLayout();

		std::vector<double> currentWidths;
		currentWidths.reserve(_columns.size());
		for (size_t index = 0; index < _columns.size(); ++index)
		{
			const auto resolved = ResolveColumnGridLength(index);
			currentWidths.push_back(resolved.IsPixel()
				? static_cast<double>(resolved.Value) : 0.0);
		}
		_columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
		_columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
		_columnWidthDirtyVisualSpan =
			(std::numeric_limits<double>::quiet_NaN)();
		_columnWidthMeasureDirty.clear();
		if (previousWidths.size() == currentWidths.size())
		{
			_columnWidthMeasureDirty.assign(currentWidths.size(), 0);
			double oldLeft = 0.0;
			double newLeft = 0.0;
			for (size_t index = 0; index < currentWidths.size(); ++index)
			{
				const double oldWidth = previousWidths[index];
				const double newWidth = currentWidths[index];
				if (std::abs(oldWidth - newWidth) > 0.000001)
					_columnWidthMeasureDirty[index] = 1;
				if (std::abs(oldLeft - newLeft) > 0.000001
					|| std::abs(oldWidth - newWidth) > 0.000001)
				{
					if (_columnWidthDirtyBegin
						== DataGridCellInfo::InvalidIndex)
						_columnWidthDirtyBegin = index;
					_columnWidthDirtyEnd = index + 1;
				}
				oldLeft += oldWidth;
				newLeft += newWidth;
			}
			if (_columnWidthDirtyBegin != DataGridCellInfo::InvalidIndex)
			{
				double oldSpan = 0.0;
				double newSpan = 0.0;
				for (size_t index = _columnWidthDirtyBegin;
					index < _columnWidthDirtyEnd; ++index)
				{
					oldSpan += previousWidths[index];
					newSpan += currentWidths[index];
				}
				_columnWidthDirtyVisualSpan =
					(std::max)(oldSpan, newSpan);
			}
		}
		// A viewport change can occur while the gripper owns capture (for example
		// when a scrollbar appears). Preserve the drag-start snapshot for Cancel,
		// but replay later absolute pointer deltas from the new projection.
		if (!_columnResizeSnapshot.empty())
		{
			if (RebaseColumnResizeTransaction()
				&& std::isfinite(_columnResizeLastRawWidth))
				_columnResizeInputBias =
					_columnResizeWorkingSnapshot[
						_columnResizeTransactionIndex].Display
					- _columnResizeLastRawWidth;
		}
	}
	else _columnViewportWidth = availableWidth;
	if (refreshRealizedColumns && _columnResizeSnapshot.empty())
		RefreshRealizedColumns();
}

std::pair<size_t, size_t>
DataGrid::ResolveDeferredColumnSampleRange() const
{
	const size_t count = _columns.size();
	if (!_enableColumnVirtualization || count == 0) return { 0, count };
	const size_t projectionRevision = _columnWidthProjectionRevision;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	std::vector<std::pair<DataGridColumn*, std::uint32_t>> columns;
	columns.reserve(count);
	for (const auto& column : _columns)
		columns.emplace_back(column.get(), column->_accessibilityIdentity);
	if (_realizedColumnBegin != DataGridCellInfo::InvalidIndex
		&& _realizedColumnEnd != DataGridCellInfo::InvalidIndex
		&& _realizedColumnBegin <= _realizedColumnEnd
		&& _realizedColumnEnd <= count)
		return { _realizedColumnBegin, _realizedColumnEnd };

	const double viewport = std::isfinite(_columnViewportWidth)
		&& _columnViewportWidth > 0.0 ? _columnViewportWidth : 480.0;
	const double offset = std::isfinite(_horizontalScrollOffset)
		? (std::max)(0.0, _horizontalScrollOffset) : 0.0;
	const double visibleEnd = offset >
		(std::numeric_limits<double>::max)() - viewport
		? (std::numeric_limits<double>::max)() : offset + viewport;
	size_t first = count;
	size_t last = count;
	double left = 0.0;
	for (size_t index = 0; index < count; ++index)
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_columns.size() != count
			|| live->_columnWidthProjectionRevision != projectionRevision
			|| live->_columns[index].get() != columns[index].first
			|| live->_columns[index]->_accessibilityIdentity
				!= columns[index].second) return { 0, count };
		const auto& column = *live->_columns[index];
		if (column.GetVisibility() != Visibility::Visible) continue;
		const auto length = column.GetWidth();
		const double minimum = column.GetLayoutMinWidth();
		const double maximum = column.GetLayoutMaxWidth();
		const auto runtimeWidth = column._runtimeWidth;
		double width = column.GetLayoutMinWidth();
		if (runtimeWidth.HasDisplayOverride
			&& std::isfinite(runtimeWidth.Display))
			width = (std::clamp)(runtimeWidth.Display, minimum, maximum);
		else if (length.UnitType == DataGridLengthUnitType::Pixel)
			width = (std::clamp)(length.Value, minimum, maximum);
		else
		{
			if (length.UnitType != DataGridLengthUnitType::SizeToCells)
			{
				const auto header = column.GetHeader();
				const auto headerLength = header.ToString().size();
				live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
				if (!live || live->_columns.size() != count
					|| live->_columnWidthProjectionRevision != projectionRevision
					|| live->_columns[index].get() != columns[index].first
					|| live->_columns[index]->_accessibilityIdentity
						!= columns[index].second) return { 0, count };
				width = (std::max)(width, 16.0 + 7.0
					* static_cast<double>(headerLength));
			}
			const bool widthUsesCellContent =
				length.UnitType == DataGridLengthUnitType::Auto
				|| length.UnitType == DataGridLengthUnitType::SizeToCells;
			if (widthUsesCellContent
				&& live->_sampledColumnContentWidths.size() == count
				&& live->_sampledColumnContentWidths[index])
				width = (std::max)(
					width, *live->_sampledColumnContentWidths[index]);
			width = (std::clamp)(width, minimum, maximum);
		}
		if (!std::isfinite(width) || width < 0.0) width = 0.0;
		const double right = left >
			(std::numeric_limits<double>::max)() - width
			? (std::numeric_limits<double>::max)() : left + width;
		if (first == count && right > offset) first = index;
		if (first != count && left < visibleEnd) last = index + 1;
		if (first != count && left >= visibleEnd) break;
		left = right;
	}
	if (first == count)
	{
		first = count - 1;
		last = count;
	}
	else if (last <= first) last = first + 1;
	constexpr size_t overscan = 2;
	if (first == 0)
		last = (std::min)(count, last + overscan);
	else if (last == count)
		first = first > overscan ? first - overscan : 0;
	else
	{
		--first;
		last = (std::min)(count, last + 1);
	}
	return { first, last };
}

void DataGrid::RebuildResolvedColumnWidths() const
{
	const size_t count = _columns.size();
	_resolvedColumnWidths.assign(count, std::nullopt);
	if (count == 0) return;
	const size_t projectionRevision = _columnWidthProjectionRevision;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	std::vector<std::pair<DataGridColumn*, std::uint32_t>> columns;
	columns.reserve(count);
	for (const auto& column : _columns)
		columns.emplace_back(column.get(), column->_accessibilityIdentity);
	const auto inputsAreCurrent = [&]() noexcept
	{
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live || live->_columns.size() != count
			|| live->_columnWidthProjectionRevision != projectionRevision)
			return false;
		for (size_t index = 0; index < count; ++index)
			if (live->_columns[index].get() != columns[index].first
				|| live->_columns[index]->_accessibilityIdentity
					!= columns[index].second) return false;
		return true;
	};
	const auto sampleRange = ResolveDeferredColumnSampleRange();
	if (!inputsAreCurrent()) return;
	const size_t frozenSampleEnd = (std::min)(count,
		static_cast<size_t>((std::max)(0, _frozenColumnCount)));

	struct StarColumn final
	{
		size_t Index = 0;
		double Weight = 0.0;
		double Minimum = 0.0;
		double Maximum = 0.0;
	};
	std::vector<StarColumn> stars;
	stars.reserve(count);
	double nonStarTotal = 0.0;
	for (size_t index = 0; index < count; ++index)
	{
		auto& column = *_columns[index];
		if (column.GetVisibility() != Visibility::Visible)
		{
			_resolvedColumnWidths[index] = GridLength::Pixels(0.0f);
			continue;
		}
		const auto& width = column.GetWidth();
		if (column._runtimeWidth.HasDisplayOverride
			&& std::isfinite(column._runtimeWidth.Display))
		{
			if (width.UnitType != DataGridLengthUnitType::Pixel)
			{
				if (width.UnitType != DataGridLengthUnitType::Star)
				{
					const bool header = width.UnitType
						!= DataGridLengthUnitType::SizeToCells;
					const bool cells = width.UnitType
						!= DataGridLengthUnitType::SizeToHeader
						&& (!_enableColumnVirtualization
							|| index < frozenSampleEnd
							|| (index >= sampleRange.first
								&& index < sampleRange.second));
					const double desired =
						EstimateColumnWidth(index, header, cells);
					if (!inputsAreCurrent()) return;
					column._runtimeWidth.Desired = desired;
				}
			}
			const double pixels = (std::clamp)(
				column._runtimeWidth.Display,
				column.GetLayoutMinWidth(), column.GetLayoutMaxWidth());
			_resolvedColumnWidths[index] = GridLength::Pixels(
				static_cast<float>(pixels));
			nonStarTotal += pixels;
			column._runtimeWidth.Display = pixels;
			continue;
		}
		if (width.UnitType == DataGridLengthUnitType::Star)
		{
			stars.push_back(StarColumn{
				index,
				(std::max)(0.0, width.Value),
				column.GetLayoutMinWidth(),
				column.GetLayoutMaxWidth() });
			continue;
		}
		double pixels = 0.0;
		if (width.UnitType == DataGridLengthUnitType::Pixel)
			pixels = (std::clamp)(
				width.Value,
				column.GetLayoutMinWidth(), column.GetLayoutMaxWidth());
		else
		{
			const bool header = width.UnitType
				!= DataGridLengthUnitType::SizeToCells;
		const bool cells = width.UnitType
			!= DataGridLengthUnitType::SizeToHeader
			&& (!_enableColumnVirtualization
				|| index < frozenSampleEnd
				|| (index >= sampleRange.first
						&& index < sampleRange.second));
			pixels = EstimateColumnWidth(index, header, cells);
			if (!inputsAreCurrent()) return;
		}
		_resolvedColumnWidths[index] = GridLength::Pixels(
			static_cast<float>(pixels));
		nonStarTotal += pixels;
		column._runtimeWidth.Desired = width.UnitType
			== DataGridLengthUnitType::Pixel ? width.Value : pixels;
		column._runtimeWidth.Display = pixels;
	}

	if (stars.empty()) return;
	// Interactive resize persists private Display values. A later viewport
	// change gives/takes only the delta to/from Star columns; it must not restore
	// a compensating Pixel/Auto column to its declaration and recreate the
	// original "right edge stays fixed while left Stars move" defect.
	double remaining = std::isfinite(_columnViewportWidth)
		? (std::max)(0.0, _columnViewportWidth - nonStarTotal)
		: 0.0;
	std::vector<size_t> unresolved;
	unresolved.reserve(stars.size());
	for (size_t starIndex = 0; starIndex < stars.size(); ++starIndex)
	{
		const auto& star = stars[starIndex];
		if (star.Weight > 0.0)
		{
			unresolved.push_back(starIndex);
			continue;
		}
		_resolvedColumnWidths[star.Index] = GridLength::Pixels(
			static_cast<float>(star.Minimum));
		_columns[star.Index]->_runtimeWidth.Desired = 0.0;
		_columns[star.Index]->_runtimeWidth.Display = star.Minimum;
		remaining = (std::max)(0.0, remaining - star.Minimum);
	}
	if (unresolved.empty()) return;
	double minimumTotal = 0.0;
	double maximumTotal = 0.0;
	double totalFactors = 0.0;
	for (const size_t starIndex : unresolved)
	{
		minimumTotal += stars[starIndex].Minimum;
		maximumTotal += stars[starIndex].Maximum;
		totalFactors += stars[starIndex].Weight;
	}
	remaining = (std::max)(remaining, minimumTotal);
	if (std::isfinite(maximumTotal))
		remaining = (std::min)(remaining, maximumTotal);
	std::vector<size_t> partial;
	partial.reserve(unresolved.size());
	while (!unresolved.empty())
	{
		if (!(totalFactors > 0.0) || !std::isfinite(totalFactors)) break;
		const double starValue = remaining / totalFactors;
		for (size_t index = 0; index < unresolved.size();)
		{
			const auto& star = stars[unresolved[index]];
			const double share = remaining * star.Weight / totalFactors;
			if (star.Minimum <= share + 0.000001)
			{
				++index;
				continue;
			}
			remaining = (std::max)(0.0, remaining - star.Minimum);
			totalFactors -= star.Weight;
			partial.push_back(unresolved[index]);
			unresolved.erase(unresolved.begin() + index);
		}

		bool iterate = false;
		if (totalFactors > 0.0)
		{
			for (size_t index = 0; index < unresolved.size(); ++index)
			{
				const auto& star = stars[unresolved[index]];
				const double share = remaining * star.Weight / totalFactors;
				if (star.Maximum + 0.000001 >= share) continue;
				_resolvedColumnWidths[star.Index] = GridLength::Pixels(
					static_cast<float>(star.Maximum));
				_columns[star.Index]->_runtimeWidth.Desired =
					starValue * star.Weight;
				_columns[star.Index]->_runtimeWidth.Display = star.Maximum;
				remaining -= star.Maximum;
				totalFactors -= star.Weight;
				unresolved.erase(unresolved.begin() + index);
				iterate = true;
				break;
			}
		}
		if (iterate)
		{
			for (const size_t starIndex : partial)
			{
				unresolved.push_back(starIndex);
				remaining += stars[starIndex].Minimum;
				totalFactors += stars[starIndex].Weight;
			}
			partial.clear();
			continue;
		}

		for (const size_t starIndex : partial)
		{
			const auto& star = stars[starIndex];
			_resolvedColumnWidths[star.Index] = GridLength::Pixels(
				static_cast<float>(star.Minimum));
			_columns[star.Index]->_runtimeWidth.Desired =
				starValue * star.Weight;
			_columns[star.Index]->_runtimeWidth.Display = star.Minimum;
		}
		partial.clear();
		for (const size_t starIndex : unresolved)
		{
			const auto& star = stars[starIndex];
			const double display = totalFactors > 0.0
				? remaining * star.Weight / totalFactors : star.Minimum;
			_resolvedColumnWidths[star.Index] = GridLength::Pixels(
				static_cast<float>(display));
			_columns[star.Index]->_runtimeWidth.Desired =
				starValue * star.Weight;
			_columns[star.Index]->_runtimeWidth.Display = display;
		}
		unresolved.clear();
	}
}

double DataGrid::GetColumnDisplayWidth(size_t columnIndex) const
{
	if (columnIndex >= _columns.size())
		return (std::numeric_limits<double>::quiet_NaN)();
	if (_columns[columnIndex]->GetVisibility() != Visibility::Visible)
		return 0.0;
	const auto& runtimeWidth = _columns[columnIndex]->_runtimeWidth;
	if (runtimeWidth.HasDisplayOverride
		&& std::isfinite(runtimeWidth.Display))
		return runtimeWidth.Display;
	const auto resolved = ResolveColumnGridLength(columnIndex);
	if (resolved.IsPixel()) return resolved.Value;
	if (auto* presenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
		_headersPresenter.Get()))
	{
		if (auto* header = presenter->GetHeader(columnIndex))
		{
			const double width = header->GetActualSizeDip().width;
			if (std::isfinite(width) && width > 0.0) return width;
		}
	}
	return EstimateColumnWidth(columnIndex, true, true);
}

double DataGrid::EstimateColumnWidth(
	size_t columnIndex, bool includeHeader, bool includeCells) const
{
	if (columnIndex >= _columns.size()) return 20.0;
	auto* const columnIdentity = _columns[columnIndex].get();
	const auto columnAccessibilityIdentity =
		columnIdentity->_accessibilityIdentity;
	const size_t projectionRevision = _columnWidthProjectionRevision;
	const ControlWeakReference ownerLifetime(
		const_cast<DataGrid*>(this));
	if (columnIdentity->GetVisibility() != Visibility::Visible) return 0.0;
	const double minimum = columnIdentity->GetLayoutMinWidth();
	const double maximum = columnIdentity->GetLayoutMaxWidth();
	const auto widthUnit = columnIdentity->GetWidth().UnitType;
	const auto* boundColumn = dynamic_cast<const DataGridBoundColumn*>(
		columnIdentity);
	const bool boundToRow = boundColumn && boundColumn->GetBindingSourceKind()
		== DataGridBindingSourceKind::RowDataContext;
#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring bindingPath = boundToRow
		? boundColumn->GetBindingPath() : std::wstring{};
#endif
	const CompiledBindingPathView compiledBindingPath = boundToRow
		? boundColumn->GetCompiledBindingPath() : CompiledBindingPathView{};
	double width = minimum;
	if (includeHeader)
	{
		const auto header = columnIdentity->GetHeader();
		width = (std::max)(width, 16.0 + 7.0
			* static_cast<double>(header.ToString().size()));
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || live->_columnWidthProjectionRevision != projectionRevision
		|| columnIndex >= live->_columns.size()
		|| live->_columns[columnIndex].get() != columnIdentity
		|| live->_columns[columnIndex]->_accessibilityIdentity
			!= columnAccessibilityIdentity)
		return (std::clamp)(width, minimum, maximum);
	const bool widthUsesCellContent =
		widthUnit == DataGridLengthUnitType::Auto
		|| widthUnit == DataGridLengthUnitType::SizeToCells;
	if (widthUsesCellContent
		&& live->_sampledColumnContentWidths.size() == live->_columns.size()
		&& live->_sampledColumnContentWidths[columnIndex])
		width = (std::max)(
			width, *live->_sampledColumnContentWidths[columnIndex]);
	const auto items = live->GetItemsView();
	if (includeCells && items)
	{
		if (live->_sampledColumnContentWidths.size() != live->_columns.size())
		{
			live->_sampledColumnContentWidths.assign(
				live->_columns.size(), std::nullopt);
			++live->_columnContentWidthCacheEpoch;
			if (live->_columnContentWidthCacheEpoch == 0)
				live->_columnContentWidthCacheEpoch = 1;
		}
		auto& cached = live->_sampledColumnContentWidths[columnIndex];
		if (cached) width = (std::max)(width, *cached);
		else
		{
			const size_t cacheEpoch = live->_columnContentWidthCacheEpoch;
			double sampledWidth = 0.0;
			const size_t count = boundToRow ? (std::min)(
				items.Get()->Count(), size_t{ 1000 }) : 0;
			for (size_t index = 0; index < count; ++index)
			{
				BindingSourceReference item;
				BindingValue value;
				if (!items.Get()->TryGetItem(index, item) || !item) continue;
				bool read = false;
#if CUI_ENABLE_DYNAMIC_XAML
				if (!bindingPath.empty())
					read = TryGetBindingPathValue(
						*item.Get(), bindingPath, value);
#endif
				if (!read && !compiledBindingPath.Empty())
					read = TryGetBindingPathValue(
						*item.Get(), compiledBindingPath, value);
				if (read) sampledWidth = (std::max)(sampledWidth,
					16.0 + 7.0 * static_cast<double>(
						value.ToString().size()));
			}
			width = (std::max)(width, sampledWidth);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (live == this
				&& live->_columnContentWidthCacheEpoch == cacheEpoch
				&& live->_columnWidthProjectionRevision == projectionRevision
				&& columnIndex < live->_columns.size()
				&& live->_columns[columnIndex].get() == columnIdentity
				&& live->_columns[columnIndex]->_accessibilityIdentity
					== columnAccessibilityIdentity
				&& live->GetItemsView().Shared() == items.Shared()
				&& live->_sampledColumnContentWidths.size()
					== live->_columns.size())
				live->_sampledColumnContentWidths[columnIndex] = sampledWidth;
		}
	}
	return (std::clamp)(width, minimum, maximum);
}

void DataGrid::OnGeneratedItemsRebuilt()
{
	const ControlWeakReference ownerLifetime(this);
	ListBox::OnGeneratedItemsRebuilt();
	if (!ownerLifetime.Get()) return;
	RefreshNewItemContainerStates();
	if (!ownerLifetime.Get()) return;
	InvalidateItemOccurrenceCache();
	if (_currentCell.Item || _currentCell.Column)
	{
		const auto previousCurrent = _currentCell;
		DataGridCellInfo normalized;
		if (TryNormalizeCurrentCellInfo(previousCurrent, normalized))
			_currentCell = std::move(normalized);
		else _currentCell = {};
		if (!RaiseCurrentCellChanged(previousCurrent)) return;
	}
	if (!ReconcileSelectedCells()) return;
	if (!SynchronizeSelectedCellsFromRows()) return;
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live || !live->SynchronizeRealizedRowLifecycle()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live && !live->IsItemsSourceUpdateInProgress())
		(void)live->RefreshRowHeaderActualWidth();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live) live->RequestAccessibilityStructureChanged();
}

void DataGrid::OnGeneratedItemsRealized()
{
	const ControlWeakReference ownerLifetime(this);
	ListBox::OnGeneratedItemsRealized();
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	live->RefreshNewItemContainerStates();
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::vector<ControlWeakReference> rows;
	rows.reserve(live->GetRealizedItems().size());
	for (const auto& [index, realized] : live->GetRealizedItems())
	{
		(void)realized;
		if (auto* row = dynamic_cast<DataGridRow*>(live->GetGeneratedItem(index)))
			rows.emplace_back(row);
	}
	if (!live->SynchronizeRealizedRowLifecycle()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	std::unordered_set<Control*> currentValidationRows;
	currentValidationRows.reserve(rows.size());
	for (const auto& row : rows)
		if (auto* value = row.Get()) currentValidationRows.insert(value);
	// ItemsControl has already moved retired containers into its recycle pool
	// when this callback runs. Retain weak identities from the previous frame so
	// those no-longer-realized rows can release source validation callbacks even
	// though they are no longer addressable through GetGeneratedItem.
	for (const auto& previous : live->_validationTrackedRows)
	{
		if (currentValidationRows.contains(previous.Get())) continue;
		if (auto* retired = dynamic_cast<DataGridRow*>(previous.Get()))
		{
			retired->_itemValidationConnection.Disconnect();
			retired->_validationTrackedItem = {};
			retired->_validationStateDirty = true;
		}
	}
	live->_validationTrackedRows = rows;
	for (const auto& rowLifetime : rows)
	{
		auto* row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (row && row->GetDataGridOwner() == live)
		{
			row->UpdateColumnWidths();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!live || !row || row->GetDataGridOwner() != live) return;
			row->UpdateRowHeader();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!live || !row || row->GetDataGridOwner() != live) return;
			row->UpdateHorizontalScrollOffset(live->_horizontalScrollOffset);
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			if (!live || !row || row->GetDataGridOwner() != live) return;
			row->AttachValidationTracking();
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (!live) return;
	}
	if (!live->IsItemsSourceUpdateInProgress()
		&& !live->RefreshRowHeaderActualWidth()) return;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return;
	if (live->RefreshSelectedCellContainers(false, true))
		live->_selectedCellsVisualRefreshPending = false;
	live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (live && HasRowHeaders(live->_headersVisibility))
		live->RequestAccessibilityStructureChanged();
}

void DataGrid::OnGeneratedItemIndexChanged(
	Control& visual, size_t oldIndex, size_t newIndex)
{
	const ControlWeakReference ownerLifetime(this);
	std::optional<DataGridCellInfo> previousCurrentCell;
	if (auto* row = dynamic_cast<DataGridRow*>(&visual))
	{
		const ControlWeakReference rowLifetime(row);
		row->SetItemIndex(newIndex);
		const auto item = row->GetItem();
		const bool isPlaceholder = IsNewItemPlaceholder(item);
		row->SetCurrentIsNewItem(
			isPlaceholder || IsPendingNewItem(item, newIndex));
		auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		std::vector<ControlWeakReference> cells;
		cells.reserve(row->GetCells().size());
		for (auto* cell : row->GetCells())
			if (cell) cells.emplace_back(cell);
		for (const auto& cellLifetime : cells)
		{
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
			auto* cell = dynamic_cast<DataGridCell*>(cellLifetime.Get());
			if (!live || !row || row->GetDataGridOwner() != live
				|| row->GetItem().Shared() != item.Shared()
				|| row->ItemIndex() != newIndex) return;
			if (!cell || cell->GetRowOwner() != row) continue;
			cell->SetCurrentIsNewItemPlaceholder(isPlaceholder);
		}
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		if (live->_editingRowItem
			&& row->GetItem().Shared() == live->_editingRowItem.Shared()
			&& (live->_editingRowOccurrence == DataGridCellInfo::InvalidIndex
				? live->_editingRowIndex == oldIndex
				: live->IsRowEditIdentity(row->GetItem(), newIndex)))
			live->_editingRowIndex = newIndex;
		row->UpdateRowHeader();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		std::wstring rowStyle;
		if (!live->TryResolveRowStyle(*row, rowStyle)) return;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		cui::framework::StyleAccess::SetResourceKey(*row, rowStyle);
		if (!row->UpdateRowHeaderPresentation()) return;
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		const size_t detailsRevision = row->_detailsPresentationRevision;
		(void)row->CoerceValue(DataGridRow::DetailsVisibilityProperty());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!row) return;
		if (row->_detailsPresentationRevision == detailsRevision)
			(void)row->UpdateDetailsPresentation();
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		row = dynamic_cast<DataGridRow*>(rowLifetime.Get());
		if (!live || !row || row->GetDataGridOwner() != live
			|| row->GetItem().Shared() != item.Shared()
			|| row->ItemIndex() != newIndex) return;
		if (live->_currentCell.IsValid()
			&& live->_currentCell.RowIndex == oldIndex
			&& row->GetItem().Shared() == live->_currentCell.Item.Shared())
		{
			previousCurrentCell = live->_currentCell;
			live->_currentCell.RowIndex = newIndex;
		}
		for (auto& cell : live->_selectedCells._cells)
			if (cell.RowIndex == oldIndex
				&& cell.Item.Shared() == row->GetItem().Shared())
				cell.RowIndex = newIndex;
		bool excludedLocatorChanged = false;
		for (auto& cell : live->_selectedCells._excludedCells)
			if (cell.RowIndex == oldIndex
				&& cell.Item.Shared() == row->GetItem().Shared())
			{
				cell.RowIndex = newIndex;
				excludedLocatorChanged = true;
			}
		if (excludedLocatorChanged)
			live->_selectedCells.InvalidateExcludedOffsets();
		if (live->_selectionAnchor
			&& live->_selectionAnchor->RowIndex == oldIndex
			&& live->_selectionAnchor->Item.Shared() == row->GetItem().Shared())
			live->_selectionAnchor->RowIndex = newIndex;
	}
	// Incremental generator moves precede OnGeneratedItemsRebuilt. Capture the
	// old public locator here so a source transaction can coalesce the remap and
	// publish the computed CurrentCell DP without raising the logical WPF
	// CurrentCellChanged event for the same item/column identity.
	if (previousCurrentCell)
		if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
			(void)live->RaiseCurrentCellChanged(
				std::move(*previousCurrentCell));
}

void DataGrid::OnSelectedIndexChanged(int oldValue, int newValue)
{
	ListBox::OnSelectedIndexChanged(oldValue, newValue);
}

void DataGrid::OnSelectionChanged(SelectionChangedEventArgs& args)
{
	const ControlWeakReference ownerLifetime(this);
	if (++_rowSelectionRevision == 0) _rowSelectionRevision = 1;
	HandleRowSelectionChanged(args);
	if (auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get()))
	{
		live->Selector::OnSelectionChanged(args);
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		if (live)
		{
			live->RefreshRealizedRowDetails();
			live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
			if (live) live->InvalidateCommandState();
		}
	}
}

bool DataGrid::HandleCellKey(Key key, ModifierKeys modifiers)
{
	ControlWeakReference ownerLifetime(this);
	if (key == Key::Delete
		&& !HasModifier(modifiers, ModifierKeys::Control)
		&& !HasModifier(modifiers, ModifierKeys::Alt))
		return DeleteSelectedRows();
	if (key == Key::F2)
	{
		KeyEventArgs args(key, modifiers);
		return BeginEdit(&args);
	}
	if (key == Key::Space)
	{
		auto* cell = ResolveCurrentCellContainer();
		if (cell && !cell->GetIsEditing()
			&& dynamic_cast<DataGridCheckBoxColumn*>(cell->GetColumn()))
		{
			KeyEventArgs args(key, modifiers);
			return BeginEditCore(&args, true);
		}
		return false;
	}
	if (key == Key::Escape)
	{
		if (auto* cell = ResolveCurrentCellContainer();
			cell && cell->GetIsEditing())
			return CancelEdit(DataGridEditingUnit::Cell);
		return _editingRowItem
			? CancelEdit(DataGridEditingUnit::Row) : false;
	}
	if (key == Key::A && HasModifier(modifiers, ModifierKeys::Control)
		&& GetSelectionMode() == SelectionMode::Extended)
	{
		HandleSelectAll();
		return true;
	}
	if (_visibleColumns.empty() || ItemCount() == 0)
		return false;
	const bool navigation = key == Key::Left || key == Key::Right
		|| key == Key::Up || key == Key::Down || key == Key::Home
		|| key == Key::End || key == Key::Prior || key == Key::Next
		|| key == Key::Tab || key == Key::Return;
	if (!navigation) return false;

	const size_t columnCount = _visibleColumns.size();
	const size_t rowCount = ItemCount();
	size_t rowIndex = _currentCell.IsValid() ? _currentCell.RowIndex : 0;
	size_t visibleColumnIndex = _currentCell.IsValid()
		&& _currentCell.Column
		&& _currentCell.Column->_visibleIndex
			!= DataGridColumn::UnsetDisplayIndex
		? _currentCell.Column->_visibleIndex : 0;
	size_t columnIndex = _visibleColumns[visibleColumnIndex]->_displayIndex;
	const size_t previousRowIndex = rowIndex;
	const bool wasEditing = [&]
	{
		auto* cell = ResolveCurrentCellContainer();
		return cell && cell->GetIsEditing();
	}();
	const bool control = HasModifier(modifiers, ModifierKeys::Control);
	const bool shift = HasModifier(modifiers, ModifierKeys::Shift);
	if (key == Key::Tab)
	{
		const size_t total = rowCount * columnCount;
		long long flat = _currentCell.IsValid()
			? static_cast<long long>(
				rowIndex * columnCount + visibleColumnIndex)
			: (shift ? static_cast<long long>(total) : -1);
		flat += shift ? -1 : 1;
		if (flat < 0 || flat >= static_cast<long long>(total))
		{
			if (wasEditing && !CommitEdit()) return true;
			if (!ownerLifetime.Get()) return true;
			if (auto* window = GetPresentationWindow())
				(void)window->MoveFocus(shift
					? FocusNavigationDirection::Previous
					: FocusNavigationDirection::Next);
			return true;
		}
		rowIndex = static_cast<size_t>(flat) / columnCount;
		visibleColumnIndex = static_cast<size_t>(flat) % columnCount;
	}
	else if (key == Key::Left)
		visibleColumnIndex = control ? 0
			: (visibleColumnIndex == 0 ? 0 : visibleColumnIndex - 1);
	else if (key == Key::Right)
		visibleColumnIndex = control ? columnCount - 1
			: (std::min)(visibleColumnIndex + 1, columnCount - 1);
	else if (key == Key::Up)
		rowIndex = control ? 0 : (rowIndex == 0 ? 0 : rowIndex - 1);
	else if (key == Key::Down)
		rowIndex = control ? rowCount - 1
			: (std::min)(rowIndex + 1, rowCount - 1);
	else if (key == Key::Home)
	{
		visibleColumnIndex = 0;
		if (control) rowIndex = 0;
	}
	else if (key == Key::End)
	{
		visibleColumnIndex = columnCount - 1;
		if (control) rowIndex = rowCount - 1;
	}
	else if (key == Key::Prior || key == Key::Next)
	{
		const double rowHeight = (std::max)(
			1.0, static_cast<double>(GetVirtualizedItemHeight()));
		const size_t page = (std::max)(size_t{ 1 }, static_cast<size_t>(
			(std::max)(1.0, static_cast<double>(GetActualSizeDip().height))
			/ rowHeight));
		if (key == Key::Prior)
			rowIndex = rowIndex > page ? rowIndex - page : 0;
		else rowIndex = (std::min)(rowIndex + page, rowCount - 1);
	}
	else if (key == Key::Return)
	{
		if (_editingRowItem)
		{
			if (!CommitEdit(DataGridEditingUnit::Row, true)) return true;
			if (!ownerLifetime.Get()) return true;
		}
		else if (auto* cell = ResolveCurrentCellContainer();
			cell && cell->GetIsEditing())
		{
			if (!CommitEdit(DataGridEditingUnit::Cell, true)) return true;
			if (!ownerLifetime.Get()) return true;
		}
		if (control) return true;
		if (shift)
		{
			if (rowIndex == 0) return true;
			--rowIndex;
		}
		else
		{
			if (rowIndex + 1 >= rowCount) return true;
			++rowIndex;
		}
	}
	columnIndex = _visibleColumns[visibleColumnIndex]->_displayIndex;
	if (_currentCell.IsValid() && rowIndex == _currentCell.RowIndex
		&& columnIndex == _currentCell.ColumnIndex)
	{
		if (key == Key::Left || key == Key::Right
			|| key == Key::Up || key == Key::Down)
		{
			if (auto* window = GetPresentationWindow())
			{
				const auto direction = key == Key::Left
					? FocusNavigationDirection::Left
					: key == Key::Right
						? FocusNavigationDirection::Right
						: key == Key::Up
							? FocusNavigationDirection::Up
							: FocusNavigationDirection::Down;
				(void)window->MoveFocus(direction);
			}
			return true;
		}
		return true;
	}
	if (!SetCurrentCell(rowIndex, columnIndex))
		return ownerLifetime.Get() ? false : true;
	if (!ownerLifetime.Get()) return true;
	(void)BringItemIntoView(rowIndex);
	if (!ownerLifetime.Get()) return true;
	ModifierKeys selectionModifiers = ModifierKeys::None;
	if (key != Key::Tab && key != Key::Return && shift)
		selectionModifiers = control
			? ModifierKeys::Shift | ModifierKeys::Control
			: ModifierKeys::Shift;
	(void)ApplySelectionForCellInput(
		rowIndex, columnIndex, selectionModifiers, true, false);
	if (!ownerLifetime.Get()) return true;
	const bool continueEditing = wasEditing
		&& rowIndex == previousRowIndex
		&& (key == Key::Tab
			|| key == Key::Left || key == Key::Right);
	if (continueEditing)
	{
		(void)BeginEdit();
		return true;
	}
	if (auto* window = GetPresentationWindow())
	{
		auto* target = ResolveCurrentCellContainer();
		window->SetKeyboardFocus(target ? static_cast<Control*>(target) : this, true);
	}
	return true;
}

bool DataGrid::HandlesNavigationKey(Key key) const
{
	if (key == Key::Escape)
	{
		auto* cell = ResolveCurrentCellContainer();
		return (cell && cell->GetIsEditing()) || !!_editingRowItem;
	}
	switch (key)
	{
	case Key::Tab:
	case Key::Left:
	case Key::Right:
	case Key::Up:
	case Key::Down:
	case Key::Home:
	case Key::End:
	case Key::Prior:
	case Key::Next:
	case Key::Return:
		return !_visibleColumns.empty() && ItemCount() != 0;
	default:
		return ListBox::HandlesNavigationKey(key);
	}
}

bool DataGrid::ApplyTextInput(const TextCompositionEventArgs& input)
{
	const ControlWeakReference ownerLifetime(this);
	if (input.Text.empty() || _isReadOnly)
		return ListBox::ApplyTextInput(input);
	if (!_currentCell.IsValid())
	{
		if (_visibleColumns.empty() || ItemCount() == 0
			|| !SetCurrentCell(0, FirstVisibleDisplayIndex()))
			return ownerLifetime.Get() ? false : true;
	}
	auto* live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
	if (!live) return true;
	if (!dynamic_cast<DataGridTextColumn*>(live->_currentCell.Column))
		return live->ListBox::ApplyTextInput(input);
	if (!live->BeginEdit(&input))
	{
		live = dynamic_cast<DataGrid*>(ownerLifetime.Get());
		return live ? live->ListBox::ApplyTextInput(input) : true;
	}
	return true;
}

bool DataGrid::ProcessInput(const InputReport& input)
{
	ControlWeakReference ownerLifetime(this);
	if (input.Kind == InputReportKind::KeyDown)
	{
		const bool handled = HandleCellKey(input.Key, input.Modifiers);
		if (!ownerLifetime.Get() || handled) return true;
	}
	return ListBox::ProcessInput(input);
}
