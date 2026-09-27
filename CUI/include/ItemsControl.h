#pragma once

#include "ItemTemplate.h"
#include "GroupStyle.h"
#include "ItemContainerGenerator.h"
#include "ItemsPanelTemplate.h"
#include "ItemsPresenter.h"
#include "ScrollViewer.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace cui::framework
{
	struct TemplateAccess;
	struct ItemsControlAccess;
}

class ContentPresenter;

/**
 * Sparse fixed-height contribution used by a virtualizing ItemsControl.
 * Entries are sorted by item index and replace the host's uniform item height;
 * they do not include any group-header contribution.
 */
struct VirtualizedItemExtentOverride final
{
	size_t ItemIndex = 0;
	double Extent = 0.0;
};

/**
 * Generic templated collection presenter.
 *
 * Items are either authored UIElement instances or records supplied by
 * ItemsSource, never both. In either mode the sole direct visual child is an
 * internal ItemsHost; authored items are logical children of this control and
 * visual children of that host. Item records remain ordinary IBindingSource
 * objects, so bindings inside a template receive the item as their DataContext
 * without introducing an untyped object bag.
 */
class ItemsControl : public Control
{
public:
	using GeneratedContainerInitializer =
		std::function<bool(Control&, std::wstring*)>;

	ItemsControl();
	UIClass Type() override { return UIClass::UI_ItemsControl; }
	/** WPF dependency-property identities used by generated/native code. */
	static const DependencyProperty& ItemsSourceProperty();
	static const DependencyProperty& ItemTemplateProperty();
	static const DependencyProperty& GroupStyleProperty();
	static const DependencyProperty& ItemsPanelProperty();
#if CUI_ENABLE_DYNAMIC_XAML
	static const DependencyProperty& DisplayMemberPathProperty();
#endif
	static const DependencyProperty& ItemContainerStyleProperty();
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override { RegisterDependencyProperties(); }
#endif

	virtual BindingListReference GetItemsSource() const noexcept
	{
		return _itemsSource;
	}
	virtual void SetItemsSource(BindingListReference value);
	virtual ItemTemplateReference GetItemTemplate() const noexcept
	{
		return _itemTemplate;
	}
	virtual void SetItemTemplate(ItemTemplateReference value);
	GroupStyleReference GetGroupStyle() const noexcept { return _groupStyle; }
	void SetGroupStyle(GroupStyleReference value);
	ItemsPanelTemplateReference GetItemsPanel() const noexcept
	{
		return _itemsPanel;
	}
	void SetItemsPanel(ItemsPanelTemplateReference value);
#if CUI_ENABLE_DYNAMIC_XAML
	virtual const std::wstring& GetDisplayMemberPath() const noexcept
	{
		return _displayMemberPath;
	}
	virtual void SetDisplayMemberPath(std::wstring value);
#endif
	[[nodiscard]] virtual CompiledBindingPathView
		GetCompiledDisplayMemberPath() const noexcept
	{
		return _compiledDisplayMemberPath;
	}
	virtual void SetCompiledDisplayMemberPath(
		CompiledBindingPathView value);
	static const DependencyProperty& IsTextSearchEnabledProperty();
	static const DependencyProperty& IsTextSearchCaseSensitiveProperty();
	bool GetIsTextSearchEnabled() const;
	void SetIsTextSearchEnabled(bool value);
	__declspec(property(
		get = GetIsTextSearchEnabled,
		put = SetIsTextSearchEnabled))
		bool IsTextSearchEnabled;
	bool GetIsTextSearchCaseSensitive() const;
	void SetIsTextSearchCaseSensitive(bool value);
	__declspec(property(
		get = GetIsTextSearchCaseSensitive,
		put = SetIsTextSearchCaseSensitive))
		bool IsTextSearchCaseSensitive;
	/**
	 * Framework entry used when committed text originated on this control or
	 * one of its generated item containers.
	 */
	bool ProcessTextSearchInput(
		const TextCompositionEventArgs& input);
	/** Updates the active incremental-search prefix for Backspace. */
	void ProcessTextSearchKey(const InputReport& input);
	const std::wstring& GetItemContainerStyle() const noexcept
	{
		return _itemContainerStyle;
	}
	void SetItemContainerStyle(std::wstring value);
	/**
	 * Installs the XAML-schema initializer used for lazily generated containers.
	 * The runtime owns schema knowledge; ItemsControl only invokes this boundary
	 * before a generated container enters the logical/visual tree.
	 */
	void SetGeneratedContainerInitializer(
		GeneratedContainerInitializer value);
	const std::wstring& LastTemplateError() const noexcept
	{
		return _lastTemplateError;
	}
	size_t GeneratedItemCount() const noexcept;
	size_t RecycledItemCount() const noexcept
	{
		return _generator.RecycledCount();
	}
	virtual size_t ItemCount() const noexcept;
	Control* GetGeneratedItem(size_t index) const noexcept;
	/**
	 * Adds one authored UIElement item. Authored Items and ItemsSource are
	 * mutually exclusive, matching WPF ItemCollection semantics.
	 */
	Control* AddItemControl(std::unique_ptr<Control> item);
	Control* InsertItemControl(size_t index, std::unique_ptr<Control> item);
	Control* AdoptItemControl(Control* item);
	Control* InsertItemControl(size_t index, Control* item);
	virtual std::unique_ptr<Control> DetachItemControlAt(size_t index);
	std::unique_ptr<Control> DetachItemControl(Control* item);
	bool RemoveItemControlAt(size_t index);
	bool RemoveItemControl(Control* item);
	bool MoveItemControl(size_t oldIndex, size_t newIndex);
	void ClearItemControls();
	Control* GetAuthoredItem(size_t index) const noexcept;
	size_t AuthoredItemCount() const noexcept { return _authoredItems.size(); }

	template<typename T>
	T* AddItem()
	{
		static_assert(std::is_base_of_v<Control, T>, "T must derive from Control");
		return static_cast<T*>(AddItemControl(
			std::make_unique<T>()));
	}
	bool IsVirtualizing() const noexcept;
protected:
	void PreparePresentation() override;
	void OnApplyTemplate() override;
	void OnRender() override;
	bool ProcessInput(const InputReport& input) override;
	bool ApplyTextInput(
		const TextCompositionEventArgs& input) override;
public:
	cui::core::Size MeasureCore(
		const cui::core::Constraints& available) override;
	void Arrange(cui::core::Rect finalRect) override;

protected:
	std::unique_ptr<AutomationPeer> OnCreateAutomationPeer() override
	{
		return std::make_unique<AutomationPeer>(
			*this, AutomationControlType::List, L"ItemsControl");
	}
	Panel* GetItemsHost() const noexcept { return _itemsHost; }
	ItemsPresenter* GetTemplateItemsPresenter() const noexcept
	{
		return _templateItemsPresenter;
	}
	Control* GetControlTemplateRoot() const noexcept override
	{
		return _controlTemplateRoot;
	}
	/** Framework hook used by a ControlTemplate ItemsPresenter slot. */
	bool RegisterTemplateItemsPresenter(ItemsPresenter* presenter);
	Control* SetControlTemplateRoot(std::unique_ptr<Control> value) override;
	std::unique_ptr<Control> DetachVisualChildTemplateRoot() override;

	class AuthoredItemsUpdateScope final
	{
	public:
		AuthoredItemsUpdateScope(AuthoredItemsUpdateScope&& other) noexcept;
		AuthoredItemsUpdateScope& operator=(
			AuthoredItemsUpdateScope&& other) noexcept;
		AuthoredItemsUpdateScope(const AuthoredItemsUpdateScope&) = delete;
		AuthoredItemsUpdateScope& operator=(
			const AuthoredItemsUpdateScope&) = delete;
		~AuthoredItemsUpdateScope();

	private:
		explicit AuthoredItemsUpdateScope(ItemsControl& owner) noexcept;
		ItemsControl* _owner = nullptr;
		friend class ItemsControl;
	};
	AuthoredItemsUpdateScope DeferAuthoredItemsChanges() noexcept;
	friend struct cui::framework::ItemsControlAccess;
	/**
	 * Per-operation token for state owned by a derived item control.
	 *
	 * Source replacement and live-change rollback restore the base source,
	 * subscriptions and generated tree before handing this token back. Tokens
	 * stay local to the operation so rejected reentry cannot overwrite a shared
	 * rollback slot in the derived control.
	 */
	struct ItemsSourceTransactionState
	{
		virtual ~ItemsSourceTransactionState() = default;
	};
	virtual std::unique_ptr<ItemsSourceTransactionState>
		CaptureItemsSourceTransactionState()
	{
		return {};
	}
	virtual void RestoreItemsSourceTransactionState(
		ItemsSourceTransactionState&) noexcept {}
	void RequestLayout() override;
	void OnComputedLayoutSizeChanged() override;
	void PerformPendingLayout() override;
	void OnLocalMeasurePathInvalidated() override
	{
		_itemsLayoutPending = true;
	}
	bool IsItemsLayoutPending() const noexcept
	{
		return _itemsLayoutPending;
	}
	void CommitItemsLayout() noexcept
	{
		_itemsLayoutPending = false;
	}
	bool ValidateVisualChildCollection(
		std::span<Control* const> children,
		std::string& error) const override;
	virtual std::unique_ptr<Control> WrapGeneratedItem(
		std::unique_ptr<Control> visual,
		const BindingSourceReference& item,
		size_t index);
	bool InitializeGeneratedContainer(Control& container);
	virtual bool ValidateAuthoredItemControl(
		const Control& item, std::string& error) const
	{
		(void)item;
		(void)error;
		return true;
	}
	virtual void OnAuthoredItemsChanged() noexcept {}
	virtual std::unique_ptr<Control> BuildGeneratedItem(
		const BindingSourceReference& item,
		size_t index,
		BindingPathObservation& observation);
	/**
	 * Opt-in recycling contract for containers detached by an earlier virtual
	 * viewport commit. Generic ItemsControl keeps exact-index semantics; a
	 * derived control must explicitly prove that a donor is safe and fully
	 * rebind its item state before it can be attached at another index.
	 */
	virtual bool CanRecycleGeneratedItemAcrossIndices(
		const Control& visual, size_t oldIndex) const noexcept
	{
		(void)visual;
		(void)oldIndex;
		return false;
	}
	virtual bool TryRebindGeneratedItemAcrossIndices(
		Control& visual,
		size_t oldIndex,
		size_t newIndex,
		const BindingSourceReference& item,
		BindingPathObservation& observation,
		std::wstring* outError)
	{
		(void)visual;
		(void)oldIndex;
		(void)newIndex;
		(void)item;
		(void)observation;
		if (outError) outError->clear();
		return false;
	}
	/**
	 * Opt-in in-place virtualization: moves a container that is still attached
	 * to the virtual host to another item index. Detaching and re-attaching a
	 * recycled container refreshes the inheritance/style context and presentation
	 * window of its entire subtree twice and rebuilds the retained scene topology;
	 * an in-place move only rebinds item state. The derived control proves the
	 * container is safe to move and must leave it fully consistent on success;
	 * on failure ItemsControl detaches and discards the container.
	 */
	virtual bool CanRebindRealizedItemInPlace(
		const Control& visual, size_t oldIndex) const noexcept
	{
		(void)visual;
		(void)oldIndex;
		return false;
	}
	virtual bool TryRebindRealizedItemInPlace(
		Control& visual,
		size_t oldIndex,
		size_t newIndex,
		const BindingSourceReference& item,
		BindingPathObservation& observation,
		std::wstring* outError)
	{
		(void)visual;
		(void)oldIndex;
		(void)newIndex;
		(void)item;
		(void)observation;
		if (outError) outError->clear();
		return false;
	}
	/** Called before a rebuild prepares any replacement item containers. */
	virtual void OnBeforeGeneratedItemsPrepared() {}
	virtual void OnBeforeGeneratedItemsRebuilt() {}
	/** Called while a realized container is still attached, immediately before it is cleared. */
	virtual void OnGeneratedItemClearing(Control&) {}
	virtual void OnGeneratedItemsRebuilt() {}
	virtual void OnGeneratedItemsRealized() {}
	/** Called with the logical generated container, never its grouped visual wrapper. */
	virtual void OnGeneratedItemIndexChanged(
		Control&, size_t, size_t) {}
	virtual std::wstring GetTextSearchItemText(
		size_t index) const;
	/** Evaluates the active Design or AOT display projection. */
	std::wstring GetDisplayMemberText(
		const BindingSourceReference& item) const;
	/** Observes only the active display projection lane. */
	BindingPathObservation ObserveDisplayMemberPath(
		const BindingSourceReference& item,
		std::function<void()> changed) const;
	virtual void OnTextSearchMatch(size_t index)
	{
		(void)index;
	}
	virtual bool ApplyItemContainerStyle();
	virtual void OnItemsSourceChanged(
		const BindingListReference&,
		const BindingListReference&) {}
	/**
	 * A live source has mutated and a replacement materialized snapshot can be
	 * committed, but no container change has committed yet. previousSnapshot is
	 * the last committed occurrence domain. Derived selection models can prepare
	 * deltas here; any later failure restores the state captured beforehand.
	 */
	virtual void OnItemsSourceCollectionChangePreparing(
		const CollectionChangedEventArgs&,
		const BindingListReference&) {}
	/** A different source has been staged and derived rollback state captured,
	 *  but its generated tree has not been prepared yet. */
	virtual void OnItemsSourceReplacementPreparing(
		const BindingListReference&,
		const BindingListReference&) {}
	/** Called only after a source replacement or live change has committed. */
	virtual void OnItemsSourceTransactionCommitted() {}
	/** Called after one live collection notification commits successfully. */
	virtual void OnItemsSourceCollectionChangeCommitted(
		const CollectionChangedEventArgs&) {}
	virtual std::unique_ptr<Panel> CreateItemsHost() const;
	/**
	 * A virtualizing host normally realizes all items when it has no viewport.
	 * Popup-backed controls may defer realization until their ItemsPresenter is
	 * attached to a ScrollViewer with a finite viewport.
	 */
	virtual bool ShouldRealizeVirtualItemsWithoutViewport() const noexcept
	{
		return true;
	}
	/** A large direct Thumb jump benefits from realizing only the visible page.
	 *  The configured cache is restored when the drag completes. */
	virtual bool UseVisibleOnlyRangeDuringVerticalThumbDrag() const noexcept
	{
		return false;
	}
	/** Fixed extent consumed by the virtual host for one ungrouped item. */
	virtual float GetVirtualizedItemHeight() const noexcept
	{
		return EffectiveItemsPanel().ItemHeight;
	}
	/** A derived container may temporarily exceed the fixed virtual slot.  The
	 *  host measures only explicitly opted-in logical containers and keeps the
	 *  resulting sparse extent in its offset model. */
	virtual bool UseMeasuredVirtualizedItemHeight(
		const Control&) const noexcept
	{
		return false;
	}
	/** Sparse persistent replacements for the uniform virtual item height. */
	virtual std::span<const VirtualizedItemExtentOverride>
		GetVirtualizedItemExtentOverrides() const noexcept
	{
		return {};
	}
	/** Monotonic revision for GetVirtualizedItemExtentOverrides(). */
	virtual size_t GetVirtualizedItemExtentOverridesRevision() const noexcept
	{
		return 0;
	}
	/** Logical horizontal extent when realized children intentionally omit
	 *  offscreen content, such as DataGrid column virtualization. */
	virtual double GetVirtualizedHorizontalExtent() const
	{
		return 0.0;
	}
	void RefreshVirtualScrollMetrics() { ConfigureVirtualHost(); }
	/** Updates one persistent virtual item extent without rebuilding the complete
	 *  sparse projection. Returns false when the active host cannot accept it. */
	bool TryUpdateVirtualizedItemExtentOverride(
		size_t itemIndex, double extent);
	bool IsVirtualizedItemInViewport(size_t itemIndex) const noexcept;
	/** Called after the active ControlTemplate root changes. */
	void OnControlTemplatePresentationChanged() override {}
	/** Replaces the framework ItemsHost before any items are attached. */
	void ReplaceItemsHostCore(std::unique_ptr<Panel> host);
	bool IsItemsSourceUpdateInProgress() const noexcept
	{
		return _itemsSourceUpdateDepth != 0;
	}
	/** True only while SetItemsSource is constructing a different occurrence
	 *  domain. Live changes within the current source leave this false. */
	bool IsItemsSourceReplacementInProgress() const noexcept
	{
		return _itemsSourceReplacementInProgress;
	}
	bool IsChangingItemsInfrastructure() const noexcept
	{
		return _activeDirectVisualMutationFrame != nullptr;
	}
	bool IsAuthoredItemsMigrationInProgress() const noexcept
	{
		return _migratingAuthoredItems;
	}
	/** Effective records consumed by the generator. A derived control may use
	 *  an internal authored-items view without exposing it as ItemsSource. */
	const BindingListReference& GetItemsView() const noexcept
	{
		return _itemsSource;
	}
	bool BringItemIntoView(size_t index);
	const ItemContainerGenerator::RealizedMap& GetRealizedItems() const noexcept
	{
		return _generator.RealizedItems();
	}
	bool RebuildGeneratedItems();
	void SetLastTemplateError(std::wstring value)
	{
		_lastTemplateError = std::move(value);
	}
	std::wstring& MutableLastTemplateError() noexcept
	{
		return _lastTemplateError;
	}
	/**
	 * Commits canonical ItemsControl property storage for a derived control
	 * whose projection is not the flat ItemContainerGenerator pipeline.
	 * The derived projection remains responsible for subscriptions, rollback
	 * and visual realization; the public property still has exactly one value.
	 */
	void SetCustomProjectionItemsSource(BindingListReference value);
	void SetCustomProjectionItemTemplate(ItemTemplateReference value) noexcept
	{
		_itemTemplate = std::move(value);
	}
#if CUI_ENABLE_DYNAMIC_XAML
	void SetCustomProjectionDisplayMemberPath(std::wstring value)
	{
		_displayMemberPath = std::move(value);
		_compiledDisplayMemberPath = {};
	}
#endif
	void SetCustomProjectionCompiledDisplayMemberPath(
		CompiledBindingPathView value) noexcept
	{
		_compiledDisplayMemberPath = value;
#if CUI_ENABLE_DYNAMIC_XAML
		_displayMemberPath.clear();
#endif
	}

private:
	friend struct cui::framework::TemplateAccess;
#if CUI_ENABLE_DYNAMIC_XAML
	static void RegisterDesignDependencyProperties();
#endif

	struct PreparedItem final
	{
		size_t Index = 0;
		std::unique_ptr<Control> Visual;
		BindingPathObservation Observation;
		bool WasRecycled = false;
	};
	struct PreparedGroupHeaders final
	{
		std::vector<std::unique_ptr<Control>> Visuals;
		std::vector<BindingSourceReference> Contexts;
	};
	struct CrossIndexRecycleCandidate final
	{
		size_t Index = 0;
		Control* Visual = nullptr;
	};
	struct DirectVisualMutationFrame;

	BindingListReference _itemsSource;
	// Immutable projection of the last ItemsSource state whose generated tree
	// committed successfully. A live collection notification cannot generally
	// be undone by IBindingList, so this snapshot is the rollback value when a
	// newly added/replaced item cannot be materialized.
	BindingListReference _materializedItemsSourceSnapshot;
	ItemTemplateReference _itemTemplate;
	GroupStyleReference _groupStyle;
	ItemsPanelTemplateReference _itemsPanel;
	EventConnection _itemsSourceChanged;
	EventConnection _groupsChanged;
	EventConnection _scrollChanged;
	// Stable-radix normalized by StartIndex then Level. The parallel bottom-level
	// flags are computed once with the group revision so realizing one row never
	// scans the complete group collection.
	std::vector<BindingListGroup> _cachedGroupDefinitions;
	std::vector<uint8_t> _cachedGroupBottomLevels;
	// Canonical [StartIndex, header-count] pairs. This remains stable across
	// ordinary viewport queries and changes only after an explicit group signal.
	std::vector<size_t> _virtualGroupHeaderStarts;
	size_t _virtualGroupHeaderMetadataRevision = 1;
	bool _virtualGroupHeaderMetadataDirty = true;
	bool _virtualGroupingActive = false;
	ItemContainerGenerator _generator;
	std::vector<Control*> _authoredItems;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring _displayMemberPath;
#endif
	CompiledBindingPathView _compiledDisplayMemberPath;
	std::wstring _itemContainerStyle;
	std::wstring _lastTemplateError;
	GeneratedContainerInitializer _generatedContainerInitializer;
	Panel* _itemsHost = nullptr;
	ItemsPresenter* _templateItemsPresenter = nullptr;
	ControlWeakReference _pendingTemplateItemsPresenter;
	Control* _controlTemplateRoot = nullptr;
	ScrollViewer* _itemsScrollOwner = nullptr;
	std::unique_ptr<Panel> _detachedItemsHost;
	bool _itemsLayoutPending = true;
	bool _realizingViewport = false;
	bool _applyingCollectionChange = false;
	bool _virtualCacheRestorePending = false;
	// Native scroll input can publish offsets substantially faster than the
	// retained scene can present them.  A presented ItemsControl realizes the
	// newest range from PreparePresentation; detached/test controls retain the
	// historical synchronous behavior.
	bool _virtualRealizationPending = false;
	double _lastVirtualRealizationOffset =
		(std::numeric_limits<double>::quiet_NaN)();
	std::vector<CrossIndexRecycleCandidate> _virtualRecycleCandidatesScratch;
	std::vector<size_t> _virtualAdditionIndicesScratch;
	std::vector<size_t> _virtualRemovalIndicesScratch;
	std::vector<size_t> _virtualInPlaceLeavingScratch;
	bool _migratingAuthoredItems = false;
	size_t _itemsSourceUpdateDepth = 0;
	bool _itemsSourceReplacementInProgress = false;
	size_t _authoredItemsUpdateDepth = 0;
	bool _authoredItemsChangedPending = false;
	EventConnection _itemsPresenterParentChanged;
	std::wstring _textSearchPrefix;
	std::vector<std::wstring> _textSearchChunks;
	int _textSearchMatchedIndex = -1;
	std::uint64_t _textSearchLastInputTick = 0;
	bool _textSearchActive = false;
	DirectVisualMutationFrame* _activeDirectVisualMutationFrame = nullptr;
	size_t _generatedItemsRevision = 1;

	const ItemsPanelTemplate& EffectiveItemsPanel() const noexcept;
	bool ReplaceItemsHost(ItemsPanelTemplateReference value);
	std::unique_ptr<Panel> TakeItemsHost();
	void PlaceItemsHost(std::unique_ptr<Panel> host);
	bool CommitPendingTemplateItemsPresenter();
	void ClearPendingTemplateItemsPresenter() noexcept;
	void RefreshItemsScrollOwner();
	void AdvanceGeneratedItemsRevision() noexcept
	{
		if (++_generatedItemsRevision == 0) _generatedItemsRevision = 1;
	}
	ScrollViewer* ItemsScrollOwner() const noexcept
	{
		return _itemsScrollOwner;
	}
	bool PrepareGeneratedItem(
		size_t index,
		PreparedItem& output,
		bool allowRecycle = true,
		std::span<const size_t> crossIndexRecycleReservations = {},
		std::span<const CrossIndexRecycleCandidate>
			crossIndexRecycleCandidates = {});
	void AttachPreparedItem(PreparedItem&& item);
	void ReorderRealizedChildren();
	void ClearRealizedItems(bool keepForRecycle);
	bool RealizeVirtualViewport(
		bool localLayoutForScroll = false,
		bool useVisibleOnlyRange = false);
	void RestoreVirtualCacheAfterVerticalThumbDrag();
	bool RealizeVirtualRange(
		size_t first, size_t last, bool localLayoutForScroll = false);
	/** Moves one attached realized container; false leaves the normal path. */
	bool TryReindexRealizedItemInPlace(size_t oldIndex, size_t newIndex);
	std::pair<size_t, size_t> VirtualRangeForViewport() const noexcept;
	std::pair<size_t, size_t> VirtualRangeForOffset(
		double offset) const noexcept;
	void TrimRecyclePool(size_t first, size_t last);
	void InvalidateVirtualGroupHeaderMetadata() noexcept;
	void RefreshVirtualGroupHeaderMetadata();
	void ConfigureVirtualHost();
	size_t VirtualOffsetMetadataEntryCount() const noexcept;
	size_t VirtualOffsetConfigurationRevision() const noexcept;
	size_t VirtualItemExtentOverrideCount() const noexcept;
	bool ApplyCollectionChange(const CollectionChangedEventArgs& change);
	struct OccurrenceResetMapping final
	{
		std::vector<size_t> Complete;
		std::vector<std::pair<size_t, size_t>> Generated;
		std::optional<std::pair<size_t, size_t>> ViewportAnchor;
		bool Sparse = false;
	};
	bool TryBuildOccurrencePermutationReset(
		const CollectionChangedEventArgs& change,
		OccurrenceResetMapping& mapping);
	bool ApplyOccurrencePermutationReset(
		const CollectionChangedEventArgs& change,
		const OccurrenceResetMapping& mapping);
	void HandleItemsSourceChange(const CollectionChangedEventArgs& change);
	bool IsGroupingActive() const noexcept;
	PreparedGroupHeaders BuildGroupHeaders(
		size_t index, const BindingSourceReference& item);
	void RefreshGroupHeaders();
	static Control* UnwrapGeneratedItem(Control* visual) noexcept;
	static bool ClearGroupedItemLogicalParentPreservingOwnership(
		std::unique_ptr<Control>& visual);
	void BeginAuthoredItemsUpdate() noexcept;
	void EndAuthoredItemsUpdate() noexcept;
	void NotifyAuthoredItemsChanged();
	void RefreshGeneratedItem(
		const std::weak_ptr<IBindingSource>& itemIdentity);
	void ResetTextSearch() noexcept;
#if CUI_ENABLE_DYNAMIC_XAML
	std::wstring ReadAuthoredDisplayMemberText(
		const BindingSourceReference& item) const;
	BindingPathObservation ObserveAuthoredDisplayMemberPath(
		const BindingSourceReference& item,
		std::function<void()> changed) const;
	void ApplyAuthoredGeneratedItemProjection(
		ContentPresenter& presenter) const;
#endif
};

namespace cui::framework
{
	/** Internal batching surface used while XAML owns an unobservable tree. */
	struct ItemsControlAccess final
	{
		ItemsControlAccess() = delete;
		using AuthoredItemsUpdateScope =
			ItemsControl::AuthoredItemsUpdateScope;

		static AuthoredItemsUpdateScope
			DeferAuthoredItemsChanges(ItemsControl& target) noexcept
		{
			return target.DeferAuthoredItemsChanges();
		}
		static size_t VirtualOffsetMetadataEntryCount(
			const ItemsControl& target) noexcept
		{
			return target.VirtualOffsetMetadataEntryCount();
		}
		static size_t VirtualOffsetConfigurationRevision(
			const ItemsControl& target) noexcept
		{
			return target.VirtualOffsetConfigurationRevision();
		}
		static size_t VirtualItemExtentOverrideCount(
			const ItemsControl& target) noexcept
		{
			return target.VirtualItemExtentOverrideCount();
		}
		static bool UseMeasuredVirtualizedItemHeight(
			const ItemsControl& target, const Control& item) noexcept
		{
			return target.UseMeasuredVirtualizedItemHeight(item);
		}
		static void RestoreVirtualCacheAfterVerticalThumbDrag(
			ItemsControl& target)
		{
			target.RestoreVirtualCacheAfterVerticalThumbDrag();
		}
	};
}
