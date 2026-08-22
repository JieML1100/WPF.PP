#pragma once

#include "Button.h"
#include "CheckBox.h"
#include "CollectionViewSource.h"
#include "ComboBox.h"
#include "ContentControl.h"
#include "Label.h"
#include "Layout/Grid.h"
#include "ListBox.h"
#include "TextBox.h"

#include <limits>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

class DataGrid;
class DataGridCell;
class DataGridColumn;
class DataGridRow;
class DataGridRowHeader;
class DataGridCellsPresenter;
class ContentPresenter;
class ScrollViewer;

/** Navigation request raised by a generated DataGrid hyperlink face. */
struct DataGridHyperlinkNavigateEventArgs final : EventArgs
{
	std::wstring NavigateUri;
	std::wstring TargetName;
	bool Handled = false;
};

/**
 * Native hyperlink face used by DataGridHyperlinkColumn.
 *
 * It intentionally keeps Button's input contract while publishing Hyperlink
 * UIA semantics. Navigation is a request for the application/navigation host;
 * the control never launches an external process by itself.
 */
class DataGridHyperlink final : public Button
{
public:
	DataGridHyperlink();
	static const DependencyProperty& NavigateUriProperty();
	static const DependencyProperty& TargetNameProperty();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif
	const std::wstring& GetNavigateUri() const noexcept { return _navigateUri; }
	void SetNavigateUri(std::wstring value);
	const std::wstring& GetTargetName() const noexcept { return _targetName; }
	void SetTargetName(std::wstring value);

	Event<void(DataGridHyperlink*, DataGridHyperlinkNavigateEventArgs&)>
		NavigateRequested;

protected:
	std::unique_ptr<AutomationPeer> OnCreateAutomationPeer() override;
	bool OnClick() override;

private:
	std::wstring _navigateUri;
	std::wstring _targetName;
};

/** WPF DataGrid selection unit values. */
enum class DataGridSelectionUnit : int
{
	Cell = 0,
	FullRow = 1,
	CellOrRowHeader = 2,
};

/** WPF-compatible bit values consumed independently by row/column chrome. */
enum class DataGridHeadersVisibility : int
{
	None = 0,
	Column = 1,
	Row = 2,
	All = 3,
};

enum class DataGridGridLinesVisibility : int
{
	All = 0,
	Horizontal = 1,
	None = 2,
	Vertical = 3,
};

/** WPF DataGrid clipboard header policy. */
enum class DataGridClipboardCopyMode : int
{
	None = 0,
	ExcludeHeader = 1,
	IncludeHeader = 2,
};

/** WPF DataGrid row-details visibility policy. */
enum class DataGridRowDetailsVisibilityMode : int
{
	Collapsed = 0,
	Visible = 1,
	VisibleWhenSelected = 2,
};

enum class DataGridLengthUnitType : int
{
	Auto = 0,
	Pixel = 1,
	SizeToCells = 2,
	SizeToHeader = 3,
	Star = 4,
};

/** Value-only WPF DataGridLength projection used by declarative columns. */
struct DataGridLength final
{
	double Value = 1.0;
	DataGridLengthUnitType UnitType = DataGridLengthUnitType::Auto;

	constexpr DataGridLength() noexcept = default;
	explicit constexpr DataGridLength(double pixels) noexcept
		: Value(pixels), UnitType(DataGridLengthUnitType::Pixel) {}
	constexpr DataGridLength(
		double value, DataGridLengthUnitType unitType) noexcept
		: Value(value), UnitType(unitType) {}

	[[nodiscard]] static constexpr DataGridLength Auto() noexcept
	{
		return { 1.0, DataGridLengthUnitType::Auto };
	}
	[[nodiscard]] static constexpr DataGridLength SizeToCells() noexcept
	{
		return { 1.0, DataGridLengthUnitType::SizeToCells };
	}
	[[nodiscard]] static constexpr DataGridLength SizeToHeader() noexcept
	{
		return { 1.0, DataGridLengthUnitType::SizeToHeader };
	}
	[[nodiscard]] static constexpr DataGridLength Star(
		double weight = 1.0) noexcept
	{
		return { weight, DataGridLengthUnitType::Star };
	}

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] static bool TryParse(
		std::wstring_view text, DataGridLength& out) noexcept;
	[[nodiscard]] std::wstring ToString() const;
	bool operator==(const DataGridLength&) const noexcept = default;
};

enum class DataGridEditAction : int
{
	Cancel = 0,
	Commit = 1,
};

/** Selects which WPF ComboBox selection projection receives the row Binding. */
enum class DataGridComboBoxSelectionBinding : int
{
	SelectedItem = 0,
	SelectedValue = 1,
};

/**
 * Supported Binding source forms for generated bound-column elements.
 * RowDataContext preserves the ordinary per-item source; the container values
 * are the useful level-one FindAncestor subset and ElementName is a weak
 * reference to one native Control in the owning XAML namescope.
 */
enum class DataGridBindingSourceKind : int
{
	RowDataContext = 0,
	DataGridCell = 1,
	DataGridRow = 2,
	DataGrid = 3,
	ElementName = 4,
};

/**
 * One immutable child description in a DataGrid column MultiBinding plan.
 *
 * The description is shared once per column. Realized cells allocate only the
 * Binding endpoints and observation state required by MultiBinding itself.
 */
struct DataGridMultiBindingSourcePlan final
{
	DataGridBindingSourceKind SourceKind =
		DataGridBindingSourceKind::RowDataContext;
	ControlWeakReference ElementSource;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring SourcePath;
#endif
	CompiledBindingPathView CompiledPath;
	BindingMode Mode = BindingMode::Default;
	DataSourceUpdateMode UpdateMode = DataSourceUpdateMode::Default;
	std::shared_ptr<const IBindingValueConverter> Converter;
	std::optional<BindingValue> FallbackValue;
	std::optional<BindingValue> TargetNullValue;
	std::optional<BindingValue> ConverterParameter;
	std::optional<std::wstring> StringFormat;
};

/** WPF BindingBase plan for a non-nested DataGrid column MultiBinding. */
struct DataGridMultiBindingPlan final
{
	std::vector<DataGridMultiBindingSourcePlan> Sources;
	BindingMode Mode = BindingMode::Default;
	DataSourceUpdateMode UpdateMode = DataSourceUpdateMode::Default;
	std::shared_ptr<const IMultiBindingValueConverter> Converter;
	std::optional<BindingValue> FallbackValue;
	std::optional<BindingValue> TargetNullValue;
	std::optional<BindingValue> ConverterParameter;
	std::optional<std::wstring> StringFormat;
};

/** Selects the WPF-shaped cell or row edit transaction boundary. */
enum class DataGridEditingUnit : int
{
	Cell = 0,
	Row = 1,
};

/** One mutable cell payload in CopyingRowClipboardContent. */
struct DataGridClipboardCellContent final
{
	BindingSourceReference Item;
	DataGridColumn* Column = nullptr;
	BindingValue Content;
};

/** WPF-shaped per-row clipboard customization payload. */
struct DataGridRowClipboardEventArgs final : EventArgs
{
	BindingSourceReference Item;
	size_t StartColumnDisplayIndex = 0;
	size_t EndColumnDisplayIndex = 0;
	bool IsColumnHeadersRow = false;
	size_t RowIndexHint = (std::numeric_limits<size_t>::max)();
	std::vector<DataGridClipboardCellContent> ClipboardRowContent;
};

/** Stable cell identity; it deliberately is not a realized-container handle. */
struct DataGridCellInfo final
{
	static constexpr size_t InvalidIndex =
		(std::numeric_limits<size_t>::max)();

	BindingSourceReference Item;
	DataGridColumn* Column = nullptr;
	size_t RowIndex = InvalidIndex;
	size_t ColumnIndex = InvalidIndex;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return Item && Column && RowIndex != InvalidIndex
			&& ColumnIndex != InvalidIndex;
	}
	bool operator==(const DataGridCellInfo& other) const noexcept
	{
		return Item.Shared() == other.Item.Shared()
			&& Column == other.Column
			&& _itemOccurrence == other._itemOccurrence;
	}

private:
	// Stable CollectionView source-slot identity for repeated references to the
	// same object. RowIndex remains only a remappable public locator.
	size_t _itemOccurrence = InvalidIndex;
	friend class DataGrid;
	friend class DataGridSelectedCellCollection;
};

/**
 * WPF-shaped lazy selected-cell collection.
 *
 * Sparse selections retain exact occurrence identities. Dense selections may
 * retain rectangular row/column regions and create DataGridCellInfo values only
 * when an indexer, iterator, or automation client actually asks for them.
 */
class DataGridSelectedCellCollection final
{
public:
	struct RegionExcludedOffsets final
	{
		struct RowInterval final
		{
			size_t Start = 0;
			size_t Count = 0;
			// Number of interval-covered rows before Start.  This makes
			// ordinal-to-snapshot translation logarithmic in the number of
			// retired runs without expanding a run into one offset per row.
			size_t ExcludedBefore = 0;
		};
		std::vector<RowInterval> RowIntervals;
		std::vector<size_t> Rows;
		std::vector<size_t> Cells;
	};
	class const_iterator final
	{
	public:
		using iterator_category = std::input_iterator_tag;
		using value_type = DataGridCellInfo;
		using difference_type = std::ptrdiff_t;
		using pointer = void;
		using reference = DataGridCellInfo;

		const_iterator() noexcept = default;
		DataGridCellInfo operator*() const;
		const_iterator& operator++() noexcept
		{
			++_index;
			return *this;
		}
		const_iterator operator++(int) noexcept
		{
			auto previous = *this;
			++*this;
			return previous;
		}
		bool operator==(const const_iterator& other) const noexcept
		{
			return _owner == other._owner && _index == other._index;
		}

	private:
		const_iterator(
			const DataGridSelectedCellCollection* owner,
			size_t index,
			std::shared_ptr<const std::vector<RegionExcludedOffsets>>
				excludedOffsets = {}) noexcept
			: _owner(owner), _index(index),
			  _excludedOffsets(std::move(excludedOffsets)) {}

		const DataGridSelectedCellCollection* _owner = nullptr;
		size_t _index = 0;
		std::shared_ptr<const std::vector<RegionExcludedOffsets>>
			_excludedOffsets;
		friend class DataGridSelectedCellCollection;
	};

	DataGridSelectedCellCollection() noexcept = default;
	[[nodiscard]] size_t size() const noexcept;
	[[nodiscard]] bool empty() const noexcept { return size() == 0; }
	[[nodiscard]] DataGridCellInfo at(size_t index) const;
	[[nodiscard]] DataGridCellInfo operator[](size_t index) const
	{
		return at(index);
	}
	[[nodiscard]] DataGridCellInfo front() const { return at(0); }
	[[nodiscard]] const_iterator begin() const;
	[[nodiscard]] const_iterator end() const noexcept
	{
		return const_iterator(this, size());
	}
	[[nodiscard]] bool Contains(
		const DataGridCellInfo& candidate) const noexcept;
	[[nodiscard]] bool SameLogicalCells(
		const DataGridSelectedCellCollection& other) const noexcept;
	/** White-box scale probe; not part of the XAML/application contract. */
	[[nodiscard]] size_t RetiredSnapshotRowIntervalCountForTesting()
		const noexcept
	{
		size_t result = 0;
		for (const auto& region : _regions)
			result += region.ExcludedSnapshotRowIntervals.size();
		return result;
	}
	[[nodiscard]] size_t RetiredSnapshotRowIdentityCountForTesting()
		const noexcept
	{
		size_t result = 0;
		for (const auto& region : _regions)
			result += region.ExcludedRows.size();
		return result;
	}

private:
	struct CellRegion final
	{
		size_t Top = 0;
		size_t Height = 0;
		std::vector<DataGridColumn*> Columns;
		std::vector<size_t> ColumnIndices;
		struct ExcludedRow final
		{
			BindingSourceReference Item;
			size_t Occurrence = DataGridCellInfo::InvalidIndex;
			size_t SnapshotRow = DataGridCellInfo::InvalidIndex;
		};
		struct ExcludedSnapshotRowInterval final
		{
			size_t Start = 0;
			size_t Count = 0;
		};
		// Empty means every row in [Top, Top + Height). A non-empty vector is a
		// sparse, sorted row projection into that immutable snapshot range.
		std::vector<size_t> IncludedRowOffsets;
		// Sorted, disjoint absolute ranges in the immutable region snapshot.
		// Mutable Remove/Replace retires a continuous old range as one entry
		// instead of retaining one BindingSourceReference per removed row.
		std::vector<ExcludedSnapshotRowInterval>
			ExcludedSnapshotRowIntervals;
		std::vector<ExcludedRow> ExcludedRows;
		bool UseSnapshotLocators = false;
	};

	ControlWeakReference _ownerLifetime;
	bool _ownerBound = false;
	BindingListReference _ownerSourceIdentity;
	BindingListReference _source;
	std::vector<DataGridCellInfo> _cells;
	std::vector<CellRegion> _regions;
	std::vector<DataGridCellInfo> _excludedCells;
	mutable std::shared_ptr<const std::vector<RegionExcludedOffsets>>
		_excludedOffsetsCache;

	explicit DataGridSelectedCellCollection(DataGrid* owner) noexcept;
	void SetSparse(
		DataGrid* owner,
		BindingListReference source,
		std::vector<DataGridCellInfo> cells) noexcept;
	void SetFullRegion(
		DataGrid* owner,
		BindingListReference source,
		size_t rowCount,
		std::vector<DataGridColumn*> columns) noexcept;
	[[nodiscard]] bool IsRegionBacked() const noexcept
	{
		return !_regions.empty();
	}
	[[nodiscard]] bool TryCreateRegionCell(
		const CellRegion& region,
		size_t rowOffset,
		size_t columnOffset,
		DataGridCellInfo& result) const;
	[[nodiscard]] RegionExcludedOffsets ExcludedOffsetsForRegion(
		const CellRegion& region) const;
	[[nodiscard]] std::shared_ptr<
		const std::vector<RegionExcludedOffsets>> ExcludedOffsets() const;
	void InvalidateExcludedOffsets() const noexcept
	{
		_excludedOffsetsCache.reset();
	}
	[[nodiscard]] DataGridCellInfo AtWithExcludedOffsets(
		size_t index,
		const std::vector<RegionExcludedOffsets>& excludedOffsets) const;
	[[nodiscard]] DataGridSelectedCellCollection Difference(
		const DataGridSelectedCellCollection& other) const;
	void PruneColumns(const std::vector<std::unique_ptr<DataGridColumn>>& columns);
	bool Exclude(const DataGridCellInfo& cell);
	bool Include(const DataGridCellInfo& cell);

	friend class DataGrid;
};

struct DataGridSortingEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	CollectionSortDirection Direction = CollectionSortDirection::Ascending;
	bool MultiColumn = false;
	bool Handled = false;
};

/** A committed cold-path change to the visual column projection. */
struct DataGridColumnDisplayIndexChangedEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	size_t OldDisplayIndex = DataGridCellInfo::InvalidIndex;
	size_t NewDisplayIndex = DataGridCellInfo::InvalidIndex;
};

/** WPF-shaped cold-path payload raised before a column-header drag begins. */
struct DataGridColumnReorderingEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	/**
	 * Unparented visuals whose ownership transfers to the header presenter when
	 * the operation proceeds. A handler may replace or clear either visual.
	 */
	std::unique_ptr<Control> DragIndicator;
	std::unique_ptr<Control> DropLocationIndicator;
	bool Cancel = false;
};

struct DataGridColumnHeaderDragStartedEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	double HorizontalOffset = 0.0;
	double VerticalOffset = 0.0;
};

struct DataGridColumnHeaderDragDeltaEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	double HorizontalChange = 0.0;
	double VerticalChange = 0.0;
};

struct DataGridColumnHeaderDragCompletedEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	double HorizontalChange = 0.0;
	double VerticalChange = 0.0;
	bool Canceled = false;
};

struct DataGridColumnEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
};

struct DataGridBeginningEditEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	DataGridRow* Row = nullptr;
	DataGridCell* Cell = nullptr;
	/** Non-owning input which initiated the synchronous edit transaction. */
	const RoutedEventArgs* EditingEventArgs = nullptr;
	bool Cancel = false;
};

struct DataGridPreparingCellForEditEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	DataGridRow* Row = nullptr;
	DataGridCell* Cell = nullptr;
	Control* EditingElement = nullptr;
	/** Same non-owning activation payload exposed by BeginningEdit. */
	const RoutedEventArgs* EditingEventArgs = nullptr;
};

struct DataGridCellEditEndingEventArgs final : EventArgs
{
	DataGridColumn* Column = nullptr;
	DataGridRow* Row = nullptr;
	DataGridCell* Cell = nullptr;
	Control* EditingElement = nullptr;
	DataGridEditAction EditAction = DataGridEditAction::Commit;
	bool Cancel = false;
};

struct DataGridRowEditEndingEventArgs final : EventArgs
{
	DataGridRow* Row = nullptr;
	DataGridEditAction EditAction = DataGridEditAction::Commit;
	bool Cancel = false;
};

/** WPF-shaped row-container lifecycle payload. */
struct DataGridRowEventArgs final : EventArgs
{
	DataGridRow* Row = nullptr;
};

/** WPF-shaped row-details lifecycle payload. */
struct DataGridRowDetailsEventArgs final : EventArgs
{
	DataGridRow* Row = nullptr;
	Control* DetailsElement = nullptr;
};

/** WPF AddingNewItem payload; handlers may provide a source-owned record. */
struct DataGridAddingNewItemEventArgs final : EventArgs
{
	BindingSourceReference NewItem;
};

/** WPF InitializingNewItem payload raised after the record enters the source. */
struct DataGridInitializingNewItemEventArgs final : EventArgs
{
	BindingSourceReference NewItem;
};

struct DataGridCurrentCellChangedEventArgs final : EventArgs
{
	DataGridCellInfo OldCell;
	DataGridCellInfo NewCell;
};

/** Net cell-selection changes published after container state is synchronized. */
struct DataGridSelectedCellsChangedEventArgs final : EventArgs
{
	DataGridSelectedCellCollection AddedCells;
	DataGridSelectedCellCollection RemovedCells;
};

/**
 * Non-visual WPF column definition. The row item is supplied only when a cell
 * visual is built; a column never inherits or evaluates DataContext itself.
 */
class DataGridColumn : public DependencyObject
{
public:
	static constexpr size_t UnsetDisplayIndex =
		DataGridCellInfo::InvalidIndex;

	virtual ~DataGridColumn() = default;

	const BindingValue& GetHeader() const noexcept { return _header; }
	void SetHeader(BindingValue value);
	/** WPF column-local overrides; an empty key inherits the DataGrid value. */
	const std::wstring& GetHeaderStyle() const noexcept { return _headerStyle; }
	void SetHeaderStyle(std::wstring value);
	ItemTemplateReference GetHeaderTemplate() const noexcept
	{
		return _headerTemplate;
	}
	void SetHeaderTemplate(ItemTemplateReference value);
	const std::wstring& GetCellStyle() const noexcept { return _cellStyle; }
	void SetCellStyle(std::wstring value);
	/** Effective WPF width: a local declaration wins over DataGrid.ColumnWidth. */
	const DataGridLength& GetWidth() const noexcept;
	void SetWidth(DataGridLength value);
	void ClearWidth();
	bool HasLocalWidth() const noexcept { return _hasWidth; }
	double GetMinWidth() const noexcept;
	void SetMinWidth(double value);
	void ClearMinWidth();
	bool HasLocalMinWidth() const noexcept { return _hasMinWidth; }
	double GetMaxWidth() const noexcept;
	void SetMaxWidth(double value);
	void ClearMaxWidth();
	bool HasLocalMaxWidth() const noexcept { return _hasMaxWidth; }
	/** Coercion bounds used by shared header/cell layout. */
	double GetLayoutMinWidth() const noexcept;
	double GetLayoutMaxWidth() const noexcept;
	bool GetIsReadOnly() const noexcept { return _isReadOnly; }
	void SetIsReadOnly(bool value);
	bool GetCanUserSort() const noexcept { return _canUserSort; }
	void SetCanUserSort(bool value);
	bool GetCanUserResize() const noexcept { return _canUserResize; }
	void SetCanUserResize(bool value);
	bool GetCanUserReorder() const noexcept { return _canUserReorder; }
	void SetCanUserReorder(bool value);
	Visibility GetVisibility() const noexcept { return _visibility; }
	/**
	 * Matches WPF DataGridColumn visibility semantics: only Visible columns
	 * participate in the realized layout projection. Hidden and Collapsed are
	 * both retained in the logical/display collections without a visual cell.
	 */
	void SetVisibility(Visibility value);
	/** True when this column's display position is inside the frozen prefix. */
	bool GetIsFrozen() const noexcept;
	size_t GetDisplayIndex() const noexcept { return _displayIndex; }
	/**
	 * Sets the visual position. An unattached column retains the request until
	 * adoption; an attached column requires an index inside the current schema.
	 */
	void SetDisplayIndex(size_t value);

#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring& GetSortMemberPath() const noexcept
	{
		return _sortMemberPath;
	}
	void SetSortMemberPath(std::wstring value);
#endif
	CompiledBindingPathView GetCompiledSortMemberPath() const noexcept
	{
		return _compiledSortMemberPath;
	}
	void SetCompiledSortMemberPath(CompiledBindingPathView value);
	std::optional<CollectionSortDirection> GetSortDirection() const noexcept
	{
		return _sortDirection;
	}
	/**
	 * Sets the header sort indicator for a custom/server-side Sorting handler.
	 * This mirrors WPF DataGridColumn.SortDirection: setting it does not reorder
	 * ItemsSource; the handler remains responsible for applying the sort.
	 */
	void SetSortDirection(
		std::optional<CollectionSortDirection> value);
	bool GetIsAutoGenerated() const noexcept { return _isAutoGenerated; }
	DataGrid* GetDataGridOwner() const noexcept { return _owner; }

protected:
	virtual std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const = 0;
	virtual std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const = 0;
	void NotifyOwnerChanged();

private:
	friend class DataGrid;
	friend class DataGridCell;
	friend class DataGridColumnHeader;
	friend class DataGridColumnHeadersPresenter;
	friend class DataGridRow;
	struct RuntimeWidthState final
	{
		double Desired = (std::numeric_limits<double>::quiet_NaN)();
		double Display = (std::numeric_limits<double>::quiet_NaN)();
		bool HasDisplayOverride = false;
	};
	BindingValue _header;
	std::wstring _headerStyle;
	ItemTemplateReference _headerTemplate;
	std::wstring _cellStyle;
	DataGridLength _width = DataGridLength::Auto();
	// DataGridLength remains the declaration-facing value. Interactive resize
	// keeps WPF's desired/display values privately so Star columns retain their
	// unit and compensating columns do not have their declarations rewritten.
	RuntimeWidthState _runtimeWidth;
	double _minWidth = 20.0;
	double _maxWidth = (std::numeric_limits<double>::infinity)();
	bool _hasWidth = false;
	bool _hasMinWidth = false;
	bool _hasMaxWidth = false;
	bool _isReadOnly = false;
	bool _canUserSort = true;
	bool _canUserResize = true;
	bool _canUserReorder = true;
	Visibility _visibility = Visibility::Visible;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring _sortMemberPath;
#endif
	CompiledBindingPathView _compiledSortMemberPath;
	std::optional<CollectionSortDirection> _sortDirection;
	DataGrid* _owner = nullptr;
	size_t _displayIndex = UnsetDisplayIndex;
	size_t _visibleIndex = UnsetDisplayIndex;
	uint32_t _accessibilityIdentity = 0;
	bool _isAutoGenerated = false;
	bool _isRuntimeAutoGenerated = false;

};

class DataGridBoundColumn : public DataGridColumn
{
public:
	/**
	 * WPF ElementStyle/EditingElementStyle projection. CUI stores the keyed
	 * Style reference because a column is non-visual and the generated element
	 * resolves the key in its inherited resource scope.
	 */
	const std::wstring& GetElementStyle() const noexcept
	{
		return _elementStyle;
	}
	void SetElementStyle(std::wstring value);
	const std::wstring& GetEditingElementStyle() const noexcept
	{
		return _editingElementStyle;
	}
	void SetEditingElementStyle(std::wstring value);
#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring& GetBindingPath() const noexcept
	{
		return _bindingPath;
	}
	void SetBindingPath(std::wstring value);
#endif
	CompiledBindingPathView GetCompiledBindingPath() const noexcept
	{
		return _compiledBindingPath;
	}
	void SetCompiledBindingPath(CompiledBindingPathView value);
	DataGridBindingSourceKind GetBindingSourceKind() const noexcept
	{
		return _bindingSourceKind;
	}
	void SetBindingSourceKind(DataGridBindingSourceKind value);
	Control* GetBindingElementSource() const noexcept
	{
		return _bindingElementSource.Get();
	}
	void SetBindingElementSource(Control* value);
	const std::shared_ptr<const DataGridMultiBindingPlan>&
		GetMultiBindingPlan() const noexcept { return _multiBindingPlan; }
	void SetMultiBindingPlan(
		std::shared_ptr<const DataGridMultiBindingPlan> value);
	BindingMode GetBindingMode() const noexcept { return _bindingMode; }
	void SetBindingMode(BindingMode value);
	DataSourceUpdateMode GetDataSourceUpdateMode() const noexcept
	{
		return _dataSourceUpdateMode;
	}
	void SetDataSourceUpdateMode(DataSourceUpdateMode value);
	const std::shared_ptr<const IBindingValueConverter>&
		GetBindingConverter() const noexcept { return _converter; }
	void SetBindingConverter(
		std::shared_ptr<const IBindingValueConverter> value);
	const std::optional<BindingValue>& GetFallbackValue() const noexcept
	{
		return _fallbackValue;
	}
	void SetFallbackValue(std::optional<BindingValue> value);
	const std::optional<BindingValue>& GetTargetNullValue() const noexcept
	{
		return _targetNullValue;
	}
	void SetTargetNullValue(std::optional<BindingValue> value);
	const std::optional<BindingValue>& GetConverterParameter() const noexcept
	{
		return _converterParameter;
	}
	void SetConverterParameter(std::optional<BindingValue> value);
	const std::optional<std::wstring>& GetStringFormat() const noexcept
	{
		return _stringFormat;
	}
	void SetStringFormat(std::optional<std::wstring> value);

protected:
	static Binding* ApplyBindingPlan(
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
		bool forceMode = false);
	static MultiBinding* ApplyMultiBindingPlan(
		DataGridCell& cell,
		Control& target,
		const DependencyProperty& targetProperty,
		const BindingSourceReference& item,
		const DataGridMultiBindingPlan& plan,
		BindingMode defaultMode,
		bool forceMode = false);
	Binding* ApplyBinding(
		DataGridCell& cell,
		Control& target,
		const DependencyProperty& targetProperty,
		const BindingSourceReference& item,
		BindingMode defaultMode,
		bool forceMode = false) const;
	void ApplyElementStyle(
		Control& target,
		bool editing,
		std::wstring_view defaultResourceKey) const;

private:
	std::wstring _elementStyle;
	std::wstring _editingElementStyle;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring _bindingPath;
#endif
	CompiledBindingPathView _compiledBindingPath;
	DataGridBindingSourceKind _bindingSourceKind =
		DataGridBindingSourceKind::RowDataContext;
	ControlWeakReference _bindingElementSource;
	std::shared_ptr<const DataGridMultiBindingPlan> _multiBindingPlan;
	BindingMode _bindingMode = BindingMode::Default;
	DataSourceUpdateMode _dataSourceUpdateMode = DataSourceUpdateMode::Default;
	std::shared_ptr<const IBindingValueConverter> _converter;
	std::optional<BindingValue> _fallbackValue;
	std::optional<BindingValue> _targetNullValue;
	std::optional<BindingValue> _converterParameter;
	std::optional<std::wstring> _stringFormat;
};

class DataGridTextColumn final : public DataGridBoundColumn
{
protected:
	std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
	std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
};

class DataGridCheckBoxColumn final : public DataGridBoundColumn
{
public:
	bool GetIsThreeState() const noexcept { return _isThreeState; }
	void SetIsThreeState(bool value);

protected:
	std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
	std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;

private:
	bool _isThreeState = false;
};

/**
 * WPF-style choice column with one shared ItemsSource.
 *
 * Every realized cell shares the list/view identity. The closed display face
 * is inert; an editing element owns dropdown interaction.
 */
class DataGridComboBoxColumn final : public DataGridBoundColumn
{
public:
	BindingListReference GetItemsSource() const noexcept
	{
		return _itemsSource;
	}
	void SetItemsSource(BindingListReference value);
	DataGridComboBoxSelectionBinding GetSelectionBinding() const noexcept
	{
		return _selectionBinding;
	}
	void SetSelectionBinding(DataGridComboBoxSelectionBinding value);
#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring& GetDisplayMemberPath() const noexcept
	{
		return _displayMemberPath;
	}
	void SetDisplayMemberPath(std::wstring value);
	const std::wstring& GetSelectedValuePath() const noexcept
	{
		return _selectedValuePath;
	}
	void SetSelectedValuePath(std::wstring value);
#endif
	CompiledBindingPathView GetCompiledDisplayMemberPath() const noexcept
	{
		return _compiledDisplayMemberPath;
	}
	void SetCompiledDisplayMemberPath(CompiledBindingPathView value);
	CompiledBindingPathView GetCompiledSelectedValuePath() const noexcept
	{
		return _compiledSelectedValuePath;
	}
	void SetCompiledSelectedValuePath(CompiledBindingPathView value);

protected:
	std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
	std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;

private:
	std::unique_ptr<ComboBox> GenerateComboBox(
		DataGridCell& cell,
		const BindingSourceReference& item,
		bool editing) const;
	BindingListReference _itemsSource;
	DataGridComboBoxSelectionBinding _selectionBinding =
		DataGridComboBoxSelectionBinding::SelectedItem;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring _displayMemberPath;
	std::wstring _selectedValuePath;
#endif
	CompiledBindingPathView _compiledDisplayMemberPath;
	CompiledBindingPathView _compiledSelectedValuePath;
};

/** WPF-style URI column with a clickable display face and TextBox editor. */
class DataGridHyperlinkColumn final : public DataGridBoundColumn
{
public:
	const std::wstring& GetTargetName() const noexcept { return _targetName; }
	void SetTargetName(std::wstring value);
#if CUI_ENABLE_DYNAMIC_XAML
	const std::wstring& GetContentBindingPath() const noexcept
	{
		return _contentBindingPath;
	}
	void SetContentBindingPath(std::wstring value);
#endif
	CompiledBindingPathView GetCompiledContentBindingPath() const noexcept
	{
		return _compiledContentBindingPath;
	}
	void SetCompiledContentBindingPath(CompiledBindingPathView value);
	DataGridBindingSourceKind GetContentBindingSourceKind() const noexcept
	{
		return _contentBindingSourceKind;
	}
	void SetContentBindingSourceKind(DataGridBindingSourceKind value);
	Control* GetContentBindingElementSource() const noexcept
	{
		return _contentBindingElementSource.Get();
	}
	void SetContentBindingElementSource(Control* value);
	const std::shared_ptr<const DataGridMultiBindingPlan>&
		GetContentMultiBindingPlan() const noexcept
	{
		return _contentMultiBindingPlan;
	}
	void SetContentMultiBindingPlan(
		std::shared_ptr<const DataGridMultiBindingPlan> value);
	bool HasContentBinding() const noexcept;
	void ClearContentBinding();
	BindingMode GetContentBindingMode() const noexcept
	{
		return _contentBindingMode;
	}
	void SetContentBindingMode(BindingMode value);
	DataSourceUpdateMode GetContentDataSourceUpdateMode() const noexcept
	{
		return _contentDataSourceUpdateMode;
	}
	void SetContentDataSourceUpdateMode(DataSourceUpdateMode value);
	const std::shared_ptr<const IBindingValueConverter>&
		GetContentBindingConverter() const noexcept
	{
		return _contentConverter;
	}
	void SetContentBindingConverter(
		std::shared_ptr<const IBindingValueConverter> value);
	const std::optional<BindingValue>& GetContentFallbackValue() const noexcept
	{
		return _contentFallbackValue;
	}
	void SetContentFallbackValue(std::optional<BindingValue> value);
	const std::optional<BindingValue>& GetContentTargetNullValue() const noexcept
	{
		return _contentTargetNullValue;
	}
	void SetContentTargetNullValue(std::optional<BindingValue> value);
	const std::optional<BindingValue>& GetContentConverterParameter() const noexcept
	{
		return _contentConverterParameter;
	}
	void SetContentConverterParameter(std::optional<BindingValue> value);
	const std::optional<std::wstring>& GetContentStringFormat() const noexcept
	{
		return _contentStringFormat;
	}
	void SetContentStringFormat(std::optional<std::wstring> value);

protected:
	std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
	std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;

private:
	Binding* ApplyContentBinding(
		DataGridCell& cell,
		Control& target,
		const DependencyProperty& targetProperty,
		const BindingSourceReference& item) const;
	std::wstring _targetName;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring _contentBindingPath;
#endif
	CompiledBindingPathView _compiledContentBindingPath;
	DataGridBindingSourceKind _contentBindingSourceKind =
		DataGridBindingSourceKind::RowDataContext;
	ControlWeakReference _contentBindingElementSource;
	std::shared_ptr<const DataGridMultiBindingPlan> _contentMultiBindingPlan;
	BindingMode _contentBindingMode = BindingMode::Default;
	DataSourceUpdateMode _contentDataSourceUpdateMode =
		DataSourceUpdateMode::Default;
	std::shared_ptr<const IBindingValueConverter> _contentConverter;
	std::optional<BindingValue> _contentFallbackValue;
	std::optional<BindingValue> _contentTargetNullValue;
	std::optional<BindingValue> _contentConverterParameter;
	std::optional<std::wstring> _contentStringFormat;
};

class DataGridTemplateColumn final : public DataGridColumn
{
public:
	ItemTemplateReference GetCellTemplate() const noexcept
	{
		return _cellTemplate;
	}
	void SetCellTemplate(ItemTemplateReference value);
	ItemTemplateReference GetCellEditingTemplate() const noexcept
	{
		return _cellEditingTemplate;
	}
	void SetCellEditingTemplate(ItemTemplateReference value);
	ItemTemplateSelectorReference GetCellTemplateSelector() const noexcept
	{
		return _cellTemplateSelector;
	}
	void SetCellTemplateSelector(ItemTemplateSelectorReference value);
	ItemTemplateSelectorReference GetCellEditingTemplateSelector() const noexcept
	{
		return _cellEditingTemplateSelector;
	}
	void SetCellEditingTemplateSelector(ItemTemplateSelectorReference value);

protected:
	std::unique_ptr<Control> GenerateElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;
	std::unique_ptr<Control> GenerateEditingElement(
		DataGridCell& cell,
		const BindingSourceReference& item) const override;

private:
	std::unique_ptr<Control> BuildTemplateContent(
		DataGridCell& cell,
		const BindingSourceReference& item,
		bool editing) const;
	ItemTemplateReference _cellTemplate;
	ItemTemplateReference _cellEditingTemplate;
	ItemTemplateSelectorReference _cellTemplateSelector;
	ItemTemplateSelectorReference _cellEditingTemplateSelector;
};

/**
 * Cold-path runtime schema customization payload.
 *
 * Column owns the candidate definition while the event is being raised.  A
 * handler may replace it with another unattached column or set Cancel.  This
 * explicit ownership is the C++ equivalent of WPF's settable Column property
 * without relying on garbage-collected transfer semantics.
 */
struct DataGridAutoGeneratingColumnEventArgs final : EventArgs
{
	std::wstring PropertyName;
	BindingValueKind PropertyKind = BindingValueKind::Empty;
	std::type_index PropertyType{ typeid(void) };
	std::unique_ptr<DataGridColumn> Column;
	bool Cancel = false;
};

class DataGridCell : public ContentControl
{
public:
	DataGridCell();
	UIClass Type() override { return UIClass::UI_DataGridCell; }
	static const DependencyProperty& IsSelectedProperty();
	/** CUI projection of the synthetic, non-data new-row placeholder. */
	static const DependencyProperty& IsNewItemPlaceholderProperty();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif
	bool GetIsSelected() const noexcept { return _isSelected; }
	bool GetIsNewItemPlaceholder() const noexcept
	{
		return _isNewItemPlaceholder;
	}
	void SetIsSelected(bool value);
	bool GetIsEditing() const noexcept { return _isEditing; }
	bool GetIsReadOnly() const noexcept;
	DataGridColumn* GetColumn() const noexcept { return _column; }
	DataGridRow* GetRowOwner() const noexcept { return _row; }
	Control* GetEditingElement() const noexcept { return _editingElement; }
	bool BeginEdit();
	bool CommitEdit();
	bool CancelEdit();

protected:
	const DependencyPropertyMetadata* ResolveExactDependencyPropertyMetadata(
		const DependencyProperty& property) const override;
	bool DefaultSelectOnLeftButtonDown() const override { return false; }
	bool ProcessInput(const InputReport& input) override;
	void OnRender() override;

private:
	friend class DataGrid;
	friend class DataGridCellsPresenter;
	friend class DataGridRow;
	static const DependencyPropertyKey& IsNewItemPlaceholderPropertyKey();
	bool Initialize(
		DataGridRow& row,
		DataGridColumn& column,
		const BindingSourceReference& item,
		size_t columnIndex,
		std::wstring* outError);
	bool ReplaceContent(bool editing, std::wstring* outError = nullptr);
	void ApplyIsSelectedValue(bool value);
	void SetCurrentIsSelected(
		bool value, bool suppressRoutedEvents = false);
	void SetCurrentIsNewItemPlaceholder(bool value);
	DataGridRow* _row = nullptr;
	DataGridColumn* _column = nullptr;
	BindingSourceReference _item;
	size_t _columnIndex = DataGridCellInfo::InvalidIndex;
	Control* _editingElement = nullptr;
	bool _isSelected = false;
	bool _synchronizingIsSelected = false;
	bool _suppressIsSelectedRoutedEvents = false;
	bool _isEditing = false;
	bool _isNewItemPlaceholder = false;
};

class DataGridRow : public ListBoxItem
{
public:
	DataGridRow();
	UIClass Type() override { return UIClass::UI_DataGridRow; }
	static const DependencyProperty& IsEditingProperty();
	static const DependencyProperty& IsNewItemProperty();
	static const DependencyProperty& HasValidationErrorProperty();
	static const DependencyProperty& ValidationErrorsProperty();
	static const DependencyProperty& ValidationErrorTemplateProperty();
	static const DependencyProperty& DetailsTemplateProperty();
	static const DependencyProperty& DetailsTemplateSelectorProperty();
	static const DependencyProperty& DetailsVisibilityProperty();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif
	DataGrid* GetDataGridOwner() const noexcept;
	const BindingSourceReference& GetItem() const noexcept { return _item; }
	bool GetIsEditing() const noexcept { return _isEditing; }
	bool GetIsNewItem() const noexcept { return _isNewItem; }
	bool GetHasValidationError() const noexcept { return _hasValidationError; }
	const std::vector<BindingValidationResult>&
		GetValidationErrors() const noexcept { return _validationErrors; }
	ControlTemplateReference GetValidationErrorTemplate() const
	{
		return _validationErrorTemplate;
	}
	void SetValidationErrorTemplate(ControlTemplateReference value);
	ItemTemplateReference GetDetailsTemplate() const noexcept
	{
		return _detailsTemplate;
	}
	void SetDetailsTemplate(ItemTemplateReference value);
	bool ClearDetailsTemplate();
	ItemTemplateSelectorReference GetDetailsTemplateSelector() const noexcept
	{
		return _detailsTemplateSelector;
	}
	void SetDetailsTemplateSelector(ItemTemplateSelectorReference value);
	bool ClearDetailsTemplateSelector();
	std::wstring GetValidationSummary(size_t maxIssues = 0) const;
	DataGridCell* GetCell(size_t columnIndex) const noexcept;
	DataGridRowHeader* GetRowHeader() const noexcept { return _rowHeader; }
	::Visibility GetDetailsVisibility() const noexcept
	{
		return _detailsVisibility;
	}
	void SetDetailsVisibility(::Visibility value);
	bool ClearDetailsVisibility();
	ContentPresenter* GetDetailsPresenter() const noexcept
	{
		return _detailsPresenter;
	}
	Control* GetDetailsElement() const noexcept;
	// Enumerates realized containers. With column virtualization enabled this is
	// the frozen prefix followed by the disjoint scrolling viewport strip.
	std::span<DataGridCell* const> GetCells() const noexcept
	{
		const auto& cells = _columnStorageIsSparse ? _realizedCells : _cells;
		return { cells.data(), cells.size() };
	}

private:
	friend class DataGrid;
	friend class DataGridCell;
	friend class DataGridCellsPresenter;
	static const DependencyPropertyKey& IsEditingPropertyKey();
	static const DependencyPropertyKey& IsNewItemPropertyKey();
	static const DependencyPropertyKey& HasValidationErrorPropertyKey();
	static const DependencyPropertyKey& ValidationErrorsPropertyKey();
	void SetCurrentIsEditing(bool value);
	void SetCurrentIsNewItem(bool value);
	bool RefreshValidationState();
	bool AttachValidationTracking();
	void InvalidateValidationVisualTracking() noexcept;
	void UpdateValidationVisual();
	bool Initialize(
		DataGrid& owner,
		const BindingSourceReference& item,
		size_t index,
		std::wstring* outError);
	bool RefreshRealizedColumns(
		size_t frozenEnd, size_t begin, size_t end,
		std::wstring* outError);
	void UpdateColumnWidths(bool propagateLayoutInvalidation = true);
	void UpdateRowHeader();
	bool UpdateRowHeaderPresentation();
	bool UpdateDetailsPresentation(std::wstring* outError = nullptr);
	void UpdateHeightPresentation();
	void UpdateHorizontalScrollOffset(double offset);
	void UpdateDetailsHorizontalScrollOffset(double offset);
	ControlWeakReference _ownerLifetime;
	BindingSourceReference _item;
	bool _isEditing = false;
	bool _isNewItem = false;
	bool _hasValidationError = false;
	bool _refreshingValidation = false;
	std::vector<BindingValidationResult> _validationErrors;
	ControlTemplateReference _validationErrorTemplate;
	EventConnection _itemValidationConnection;
	std::vector<EventConnection> _visualValidationConnections;
	BindingSourceReference _validationTrackedItem;
	size_t _validationVisualRevision = 1;
	size_t _appliedValidationVisualRevision = 0;
	bool _validationStateDirty = true;
	Grid* _rowLayoutGrid = nullptr;
	Grid* _rowHeaderHost = nullptr;
	DataGridCellsPresenter* _cellsGrid = nullptr;
	DataGridRowHeader* _rowHeader = nullptr;
	ContentPresenter* _detailsPresenter = nullptr;
	Control* _validationErrorIndicator = nullptr;
	ItemTemplateReference _detailsTemplate;
	ItemTemplateSelectorReference _detailsTemplateSelector;
	::Visibility _detailsVisibility = ::Visibility::Collapsed;
	ItemTemplateReference _appliedDetailsTemplate;
	bool _detailsVisible = false;
	bool _detailsLoaded = false;
	size_t _detailsPresentationRevision = 0;
	size_t _rowHeaderPresentationRevision = 0;
	bool _restoringDetailsOverrides = false;
	std::vector<DataGridCell*> _frozenCells;
	std::vector<DataGridCell*> _cells;
	std::vector<DataGridCell*> _realizedCells;
	size_t _appliedColumnWidthProjectionRevision = 0;
	size_t _appliedCellSelectionRevision = 0;
	bool _rowHeaderProjectionInitialized = false;
	bool _appliedRowHeaderVisible = false;
	bool _appliedRowHeaderAutoWidth = false;
	double _appliedRowHeaderWidth = 0.0;
	double _appliedHorizontalScrollOffset =
		(std::numeric_limits<double>::quiet_NaN)();
	bool _columnStorageIsSparse = false;
	size_t _realizedFrozenColumnEnd = 0;
	size_t _realizedColumnBegin = 0;
	size_t _realizedColumnEnd = 0;
};

/** WPF-shaped row selector surface hosted by every realized DataGridRow. */
class DataGridRowHeader : public Button
{
public:
	DataGridRowHeader();
	UIClass Type() override { return UIClass::UI_DataGridRowHeader; }
	static const DependencyProperty& IsRowSelectedProperty();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif
	bool GetIsRowSelected() const noexcept { return _isRowSelected; }
	DataGridRow* GetRowOwner() const noexcept;

protected:
	const DependencyPropertyMetadata* ResolveExactDependencyPropertyMetadata(
		const DependencyProperty& property) const override;
	cui::core::Size FinalizeMeasureCore(
		cui::core::Size intrinsic,
		const cui::core::Constraints& available) override;
	bool ProcessInput(const InputReport& input) override;
	bool OnClick() override;
	CursorKind QueryCursor(int localX, int localY) override;

private:
	friend class DataGrid;
	friend class DataGridRow;
	static const DependencyPropertyKey& IsRowSelectedPropertyKey();
	void Initialize(DataGridRow& row);
	void InvalidateSharedWidthMeasure();
	void SetCurrentIsRowSelected(bool value);
	bool TryResolveResizeRow(
		int localY, size_t& rowIndex, bool& fromTopEdge) const noexcept;
	bool BeginRowResize(int localY);
	bool ContinueRowResize(int localY);
	void EndRowResize(bool cancel);
	ControlWeakReference _rowLifetime;
	ModifierKeys _activationModifiers = ModifierKeys::None;
	bool _isRowSelected = false;
	bool _isResizing = false;
	bool _resizeFromTopEdge = false;
	double _resizeStartRenderY = 0.0;
	double _resizeStartHeight = 0.0;
};

class DataGridColumnHeader : public Button
{
public:
	DataGridColumnHeader();
	UIClass Type() override { return UIClass::UI_DataGridColumnHeader; }
	DataGridColumn* GetColumn() const noexcept { return _column; }

protected:
	void OnApplyTemplate() override;
	bool ProcessInput(const InputReport& input) override;
	bool OnClick() override;
	CursorKind QueryCursor(int localX, int localY) override;

private:
	friend class DataGrid;
	friend class DataGridColumnHeadersPresenter;
	bool Initialize(DataGrid& owner, DataGridColumn& column, size_t index,
		std::wstring* outError = nullptr);
	std::wstring ContentPresentationError() const;
	bool TryResolveResizeColumn(
		int localX, size_t& columnIndex,
		bool& resizeFromLeftEdge) const noexcept;
	bool BeginColumnResize(int localX);
	bool ContinueColumnResize(int localX);
	void EndColumnResize(bool cancel);
	bool PrepareColumnReorder(int localX, int localY);
	bool BeginColumnReorder(int localX, int localY);
	bool ContinueColumnReorder(int localX, int localY);
	void EndColumnReorder(bool cancel);
	DataGrid* _owner = nullptr;
	ControlWeakReference _ownerLifetime;
	DataGridColumn* _column = nullptr;
	size_t _columnIndex = DataGridCellInfo::InvalidIndex;
	bool _multiColumnSortRequested = false;
	bool _isResizing = false;
	bool _resizeFromLeftEdge = false;
	size_t _resizingColumnIndex = DataGridCellInfo::InvalidIndex;
	double _resizeStartRenderX = 0.0;
	double _resizeStartWidth = 0.0;
	bool _isReorderPending = false;
	bool _isReordering = false;
	double _reorderStartLocalX = 0.0;
	double _reorderStartRenderX = 0.0;
	double _reorderStartRenderY = 0.0;
};

class DataGridColumnHeadersPresenter : public Grid
{
public:
	DataGridColumnHeadersPresenter();
	UIClass Type() override
	{
		return UIClass::UI_DataGridColumnHeadersPresenter;
	}
	bool Initialize(DataGrid& owner, std::wstring* outError = nullptr);
	DataGrid* GetDataGridOwner() const noexcept { return _owner; }
	DataGridColumnHeader* GetHeader(size_t columnIndex) const noexcept
	{
		if (!_columnStorageIsSparse)
			return columnIndex < _headers.size() ? _headers[columnIndex] : nullptr;
		if (columnIndex < _realizedFrozenColumnEnd)
			return columnIndex < _frozenHeaders.size()
				? _frozenHeaders[columnIndex] : nullptr;
		return columnIndex >= _realizedColumnBegin
			&& columnIndex < _realizedColumnEnd
			&& columnIndex - _realizedColumnBegin < _headers.size()
			? _headers[columnIndex - _realizedColumnBegin] : nullptr;
	}
	size_t GetRealizedHeaderSlotCount() const noexcept
	{
		return _frozenHeaders.size() + _headers.size();
	}
	bool GetIsColumnHeaderDragging() const noexcept
	{
		return _isColumnHeaderDragging;
	}
	size_t GetColumnHeaderDropDisplayIndex() const noexcept
	{
		return _dropDisplayIndex;
	}
	Control* GetColumnHeaderDragIndicator() const noexcept
	{
		return _dragIndicator.Get();
	}
	Control* GetColumnHeaderDropLocationIndicator() const noexcept
	{
		return _dropIndicator.Get();
	}
	bool UpdateColumnWidths(bool propagateLayoutInvalidation = true);
	bool UpdateHorizontalScrollOffset(double offset);
	void Arrange(cui::core::Rect finalRect) override;

protected:
	cui::core::Size MeasureCore(
		const cui::core::Constraints& available) override;

private:
	friend class DataGrid;
	friend class DataGridColumnHeader;
	enum class ColumnReorderStartResult : uint8_t
	{
		Aborted,
		Canceled,
		Started,
	};
	bool RefreshRealizedColumns(
		size_t frozenEnd, size_t begin, size_t end,
		std::wstring* outError);
	bool TryCommitResizeLayoutLocally(bool heightIsFixed);
	ColumnReorderStartResult BeginColumnHeaderDrag(
		DataGridColumnHeader& header,
		double renderX, double renderY);
	bool UpdateColumnHeaderDrag(double renderX, double renderY);
	void FinishColumnHeaderDrag(bool cancel);
	void AbandonColumnHeaderDragAfterOwnerDestruction();
	bool ResolveColumnHeaderDrop(
		double renderX, double renderY,
		size_t& displayIndex, double& boundary,
		bool& valid) const;
	void ArrangeColumnHeaderDragIndicators();
	DataGrid* _owner = nullptr;
	std::uint64_t _initializeGeneration = 0;
	std::uint64_t _committedInitializeGeneration = 0;
	std::uint64_t _presentationRefreshGeneration = 0;
	std::vector<DataGridColumnHeader*> _frozenHeaders;
	std::vector<DataGridColumnHeader*> _headers;
	size_t _appliedColumnWidthProjectionRevision = 0;
	bool _columnStorageIsSparse = false;
	size_t _realizedFrozenColumnEnd = 0;
	size_t _realizedColumnBegin = 0;
	size_t _realizedColumnEnd = 0;
	bool _isColumnHeaderDragging = false;
	DataGridColumn* _reorderingColumn = nullptr;
	double _reorderStartRenderX = 0.0;
	double _reorderStartRenderY = 0.0;
	double _reorderLastRenderX = 0.0;
	double _reorderLastRenderY = 0.0;
	double _reorderCurrentRenderX = 0.0;
	double _reorderCurrentRenderY = 0.0;
	double _dragPointerOffsetX = 0.0;
	double _dragPointerOffsetY = 0.0;
	double _dragIndicatorWidth = 0.0;
	double _dragIndicatorHeight = 0.0;
	double _dropIndicatorBoundary = 0.0;
	bool _dropIndicatorVisible = false;
	size_t _dropDisplayIndex = DataGridCellInfo::InvalidIndex;
	ControlWeakReference _dragIndicator;
	ControlWeakReference _dropIndicator;
};

/**
 * WPF-shaped tabular selector. Row selection and item virtualization remain
 * owned by ListBox/ItemsControl; this type adds non-visual columns, cell
 * composition, sorting, and cell editing.
 */
class DataGrid : public ListBox
{
public:
	static constexpr size_t ClipboardCopyCellLimit = 1'000'000;
	static constexpr size_t ClipboardCopyCharacterLimit = 16 * 1024 * 1024;

	DataGrid();
	~DataGrid() override;
	UIClass Type() override { return UIClass::UI_DataGrid; }

	static const DependencyProperty& AutoGenerateColumnsProperty();
	static const DependencyProperty& CurrentItemProperty();
	static const DependencyProperty& CurrentColumnProperty();
	static const DependencyProperty& CurrentCellProperty();
	static const DependencyProperty& IsReadOnlyProperty();
	static const DependencyProperty& CanUserAddRowsProperty();
	static const DependencyProperty& CanUserDeleteRowsProperty();
	static const DependencyProperty& CanUserSortColumnsProperty();
	static const DependencyProperty& CanUserResizeColumnsProperty();
	static const DependencyProperty& CanUserResizeRowsProperty();
	static const DependencyProperty& CanUserReorderColumnsProperty();
	static const DependencyProperty& EnableColumnVirtualizationProperty();
	static const DependencyProperty& FrozenColumnCountProperty();
	static const DependencyProperty& SelectionUnitProperty();
	static const DependencyProperty& ColumnWidthProperty();
	static const DependencyProperty& MinColumnWidthProperty();
	static const DependencyProperty& MaxColumnWidthProperty();
	static const DependencyProperty& ColumnHeaderHeightProperty();
	static const DependencyProperty& RowHeaderWidthProperty();
	static const DependencyProperty& RowHeaderActualWidthProperty();
	static const DependencyProperty& RowHeightProperty();
	static const DependencyProperty& MinRowHeightProperty();
	static const DependencyProperty& HeadersVisibilityProperty();
	static const DependencyProperty& GridLinesVisibilityProperty();
	static const DependencyProperty& RowBackgroundProperty();
	static const DependencyProperty& AlternatingRowBackgroundProperty();
	static const DependencyProperty& HorizontalGridLinesBrushProperty();
	static const DependencyProperty& VerticalGridLinesBrushProperty();
	static const DependencyProperty& RowValidationErrorTemplateProperty();
	static const DependencyProperty& ClipboardCopyModeProperty();
	static const DependencyProperty& CellStyleProperty();
	static const DependencyProperty& ColumnHeaderStyleProperty();
	static const DependencyProperty& RowStyleProperty();
	static const DependencyProperty& RowStyleSelectorProperty();
	static const DependencyProperty& RowHeaderStyleProperty();
	static const DependencyProperty& RowHeaderTemplateProperty();
	static const DependencyProperty& RowHeaderTemplateSelectorProperty();
	static const DependencyProperty& AreRowDetailsFrozenProperty();
	static const DependencyProperty& RowDetailsVisibilityModeProperty();
	static const DependencyProperty& RowDetailsTemplateProperty();
	static const DependencyProperty& RowDetailsTemplateSelectorProperty();
	static const RoutedCommand& BeginEditCommand();
	static const RoutedCommand& CommitEditCommand();
	static const RoutedCommand& CancelEditCommand();
	/** WPF identity alias for ApplicationCommands.Delete. */
	static const RoutedCommand& DeleteCommand();
	/** WPF identity alias for ApplicationCommands.Copy. */
	static const RoutedCommand& CopyCommand();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif

	BindingListReference GetItemsSource() const noexcept override
	{
		return _source;
	}
	void SetItemsSource(BindingListReference value) override;

	bool GetAutoGenerateColumns() const noexcept { return _autoGenerateColumns; }
	void SetAutoGenerateColumns(bool value);
	bool GetIsReadOnly() const noexcept { return _isReadOnly; }
	void SetIsReadOnly(bool value);
	bool GetCanUserAddRows() const noexcept { return _canUserAddRows; }
	void SetCanUserAddRows(bool value);
	bool GetCanUserDeleteRows() const noexcept { return _canUserDeleteRows; }
	void SetCanUserDeleteRows(bool value);
	bool GetCanUserSortColumns() const noexcept { return _canUserSortColumns; }
	void SetCanUserSortColumns(bool value);
	bool GetCanUserResizeColumns() const noexcept { return _canUserResizeColumns; }
	void SetCanUserResizeColumns(bool value);
	bool GetCanUserResizeRows() const noexcept { return _canUserResizeRows; }
	void SetCanUserResizeRows(bool value);
	bool GetCanUserReorderColumns() const noexcept
	{
		return _canUserReorderColumns;
	}
	void SetCanUserReorderColumns(bool value);
	bool GetEnableColumnVirtualization() const noexcept
	{
		return _enableColumnVirtualization;
	}
	void SetEnableColumnVirtualization(bool value);
	int GetFrozenColumnCount() const noexcept { return _frozenColumnCount; }
	void SetFrozenColumnCount(int value);
	DataGridSelectionUnit GetSelectionUnit() const noexcept
	{
		return _selectionUnit;
	}
	void SetSelectionUnit(DataGridSelectionUnit value);
	const DataGridLength& GetColumnWidth() const noexcept
	{
		return _columnWidth;
	}
	void SetColumnWidth(DataGridLength value);
	double GetMinColumnWidth() const noexcept { return _minColumnWidth; }
	void SetMinColumnWidth(double value);
	double GetMaxColumnWidth() const noexcept { return _maxColumnWidth; }
	void SetMaxColumnWidth(double value);
	double GetColumnHeaderHeight() const noexcept { return _columnHeaderHeight; }
	void SetColumnHeaderHeight(double value);
	double GetRowHeaderWidth() const noexcept { return _rowHeaderWidth; }
	void SetRowHeaderWidth(double value);
	double GetRowHeaderActualWidth() const noexcept
	{
		return _rowHeaderActualWidth;
	}
	double GetRowHeight() const noexcept { return _rowHeight; }
	void SetRowHeight(double value);
	double GetMinRowHeight() const noexcept { return _minRowHeight; }
	void SetMinRowHeight(double value);
	DataGridHeadersVisibility GetHeadersVisibility() const noexcept
	{
		return _headersVisibility;
	}
	void SetHeadersVisibility(DataGridHeadersVisibility value);
	DataGridGridLinesVisibility GetGridLinesVisibility() const noexcept
	{
		return _gridLinesVisibility;
	}
	void SetGridLinesVisibility(DataGridGridLinesVisibility value);
	const cui::drawing::Brush& GetRowBackground() const noexcept
	{
		return _rowBackground;
	}
	void SetRowBackground(cui::drawing::Brush value);
	const cui::drawing::Brush& GetAlternatingRowBackground() const noexcept
	{
		return _alternatingRowBackground;
	}
	void SetAlternatingRowBackground(cui::drawing::Brush value);
	const cui::drawing::Brush& GetHorizontalGridLinesBrush() const noexcept
	{
		return _horizontalGridLinesBrush;
	}
	void SetHorizontalGridLinesBrush(cui::drawing::Brush value);
	const cui::drawing::Brush& GetVerticalGridLinesBrush() const noexcept
	{
		return _verticalGridLinesBrush;
	}
	void SetVerticalGridLinesBrush(cui::drawing::Brush value);
	ControlTemplateReference GetRowValidationErrorTemplate() const
	{
		return _rowValidationErrorTemplate;
	}
	void SetRowValidationErrorTemplate(ControlTemplateReference value);
	DataGridClipboardCopyMode GetClipboardCopyMode() const noexcept
	{
		return _clipboardCopyMode;
	}
	void SetClipboardCopyMode(DataGridClipboardCopyMode value);
	const std::wstring& GetCellStyle() const noexcept { return _cellStyle; }
	void SetCellStyle(std::wstring value);
	const std::wstring& GetColumnHeaderStyle() const noexcept
	{
		return _columnHeaderStyle;
	}
	void SetColumnHeaderStyle(std::wstring value);
	const std::wstring& GetRowStyle() const noexcept { return _rowStyle; }
	void SetRowStyle(std::wstring value);
	ItemStyleSelectorReference GetRowStyleSelector() const noexcept
	{
		return _rowStyleSelector;
	}
	void SetRowStyleSelector(ItemStyleSelectorReference value);
	const std::wstring& GetRowHeaderStyle() const noexcept
	{
		return _rowHeaderStyle;
	}
	void SetRowHeaderStyle(std::wstring value);
	ItemTemplateReference GetRowHeaderTemplate() const noexcept
	{
		return _rowHeaderTemplate;
	}
	void SetRowHeaderTemplate(ItemTemplateReference value);
	ItemTemplateSelectorReference GetRowHeaderTemplateSelector() const noexcept
	{
		return _rowHeaderTemplateSelector;
	}
	void SetRowHeaderTemplateSelector(ItemTemplateSelectorReference value);
	bool GetAreRowDetailsFrozen() const noexcept
	{
		return _areRowDetailsFrozen;
	}
	void SetAreRowDetailsFrozen(bool value);
	DataGridRowDetailsVisibilityMode GetRowDetailsVisibilityMode() const noexcept
	{
		return _rowDetailsVisibilityMode;
	}
	void SetRowDetailsVisibilityMode(DataGridRowDetailsVisibilityMode value);
	ItemTemplateReference GetRowDetailsTemplate() const noexcept
	{
		return _rowDetailsTemplate;
	}
	void SetRowDetailsTemplate(ItemTemplateReference value);
	ItemTemplateSelectorReference GetRowDetailsTemplateSelector() const noexcept
	{
		return _rowDetailsTemplateSelector;
	}
	void SetRowDetailsTemplateSelector(ItemTemplateSelectorReference value);

	DataGridColumn* AddColumn(std::unique_ptr<DataGridColumn> column);
	/** Inserts a column at a stable logical collection index. */
	DataGridColumn* InsertColumn(
		size_t logicalIndex, std::unique_ptr<DataGridColumn> column);
	/** AOT schema expansion entry; marks the supplied column as generated. */
	DataGridColumn* AddAutoGeneratedColumn(
		std::unique_ptr<DataGridColumn> column);
	/** Takes ownership of an unattached column produced by a materializer. */
	DataGridColumn* AdoptColumn(DataGridColumn* column);
	/** Detaches and transfers ownership of a logical collection entry. */
	std::unique_ptr<DataGridColumn> RemoveColumn(size_t logicalIndex);
	/** Detaches and transfers ownership when the supplied column is owned here. */
	std::unique_ptr<DataGridColumn> RemoveColumn(DataGridColumn& column);
	void ClearColumns();
	size_t ColumnCount() const noexcept { return _logicalColumns.size(); }
	size_t VisibleColumnCount() const noexcept
	{
		return _visibleColumns.size();
	}
	/** Returns a column in stable logical collection/insertion order. */
	DataGridColumn* GetColumn(size_t index) const noexcept;
	/** Returns a column in the current visual order. */
	DataGridColumn* GetColumnFromDisplayIndex(size_t index) const noexcept;
	/** Commits one visual-order change without relocating column objects. */
	bool SetColumnDisplayIndex(DataGridColumn& column, size_t displayIndex);
	bool MoveColumn(size_t oldDisplayIndex, size_t newDisplayIndex);

	template<typename TColumn, typename... TArgs>
	TColumn* AddColumn(TArgs&&... args)
	{
		static_assert(std::is_base_of_v<DataGridColumn, TColumn>);
		auto column = std::make_unique<TColumn>(
			std::forward<TArgs>(args)...);
		auto* raw = column.get();
		(void)AddColumn(std::move(column));
		return raw;
	}

	template<typename TColumn, typename... TArgs>
	TColumn* InsertColumn(size_t logicalIndex, TArgs&&... args)
	{
		static_assert(std::is_base_of_v<DataGridColumn, TColumn>);
		auto column = std::make_unique<TColumn>(
			std::forward<TArgs>(args)...);
		auto* raw = column.get();
		(void)InsertColumn(logicalIndex, std::move(column));
		return raw;
	}

	const DataGridCellInfo& GetCurrentCell() const noexcept
	{
		return _currentCell;
	}
	BindingValue GetCurrentItem() const
	{
		return _currentCell.Item
			? BindingValue(_currentCell.Item) : BindingValue{};
	}
	void SetCurrentItem(const BindingValue& value);
	DataGridColumn* GetCurrentColumn() const noexcept
	{
		return _currentCell.Column;
	}
	void SetCurrentColumn(DataGridColumn* value);
	bool SetCurrentCell(const DataGridCellInfo& value);
	bool SetCurrentCell(size_t rowIndex, size_t columnIndex);
	/** WPF-shaped vertical row positioning; an empty item is rejected. */
	bool ScrollIntoView(const BindingValue& item);
	/**
	 * WPF-shaped cell positioning. An empty item performs only horizontal
	 * positioning; a null column performs only vertical positioning.
	 */
	bool ScrollIntoView(
		const BindingValue& item, DataGridColumn* column);
	const DataGridSelectedCellCollection& GetSelectedCells() const noexcept
	{
		return _selectedCells;
	}
	bool IsCellSelected(size_t rowIndex, size_t columnIndex) const;
	bool SelectCell(size_t rowIndex, size_t columnIndex);
	bool UnselectCell(size_t rowIndex, size_t columnIndex);
	void SelectAllCells();
	void UnselectAllCells();
	bool BeginEdit();
	bool BeginEdit(const RoutedEventArgs* editingEventArgs);
	bool CommitEdit();
	bool CommitEdit(
		DataGridEditingUnit editingUnit,
		bool exitEditingMode);
	bool CancelEdit();
	bool CancelEdit(DataGridEditingUnit editingUnit);
	/** Executes the WPF user-delete policy against selected data rows. */
	bool DeleteSelectedRows();
	/** Builds and publishes the selected cells as bounded Unicode TSV text. */
	bool Copy();
	bool PerformSort(DataGridColumn& column, bool multiColumn);
	bool ResizeColumn(size_t columnIndex, double pixelWidth);
	/** Applies the native user row-resize policy to one item occurrence. */
	bool ResizeRow(size_t rowIndex, double pixelHeight);
	/** Clears the user height override for one item occurrence. */
	bool AutoSizeRow(size_t rowIndex);
	std::unique_ptr<DataGridColumnHeadersPresenter>
		CreateColumnHeadersPresenter();
	DataGridColumnHeadersPresenter* GetColumnHeadersPresenter() const noexcept
	{
		return dynamic_cast<DataGridColumnHeadersPresenter*>(
			_headersPresenter.Get());
	}
	Button* GetSelectAllButton() const noexcept
	{
		return dynamic_cast<Button*>(_selectAllButton.Get());
	}

	/** WPF DataGridAutomationPeer logical Grid/Table/Selection surface. */
	bool TryGetAccessibilityVirtualNode(
		uint32_t id, AccessibilityVirtualNode& result);
	size_t GetAccessibilityVirtualChildCount(uint32_t parentId) const;
	bool TryGetAccessibilityVirtualChildAt(
		uint32_t parentId, size_t index, uint32_t& result) const;
	bool TryGetAccessibilityVirtualSibling(
		uint32_t parentId, uint32_t id, bool next,
		uint32_t& result) const;
	bool TryHitTestAccessibilityVirtualNode(
		float localX, float localY, uint32_t& result) const;
	AccessibilityVirtualContainerInfo
		GetAccessibilityVirtualContainerInfo() const noexcept;
	void GetAccessibilityVirtualSelection(
		std::vector<uint32_t>& result) const;
	bool GetAccessibilityVirtualItemAt(
		int row, int column, uint32_t& result) const;
	void GetAccessibilityVirtualColumnHeaders(
		std::vector<uint32_t>& result) const;
	void GetAccessibilityVirtualRowHeaders(
		std::vector<uint32_t>& result) const;
	AutomationOperationResult FocusAccessibilityVirtualNode(uint32_t id);
	bool TryGetAccessibilityVirtualFocusedNode(uint32_t& result) const;
	bool InvokeAccessibilityVirtualNode(uint32_t id);
	bool SetAccessibilityVirtualNodeValue(
		uint32_t id, const std::wstring& value);
	bool SelectAccessibilityVirtualNode(
		uint32_t id, AccessibilitySelectionAction action);
	bool ScrollAccessibilityVirtualNodeIntoView(uint32_t id);
	bool GetAccessibilityScrollInfo(
		AccessibilityScrollInfo& result) const noexcept;
	bool ScrollAccessibility(
		AccessibilityScrollAmount horizontal,
		AccessibilityScrollAmount vertical);
	bool SetAccessibilityScrollPercent(
		double horizontalPercent, double verticalPercent);

	Event<void(DataGrid*, DataGridSortingEventArgs&)> Sorting;
	Event<void(DataGrid*, DataGridBeginningEditEventArgs&)> BeginningEdit;
	Event<void(DataGrid*, DataGridPreparingCellForEditEventArgs&)>
		PreparingCellForEdit;
	Event<void(DataGrid*, DataGridCellEditEndingEventArgs&)> CellEditEnding;
	Event<void(DataGrid*, DataGridRowEditEndingEventArgs&)> RowEditEnding;
	Event<void(DataGrid*, DataGridAddingNewItemEventArgs&)> AddingNewItem;
	Event<void(DataGrid*, DataGridInitializingNewItemEventArgs&)>
		InitializingNewItem;
	Event<void(DataGrid*, DataGridCurrentCellChangedEventArgs&)>
		CurrentCellChanged;
	Event<void(DataGrid*, DataGridSelectedCellsChangedEventArgs&)>
		SelectedCellsChanged;
	Event<void(DataGrid*, DataGridRowClipboardEventArgs&)>
		CopyingRowClipboardContent;
	Event<void(DataGrid*, DataGridRowEventArgs&)> LoadingRow;
	Event<void(DataGrid*, DataGridRowEventArgs&)> UnloadingRow;
	Event<void(DataGrid*, DataGridRowDetailsEventArgs&)> LoadingRowDetails;
	Event<void(DataGrid*, DataGridRowDetailsEventArgs&)> UnloadingRowDetails;
	Event<void(DataGrid*, DataGridRowDetailsEventArgs&)>
		RowDetailsVisibilityChanged;
	Event<void(DataGrid*, DataGridColumnDisplayIndexChangedEventArgs&)>
		ColumnDisplayIndexChanged;
	Event<void(DataGrid*, DataGridColumnHeaderDragStartedEventArgs&)>
		ColumnHeaderDragStarted;
	Event<void(DataGrid*, DataGridColumnHeaderDragDeltaEventArgs&)>
		ColumnHeaderDragDelta;
	Event<void(DataGrid*, DataGridColumnHeaderDragCompletedEventArgs&)>
		ColumnHeaderDragCompleted;
	Event<void(DataGrid*, DataGridColumnReorderingEventArgs&)>
		ColumnReordering;
	Event<void(DataGrid*, DataGridColumnEventArgs&)> ColumnReordered;
	Event<void(DataGrid*, DataGridAutoGeneratingColumnEventArgs&)>
		AutoGeneratingColumn;
	/** No empty EventArgs object is constructed for this payload-free event. */
	Event<void(DataGrid*)> AutoGeneratedColumns;
	void Arrange(cui::core::Rect finalRect) override;

protected:
	/** Actual sortable/filterable data projection; excludes the new-row sentinel. */
	CollectionViewSource* GetCollectionView() const noexcept
	{
		return _itemsView.get();
	}
	void PrepareMeasureCore(
		const cui::core::Constraints& available) override;
	std::unique_ptr<AutomationPeer> OnCreateAutomationPeer() override;
	std::unique_ptr<ItemsSourceTransactionState>
		CaptureItemsSourceTransactionState() override;
	void RestoreItemsSourceTransactionState(
		ItemsSourceTransactionState& state) noexcept override;
	std::unique_ptr<Control> BuildGeneratedItem(
		const BindingSourceReference& item,
		size_t index,
		BindingPathObservation& observation) override;
	bool CanRecycleGeneratedItemAcrossIndices(
		const Control& visual, size_t oldIndex) const noexcept override;
	bool TryRebindGeneratedItemAcrossIndices(
		Control& visual,
		size_t oldIndex,
		size_t newIndex,
		const BindingSourceReference& item,
		BindingPathObservation& observation,
		std::wstring* outError) override;
	void OnBeforeGeneratedItemsPrepared() override;
	void OnGeneratedItemClearing(Control& visual) override;
	void OnGeneratedItemsRebuilt() override;
	void OnGeneratedItemsRealized() override;
	void OnItemsSourceTransactionCommitted() override;
	void OnItemsSourceReplacementPreparing(
		const BindingListReference& oldValue,
		const BindingListReference& newValue) override;
	void OnItemsSourceCollectionChangePreparing(
		const CollectionChangedEventArgs& change,
		const BindingListReference& previousSnapshot) override;
	void OnItemsSourceCollectionChangeCommitted(
		const CollectionChangedEventArgs& change) override;
	void OnItemsSourceChanged(
		const BindingListReference& oldValue,
		const BindingListReference& newValue) override;
	void OnGeneratedItemIndexChanged(
		Control& visual, size_t oldIndex, size_t newIndex) override;
	void OnSelectedIndexChanged(int oldValue, int newValue) override;
	void OnSelectionChanged(SelectionChangedEventArgs& args) override;
	void OnControlTemplatePresentationChanged() override;
	void PreparePresentation() override;
	bool ShouldRealizeVirtualItemsWithoutViewport() const noexcept override
	{
		// A DataGrid commonly lives on a non-selected TabItem.  Until its
		// ScrollViewer supplies a finite viewport, realizing rows only creates a
		// complete off-screen cell tree that will be discarded during templating.
		return false;
	}
	bool UseVisibleOnlyRangeDuringVerticalThumbDrag() const noexcept override
	{
		return true;
	}
	float GetVirtualizedItemHeight() const noexcept override;
	bool UseMeasuredVirtualizedItemHeight(
		const Control& item) const noexcept override;
	std::span<const VirtualizedItemExtentOverride>
		GetVirtualizedItemExtentOverrides() const noexcept override
	{
		return _virtualRowHeightOverrides;
	}
	size_t GetVirtualizedItemExtentOverridesRevision() const noexcept override
	{
		return _virtualRowHeightOverrideRevision;
	}
	double GetVirtualizedHorizontalExtent() const override;
	bool HandlesNavigationKey(Key key) const override;
	bool ApplyTextInput(const TextCompositionEventArgs& input) override;
	bool ProcessInput(const InputReport& input) override;
	virtual void OnCanExecuteBeginEdit(CanExecuteRoutedEventArgs& args);
	virtual void OnExecutedBeginEdit(ExecutedRoutedEventArgs& args);
	virtual void OnCanExecuteCommitEdit(CanExecuteRoutedEventArgs& args);
	virtual void OnExecutedCommitEdit(ExecutedRoutedEventArgs& args);
	virtual void OnCanExecuteCancelEdit(CanExecuteRoutedEventArgs& args);
	virtual void OnExecutedCancelEdit(ExecutedRoutedEventArgs& args);
	virtual void OnCanExecuteDelete(CanExecuteRoutedEventArgs& args);
	virtual void OnExecutedDelete(ExecutedRoutedEventArgs& args);
	virtual void OnCanExecuteCopy(CanExecuteRoutedEventArgs& args);
	virtual void OnExecutedCopy(ExecutedRoutedEventArgs& args);
	virtual void OnCopyingRowClipboardContent(
		DataGridRowClipboardEventArgs& args);

private:
	friend class DataGridSelectedCellCollection;
	struct DataGridItemsSourceTransactionState;
	struct DataGridItemsView;
	struct ColumnResizeSnapshot final
	{
		DataGridColumn* Column = nullptr;
		DataGridLength Width;
		DataGridLength LocalWidth;
		bool HasLocalWidth = false;
		DataGridColumn::RuntimeWidthState RuntimeWidth;
		double Desired = (std::numeric_limits<double>::quiet_NaN)();
		double Display = (std::numeric_limits<double>::quiet_NaN)();
	};
	struct RowHeightOverride final
	{
		BindingSourceReference Item;
		size_t Occurrence = DataGridCellInfo::InvalidIndex;
		double Height = 0.0;
	};
	struct RowPresentationOverride final
	{
		BindingSourceReference Item;
		size_t Occurrence = DataGridCellInfo::InvalidIndex;
		bool HasDetailsTemplate = false;
		ItemTemplateReference DetailsTemplate;
		bool HasDetailsTemplateSelector = false;
		ItemTemplateSelectorReference DetailsTemplateSelector;
		bool HasDetailsVisibility = false;
		::Visibility DetailsVisibility = ::Visibility::Collapsed;
	};
	enum class AccessibilityNodeKind : uint8_t
	{
		ColumnHeader,
		Row,
		RowHeader,
		Cell,
	};
	struct AccessibilityColumnIdentity final
	{
		DataGridColumn* Column = nullptr;
		uint32_t ColumnIdentity = 0;
		uint32_t HeaderId = 0;
	};
	struct AccessibilityCellIdentity final
	{
		DataGridColumn* Column = nullptr;
		uint32_t ColumnIdentity = 0;
		size_t ColumnIndex = DataGridCellInfo::InvalidIndex;
		uint32_t Id = 0;
	};
	struct AccessibilityRowIdentity final
	{
		BindingSourceReference Item;
		size_t Occurrence = DataGridCellInfo::InvalidIndex;
		size_t ViewIndex = DataGridCellInfo::InvalidIndex;
		uint32_t RowId = 0;
		uint32_t HeaderId = 0;
		std::vector<AccessibilityCellIdentity> Cells;
	};
	struct AccessibilityNodeLocator final
	{
		AccessibilityNodeKind Kind = AccessibilityNodeKind::Row;
		size_t RowIndex = DataGridCellInfo::InvalidIndex;
		size_t ColumnIndex = DataGridCellInfo::InvalidIndex;
	};
	struct AccessibilityIdentityState final
	{
		std::vector<AccessibilityColumnIdentity> Columns;
		std::vector<AccessibilityRowIdentity> Rows;
		bool StructureChangePending = false;
	};
	friend class DataGridCell;
	friend class DataGridColumn;
	friend class DataGridBoundColumn;
	friend class DataGridCheckBoxColumn;
	friend class DataGridComboBoxColumn;
	friend class DataGridHyperlinkColumn;
	friend class DataGridTemplateColumn;
	friend class DataGridTextColumn;
	friend class DataGridColumnHeader;
	friend class DataGridColumnHeadersPresenter;
	friend class DataGridCellsPresenter;
	friend class DataGridRow;
	friend class DataGridRowHeader;

	BindingListReference _source;
	std::shared_ptr<CollectionViewSource> _itemsView;
	std::shared_ptr<DataGridItemsView> _displayItemsView;
	// _columns is the hot visual projection. The companion pointer vector keeps
	// public collection order stable without relocating DataGridColumn objects.
	std::vector<std::unique_ptr<DataGridColumn>> _columns;
	std::vector<DataGridColumn*> _logicalColumns;
	// Compact display-ordered projection used by navigation and automation.
	// Hidden/Collapsed columns remain owned by _columns but never appear here.
	std::vector<DataGridColumn*> _visibleColumns;
	mutable std::vector<std::optional<GridLength>> _resolvedColumnWidths;
	std::vector<ColumnResizeSnapshot> _columnResizeSnapshot;
	std::vector<ColumnResizeSnapshot> _columnResizeWorkingSnapshot;
	// Native pointer moves reuse these transaction buffers. Keeping them on the
	// DataGrid removes three heap allocations from every resize input report.
	std::vector<double> _columnResizeDisplayScratch;
	std::vector<double> _columnResizeDesiredScratch;
	std::vector<double> _columnResizeFactorScratch;
	std::vector<double> _columnResizePreviousDisplayScratch;
	size_t _columnResizeTransactionIndex = DataGridCellInfo::InvalidIndex;
	bool _columnResizeCompensatesLeft = false;
	double _columnResizeLastRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	double _columnResizeInputBias = 0.0;
	double _columnResizePendingRawWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	bool _applyingPendingColumnResizeInput = false;
	// User row sizes are attached to stable item occurrences. The second vector
	// is the sorted current-index projection consumed by the virtual offset host.
	std::vector<RowHeightOverride> _rowHeightOverrides;
	std::vector<VirtualizedItemExtentOverride> _virtualRowHeightOverrides;
	// Only item occurrences with an authored DataGridRow local value appear
	// here. Selectors stay shared by the Grid and never expand this vector.
	std::vector<RowPresentationOverride> _rowPresentationOverrides;
	size_t _virtualRowHeightOverrideRevision = 1;
	BindingSourceReference _rowResizeItem;
	size_t _rowResizeOccurrence = DataGridCellInfo::InvalidIndex;
	size_t _rowResizeIndex = DataGridCellInfo::InvalidIndex;
	double _rowResizeHeight = 0.0;
	bool _rowResizeActive = false;
	double _rowResizePendingHeight =
		(std::numeric_limits<double>::quiet_NaN)();
	bool _applyingPendingRowResizeInput = false;
	// Realized header/row grids cache the revision they projected. This keeps a
	// vertical viewport change from rebuilding identical column definitions.
	size_t _columnWidthProjectionRevision = 1;
	bool _columnWidthRefreshPending = false;
	size_t _columnWidthDirtyBegin = DataGridCellInfo::InvalidIndex;
	size_t _columnWidthDirtyEnd = DataGridCellInfo::InvalidIndex;
	double _columnWidthDirtyVisualSpan =
		(std::numeric_limits<double>::quiet_NaN)();
	std::vector<uint8_t> _columnWidthMeasureDirty;
	// Prefix[i] is the logical left edge of column i; Prefix.back() is the
	// complete data-column width. It turns viewport, bring-into-view and UIA
	// column positioning into O(log C)/O(1) operations for very wide grids.
	mutable std::vector<double> _columnWidthPrefix;
	// Content measurement is independent from viewport/Star resolution.  Keep
	// the sampled cell widths across row-only visual regeneration so an Auto
	// column does not reread the first 1000 source records for every row style
	// change.
	mutable std::vector<std::optional<double>> _sampledColumnContentWidths;
	mutable size_t _columnContentWidthCacheEpoch = 1;
	double _columnViewportWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	mutable bool _accessibilityIdentitiesDirty = true;
	mutable bool _buildingAccessibilityIdentities = false;
	mutable size_t _accessibilityIdentityRevision = 1;
	mutable std::vector<AccessibilityColumnIdentity>
		_accessibilityColumns;
	mutable std::vector<AccessibilityRowIdentity> _accessibilityRows;
	mutable std::unordered_map<size_t, size_t>
		_accessibilityRowIndexLookup;
	mutable std::unordered_map<uint32_t, AccessibilityNodeLocator>
		_accessibilityNodeLookup;
	bool _accessibilityStructureChangePending = false;
	DataGridCellInfo _currentCell;
	DataGridSelectedCellCollection _selectedCells;
	std::optional<DataGridCellInfo> _selectionAnchor;
	std::vector<DataGridCellInfo> _selectionRangeBase;
	std::vector<std::pair<BindingSourceReference, size_t>>
		_selectedRowSnapshot;
	// Mirrors ListBox's compact row-selection shape.  FullRow SelectAll keeps
	// [0, Count) plus sparse holes here instead of duplicating one item/index
	// pair per selected row in _selectedRowSnapshot.
	SelectedIndexCollection _selectedRowIndexSnapshot;
	ControlWeakReference _headersPresenter;
	std::wstring _columnHeaderPresenterInitializationError;
	ControlWeakReference _columnHeaderPresentationErrorSource;
	std::wstring _columnHeaderPresentationErrorText;
	// Every presenter that has entered Initialize registers here, including
	// programmatic presenters that are not the active template PART. Weak
	// tracking lets the owner retire all raw back-links before its lifetime ends.
	std::vector<ControlWeakReference> _trackedColumnHeaderPresenters;
	ControlWeakReference _selectAllButton;
	ControlWeakReference _scrollViewer;
	EventConnection _selectAllClick;
	EventConnection _dataGridScrollChanged;
	EventConnection _rowHeaderSourceChanged;
	Event<void(DataGrid*)> _currentItemChanged;
	Event<void(DataGrid*)> _currentColumnChanged;
	Event<void(DataGrid*)> _currentCellProjectionChanged;
	double _horizontalScrollOffset = 0.0;
	bool _horizontalScrollAlignmentPending = false;
	int _frozenColumnCount = 0;
	bool _coercingFrozenColumnCountForSchema = false;
	bool _autoColumnsChangedDuringPreparation = false;
	bool _runtimeAutoGenerationComplete = false;
	bool _autoGenerationInProgress = false;
	bool _autoGeneratedColumnsEventPending = false;
	bool _settingItemsSource = false;
	bool _currentCellChangeDeferred = false;
	DataGridCellInfo _deferredCurrentCellOld;
	bool _selectedCellsChangeDeferred = false;
	DataGridSelectedCellCollection _deferredSelectedCellsOld;
	bool _deferredSelectedCellsIgnoreLocators = false;
	size_t _selectedCellsApplyDepth = 0;
	DataGridSelectedCellCollection _selectedCellsApplyOld;
	bool _reconcilingSelectedCellLocators = false;
	bool _selectedCellsVisualRefreshPending = false;
	size_t _rowSelectionRevision = 1;
	// Selection.GetSelection validates a copied cell-selection shape against
	// this monotonic boundary after every extensible source/identity callback.
	// Unlike transaction state, a rollback never restores an older value.
	size_t _cellSelectionRevision = 1;
	bool _selectionRangeActive = false;
	bool _suppressRowSelectionCellSync = false;
	bool _replaceCellSelectionFromRows = false;
	bool _updatingCellSelectionVisuals = false;
	bool _autoGenerateColumns = true;
	bool _isReadOnly = false;
	bool _canUserAddRows = true;
	bool _canUserDeleteRows = true;
	DataGridClipboardCopyMode _clipboardCopyMode =
		DataGridClipboardCopyMode::ExcludeHeader;
	bool _raisingBeginningEdit = false;
	bool _endingCellEdit = false;
	BindingSourceReference _editingRowItem;
	size_t _editingRowOccurrence = DataGridCellInfo::InvalidIndex;
	size_t _editingRowIndex = DataGridCellInfo::InvalidIndex;
	size_t _rowEditRevision = 1;
	bool _editingRowHasSourceTransaction = false;
	bool _startingRowEdit = false;
	bool _endingRowEdit = false;
	bool _callingRowEditSource = false;
	bool _startingNewItem = false;
	bool _callingEditableList = false;
	bool _deletingRows = false;
	bool _destroyingDataGrid = false;
	bool _canUserSortColumns = true;
	bool _canUserResizeColumns = true;
	bool _canUserResizeRows = true;
	bool _canUserReorderColumns = true;
	bool _enableColumnVirtualization = false;
	size_t _realizedFrozenColumnEnd = DataGridCellInfo::InvalidIndex;
	size_t _realizedColumnBegin = DataGridCellInfo::InvalidIndex;
	size_t _realizedColumnEnd = DataGridCellInfo::InvalidIndex;
	bool _refreshingRealizedColumns = false;
	DataGridSelectionUnit _selectionUnit = DataGridSelectionUnit::FullRow;
	DataGridLength _columnWidth = DataGridLength::SizeToHeader();
	double _minColumnWidth = 20.0;
	double _maxColumnWidth = (std::numeric_limits<double>::infinity)();
	double _columnHeaderHeight =
		(std::numeric_limits<double>::quiet_NaN)();
	double _rowHeaderWidth =
		(std::numeric_limits<double>::quiet_NaN)();
	double _rowHeaderActualWidth = 0.0;
	size_t _rowHeaderWidthEpoch = 1;
	double _rowHeight = (std::numeric_limits<double>::quiet_NaN)();
	double _minRowHeight = 0.0;
	DataGridHeadersVisibility _headersVisibility =
		DataGridHeadersVisibility::All;
	DataGridGridLinesVisibility _gridLinesVisibility =
		DataGridGridLinesVisibility::All;
	cui::drawing::Brush _rowBackground;
	cui::drawing::Brush _alternatingRowBackground;
	cui::drawing::Brush _horizontalGridLinesBrush;
	cui::drawing::Brush _verticalGridLinesBrush;
	ControlTemplateReference _rowValidationErrorTemplate;
	std::wstring _cellStyle;
	std::wstring _columnHeaderStyle;
	std::wstring _rowStyle;
	ItemStyleSelectorReference _rowStyleSelector;
	std::wstring _rowHeaderStyle;
	ItemTemplateReference _rowHeaderTemplate;
	ItemTemplateSelectorReference _rowHeaderTemplateSelector;
	bool _areRowDetailsFrozen = false;
	DataGridRowDetailsVisibilityMode _rowDetailsVisibilityMode =
		DataGridRowDetailsVisibilityMode::VisibleWhenSelected;
	ItemTemplateReference _rowDetailsTemplate;
	ItemTemplateSelectorReference _rowDetailsTemplateSelector;
	std::vector<ControlWeakReference> _validationTrackedRows;
	std::vector<ControlWeakReference> _lifecycleLoadedRows;
	bool _preserveColumnContentWidthsDuringRowRebuild = false;
	bool _changingColumnDisplayIndex = false;
	bool _changingColumnVisibility = false;

	static void EnsureCommandBindingsRegistered();
	DataGridEditingUnit ResolveCommandEditingUnit(
		const std::any& parameter) const noexcept;
	bool HasCurrentCellValidationError() const noexcept;
	bool HasSelectedDataRows() const noexcept;
	bool TryBuildClipboardText(std::wstring& result);
	void InvalidateCommandState();
	void RefreshInheritedColumnWidthDefaults(
		bool widthChanged, bool minimumChanged, bool maximumChanged);

	void RefreshColumns();
	const std::wstring& EffectiveCellStyle(
		const DataGridColumn& column) const noexcept;
	const std::wstring& EffectiveColumnHeaderStyle(
		const DataGridColumn& column) const noexcept;
	const std::wstring& EffectiveRowStyle() const noexcept;
	bool TryResolveRowStyle(
		DataGridRow& row, std::wstring& result) const;
	bool TryResolveRowHeaderTemplate(
		DataGridRow& row, DataGridRowHeader& header,
		ItemTemplateReference& result) const;
	void RefreshRealizedCellStyles(const DataGridColumn* column = nullptr);
	void RefreshRealizedColumnHeaderPresentation(
		const DataGridColumn* column = nullptr);
	void PublishColumnHeaderPresentationError(
		DataGridColumnHeader& source, std::wstring error);
	void ClearColumnHeaderPresentationError(
		DataGridColumnHeader& source) noexcept;
	void RetireColumnHeadersPresenterNoCallbacks(
		DataGridColumnHeadersPresenter* presenter,
		std::uint64_t committedGeneration = 0) noexcept;
	void RefreshRealizedRowHeaderPresentation();
	void RefreshRealizedRowDetails();
	void RefreshRealizedRowDetailsHorizontalAlignment();
	bool SynchronizeRealizedRowLifecycle();
	bool RaiseRowLifecycleEvent(
		Event<void(DataGrid*, DataGridRowEventArgs&)>& event,
		DataGridRow& row);
	bool RaiseRowDetailsEvent(
		Event<void(DataGrid*, DataGridRowDetailsEventArgs&)>& event,
		DataGridRow& row, Control& detailsElement);
	bool ApplyItemContainerStyle() override;
	void RefreshColumnDisplayOrder();
	bool IsColumnReorderEligible(const DataGridColumn& column) const noexcept;
	void CancelColumnReorderForEligibilityChange(
		const DataGridColumn* column = nullptr);
	void ReindexDisplayColumns(size_t begin = 0) noexcept;
	void RebuildVisibleColumnProjection() noexcept;
	bool SetColumnVisibility(DataGridColumn& column, ::Visibility value);
	DataGridColumn* GetColumnFromVisibleIndex(size_t index) const noexcept;
	size_t VisibleIndexFromDisplayIndex(size_t displayIndex) const noexcept;
	size_t FirstVisibleDisplayIndex() const noexcept;
	size_t LastVisibleDisplayIndex() const noexcept;
	void RemoveAutoGeneratedColumns();
	void EnsureAutoGeneratedColumns();
	void FlushAutoGeneratedColumnsEvent();
	DataGridColumn* AddColumnCore(
		std::unique_ptr<DataGridColumn> column,
		bool autoGenerated,
		size_t logicalIndex = DataGridCellInfo::InvalidIndex);
	DataGridCell* ResolveCurrentCellContainer() const noexcept;
	DataGridRow* ResolveRow(size_t rowIndex) const noexcept;
	void InvalidateRows();
	void RefreshRealizedRowHeights();
	void ApplyRowHeightToRealizedItem(size_t rowIndex);
	double EffectiveDefaultRowHeight() const noexcept;
	bool TryGetProjectedRowHeight(size_t rowIndex, double& height) const noexcept;
	bool RefreshRowHeightOverrideProjection();
	void PruneRowHeightOverrides();
	bool PersistRowPresentationOverride(DataGridRow& row);
	bool RestoreRowPresentationOverride(DataGridRow& row);
	void PruneRowPresentationOverrides();
	bool SetRowHeightOverride(size_t rowIndex, double height);
	bool ClearRowHeightOverride(size_t rowIndex);
	bool BeginRowResizeTransaction(size_t rowIndex, double& startHeight);
	bool ResizeRowInTransaction(double pixelHeight);
	bool ApplyPendingRowResizeInput();
	void EndRowResizeTransaction(bool cancel);
	void RefreshRealizedRowValidationStates(bool coerceTemplate);
	void RefreshHeaderPresenter();
	void RefreshHeadersVisibility();
	void RefreshHorizontalScrollAlignment();
	bool RefreshRowHeaderActualWidthProjection();
	bool RefreshRowHeaderActualWidth();
	double ResolveRowHeaderWidth() const noexcept;
	static const DependencyPropertyKey& RowHeaderActualWidthPropertyKey();
	bool SetCurrentRowHeaderActualWidth(double value);
	void InvalidateRowHeaderWidthBaseline() noexcept;
	bool TryShareRowHeaderDesiredWidth(
		DataGridRowHeader& header, double desiredWidth, double& sharedWidth);
	bool HandleRowHeaderClick(
		DataGridRow& row, ModifierKeys modifiers);
	void HandleSelectAll();
	bool RaiseCurrentCellChanged(DataGridCellInfo previous);
	bool NotifyCurrentCellProjectionsChanged(
		const DataGridCellInfo& previous);
	bool ReconcileCurrentCellColumn();
	bool TryNormalizeCurrentCellInfo(
		const DataGridCellInfo& value,
		DataGridCellInfo& result) const;
	bool TryCreateCellInfo(
		size_t rowIndex, size_t columnIndex, DataGridCellInfo& result) const;
	bool TryResolveItemOccurrence(
		const BindingSourceReference& item,
		size_t occurrence,
		size_t& rowIndex) const;
	bool TryGetItemOccurrenceAt(
		size_t rowIndex, size_t& occurrence) const;
	bool TryGetStableSelectedCellRegionSnapshot(
		BindingListReference& snapshot) const;
	void InvalidateItemOccurrenceCache() const noexcept;
	bool EnsureAccessibilityVirtualIdentities() const;
	AccessibilityIdentityState CaptureAccessibilityIdentityState() const;
	void RestoreAccessibilityIdentityState(
		AccessibilityIdentityState&& state) noexcept;
	void InvalidateAccessibilityVirtualIdentities() const noexcept;
	void PruneAccessibilityColumnIdentities() noexcept;
	void RequestAccessibilityStructureChanged();
	void FlushAccessibilityStructureChange();
	bool TryResolveAccessibilityVirtualNode(
		uint32_t id, AccessibilityNodeLocator& result) const;
	AccessibilityRowIdentity* EnsureAccessibilityRowIdentity(
		size_t rowIndex, bool ensureCells) const;
	AccessibilityCellIdentity* EnsureAccessibilityCellIdentity(
		size_t rowIndex, size_t columnIndex) const;
	AccessibilityRowIdentity* FindAccessibilityRowIdentity(
		size_t rowIndex) const noexcept;
	bool TryReadAccessibilityCellValue(
		size_t rowIndex, size_t columnIndex,
		std::wstring& result) const;
	bool ApplySelectedCells(std::vector<DataGridCellInfo> cells);
	bool ApplySelectedCellCollection(
		DataGridSelectedCellCollection cells);
	bool BeginEditCore(
		const RoutedEventArgs* editingEventArgs,
		bool toggleCheckBox);
	IEditableBindingList* EditableItems() const noexcept;
	bool IsNewItemPlaceholder(
		const BindingSourceReference& item) const noexcept;
	bool IsAddingNewItem() const noexcept;
	bool TryGetPendingNewItemOccurrence(
		const BindingSourceReference& item,
		size_t& occurrence) const noexcept;
	bool IsPendingNewItem(
		const BindingSourceReference& item,
		size_t rowIndex) const noexcept;
	void RefreshNewItemPlaceholder();
	void RefreshNewItemContainerStates();
	bool BeginNewItemFromPlaceholder(
		DataGridColumn& column, DataGridCellInfo& identity);
	bool CancelPendingNewItem(DataGridColumn* restoreColumn = nullptr);
	bool BeginRowEditTransaction(const DataGridCellInfo& identity);
	bool CommitCellEdit(bool exitEditingMode);
	bool CancelCellEdit();
	bool EndRowEdit(
		DataGridEditAction action,
		bool exitEditingMode);
	bool IsRowEditIdentity(
		const BindingSourceReference& item,
		size_t rowIndex) const;
	bool TryResolveEditingRowIndex(size_t& rowIndex) const;
	void RefreshEditingRowContainers();
	void ClearRowEditTransaction();
	void AbandonRowEditTransaction(bool cancelSource) noexcept;
	void ReconcileRowEditTransaction();
	bool RaiseSelectedCellsChanged(
		const DataGridSelectedCellCollection& previous,
		bool ignoreLocators = false);
	bool RaiseSelectedCellsChangedCore(
		const DataGridSelectedCellCollection& previous,
		const DataGridSelectedCellCollection& current,
		bool ignoreLocators);
	bool RefreshSelectedCellContainers(
		bool suppressRoutedEvents = false,
		bool onlyStaleRows = false);
	void RefreshSelectedCellContainersAfterRollback() noexcept;
	bool ReconcileSelectedCells();
	bool SynchronizeSelectedCellsFromRows();
	bool SynchronizeRangeSelectedCellsFromRows(
		SelectedIndexCollection selectedIndices);
	void HandleRowSelectionChanged(SelectionChangedEventArgs& args);
	void OnSelectionUnitChanged(
		DataGridSelectionUnit oldValue, DataGridSelectionUnit newValue);
	void OnCellIsSelectedChanged(DataGridCell& cell, bool selected);
	bool ApplySelectionForCellInput(
		size_t rowIndex, size_t columnIndex, ModifierKeys modifiers,
		bool allowsRange, bool allowsToggle);
	bool DeselectAllRowsWithoutCellSync();
	void ResetSelectionRange() noexcept;
	void FlushCommittedItemsSourceState();
	void InvalidateColumnWidthCache() noexcept;
	void InvalidateColumnContentWidthCache() noexcept;
	void RedistributeRuntimeWidthsForViewportChange(
		double oldViewportWidth, double newViewportWidth) noexcept;
	bool RebaseColumnResizeTransaction();
	bool BeginColumnResizeTransaction(
		size_t columnIndex, bool compensateLeft);
	bool ResizeColumnInTransaction(size_t columnIndex, double pixelWidth);
	bool ApplyPendingColumnResizeInput();
	void EndColumnResizeTransaction(bool cancel);
	bool ResizeColumnCore(
		size_t columnIndex, double pixelWidth,
		bool preserveRealizedColumnRange);
	void ApplyPendingColumnWidths(bool refreshVirtualMetrics = true);
	void CommitColumnWidthLayoutToAncestors();
	void InvalidatePendingColumnResizeVisual();
	void RefreshColumnWidths(bool preserveRealizedColumnRange = false);
	void ReevaluateFrozenColumnCountForSchemaChange();
	void ProjectColumnWidthsForViewportLayout();
	bool TryCommitViewportColumnLayoutLocally();
	std::pair<size_t, size_t> ResolveRealizedColumnRange() const;
	void InvalidateRealizedColumnRange() noexcept;
	void RefreshRealizedColumns();
	bool BringColumnIntoView(size_t columnIndex);
	void UpdateColumnViewportWidth(
		double availableWidth, bool refreshRealizedColumns = true);
	double ResolveColumnViewportWidth(double fallbackWidth) const noexcept;
	void RebuildResolvedColumnWidths() const;
	std::pair<size_t, size_t>
		ResolveDeferredColumnSampleRange() const;
	bool EnsureColumnWidthPrefix() const;
	bool TryResolveColumnBounds(
		size_t columnIndex, double& left, double& right) const;
	double GetColumnDisplayWidth(size_t columnIndex) const;
	bool AutoSizeColumn(size_t columnIndex);
	GridLength ResolveColumnGridLength(size_t columnIndex) const;
	double EstimateColumnWidth(size_t columnIndex, bool includeHeader,
		bool includeCells) const;
	CollectionSortDescription MakeSortDescription(
		const DataGridColumn& column,
		CollectionSortDirection direction) const;
	bool HandleCellKey(Key key, ModifierKeys modifiers);
};
